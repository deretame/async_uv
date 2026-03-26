#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <async_simple/Promise.h>
#include <async_simple/util/Queue.h>

#include "async_uv/error.h"
#include "async_uv/cancel.h"
#include "async_uv/runtime.h"
#include "async_uv/stream.h"

namespace async_uv {

template <typename T>
concept MovableToChannel = std::move_constructible<std::decay_t<T>>;

template <typename T>
concept MoveOnlyMessage = MovableToChannel<T> && !std::copy_constructible<std::decay_t<T>>;

struct MailboxOptions {
    enum class OverflowPolicy {
        reject_new,
        drop_oldest,
    };

    // 可选上限；未设置时为无界队列。
    std::optional<std::size_t> max_buffered_messages = std::nullopt;
    // 队列满时的策略。
    OverflowPolicy overflow_policy = OverflowPolicy::reject_new;
};

template <MoveOnlyMessage T>
class MessageSender;

template <MoveOnlyMessage T>
class Mailbox {
public:
    using value_type = T;
    using next_type = std::optional<T>;
    using task_type = Task<next_type>;
    using stream_type = Stream<T>;

    Mailbox() = default;
    ~Mailbox() {
        request_close();
    }

    Mailbox(const Mailbox &) = delete;
    Mailbox &operator=(const Mailbox &) = delete;
    Mailbox(Mailbox &&) noexcept = default;
    Mailbox &operator=(Mailbox &&) noexcept = default;

    static Task<Mailbox> create(MailboxOptions options = {}) {
        auto *runtime = co_await get_current_runtime();
        co_return co_await create(*runtime, options);
    }

