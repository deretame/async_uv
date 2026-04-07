#include "async_uv/udp.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

namespace async_uv {

namespace {

namespace asio = exec::asio::asio_impl;

SocketAddress from_udp_endpoint(const asio::ip::udp::endpoint &endpoint) {
    auto *addr = reinterpret_cast<const sockaddr *>(endpoint.data());
    return SocketAddress::from_native(addr, static_cast<int>(endpoint.size()));
}

asio::ip::address to_asio_address(const SocketAddress &address) {
    return asio::ip::make_address(address.ip());
}

asio::ip::udp::endpoint to_udp_endpoint(const SocketAddress &address) {
    if (!address.valid()) {
        throw std::runtime_error("invalid socket address");
    }
    return asio::ip::udp::endpoint(to_asio_address(address), static_cast<unsigned short>(address.port()));
}

} // namespace

struct UdpSocket::State {
    Runtime *runtime = nullptr;
    asio::ip::udp::socket socket;
    bool connected = false;
    std::optional<asio::ip::udp::endpoint> remote;

    explicit State(Runtime *rt) : runtime(rt), socket(rt->executor()) {}
};

UdpSocket::UdpSocket(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

UdpSocket::~UdpSocket() = default;

UdpSocket::UdpSocket(UdpSocket &&other) noexcept : state_(std::move(other.state_)) {}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<UdpSocket> UdpSocket::bind(std::string host, int port) {
    co_return co_await bind(std::move(host), port, {});
}

Task<UdpSocket> UdpSocket::bind(std::string host, int port, UdpBindOptions options) {
    ResolveOptions resolve_options;
    resolve_options.family = options.family;
    resolve_options.passive = true;
    resolve_options.transport = ResolveTransport::udp;

    auto endpoints = co_await resolve(std::move(host), port, resolve_options);
    std::exception_ptr last_error;
    for (const auto &endpoint : endpoints) {
        try {
            co_return co_await bind(endpoint, options);
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }
    throw std::runtime_error("unable to bind UDP socket");
}

Task<UdpSocket> UdpSocket::bind(SocketAddress endpoint) {
    co_return co_await bind(std::move(endpoint), {});
}

Task<UdpSocket> UdpSocket::bind(SocketAddress endpoint, UdpBindOptions options) {
    auto *runtime = co_await get_current_runtime();
    auto state = std::make_shared<State>(runtime);
    auto ep = to_udp_endpoint(endpoint);

    state->socket.open(ep.protocol());
    if (options.reuse_addr) {
        state->socket.set_option(asio::socket_base::reuse_address(true));
    }
    if (options.ipv6_only && ep.address().is_v6()) {
        state->socket.set_option(asio::ip::v6_only(true));
    }
    if (options.reuse_port) {
#ifdef SO_REUSEPORT
        int enabled = 1;
        if (::setsockopt(state->socket.native_handle(),
                         SOL_SOCKET,
                         SO_REUSEPORT,
                         &enabled,
                         static_cast<socklen_t>(sizeof(enabled))) != 0) {
            throw std::system_error(errno, std::generic_category(), "setsockopt(SO_REUSEPORT)");
        }
#endif
    }
    state->socket.bind(ep);

    co_return UdpSocket(std::move(state));
}

Task<UdpSocket> UdpSocket::connect(std::string host, int port) {
    co_return co_await connect(std::move(host), port, {});
}

Task<UdpSocket> UdpSocket::connect(std::string host, int port, UdpConnectOptions options) {
    auto endpoints = co_await resolve_udp(std::move(host), port, options.family);
    std::exception_ptr last_error;
    for (const auto &endpoint : endpoints) {
        try {
            co_return co_await connect(endpoint);
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }
    throw std::runtime_error("no UDP endpoint resolved");
}

Task<UdpSocket> UdpSocket::connect(SocketAddress endpoint) {
    auto *runtime = co_await get_current_runtime();
    auto state = std::make_shared<State>(runtime);
    const auto remote = to_udp_endpoint(endpoint);

    state->socket.open(remote.protocol());
    co_await state->socket.async_connect(remote, exec::asio::use_sender);
    state->connected = true;
    state->remote = remote;

    co_return UdpSocket(std::move(state));
}

bool UdpSocket::valid() const noexcept {
    return state_ != nullptr && state_->socket.is_open();
}

bool UdpSocket::connected() const noexcept {
    return state_ != nullptr && state_->connected;
}

Task<std::size_t> UdpSocket::send(std::string_view data) {
    if (!valid() || !connected()) {
        throw std::runtime_error("UdpSocket::send requires a connected socket");
    }
    co_return co_await state_->socket.async_send(
        asio::buffer(data.data(), data.size()), exec::asio::use_sender);
}

Task<std::size_t> UdpSocket::send_all(std::string_view data) {
    co_return co_await send(data);
}

Task<std::size_t> UdpSocket::send_to(std::string_view data, SocketAddress endpoint) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    auto remote = to_udp_endpoint(endpoint);
    co_return co_await state_->socket.async_send_to(
        asio::buffer(data.data(), data.size()), remote, exec::asio::use_sender);
}

Task<UdpDatagram> UdpSocket::receive_from(std::size_t max_bytes) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    std::string payload(max_bytes, '\0');
    asio::ip::udp::endpoint remote;
    const std::size_t n = co_await state_->socket.async_receive_from(
        asio::buffer(payload.data(), payload.size()), remote, exec::asio::use_sender);
    payload.resize(n);
    co_return UdpDatagram{std::move(payload), from_udp_endpoint(remote), 0u};
}

Task<std::string> UdpSocket::receive(std::size_t max_bytes) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    if (connected()) {
        std::string payload(max_bytes, '\0');
        const std::size_t n = co_await state_->socket.async_receive(
            asio::buffer(payload.data(), payload.size()), exec::asio::use_sender);
        payload.resize(n);
        co_return payload;
    }

    auto datagram = co_await receive_from(max_bytes);
    co_return std::move(datagram.payload);
}

UdpSocket::task_type UdpSocket::next(std::size_t max_bytes) {
    co_return co_await next_impl(state_, max_bytes);
}

UdpSocket::stream_type UdpSocket::packets(std::size_t max_bytes) {
    return stream_type([state = state_, max_bytes]() { return next_impl(state, max_bytes); });
}

Task<SocketAddress> UdpSocket::local_endpoint() {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    co_return from_udp_endpoint(state_->socket.local_endpoint());
}

Task<SocketAddress> UdpSocket::remote_endpoint() {
    if (!valid() || !connected()) {
        throw std::runtime_error("UdpSocket is not connected");
    }
    co_return from_udp_endpoint(state_->socket.remote_endpoint());
}

Task<void> UdpSocket::set_broadcast(bool enable) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    state_->socket.set_option(asio::socket_base::broadcast(enable));
    co_return;
}

