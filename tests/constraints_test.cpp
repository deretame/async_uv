#include <algorithm>
#include <atomic>
#include <cassert>
#include <string>
#include <type_traits>

#include "flux/flux.h"

namespace {

static_assert(flux::MyConstraint<int>);
static_assert(!flux::MyConstraint<std::string>);

void run_path_checks() {
    const auto joined = flux::path::join("alpha", "beta", "gamma.txt");
    assert(!joined.empty());
    assert(flux::path::filename(joined) == "gamma.txt");
    assert(flux::path::stem(joined) == "gamma");
    assert(flux::path::extension(joined) == ".txt");
    assert(flux::path::parent(joined).find("alpha") != std::string::npos);
    assert(flux::path::is_relative(joined));
}

void run_socket_address_checks() {
    const auto v4 = flux::SocketAddress::ipv4("127.0.0.1", 8080);
    const auto v6 = flux::SocketAddress::ipv6("::1", 8081);

    assert(v4.valid());
    assert(v4.is_ipv4());
    assert(v4.port() == 8080);
    assert(v4.to_string() == "127.0.0.1:8080");

    assert(v6.valid());
    assert(v6.is_ipv6());
    assert(v6.port() == 8081);
    assert(v6.to_string() == "[::1]:8081");
}

void run_runtime_scheduler_api_checks() {
    flux::Runtime runtime(flux::Runtime::build().io_threads(2).blocking_threads(2));

    assert(runtime.find_scheduler("io").has_value());
    assert(!runtime.has_custom_io_scheduler());
    assert(!runtime.has_cpu_scheduler());
    assert(!runtime.has_gpu_scheduler());
    assert(!runtime.find_scheduler("cpu").has_value());
    assert(!runtime.find_scheduler("gpu").has_value());

    auto names = runtime.scheduler_names();
    assert(std::find(names.begin(), names.end(), "io") != names.end());

    runtime.set_io_scheduler(runtime.io_scheduler());
    assert(runtime.has_custom_io_scheduler());
    assert(runtime.custom_io_scheduler().has_value());
    runtime.reset_io_scheduler();
    assert(!runtime.has_custom_io_scheduler());
    assert(!runtime.custom_io_scheduler().has_value());

    runtime.set_cpu_scheduler(runtime.io_scheduler());
    runtime.set_gpu_scheduler(runtime.io_scheduler());
    assert(runtime.has_cpu_scheduler());
    assert(runtime.has_gpu_scheduler());
    assert(runtime.find_scheduler("cpu").has_value());
    assert(runtime.find_scheduler("gpu").has_value());

    runtime.reset_cpu_scheduler();
    runtime.reset_gpu_scheduler();
    assert(!runtime.has_cpu_scheduler());
    assert(!runtime.has_gpu_scheduler());

    runtime.register_scheduler("io_alt", runtime.io_scheduler());
    assert(runtime.find_scheduler("io_alt").has_value());
    runtime.unregister_scheduler("io_alt");
    assert(!runtime.find_scheduler("io_alt").has_value());

    const auto io_thread_ok = runtime.block_on([&runtime]() -> flux::Task<bool> {
        auto *observed = co_await flux::get_current_runtime();
        co_return observed == &runtime && runtime.in_io_thread() && !runtime.in_blocking_thread();
    }());
    assert(io_thread_ok);

    auto blocking_thread_future = runtime.spawn_blocking([&runtime] {
        return runtime.in_blocking_thread() && !runtime.in_io_thread();
    });
    auto blocking_thread_ok = stdexec::sync_wait(std::move(blocking_thread_future));
    assert(blocking_thread_ok.has_value());
    assert(std::get<0>(*blocking_thread_ok));

    std::atomic<std::size_t> checked{0};
    std::atomic<bool> bad_executor_context{false};
    constexpr std::size_t probes = 128;
    for (std::size_t i = 0; i < probes; ++i) {
        runtime.post([&runtime, &checked, &bad_executor_context] {
            if (!runtime.in_io_thread() || runtime.in_blocking_thread()) {
                bad_executor_context.store(true, std::memory_order_release);
            }
            checked.fetch_add(1, std::memory_order_acq_rel);
            checked.notify_one();
        });
    }
    while (checked.load(std::memory_order_acquire) < probes) {
        checked.wait(checked.load(std::memory_order_acquire), std::memory_order_acquire);
    }
    assert(!bad_executor_context.load(std::memory_order_acquire));
}

} // namespace

int main() {
    run_path_checks();
    run_socket_address_checks();
    run_runtime_scheduler_api_checks();
    return 0;
}
