#include "async_uv/watch.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <async_simple/Promise.h>

#include "async_uv/error.h"

namespace async_uv {

namespace {

FileInfo make_file_info(const uv_stat_t &stat) {
    FileInfo info;
    info.size = static_cast<std::uint64_t>(stat.st_size);
    info.mode = stat.st_mode;
    info.inode = static_cast<std::uint64_t>(stat.st_ino);
    info.device = static_cast<std::uint64_t>(stat.st_dev);
    info.uid = static_cast<int>(stat.st_uid);
    info.gid = static_cast<int>(stat.st_gid);
    info.access_time = stat.st_atim;
    info.modify_time = stat.st_mtim;
    info.change_time = stat.st_ctim;
    info.birth_time = stat.st_birthtim;
    return info;
}

} // namespace

bool FsEvent::ok() const noexcept {
    return status >= 0;
}

bool FsEvent::renamed() const noexcept {
    return (events & UV_RENAME) != 0;
}

bool FsEvent::changed() const noexcept {
    return (events & UV_CHANGE) != 0;
}

bool FsPollEvent::ok() const noexcept {
    return status >= 0;
}

struct FsEventWatcher::State : std::enable_shared_from_this<FsEventWatcher::State> {
    State(Runtime *runtime_in, Mailbox<FsEvent> mailbox_in, std::string path_in, unsigned flags_in)
        : runtime(runtime_in), mailbox(std::move(mailbox_in)), sender(mailbox.sender()),
          watched_path(std::move(path_in)), watch_flags(flags_in) {
        std::error_code error;
        watched_is_directory =
            std::filesystem::is_directory(std::filesystem::path(watched_path), error);
    }

    void request_stop() {
        auto self = shared_from_this();
        runtime->post([self]() mutable {
            std::vector<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
            bool should_close = false;

            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (self->closed) {
                    stop_waiters.assign(self->stop_waiters.begin(), self->stop_waiters.end());
                    self->stop_waiters.clear();
                } else {
                    if (!self->stop_requested) {
                        self->stop_requested = true;
                        self->self_keepalive = self;
                        if (self->initialized && !self->closing) {
                            self->closing = true;
                            should_close = true;
                        }
                    }
                }
            }

            self->sender.close();

            for (auto &waiter : stop_waiters) {
                waiter->setValue();
            }

            if (!should_close) {
                return;
            }

            uv_fs_event_stop(&self->handle);
            uv_close(reinterpret_cast<uv_handle_t *>(&self->handle), [](uv_handle_t *handle) {
                auto *state = static_cast<State *>(handle->data);
                std::vector<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
                std::shared_ptr<State> keepalive;

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->initialized = false;
                    state->closing = false;
                    state->closed = true;
                    stop_waiters.assign(state->stop_waiters.begin(), state->stop_waiters.end());
                    state->stop_waiters.clear();
                    keepalive = std::move(state->self_keepalive);
                }

                for (auto &waiter : stop_waiters) {
                    waiter->setValue();
                }
            });
        });
    }

    Runtime *runtime = nullptr;
    Mailbox<FsEvent> mailbox;
    MessageSender<FsEvent> sender;
    uv_fs_event_t handle{};
    std::string watched_path;
    unsigned watch_flags = 0;
    bool watched_is_directory = false;
    std::mutex mutex;
    std::deque<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
    std::shared_ptr<State> self_keepalive;
    bool initialized = false;
    bool stop_requested = false;
    bool closing = false;
    bool closed = false;
};

FsEventWatcher::FsEventWatcher(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

FsEventWatcher::~FsEventWatcher() {
    request_stop();
}

Task<FsEventWatcher> FsEventWatcher::watch(std::string path, unsigned flags) {
    auto *runtime = co_await get_current_runtime();
    auto mailbox = co_await Mailbox<FsEvent>::create(*runtime);
    auto state = std::make_shared<State>(runtime, std::move(mailbox), std::move(path), flags);
    auto promise = std::make_shared<async_simple::Promise<void>>();
    auto future = promise->getFuture().via(runtime);

    runtime->post([state, promise]() mutable {
        const int init_rc = uv_fs_event_init(state->runtime->loop(), &state->handle);
        if (init_rc < 0) {
            promise->setException(std::make_exception_ptr(Error("uv_fs_event_init", init_rc)));
            return;
        }

        state->handle.data = state.get();
        const int start_rc = uv_fs_event_start(
            &state->handle,
            [](uv_fs_event_t *handle, const char *filename, int events, int status) {
                auto *state = static_cast<State *>(handle->data);
                auto name = filename == nullptr ? std::string{} : std::string(filename);
                auto event_path = state->watched_is_directory && !name.empty()
                                      ? path::join(state->watched_path, name)
                                      : state->watched_path;
                state->sender.send(FsEvent{std::move(event_path), std::move(name), events, status});
            },
            state->watched_path.c_str(),
            state->watch_flags);

        if (start_rc < 0) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->self_keepalive = state;
                state->closing = true;
            }

            uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
                auto *state = static_cast<State *>(handle->data);
                std::shared_ptr<State> keepalive;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->closed = true;
                    state->closing = false;
                    keepalive = std::move(state->self_keepalive);
                }
            });

            promise->setException(std::make_exception_ptr(Error("uv_fs_event_start", start_rc)));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->initialized = true;
        }
        promise->setValue();
    });

    co_await std::move(future);
    co_return FsEventWatcher(std::move(state));
}

