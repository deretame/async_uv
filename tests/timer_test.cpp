#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <future>
#include <stdexcept>

#include "async_uv/async_uv.h"

namespace {

using namespace std::chrono_literals;

async_uv::Task<void> run_sleep_example() {
    const auto begin = std::chrono::steady_clock::now();
    co_await async_uv::sleep_for(30ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    assert(elapsed >= 20ms);
}

async_uv::Task<void> run_timeout_example() {
    bool timeout_hit = false;
    try {
        (void)co_await async_uv::with_timeout(20ms, []() -> async_uv::Task<int> {
            co_await async_uv::sleep_for(80ms);
            co_return 1;
        }());
        assert(false && "with_timeout should throw ETIMEDOUT");
    } catch (const async_uv::Error &error) {
        timeout_hit = true;
        assert(error.code() == ETIMEDOUT || error.code() == -ETIMEDOUT);
    }
    assert(timeout_hit);
}

async_uv::Task<void> run_one_shot_schedule_example() {
    auto timer = co_await async_uv::SteadyTimer::create();
    const auto begin = std::chrono::steady_clock::now();

    (void)co_await timer.expires_after(25ms);
    const bool fired = co_await timer.wait();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    bool executed = false;
    int value = 0;
    if (fired) {
        executed = true;
        value = 42;
    }

    assert(fired);
    assert(executed);
    assert(value == 42);
    assert(elapsed >= 15ms);

    co_await timer.close();
}

async_uv::Task<void> run_periodic_schedule_example() {
    auto timer = co_await async_uv::SteadyTimer::create();
    const auto begin = std::chrono::steady_clock::now();

    std::size_t ticks = 0;
    for (int i = 0; i < 3; ++i) {
        (void)co_await timer.expires_after(15ms);
        const bool fired = co_await timer.wait();
        assert(fired);
        ++ticks;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    assert(ticks == 3);
    assert(elapsed >= 40ms);

    co_await timer.close();
}

async_uv::Task<void> run_spawn_cancellation_example() {
    async_uv::CancellationSource source;
    auto future = async_uv::spawn([]() -> async_uv::Task<int> {
        co_await async_uv::sleep_for(5s);
        co_return 7;
    }(), source);

    co_await async_uv::sleep_for(20ms);
    assert(source.cancel());
    assert(future.wait_for(500ms) == std::future_status::ready);

    bool cancelled = false;
    try {
        (void)future.get();
    } catch (const std::exception &) {
        cancelled = true;
    }
    assert(cancelled);
}

async_uv::Task<void> run_spawn_blocking_cancellation_example() {
    async_uv::CancellationSource source;
    std::atomic<bool> executed{false};

    assert(source.cancel());
    auto future = async_uv::spawn_blocking(
        [&executed] {
            executed.store(true, std::memory_order_release);
            return 11;
        },
        source);
    assert(future.wait_for(500ms) == std::future_status::ready);

    bool cancelled = false;
    try {
        (void)future.get();
    } catch (const std::exception &) {
        cancelled = true;
    }
    assert(cancelled);
    assert(!executed.load(std::memory_order_acquire));
    co_return;
}

async_uv::Task<void> run_all() {
    co_await run_sleep_example();
    co_await run_timeout_example();
    co_await run_one_shot_schedule_example();
    co_await run_periodic_schedule_example();
    co_await run_spawn_cancellation_example();
    co_await run_spawn_blocking_cancellation_example();
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_all());
    return 0;
}
