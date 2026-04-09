#include "flux/runtime.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

namespace flux {

struct Runtime::BoostIoState {
    explicit BoostIoState(std::size_t thread_count)
        : io_context(static_cast<int>(thread_count)),
          work_guard(boost::asio::make_work_guard(io_context)) {}

    boost::asio::io_context io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
    std::vector<std::thread> workers;
};

namespace {

std::mutex g_trace_hook_mutex;
TraceHook g_trace_hook;
std::atomic<Runtime *> g_current_runtime{nullptr};

std::size_t fallback_threads() {
    const auto hc = std::thread::hardware_concurrency();
    return hc == 0 ? 1u : static_cast<std::size_t>(hc);
}

std::size_t resolve_io_threads(const RuntimeOptions &options) {
    return options.io_threads == 0 ? fallback_threads() : options.io_threads;
}

std::size_t resolve_blocking_threads(const RuntimeOptions &options) {
    if (options.blocking_threads != 0) {
        return options.blocking_threads;
    }
    return fallback_threads();
}

bool running_in_executor_thread(const exec::asio::asio_impl::any_io_executor &executor) noexcept {
    using ThreadPoolExecutor = exec::asio::asio_impl::thread_pool::executor_type;
    const auto *typed = executor.target<ThreadPoolExecutor>();
    return typed != nullptr && typed->running_in_this_thread();
}

void stop_and_join_io_pool(exec::asio::asio_thread_pool &pool) noexcept {
    try {
        auto executor = pool.get_executor();
        auto &context = executor.context();
        context.stop();

        if (auto *thread_pool =
                dynamic_cast<exec::asio::asio_impl::thread_pool *>(&context)) {
            thread_pool->join();
        }
    } catch (...) {
    }
}

} // namespace

void set_trace_hook(TraceHook hook) {
    std::lock_guard<std::mutex> lock(g_trace_hook_mutex);
    g_trace_hook = std::move(hook);
}

void reset_trace_hook() {
    std::lock_guard<std::mutex> lock(g_trace_hook_mutex);
    g_trace_hook = nullptr;
}

void emit_trace_event(TraceEvent event) noexcept {
    TraceHook hook;
    {
        std::lock_guard<std::mutex> lock(g_trace_hook_mutex);
        hook = g_trace_hook;
    }

    if (!hook) {
        return;
    }

    try {
        hook(event);
    } catch (...) {
    }
}

Runtime::Runtime(RuntimeOptions options)
    : options_(std::move(options)),
      io_pool_(static_cast<std::uint32_t>(resolve_io_threads(options_))),
      blocking_pool_(static_cast<std::uint32_t>(resolve_blocking_threads(options_))),
      boost_io_state_(std::make_unique<BoostIoState>(resolve_io_threads(options_))) {
    boost_io_state_->workers.reserve(resolve_io_threads(options_));
    for (std::size_t i = 0; i < resolve_io_threads(options_); ++i) {
        boost_io_state_->workers.emplace_back([state = boost_io_state_.get()] {
            state->io_context.run();
        });
    }
    g_current_runtime.store(this, std::memory_order_release);
}

Runtime *Runtime::current() noexcept {
    return g_current_runtime.load(std::memory_order_acquire);
}

Runtime::~Runtime() noexcept {
    shutdown();
    Runtime *expected = this;
    (void)g_current_runtime.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

void Runtime::shutdown() noexcept {
    bool expected = false;
    if (!shutdown_started_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    spawn_scope_.request_stop();
    try {
        auto drained = stdexec::sync_wait(spawn_scope_.on_empty());
        (void)drained;
    } catch (...) {
    }

    if (boost_io_state_) {
        try {
            boost_io_state_->work_guard.reset();
            boost_io_state_->io_context.stop();
        } catch (...) {
        }
        for (auto &worker : boost_io_state_->workers) {
            if (!worker.joinable()) {
                continue;
            }
            if (worker.get_id() == std::this_thread::get_id()) {
                worker.detach();
            } else {
                worker.join();
            }
        }
    }

    stop_and_join_io_pool(io_pool_);
    stop_and_join_io_pool(blocking_pool_);
}

bool Runtime::is_shutting_down() const noexcept {
    return shutdown_started_.load(std::memory_order_acquire);
}

exec::asio::asio_impl::any_io_executor Runtime::executor() noexcept {
    return io_pool_.get_executor();
}

exec::asio::asio_impl::any_io_executor Runtime::executor() const noexcept {
    return io_pool_.get_executor();
}

exec::asio::asio_impl::any_io_executor Runtime::blocking_executor() noexcept {
    return blocking_pool_.get_executor();
}

exec::asio::asio_impl::any_io_executor Runtime::blocking_executor() const noexcept {
    return const_cast<exec::asio::asio_thread_pool &>(blocking_pool_).get_executor();
}

boost::asio::io_context &Runtime::io_context() noexcept {
    return boost_io_state_->io_context;
}

const boost::asio::io_context &Runtime::io_context() const noexcept {
    return boost_io_state_->io_context;
}

execution::AnyScheduler Runtime::io_scheduler() {
    return static_cast<const Runtime &>(*this).io_scheduler();
}

execution::AnyScheduler Runtime::io_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if (io_scheduler_override_) {
        return *io_scheduler_override_;
    }
    return execution::AnyScheduler(
        const_cast<exec::asio::asio_thread_pool &>(io_pool_).get_scheduler());
}

std::optional<execution::AnyScheduler> Runtime::custom_io_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return io_scheduler_override_;
}

bool Runtime::has_custom_io_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return io_scheduler_override_.has_value();
}

