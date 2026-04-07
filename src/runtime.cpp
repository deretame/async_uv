#include "async_uv/runtime.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace async_uv {

namespace {

thread_local Runtime *tls_current_runtime = nullptr;
thread_local Runtime::ThreadRole tls_thread_role = Runtime::ThreadRole::external;
std::mutex g_trace_hook_mutex;
TraceHook g_trace_hook;

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

template <typename PostFn>
void bind_runtime_to_pool_threads(std::size_t thread_count,
                                  Runtime *runtime,
                                  Runtime::ThreadRole role,
                                  PostFn post_fn) {
    if (thread_count == 0) {
        return;
    }

    auto start_signal = std::make_shared<std::promise<void>>();
    auto start_gate = std::make_shared<std::shared_future<void>>(start_signal->get_future().share());
    auto done_signal = std::make_shared<std::promise<void>>();
    auto done_future = done_signal->get_future();
    auto ready_count = std::make_shared<std::atomic<std::size_t>>(0);
    auto leave_count = std::make_shared<std::atomic<std::size_t>>(thread_count);

    for (std::size_t i = 0; i < thread_count; ++i) {
        post_fn([runtime,
                 role,
                 thread_count,
                 start_signal,
                 start_gate,
                 done_signal,
                 ready_count,
                 leave_count] {
            tls_current_runtime = runtime;
            tls_thread_role = role;

            const auto ready = ready_count->fetch_add(1, std::memory_order_acq_rel) + 1;
            if (ready == thread_count) {
                try {
                    start_signal->set_value();
                } catch (...) {
                }
            }

            start_gate->wait();
            const auto remaining = leave_count->fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                try {
                    done_signal->set_value();
                } catch (...) {
                }
            }
        });
    }

    done_future.wait();
}

} // namespace

namespace detail {

RuntimeGuard::RuntimeGuard(Runtime *runtime) noexcept : previous_(Runtime::current()) {
    tls_current_runtime = runtime;
}

RuntimeGuard::~RuntimeGuard() {
    tls_current_runtime = previous_;
}

} // namespace detail

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
      blocking_pool_(resolve_blocking_threads(options_)) {
    bind_runtime_to_pool_threads(
        resolve_io_threads(options_), this, ThreadRole::io, [this](auto fn) {
            exec::asio::asio_impl::post(io_pool_.get_executor(), std::move(fn));
        });
    bind_runtime_to_pool_threads(
        resolve_blocking_threads(options_), this, ThreadRole::blocking, [this](auto fn) {
            exec::asio::asio_impl::post(blocking_pool_, std::move(fn));
        });
}

Runtime::~Runtime() {
    blocking_pool_.join();
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
    return const_cast<exec::asio::asio_impl::thread_pool &>(blocking_pool_).get_executor();
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
    named_schedulers_[std::move(name)] = std::move(scheduler);
}

void Runtime::unregister_scheduler(std::string_view name) {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    named_schedulers_.erase(std::string(name));
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

    const auto it = named_schedulers_.find(std::string(name));
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
    exec::asio::asio_impl::post(io_pool_.get_executor(), [this, fn = std::move(fn)]() mutable {
        detail::RuntimeGuard guard(this);
        fn();
    });
}

Runtime *Runtime::current() noexcept {
    return tls_current_runtime;
}

Runtime::ThreadRole Runtime::current_thread_role() noexcept {
    return tls_thread_role;
}

bool Runtime::in_io_thread() const noexcept {
    return Runtime::current() == this && Runtime::current_thread_role() == ThreadRole::io;
}

bool Runtime::in_blocking_thread() const noexcept {
    return Runtime::current() == this && Runtime::current_thread_role() == ThreadRole::blocking;
}

exec::asio::asio_impl::any_io_executor Runtime::current_executor() {
    auto *runtime = current();
    if (runtime == nullptr) {
        throw std::runtime_error("async_uv::Runtime::current_executor requires a current runtime");
    }
    return runtime->executor();
}

Runtime *try_current_runtime() noexcept {
    return Runtime::current();
}

exec::asio::asio_impl::any_io_executor try_current_executor() noexcept {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        return {};
    }
    return runtime->executor();
}

Task<Runtime *> get_current_runtime() {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error("no current async_uv::Runtime in this execution context");
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

} // namespace async_uv
