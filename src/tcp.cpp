#include "flux/tcp.h"

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace flux {

namespace {

namespace asio = exec::asio::asio_impl;

int to_port(std::uint16_t port) {
    return static_cast<int>(port);
}

SocketAddress from_tcp_endpoint(const asio::ip::tcp::endpoint &endpoint) {
    return SocketAddress::from_asio(endpoint.address(), static_cast<int>(endpoint.port()));
}

SocketAddress from_udp_endpoint(const asio::ip::udp::endpoint &endpoint) {
    return SocketAddress::from_asio(endpoint.address(), static_cast<int>(endpoint.port()));
}

asio::ip::address to_asio_address(const SocketAddress &address) {
    return address.as_asio_address();
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

TcpClient::ConstBuffer to_const_buffer(std::string_view data) {
    return TcpClient::ConstBuffer{
        reinterpret_cast<const std::byte *>(data.data()),
        data.size()};
}

} // namespace

SocketAddress::SocketAddress(exec::asio::asio_impl::ip::address address, unsigned short port)
    : address_(std::move(address)), port_(port) {}

bool SocketAddress::valid() const noexcept {
    return address_.has_value();
}

int SocketAddress::family() const noexcept {
    if (!address_) {
        return AF_UNSPEC;
    }
    return address_->is_v4() ? AF_INET : AF_INET6;
}

int SocketAddress::port() const noexcept {
    return static_cast<int>(port_);
}

bool SocketAddress::is_ipv4() const noexcept {
    return address_.has_value() && address_->is_v4();
}

bool SocketAddress::is_ipv6() const noexcept {
    return address_.has_value() && address_->is_v6();
}

exec::asio::asio_impl::ip::address SocketAddress::as_asio_address() const {
    if (!address_) {
        throw std::runtime_error("invalid socket address");
    }
    return *address_;
}

std::string SocketAddress::ip() const {
    return address_ ? address_->to_string() : std::string{};
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

void SocketAddress::refresh_native_cache() const noexcept {
    if (!address_) {
        native_length_ = 0;
        native_ready_ = true;
        return;
    }

    if (native_ready_) {
        return;
    }

    std::memset(&native_storage_, 0, sizeof(native_storage_));
    if (address_->is_v4()) {
        auto *addr = reinterpret_cast<sockaddr_in *>(&native_storage_);
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port_);
        const auto bytes = address_->to_v4().to_bytes();
        std::memcpy(&addr->sin_addr, bytes.data(), bytes.size());
        native_length_ = static_cast<int>(sizeof(sockaddr_in));
    } else {
        auto *addr = reinterpret_cast<sockaddr_in6 *>(&native_storage_);
        addr->sin6_family = AF_INET6;
        addr->sin6_port = htons(port_);
        addr->sin6_scope_id = address_->to_v6().scope_id();
        const auto bytes = address_->to_v6().to_bytes();
        std::memcpy(&addr->sin6_addr, bytes.data(), bytes.size());
        native_length_ = static_cast<int>(sizeof(sockaddr_in6));
    }
    native_ready_ = true;
}

const sockaddr *SocketAddress::data() const noexcept {
    refresh_native_cache();
    return native_length_ > 0 ? reinterpret_cast<const sockaddr *>(&native_storage_) : nullptr;
}

sockaddr *SocketAddress::data() noexcept {
    refresh_native_cache();
    return native_length_ > 0 ? reinterpret_cast<sockaddr *>(&native_storage_) : nullptr;
}

int SocketAddress::size() const noexcept {
    refresh_native_cache();
    return native_length_;
}

SocketAddress SocketAddress::from_asio(exec::asio::asio_impl::ip::address address, int port) {
    if (port < 0 || port > 65535) {
        throw std::runtime_error("port out of range");
    }
    return SocketAddress(std::move(address), static_cast<unsigned short>(port));
}

SocketAddress SocketAddress::from_native(const sockaddr *address, int length) {
    if (address == nullptr || length <= 0) {
        return {};
    }

    if (address->sa_family == AF_INET) {
        if (length < static_cast<int>(sizeof(sockaddr_in))) {
            return {};
        }
        const auto *addr = reinterpret_cast<const sockaddr_in *>(address);
        asio::ip::address_v4::bytes_type bytes{};
        std::memcpy(bytes.data(), &addr->sin_addr, bytes.size());
        return SocketAddress(asio::ip::address_v4(bytes), ntohs(addr->sin_port));
    }

    if (address->sa_family == AF_INET6) {
        if (length < static_cast<int>(sizeof(sockaddr_in6))) {
            return {};
        }
        const auto *addr = reinterpret_cast<const sockaddr_in6 *>(address);
        asio::ip::address_v6::bytes_type bytes{};
        std::memcpy(bytes.data(), &addr->sin6_addr, bytes.size());
        return SocketAddress(asio::ip::address_v6(bytes, addr->sin6_scope_id),
                             ntohs(addr->sin6_port));
    }

    return {};
}

SocketAddress SocketAddress::ipv4(std::string ip, int port) {
    std::error_code ec;
    const auto parsed = asio::ip::make_address_v4(ip, ec);
    if (ec) {
        throw std::runtime_error("invalid IPv4 address: " + ip);
    }

    return from_asio(parsed, port);
}

SocketAddress SocketAddress::ipv6(std::string ip, int port) {
    std::error_code ec;
    const auto parsed = asio::ip::make_address_v6(ip, ec);
    if (ec) {
        throw std::runtime_error("invalid IPv6 address: " + ip);
    }

    return from_asio(parsed, port);
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

    // Asio-first path: when no getnameinfo-specific flags are requested,
    // use resolver reverse lookup directly.
    if (!options.numeric_host && !options.numeric_service && !options.name_required &&
        !options.datagram_service && !options.no_fqdn) {
        auto ex = co_await get_current_loop();
        if (endpoint.is_ipv4() || endpoint.is_ipv6()) {
            asio::ip::tcp::resolver resolver(ex);
            asio::ip::tcp::endpoint ep(
                endpoint.as_asio_address(), static_cast<unsigned short>(endpoint.port()));
            auto results = co_await resolver.async_resolve(ep, exec::asio::use_sender);
            const auto it = results.begin();
            if (it == results.end()) {
                throw std::runtime_error("lookup_name failed: empty reverse resolution result");
            }
            co_return NameInfo{it->host_name(), it->service_name()};
        }
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

Task<std::size_t> TcpClient::write(ConstBuffer data) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    if (data.empty()) {
        co_return 0;
    }
    co_return co_await state_->socket.async_write_some(
        asio::buffer(static_cast<const void *>(data.data()), data.size()), exec::asio::use_sender);
}

Task<std::size_t> TcpClient::write_all(ConstBuffer data) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    if (data.empty()) {
        co_return 0;
    }
    co_return co_await asio::async_write(
        state_->socket,
        asio::buffer(static_cast<const void *>(data.data()), data.size()),
        exec::asio::use_sender);
}

Task<std::size_t> TcpClient::write(std::string_view data) {
    co_return co_await write(to_const_buffer(data));
}

Task<std::size_t> TcpClient::write_all(std::string_view data) {
    co_return co_await write_all(to_const_buffer(data));
}

Task<std::string> TcpClient::read_once(std::size_t max_bytes) {
    co_return co_await read_some(max_bytes);
}

Task<std::size_t> TcpClient::read_some(MutableBuffer output) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    if (output.empty()) {
        co_return 0;
    }

    try {
        const std::size_t n = co_await state_->socket.async_read_some(
            asio::buffer(static_cast<void *>(output.data()), output.size()), exec::asio::use_sender);
        if (n == 0) {
            state_->eof = true;
            co_return 0;
        }
        co_return n;
    } catch (const std::system_error &error) {
        if (error.code() == asio::error::eof) {
            state_->eof = true;
            co_return 0;
        }
        throw;
    }
}

Task<std::size_t> TcpClient::read_exactly(MutableBuffer output) {
    if (!valid()) {
        throw std::runtime_error("TcpClient is not connected");
    }
    if (output.empty()) {
        co_return 0;
    }
    try {
        co_return co_await asio::async_read(
            state_->socket, asio::buffer(static_cast<void *>(output.data()), output.size()), exec::asio::use_sender);
    } catch (const std::system_error &error) {
        if (error.code() == asio::error::eof) {
            state_->eof = true;
            throw;
        }
        throw;
    }
}

Task<std::string> TcpClient::read_some(std::size_t max_bytes) {
    if (max_bytes == 0) {
        co_return std::string{};
    }

    std::string buffer(max_bytes, '\0');
    const auto n = co_await read_some(MutableBuffer{
        reinterpret_cast<std::byte *>(buffer.data()),
        buffer.size()});
    buffer.resize(n);
    co_return buffer;
}

Task<std::string> TcpClient::read_exactly(std::size_t bytes) {
    if (bytes == 0) {
        co_return std::string{};
    }

    std::string buffer(bytes, '\0');
    const auto n = co_await read_exactly(MutableBuffer{
        reinterpret_cast<std::byte *>(buffer.data()),
        buffer.size()});
    buffer.resize(n);
    co_return buffer;
}

Task<std::string> TcpClient::read_all(std::size_t chunk_size) {
    if (chunk_size == 0) {
        co_return std::string{};
    }

    std::string data;
    data.reserve(chunk_size);
    std::string chunk(chunk_size, '\0');

    while (true) {
        const auto n = co_await read_some(MutableBuffer{
            reinterpret_cast<std::byte *>(chunk.data()),
            chunk.size()});
        if (n == 0 && eof()) {
            break;
        }
        data.append(chunk.data(), n);
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

} // namespace flux