Task<FsEventWatcher> FsEventWatcher::watch(std::string path, FsEventFlags flags) {
    co_return co_await watch(std::move(path), std::to_underlying(flags));
}

bool FsEventWatcher::valid() const noexcept {
    return state_ != nullptr;
}

const std::string &FsEventWatcher::path() const noexcept {
    static const std::string kEmpty;
    return state_ == nullptr ? kEmpty : state_->watched_path;
}

unsigned FsEventWatcher::flags() const noexcept {
    return state_ == nullptr ? 0U : state_->watch_flags;
}

FsEventWatcher::task_type FsEventWatcher::next() const {
    if (!state_) {
        throw std::runtime_error("fs event watcher is empty");
    }
    return state_->mailbox.recv();
}

FsEventWatcher::stream_type FsEventWatcher::events() const {
    if (!state_) {
        co_return;
    }

    for (auto next : state_->mailbox.messages()) {
        co_yield std::move(next);
    }
}

Task<void> FsEventWatcher::stop() {
    if (!state_) {
        co_return;
    }

    auto state = state_;
    auto promise = std::make_shared<async_simple::Promise<void>>();
    auto future = promise->getFuture().via(state->runtime);

    state->runtime->post([state, promise]() mutable {
        bool complete_now = false;
        bool should_close = false;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) {
                complete_now = true;
            } else {
                state->stop_waiters.push_back(promise);
                if (!state->stop_requested) {
                    state->stop_requested = true;
                    state->self_keepalive = state;
                    if (state->initialized && !state->closing) {
                        state->closing = true;
                        should_close = true;
                    }
                }
            }
        }

        state->sender.close();

        if (complete_now) {
            promise->setValue();
            return;
        }

        if (!should_close) {
            return;
        }

        uv_fs_event_stop(&state->handle);
        uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
            auto *state = static_cast<State *>(handle->data);
            std::vector<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
            std::shared_ptr<State> keepalive;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->initialized = false;
                state->closing = false;
                state->closed = true;
                stop_waiters.assign(state->stop_waiters.begin(), state->stop_waiters.end());
                state->stop_waiters.clear();
                keepalive = std::move(state->self_keepalive);
            }

            for (auto &waiter : stop_waiters) {
                waiter->setValue();
            }
        });
    });

    co_await std::move(future);
}

void FsEventWatcher::request_stop() {
    if (state_) {
        state_->request_stop();
    }
}

struct FsPollWatcher::State : std::enable_shared_from_this<FsPollWatcher::State> {
    State(Runtime *runtime_in,
          Mailbox<FsPollEvent> mailbox_in,
          std::string path_in,
          std::chrono::milliseconds interval_in)
        : runtime(runtime_in), mailbox(std::move(mailbox_in)), sender(mailbox.sender()),
          watched_path(std::move(path_in)), poll_interval(interval_in) {}

    void request_stop() {
        auto self = shared_from_this();
        runtime->post([self]() mutable {
            std::vector<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
            bool should_close = false;

            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (self->closed) {
                    stop_waiters.assign(self->stop_waiters.begin(), self->stop_waiters.end());
                    self->stop_waiters.clear();
                } else {
                    if (!self->stop_requested) {
                        self->stop_requested = true;
                        self->self_keepalive = self;
                        if (self->initialized && !self->closing) {
                            self->closing = true;
                            should_close = true;
                        }
                    }
                }
            }

            self->sender.close();

            for (auto &waiter : stop_waiters) {
                waiter->setValue();
            }

            if (!should_close) {
                return;
            }

            uv_fs_poll_stop(&self->handle);
            uv_close(reinterpret_cast<uv_handle_t *>(&self->handle), [](uv_handle_t *handle) {
                auto *state = static_cast<State *>(handle->data);
                std::vector<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
                std::shared_ptr<State> keepalive;

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->initialized = false;
                    state->closing = false;
                    state->closed = true;
                    stop_waiters.assign(state->stop_waiters.begin(), state->stop_waiters.end());
                    state->stop_waiters.clear();
                    keepalive = std::move(state->self_keepalive);
                }

                for (auto &waiter : stop_waiters) {
                    waiter->setValue();
                }
            });
        });
    }

    Runtime *runtime = nullptr;
    Mailbox<FsPollEvent> mailbox;
    MessageSender<FsPollEvent> sender;
    uv_fs_poll_t handle{};
    std::string watched_path;
    std::chrono::milliseconds poll_interval{};
    std::mutex mutex;
    std::deque<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
    std::shared_ptr<State> self_keepalive;
    bool initialized = false;
    bool stop_requested = false;
    bool closing = false;
    bool closed = false;
};

