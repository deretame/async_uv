#include "async_uv/tcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace async_uv {

namespace {

namespace asio = exec::asio::asio_impl;

int to_port(std::uint16_t port) {
    return static_cast<int>(port);
}

SocketAddress from_tcp_endpoint(const asio::ip::tcp::endpoint &endpoint) {
    auto *addr = reinterpret_cast<const sockaddr *>(endpoint.data());
    return SocketAddress::from_native(addr, static_cast<int>(endpoint.size()));
}

SocketAddress from_udp_endpoint(const asio::ip::udp::endpoint &endpoint) {
    auto *addr = reinterpret_cast<const sockaddr *>(endpoint.data());
    return SocketAddress::from_native(addr, static_cast<int>(endpoint.size()));
}

asio::ip::address to_asio_address(const SocketAddress &address) {
    return asio::ip::make_address(address.ip());
}

asio::ip::tcp::endpoint to_tcp_endpoint(const SocketAddress &address) {
    if (!address.valid()) {
        throw std::runtime_error("invalid socket address");
    }
    return asio::ip::tcp::endpoint(to_asio_address(address), static_cast<unsigned short>(address.port()));
}

asio::ip::resolver_base::flags to_resolver_flags(const ResolveOptions &options) {
    asio::ip::resolver_base::flags flags = asio::ip::resolver_base::flags{};
    if (options.passive) {
        flags |= asio::ip::resolver_base::passive;
    }
    if (options.family == AddressFamily::ipv4) {
        flags |= asio::ip::resolver_base::address_configured;
        flags |= asio::ip::resolver_base::v4_mapped;
    }
    if (options.family == AddressFamily::ipv6) {
        flags |= asio::ip::resolver_base::address_configured;
    }
    return flags;
}

bool family_matches(const SocketAddress &address, AddressFamily family) {
    if (family == AddressFamily::unspecified) {
        return true;
    }
    if (family == AddressFamily::ipv4) {
        return address.is_ipv4();
    }
    if (family == AddressFamily::ipv6) {
        return address.is_ipv6();
    }
    return false;
}

} // namespace

SocketAddress::SocketAddress(const sockaddr *address, int length)
    : length_(length) {
    if (address == nullptr || length <= 0 || length > static_cast<int>(sizeof(storage_))) {
        length_ = 0;
        std::memset(&storage_, 0, sizeof(storage_));
        return;
    }
    std::memset(&storage_, 0, sizeof(storage_));
    std::memcpy(&storage_, address, static_cast<std::size_t>(length));
}

bool SocketAddress::valid() const noexcept {
    return length_ > 0;
}

int SocketAddress::family() const noexcept {
    return valid() ? static_cast<int>(storage_.ss_family) : AF_UNSPEC;
}

int SocketAddress::port() const noexcept {
    if (!valid()) {
        return 0;
    }
    if (storage_.ss_family == AF_INET) {
        const auto *addr = reinterpret_cast<const sockaddr_in *>(&storage_);
        return ntohs(addr->sin_port);
    }
    if (storage_.ss_family == AF_INET6) {
        const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&storage_);
        return ntohs(addr->sin6_port);
    }
    return 0;
}

bool SocketAddress::is_ipv4() const noexcept {
    return family() == AF_INET;
}

bool SocketAddress::is_ipv6() const noexcept {
    return family() == AF_INET6;
}

std::string SocketAddress::ip() const {
    if (!valid()) {
        return {};
    }

    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (storage_.ss_family == AF_INET) {
        const auto *addr = reinterpret_cast<const sockaddr_in *>(&storage_);
        if (inet_ntop(AF_INET, &addr->sin_addr, buffer.data(), static_cast<socklen_t>(buffer.size())) ==
            nullptr) {
            throw std::system_error(errno, std::generic_category(), "inet_ntop(AF_INET)");
        }
        return std::string(buffer.data());
    }
    if (storage_.ss_family == AF_INET6) {
        const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&storage_);
        if (inet_ntop(
                AF_INET6, &addr->sin6_addr, buffer.data(), static_cast<socklen_t>(buffer.size())) ==
            nullptr) {
            throw std::system_error(errno, std::generic_category(), "inet_ntop(AF_INET6)");
        }
        return std::string(buffer.data());
    }
    return {};
}

