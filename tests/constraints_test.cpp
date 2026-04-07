#include <algorithm>
#include <cassert>
#include <string>
#include <type_traits>

#include "async_uv/async_uv.h"

namespace {

static_assert(async_uv::MyConstraint<int>);
static_assert(!async_uv::MyConstraint<std::string>);

void run_path_checks() {
    const auto joined = async_uv::path::join("alpha", "beta", "gamma.txt");
    assert(!joined.empty());
    assert(async_uv::path::filename(joined) == "gamma.txt");
    assert(async_uv::path::stem(joined) == "gamma");
    assert(async_uv::path::extension(joined) == ".txt");
    assert(async_uv::path::parent(joined).find("alpha") != std::string::npos);
    assert(async_uv::path::is_relative(joined));
}

void run_socket_address_checks() {
    const auto v4 = async_uv::SocketAddress::ipv4("127.0.0.1", 8080);
    const auto v6 = async_uv::SocketAddress::ipv6("::1", 8081);

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
    assert(async_uv::Runtime::current() == nullptr);
    assert(async_uv::try_current_runtime() == nullptr);
    assert(async_uv::Runtime::current_thread_role() == async_uv::Runtime::ThreadRole::external);

    async_uv::Runtime runtime(async_uv::Runtime::build().io_threads(2).blocking_threads(2));

    assert(async_uv::Runtime::current() == nullptr);
    assert(async_uv::try_current_runtime() == nullptr);
    assert(async_uv::Runtime::current_thread_role() == async_uv::Runtime::ThreadRole::external);

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

    const auto io_role = runtime.block_on(
        []() -> async_uv::Task<async_uv::Runtime::ThreadRole> {
            co_return async_uv::Runtime::current_thread_role();
        }());
    assert(io_role == async_uv::Runtime::ThreadRole::io);

    auto blocking_role_future = runtime.spawn_blocking([] {
        return async_uv::Runtime::current_thread_role();
    });
    assert(blocking_role_future.get() == async_uv::Runtime::ThreadRole::blocking);
}

} // namespace

int main() {
    run_path_checks();
    run_socket_address_checks();
    run_runtime_scheduler_api_checks();
    return 0;
}