FsPollWatcher::FsPollWatcher(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

FsPollWatcher::~FsPollWatcher() {
    request_stop();
}

Task<FsPollWatcher> FsPollWatcher::watch(std::string path, std::chrono::milliseconds interval) {
    auto *runtime = co_await get_current_runtime();
    auto mailbox = co_await Mailbox<FsPollEvent>::create(*runtime);
    auto state = std::make_shared<State>(runtime, std::move(mailbox), std::move(path), interval);
    auto promise = std::make_shared<async_simple::Promise<void>>();
    auto future = promise->getFuture().via(runtime);

    runtime->post([state, promise]() mutable {
        const int init_rc = uv_fs_poll_init(state->runtime->loop(), &state->handle);
        if (init_rc < 0) {
            promise->setException(std::make_exception_ptr(Error("uv_fs_poll_init", init_rc)));
            return;
        }

        state->handle.data = state.get();
        const int start_rc = uv_fs_poll_start(
            &state->handle,
            [](uv_fs_poll_t *handle, int status, const uv_stat_t *prev, const uv_stat_t *curr) {
                auto *state = static_cast<State *>(handle->data);
                FsPollEvent event;
                event.path = state->watched_path;
                event.status = status;
                if (status >= 0) {
                    event.previous = make_file_info(*prev);
                    event.current = make_file_info(*curr);
                }
                state->sender.send(std::move(event));
            },
            state->watched_path.c_str(),
            static_cast<unsigned int>(std::max<std::int64_t>(state->poll_interval.count(), 1)));

        if (start_rc < 0) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->self_keepalive = state;
                state->closing = true;
            }

            uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
                auto *state = static_cast<State *>(handle->data);
                std::shared_ptr<State> keepalive;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->closed = true;
                    state->closing = false;
                    keepalive = std::move(state->self_keepalive);
                }
            });

            promise->setException(std::make_exception_ptr(Error("uv_fs_poll_start", start_rc)));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->initialized = true;
        }
        promise->setValue();
    });

    co_await std::move(future);
    co_return FsPollWatcher(std::move(state));
}

bool FsPollWatcher::valid() const noexcept {
    return state_ != nullptr;
}

const std::string &FsPollWatcher::path() const noexcept {
    static const std::string kEmpty;
    return state_ == nullptr ? kEmpty : state_->watched_path;
}

std::chrono::milliseconds FsPollWatcher::interval() const noexcept {
    return state_ == nullptr ? std::chrono::milliseconds::zero() : state_->poll_interval;
}

FsPollWatcher::task_type FsPollWatcher::next() const {
    if (!state_) {
        throw std::runtime_error("fs poll watcher is empty");
    }
    return state_->mailbox.recv();
}

FsPollWatcher::stream_type FsPollWatcher::events() const {
    if (!state_) {
        co_return;
    }

    for (auto next : state_->mailbox.messages()) {
        co_yield std::move(next);
    }
}

Task<void> FsPollWatcher::stop() {
    if (!state_) {
        co_return;
    }

    auto state = state_;
    auto promise = std::make_shared<async_simple::Promise<void>>();
    auto future = promise->getFuture().via(state->runtime);

    state->runtime->post([state, promise]() mutable {
        bool complete_now = false;
        bool should_close = false;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) {
                complete_now = true;
            } else {
                state->stop_waiters.push_back(promise);
                if (!state->stop_requested) {
                    state->stop_requested = true;
                    state->self_keepalive = state;
                    if (state->initialized && !state->closing) {
                        state->closing = true;
                        should_close = true;
                    }
                }
            }
        }

        state->sender.close();

        if (complete_now) {
            promise->setValue();
            return;
        }

        if (!should_close) {
            return;
        }

        uv_fs_poll_stop(&state->handle);
        uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
            auto *state = static_cast<State *>(handle->data);
            std::vector<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
            std::shared_ptr<State> keepalive;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->initialized = false;
                state->closing = false;
                state->closed = true;
                stop_waiters.assign(state->stop_waiters.begin(), state->stop_waiters.end());
                state->stop_waiters.clear();
                keepalive = std::move(state->self_keepalive);
            }

            for (auto &waiter : stop_waiters) {
                waiter->setValue();
            }
        });
    });

    co_await std::move(future);
}

void FsPollWatcher::request_stop() {
    if (state_) {
        state_->request_stop();
    }
}

} // namespace async_uv