std::string SocketAddress::to_string() const {
    if (!valid()) {
        return {};
    }
    const auto host = ip();
    if (is_ipv6()) {
        return "[" + host + "]:" + std::to_string(port());
    }
    return host + ":" + std::to_string(port());
}

const sockaddr *SocketAddress::data() const noexcept {
    return valid() ? reinterpret_cast<const sockaddr *>(&storage_) : nullptr;
}

sockaddr *SocketAddress::data() noexcept {
    return valid() ? reinterpret_cast<sockaddr *>(&storage_) : nullptr;
}

int SocketAddress::size() const noexcept {
    return length_;
}

SocketAddress SocketAddress::from_native(const sockaddr *address, int length) {
    return SocketAddress(address, length);
}

SocketAddress SocketAddress::ipv4(std::string ip, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + ip);
    }
    return SocketAddress(reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
}

SocketAddress SocketAddress::ipv6(std::string ip, int port) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET6, ip.c_str(), &addr.sin6_addr) != 1) {
        throw std::runtime_error("invalid IPv6 address: " + ip);
    }
    return SocketAddress(reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
}

Task<std::vector<SocketAddress>> resolve(std::string host, int port, ResolveOptions options) {
    co_return co_await resolve(std::move(host), std::to_string(port), options);
}

Task<std::vector<SocketAddress>>
resolve(std::string host, std::string service, ResolveOptions options) {
    auto ex = co_await get_current_loop();
    auto flags = to_resolver_flags(options);

    std::vector<SocketAddress> addresses;
    if (options.transport == ResolveTransport::udp) {
        asio::ip::udp::resolver resolver(ex);
        auto results = co_await resolver.async_resolve(host, service, flags, exec::asio::use_sender);
        for (const auto &entry : results) {
            addresses.emplace_back(from_udp_endpoint(entry.endpoint()));
        }
    } else {
        asio::ip::tcp::resolver resolver(ex);
        auto results = co_await resolver.async_resolve(host, service, flags, exec::asio::use_sender);
        for (const auto &entry : results) {
            addresses.emplace_back(from_tcp_endpoint(entry.endpoint()));
        }
    }
    co_return addresses;
}

Task<std::vector<SocketAddress>>
resolve_tcp(std::string host, int port, AddressFamily family) {
    ResolveOptions options;
    options.family = family;
    options.transport = ResolveTransport::tcp;
    co_return co_await resolve(std::move(host), port, options);
}

Task<std::vector<SocketAddress>>
resolve_tcp(std::string host, std::string service, AddressFamily family) {
    ResolveOptions options;
    options.family = family;
    options.transport = ResolveTransport::tcp;
    co_return co_await resolve(std::move(host), std::move(service), options);
}

Task<std::vector<SocketAddress>>
resolve_udp(std::string host, int port, AddressFamily family) {
    ResolveOptions options;
    options.family = family;
    options.transport = ResolveTransport::udp;
    co_return co_await resolve(std::move(host), port, options);
}

Task<std::vector<SocketAddress>>
resolve_udp(std::string host, std::string service, AddressFamily family) {
    ResolveOptions options;
    options.family = family;
    options.transport = ResolveTransport::udp;
    co_return co_await resolve(std::move(host), std::move(service), options);
}

Task<NameInfo> lookup_name(SocketAddress endpoint, NameInfoOptions options) {
    if (!endpoint.valid()) {
        throw std::runtime_error("lookup_name: invalid endpoint");
    }

    int flags = 0;
    if (options.numeric_host) {
        flags |= NI_NUMERICHOST;
    }
    if (options.numeric_service) {
        flags |= NI_NUMERICSERV;
    }
    if (options.name_required) {
        flags |= NI_NAMEREQD;
    }
    if (options.datagram_service) {
        flags |= NI_DGRAM;
    }
    if (options.no_fqdn) {
        flags |= NI_NOFQDN;
    }

    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> service{};
    const int rc = getnameinfo(endpoint.data(),
                               static_cast<socklen_t>(endpoint.size()),
                               host.data(),
                               static_cast<socklen_t>(host.size()),
                               service.data(),
                               static_cast<socklen_t>(service.size()),
                               flags);
    if (rc != 0) {
        throw std::runtime_error(std::string("lookup_name failed: ") + gai_strerror(rc));
    }

    co_return NameInfo{std::string(host.data()), std::string(service.data())};
}

