#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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
#include <exec/async_scope.hpp>
#include <exec/any_sender_of.hpp>
#include <exec/asio/use_sender.hpp>
#include <exec/env.hpp>
#include <stdexec/execution.hpp>

#include "flux/task.h"

namespace boost::asio {
class io_context;
}

namespace flux {

class Runtime;

namespace execution {

using AnySchedulerCompletions =
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>;
using AnySender = exec::any_receiver_ref<AnySchedulerCompletions>::any_sender<>;
using AnyScheduler = AnySender::any_scheduler<>;

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

namespace detail {

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t operator()(const std::string &value) const noexcept {
        return (*this)(std::string_view(value));
    }

    std::size_t operator()(const char *value) const noexcept {
        return (*this)(std::string_view(value));
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view left, std::string_view right) const noexcept {
        return left == right;
    }
};

} // namespace detail

struct RuntimeOptions {
    std::string name = "flux";
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

class Runtime final {
public:
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

    static Runtime *current() noexcept;

    explicit Runtime(RuntimeOptions options = {});
    ~Runtime() noexcept;

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Runtime(Runtime &&) = delete;
    Runtime &operator=(Runtime &&) = delete;

    [[nodiscard]] exec::asio::asio_impl::any_io_executor executor() noexcept;
    [[nodiscard]] exec::asio::asio_impl::any_io_executor executor() const noexcept;
    [[nodiscard]] exec::asio::asio_impl::any_io_executor blocking_executor() noexcept;
    [[nodiscard]] exec::asio::asio_impl::any_io_executor blocking_executor() const noexcept;
    [[nodiscard]] boost::asio::io_context &io_context() noexcept;
    [[nodiscard]] const boost::asio::io_context &io_context() const noexcept;
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
    [[nodiscard]] auto blocking_scheduler() noexcept {
        return blocking_pool_.get_scheduler();
    }
    [[nodiscard]] auto blocking_scheduler() const noexcept {
        return const_cast<exec::asio::asio_thread_pool &>(blocking_pool_).get_scheduler();
    }

    template <typename Lazy>
    decltype(auto) block_on(Lazy &&lazy) {
        auto bound = with_execution_env(std::forward<Lazy>(lazy));
        auto started = stdexec::starts_on(scheduler(), std::move(bound));
        auto result = stdexec::sync_wait(std::move(started));
        if (!result.has_value()) {
            throw std::runtime_error("flux::Runtime::block_on: task stopped");
        }

        if constexpr (std::is_void_v<TaskValue<Lazy>>) {
            return;
        } else {
            return std::move(std::get<0>(*result));
        }
    }

    template <typename Lazy>
    auto spawn(Lazy &&lazy) {
        if (shutdown_started_.load(std::memory_order_acquire)) {
            throw std::runtime_error("flux::Runtime::spawn called during shutdown");
        }
        return spawn_with_stop_token(std::forward<Lazy>(lazy), stdexec::never_stop_token{});
    }

    template <typename Lazy>
    auto spawn(Lazy &&lazy, stdexec::inplace_stop_token token) {
        if (shutdown_started_.load(std::memory_order_acquire)) {
            throw std::runtime_error("flux::Runtime::spawn called during shutdown");
        }
        return spawn_with_stop_token(std::forward<Lazy>(lazy), token);
    }

    template <typename Func>
    auto spawn_blocking(Func &&func) {
        if (shutdown_started_.load(std::memory_order_acquire)) {
            throw std::runtime_error("flux::Runtime::spawn_blocking called during shutdown");
        }
        return spawn_blocking_with_stop_token(std::forward<Func>(func), stdexec::never_stop_token{});
    }

    template <typename Func>
    auto spawn_blocking(Func &&func, stdexec::inplace_stop_token token) {
        if (shutdown_started_.load(std::memory_order_acquire)) {
            throw std::runtime_error("flux::Runtime::spawn_blocking called during shutdown");
        }
        return spawn_blocking_with_stop_token(std::forward<Func>(func), token);
    }

