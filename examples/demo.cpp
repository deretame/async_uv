#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "flux/flux.h"

namespace {

flux::Task<void> run_tcp_server(flux::TcpAcceptor acceptor) {
    co_await acceptor.set_simultaneous_accepts(true);
    auto peer = co_await acceptor.accept();
    auto request = co_await peer.receive_all();
    co_await peer.send_all(std::string("echo: ") + request);
    co_await peer.shutdown();
    co_await peer.close();
    co_await acceptor.close();
}

flux::Task<std::string> run_tcp_client(int port) {
    using namespace std::chrono_literals;

    auto *runtime = co_await flux::get_current_runtime();
    auto *loop = co_await flux::get_current_loop();
    if (runtime == nullptr || loop == nullptr || !runtime->in_io_thread()) {
        throw std::runtime_error("failed to get current flux runtime context");
    }

    co_await flux::sleep_for(20ms);
    auto client = co_await flux::TcpSocket::connect("localhost", port);
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

flux::Task<void> run_udp_server(flux::UdpSocket socket) {
    auto datagram = co_await socket.receive_datagram();
    co_await socket.send_to(std::string("udp: ") + datagram.payload, datagram.remote_endpoint);
    co_await socket.close();
}

flux::Task<std::string> run_udp_client(int port) {
    using namespace std::chrono_literals;

    co_await flux::sleep_for(20ms);
    auto client =
        co_await flux::UdpSocket::connect(flux::SocketAddress::ipv4("127.0.0.1", port));
    co_await client.send("ping");
    auto reply = co_await client.receive();
    co_await client.close();
    co_return reply;
}

flux::Task<int> delayed_value(std::chrono::milliseconds delay, int value) {
    co_await flux::sleep_for(delay);
    co_return value;
}

flux::Task<int> delayed_fail(std::chrono::milliseconds delay, std::string message) {
    co_await flux::sleep_for(delay);
    throw std::runtime_error(std::move(message));
}

flux::Task<void> demo() {
    using namespace std::chrono_literals;

    auto *runtime = co_await flux::get_current_runtime();
    const std::string demo_dir = "demo_fs";
    const std::string demo_path = demo_dir + "/demo.txt";

    co_await flux::Fs::create_directories(demo_dir);
    co_await flux::Fs::write_file(demo_path, "hello from flux\n");
    auto text = co_await flux::Fs::read_file(demo_path);
    auto text_size = co_await flux::spawn_blocking(*runtime, [copy = text] {
        return copy.size();
    });
    auto entries = co_await flux::Fs::list_directory(demo_dir);

    auto localhost = co_await flux::resolve_tcp("localhost", std::string("8080"));
    auto udp_localhost = co_await flux::resolve_udp("localhost", std::string("8081"));

    flux::NameInfoOptions name_info_options;
    name_info_options.numeric_host = true;
    name_info_options.numeric_service = true;
    auto loopback_http = co_await flux::lookup_name(
        flux::SocketAddress::ipv4("127.0.0.1", 8080), name_info_options);

    auto timer = co_await flux::SteadyTimer::create(5ms);
    const auto timer_fired = co_await timer.wait();
    co_await timer.close();
    co_await flux::sleep_until(std::chrono::steady_clock::now() + 5ms);

    flux::TcpBindOptions bind_options;
    bind_options.backlog = 64;
    auto acceptor = co_await flux::TcpAcceptor::bind(
        flux::SocketAddress::ipv4("127.0.0.1", 0), bind_options);
    const int tcp_port = acceptor.port();
    auto tcp_endpoint = co_await acceptor.local_endpoint();

    auto tcp_server = flux::spawn(*runtime, run_tcp_server(std::move(acceptor)));
    auto tcp_client = flux::spawn(*runtime, run_tcp_client(tcp_port));

    auto tcp_reply = co_await std::move(tcp_client);
    co_await std::move(tcp_server);

    auto udp_socket =
        co_await flux::UdpSocket::bind(flux::SocketAddress::ipv4("127.0.0.1", 0));
    auto udp_endpoint = co_await udp_socket.local_endpoint();
    auto udp_server = flux::spawn(*runtime, run_udp_server(std::move(udp_socket)));
    auto udp_client = flux::spawn(*runtime, run_udp_client(udp_endpoint.port()));

    auto udp_reply = co_await std::move(udp_client);
    co_await std::move(udp_server);

    const auto scoped = co_await flux::with_task_scope(
        [](flux::TaskScope &scope) -> flux::Task<std::tuple<int, int, int>> {
            auto [first, second] =
                co_await scope.all(delayed_value(5ms, 1), delayed_value(10ms, 2));
            auto fastest = co_await scope.any_success(
                delayed_fail(5ms, "unreachable"), delayed_value(15ms, 7), delayed_value(25ms, 9));
            co_return std::tuple<int, int, int>{first, second, fastest};
        });

    auto mailbox_trace_events = std::make_shared<std::atomic_int>(0);
    flux::set_trace_hook([mailbox_trace_events](const flux::TraceEvent &event) {
        if (std::string_view(event.category) == "mailbox") {
            mailbox_trace_events->fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto mailbox = co_await flux::Mailbox<std::unique_ptr<std::string>>::create();
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
    flux::reset_trace_hook();

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

    co_await flux::Fs::remove_all(demo_dir);
}

} // namespace

int main() {
    try {
        flux::Runtime runtime(flux::Runtime::build().name("flux_demo"));
        runtime.block_on(demo());
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
