#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include <uv.h>

#include "async_uv/fs.h"
#include "async_uv/message.h"

namespace async_uv {

enum class FdEventFlags : int {
    none = 0,
    readable = UV_READABLE,
    writable = UV_WRITABLE,
#ifdef UV_DISCONNECT
    disconnect = UV_DISCONNECT,
#endif
#ifdef UV_PRIORITIZED
    prioritized = UV_PRIORITIZED,
#endif
};

template <>
struct enable_bitmask_operators<FdEventFlags> : std::true_type {};

struct FdEvent {
    int status = 0;
    int events = 0;

    FdEvent() = default;
    FdEvent(int status_in, int events_in) : status(status_in), events(events_in) {}
    FdEvent(const FdEvent &) = delete;
    FdEvent &operator=(const FdEvent &) = delete;
    FdEvent(FdEvent &&) noexcept = default;
    FdEvent &operator=(FdEvent &&) noexcept = default;

    bool ok() const noexcept;
    bool readable() const noexcept;
    bool writable() const noexcept;
    bool disconnected() const noexcept;
    bool prioritized() const noexcept;
};

class FdWatcher {
public:
    using next_type = std::optional<FdEvent>;
    using task_type = Task<next_type>;
    using stream_type = Stream<FdEvent>;

    FdWatcher() = default;
    ~FdWatcher();

    FdWatcher(const FdWatcher &) = delete;
    FdWatcher &operator=(const FdWatcher &) = delete;
    FdWatcher(FdWatcher &&) noexcept = default;
    FdWatcher &operator=(FdWatcher &&) noexcept = default;

    static Task<FdWatcher> watch(uv_os_sock_t fd, int events);
    static Task<FdWatcher> watch(uv_os_sock_t fd, FdEventFlags flags);

    bool valid() const noexcept;
    uv_os_sock_t fd() const noexcept;
    int watch_events() const noexcept;

    task_type next() const;

    template <typename Rep, typename Period>
    task_type next_for(std::chrono::duration<Rep, Period> timeout) const {
        co_return co_await async_uv::with_timeout(timeout, next());
    }

    template <typename Clock, typename Duration>
    task_type next_until(std::chrono::time_point<Clock, Duration> deadline) const {
        co_return co_await async_uv::with_deadline(deadline, next());
    }

    stream_type events_stream() const;
    stream_type events() const {
        return events_stream();
    }
    Task<void> stop();

    template <typename Rep, typename Period>
    Task<void> stop_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, stop());
    }

    template <typename Clock, typename Duration>
    Task<void> stop_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, stop());
    }

private:
    struct State;

    explicit FdWatcher(std::shared_ptr<State> state) noexcept;
    void request_stop();

    std::shared_ptr<State> state_;
};

} // namespace async_uv
