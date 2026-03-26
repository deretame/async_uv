#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<void> run_tcp_server(async_uv::TcpAcceptor acceptor) {
    co_await acceptor.set_simultaneous_accepts(true);
    auto peer = co_await acceptor.accept();
    auto request = co_await peer.receive_all();
    co_await peer.send_all(std::string("echo: ") + request);
    co_await peer.shutdown();
    co_await peer.close();
    co_await acceptor.close();
}

async_uv::Task<std::string> run_tcp_client(int port) {
    using namespace std::chrono_literals;

    auto *runtime = co_await async_uv::get_current_runtime();
    auto *loop = co_await async_uv::get_current_loop();
    if (runtime == nullptr || loop == nullptr || async_uv::try_current_runtime() != runtime) {
        throw std::runtime_error("failed to get current libuv loop");
    }

    co_await async_uv::sleep_for(20ms);
    auto client = co_await async_uv::TcpSocket::connect("localhost", port);
    co_await client.set_nodelay(true);
    co_await client.send_all("ping");
    co_await client.shutdown();

    std::string reply;
    auto chunks = client.receive_chunks(8);
    while (auto chunk = co_await chunks.next()) {
        reply += *chunk;
    }

    co_await client.close();
    co_return reply;
}

async_uv::Task<void> run_udp_server(async_uv::UdpSocket socket) {
    auto datagram = co_await socket.receive_datagram();
    co_await socket.send_to(std::string("udp: ") + datagram.payload, datagram.remote_endpoint);
    co_await socket.close();
}

async_uv::Task<std::string> run_udp_client(int port) {
    using namespace std::chrono_literals;

    co_await async_uv::sleep_for(20ms);
    auto client =
        co_await async_uv::UdpSocket::connect(async_uv::SocketAddress::ipv4("127.0.0.1", port));
    co_await client.send("ping");
    auto reply = co_await client.receive();
    co_await client.close();
    co_return reply;
}

async_uv::Task<int> delayed_value(std::chrono::milliseconds delay, int value) {
    co_await async_uv::sleep_for(delay);
    co_return value;
}

async_uv::Task<int> delayed_fail(std::chrono::milliseconds delay, std::string message) {
    co_await async_uv::sleep_for(delay);
    throw std::runtime_error(std::move(message));
}

async_uv::Task<void> demo() {
    using namespace std::chrono_literals;

    const std::string demo_dir = "demo_fs";
    const std::string demo_path = demo_dir + "/demo.txt";

    co_await async_uv::Fs::create_directories(demo_dir);
    co_await async_uv::Fs::write_file(demo_path, "hello from async_uv\n");
    auto text = co_await async_uv::Fs::read_file(demo_path);
    auto text_size = co_await async_uv::spawn_blocking([copy = text] {
        return copy.size();
    });
    auto entries = co_await async_uv::Fs::list_directory(demo_dir);

    auto localhost = co_await async_uv::resolve_tcp("localhost", std::string("8080"));
    auto udp_localhost = co_await async_uv::resolve_udp("localhost", std::string("8081"));

    async_uv::NameInfoOptions name_info_options;
    name_info_options.numeric_host = true;
    name_info_options.numeric_service = true;
    auto loopback_http = co_await async_uv::lookup_name(
        async_uv::SocketAddress::ipv4("127.0.0.1", 8080), name_info_options);

    auto timer = co_await async_uv::SteadyTimer::create(5ms);
    const auto timer_fired = co_await timer.wait();
    co_await timer.close();
    co_await async_uv::sleep_until(std::chrono::steady_clock::now() + 5ms);

    async_uv::TcpBindOptions bind_options;
    bind_options.backlog = 64;
    auto acceptor = co_await async_uv::TcpAcceptor::bind(
        async_uv::SocketAddress::ipv4("127.0.0.1", 0), bind_options);
    const int tcp_port = acceptor.port();
    auto tcp_endpoint = co_await acceptor.local_endpoint();

    auto tcp_server = async_uv::spawn(run_tcp_server(std::move(acceptor)));
    auto tcp_client = async_uv::spawn(run_tcp_client(tcp_port));

    auto tcp_reply = co_await std::move(tcp_client);
    co_await std::move(tcp_server);

    auto udp_socket =
        co_await async_uv::UdpSocket::bind(async_uv::SocketAddress::ipv4("127.0.0.1", 0));
    auto udp_endpoint = co_await udp_socket.local_endpoint();
    auto udp_server = async_uv::spawn(run_udp_server(std::move(udp_socket)));
    auto udp_client = async_uv::spawn(run_udp_client(udp_endpoint.port()));

    auto udp_reply = co_await std::move(udp_client);
    co_await std::move(udp_server);

    const auto scoped = co_await async_uv::with_task_scope(
        [](async_uv::TaskScope &scope) -> async_uv::Task<std::tuple<int, int, int>> {
            auto [first, second] =
                co_await scope.all(delayed_value(5ms, 1), delayed_value(10ms, 2));
            auto fastest = co_await scope.any_success(
                delayed_fail(5ms, "unreachable"), delayed_value(15ms, 7), delayed_value(25ms, 9));
            co_return std::tuple<int, int, int>{first, second, fastest};
        });

    auto mailbox_trace_events = std::make_shared<std::atomic_int>(0);
    async_uv::set_trace_hook([mailbox_trace_events](const async_uv::TraceEvent &event) {
        if (std::string_view(event.category) == "mailbox") {
            mailbox_trace_events->fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto mailbox = co_await async_uv::Mailbox<std::unique_ptr<std::string>>::create();
    auto sender = mailbox.sender();
    sender.send(std::make_unique<std::string>("alpha"));
    sender.send(std::make_unique<std::string>("beta"));
    sender.close();

    std::vector<std::string> messages;
    auto message_stream = mailbox.messages();
    while (auto message = co_await message_stream.next()) {
        messages.push_back(std::move(**message));
    }
    co_await mailbox.close();
    async_uv::reset_trace_hook();

    std::cout << "file => " << text;
    std::cout << "size => " << text_size << '\n';
    std::cout << "dir  => " << entries.size() << " entry\n";
    std::cout << "addr => " << tcp_endpoint.to_string() << " (" << localhost.size() << " tcp, "
              << udp_localhost.size() << " udp localhost endpoints)\n";
    std::cout << "dns  => " << loopback_http.host << ":" << loopback_http.service
              << " timer=" << timer_fired << '\n';
    std::cout << "tcp  => " << tcp_reply << '\n';
    std::cout << "udp  => " << udp_reply << " via " << udp_endpoint.to_string() << '\n';
    std::cout << "scope=> all=(" << std::get<0>(scoped) << "," << std::get<1>(scoped)
              << ") any=" << std::get<2>(scoped) << '\n';
    std::cout << "msg  => ";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << messages[i];
    }
    std::cout << '\n';
    std::cout << "trace=> mailbox events=" << mailbox_trace_events->load(std::memory_order_relaxed)
              << '\n';

    co_await async_uv::Fs::remove_all(demo_dir);
}

} // namespace

int main() {
    try {
        async_uv::Runtime runtime(async_uv::Runtime::build().name("async_uv_demo"));
        runtime.block_on(demo());
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
