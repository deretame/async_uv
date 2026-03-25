#include <fcntl.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <async_simple/Signal.h>

#include "async_uv/async_uv.h"

namespace {

async_uv::Task<void> run_tcp_server(async_uv::TcpAcceptor acceptor) {
    co_await acceptor.set_simultaneous_accepts(true);
    auto listen_endpoint = co_await acceptor.local_endpoint();
    assert(listen_endpoint.valid());
    assert(listen_endpoint.port() == acceptor.port());

    auto peer = co_await acceptor.accept();
    auto peer_endpoint = co_await peer.remote_endpoint();
    auto local_endpoint = co_await peer.local_endpoint();
    assert(peer_endpoint.valid());
    assert(local_endpoint.valid());
    assert(local_endpoint.port() == acceptor.port());

    auto request = co_await peer.receive_exactly(4);
    auto trailing = co_await peer.receive_all(2);
    assert(trailing.empty());
    assert(peer.eof());
    assert(request == "ping");
    co_await peer.send_all("pong");
    co_await peer.shutdown();
    co_await peer.close();
    co_await acceptor.close();
}

async_uv::Task<std::string> run_tcp_client(int port) {
    using namespace std::chrono_literals;

    auto *runtime = co_await async_uv::get_current_runtime();
    auto *loop = co_await async_uv::get_current_loop();
    assert(runtime != nullptr);
    assert(loop != nullptr);
    assert(async_uv::try_current_runtime() == runtime);
    assert(async_uv::try_current_loop() == loop);

    co_await async_uv::sleep_for(20ms);
    auto client = co_await async_uv::TcpSocket::connect("localhost", port);
    co_await client.set_nodelay(true);
    co_await client.set_keepalive(true, 30);

    auto local_endpoint = co_await client.local_endpoint();
    auto peer_endpoint = co_await client.remote_endpoint();
    assert(local_endpoint.valid());
    assert(local_endpoint.port() != 0);
    assert(peer_endpoint.valid());
    assert(peer_endpoint.port() == port);

    co_await client.send_all("ping");
    co_await client.shutdown();

    std::string reply;
    for (auto next : client.receive_chunks(2)) {
        auto chunk = co_await std::move(next);
        if (!chunk) {
            break;
        }
        reply += *chunk;
    }

    assert(client.eof());
    co_await client.close();

    co_return reply;
}

async_uv::Task<void> run_udp_server(async_uv::UdpSocket socket) {
    auto local_endpoint = co_await socket.local_endpoint();
    assert(local_endpoint.valid());
    assert(local_endpoint.port() != 0);

    auto next = socket.receive_next(64);
    auto datagram = co_await std::move(next);
    assert(datagram.has_value());
    assert(datagram->payload == "ping");
    assert(datagram->remote_endpoint.valid());

    co_await socket.send_to("pong", datagram->remote_endpoint);
    co_await socket.close();
}

async_uv::Task<void> run_delayed_tcp_server(async_uv::TcpAcceptor acceptor) {
    using namespace std::chrono_literals;

    auto peer = co_await acceptor.accept();
    co_await async_uv::sleep_for(80ms);
    co_await peer.send_all("late");
    co_await peer.shutdown();
    co_await peer.close();
    co_await acceptor.close();
}

async_uv::Task<void> run_delayed_udp_sender(async_uv::UdpSocket socket,
                                            async_uv::SocketAddress target) {
    using namespace std::chrono_literals;

    co_await async_uv::sleep_for(80ms);
    co_await socket.send_to("late", target);
    co_await socket.close();
}

async_uv::Task<std::string> run_udp_client(int port) {
    using namespace std::chrono_literals;

    co_await async_uv::sleep_for(20ms);
    auto client =
        co_await async_uv::UdpSocket::connect(async_uv::SocketAddress::ipv4("127.0.0.1", port));
    assert(client.connected());

    auto remote_endpoint = co_await client.remote_endpoint();
    auto local_endpoint = co_await client.local_endpoint();
    assert(remote_endpoint.valid());
    assert(remote_endpoint.port() == port);
    assert(local_endpoint.valid());
    assert(local_endpoint.port() != 0);

    co_await client.set_ttl(1);
    co_await client.send("ping");
    auto reply = co_await client.receive();
    co_await client.close();
    co_return reply;
}

async_uv::Task<std::vector<int>> consume_mailbox(async_uv::Mailbox<int> mailbox);

async_uv::Task<void> fail_after(std::chrono::milliseconds delay, std::string message) {
    co_await async_uv::sleep_for(delay);
    throw std::runtime_error(std::move(message));
}

async_uv::Task<void> smoke_test_body(const std::string &temp_file) {
    using namespace std::chrono_literals;

    co_await async_uv::write_file(temp_file, "smoke-check");
    auto text = co_await async_uv::read_file(temp_file);
    assert(text == "smoke-check");
    assert((co_await async_uv::Fs::read_file_for(temp_file, 50ms)) == "smoke-check");

    const auto unique_suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string fs_root = "async_uv_fs_tree_" + unique_suffix;
    const std::string leaf_dir = async_uv::path::join(fs_root, "nested", "leaf");
    const std::string empty_file = async_uv::path::join(leaf_dir, "empty.txt");
    const std::string data_file = async_uv::path::join(leaf_dir, "data.txt");
    const std::string renamed_file = async_uv::path::join(leaf_dir, "renamed.txt");
    const std::string copied_file = async_uv::path::join(leaf_dir, "copied.txt");
    const std::string moved_file = async_uv::path::join(leaf_dir, "moved.txt");
    const std::string linked_file = async_uv::path::join(leaf_dir, "linked.txt");
    const std::string sendfile_copy = async_uv::path::join(leaf_dir, "sendfile.txt");
    const std::string copied_root = "async_uv_fs_tree_copy_" + unique_suffix;
    const std::string sample_path = async_uv::path::join("alpha", "beta", "gamma.txt");

    assert(sample_path == (std::filesystem::path("alpha") / "beta" / "gamma.txt").string());
    assert(async_uv::path::normalize("alpha/./beta/../gamma.txt") ==
           std::filesystem::path("alpha/./beta/../gamma.txt").lexically_normal().string());
    assert(async_uv::path::filename(sample_path) == "gamma.txt");
    assert(async_uv::path::stem(sample_path) == "gamma");
    assert(async_uv::path::extension(sample_path) == ".txt");
    assert(async_uv::path::parent(sample_path) ==
           (std::filesystem::path("alpha") / "beta").string());
    assert(async_uv::path::relative(sample_path, "alpha") ==
           (std::filesystem::path("beta") / "gamma.txt").string());
    assert(async_uv::path::is_relative(sample_path));
    assert(async_uv::path::is_absolute(async_uv::path::absolute(sample_path)));

    auto ipv4_endpoint = async_uv::SocketAddress::ipv4("127.0.0.1", 8080);
    auto ipv6_endpoint = async_uv::SocketAddress::ipv6("::1", 8080);
    assert(ipv4_endpoint.valid());
    assert(ipv4_endpoint.is_ipv4());
    assert(ipv4_endpoint.to_string() == "127.0.0.1:8080");
    assert(ipv6_endpoint.valid());
    assert(ipv6_endpoint.is_ipv6());
    assert(ipv6_endpoint.to_string() == "[::1]:8080");

    auto localhost_endpoints = co_await async_uv::resolve("localhost", 9000);
    assert(!localhost_endpoints.empty());
    for (const auto &endpoint : localhost_endpoints) {
        assert(endpoint.valid());
        assert(endpoint.port() == 9000);
    }

    async_uv::ResolveOptions udp_resolve_options;
    udp_resolve_options.transport = async_uv::ResolveTransport::udp;
    auto udp_localhost_endpoints =
        co_await async_uv::resolve("localhost", 9001, udp_resolve_options);
    assert(!udp_localhost_endpoints.empty());
    for (const auto &endpoint : udp_localhost_endpoints) {
        assert(endpoint.valid());
        assert(endpoint.port() == 9001);
    }

    auto tcp_service_endpoints = co_await async_uv::resolve_tcp("localhost", std::string("9002"));
    assert(!tcp_service_endpoints.empty());
    auto udp_service_endpoints = co_await async_uv::resolve_udp("localhost", std::string("9003"));
    assert(!udp_service_endpoints.empty());

    async_uv::NameInfoOptions name_info_options;
    name_info_options.numeric_host = true;
    name_info_options.numeric_service = true;
    auto numeric_name = co_await async_uv::lookup_name(
        async_uv::SocketAddress::ipv4("127.0.0.1", 8080), name_info_options);
    assert(numeric_name.host == "127.0.0.1");
    assert(numeric_name.service == "8080");

    (void)co_await async_uv::Fs::remove_all(fs_root);
    (void)co_await async_uv::Fs::remove_all(copied_root);
    co_await async_uv::Fs::create_directories(leaf_dir);
    co_await async_uv::Fs::create_file(empty_file);
    co_await async_uv::Fs::write_file(data_file, "hello");
    co_await async_uv::Fs::append_file(data_file, ", world");

    auto fs_text = co_await async_uv::Fs::read_file(data_file);
    assert(fs_text == "hello, world");
    assert(co_await async_uv::Fs::exists(data_file));
    assert(co_await async_uv::Fs::access(data_file, 0));
    assert(co_await async_uv::Fs::access(data_file, async_uv::AccessFlags::exists));

    auto info = co_await async_uv::Fs::stat(data_file);
    assert(info.is_file());
    assert(info.size == fs_text.size());

    auto entries = co_await async_uv::Fs::list_directory(leaf_dir);
    bool saw_empty = false;
    bool saw_data = false;
    for (const auto &entry : entries) {
        if (entry.name == "empty.txt") {
            saw_empty = true;
        }
        if (entry.name == "data.txt") {
            saw_data = true;
        }
    }
    assert(saw_empty);
    assert(saw_data);

    {
        auto directory = co_await async_uv::Fs::open_directory(leaf_dir);
        std::vector<std::string> streamed_entries;
        for (auto next : directory.entries()) {
            auto entry = co_await std::move(next);
            if (!entry) {
                break;
            }
            streamed_entries.push_back(entry->name);
        }
        assert(streamed_entries.size() == entries.size());
        co_await directory.close();
    }

    co_await async_uv::Fs::rename(data_file, renamed_file);
    assert(!(co_await async_uv::Fs::exists(data_file)));
    assert(co_await async_uv::Fs::exists(renamed_file));

    co_await async_uv::Fs::copy_file(renamed_file, copied_file, async_uv::CopyFlags::none);
    assert((co_await async_uv::Fs::read_file(copied_file)) == "hello, world");

    co_await async_uv::Fs::move(copied_file, moved_file);
    assert(!(co_await async_uv::Fs::exists(copied_file)));
    assert((co_await async_uv::Fs::read_file(moved_file)) == "hello, world");

    co_await async_uv::Fs::link(renamed_file, linked_file);
    assert((co_await async_uv::Fs::read_file(linked_file)) == "hello, world");

    {
        auto source = co_await async_uv::Fs::open(renamed_file, async_uv::OpenFlags::read_only);
        auto destination = co_await async_uv::Fs::open(sendfile_copy,
                                                       async_uv::OpenFlags::write_only |
                                                           async_uv::OpenFlags::create |
                                                           async_uv::OpenFlags::truncate,
                                                       0644);
        const auto source_info = co_await source.stat();
        assert((co_await source.send_to(destination, source_info.size)) == source_info.size);
        co_await destination.close();
        co_await source.close();
    }

    assert((co_await async_uv::Fs::read_file(sendfile_copy)) == "hello, world");

    {
        auto file = co_await async_uv::Fs::open(moved_file, O_RDWR);
        assert((co_await file.read_some_at(0, 5)) == "hello");
        assert((co_await file.write_all_at(7, "async_uv")) == 8);
        co_await file.sync();
        co_await file.datasync();

        auto file_info = co_await file.stat();
        assert(file_info.size == 15);

        co_await file.truncate(5);
        co_await file.close();
    }

    assert((co_await async_uv::Fs::read_file(moved_file)) == "hello");

    auto real_path = std::filesystem::path(co_await async_uv::Fs::real_path(renamed_file));
    assert(!real_path.empty());
    assert(std::filesystem::exists(real_path));

    auto statfs = co_await async_uv::Fs::statfs(fs_root);
    assert(statfs.block_size != 0);

    co_await async_uv::Fs::copy(fs_root, copied_root);
    assert(co_await async_uv::Fs::exists(
        async_uv::path::join(copied_root, "nested", "leaf", "renamed.txt")));
    assert((co_await async_uv::Fs::read_file(
               async_uv::path::join(copied_root, "nested", "leaf", "moved.txt"))) == "hello");

    {
        auto watcher =
            co_await async_uv::FsEventWatcher::watch(leaf_dir, async_uv::FsEventFlags::none);
        bool timed_out = false;
        try {
            (void)co_await watcher.next_for(20ms);
        } catch (const async_uv::Error &error) {
            timed_out = error.code() == UV_ETIMEDOUT;
        }
        assert(timed_out);
        co_await watcher.stop_for(500ms);
    }

    {
        auto watcher =
            co_await async_uv::FsEventWatcher::watch(leaf_dir, async_uv::FsEventFlags::none);

        auto next_event = async_uv::spawn(watcher.next());
        auto producer = async_uv::spawn_blocking([path = moved_file] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));

            auto *file = std::fopen(path.c_str(), "ab");
            if (file == nullptr) {
                return false;
            }

            const char value = '!';
            const auto written = std::fwrite(&value, 1, 1, file);
            std::fclose(file);
            return written == 1;
        });

        assert(co_await std::move(producer));
        co_await async_uv::sleep_for(250ms);
        auto event = co_await std::move(next_event);
        co_await watcher.stop_for(500ms);

        assert(event.has_value());
        assert(event->ok());
        assert(event->path == leaf_dir || event->path == moved_file ||
               event->name == async_uv::path::filename(moved_file));
        assert(event->name.empty() || event->name == async_uv::path::filename(moved_file));
        assert(event->changed() || event->renamed() || event->events != 0);
    }

    {
        auto watcher = co_await async_uv::FsPollWatcher::watch(moved_file, 50ms);
        bool timed_out = false;
        try {
            (void)co_await watcher.next_for(20ms);
        } catch (const async_uv::Error &error) {
            timed_out = error.code() == UV_ETIMEDOUT;
        }
        assert(timed_out);
        co_await watcher.stop_for(500ms);
    }

    {
        auto watcher = co_await async_uv::FsPollWatcher::watch(moved_file, 50ms);
        auto next_event = async_uv::spawn(watcher.next());
        auto producer = async_uv::spawn_blocking([path = moved_file] {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));

            auto *file = std::fopen(path.c_str(), "ab");
            if (file == nullptr) {
                return false;
            }

            const char value = '?';
            const auto written = std::fwrite(&value, 1, 1, file);
            std::fclose(file);
            return written == 1;
        });

        assert(co_await std::move(producer));
        co_await async_uv::sleep_for(300ms);
        auto event = co_await std::move(next_event);
        co_await watcher.stop_for(500ms);

        assert(event.has_value());
        assert(event->ok());
        assert(event->path == moved_file);
        assert(event->previous.has_value());
        assert(event->current.has_value());
        assert(event->current->size >= event->previous->size);
    }

    auto temp_dir = co_await async_uv::Fs::create_temporary_directory("async_uv_tmp_XXXXXX");
    assert(co_await async_uv::Fs::exists(temp_dir));

    auto temporary_file = co_await async_uv::Fs::create_temporary_file("async_uv_temp_XXXXXX");
    assert(temporary_file.file.is_open());
    assert(!temporary_file.path.empty());
    co_await temporary_file.file.write_all("temp-data");
    co_await temporary_file.file.close();
    assert((co_await async_uv::Fs::read_file(temporary_file.path)) == "temp-data");
    co_await async_uv::Fs::remove(temporary_file.path);
    co_await async_uv::Fs::remove(temp_dir);

    auto removed = co_await async_uv::Fs::remove_all(fs_root);
    assert(removed >= 6);
    assert(!(co_await async_uv::Fs::exists(fs_root)));
    assert((co_await async_uv::Fs::remove_all(fs_root)) == 0);
    assert((co_await async_uv::Fs::remove_all(copied_root)) >= 6);
    assert(!(co_await async_uv::Fs::exists(copied_root)));

    const auto started = std::chrono::steady_clock::now();
    auto slow = async_uv::spawn_blocking([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return 42;
    });

    co_await async_uv::sleep_for(10ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    assert(elapsed < 60ms);
    assert((co_await std::move(slow)) == 42);

    {
        auto timer = co_await async_uv::SteadyTimer::create();
        assert((co_await timer.expires_after(120ms)) == 0);
        auto canceled_wait = async_uv::spawn(timer.wait());
        co_await async_uv::sleep_for(20ms);
        assert((co_await timer.cancel()) == 1);
        assert(!(co_await std::move(canceled_wait)));

        const auto timer_started = std::chrono::steady_clock::now();
        assert((co_await timer.expires_at(std::chrono::steady_clock::now() + 30ms)) == 0);
        assert(co_await timer.wait());
        const auto timer_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - timer_started);
        assert(timer_elapsed >= 20ms);

        co_await timer.close();
    }

    {
        async_uv::CancellationSource source;
        auto timer = co_await async_uv::SteadyTimer::create(2s);
        auto waiter = async_uv::spawn(timer.wait(), source);

        co_await async_uv::sleep_for(20ms);
        assert(source.cancel() == async_simple::Terminate);

        bool canceled = false;
        try {
            co_await std::move(waiter);
        } catch (const async_simple::SignalException &ex) {
            canceled = ex.value() == async_simple::Terminate;
        }
        assert(canceled);
        co_await timer.close();
    }

    {
        async_uv::CancellationSource source;
        assert(source.cancel() == async_simple::Terminate);

        auto canceled_read = async_uv::spawn(async_uv::Fs::read_file(temp_file), source);
        bool canceled = false;
        try {
            (void)co_await std::move(canceled_read);
        } catch (const async_simple::SignalException &ex) {
            canceled = ex.value() == async_simple::Terminate;
        }
        assert(canceled);
    }

    {
        bool timed_out = false;
        try {
            co_await async_uv::with_timeout(20ms, async_uv::sleep_for(2s));
        } catch (const async_uv::Error &error) {
            timed_out = error.code() == UV_ETIMEDOUT;
        }
        assert(timed_out);
    }

    {
        const auto deadline = std::chrono::steady_clock::now() + 15ms;
        co_await async_uv::sleep_until(deadline);
    }

    async_uv::TcpBindOptions bind_options;
    bind_options.backlog = 64;
    auto acceptor = co_await async_uv::TcpAcceptor::bind(
        async_uv::SocketAddress::ipv4("127.0.0.1", 0), bind_options);
    const int tcp_port = acceptor.port();
    assert(tcp_port != 0);

    auto tcp_server = async_uv::spawn(run_tcp_server(std::move(acceptor)));
    auto tcp_client = async_uv::spawn(run_tcp_client(tcp_port));

    assert((co_await std::move(tcp_client)) == "pong");
    co_await std::move(tcp_server);

    auto udp_socket =
        co_await async_uv::UdpSocket::bind(async_uv::SocketAddress::ipv4("127.0.0.1", 0));
    const int udp_port = (co_await udp_socket.local_endpoint()).port();
    assert(udp_port != 0);

    auto udp_server = async_uv::spawn(run_udp_server(std::move(udp_socket)));
    auto udp_client = async_uv::spawn(run_udp_client(udp_port));

    assert((co_await std::move(udp_client)) == "pong");
    co_await std::move(udp_server);

    {
        auto acceptor =
            co_await async_uv::TcpAcceptor::bind(async_uv::SocketAddress::ipv4("127.0.0.1", 0));
        const auto port = acceptor.port();
        assert(port != 0);

        auto server = async_uv::spawn(run_delayed_tcp_server(std::move(acceptor)));
        auto client = co_await async_uv::TcpSocket::connect("127.0.0.1", port);

        bool timed_out = false;
        try {
            (void)co_await client.receive_exactly_for(4, 20ms);
        } catch (const async_uv::Error &error) {
            timed_out = error.code() == UV_ETIMEDOUT;
        }

        assert(timed_out);
        assert((co_await client.receive_exactly(4)) == "late");
        co_await client.close();
        co_await std::move(server);
    }

    {
        auto server_socket =
            co_await async_uv::UdpSocket::bind(async_uv::SocketAddress::ipv4("127.0.0.1", 0));
        auto client_socket =
            co_await async_uv::UdpSocket::bind(async_uv::SocketAddress::ipv4("127.0.0.1", 0));
        auto client_endpoint = co_await client_socket.local_endpoint();

        auto server =
            async_uv::spawn(run_delayed_udp_sender(std::move(server_socket), client_endpoint));

        bool timed_out = false;
        try {
            (void)co_await client_socket.receive_from_for(20ms, 64);
        } catch (const async_uv::Error &error) {
            timed_out = error.code() == UV_ETIMEDOUT;
        }

        assert(timed_out);
        const auto datagram = co_await client_socket.receive_from(64);
        assert(datagram.payload == "late");
        co_await client_socket.close();
        co_await std::move(server);
    }

    {
        auto mailbox = co_await async_uv::Mailbox<int>::create();
        auto sender = mailbox.sender();
        sender.send(1);
        sender.send(2);
        sender.close();

        std::vector<int> same_thread_messages;
        for (auto next : mailbox.messages()) {
            auto message = co_await std::move(next);
            if (!message) {
                break;
            }
            same_thread_messages.push_back(*message);
        }

        assert((same_thread_messages == std::vector<int>{1, 2}));
        co_await mailbox.close();
    }

    {
        auto mailbox = co_await async_uv::Mailbox<int>::create();

        bool timed_out = false;
        try {
            (void)co_await mailbox.recv_for(10ms);
        } catch (const async_uv::Error &error) {
            timed_out = error.code() == UV_ETIMEDOUT;
        }
        assert(timed_out);

        auto sender = mailbox.sender();
        sender.send(42);
        sender.close();

        auto message = co_await mailbox.recv_until(std::chrono::steady_clock::now() +
                                                   std::chrono::milliseconds(100));
        assert(message.has_value());
        assert(*message == 42);
        co_await mailbox.close_for(100ms);
    }

    {
        auto mailbox = co_await async_uv::Mailbox<int>::create();
        auto sender = mailbox.sender();
        const auto mailbox_started = std::chrono::steady_clock::now();

        std::thread producer([sender]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            sender.send(7);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sender.send(8);
            sender.close();
        });

        auto consumer = async_uv::spawn(consume_mailbox(std::move(mailbox)));

        co_await async_uv::sleep_for(10ms);
        const auto mailbox_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mailbox_started);
        assert(mailbox_elapsed < 50ms);

        const auto cross_thread_messages = co_await std::move(consumer);
        producer.join();

        assert((cross_thread_messages == std::vector<int>{7, 8}));
    }

    {
        auto signal = async_simple::Signal::create();
        auto sleeper = async_uv::spawn([]() -> async_uv::Task<int> {
            co_await async_uv::sleep_for(std::chrono::seconds(2));
            co_return 1;
        }()
                                                   .setLazyLocal(signal.get()));

        co_await async_uv::sleep_for(20ms);
        const auto emitted = signal->emits(async_simple::Terminate);
        assert(emitted == async_simple::Terminate);

        bool canceled = false;
        try {
            (void)co_await std::move(sleeper);
        } catch (const async_simple::SignalException &ex) {
            canceled = true;
            assert(ex.value() == async_simple::Terminate);
        }
        assert(canceled);
    }
}

async_uv::Task<int> trivial_spawn_value() {
    co_return 7;
}

async_uv::Task<std::vector<int>> consume_mailbox(async_uv::Mailbox<int> mailbox) {
    std::vector<int> values;
    for (auto next : mailbox.messages()) {
        auto message = co_await std::move(next);
        if (!message) {
            break;
        }
        values.push_back(*message);
    }
    co_await mailbox.close();
    co_return values;
}

} // namespace

int main() {
    const std::string temp_file = "async_uv_smoke.txt";

    try {
        async_uv::Runtime runtime;
        auto spawned = async_uv::spawn(runtime, trivial_spawn_value());
        assert(std::move(spawned).get() == 7);
        auto blocking_spawned = runtime.spawn_blocking([] {
            return 11;
        });
        assert(std::move(blocking_spawned).get() == 11);

        runtime.block_on(smoke_test_body(temp_file));

        std::remove(temp_file.c_str());
        std::cout << "async_uv smoke test passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::remove(temp_file.c_str());
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
