#include "flux/watch.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <system_error>
#include <vector>

#include <asio/posix/stream_descriptor.hpp>
#include <unistd.h>

namespace flux {

namespace {

namespace asio = exec::asio::asio_impl;

struct Snapshot {
    bool exists = false;
    FileInfo info{};
};

struct PendingEvent {
    std::string name;
    int status = 0;
    std::uint32_t mask = 0;
    bool ignored = false;
};

std::optional<FileInfo> load_file_info(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            throw std::system_error(ec);
        }
        return std::nullopt;
    }

    FileInfo info;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        throw std::system_error(ec);
    }

    info.regular_file = std::filesystem::is_regular_file(status);
    info.directory = std::filesystem::is_directory(status);
    info.symlink = std::filesystem::is_symlink(status);
    if (info.regular_file) {
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            throw std::system_error(ec);
        }
        info.size = static_cast<std::uint64_t>(size);
    }
    info.last_write_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        throw std::system_error(ec);
    }
    return info;
}

Task<std::optional<FileInfo>> load_file_info_async(std::filesystem::path path) {
    auto *runtime = co_await get_current_runtime();
    auto sender = stdexec::starts_on(
        runtime->blocking_scheduler(),
        stdexec::just() | stdexec::then([path = std::move(path)]() mutable {
            return load_file_info(path);
        }));
    co_return co_await std::move(sender);
}

std::string normalize_inotify_name(const char *name, std::size_t len) {
    if (name == nullptr || len == 0) {
        return {};
    }
    const auto *end = static_cast<const char *>(memchr(name, '\0', len));
    const auto n = end == nullptr ? len : static_cast<std::size_t>(end - name);
    return std::string(name, n);
}

int default_watch_mask() {
    return IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
           IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF;
}

} // namespace

struct FsWatcher::State {
    Runtime *runtime = nullptr;
    std::filesystem::path watched_path;
    int inotify_fd = -1;
    int watch_fd = -1;
    asio::posix::stream_descriptor descriptor;
    std::vector<char> pending;
    std::size_t pending_offset = 0;
    Snapshot snapshot{};
    bool running = true;
    mutable std::mutex mutex;

    explicit State(Runtime *rt)
        : runtime(rt),
          descriptor(rt->executor()) {}

    ~State() {
        close_all();
    }

    void close_all() {
        std::lock_guard<std::mutex> lock(mutex);
        running = false;

        if (watch_fd >= 0 && inotify_fd >= 0) {
            (void)::inotify_rm_watch(inotify_fd, watch_fd);
            watch_fd = -1;
        }

        std::error_code ec;
        descriptor.cancel(ec);
        descriptor.close(ec);
        inotify_fd = -1;
    }

    std::optional<PendingEvent> try_pop_pending_event_locked() {
        const auto available = pending.size() - pending_offset;
        if (available < sizeof(inotify_event)) {
            return std::nullopt;
        }

        const auto *base = pending.data() + pending_offset;
        const auto *raw = reinterpret_cast<const inotify_event *>(base);
        const std::size_t event_size = sizeof(inotify_event) + raw->len;
        if (available < event_size) {
            return std::nullopt;
        }

        PendingEvent event;
        event.name = normalize_inotify_name(raw->name, raw->len);
        event.mask = raw->mask;
        if ((raw->mask & IN_Q_OVERFLOW) != 0U) {
            event.status = ENOSPC;
        } else {
            event.status = 0;
        }

        pending_offset += event_size;
        if (pending_offset >= pending.size()) {
            pending.clear();
            pending_offset = 0;
        } else if (pending_offset > 2048) {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pending_offset));
            pending_offset = 0;
        }

        event.ignored = (raw->mask & IN_IGNORED) != 0U;

        return event;
    }
};

