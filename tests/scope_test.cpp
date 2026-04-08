#include <cassert>
#include <chrono>
#include <tuple>

#include "flux/flux.h"

namespace {

flux::Task<int> delayed_value(int value, std::chrono::milliseconds delay) {
    co_await flux::sleep_for(delay);
    co_return value;
}

flux::Task<void> run_scope_checks() {
    using namespace std::chrono_literals;

    auto [a, b, c] = co_await flux::scope::all(
        delayed_value(1, 10ms), delayed_value(2, 20ms), delayed_value(3, 5ms));
    assert(a == 1);
    assert(b == 2);
    assert(c == 3);

    int side_effect = 0;
    auto task1 = [&]() -> flux::Task<void> {
        co_await flux::sleep_for(5ms);
        side_effect += 1;
    };
    auto task2 = [&]() -> flux::Task<void> {
        co_await flux::sleep_for(15ms);
        side_effect += 2;
    };

    co_await flux::scope::all(task1(), task2());
    assert(side_effect == 3);
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_scope_checks());
    return 0;
}

