#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <asio/experimental/channel_error.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/bind_executor.hpp>
#include <asio/strand.hpp>

#include "flux/cancel.h"
#include "flux/runtime.h"
#include "flux/stream.h"

namespace flux {

template <typename T>
concept MyConstraint = std::is_trivial_v<T> && std::is_copy_constructible_v<T>;

namespace message_detail {
namespace asio = exec::asio::asio_impl;

inline std::error_code make_aborted_error() {
    return std::make_error_code(std::errc::operation_canceled);
}

} // namespace message_detail

template <MyConstraint T>
class MessageBus {
public:
    using value_type = T;

    struct Options {
        std::size_t topic_capacity = 1024;
        std::size_t subscription_capacity = 1024;
    };

    class IoPublisher;
    class ThreadPublisher;
    class Subscription;

    MessageBus() = default;

    static Task<MessageBus> create(Options options = {}) {
        auto *runtime = co_await get_current_runtime();
        co_return MessageBus(std::make_shared<State>(runtime, std::move(options)));
    }

    static MessageBus create(Runtime &runtime, Options options = {}) {
        return MessageBus(std::make_shared<State>(&runtime, std::move(options)));
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    [[nodiscard]] bool is_open() const noexcept {
        return state_ != nullptr && state_->is_open();
    }

    IoPublisher io_publisher(std::string topic) const {
        return IoPublisher(state_, std::move(topic));
    }

    ThreadPublisher thread_publisher(std::string topic) const {
        Runtime *runtime = state_ ? state_->runtime : nullptr;
        return ThreadPublisher(state_, std::move(topic), runtime);
    }

    Subscription subscribe(std::string topic) const {
        if (!state_) {
            return Subscription{};
        }
        state_->require_io_thread("MessageBus::subscribe");
        return state_->subscribe(std::move(topic));
    }

    void close_topic(std::string_view topic) const {
        if (!state_) {
            return;
        }
        state_->close_topic(topic);
    }

    void close() const {
        if (!state_) {
            return;
        }
        state_->close();
    }

private:
    using Executor = message_detail::asio::any_io_executor;
    using TopicStrand = message_detail::asio::strand<Executor>;
    using IngressChannel =
        message_detail::asio::experimental::concurrent_channel<Executor,
                                                               void(std::error_code, value_type)>;
    using EgressChannel =
        message_detail::asio::experimental::concurrent_channel<Executor,
                                                               void(std::error_code, value_type)>;

    static bool is_channel_terminal_error(const std::error_code &ec) {
        return ec == message_detail::asio::experimental::error::channel_closed ||
               ec == message_detail::asio::experimental::error::channel_cancelled ||
               ec == message_detail::asio::error::operation_aborted;
    }

    static bool is_io_context_for_runtime(const Runtime *runtime) noexcept {
        return runtime != nullptr && runtime->in_io_thread();
    }

    struct SubscriberState;

    struct TopicState : std::enable_shared_from_this<TopicState> {
        Runtime *runtime = nullptr;
        std::string name;
        TopicStrand strand;
        IngressChannel ingress;
        std::size_t subscription_capacity = 0;
        std::atomic<bool> closed{false};
        std::mutex subscribers_mutex;
        std::unordered_map<std::uint64_t, std::weak_ptr<SubscriberState>> subscribers;
        std::uint64_t next_subscriber_id = 1;
        std::mutex dispatcher_mutex;
        bool dispatcher_started = false;

        TopicState(Runtime *rt,
                   std::string topic_name,
                   std::size_t topic_capacity,
                   std::size_t per_sub_capacity)
            : runtime(rt),
              name(std::move(topic_name)),
              strand(message_detail::asio::make_strand(rt->executor())),
              ingress(strand, topic_capacity),
              subscription_capacity(per_sub_capacity) {}

        std::shared_ptr<SubscriberState> add_subscriber() {
            auto self = this->shared_from_this();
            auto subscriber = std::make_shared<SubscriberState>(
                std::move(self), runtime, runtime->executor(), subscription_capacity);
            std::lock_guard<std::mutex> lock(subscribers_mutex);
            const auto id = next_subscriber_id++;
            subscriber->id = id;
            subscribers[id] = subscriber;
            return subscriber;
        }

        void remove_subscriber(std::uint64_t id) {
            std::lock_guard<std::mutex> lock(subscribers_mutex);
            subscribers.erase(id);
        }

        std::vector<std::shared_ptr<SubscriberState>> collect_subscribers() {
            std::vector<std::shared_ptr<SubscriberState>> alive;
            std::lock_guard<std::mutex> lock(subscribers_mutex);
            for (auto it = subscribers.begin(); it != subscribers.end();) {
                if (auto subscriber = it->second.lock()) {
                    alive.push_back(std::move(subscriber));
                    ++it;
                } else {
                    it = subscribers.erase(it);
                }
            }
            return alive;
        }

        void close_subscribers() {
            std::vector<std::shared_ptr<SubscriberState>> alive;
            {
                std::lock_guard<std::mutex> lock(subscribers_mutex);
                for (auto &[_, weak] : subscribers) {
                    if (auto subscriber = weak.lock()) {
                        alive.push_back(std::move(subscriber));
                    }
                }
                subscribers.clear();
            }
            for (auto &subscriber : alive) {
                subscriber->closed.store(true, std::memory_order_release);
                subscriber->egress.close();
            }
        }

        void close_topic() {
            if (closed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            ingress.close();
            close_subscribers();
        }

        bool mark_dispatcher_started() {
            std::lock_guard<std::mutex> lock(dispatcher_mutex);
            if (dispatcher_started) {
                return false;
            }
            dispatcher_started = true;
            return true;
        }

        void start_dispatch_loop() {
            auto self = this->shared_from_this();
            ingress.async_receive(message_detail::asio::bind_executor(
                strand,
                [self](std::error_code ec, value_type payload) mutable {
                    if (MessageBus::is_channel_terminal_error(ec)) {
                        self->close_topic();
                        return;
                    }
                    if (ec) {
                        self->close_topic();
                        return;
                    }

                    auto subscribers =
                        std::make_shared<std::vector<std::shared_ptr<SubscriberState>>>(
                            self->collect_subscribers());
                    self->fanout_next(std::move(payload), std::move(subscribers), 0);
                }));
        }

        void fanout_next(value_type payload,
                         std::shared_ptr<std::vector<std::shared_ptr<SubscriberState>>> subscribers,
                         std::size_t index) {
            while (index < subscribers->size()) {
                auto &subscriber = (*subscribers)[index];
                if (!subscriber ||
                    subscriber->closed.load(std::memory_order_acquire) ||
                    !subscriber->egress.is_open()) {
                    if (subscriber) {
                        subscriber->closed.store(true, std::memory_order_release);
                        remove_subscriber(subscriber->id);
                    }
                    ++index;
                    continue;
                }

                auto self = this->shared_from_this();
                subscriber->egress.async_send(
                    std::error_code{},
                    payload,
                    message_detail::asio::bind_executor(
                        strand,
                        [self,
                         payload = std::move(payload),
                         subscribers = std::move(subscribers),
                         subscriber,
                         index](std::error_code ec) mutable {
                            if (ec) {
                                subscriber->closed.store(true, std::memory_order_release);
                                self->remove_subscriber(subscriber->id);
                            }
                            self->fanout_next(std::move(payload), std::move(subscribers), index + 1);
                        }));
                return;
            }

            start_dispatch_loop();
        }
    };

    struct SubscriberState {
        std::weak_ptr<TopicState> topic;
        Runtime *runtime = nullptr;
        std::uint64_t id = 0;
        EgressChannel egress;
        std::atomic<bool> closed{false};

        SubscriberState(std::shared_ptr<TopicState> parent,
                        Runtime *runtime_in,
                        Executor executor,
                        std::size_t capacity)
            : topic(std::move(parent)),
              runtime(runtime_in),
              egress(std::move(executor), capacity) {}

        void require_io_thread(std::string_view action) const {
            if (MessageBus::is_io_context_for_runtime(runtime)) {
                return;
            }
            throw std::runtime_error(std::string(action) + " requires current thread to be runtime IO thread");
        }

        void close() {
            if (closed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            egress.close();
            if (auto parent = topic.lock()) {
                parent->remove_subscriber(id);
            }
        }
    };

    struct State : std::enable_shared_from_this<State> {
        Runtime *runtime = nullptr;
        Options options{};
        std::atomic<bool> closed{false};
        mutable std::mutex topics_mutex;
        std::unordered_map<std::string, std::shared_ptr<TopicState>> topics;
        std::unordered_set<std::string> closed_topics;

        explicit State(Runtime *rt, Options in_options)
            : runtime(rt),
              options(std::move(in_options)) {
            if (options.topic_capacity == 0) {
                options.topic_capacity = 1;
            }
            if (options.subscription_capacity == 0) {
                options.subscription_capacity = 1;
            }
        }

        [[nodiscard]] bool is_open() const noexcept {
            return !closed.load(std::memory_order_acquire);
        }

        void require_io_thread(std::string_view action) const {
            if (MessageBus::is_io_context_for_runtime(runtime)) {
                return;
            }
            throw std::runtime_error(std::string(action) + " requires current thread to be runtime IO thread");
        }

        Subscription subscribe(std::string topic_name) {
            auto topic = get_or_create_topic(topic_name);
            if (!topic || topic->closed.load(std::memory_order_acquire)) {
                return Subscription{};
            }
            return Subscription(topic->add_subscriber());
        }

        Task<bool> publish_from_io(std::string_view topic_name, value_type value) {
            co_return co_await publish(topic_name, std::move(value));
        }

        bool sync_publish(std::string_view topic_name, value_type value) {
            if (runtime == nullptr || closed.load(std::memory_order_acquire)) {
                return false;
            }
            if (MessageBus::is_io_context_for_runtime(runtime)) {
                emit_trace_event({"message_bus", "sync_publish_on_io_thread", 0, 0});
                return false;
            }

            try {
                return runtime->block_on(publish(topic_name, std::move(value)));
            } catch (...) {
                return false;
            }
        }

        template <typename Rep, typename Period>
        bool sync_publish_for(std::string_view topic_name,
                              value_type value,
                              std::chrono::duration<Rep, Period> timeout) {
            if (runtime == nullptr || closed.load(std::memory_order_acquire)) {
                return false;
            }
            if (MessageBus::is_io_context_for_runtime(runtime)) {
                emit_trace_event({"message_bus", "sync_publish_for_on_io_thread", 0, 0});
                return false;
            }

            try {
                return runtime->block_on(
                    with_timeout(timeout, publish(topic_name, std::move(value))));
            } catch (const Error &error) {
                if (error.code() == ETIMEDOUT || error.code() == -ETIMEDOUT) {
                    return false;
                }
                return false;
            } catch (...) {
                return false;
            }
        }

        void async_publish(std::string_view topic_name,
                           value_type value,
                           std::function<void(std::error_code)> callback) {
            if (runtime == nullptr || closed.load(std::memory_order_acquire)) {
                dispatch_callback_on_io(std::move(callback), message_detail::make_aborted_error());
                return;
            }

            auto topic = get_or_create_topic(topic_name);
            if (!topic || topic->closed.load(std::memory_order_acquire)) {
                dispatch_callback_on_io(std::move(callback), message_detail::make_aborted_error());
                return;
            }

            try {
                topic->ingress.async_send(
                    std::error_code{},
                    value,
                    [runtime = runtime, callback = std::move(callback)](std::error_code ec) mutable {
                        State::dispatch_callback_on_io(runtime, std::move(callback), ec);
                    });
            } catch (const std::system_error &error) {
                dispatch_callback_on_io(std::move(callback), error.code());
            } catch (...) {
                dispatch_callback_on_io(std::move(callback), message_detail::make_aborted_error());
            }
        }

        void close_topic(std::string_view topic_name) {
            std::shared_ptr<TopicState> topic;
            {
                std::lock_guard<std::mutex> lock(topics_mutex);
                const std::string key(topic_name);
                closed_topics.insert(key);
                const auto it = topics.find(key);
                if (it == topics.end()) {
                    return;
                }
                topic = it->second;
                topics.erase(it);
            }
            topic->close_topic();
        }

        void close() {
            if (closed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            std::vector<std::shared_ptr<TopicState>> active_topics;
            {
                std::lock_guard<std::mutex> lock(topics_mutex);
                for (auto &[_, topic] : topics) {
                    active_topics.push_back(topic);
                }
                topics.clear();
                closed_topics.clear();
            }
            for (auto &topic : active_topics) {
                topic->close_topic();
            }
        }

    private:
        static void dispatch_callback_on_io(Runtime *runtime,
                                            std::function<void(std::error_code)> callback,
                                            std::error_code ec) {
            if (!callback) {
                return;
            }
            if (runtime == nullptr) {
                callback(ec);
                return;
            }
            runtime->post([callback = std::move(callback), ec]() mutable {
                callback(ec);
            });
        }

        void dispatch_callback_on_io(std::function<void(std::error_code)> callback,
                                     std::error_code ec) {
            dispatch_callback_on_io(runtime, std::move(callback), ec);
        }

        Task<bool> publish(std::string_view topic_name, value_type value) {
            auto topic = get_or_create_topic(topic_name);
            if (!topic || topic->closed.load(std::memory_order_acquire)) {
                co_return false;
            }

            try {
                co_await topic->ingress.async_send(std::error_code{}, value, exec::asio::use_sender);
                co_return true;
            } catch (const std::system_error &error) {
                if (MessageBus::is_channel_terminal_error(error.code())) {
                    co_return false;
                }
                throw;
            }
        }

        std::shared_ptr<TopicState> get_or_create_topic(std::string_view topic_name) {
            if (closed.load(std::memory_order_acquire)) {
                return nullptr;
            }

            std::shared_ptr<TopicState> topic;
            {
                std::lock_guard<std::mutex> lock(topics_mutex);
                if (closed.load(std::memory_order_acquire)) {
                    return nullptr;
                }

                const std::string key(topic_name);
                if (closed_topics.contains(key)) {
                    return nullptr;
                }
                if (const auto it = topics.find(key); it != topics.end()) {
                    topic = it->second;
                } else {
                    topic = std::make_shared<TopicState>(
                        runtime, key, options.topic_capacity, options.subscription_capacity);
                    topics.emplace(key, topic);
                }
            }

            start_dispatcher_if_needed(topic);
            return topic;
        }

        void start_dispatcher_if_needed(std::shared_ptr<TopicState> topic) {
            if (!topic || !topic->mark_dispatcher_started()) {
                return;
            }
            topic->start_dispatch_loop();
        }
    };

public:
    class IoPublisher {
    public:
        IoPublisher() = default;

        [[nodiscard]] bool valid() const noexcept {
            return !topic_.empty() && !state_.expired();
        }

        Task<bool> publish(value_type value) const {
            auto state = state_.lock();
            if (!state) {
                co_return false;
            }
            state->require_io_thread("MessageBus::IoPublisher::publish");
            co_return co_await state->publish_from_io(topic_, value);
        }

        void close_topic() const {
            auto state = state_.lock();
            if (!state) {
                return;
            }
            state->close_topic(topic_);
        }

    private:
        friend class MessageBus;
        IoPublisher(std::weak_ptr<State> state, std::string topic)
            : state_(std::move(state)),
              topic_(std::move(topic)) {}

        std::weak_ptr<State> state_;
        std::string topic_;
    };

    class ThreadPublisher {
    public:
        using Callback = std::function<void(std::error_code)>;

        ThreadPublisher() = default;

        [[nodiscard]] bool valid() const noexcept {
            return !topic_.empty() && !state_.expired();
        }

        bool sync_publish(value_type value) const {
            auto state = state_.lock();
            if (!state) {
                return false;
            }
            return state->sync_publish(topic_, value);
        }

        template <typename Rep, typename Period>
        bool sync_publish_for(value_type value, std::chrono::duration<Rep, Period> timeout) const {
            auto state = state_.lock();
            if (!state) {
                return false;
            }
            return state->sync_publish_for(topic_, value, timeout);
        }

        void async_publish(value_type value, Callback callback) const {
            auto state = state_.lock();
            if (!state) {
                if (callback) {
                    const auto ec = message_detail::make_aborted_error();
                    if (runtime_ != nullptr) {
                        runtime_->post([callback = std::move(callback), ec]() mutable {
                            callback(ec);
                        });
                    } else {
                        callback(ec);
                    }
                }
                return;
            }
            state->async_publish(topic_, value, std::move(callback));
        }

        void close_topic() const {
            auto state = state_.lock();
            if (!state) {
                return;
            }
            state->close_topic(topic_);
        }

    private:
        friend class MessageBus;
        ThreadPublisher(std::weak_ptr<State> state, std::string topic, Runtime *runtime)
            : state_(std::move(state)),
              topic_(std::move(topic)),
              runtime_(runtime) {}

        std::weak_ptr<State> state_;
        std::string topic_;
        Runtime *runtime_ = nullptr;
    };

    class Subscription {
    public:
        using next_type = std::optional<value_type>;
        using task_type = Task<next_type>;
        using stream_type = Stream<value_type>;

        Subscription() = default;

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        task_type next() const {
            if (!state_ || state_->closed.load(std::memory_order_acquire)) {
                co_return std::nullopt;
            }

            state_->require_io_thread("MessageBus::Subscription::next");
            try {
                auto payload = co_await state_->egress.async_receive(exec::asio::use_sender);
                co_return std::optional<value_type>(payload);
            } catch (const std::system_error &error) {
                if (MessageBus::is_channel_terminal_error(error.code())) {
                    state_->closed.store(true, std::memory_order_release);
                    co_return std::nullopt;
                }
                throw;
            }
        }

        stream_type messages() const {
            auto state = state_;
            return stream_type([state = std::move(state)]() -> task_type {
                if (!state || state->closed.load(std::memory_order_acquire)) {
                    co_return std::nullopt;
                }
                Subscription sub(state);
                co_return co_await sub.next();
            });
        }

        Task<void> close() const {
            if (!state_) {
                co_return;
            }
            state_->require_io_thread("MessageBus::Subscription::close");
            state_->close();
        }

    private:
        friend class MessageBus;
        explicit Subscription(std::shared_ptr<SubscriberState> state)
            : state_(std::move(state)) {}

        std::shared_ptr<SubscriberState> state_;
    };

private:
    explicit MessageBus(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

template <typename T>
class Mailbox {
public:
    struct State;

    class Sender {
    public:
        Sender() = default;

        template <typename U>
            requires std::same_as<std::remove_cvref_t<U>, T>
        bool try_send(U &&value) const {
            if (!state_ || state_->closed.load(std::memory_order_acquire)) {
                return false;
            }
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed.load(std::memory_order_acquire)) {
                return false;
            }
            if (state_->queue.size() >= state_->capacity) {
                return false;
            }
            state_->queue.push_back(std::forward<U>(value));
            return true;
        }

        template <typename Rep, typename Period>
        Task<bool> send_for(T value, std::chrono::duration<Rep, Period> timeout) const {
            if (!state_ || state_->closed.load(std::memory_order_acquire)) {
                co_return false;
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(state_->mutex);
                    if (!state_->closed.load(std::memory_order_acquire) &&
                        state_->queue.size() < state_->capacity) {
                        state_->queue.push_back(std::move(value));
                        co_return true;
                    }
                }
                co_await flux::sleep_for(std::chrono::milliseconds(1));
            }
            co_return false;
        }

    private:
        friend class Mailbox;
        explicit Sender(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    Mailbox() = default;

    static Task<Mailbox> create(Runtime &runtime, std::size_t capacity = 1024) {
        (void)runtime;
        Mailbox mailbox;
        mailbox.state_ = std::make_shared<State>();
        mailbox.state_->capacity = capacity == 0 ? 1 : capacity;
        co_return mailbox;
    }

    [[nodiscard]] Sender sender() const {
        return Sender(state_);
    }

    Task<std::optional<T>> recv() const {
        if (!state_) {
            co_return std::nullopt;
        }

        while (true) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (!state_->queue.empty()) {
                    T value = std::move(state_->queue.front());
                    state_->queue.pop_front();
                    co_return std::optional<T>(std::move(value));
                }
                if (state_->closed.load(std::memory_order_acquire)) {
                    co_return std::nullopt;
                }
            }
            co_await flux::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Task<void> close() const {
        if (state_) {
            state_->closed.store(true, std::memory_order_release);
        }
        co_return;
    }

public:
    struct State {
        mutable std::mutex mutex;
        std::deque<T> queue;
        std::size_t capacity = 1024;
        std::atomic<bool> closed{false};
    };

private:
    std::shared_ptr<State> state_;
};

} // namespace flux
