#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <async_simple/Promise.h>
#include <async_simple/coro/Generator.h>
#include <async_simple/util/Queue.h>

#include "async_uv/error.h"
#include "async_uv/cancel.h"
#include "async_uv/runtime.h"

namespace async_uv {

template <typename T>
class MessageSender;

template <typename T>
class Mailbox {
public:
    using value_type = T;
    using next_type = std::optional<T>;
    using task_type = Task<next_type>;
    using stream_type = async_simple::coro::Generator<task_type>;

    Mailbox() = default;
    ~Mailbox() {
        request_close();
    }

    Mailbox(const Mailbox &) = delete;
    Mailbox &operator=(const Mailbox &) = delete;
    Mailbox(Mailbox &&) noexcept = default;
    Mailbox &operator=(Mailbox &&) noexcept = default;

    static Task<Mailbox> create() {
        auto *runtime = co_await get_current_runtime();
        co_return co_await create(*runtime);
    }

    static Task<Mailbox> create(Runtime &runtime) {
        auto state = std::make_shared<State>(&runtime);
        auto promise = std::make_shared<async_simple::Promise<void>>();
        auto future = promise->getFuture().via(&runtime);

        runtime.post([state, promise]() mutable {
            const int rc =
                uv_async_init(state->runtime->loop(), &state->async, [](uv_async_t *handle) {
                    static_cast<State *>(handle->data)->drain();
                });

            if (rc < 0) {
                promise->setException(std::make_exception_ptr(Error("uv_async_init", rc)));
                return;
            }

            state->async.data = state.get();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->initialized = true;
            }
            promise->setValue();
        });

        co_await std::move(future);
        co_return Mailbox(std::move(state));
    }

    bool valid() const noexcept {
        return state_ != nullptr;
    }

    MessageSender<T> sender() const {
        return MessageSender<T>(state_);
    }

    task_type recv() const {
        if (!state_) {
            throw std::runtime_error("mailbox is empty");
        }
        co_return co_await recv_impl(state_);
    }

    template <typename Rep, typename Period>
    task_type recv_for(std::chrono::duration<Rep, Period> timeout) const {
        if (!state_) {
            throw std::runtime_error("mailbox is empty");
        }
        co_return co_await async_uv::with_timeout(timeout, recv_impl(state_));
    }

    template <typename Clock, typename Duration>
    task_type recv_until(std::chrono::time_point<Clock, Duration> deadline) const {
        if (!state_) {
            throw std::runtime_error("mailbox is empty");
        }
        co_return co_await async_uv::with_deadline(deadline, recv_impl(state_));
    }

    stream_type messages() const {
        auto state = state_;
        while (state) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if ((state->close_requested || state->closed) && state->messages.empty()) {
                    break;
                }
            }
            co_yield recv_impl(state);
        }
    }

    Task<void> close() {
        if (!state_) {
            co_return;
        }

        auto state = state_;
        auto promise = std::make_shared<async_simple::Promise<void>>();
        auto future = promise->getFuture().via(state->runtime);

        state->runtime->post([state, promise]() mutable {
            bool complete_now = false;
            bool need_notify = false;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->closed) {
                    complete_now = true;
                } else {
                    state->close_waiters.push_back(promise);
                    if (!state->close_requested) {
                        state->close_requested = true;
                        state->self_keepalive = state;
                        need_notify = state->initialized && !state->closing;
                    }
                }
            }

            if (complete_now) {
                promise->setValue();
                return;
            }

            if (need_notify) {
                uv_async_send(&state->async);
            }
        });

        co_await std::move(future);
    }

    template <typename Rep, typename Period>
    Task<void> close_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, close());
    }

    template <typename Clock, typename Duration>
    Task<void> close_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, close());
    }

