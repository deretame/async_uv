#include "async_uv/fd.h"

#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <async_simple/Promise.h>

#include "async_uv/error.h"

namespace async_uv {

bool FdEvent::ok() const noexcept {
    return status >= 0;
}

bool FdEvent::readable() const noexcept {
    return (events & UV_READABLE) != 0;
}

bool FdEvent::writable() const noexcept {
    return (events & UV_WRITABLE) != 0;
}

bool FdEvent::disconnected() const noexcept {
#ifdef UV_DISCONNECT
    return (events & UV_DISCONNECT) != 0;
#else
    return false;
#endif
}

bool FdEvent::prioritized() const noexcept {
#ifdef UV_PRIORITIZED
    return (events & UV_PRIORITIZED) != 0;
#else
    return false;
#endif
}

struct FdWatcher::State : std::enable_shared_from_this<FdWatcher::State> {
    State(Runtime *runtime_in,
          Mailbox<FdEvent> mailbox_in,
          uv_os_sock_t watched_fd_in,
          int watch_events_in)
        : runtime(runtime_in), mailbox(std::move(mailbox_in)), sender(mailbox.sender()),
          watched_fd(watched_fd_in), watch_events(watch_events_in) {}

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
                } else if (!self->stop_requested) {
                    self->stop_requested = true;
                    self->self_keepalive = self;
                    if (self->initialized && !self->closing) {
                        self->closing = true;
                        should_close = true;
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

            uv_poll_stop(&self->handle);
            emit_trace_event({"fd", "poll_stop", 0, static_cast<std::size_t>(self->watch_events)});
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
    Mailbox<FdEvent> mailbox;
    MessageSender<FdEvent> sender;
    uv_poll_t handle{};
    uv_os_sock_t watched_fd{};
    int watch_events = 0;
    std::mutex mutex;
    std::deque<std::shared_ptr<async_simple::Promise<void>>> stop_waiters;
    std::shared_ptr<State> self_keepalive;
    bool initialized = false;
    bool stop_requested = false;
    bool closing = false;
    bool closed = false;
};

FdWatcher::FdWatcher(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

FdWatcher::~FdWatcher() {
    request_stop();
}

Task<FdWatcher> FdWatcher::watch(uv_os_sock_t fd, int events) {
    if (events == 0) {
        throw std::runtime_error("fd watcher requires at least one event flag");
    }

    auto *runtime = co_await get_current_runtime();
    auto mailbox = co_await Mailbox<FdEvent>::create(*runtime);
    auto state = std::make_shared<State>(runtime, std::move(mailbox), fd, events);
    auto promise = std::make_shared<async_simple::Promise<void>>();
    auto future = promise->getFuture().via(runtime);

    runtime->post([state, promise]() mutable {
        const int init_rc = uv_poll_init(state->runtime->loop(), &state->handle, state->watched_fd);
        if (init_rc < 0) {
            promise->setException(std::make_exception_ptr(Error("uv_poll_init", init_rc)));
            emit_trace_event({"fd", "poll_start_error", init_rc, 0});
            return;
        }

        state->handle.data = state.get();
        const int start_rc = uv_poll_start(
            &state->handle, state->watch_events, [](uv_poll_t *handle, int status, int events) {
                auto *state = static_cast<State *>(handle->data);
                state->sender.send(FdEvent{status, events});
                emit_trace_event({"fd", "poll_event", status, static_cast<std::size_t>(events)});
            });

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

            promise->setException(std::make_exception_ptr(Error("uv_poll_start", start_rc)));
            emit_trace_event({"fd", "poll_start_error", start_rc, 0});
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->initialized = true;
        }

        emit_trace_event({"fd", "poll_start", 0, static_cast<std::size_t>(state->watch_events)});
        promise->setValue();
    });

    co_await std::move(future);
    co_return FdWatcher(std::move(state));
}

Task<FdWatcher> FdWatcher::watch(uv_os_sock_t fd, FdEventFlags flags) {
    co_return co_await watch(fd, std::to_underlying(flags));
}

bool FdWatcher::valid() const noexcept {
    return state_ != nullptr;
}

uv_os_sock_t FdWatcher::fd() const noexcept {
    return state_ == nullptr ? uv_os_sock_t{} : state_->watched_fd;
}

int FdWatcher::watch_events() const noexcept {
    return state_ == nullptr ? 0 : state_->watch_events;
}

FdWatcher::task_type FdWatcher::next() const {
    if (!state_) {
        throw std::runtime_error("fd watcher is empty");
    }
    return state_->mailbox.recv();
}

FdWatcher::stream_type FdWatcher::events_stream() const {
    if (!state_) {
        return {};
    }
    return state_->mailbox.messages();
}

Task<void> FdWatcher::stop() {
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

        uv_poll_stop(&state->handle);
        emit_trace_event({"fd", "poll_stop", 0, static_cast<std::size_t>(state->watch_events)});
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

void FdWatcher::request_stop() {
    if (state_) {
        state_->request_stop();
    }
}

} // namespace async_uv