struct TcpClient::State {
    Runtime *runtime = nullptr;
    asio::ip::tcp::socket socket;
    bool eof = false;

    explicit State(Runtime *rt) : runtime(rt), socket(rt->executor()) {}
};

TcpClient::TcpClient(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TcpClient::~TcpClient() = default;

TcpClient::TcpClient(TcpClient &&other) noexcept : state_(std::move(other.state_)) {}

TcpClient &TcpClient::operator=(TcpClient &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<TcpClient> TcpClient::connect(std::string host, int port) {
    co_return co_await connect(std::move(host), port, {});
}

Task<TcpClient> TcpClient::connect(std::string host, int port, TcpConnectOptions options) {
    auto endpoints = co_await resolve_tcp(std::move(host), port, options.family);
    std::exception_ptr last_error;

    for (const auto &endpoint : endpoints) {
        if (!family_matches(endpoint, options.family)) {
            continue;
        }
        try {
            co_return co_await connect(endpoint);
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }
    throw std::runtime_error("no TCP endpoint resolved");
}

Task<TcpClient> TcpClient::connect(SocketAddress endpoint) {
    auto *runtime = co_await get_current_runtime();
    auto state = std::make_shared<State>(runtime);
    co_await state->socket.async_connect(to_tcp_endpoint(endpoint), exec::asio::use_sender);
    co_return TcpClient(std::move(state));
}

bool TcpClient::valid() const noexcept {
    return state_ != nullptr && state_->socket.is_open();
}

bool TcpClient::eof() const noexcept {
    return state_ != nullptr && state_->eof;
}

Task<std::size_t> TcpClient::write(std::string_view data) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    co_return co_await state_->socket.async_write_some(
        asio::buffer(data.data(), data.size()), exec::asio::use_sender);
}

Task<std::size_t> TcpClient::write_all(std::string_view data) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    co_return co_await asio::async_write(
        state_->socket, asio::buffer(data.data(), data.size()), exec::asio::use_sender);
}

Task<std::string> TcpClient::read_once(std::size_t max_bytes) {
    co_return co_await read_some(max_bytes);
}

Task<std::string> TcpClient::read_some(std::size_t max_bytes) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }

    std::string buffer(max_bytes, '\0');
    try {
        const std::size_t n = co_await state_->socket.async_read_some(
            asio::buffer(buffer.data(), buffer.size()), exec::asio::use_sender);
        if (n == 0) {
            state_->eof = true;
            co_return std::string{};
        }
        buffer.resize(n);
        co_return buffer;
    } catch (const std::system_error &error) {
        if (error.code() == asio::error::eof) {
            state_->eof = true;
            co_return std::string{};
        }
        throw;
    }
}

Task<std::string> TcpClient::read_exactly(std::size_t bytes) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    std::string buffer(bytes, '\0');
    try {
        (void)co_await asio::async_read(
            state_->socket, asio::buffer(buffer.data(), buffer.size()), exec::asio::use_sender);
    } catch (const std::system_error &error) {
        if (error.code() == asio::error::eof) {
            state_->eof = true;
            throw;
        }
        throw;
    }
    co_return buffer;
}

Task<std::string> TcpClient::read_all(std::size_t chunk_size) {
    std::string data;
    while (true) {
        auto chunk = co_await read_some(chunk_size);
        if (chunk.empty() && eof()) {
            break;
        }
        data += chunk;
    }
    co_return data;
}

TcpClient::task_type TcpClient::next(std::size_t max_bytes) {
    co_return co_await next_impl(state_, max_bytes);
}

TcpClient::stream_type TcpClient::chunks(std::size_t max_bytes) {
    return stream_type([state = state_, max_bytes]() { return next_impl(state, max_bytes); });
}

Task<SocketAddress> TcpClient::local_address() {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    co_return from_tcp_endpoint(state_->socket.local_endpoint());
}

Task<SocketAddress> TcpClient::peer_address() {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    co_return from_tcp_endpoint(state_->socket.remote_endpoint());
}

Task<void> TcpClient::set_nodelay(bool enable) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    state_->socket.set_option(asio::ip::tcp::no_delay(enable));
    co_return;
}