Task<void> UdpSocket::set_ttl(int ttl) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    state_->socket.set_option(asio::ip::unicast::hops(ttl));
    co_return;
}

Task<void> UdpSocket::set_multicast_loop(bool enable) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    state_->socket.set_option(asio::ip::multicast::enable_loopback(enable));
    co_return;
}

Task<void> UdpSocket::set_multicast_ttl(int ttl) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }
    state_->socket.set_option(asio::ip::multicast::hops(ttl));
    co_return;
}

Task<void> UdpSocket::join_multicast_group(std::string multicast_address,
                                           std::string interface_address) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }

    const auto group = asio::ip::make_address(multicast_address);
    if (group.is_v4()) {
        if (interface_address.empty()) {
            state_->socket.set_option(asio::ip::multicast::join_group(group.to_v4()));
        } else {
            state_->socket.set_option(asio::ip::multicast::join_group(
                group.to_v4(), asio::ip::make_address_v4(interface_address)));
        }
    } else {
        unsigned int if_index = 0;
        if (!interface_address.empty()) {
            if_index = if_nametoindex(interface_address.c_str());
            if (if_index == 0) {
                if_index = static_cast<unsigned int>(std::stoul(interface_address));
            }
        }
        state_->socket.set_option(asio::ip::multicast::join_group(group.to_v6(), if_index));
    }
    co_return;
}

Task<void> UdpSocket::leave_multicast_group(std::string multicast_address,
                                            std::string interface_address) {
    if (!valid()) {
        throw std::runtime_error("UdpSocket is not valid");
    }

    const auto group = asio::ip::make_address(multicast_address);
    if (group.is_v4()) {
        if (interface_address.empty()) {
            state_->socket.set_option(asio::ip::multicast::leave_group(group.to_v4()));
        } else {
            state_->socket.set_option(asio::ip::multicast::leave_group(
                group.to_v4(), asio::ip::make_address_v4(interface_address)));
        }
    } else {
        unsigned int if_index = 0;
        if (!interface_address.empty()) {
            if_index = if_nametoindex(interface_address.c_str());
            if (if_index == 0) {
                if_index = static_cast<unsigned int>(std::stoul(interface_address));
            }
        }
        state_->socket.set_option(asio::ip::multicast::leave_group(group.to_v6(), if_index));
    }
    co_return;
}

Task<void> UdpSocket::close() {
    if (!state_) {
        co_return;
    }
    std::error_code ec;
    state_->socket.close(ec);
    state_.reset();
}

UdpSocket::task_type UdpSocket::next_impl(const std::shared_ptr<State> &state, std::size_t max_bytes) {
    if (!state || !state->socket.is_open()) {
        co_return std::nullopt;
    }

    UdpSocket socket(state);
    co_return co_await socket.receive_from(max_bytes);
}

} // namespace async_uv

