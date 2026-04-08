#include <cassert>
#include <chrono>
#include <string>

#include "flux/flux.h"

namespace {

flux::Task<void> run_fs_smoke() {
    auto dir = co_await flux::Fs::create_temporary_directory("flux_smoke_XXXXXX");
    const auto file_a = flux::path::join(dir, "a.txt");
    const auto file_b = flux::path::join(dir, "b.txt");

    co_await flux::Fs::write_file(file_a, "hello");
    co_await flux::Fs::append_file(file_a, " world");
    assert(co_await flux::Fs::exists(file_a));

    const auto text = co_await flux::Fs::read_file(file_a);
    assert(text == "hello world");

    co_await flux::Fs::copy_file(file_a, file_b, true);
    const auto copied = co_await flux::Fs::read_file(file_b);
    assert(copied == text);

    const auto entries = co_await flux::Fs::list_directory(dir);
    assert(entries.size() >= 2);

    (void)co_await flux::Fs::remove_all(dir);
}

flux::Task<void> run_tcp_smoke() {
    auto listener = co_await flux::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    assert(port > 0);

    auto server = [&]() -> flux::Task<void> {
        auto client = co_await listener.accept();
        const auto req = co_await client.read_exactly(4);
        assert(req == "ping");
        co_await client.write_all("pong");
        co_await client.shutdown();
        co_await client.close();
        co_await listener.close();
    };

    auto client = [port]() -> flux::Task<void> {
        auto socket = co_await flux::TcpClient::connect("127.0.0.1", port);
        co_await socket.write_all("ping");
        co_await socket.shutdown();
        const auto reply = co_await socket.read_exactly(4);
        assert(reply == "pong");
        co_await socket.close();
    };

    co_await flux::scope::all(server(), client());
}

flux::Task<void> run_udp_smoke() {
    auto server = co_await flux::UdpSocket::bind("127.0.0.1", 0);
    const auto server_ep = co_await server.local_endpoint();
    assert(server_ep.port() > 0);

    auto server_task = [&]() -> flux::Task<void> {
        auto packet = co_await server.receive_from();
        assert(packet.payload == "ping");
        co_await server.send_to("pong", packet.remote_endpoint);
        co_await server.close();
    };

    auto client_task = [port = server_ep.port()]() -> flux::Task<void> {
        auto client = co_await flux::UdpSocket::connect("127.0.0.1", port);
        co_await client.send("ping");
        const auto reply = co_await client.receive();
        assert(reply == "pong");
        co_await client.close();
    };

    co_await flux::scope::all(server_task(), client_task());
}

flux::Task<void> run_runtime_smoke() {
    using namespace std::chrono_literals;
    co_await flux::sleep_for(10ms);

    auto *runtime = co_await flux::get_current_runtime();
    assert(runtime != nullptr);

    auto fut = runtime->spawn_blocking([] { return 7; });
    auto result = stdexec::sync_wait(std::move(fut));
    assert(result.has_value());
    assert(std::get<0>(*result) == 7);
}

flux::Task<void> run_all() {
    co_await run_runtime_smoke();
    co_await run_fs_smoke();
    co_await run_tcp_smoke();
    co_await run_udp_smoke();
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().io_threads(2).blocking_threads(2));
    runtime.block_on(run_all());
    return 0;
}
