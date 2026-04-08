#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <stdexcept>

#include "flux/flux.h"

namespace {

using namespace std::chrono_literals;

flux::Task<void> run_sleep_example() {
    const auto begin = std::chrono::steady_clock::now();
    co_await flux::sleep_for(30ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    assert(elapsed >= 20ms);
}

flux::Task<void> run_timeout_example() {
    bool timeout_hit = false;
    try {
        (void)co_await flux::with_timeout(20ms, []() -> flux::Task<int> {
            co_await flux::sleep_for(80ms);
            co_return 1;
        }());
        assert(false && "with_timeout should throw ETIMEDOUT");
    } catch (const flux::Error &error) {
        timeout_hit = true;
        assert(error.code() == ETIMEDOUT || error.code() == -ETIMEDOUT);
    }
    assert(timeout_hit);
}

flux::Task<void> run_one_shot_schedule_example() {
    auto timer = co_await flux::SteadyTimer::create();
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

flux::Task<void> run_periodic_schedule_example() {
    auto timer = co_await flux::SteadyTimer::create();
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

flux::Task<void> run_spawn_cancellation_example() {
    auto *runtime = co_await flux::get_current_runtime();
    flux::CancellationSource source;
    auto future = flux::spawn(*runtime, []() -> flux::Task<int> {
        co_await flux::sleep_for(5s);
        co_return 7;
    }(), source);

    co_await flux::sleep_for(20ms);
    assert(source.cancel());

    bool cancelled = false;
    auto receiver = std::move(future)
                  | stdexec::then([](int) {
                        assert(false && "spawn should be cancelled");
                    })
                  | stdexec::upon_error([&cancelled](std::exception_ptr) {
                        cancelled = true;
                    });
    auto result = stdexec::sync_wait(std::move(receiver));
    assert(result.has_value());
    assert(cancelled);
}

flux::Task<void> run_spawn_blocking_cancellation_example() {
    auto *runtime = co_await flux::get_current_runtime();
    flux::CancellationSource source;
    std::atomic<bool> executed{false};

    assert(source.cancel());
    auto future = flux::spawn_blocking(
        *runtime,
        [&executed] {
            executed.store(true, std::memory_order_release);
            return 11;
        },
        source);

    bool cancelled = false;
    auto receiver = std::move(future)
                  | stdexec::then([](int) {
                        assert(false && "spawn_blocking should be cancelled");
                    })
                  | stdexec::upon_error([&cancelled](std::exception_ptr) {
                        cancelled = true;
                    });
    auto result = stdexec::sync_wait(std::move(receiver));
    assert(result.has_value());
    assert(cancelled);
    assert(!executed.load(std::memory_order_acquire));
    co_return;
}

flux::Task<void> run_all() {
    co_await run_sleep_example();
    co_await run_timeout_example();
    co_await run_one_shot_schedule_example();
    co_await run_periodic_schedule_example();
    co_await run_spawn_cancellation_example();
    co_await run_spawn_blocking_cancellation_example();
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_all());
    return 0;
}
