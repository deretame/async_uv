#include <cassert>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <variant>

#include <async_simple/Signal.h>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<void> mark_flag_after(std::shared_ptr<std::atomic_bool> flag,
                                     std::chrono::milliseconds delay) {
    co_await async_uv::sleep_for(delay);
    flag->store(true, std::memory_order_release);
}

async_uv::Task<void> mark_flag_on_cancel(std::shared_ptr<std::atomic_bool> flag) {
    using namespace std::chrono_literals;

    try {
        co_await async_uv::sleep_for(2s);
    } catch (const async_simple::SignalException &ex) {
        if (ex.value() == async_simple::Terminate) {
            flag->store(true, std::memory_order_release);
        }
    }
}

async_uv::Task<void> fail_after(std::chrono::milliseconds delay, std::string message) {
    co_await async_uv::sleep_for(delay);
    throw std::runtime_error(std::move(message));
}

async_uv::Task<int> fail_after_int(std::chrono::milliseconds delay, std::string message) {
    co_await async_uv::sleep_for(delay);
    throw std::runtime_error(std::move(message));
}

async_uv::Task<int> return_after(std::chrono::milliseconds delay, int value) {
    co_await async_uv::sleep_for(delay);
    co_return value;
}

async_uv::Task<int> mark_cancel_then_throw(std::shared_ptr<std::atomic_bool> flag) {
    using namespace std::chrono_literals;

    try {
        co_await async_uv::sleep_for(2s);
    } catch (const async_simple::SignalException &ex) {
        if (ex.value() == async_simple::Terminate) {
            flag->store(true, std::memory_order_release);
        }
        throw;
    }

    throw std::runtime_error("mark_cancel_then_throw should have been canceled");
}

