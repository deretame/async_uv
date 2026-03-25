#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <async_simple/Signal.h>
#include <async_simple/Try.h>
#include <async_simple/coro/Collect.h>
#include <async_simple/coro/Lazy.h>

#include "async_uv/error.h"
#include "async_uv/runtime.h"

namespace async_uv {

class CancellationSource {
public:
    CancellationSource();
    explicit CancellationSource(std::shared_ptr<async_simple::Signal> signal);

    [[nodiscard]] async_simple::Signal *signal() const noexcept;
    [[nodiscard]] const std::shared_ptr<async_simple::Signal> &shared_signal() const noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;
    [[nodiscard]] async_simple::SignalType
    cancel(async_simple::SignalType type = async_simple::Terminate) const noexcept;

private:
    std::shared_ptr<async_simple::Signal> signal_;
};

Task<async_simple::Slot *> get_current_slot();
Task<std::shared_ptr<async_simple::Signal>> get_current_signal();
Task<bool> cancellation_requested();
Task<void> throw_if_cancelled(std::string message = "async_uv operation canceled");

template <typename TimeoutTag, typename Duration>
Task<TimeoutTag> timeout_after(Duration delay) {
    co_await sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(delay));
    co_return TimeoutTag{};
}

template <typename Lazy>
Future<TaskValue<Lazy>> spawn(Runtime &runtime, Lazy &&lazy, CancellationSource &source) {
    return runtime.spawn(std::forward<Lazy>(lazy).setLazyLocal(source.signal()));
}

template <typename Lazy>
Future<TaskValue<Lazy>> spawn(Lazy &&lazy, CancellationSource &source) {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error(
            "async_uv::spawn with cancellation requires a current async_uv::Runtime");
    }

    return runtime->spawn(std::forward<Lazy>(lazy).setLazyLocal(source.signal()));
}

template <typename Rep, typename Period, typename Lazy>
Task<TaskValue<Lazy>> with_timeout(std::chrono::duration<Rep, Period> timeout, Lazy &&lazy) {
    using ValueType = TaskValue<Lazy>;
    using DurationType = std::chrono::duration<Rep, Period>;

    struct TimeoutTag {};
    const auto delay = timeout < DurationType::zero() ? DurationType::zero() : timeout;

    auto result = co_await async_simple::coro::collectAny<async_simple::Terminate>(
        std::forward<Lazy>(lazy), timeout_after<TimeoutTag>(delay));

    if (result.index() == 0) {
        auto &value = std::get<0>(result);
        if (value.hasError()) {
            std::rethrow_exception(value.getException());
        }

        if constexpr (std::is_void_v<ValueType>) {
            value.value();
            co_return;
        } else {
            co_return std::move(value).value();
        }
    }

    throw Error("async_uv::with_timeout", UV_ETIMEDOUT);
}

template <typename Clock, typename Duration, typename Lazy>
Task<TaskValue<Lazy>> with_deadline(std::chrono::time_point<Clock, Duration> deadline,
                                    Lazy &&lazy) {
    const auto now = Clock::now();
    const auto remaining = deadline <= now ? Duration::zero() : deadline - now;
    co_return co_await with_timeout(remaining, std::forward<Lazy>(lazy));
}

} // namespace async_uv