Task<void> TcpClient::set_keepalive(bool enable, unsigned int) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    state_->socket.set_option(asio::socket_base::keep_alive(enable));
    co_return;
}

Task<void> TcpClient::shutdown() {
    if (!valid()) {
        co_return;
    }
    std::error_code ec;
    state_->socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    if (ec && ec != asio::error::not_connected) {
        throw std::system_error(ec);
    }
}

Task<void> TcpClient::close() {
    if (!state_) {
        co_return;
    }
    std::error_code ec;
    state_->socket.close(ec);
    state_->eof = true;
}

TcpClient::task_type TcpClient::next_impl(const std::shared_ptr<State> &state, std::size_t max_bytes) {
    if (!state || !state->socket.is_open()) {
        co_return std::nullopt;
    }

    TcpClient client(state);
    auto chunk = co_await client.read_some(max_bytes);
    if (chunk.empty() && client.eof()) {
        co_return std::nullopt;
    }
    co_return chunk;
}

struct TcpListener::State {
    Runtime *runtime = nullptr;
    asio::ip::tcp::acceptor acceptor;
    int bound_port = 0;

    explicit State(Runtime *rt) : runtime(rt), acceptor(rt->executor()) {}
};

TcpListener::TcpListener(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TcpListener::~TcpListener() = default;

TcpListener::TcpListener(TcpListener &&other) noexcept : state_(std::move(other.state_)) {}

TcpListener &TcpListener::operator=(TcpListener &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<TcpListener> TcpListener::bind(std::string host, int port, int backlog) {
    TcpBindOptions options;
    options.backlog = backlog;
    co_return co_await bind(std::move(host), port, options);
}

Task<TcpListener> TcpListener::bind(std::string host, int port, TcpBindOptions options) {
    ResolveOptions resolve_options;
    resolve_options.family = options.family;
    resolve_options.passive = true;
    resolve_options.transport = ResolveTransport::tcp;

    auto endpoints = co_await resolve(std::move(host), port, resolve_options);
    std::exception_ptr last_error;

    for (auto &endpoint : endpoints) {
        try {
            co_return co_await bind(endpoint, options);
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }
    throw std::runtime_error("unable to bind TCP listener");
}

Task<TcpListener> TcpListener::bind(SocketAddress endpoint, int backlog) {
    TcpBindOptions options;
    options.backlog = backlog;
    co_return co_await bind(std::move(endpoint), options);
}

Task<TcpListener> TcpListener::bind(SocketAddress endpoint, TcpBindOptions options) {
    auto *runtime = co_await get_current_runtime();
    auto state = std::make_shared<State>(runtime);

    auto ep = to_tcp_endpoint(endpoint);
    state->acceptor.open(ep.protocol());
    state->acceptor.set_option(asio::socket_base::reuse_address(true));
    if (options.ipv6_only && ep.address().is_v6()) {
        state->acceptor.set_option(asio::ip::v6_only(true));
    }
    state->acceptor.bind(ep);
    state->acceptor.listen(options.backlog);
    state->bound_port = to_port(state->acceptor.local_endpoint().port());

    co_return TcpListener(std::move(state));
}

bool TcpListener::valid() const noexcept {
    return state_ != nullptr && state_->acceptor.is_open();
}

int TcpListener::port() const noexcept {
    return state_ ? state_->bound_port : 0;
}

Task<SocketAddress> TcpListener::local_address() {
    if (!valid()) {
        throw std::runtime_error("TcpListener is not bound");
    }
    co_return from_tcp_endpoint(state_->acceptor.local_endpoint());
}

Task<void> TcpListener::set_simultaneous_accepts(bool) {
    co_return;
}

Task<TcpClient> TcpListener::accept() {
    if (!valid()) {
        throw std::runtime_error("TcpListener is not bound");
    }

    auto client_state = std::make_shared<TcpClient::State>(state_->runtime);
    co_await state_->acceptor.async_accept(client_state->socket, exec::asio::use_sender);
    co_return TcpClient(std::move(client_state));
}

Task<void> TcpListener::close() {
    if (!state_) {
        co_return;
    }
    std::error_code ec;
    state_->acceptor.close(ec);
    state_.reset();
}

} // namespace async_uv
