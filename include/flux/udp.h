#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "flux/cancel.h"
#include "flux/stream.h"
#include "flux/tcp.h"

namespace flux {

struct UdpConnectOptions {
    AddressFamily family = AddressFamily::unspecified;
};

struct UdpBindOptions {
    AddressFamily family = AddressFamily::unspecified;
    bool ipv6_only = false;
    bool reuse_addr = false;
    bool reuse_port = false;
};

struct UdpDatagram {
    std::string payload;
    SocketAddress remote_endpoint;
    unsigned flags = 0;

    bool partial() const noexcept {
        return false;
    }
};

struct UdpReceiveResult {
    std::size_t bytes = 0;
    SocketAddress remote_endpoint;
    unsigned flags = 0;
};

class UdpSocket {
public:
    using ConstBuffer = std::span<const std::byte>;
    using MutableBuffer = std::span<std::byte>;
    struct State;
    using endpoint_type = SocketAddress;
    using message_type = UdpDatagram;
    using next_type = std::optional<message_type>;
    using task_type = Task<next_type>;
    using stream_type = Stream<message_type>;

    UdpSocket() = default;
    explicit UdpSocket(std::shared_ptr<State> state) noexcept;
    ~UdpSocket();

    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;
    UdpSocket(UdpSocket &&other) noexcept;
    UdpSocket &operator=(UdpSocket &&other) noexcept;

    static Task<UdpSocket> bind(std::string host, int port);
    static Task<UdpSocket> bind(std::string host, int port, UdpBindOptions options);
    static Task<UdpSocket> bind(SocketAddress endpoint);
    static Task<UdpSocket> bind(SocketAddress endpoint, UdpBindOptions options);
    static Task<UdpSocket> connect(std::string host, int port);
    static Task<UdpSocket> connect(std::string host, int port, UdpConnectOptions options);
    static Task<UdpSocket> connect(SocketAddress endpoint);

    template <typename Rep, typename Period>
    static Task<UdpSocket>
    connect_for(std::string host, int port, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await flux::with_timeout(timeout, connect(std::move(host), port));
    }

    template <typename Rep, typename Period>
    static Task<UdpSocket> connect_for(std::string host,
                                       int port,
                                       UdpConnectOptions options,
                                       std::chrono::duration<Rep, Period> timeout) {
        co_return co_await flux::with_timeout(timeout, connect(std::move(host), port, options));
    }

    template <typename Rep, typename Period>
    static Task<UdpSocket> connect_for(SocketAddress endpoint,
                                       std::chrono::duration<Rep, Period> timeout) {
        co_return co_await flux::with_timeout(timeout, connect(std::move(endpoint)));
    }

    template <typename Clock, typename Duration>
    static Task<UdpSocket>
    connect_until(std::string host, int port, std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await flux::with_deadline(deadline, connect(std::move(host), port));
    }

    template <typename Clock, typename Duration>
    static Task<UdpSocket> connect_until(std::string host,
                                         int port,
                                         UdpConnectOptions options,
                                         std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await flux::with_deadline(deadline,
                                                   connect(std::move(host), port, options));
    }

    template <typename Clock, typename Duration>
    static Task<UdpSocket> connect_until(SocketAddress endpoint,
                                         std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await flux::with_deadline(deadline, connect(std::move(endpoint)));
    }

    bool valid() const noexcept;
    bool connected() const noexcept;

    // Non-owning buffer APIs (caller keeps buffer alive during await).
    Task<std::size_t> send(ConstBuffer data);
    Task<std::size_t> send_to(ConstBuffer data, SocketAddress endpoint);
    Task<UdpReceiveResult> receive_from(MutableBuffer output);
    Task<std::size_t> receive(MutableBuffer output);

    // Convenience wrappers.
    Task<std::size_t> send(std::string_view data);
    Task<std::size_t> send_all(std::string_view data);
    Task<std::size_t> send_to(std::string_view data, SocketAddress endpoint);
    Task<UdpDatagram> receive_from(std::size_t max_bytes = 64 * 1024);
    Task<std::string> receive(std::size_t max_bytes = 64 * 1024);
    task_type next(std::size_t max_bytes = 64 * 1024);
    stream_type packets(std::size_t max_bytes = 64 * 1024);

    Task<SocketAddress> local_endpoint();
    Task<SocketAddress> remote_endpoint();
    Task<void> set_broadcast(bool enable = true);
    Task<void> set_ttl(int ttl);
    Task<void> set_multicast_loop(bool enable = true);
    Task<void> set_multicast_ttl(int ttl);
    Task<void> join_multicast_group(std::string multicast_address,
                                    std::string interface_address = {});
    Task<void> leave_multicast_group(std::string multicast_address,
                                     std::string interface_address = {});
    Task<void> close();

    template <typename Rep, typename Period>
    Task<std::size_t> send_for(std::string_view data, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await flux::with_timeout(timeout, send(data));
    }

    template <typename Rep, typename Period>
    Task<std::size_t> send_to_for(std::string_view data,
                                  SocketAddress endpoint,
                                  std::chrono::duration<Rep, Period> timeout) {
        co_return co_await flux::with_timeout(timeout, send_to(data, std::move(endpoint)));
    }

    template <typename Rep, typename Period>
    Task<UdpDatagram> receive_from_for(std::chrono::duration<Rep, Period> timeout,
                                       std::size_t max_bytes = 64 * 1024) {
        co_return co_await flux::with_timeout(timeout, receive_from(max_bytes));
    }

    template <typename Rep, typename Period>
    Task<std::string> receive_for(std::chrono::duration<Rep, Period> timeout,
                                  std::size_t max_bytes = 64 * 1024) {
        co_return co_await flux::with_timeout(timeout, receive(max_bytes));
    }

    template <typename Rep, typename Period>
    Task<void> close_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await flux::with_timeout(timeout, close());
    }

    template <typename Clock, typename Duration>
    Task<std::size_t> send_until(std::string_view data,
                                 std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await flux::with_deadline(deadline, send(data));
    }

    template <typename Clock, typename Duration>
    Task<std::size_t> send_to_until(std::string_view data,
                                    SocketAddress endpoint,
                                    std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await flux::with_deadline(deadline, send_to(data, std::move(endpoint)));
    }

    template <typename Clock, typename Duration>
    Task<UdpDatagram> receive_from_until(std::chrono::time_point<Clock, Duration> deadline,
                                         std::size_t max_bytes = 64 * 1024) {
        co_return co_await flux::with_deadline(deadline, receive_from(max_bytes));
    }

    template <typename Clock, typename Duration>
    Task<std::string> receive_until(std::chrono::time_point<Clock, Duration> deadline,
                                    std::size_t max_bytes = 64 * 1024) {
        co_return co_await flux::with_deadline(deadline, receive(max_bytes));
    }

    template <typename Clock, typename Duration>
    Task<void> close_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await flux::with_deadline(deadline, close());
    }

    Task<UdpDatagram> receive_datagram(std::size_t max_bytes = 64 * 1024) {
        return receive_from(max_bytes);
    }
    task_type receive_next(std::size_t max_bytes = 64 * 1024) {
        return next(max_bytes);
    }
    stream_type receive_packets(std::size_t max_bytes = 64 * 1024) {
        return packets(max_bytes);
    }
    Task<SocketAddress> local_address() {
        return local_endpoint();
    }
    Task<SocketAddress> peer_address() {
        return remote_endpoint();
    }

private:
    static task_type next_impl(const std::shared_ptr<State> &state, std::size_t max_bytes);

    std::shared_ptr<State> state_;
};

} // namespace flux
