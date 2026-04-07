#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <exec/asio/asio_config.hpp>
#include <exec/asio/asio_thread_pool.hpp>
#include <exec/any_sender_of.hpp>
#include <exec/start_detached.hpp>
#include <exec/asio/use_sender.hpp>
#include <stdexec/execution.hpp>

#include "async_uv/task.h"

namespace async_uv {

class Runtime;

namespace execution {

using AnySchedulerCompletions =
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>;
using AnyReceiver = exec::any_receiver<AnySchedulerCompletions>;
using AnySender = exec::any_sender<AnyReceiver>;
using AnyScheduler = exec::any_scheduler<AnySender>;

struct get_io_scheduler_t : stdexec::__query<get_io_scheduler_t> {
    using stdexec::__query<get_io_scheduler_t>::operator();

    constexpr auto operator()() const noexcept {
        return stdexec::read_env(*this);
    }

    static consteval auto query(stdexec::forwarding_query_t) noexcept -> bool {
        return true;
    }
};
struct get_cpu_scheduler_t : stdexec::__query<get_cpu_scheduler_t> {
    using stdexec::__query<get_cpu_scheduler_t>::operator();

    constexpr auto operator()() const noexcept {
        return stdexec::read_env(*this);
    }

    static consteval auto query(stdexec::forwarding_query_t) noexcept -> bool {
        return true;
    }
};
struct get_gpu_scheduler_t : stdexec::__query<get_gpu_scheduler_t> {
    using stdexec::__query<get_gpu_scheduler_t>::operator();

    constexpr auto operator()() const noexcept {
        return stdexec::read_env(*this);
    }

    static consteval auto query(stdexec::forwarding_query_t) noexcept -> bool {
        return true;
    }
};

inline constexpr get_io_scheduler_t get_io_scheduler{};
inline constexpr get_cpu_scheduler_t get_cpu_scheduler{};
inline constexpr get_gpu_scheduler_t get_gpu_scheduler{};

} // namespace execution

struct RuntimeOptions {
    std::string name = "async_uv";
    std::size_t io_threads = 0;
    std::size_t blocking_threads = 0;
};

struct TraceEvent {
    const char *category = "";
    const char *name = "";
    int code = 0;
    std::size_t value = 0;
};

using TraceHook = std::function<void(const TraceEvent &)>;

void set_trace_hook(TraceHook hook);
void reset_trace_hook();
void emit_trace_event(TraceEvent event) noexcept;

template <typename Func>
using BlockingValue =
    std::conditional_t<std::is_void_v<std::invoke_result_t<std::decay_t<Func> &>>,
                       void,
                       std::remove_cvref_t<std::invoke_result_t<std::decay_t<Func> &>>>;

namespace detail {

class RuntimeGuard {
public:
    explicit RuntimeGuard(Runtime *runtime) noexcept;
    ~RuntimeGuard();

    RuntimeGuard(const RuntimeGuard &) = delete;
    RuntimeGuard &operator=(const RuntimeGuard &) = delete;

private:
    Runtime *previous_;
};

template <typename Lazy>
Task<TaskValue<std::decay_t<Lazy>>> run_with_runtime(Runtime *runtime, Lazy lazy) {
    using Work = std::decay_t<Lazy>;
    RuntimeGuard guard(runtime);
    if constexpr (std::is_void_v<TaskValue<Work>>) {
        co_await std::move(lazy);
        co_return;
    } else {
        co_return co_await std::move(lazy);
    }
}

template <typename PromisePtr>
void set_cancelled(PromisePtr &promise) {
    try {
        promise->set_exception(
            std::make_exception_ptr(std::runtime_error("async_uv task stopped")));
    } catch (...) {
    }
}

template <typename PromisePtr>
void set_exception(PromisePtr &promise, std::exception_ptr error) {
    try {
        promise->set_exception(std::move(error));
    } catch (...) {
    }
}

template <typename PromisePtr>
void set_unknown_exception(PromisePtr &promise) {
    set_exception(promise, std::current_exception());
}

} // namespace detail

class Runtime final {
public:
    enum class ThreadRole {
        external,
        io,
        blocking,
    };

    class Builder {
    public:
        Builder &name(std::string value) {
            options_.name = std::move(value);
            return *this;
        }

        Builder &io_threads(std::size_t value) {
            options_.io_threads = value;
            return *this;
        }

        Builder &blocking_threads(std::size_t value) {
            options_.blocking_threads = value;
            return *this;
        }

        operator RuntimeOptions() const & {
            return options_;
        }

        operator RuntimeOptions() && {
            return std::move(options_);
        }

    private:
        RuntimeOptions options_;
    };

