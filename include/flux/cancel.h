#pragma once

#include <chrono>
#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include "flux/error.h"
#include "flux/runtime.h"

namespace flux {

class CancellationSource {
public:
    CancellationSource();
    explicit CancellationSource(std::shared_ptr<stdexec::inplace_stop_source> source);

    [[nodiscard]] stdexec::inplace_stop_source *signal() const noexcept;
    [[nodiscard]] const std::shared_ptr<stdexec::inplace_stop_source> &shared_signal() const noexcept;
    [[nodiscard]] stdexec::inplace_stop_token token() const noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;
    [[nodiscard]] bool cancel() const noexcept;

private:
    std::shared_ptr<stdexec::inplace_stop_source> source_;
};

Task<stdexec::inplace_stop_token> get_current_slot();
Task<std::shared_ptr<stdexec::inplace_stop_source>> get_current_signal();
Task<bool> cancellation_requested();
Task<void> throw_if_cancelled(std::string message = "flux operation canceled");

template <typename TimeoutTag, typename Duration>
Task<TimeoutTag> timeout_after(Duration delay) {
    co_await sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(delay));
    co_return TimeoutTag{};
}

template <typename Lazy>
auto spawn(Runtime &runtime, Lazy &&lazy, CancellationSource &source) {
    return runtime.spawn(std::forward<Lazy>(lazy), source.token());
}

template <typename Func>
auto spawn_blocking(Runtime &runtime, Func &&func, CancellationSource &source) {
    return runtime.spawn_blocking(std::forward<Func>(func), source.token());
}

template <typename Rep, typename Period, typename Lazy>
Task<TaskValue<Lazy>> with_timeout(std::chrono::duration<Rep, Period> timeout, Lazy lazy) {
    using ValueType = TaskValue<Lazy>;
    const auto delay = timeout < decltype(timeout)::zero() ? decltype(timeout)::zero() : timeout;
    if constexpr (std::is_void_v<ValueType>) {
        const bool completed = co_await exec::when_any(
            std::move(lazy) | stdexec::then([] { return true; }),
            timeout_after<bool>(delay));
        if (!completed) {
            throw Error("flux::with_timeout", ETIMEDOUT);
        }
        co_return;
    } else {
        using StoredValue = std::remove_cvref_t<ValueType>;
        auto maybe_value = co_await exec::when_any(
            std::move(lazy)
                | stdexec::then(
                    [](auto value) { return std::optional<StoredValue>(std::move(value)); }),
            timeout_after<std::optional<StoredValue>>(delay));
        if (!maybe_value.has_value()) {
            throw Error("flux::with_timeout", ETIMEDOUT);
        }
        co_return std::move(*maybe_value);
    }
}

template <typename Clock, typename Duration, typename Lazy>
Task<TaskValue<Lazy>> with_deadline(std::chrono::time_point<Clock, Duration> deadline, Lazy lazy) {
    const auto now = Clock::now();
    const auto remaining = deadline <= now ? Duration::zero() : deadline - now;
    co_return co_await with_timeout(remaining, std::move(lazy));
}

} // namespace flux