void Runtime::set_io_scheduler(execution::AnyScheduler scheduler) {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    io_scheduler_override_ = std::move(scheduler);
}

void Runtime::reset_io_scheduler() {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    io_scheduler_override_.reset();
}

std::optional<execution::AnyScheduler> Runtime::cpu_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return cpu_scheduler_;
}

std::optional<execution::AnyScheduler> Runtime::gpu_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return gpu_scheduler_;
}

bool Runtime::has_cpu_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return cpu_scheduler_.has_value();
}

bool Runtime::has_gpu_scheduler() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    return gpu_scheduler_.has_value();
}

void Runtime::set_cpu_scheduler(execution::AnyScheduler scheduler) {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    cpu_scheduler_ = std::move(scheduler);
}

void Runtime::set_gpu_scheduler(execution::AnyScheduler scheduler) {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    gpu_scheduler_ = std::move(scheduler);
}

void Runtime::reset_cpu_scheduler() {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    cpu_scheduler_.reset();
}

void Runtime::reset_gpu_scheduler() {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    gpu_scheduler_.reset();
}

void Runtime::register_scheduler(std::string name, execution::AnyScheduler scheduler) {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    named_schedulers_.insert_or_assign(std::move(name), std::move(scheduler));
}

void Runtime::unregister_scheduler(std::string_view name) {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if (const auto it = named_schedulers_.find(name); it != named_schedulers_.end()) {
        named_schedulers_.erase(it);
    }
}

std::optional<execution::AnyScheduler> Runtime::find_scheduler(std::string_view name) const {
    if (name == "io") {
        return io_scheduler();
    }

    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if (name == "cpu") {
        return cpu_scheduler_;
    }
    if (name == "gpu") {
        return gpu_scheduler_;
    }

    const auto it = named_schedulers_.find(name);
    if (it == named_schedulers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::string> Runtime::scheduler_names() const {
    std::vector<std::string> names{"io"};
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if (cpu_scheduler_) {
        names.emplace_back("cpu");
    }
    if (gpu_scheduler_) {
        names.emplace_back("gpu");
    }
    for (const auto &[name, _] : named_schedulers_) {
        if (name == "io" || name == "cpu" || name == "gpu") {
            continue;
        }
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void Runtime::post(std::function<void()> fn) {
    if (shutdown_started_.load(std::memory_order_acquire)) {
        return;
    }
    exec::asio::asio_impl::post(io_pool_.get_executor(), [this, fn = std::move(fn)]() mutable {
        if (shutdown_started_.load(std::memory_order_acquire)) {
            return;
        }
        fn();
    });
}

bool Runtime::in_io_thread() const noexcept {
    return running_in_executor_thread(executor());
}

bool Runtime::in_blocking_thread() const noexcept {
    return running_in_executor_thread(blocking_executor());
}

Task<Runtime *> get_current_runtime() {
    auto *runtime = co_await stdexec::read_env(execution::get_runtime);
    if (runtime == nullptr) {
        throw std::runtime_error("no current flux::Runtime in this execution context");
    }
    co_return runtime;
}

Task<exec::asio::asio_impl::any_io_executor> get_current_loop() {
    auto *runtime = co_await get_current_runtime();
    co_return runtime->executor();
}

Task<void> sleep_for(std::chrono::milliseconds delay) {
    auto ex = co_await get_current_loop();
    exec::asio::asio_impl::steady_timer timer(ex);
    timer.expires_after(std::max(delay, std::chrono::milliseconds::zero()));
    co_await timer.async_wait(exec::asio::use_sender);
}

Task<void> sleep_until(std::chrono::steady_clock::time_point deadline) {
    auto ex = co_await get_current_loop();
    exec::asio::asio_impl::steady_timer timer(ex);
    timer.expires_at(deadline);
    co_await timer.async_wait(exec::asio::use_sender);
}

} // namespace flux
