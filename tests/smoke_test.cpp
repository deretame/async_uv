#include <cassert>
#include <chrono>
#include <string>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<void> run_fs_smoke() {
    auto dir = co_await async_uv::Fs::create_temporary_directory("async_uv_smoke_XXXXXX");
    const auto file_a = async_uv::path::join(dir, "a.txt");
    const auto file_b = async_uv::path::join(dir, "b.txt");

    co_await async_uv::Fs::write_file(file_a, "hello");
    co_await async_uv::Fs::append_file(file_a, " world");
    assert(co_await async_uv::Fs::exists(file_a));

    const auto text = co_await async_uv::Fs::read_file(file_a);
    assert(text == "hello world");

    co_await async_uv::Fs::copy_file(file_a, file_b, true);
    const auto copied = co_await async_uv::Fs::read_file(file_b);
    assert(copied == text);

    const auto entries = co_await async_uv::Fs::list_directory(dir);
    assert(entries.size() >= 2);

    (void)co_await async_uv::Fs::remove_all(dir);
}

async_uv::Task<void> run_tcp_smoke() {
    auto listener = co_await async_uv::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    assert(port > 0);

    auto server = [&]() -> async_uv::Task<void> {
        auto client = co_await listener.accept();
        const auto req = co_await client.read_exactly(4);
        assert(req == "ping");
        co_await client.write_all("pong");
        co_await client.shutdown();
        co_await client.close();
        co_await listener.close();
    };

    auto client = [port]() -> async_uv::Task<void> {
        auto socket = co_await async_uv::TcpClient::connect("127.0.0.1", port);
        co_await socket.write_all("ping");
        co_await socket.shutdown();
        const auto reply = co_await socket.read_exactly(4);
        assert(reply == "pong");
        co_await socket.close();
    };

    co_await async_uv::scope::all(server(), client());
}

async_uv::Task<void> run_udp_smoke() {
    auto server = co_await async_uv::UdpSocket::bind("127.0.0.1", 0);
    const auto server_ep = co_await server.local_endpoint();
    assert(server_ep.port() > 0);

    auto server_task = [&]() -> async_uv::Task<void> {
        auto packet = co_await server.receive_from();
        assert(packet.payload == "ping");
        co_await server.send_to("pong", packet.remote_endpoint);
        co_await server.close();
    };

    auto client_task = [port = server_ep.port()]() -> async_uv::Task<void> {
        auto client = co_await async_uv::UdpSocket::connect("127.0.0.1", port);
        co_await client.send("ping");
        const auto reply = co_await client.receive();
        assert(reply == "pong");
        co_await client.close();
    };

    co_await async_uv::scope::all(server_task(), client_task());
}

async_uv::Task<void> run_runtime_smoke() {
    using namespace std::chrono_literals;
    co_await async_uv::sleep_for(10ms);

    auto *runtime = co_await async_uv::get_current_runtime();
    assert(runtime != nullptr);

    auto fut = runtime->spawn_blocking([] { return 7; });
    assert(fut.get() == 7);
}

async_uv::Task<void> run_all() {
    co_await run_runtime_smoke();
    co_await run_fs_smoke();
    co_await run_tcp_smoke();
    co_await run_udp_smoke();
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_all());
    return 0;
}

