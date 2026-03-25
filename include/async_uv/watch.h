#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <async_simple/coro/Generator.h>
#include <uv.h>

#include "async_uv/fs.h"
#include "async_uv/message.h"

namespace async_uv {

enum class FsEventFlags : unsigned {
    none = 0,
    watch_entry = UV_FS_EVENT_WATCH_ENTRY,
    stat = UV_FS_EVENT_STAT,
    recursive = UV_FS_EVENT_RECURSIVE,
};

template <>
struct enable_bitmask_operators<FsEventFlags> : std::true_type {};

struct FsEvent {
    std::string path;
    std::string name;
    int events = 0;
    int status = 0;

    bool ok() const noexcept;
    bool renamed() const noexcept;
    bool changed() const noexcept;
};

struct FsPollEvent {
    std::string path;
    int status = 0;
    std::optional<FileInfo> previous;
    std::optional<FileInfo> current;

    bool ok() const noexcept;
};

class FsEventWatcher {
public:
    using next_type = std::optional<FsEvent>;
    using task_type = Task<next_type>;
    using stream_type = async_simple::coro::Generator<task_type>;

    FsEventWatcher() = default;
    ~FsEventWatcher();

    FsEventWatcher(const FsEventWatcher &) = delete;
    FsEventWatcher &operator=(const FsEventWatcher &) = delete;
    FsEventWatcher(FsEventWatcher &&other) noexcept = default;
    FsEventWatcher &operator=(FsEventWatcher &&other) noexcept = default;

    static Task<FsEventWatcher> watch(std::string path, unsigned flags = 0);
    static Task<FsEventWatcher> watch(std::string path, FsEventFlags flags);

    bool valid() const noexcept;
    const std::string &path() const noexcept;
    unsigned flags() const noexcept;

    task_type next() const;
    template <typename Rep, typename Period>
    task_type next_for(std::chrono::duration<Rep, Period> timeout) const {
        co_return co_await async_uv::with_timeout(timeout, next());
    }
    template <typename Clock, typename Duration>
    task_type next_until(std::chrono::time_point<Clock, Duration> deadline) const {
        co_return co_await async_uv::with_deadline(deadline, next());
    }
    stream_type events() const;
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

    explicit FsEventWatcher(std::shared_ptr<State> state) noexcept;
    void request_stop();

    std::shared_ptr<State> state_;
};

class FsPollWatcher {
public:
    using next_type = std::optional<FsPollEvent>;
    using task_type = Task<next_type>;
    using stream_type = async_simple::coro::Generator<task_type>;

    FsPollWatcher() = default;
    ~FsPollWatcher();

    FsPollWatcher(const FsPollWatcher &) = delete;
    FsPollWatcher &operator=(const FsPollWatcher &) = delete;
    FsPollWatcher(FsPollWatcher &&other) noexcept = default;
    FsPollWatcher &operator=(FsPollWatcher &&other) noexcept = default;

    static Task<FsPollWatcher> watch(std::string path, std::chrono::milliseconds interval);

    bool valid() const noexcept;
    const std::string &path() const noexcept;
    std::chrono::milliseconds interval() const noexcept;

    task_type next() const;
    template <typename Rep, typename Period>
    task_type next_for(std::chrono::duration<Rep, Period> timeout) const {
        co_return co_await async_uv::with_timeout(timeout, next());
    }
    template <typename Clock, typename Duration>
    task_type next_until(std::chrono::time_point<Clock, Duration> deadline) const {
        co_return co_await async_uv::with_deadline(deadline, next());
    }
    stream_type events() const;
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

    explicit FsPollWatcher(std::shared_ptr<State> state) noexcept;
    void request_stop();

    std::shared_ptr<State> state_;
};

} // namespace async_uv