private:
    struct ReceiveWaiter {
        std::shared_ptr<async_simple::Promise<next_type>> promise;
        std::unique_ptr<async_simple::Slot> slot;
        bool completed = false;
    };

    static std::exception_ptr make_recv_cancel_error() {
        return std::make_exception_ptr(
            async_simple::SignalException(async_simple::Terminate, "mailbox receive canceled"));
    }

    struct State : std::enable_shared_from_this<State> {
        explicit State(Runtime *runtime_in) : runtime(runtime_in) {}

        bool push(T value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!initialized || close_requested || closing || closed) {
                    return false;
                }
                messages.push(next_type{std::move(value)});
            }

            return uv_async_send(&async) == 0;
        }

        void request_close() {
            bool need_notify = false;

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (close_requested || closed) {
                    return;
                }
                close_requested = true;
                self_keepalive = this->shared_from_this();
                need_notify = initialized && !closing;
            }

            if (need_notify) {
                uv_async_send(&async);
            }
        }

        void drain() {
            std::vector<std::pair<std::shared_ptr<ReceiveWaiter>, next_type>> ready_messages;
            std::vector<std::shared_ptr<ReceiveWaiter>> ready_closes;
            bool should_close_handle = false;

            {
                std::lock_guard<std::mutex> lock(mutex);

                while (!waiters.empty()) {
                    next_type message;
                    if (!messages.try_pop(message)) {
                        break;
                    }
                    waiters.front()->completed = true;
                    ready_messages.emplace_back(waiters.front(), std::move(message));
                    waiters.pop_front();
                }

                if (close_requested) {
                    while (!waiters.empty()) {
                        waiters.front()->completed = true;
                        ready_closes.push_back(waiters.front());
                        waiters.pop_front();
                    }

                    if (initialized && !closing && !closed) {
                        closing = true;
                        should_close_handle = true;
                    }
                }
            }

            for (auto &ready : ready_messages) {
                if (ready.first->slot) {
                    (void)ready.first->slot->clear(async_simple::Terminate);
                }
                ready.first->promise->setValue(std::move(ready.second));
            }

            for (auto &waiter : ready_closes) {
                if (waiter->slot) {
                    (void)waiter->slot->clear(async_simple::Terminate);
                }
                waiter->promise->setValue(next_type{});
            }

            if (should_close_handle) {
                uv_close(reinterpret_cast<uv_handle_t *>(&async), [](uv_handle_t *handle) {
                    auto *state = static_cast<State *>(handle->data);
                    std::vector<std::shared_ptr<async_simple::Promise<void>>> close_waiters;
                    std::shared_ptr<State> keepalive;

                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->initialized = false;
                        state->closing = false;
                        state->closed = true;
                        close_waiters.assign(state->close_waiters.begin(),
                                             state->close_waiters.end());
                        state->close_waiters.clear();
                        keepalive = std::move(state->self_keepalive);
                    }

                    for (auto &waiter : close_waiters) {
                        waiter->setValue();
                    }
                });
            }
        }

        Runtime *runtime = nullptr;
        uv_async_t async{};
        std::mutex mutex;
        async_simple::util::Queue<next_type> messages;
        std::deque<std::shared_ptr<ReceiveWaiter>> waiters;
        std::deque<std::shared_ptr<async_simple::Promise<void>>> close_waiters;
        std::shared_ptr<State> self_keepalive;
        bool initialized = false;
        bool close_requested = false;
        bool closing = false;
        bool closed = false;
    };

    explicit Mailbox(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    static task_type recv_impl(const std::shared_ptr<State> &state) {
        auto signal = co_await get_current_signal();
        auto promise = std::make_shared<async_simple::Promise<next_type>>();
        auto future = promise->getFuture().via(state->runtime);

        state->runtime->post([state, signal = std::move(signal), promise]() mutable {
            next_type ready;
            bool canceled = false;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                next_type message;
                if (state->messages.try_pop(message)) {
                    ready = std::move(message);
                } else if (state->close_requested || state->closed) {
                } else {
                    auto waiter = std::make_shared<ReceiveWaiter>();
                    waiter->promise = promise;

                    if (signal != nullptr) {
                        waiter->slot = std::make_unique<async_simple::Slot>(signal.get());
                        if (!async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
                                waiter->slot.get(),
                                [runtime = state->runtime,
                                 weak_state = std::weak_ptr<State>(state),
                                 weak_waiter = std::weak_ptr<ReceiveWaiter>(waiter)](
                                    async_simple::SignalType, async_simple::Signal *) mutable {
                                    auto state = weak_state.lock();
                                    auto waiter = weak_waiter.lock();
                                    if (!state || !waiter) {
                                        return;
                                    }

                                    runtime->post([state = std::move(state),
                                                   waiter = std::move(waiter)]() mutable {
                                        bool removed = false;
                                        {
                                            std::lock_guard<std::mutex> lock(state->mutex);
                                            if (!waiter->completed) {
                                                auto it = std::find(state->waiters.begin(),
                                                                    state->waiters.end(),
                                                                    waiter);
                                                if (it != state->waiters.end()) {
                                                    waiter->completed = true;
                                                    state->waiters.erase(it);
                                                    removed = true;
                                                }
                                            }
                                        }

                                        if (!removed) {
                                            return;
                                        }

                                        if (waiter->slot) {
                                            (void)waiter->slot->clear(async_simple::Terminate);
                                        }
                                        waiter->promise->setException(make_recv_cancel_error());
                                    });
                                })) {
                            canceled = true;
                        }
                    }

                    if (!canceled) {
                        state->waiters.push_back(std::move(waiter));
                        return;
                    }
                }
            }

            if (canceled) {
                promise->setException(make_recv_cancel_error());
                return;
            }

            promise->setValue(std::move(ready));
        });

        co_return co_await std::move(future);
    }

    void request_close() {
        if (state_) {
            state_->request_close();
        }
    }

    std::shared_ptr<State> state_;

    friend class MessageSender<T>;
};

template <typename T>
class MessageSender {
public:
    MessageSender() = default;

    template <typename U>
    bool send(U &&value) const {
        if (!state_) {
            return false;
        }
        return state_->push(T(std::forward<U>(value)));
    }

    void close() const {
        if (state_) {
            state_->request_close();
        }
    }

    bool valid() const noexcept {
        return state_ != nullptr;
    }

private:
    explicit MessageSender(std::shared_ptr<typename Mailbox<T>::State> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<typename Mailbox<T>::State> state_;

    friend class Mailbox<T>;
};

} // namespace async_uv