    void post(std::function<void()> fn);
    void shutdown() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

    [[nodiscard]] bool in_io_thread() const noexcept;
    [[nodiscard]] bool in_blocking_thread() const noexcept;

private:
    struct BoostIoState;

    template <typename Sender>
    auto with_execution_env(Sender &&sender) const {
        return static_cast<Sender &&>(sender)
             | stdexec::write_env(stdexec::prop(execution::get_io_scheduler, scheduler()))
             | stdexec::write_env(stdexec::prop(execution::get_cpu_scheduler, cpu_scheduler()))
             | stdexec::write_env(stdexec::prop(execution::get_gpu_scheduler, gpu_scheduler()))
             | stdexec::write_env(
                 stdexec::prop(execution::get_runtime, const_cast<Runtime *>(this)));
    }

    template <typename Func, typename StopToken>
    auto spawn_blocking_with_stop_token(Func &&func, StopToken token) {
        using Work = std::decay_t<Func>;
        using ValueType = BlockingValue<Func>;
        auto work = Work(std::forward<Func>(func));
        auto work_sender = stdexec::just()
                         | stdexec::then([work = std::move(work)]() mutable -> ValueType {
                               if constexpr (std::is_void_v<ValueType>) {
                                   std::invoke(work);
                               } else {
                                   return std::invoke(work);
                               }
                           });

        auto cancellable = stdexec::starts_on(blocking_scheduler(), std::move(work_sender))
                         | stdexec::write_env(
                             stdexec::prop(stdexec::get_stop_token, std::move(token)))
                         | stdexec::stopped_as_error(
                             std::make_exception_ptr(
                                 std::runtime_error("flux task stopped")));
        return spawn_scope_.spawn_future(std::move(cancellable));
    }

    template <typename Lazy, typename StopToken>
    auto spawn_with_stop_token(Lazy &&lazy, StopToken token) {
        auto work = with_execution_env(std::forward<Lazy>(lazy));
        auto sender = stdexec::starts_on(scheduler(), std::move(work));
        auto cancellable = std::move(sender)
                         | stdexec::write_env(
                             stdexec::prop(stdexec::get_stop_token, std::move(token)))
                         | stdexec::stopped_as_error(
                             std::make_exception_ptr(
                                 std::runtime_error("flux task stopped")));

        return spawn_scope_.spawn_future(std::move(cancellable));
    }

    RuntimeOptions options_;
    exec::asio::asio_thread_pool io_pool_;
    exec::asio::asio_thread_pool blocking_pool_;
    std::unique_ptr<BoostIoState> boost_io_state_;
    exec::async_scope spawn_scope_;
    std::atomic<bool> shutdown_started_{false};
    mutable std::mutex scheduler_mutex_;
    std::optional<execution::AnyScheduler> io_scheduler_override_;
    std::optional<execution::AnyScheduler> cpu_scheduler_;
    std::optional<execution::AnyScheduler> gpu_scheduler_;
    std::unordered_map<std::string,
                       execution::AnyScheduler,
                       detail::TransparentStringHash,
                       detail::TransparentStringEqual>
        named_schedulers_;
};

template <typename Lazy>
auto spawn(Runtime &runtime, Lazy &&lazy) {
    return runtime.spawn(std::forward<Lazy>(lazy));
}

template <typename Func>
auto spawn_blocking(Runtime &runtime, Func &&func) {
    return runtime.spawn_blocking(std::forward<Func>(func));
}

template <typename Func>
auto spawn_blocking(Runtime &runtime, Func &&func, stdexec::inplace_stop_token token) {
    return runtime.spawn_blocking(std::forward<Func>(func), token);
}

Task<Runtime *> get_current_runtime();
Task<exec::asio::asio_impl::any_io_executor> get_current_loop();
Task<void> sleep_for(std::chrono::milliseconds delay);
Task<void> sleep_until(std::chrono::steady_clock::time_point deadline);

} // namespace flux
