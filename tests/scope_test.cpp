#include <cassert>
#include <chrono>
#include <tuple>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<int> delayed_value(int value, std::chrono::milliseconds delay) {
    co_await async_uv::sleep_for(delay);
    co_return value;
}

async_uv::Task<void> run_scope_checks() {
    using namespace std::chrono_literals;

    auto [a, b, c] = co_await async_uv::scope::all(
        delayed_value(1, 10ms), delayed_value(2, 20ms), delayed_value(3, 5ms));
    assert(a == 1);
    assert(b == 2);
    assert(c == 3);

    int side_effect = 0;
    auto task1 = [&]() -> async_uv::Task<void> {
        co_await async_uv::sleep_for(5ms);
        side_effect += 1;
    };
    auto task2 = [&]() -> async_uv::Task<void> {
        co_await async_uv::sleep_for(15ms);
        side_effect += 2;
    };

    co_await async_uv::scope::all(task1(), task2());
    assert(side_effect == 3);
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_scope_checks());
    return 0;
}

