#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <sys/inotify.h>

#include "flux/cancel.h"
#include "flux/fs.h"
#include "flux/stream.h"

namespace flux {

// Linux-only watcher backed by inotify.
struct FsWatchEvent {
    std::filesystem::path path;
    std::string name;
    int status = 0;
    std::uint32_t mask = 0;
    std::optional<FileInfo> previous;
    std::optional<FileInfo> current;

    bool ok() const noexcept {
        return status == 0;
    }

    bool modified() const noexcept {
        return (mask & (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE)) != 0;
    }

    bool renamed() const noexcept {
        return (mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF)) != 0;
    }

    bool removed() const noexcept {
        return (mask & (IN_DELETE | IN_DELETE_SELF)) != 0;
    }
};

class FsWatcher {
public:
    using next_type = std::optional<FsWatchEvent>;
    using task_type = Task<next_type>;
    using stream_type = Stream<FsWatchEvent>;

    FsWatcher() = default;
    ~FsWatcher() = default;

    FsWatcher(const FsWatcher &) = delete;
    FsWatcher &operator=(const FsWatcher &) = delete;
    FsWatcher(FsWatcher &&other) noexcept = default;
    FsWatcher &operator=(FsWatcher &&other) noexcept = default;

    static Task<FsWatcher> watch(std::filesystem::path path);

    bool valid() const noexcept;
    const std::filesystem::path &path() const noexcept;

    task_type next() const;

    template <typename Rep, typename Period>
    task_type next_for(std::chrono::duration<Rep, Period> timeout) const {
        co_return co_await flux::with_timeout(timeout, next());
    }

    template <typename Clock, typename Duration>
    task_type next_until(std::chrono::time_point<Clock, Duration> deadline) const {
        co_return co_await flux::with_deadline(deadline, next());
    }

    stream_type events() const;
    Task<void> stop();

private:
    struct State;
    explicit FsWatcher(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace flux
