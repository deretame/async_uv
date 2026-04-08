#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "flux/flux.h"

namespace {

using namespace std::chrono_literals;

struct CallbackResult {
    std::mutex mutex;
    std::error_code ec;
    bool role_is_io = false;
    std::atomic<bool> done{false};
};

flux::Task<void> wait_callback(const std::shared_ptr<CallbackResult> &result) {
    while (!result->done.load(std::memory_order_acquire)) {
        co_await flux::sleep_for(1ms);
    }
}

flux::Task<void> run_message_bus_smoke_test() {
    auto bus = co_await flux::MessageBus<int>::create();
    auto subscriber = bus.subscribe("smoke");
    auto publisher = bus.io_publisher("smoke");

    assert(subscriber.valid());
    assert(publisher.valid());
    assert(co_await publisher.publish(7));

    auto event = co_await subscriber.next();
    assert(event.has_value());
    assert(*event == 7);

    co_await subscriber.close();
    bus.close();
}

flux::Task<void> run_message_bus_order_test() {
    auto bus = co_await flux::MessageBus<int>::create(
        flux::MessageBus<int>::Options{.topic_capacity = 256, .subscription_capacity = 256});
    auto subscriber = bus.subscribe("ordered");
    auto publisher = bus.io_publisher("ordered");

    for (int i = 0; i < 100; ++i) {
        assert(co_await publisher.publish(i));
    }

    for (int i = 0; i < 100; ++i) {
        auto event = co_await flux::with_timeout(1s, subscriber.next());
        assert(event.has_value());
        assert(*event == i);
    }

    co_await subscriber.close();
    bus.close();
}

flux::Task<void> run_message_bus_isolation_test() {
    auto bus = co_await flux::MessageBus<int>::create(
        flux::MessageBus<int>::Options{.topic_capacity = 128, .subscription_capacity = 128});
    auto a_subscriber = bus.subscribe("alpha");
    auto b_subscriber = bus.subscribe("beta");
    auto a_publisher = bus.io_publisher("alpha");
    auto b_publisher = bus.io_publisher("beta");

    for (int i = 0; i < 50; ++i) {
        assert(co_await a_publisher.publish(i));
        assert(co_await b_publisher.publish(1000 + i));
    }

    for (int i = 0; i < 50; ++i) {
        auto a = co_await flux::with_timeout(1s, a_subscriber.next());
        auto b = co_await flux::with_timeout(1s, b_subscriber.next());
        assert(a.has_value());
        assert(b.has_value());
        assert(*a == i);
        assert(*b == 1000 + i);
    }

    co_await a_subscriber.close();
    co_await b_subscriber.close();
    bus.close();
}

flux::Task<void> run_message_bus_multithread_test() {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    constexpr int kTotal = kThreads * kPerThread;

    auto bus = co_await flux::MessageBus<int>::create(
        flux::MessageBus<int>::Options{.topic_capacity = 512, .subscription_capacity = 512});
    auto subscriber = bus.subscribe("multi");
    auto publisher = bus.thread_publisher("multi");

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        auto worker_publisher = publisher;
        workers.emplace_back([worker_publisher, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int value = t * 100000 + i;
                const bool ok = worker_publisher.sync_publish_for(value, 2s);
                assert(ok);
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }

    std::unordered_set<int> seen;
    seen.reserve(kTotal);
    for (int i = 0; i < kTotal; ++i) {
        auto event = co_await flux::with_timeout(2s, subscriber.next());
        assert(event.has_value());
        seen.insert(*event);
    }
    assert(static_cast<int>(seen.size()) == kTotal);

    co_await subscriber.close();
    bus.close();
}

flux::Task<void> run_message_bus_backpressure_test() {
    auto bus = co_await flux::MessageBus<int>::create(
        flux::MessageBus<int>::Options{.topic_capacity = 1, .subscription_capacity = 1});
    auto subscriber = bus.subscribe("tight");
    auto publisher = bus.thread_publisher("tight");

    bool first_ok = false;
    bool second_ok = false;
    bool third_ok = false;
    bool fourth_ok = false;

    std::thread producer([publisher, &first_ok, &second_ok, &third_ok, &fourth_ok] {
        first_ok = publisher.sync_publish_for(1, 300ms);
        second_ok = publisher.sync_publish_for(2, 300ms);
        third_ok = publisher.sync_publish_for(3, 300ms);
        fourth_ok = publisher.sync_publish_for(4, 50ms);
    });
    producer.join();

    assert(first_ok);
    assert(second_ok);
    assert(third_ok);
    assert(!fourth_ok);

    auto first = co_await flux::with_timeout(1s, subscriber.next());
    assert(first.has_value());
    assert(*first == 1);

    auto second = co_await flux::with_timeout(1s, subscriber.next());
    assert(second.has_value());
    assert(*second == 2);

    auto third = co_await flux::with_timeout(1s, subscriber.next());
    assert(third.has_value());
    assert(*third == 3);

    co_await subscriber.close();
    bus.close();
}

flux::Task<void> run_message_bus_callback_thread_test() {
    auto *runtime = co_await flux::get_current_runtime();
    auto bus = co_await flux::MessageBus<int>::create();
    auto subscriber = bus.subscribe("callback");
    auto publisher = bus.thread_publisher("callback");

    auto first_result = std::make_shared<CallbackResult>();
    std::thread sender1([publisher, result = first_result, runtime] {
        publisher.async_publish(11, [result, runtime](std::error_code ec) {
            {
                std::lock_guard<std::mutex> lock(result->mutex);
                result->ec = ec;
                result->role_is_io =
                    runtime != nullptr && runtime->in_io_thread() && !runtime->in_blocking_thread();
            }
            result->done.store(true, std::memory_order_release);
        });
    });
    sender1.join();

    co_await flux::with_timeout(1s, wait_callback(first_result));
    {
        std::lock_guard<std::mutex> lock(first_result->mutex);
        assert(!first_result->ec);
        assert(first_result->role_is_io);
    }

    auto event = co_await flux::with_timeout(1s, subscriber.next());
    assert(event.has_value());
    assert(*event == 11);

    bus.close_topic("callback");

    auto second_result = std::make_shared<CallbackResult>();
    std::thread sender2([publisher, result = second_result, runtime] {
        publisher.async_publish(99, [result, runtime](std::error_code ec) {
            {
                std::lock_guard<std::mutex> lock(result->mutex);
                result->ec = ec;
                result->role_is_io =
                    runtime != nullptr && runtime->in_io_thread() && !runtime->in_blocking_thread();
            }
            result->done.store(true, std::memory_order_release);
        });
    });
    sender2.join();

    co_await flux::with_timeout(1s, wait_callback(second_result));
    {
        std::lock_guard<std::mutex> lock(second_result->mutex);
        assert(static_cast<bool>(second_result->ec));
        assert(second_result->role_is_io);
    }

    auto closed_event = co_await flux::with_timeout(500ms, subscriber.next());
    assert(!closed_event.has_value());

    co_await subscriber.close();
    bus.close();
}

flux::Task<void> run_message_bus_thread_permission_test() {
    auto bus = co_await flux::MessageBus<int>::create();

    auto thread_publisher = bus.thread_publisher("perm");
    assert(!thread_publisher.sync_publish(1));
    assert(!thread_publisher.sync_publish_for(1, 10ms));

    std::atomic<bool> subscribe_failed{false};
    std::thread subscribe_thread([&] {
        try {
            (void)bus.subscribe("perm");
        } catch (const std::runtime_error &) {
            subscribe_failed.store(true, std::memory_order_release);
        }
    });
    subscribe_thread.join();
    assert(subscribe_failed.load(std::memory_order_acquire));

    bus.close();
}

flux::Task<void> run_message_bus_close_cancel_test() {
    auto bus = co_await flux::MessageBus<int>::create();
    auto subscriber = bus.subscribe("close");
    auto io_publisher = bus.io_publisher("close");
    auto thread_publisher = bus.thread_publisher("close");

    bool timeout_hit = false;
    try {
        (void)co_await flux::with_timeout(20ms, subscriber.next());
    } catch (const flux::Error &error) {
        timeout_hit = true;
        assert(error.code() == ETIMEDOUT || error.code() == -ETIMEDOUT);
    }
    assert(timeout_hit);

    bus.close_topic("close");
    auto closed_event = co_await flux::with_timeout(500ms, subscriber.next());
    assert(!closed_event.has_value());
    assert(!co_await io_publisher.publish(99));
    assert(!thread_publisher.sync_publish(100));
    assert(!thread_publisher.sync_publish_for(101, 10ms));

    auto subscriber2 = bus.subscribe("close2");
    auto io_publisher2 = bus.io_publisher("close2");
    bus.close();
    auto closed_event2 = co_await flux::with_timeout(500ms, subscriber2.next());
    assert(!closed_event2.has_value());
    assert(!co_await io_publisher2.publish(1));

    co_await subscriber.close();
    co_await subscriber2.close();
}

flux::Task<void> run_all() {
    co_await run_message_bus_smoke_test();
    co_await run_message_bus_order_test();
    co_await run_message_bus_isolation_test();
    co_await run_message_bus_multithread_test();
    co_await run_message_bus_backpressure_test();
    co_await run_message_bus_callback_thread_test();
    co_await run_message_bus_thread_permission_test();
    co_await run_message_bus_close_cancel_test();
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().io_threads(4).blocking_threads(2));
    runtime.block_on(run_all());
    return 0;
}