    static Builder build() {
        return Builder{};
    }

    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Runtime(Runtime &&) = delete;
    Runtime &operator=(Runtime &&) = delete;

    [[nodiscard]] exec::asio::asio_impl::any_io_executor executor() noexcept;
    [[nodiscard]] exec::asio::asio_impl::any_io_executor executor() const noexcept;
    [[nodiscard]] exec::asio::asio_impl::any_io_executor blocking_executor() noexcept;
    [[nodiscard]] exec::asio::asio_impl::any_io_executor blocking_executor() const noexcept;
    [[nodiscard]] execution::AnyScheduler io_scheduler();
    [[nodiscard]] execution::AnyScheduler io_scheduler() const;
    [[nodiscard]] std::optional<execution::AnyScheduler> custom_io_scheduler() const;
    [[nodiscard]] bool has_custom_io_scheduler() const;
    void set_io_scheduler(execution::AnyScheduler scheduler);
    void reset_io_scheduler();
    [[nodiscard]] std::optional<execution::AnyScheduler> cpu_scheduler() const;
    [[nodiscard]] std::optional<execution::AnyScheduler> gpu_scheduler() const;
    [[nodiscard]] bool has_cpu_scheduler() const;
    [[nodiscard]] bool has_gpu_scheduler() const;
    void set_cpu_scheduler(execution::AnyScheduler scheduler);
    void set_gpu_scheduler(execution::AnyScheduler scheduler);
    void reset_cpu_scheduler();
    void reset_gpu_scheduler();
    void register_scheduler(std::string name, execution::AnyScheduler scheduler);
    void unregister_scheduler(std::string_view name);
    [[nodiscard]] std::optional<execution::AnyScheduler> find_scheduler(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> scheduler_names() const;
    [[nodiscard]] auto scheduler() noexcept {
        return io_pool_.get_scheduler();
    }
    [[nodiscard]] auto scheduler() const noexcept {
        return const_cast<exec::asio::asio_thread_pool &>(io_pool_).get_scheduler();
    }

    template <typename Lazy>
    decltype(auto) block_on(Lazy &&lazy) {
        auto bound = with_execution_env(detail::run_with_runtime(this, std::forward<Lazy>(lazy)));
        auto started = stdexec::starts_on(io_scheduler(), std::move(bound));
        auto result = stdexec::sync_wait(std::move(started));
        if (!result.has_value()) {
            throw std::runtime_error("async_uv::Runtime::block_on: task stopped");
        }

        if constexpr (std::is_void_v<TaskValue<Lazy>>) {
            return;
        } else {
            return std::move(std::get<0>(*result));
        }
    }

    template <typename Lazy>
    Future<TaskValue<Lazy>> spawn(Lazy &&lazy) {
        return spawn_with_stop_token(std::forward<Lazy>(lazy), stdexec::never_stop_token{});
    }

    template <typename Lazy>
    Future<TaskValue<Lazy>> spawn(Lazy &&lazy, stdexec::inplace_stop_token token) {
        return spawn_with_stop_token(std::forward<Lazy>(lazy), token);
    }

    template <typename Func>
    Future<BlockingValue<Func>> spawn_blocking(Func &&func) {
        return spawn_blocking_with_stop_token(std::forward<Func>(func), stdexec::never_stop_token{});
    }

    template <typename Func>
    Future<BlockingValue<Func>> spawn_blocking(Func &&func, stdexec::inplace_stop_token token) {
        return spawn_blocking_with_stop_token(std::forward<Func>(func), token);
    }

    void post(std::function<void()> fn);

    static Runtime *current() noexcept;
    static ThreadRole current_thread_role() noexcept;
    [[nodiscard]] bool in_io_thread() const noexcept;
    [[nodiscard]] bool in_blocking_thread() const noexcept;
    static exec::asio::asio_impl::any_io_executor current_executor();

private:
    template <typename Sender>
    auto with_execution_env(Sender &&sender) const {
        return static_cast<Sender &&>(sender)
             | stdexec::write_env(stdexec::prop(execution::get_io_scheduler, io_scheduler()))
             | stdexec::write_env(stdexec::prop(execution::get_cpu_scheduler, cpu_scheduler()))
             | stdexec::write_env(stdexec::prop(execution::get_gpu_scheduler, gpu_scheduler()));
    }

    template <typename StopToken>
    static bool stop_requested(const StopToken &token) {
        if constexpr (requires { token.stop_requested(); }) {
            return token.stop_requested();
        }
        return false;
    }

    template <typename Func, typename StopToken>
    Future<BlockingValue<Func>> spawn_blocking_with_stop_token(Func &&func, StopToken token) {
        using Work = std::decay_t<Func>;
        using ValueType = BlockingValue<Func>;

        auto promise = std::make_shared<std::promise<ValueType>>();
        auto future = promise->get_future().share();
        auto work = Work(std::forward<Func>(func));

        if (stop_requested(token)) {
            detail::set_cancelled(promise);
            return future;
        }

        exec::asio::asio_impl::post(
            blocking_pool_,
            [this, work = std::move(work), promise, token = std::move(token)]() mutable {
                detail::RuntimeGuard guard(this);
                if (stop_requested(token)) {
                    detail::set_cancelled(promise);
                    return;
                }
                try {
                    if constexpr (std::is_void_v<ValueType>) {
                        std::invoke(work);
                        promise->set_value();
                    } else {
                        promise->set_value(std::invoke(work));
                    }
                } catch (...) {
                    detail::set_unknown_exception(promise);
                }
            });

        return future;
    }

    template <typename Lazy, typename StopToken>
    Future<TaskValue<Lazy>> spawn_with_stop_token(Lazy &&lazy, StopToken token) {
        using ValueType = TaskValue<Lazy>;
        auto promise = std::make_shared<std::promise<ValueType>>();
        auto future = promise->get_future().share();

        auto work = with_execution_env(detail::run_with_runtime(this, std::forward<Lazy>(lazy)));
        auto cancellable = stdexec::starts_on(io_scheduler(), std::move(work))
                         | stdexec::write_env(
                             stdexec::prop(stdexec::get_stop_token, std::move(token)));

        if constexpr (std::is_void_v<ValueType>) {
            auto sender = std::move(cancellable)
                          | stdexec::then([promise]() mutable {
                                promise->set_value();
                            })
                          | stdexec::upon_error(
                              [promise](std::exception_ptr error) mutable {
                                  detail::set_exception(promise, std::move(error));
                              })
                          | stdexec::upon_stopped(
                              [promise]() mutable {
                                  detail::set_cancelled(promise);
                              });
            exec::start_detached(std::move(sender));
        } else {
            auto sender = std::move(cancellable)
                          | stdexec::then([promise](ValueType value) mutable {
                                promise->set_value(std::move(value));
                            })
                          | stdexec::upon_error(
                              [promise](std::exception_ptr error) mutable {
                                  detail::set_exception(promise, std::move(error));
                              })
                          | stdexec::upon_stopped(
                              [promise]() mutable {
                                  detail::set_cancelled(promise);
                              });
            exec::start_detached(std::move(sender));
        }

        return future;
    }

    RuntimeOptions options_;
    exec::asio::asio_thread_pool io_pool_;
    exec::asio::asio_impl::thread_pool blocking_pool_;
    mutable std::mutex scheduler_mutex_;
    std::optional<execution::AnyScheduler> io_scheduler_override_;
    std::optional<execution::AnyScheduler> cpu_scheduler_;
    std::optional<execution::AnyScheduler> gpu_scheduler_;
    std::unordered_map<std::string, execution::AnyScheduler> named_schedulers_;
};

Runtime *try_current_runtime() noexcept;
exec::asio::asio_impl::any_io_executor try_current_executor() noexcept;

template <typename Lazy>
Future<TaskValue<Lazy>> spawn(Runtime &runtime, Lazy &&lazy) {
    return runtime.spawn(std::forward<Lazy>(lazy));
}

template <typename Lazy>
Future<TaskValue<Lazy>> spawn(Lazy &&lazy) {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error("async_uv::spawn requires a current async_uv::Runtime");
    }
    return runtime->spawn(std::forward<Lazy>(lazy));
}

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Runtime &runtime, Func &&func) {
    return runtime.spawn_blocking(std::forward<Func>(func));
}

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Runtime &runtime,
                                           Func &&func,
                                           stdexec::inplace_stop_token token) {
    return runtime.spawn_blocking(std::forward<Func>(func), token);
}

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Func &&func) {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error("async_uv::spawn_blocking requires a current async_uv::Runtime");
    }
    return runtime->spawn_blocking(std::forward<Func>(func));
}

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Func &&func, stdexec::inplace_stop_token token) {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error("async_uv::spawn_blocking requires a current async_uv::Runtime");
    }
    return runtime->spawn_blocking(std::forward<Func>(func), token);
}

Task<Runtime *> get_current_runtime();
Task<exec::asio::asio_impl::any_io_executor> get_current_loop();
Task<void> sleep_for(std::chrono::milliseconds delay);
Task<void> sleep_until(std::chrono::steady_clock::time_point deadline);

} // namespace async_uv