async_uv::Task<void> scope_test_body() {
    using namespace std::chrono_literals;

    {
        auto child_finished = std::make_shared<std::atomic_bool>(false);
        const auto scope_started = std::chrono::steady_clock::now();

        co_await async_uv::with_task_scope(
            [child_finished](async_uv::TaskScope &scope) -> async_uv::Task<void> {
                (void)scope.spawn(mark_flag_after(child_finished, 40ms));
                co_return;
            });

        const auto scope_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scope_started);
        assert(child_finished->load(std::memory_order_acquire));
        assert(scope_elapsed >= 30ms);
    }

    {
        const auto value = co_await async_uv::with_task_scope(
            [](async_uv::TaskScope &scope) -> async_uv::Task<int> {
                co_return co_await scope.spawn(return_after(10ms, 99));
            });
        assert(value == 99);
    }

    {
        const auto scope_started = std::chrono::steady_clock::now();
        const auto value = co_await async_uv::with_task_scope(
            [](async_uv::TaskScope &scope) -> async_uv::Task<int> {
                co_return co_await scope.spawn_blocking([] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    return 123;
                });
            });

        const auto scope_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scope_started);
        assert(value == 123);
        assert(scope_elapsed >= 30ms);
    }

    {
        auto [first, second] = co_await async_uv::with_task_scope(
            [](async_uv::TaskScope &scope) -> async_uv::Task<std::tuple<int, int>> {
                co_return co_await scope.all(return_after(10ms, 1), return_after(20ms, 2));
            });
        assert(first == 1);
        assert(second == 2);
    }

    {
        std::vector<async_uv::Task<int>> tasks;
        tasks.push_back(return_after(10ms, 3));
        tasks.push_back(return_after(20ms, 4));

        const auto values = co_await async_uv::with_task_scope(
            [tasks = std::move(tasks)](
                async_uv::TaskScope &scope) mutable -> async_uv::Task<std::vector<int>> {
                co_return co_await scope.all(tasks);
            });
        assert((values == std::vector<int>{3, 4}));
    }

    {
        auto result = co_await async_uv::with_task_scope(
            [](async_uv::TaskScope &scope)
                -> async_uv::Task<async_uv::TaskScope::RaceResult<std::variant<int, int>>> {
                co_return co_await scope.race(return_after(10ms, 7), return_after(40ms, 9));
            });

        assert(result.index == 0);
        assert(std::get<0>(result.value) == 7);
    }

    {
        std::vector<async_uv::Task<int>> tasks;
        tasks.push_back(return_after(30ms, 8));
        tasks.push_back(return_after(10ms, 6));

        auto result = co_await async_uv::with_task_scope(
            [tasks = std::move(tasks)](async_uv::TaskScope &scope) mutable
                -> async_uv::Task<async_uv::TaskScope::RaceResult<int>> {
                co_return co_await scope.race(tasks);
            });

        assert(result.index == 1);
        assert(result.value == 6);
    }

    {
        bool timed_out = false;
        const auto value = co_await async_uv::with_task_scope(
            [&timed_out](async_uv::TaskScope &scope) -> async_uv::Task<int> {
                try {
                    co_return co_await scope.with_timeout(10ms, return_after(40ms, 5));
                } catch (const async_uv::Error &error) {
                    timed_out = error.code() == UV_ETIMEDOUT;
                }

                co_return co_await scope.spawn(return_after(10ms, 6));
            });

        assert(timed_out);
        assert(value == 6);
    }

    {
        auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto value = co_await async_uv::with_task_scope(
            [canceled](async_uv::TaskScope &scope) -> async_uv::Task<int> {
                co_return co_await scope.any_success(fail_after_int(10ms, "early failure"),
                                                     return_after(25ms, 77),
                                                     mark_cancel_then_throw(canceled));
            });

        assert(value == 77);
        assert(canceled->load(std::memory_order_acquire));
    }

    {
        std::vector<async_uv::Task<int>> tasks;
        tasks.push_back(fail_after_int(10ms, "range early failure"));
        tasks.push_back(return_after(25ms, 88));

        const auto value = co_await async_uv::with_task_scope(
            [tasks = std::move(tasks)](async_uv::TaskScope &scope) mutable -> async_uv::Task<int> {
                co_return co_await scope.any_success(tasks);
            });
        assert(value == 88);
    }

    {
        bool first_error = false;
        try {
            (void)co_await async_uv::with_task_scope(
                [](async_uv::TaskScope &scope) -> async_uv::Task<int> {
                    co_return co_await scope.any_success(fail_after_int(10ms, "first failure"),
                                                         fail_after_int(20ms, "second failure"));
                });
        } catch (const std::runtime_error &ex) {
            first_error = std::string(ex.what()) == "first failure";
        }

        assert(first_error);
    }

    {
        auto child_canceled = std::make_shared<std::atomic_bool>(false);
        bool body_failed = false;

        try {
            co_await async_uv::with_task_scope(
                [child_canceled](async_uv::TaskScope &scope) -> async_uv::Task<void> {
                    (void)scope.spawn(mark_flag_on_cancel(child_canceled));

                    co_await async_uv::sleep_for(20ms);
                    throw std::runtime_error("scope body failed");
                });
        } catch (const std::runtime_error &ex) {
            body_failed = std::string(ex.what()) == "scope body failed";
        }

        assert(body_failed);
        assert(child_canceled->load(std::memory_order_acquire));
    }

    {
        auto sibling_canceled = std::make_shared<std::atomic_bool>(false);
        bool child_failed = false;

        try {
            co_await async_uv::with_task_scope(
                [sibling_canceled](async_uv::TaskScope &scope) -> async_uv::Task<void> {
                    (void)scope.spawn(fail_after(20ms, "scope child failed"));
                    (void)scope.spawn(mark_flag_on_cancel(sibling_canceled));
                    co_return;
                });
        } catch (const std::runtime_error &ex) {
            child_failed = std::string(ex.what()) == "scope child failed";
        }

        assert(child_failed);
        assert(sibling_canceled->load(std::memory_order_acquire));
    }

    {
        async_uv::Future<int> blocking_result = async_uv::Future<int>(async_simple::Try<int>(
            std::make_exception_ptr(std::runtime_error("blocking result not assigned"))));
        bool body_failed = false;
        const auto scope_started = std::chrono::steady_clock::now();

        try {
            co_await async_uv::with_task_scope(
                [&blocking_result](async_uv::TaskScope &scope) -> async_uv::Task<void> {
                    blocking_result = scope.spawn_blocking([] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                        return 456;
                    });

                    co_await async_uv::sleep_for(10ms);
                    throw std::runtime_error("scope body failed after blocking");
                });
        } catch (const std::runtime_error &ex) {
            body_failed = std::string(ex.what()) == "scope body failed after blocking";
        }

        const auto scope_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scope_started);
        assert(body_failed);
        assert(scope_elapsed >= 30ms);

        bool canceled = false;
        try {
            (void)co_await std::move(blocking_result);
        } catch (const async_simple::SignalException &ex) {
            canceled = ex.value() == async_simple::Terminate;
        }
        assert(canceled);
    }

    {
        auto destructor_canceled = std::make_shared<std::atomic_bool>(false);

        {
            auto scope = co_await async_uv::TaskScope::create();
            (void)scope.spawn(mark_flag_on_cancel(destructor_canceled));
        }

        co_await async_uv::sleep_for(50ms);
        assert(destructor_canceled->load(std::memory_order_acquire));
    }
}

} // namespace

int main() {
    try {
        async_uv::Runtime runtime;
        runtime.block_on(scope_test_body());
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