FsWatcher::FsWatcher(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

Task<FsWatcher> FsWatcher::watch(std::filesystem::path path) {
    auto *runtime = co_await get_current_runtime();
    auto state = std::make_shared<State>(runtime);
    state->watched_path = std::move(path);
    auto initial = co_await load_file_info_async(state->watched_path);
    state->snapshot.exists = initial.has_value();
    if (initial) {
        state->snapshot.info = *initial;
    }

    const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "inotify_init1");
    }
    state->inotify_fd = fd;

    const int wd = ::inotify_add_watch(fd, state->watched_path.c_str(), default_watch_mask());
    if (wd < 0) {
        const int err = errno;
        (void)::close(fd);
        state->inotify_fd = -1;
        throw std::system_error(err, std::generic_category(), "inotify_add_watch");
    }
    state->watch_fd = wd;

    state->descriptor.assign(fd);
    co_return FsWatcher(std::move(state));
}

bool FsWatcher::valid() const noexcept {
    return state_ != nullptr;
}

const std::filesystem::path &FsWatcher::path() const noexcept {
    static const std::filesystem::path empty;
    return state_ ? state_->watched_path : empty;
}

FsWatcher::task_type FsWatcher::next() const {
    if (!state_) {
        co_return std::nullopt;
    }

    while (true) {
        std::optional<PendingEvent> pending_event;
        Snapshot previous_snapshot;
        std::filesystem::path watched_path;

        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->running) {
                co_return std::nullopt;
            }
            if (auto ready = state_->try_pop_pending_event_locked()) {
                pending_event = std::move(ready);
                previous_snapshot = state_->snapshot;
                watched_path = state_->watched_path;
            }
        }

        if (pending_event) {
            auto current = co_await load_file_info_async(watched_path);

            FsWatchEvent event;
            event.path = std::move(watched_path);
            event.name = std::move(pending_event->name);
            event.status = pending_event->status;
            event.mask = pending_event->mask;
            if (previous_snapshot.exists) {
                event.previous = previous_snapshot.info;
            }
            if (current) {
                event.current = *current;
            }

            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->snapshot.exists = current.has_value();
                if (current) {
                    state_->snapshot.info = *current;
                }
                if (pending_event->ignored) {
                    state_->running = false;
                }
            }

            co_return event;
        }

        std::array<char, 4096> chunk{};
        std::size_t n = 0;
        try {
            n = co_await state_->descriptor.async_read_some(
                asio::buffer(chunk.data(), chunk.size()), exec::asio::use_sender);
        } catch (const std::system_error &error) {
            if (error.code() == asio::error::operation_aborted ||
                error.code() == asio::error::bad_descriptor) {
                co_return std::nullopt;
            }
            throw;
        }

        if (n == 0) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->running) {
                co_return std::nullopt;
            }
            state_->pending.insert(state_->pending.end(), chunk.begin(), chunk.begin() + n);
            if (auto ready = state_->try_pop_pending_event_locked()) {
                pending_event = std::move(ready);
                previous_snapshot = state_->snapshot;
                watched_path = state_->watched_path;
            }
        }

        if (pending_event) {
            auto current = co_await load_file_info_async(watched_path);

            FsWatchEvent event;
            event.path = std::move(watched_path);
            event.name = std::move(pending_event->name);
            event.status = pending_event->status;
            event.mask = pending_event->mask;
            if (previous_snapshot.exists) {
                event.previous = previous_snapshot.info;
            }
            if (current) {
                event.current = *current;
            }

            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->snapshot.exists = current.has_value();
                if (current) {
                    state_->snapshot.info = *current;
                }
                if (pending_event->ignored) {
                    state_->running = false;
                }
            }

            co_return event;
        }
    }
}

FsWatcher::stream_type FsWatcher::events() const {
    auto state = state_;
    return stream_type([state = std::move(state)]() -> task_type {
        if (!state) {
            co_return std::nullopt;
        }
        FsWatcher watcher(state);
        co_return co_await watcher.next();
    });
}

Task<void> FsWatcher::stop() {
    if (!state_) {
        co_return;
    }
    state_->close_all();
}

} // namespace flux