    static Task<Mailbox> create(Runtime &runtime, MailboxOptions options = {}) {
        if (options.max_buffered_messages.has_value() && *options.max_buffered_messages == 0) {
            throw std::invalid_argument("mailbox max_buffered_messages must be greater than zero");
        }

        auto state = std::make_shared<State>(&runtime, options);
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

    std::size_t buffered_size() const noexcept {
        if (!state_) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->queued_messages;
    }

    bool is_bounded() const noexcept {
        return max_buffered_messages().has_value();
    }

    std::optional<std::size_t> max_buffered_messages() const noexcept {
        if (!state_) {
            return std::nullopt;
        }
        return state_->max_buffered_messages;
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
        if (!state) {
            return {};
        }

        return stream_type([state = std::move(state)]() -> task_type {
            co_return co_await recv_impl(state);
        });
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
        enum class PushStatus {
            sent,
            closed,
            full,
            dropped_oldest,
            async_error,
        };

        State(Runtime *runtime_in, MailboxOptions options_in)
            : runtime(runtime_in), max_buffered_messages(options_in.max_buffered_messages),
              overflow_policy(options_in.overflow_policy) {}

        PushStatus push_with_status(T &&value) {
            std::size_t queued_after = 0;
            std::size_t queued_snapshot = 0;
            bool closed = false;
            bool full = false;
            bool dropped_oldest = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                queued_snapshot = queued_messages;
                if (!initialized || close_requested || closing || this->closed) {
                    closed = true;
                } else if (max_buffered_messages.has_value() &&
                           queued_messages >= *max_buffered_messages) {
                    if (overflow_policy == MailboxOptions::OverflowPolicy::drop_oldest) {
                        next_type dropped;
                        if (messages.try_pop(dropped)) {
                            if (queued_messages > 0) {
                                --queued_messages;
                            }
                            dropped_oldest = true;
                        } else {
                            full = true;
                        }
                    } else {
                        full = true;
                    }
                }

                if (!closed && !full) {
                    messages.push(next_type{std::move(value)});
                    ++queued_messages;
                    queued_after = queued_messages;
                } else {
                    queued_after = queued_messages;
                }
            }

            if (closed) {
                emit_trace_event({"mailbox", "send_rejected_closed", 0, queued_snapshot});
                return PushStatus::closed;
            }
            if (full) {
                emit_trace_event({"mailbox", "send_rejected_full", 0, queued_snapshot});
                return PushStatus::full;
            }

            if (dropped_oldest) {
                emit_trace_event({"mailbox", "send_drop_oldest", 0, queued_snapshot});
            }

            const int rc = uv_async_send(&async);
            if (rc < 0) {
                emit_trace_event({"mailbox", "send_async_error", rc, queued_after});
                return PushStatus::async_error;
            }

            emit_trace_event({"mailbox", "send", 0, queued_after});
            return dropped_oldest ? PushStatus::dropped_oldest : PushStatus::sent;
        }

        bool push(T &&value) {
            const auto status = push_with_status(std::move(value));
            return status == PushStatus::sent || status == PushStatus::dropped_oldest;
        }

        template <typename Clock, typename Duration>
        bool push_until(T &&value, std::chrono::time_point<Clock, Duration> deadline) {
            if (runtime != nullptr && runtime->currentThreadInExecutor()) {
                return push(std::move(value));
            }

            std::unique_lock<std::mutex> lock(mutex);
            while (true) {
                if (!initialized || close_requested || closing || closed) {
                    const std::size_t queued = queued_messages;
                    lock.unlock();
                    emit_trace_event({"mailbox", "send_rejected_closed", 0, queued});
                    return false;
                }

                if (!max_buffered_messages.has_value() ||
                    queued_messages < *max_buffered_messages) {
                    messages.push(next_type{std::move(value)});
                    ++queued_messages;
                    const std::size_t queued_after = queued_messages;
                    lock.unlock();

                    const int rc = uv_async_send(&async);
                    if (rc < 0) {
                        emit_trace_event({"mailbox", "send_async_error", rc, queued_after});
                        return false;
                    }

                    emit_trace_event({"mailbox", "send", 0, queued_after});
                    return true;
                }

                if (overflow_policy == MailboxOptions::OverflowPolicy::drop_oldest) {
                    next_type dropped;
                    if (messages.try_pop(dropped)) {
                        if (queued_messages > 0) {
                            --queued_messages;
                        }

                        messages.push(next_type{std::move(value)});
                        ++queued_messages;
                        const std::size_t queued_after = queued_messages;
                        lock.unlock();

                        emit_trace_event({"mailbox", "send_drop_oldest", 0, queued_after});

                        const int rc = uv_async_send(&async);
                        if (rc < 0) {
                            emit_trace_event({"mailbox", "send_async_error", rc, queued_after});
                            return false;
                        }

                        emit_trace_event({"mailbox", "send", 0, queued_after});
                        return true;
                    }
                }

                if (space_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                    const std::size_t queued = queued_messages;
                    lock.unlock();
                    emit_trace_event({"mailbox", "send_timeout", 0, queued});
                    return false;
                }
            }
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

            space_cv.notify_all();
        }

        void drain() {
            std::vector<std::pair<std::shared_ptr<ReceiveWaiter>, next_type>> ready_messages;
            std::vector<std::shared_ptr<ReceiveWaiter>> ready_closes;
            bool should_close_handle = false;
            bool freed_space = false;

            {
                std::lock_guard<std::mutex> lock(mutex);

                while (!waiters.empty()) {
                    next_type message;
                    if (!messages.try_pop(message)) {
                        break;
                    }
                    if (queued_messages > 0) {
                        --queued_messages;
                        freed_space = true;
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

            if (freed_space || !ready_closes.empty()) {
                space_cv.notify_all();
            }

            for (auto &ready : ready_messages) {
                ready.first->promise->setValue(std::move(ready.second));
            }

            for (auto &waiter : ready_closes) {
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

                    state->space_cv.notify_all();
                });
            }
        }

        Runtime *runtime = nullptr;
        std::optional<std::size_t> max_buffered_messages;
        MailboxOptions::OverflowPolicy overflow_policy = MailboxOptions::OverflowPolicy::reject_new;
        uv_async_t async{};
        std::mutex mutex;
        std::condition_variable space_cv;
        async_simple::util::Queue<next_type> messages;
        std::deque<std::shared_ptr<ReceiveWaiter>> waiters;
        std::deque<std::shared_ptr<async_simple::Promise<void>>> close_waiters;
        std::shared_ptr<State> self_keepalive;
        bool initialized = false;
        bool close_requested = false;
        bool closing = false;
        bool closed = false;
        std::size_t queued_messages = 0;
    };

    explicit Mailbox(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    static task_type recv_impl(const std::shared_ptr<State> &state) {
        auto signal = co_await get_current_signal();
        auto promise = std::make_shared<async_simple::Promise<next_type>>();
        auto future = promise->getFuture().via(state->runtime);

        state->runtime->post([state, signal = std::move(signal), promise]() mutable {
            next_type ready;
            bool canceled = false;
            bool freed_space = false;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                next_type message;
                if (state->messages.try_pop(message)) {
                    if (state->queued_messages > 0) {
                        --state->queued_messages;
                        freed_space = true;
                    }
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

            if (freed_space) {
                state->space_cv.notify_all();
            }

            promise->setValue(std::move(ready));
        });

        auto result = co_await std::move(future);
        emit_trace_event({"mailbox", result.has_value() ? "recv" : "recv_closed", 0, 0});
        co_return result;
    }

    void request_close() {
        if (state_) {
            state_->request_close();
        }
    }

    std::shared_ptr<State> state_;

    friend class MessageSender<T>;
};

template <MoveOnlyMessage T>
class MessageSender {
public:
    MessageSender() = default;

    bool try_send(T &&value) const {
        if (!state_) {
            return false;
        }
        return state_->push(std::move(value));
    }

    bool try_send(T &value) const {
        return try_send(std::move(value));
    }

    bool try_send(const T &) const = delete;

    bool send(T &&value) const {
        return try_send(std::move(value));
    }

    bool send(T &value) const {
        return send(std::move(value));
    }

    bool send(const T &) const = delete;

    template <typename Rep, typename Period>
    // 异步等待容量（协程接口）。
    Task<bool> send_for(T &&value, std::chrono::duration<Rep, Period> timeout) const {
        co_return co_await send_until(std::move(value), std::chrono::steady_clock::now() + timeout);
    }

    template <typename Rep, typename Period>
    Task<bool> send_for(T &value, std::chrono::duration<Rep, Period> timeout) const {
        co_return co_await send_for(std::move(value), timeout);
    }

    template <typename Clock, typename Duration>
    // 异步等待直到 deadline（协程接口）。
    Task<bool> send_until(T &&value, std::chrono::time_point<Clock, Duration> deadline) const {
        if (!state_) {
            co_return false;
        }

        std::optional<T> pending;
        pending.emplace(std::move(value));

        using PushStatus = typename Mailbox<T>::State::PushStatus;
        auto backoff = std::chrono::milliseconds(1);
        while (true) {
            const auto status = state_->push_with_status(std::move(*pending));
            if (status == PushStatus::sent || status == PushStatus::dropped_oldest) {
                co_return true;
            }
            if (status == PushStatus::closed || status == PushStatus::async_error) {
                co_return false;
            }

            const auto now = Clock::now();
            if (now >= deadline) {
                emit_trace_event({"mailbox", "send_timeout", 0, snapshot_queue_size(state_)});
                co_return false;
            }

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            auto sleep_duration = std::min(backoff, remaining);
            if (sleep_duration <= std::chrono::milliseconds::zero()) {
                sleep_duration = std::chrono::milliseconds(1);
            }
            co_await async_uv::sleep_for(sleep_duration);

            if (backoff < std::chrono::milliseconds(8)) {
                backoff *= 2;
            }
        }
    }

    template <typename Clock, typename Duration>
    Task<bool> send_until(T &value, std::chrono::time_point<Clock, Duration> deadline) const {
        co_return co_await send_until(std::move(value), deadline);
    }

    template <typename Rep, typename Period>
    // 同步阻塞等待容量（建议只在外部线程调用）。
    bool sync_send_for(T &&value, std::chrono::duration<Rep, Period> timeout) const {
        return sync_send_until(std::move(value), std::chrono::steady_clock::now() + timeout);
    }

    template <typename Rep, typename Period>
    bool sync_send_for(T &value, std::chrono::duration<Rep, Period> timeout) const {
        return sync_send_for(std::move(value), timeout);
    }

    template <typename Clock, typename Duration>
    // 同步阻塞等待直到 deadline（建议只在外部线程调用）。
    bool sync_send_until(T &&value, std::chrono::time_point<Clock, Duration> deadline) const {
        if (!state_) {
            return false;
        }
        return state_->push_until(std::move(value), deadline);
    }

    template <typename Clock, typename Duration>
    bool sync_send_until(T &value, std::chrono::time_point<Clock, Duration> deadline) const {
        return sync_send_until(std::move(value), deadline);
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
    static std::size_t
    snapshot_queue_size(const std::shared_ptr<typename Mailbox<T>::State> &state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->queued_messages;
    }

    explicit MessageSender(std::shared_ptr<typename Mailbox<T>::State> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<typename Mailbox<T>::State> state_;

    friend class Mailbox<T>;
};

} // namespace async_uv
