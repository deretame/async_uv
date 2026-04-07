#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

#include "async_uv/cancel.h"
#include "async_uv/runtime.h"
#include "async_uv/stream.h"

namespace async_uv {

enum class AddressFamily : int {
    unspecified = AF_UNSPEC,
    ipv4 = AF_INET,
    ipv6 = AF_INET6,
};

enum class ResolveTransport : int {
    tcp,
    udp,
};

struct ResolveOptions {
    AddressFamily family = AddressFamily::unspecified;
    bool passive = false;
    ResolveTransport transport = ResolveTransport::tcp;
};

struct NameInfoOptions {
    bool numeric_host = false;
    bool numeric_service = false;
    bool name_required = false;
    bool datagram_service = false;
    bool no_fqdn = false;
};

struct NameInfo {
    std::string host;
    std::string service;
};

struct TcpConnectOptions {
    AddressFamily family = AddressFamily::unspecified;
};

struct TcpBindOptions {
    int backlog = 128;
    AddressFamily family = AddressFamily::unspecified;
    bool ipv6_only = false;
    bool reuse_port = false;
};

class SocketAddress {
public:
    SocketAddress() = default;

    bool valid() const noexcept;
    int family() const noexcept;
    int port() const noexcept;
    bool is_ipv4() const noexcept;
    bool is_ipv6() const noexcept;

    std::string ip() const;
    std::string to_string() const;

    const sockaddr *data() const noexcept;
    sockaddr *data() noexcept;
    int size() const noexcept;

    static SocketAddress from_native(const sockaddr *address, int length);
    static SocketAddress ipv4(std::string ip, int port);
    static SocketAddress ipv6(std::string ip, int port);

private:
    SocketAddress(const sockaddr *address, int length);

    sockaddr_storage storage_{};
    int length_ = 0;
};

using Endpoint = SocketAddress;

Task<std::vector<SocketAddress>> resolve(std::string host, int port, ResolveOptions options = {});
Task<std::vector<SocketAddress>>
resolve(std::string host, std::string service, ResolveOptions options = {});
Task<std::vector<SocketAddress>>
resolve_tcp(std::string host, int port, AddressFamily family = AddressFamily::unspecified);
Task<std::vector<SocketAddress>> resolve_tcp(std::string host,
                                             std::string service,
                                             AddressFamily family = AddressFamily::unspecified);
Task<std::vector<SocketAddress>>
resolve_udp(std::string host, int port, AddressFamily family = AddressFamily::unspecified);
Task<std::vector<SocketAddress>> resolve_udp(std::string host,
                                             std::string service,
                                             AddressFamily family = AddressFamily::unspecified);
Task<NameInfo> lookup_name(SocketAddress endpoint, NameInfoOptions options = {});

class TcpClient {
public:
    struct State;
    using endpoint_type = SocketAddress;
    using next_type = std::optional<std::string>;
    using task_type = Task<next_type>;
    using stream_type = Stream<std::string>;

    TcpClient() = default;
    explicit TcpClient(std::shared_ptr<State> state) noexcept;
    ~TcpClient();

    TcpClient(const TcpClient &) = delete;
    TcpClient &operator=(const TcpClient &) = delete;
    TcpClient(TcpClient &&other) noexcept;
    TcpClient &operator=(TcpClient &&other) noexcept;

    static Task<TcpClient> connect(std::string host, int port);
    static Task<TcpClient> connect(std::string host, int port, TcpConnectOptions options);
    static Task<TcpClient> connect(SocketAddress endpoint);

    template <typename Rep, typename Period>
    static Task<TcpClient>
    connect_for(std::string host, int port, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, connect(std::move(host), port));
    }

    template <typename Rep, typename Period>
    static Task<TcpClient> connect_for(std::string host,
                                       int port,
                                       TcpConnectOptions options,
                                       std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, connect(std::move(host), port, options));
    }

    template <typename Rep, typename Period>
    static Task<TcpClient> connect_for(SocketAddress endpoint,
                                       std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, connect(std::move(endpoint)));
    }

    template <typename Clock, typename Duration>
    static Task<TcpClient>
    connect_until(std::string host, int port, std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, connect(std::move(host), port));
    }

    template <typename Clock, typename Duration>
    static Task<TcpClient> connect_until(std::string host,
                                         int port,
                                         TcpConnectOptions options,
                                         std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline,
                                                   connect(std::move(host), port, options));
    }

    template <typename Clock, typename Duration>
    static Task<TcpClient> connect_until(SocketAddress endpoint,
                                         std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, connect(std::move(endpoint)));
    }

    bool valid() const noexcept;
    bool eof() const noexcept;

    Task<std::size_t> write(std::string_view data);
    Task<std::size_t> write_all(std::string_view data);
    Task<std::string> read_once(std::size_t max_bytes = 64 * 1024);
    Task<std::string> read_some(std::size_t max_bytes = 64 * 1024);
    Task<std::string> read_exactly(std::size_t bytes);
    Task<std::string> read_all(std::size_t chunk_size = 64 * 1024);
    task_type next(std::size_t max_bytes = 64 * 1024);
    stream_type chunks(std::size_t max_bytes = 64 * 1024);

    Task<SocketAddress> local_address();
    Task<SocketAddress> peer_address();
    Task<void> set_nodelay(bool enable = true);
    Task<void> set_keepalive(bool enable = true, unsigned int delay_seconds = 60);
    Task<void> shutdown();
    Task<void> close();

    template <typename Rep, typename Period>
    Task<std::size_t> write_for(std::string_view data, std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, write(data));
    }

    template <typename Rep, typename Period>
    Task<std::size_t> write_all_for(std::string_view data,
                                    std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, write_all(data));
    }

    template <typename Rep, typename Period>
    Task<std::string> read_once_for(std::chrono::duration<Rep, Period> timeout,
                                    std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_timeout(timeout, read_once(max_bytes));
    }

    template <typename Rep, typename Period>
    Task<std::string> read_some_for(std::chrono::duration<Rep, Period> timeout,
                                    std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_timeout(timeout, read_some(max_bytes));
    }

    template <typename Rep, typename Period>
    Task<std::string> read_exactly_for(std::size_t bytes,
                                       std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, read_exactly(bytes));
    }

    template <typename Rep, typename Period>
    Task<std::string> read_all_for(std::chrono::duration<Rep, Period> timeout,
                                   std::size_t chunk_size = 64 * 1024) {
        co_return co_await async_uv::with_timeout(timeout, read_all(chunk_size));
    }

    template <typename Clock, typename Duration>
    Task<std::size_t> write_until(std::string_view data,
                                  std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, write(data));
    }

    template <typename Clock, typename Duration>
    Task<std::size_t> write_all_until(std::string_view data,
                                      std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, write_all(data));
    }

    template <typename Clock, typename Duration>
    Task<std::string> read_once_until(std::chrono::time_point<Clock, Duration> deadline,
                                      std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_deadline(deadline, read_once(max_bytes));
    }

    template <typename Clock, typename Duration>
    Task<std::string> read_some_until(std::chrono::time_point<Clock, Duration> deadline,
                                      std::size_t max_bytes = 64 * 1024) {
        co_return co_await async_uv::with_deadline(deadline, read_some(max_bytes));
    }

    template <typename Clock, typename Duration>
    Task<std::string> read_exactly_until(std::size_t bytes,
                                         std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, read_exactly(bytes));
    }

    template <typename Clock, typename Duration>
    Task<std::string> read_all_until(std::chrono::time_point<Clock, Duration> deadline,
                                     std::size_t chunk_size = 64 * 1024) {
        co_return co_await async_uv::with_deadline(deadline, read_all(chunk_size));
    }

    template <typename Rep, typename Period>
    Task<void> shutdown_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, shutdown());
    }

    template <typename Rep, typename Period>
    Task<void> close_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, close());
    }

    template <typename Clock, typename Duration>
    Task<void> shutdown_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, shutdown());
    }

    template <typename Clock, typename Duration>
    Task<void> close_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, close());
    }

    Task<std::size_t> send(std::string_view data) {
        return write(data);
    }
    Task<std::size_t> send_all(std::string_view data) {
        return write_all(data);
    }
    Task<std::string> receive(std::size_t max_bytes = 64 * 1024) {
        return read_some(max_bytes);
    }
    Task<std::string> receive_exactly(std::size_t bytes) {
        return read_exactly(bytes);
    }
    Task<std::string> receive_all(std::size_t chunk_size = 64 * 1024) {
        return read_all(chunk_size);
    }
    task_type receive_next(std::size_t max_bytes = 64 * 1024) {
        return next(max_bytes);
    }
    stream_type receive_chunks(std::size_t max_bytes = 64 * 1024) {
        return chunks(max_bytes);
    }
    Task<SocketAddress> local_endpoint() {
        return local_address();
    }
    Task<SocketAddress> remote_endpoint() {
        return peer_address();
    }

private:
    static task_type next_impl(const std::shared_ptr<State> &state, std::size_t max_bytes);

    std::shared_ptr<State> state_;
};

class TcpListener {
public:
    struct State;
    using endpoint_type = SocketAddress;
    using socket_type = TcpClient;

    TcpListener() = default;
    explicit TcpListener(std::shared_ptr<State> state) noexcept;
    ~TcpListener();

    TcpListener(const TcpListener &) = delete;
    TcpListener &operator=(const TcpListener &) = delete;
    TcpListener(TcpListener &&other) noexcept;
    TcpListener &operator=(TcpListener &&other) noexcept;

    static Task<TcpListener> bind(std::string host, int port, int backlog = 128);
    static Task<TcpListener> bind(std::string host, int port, TcpBindOptions options);
    static Task<TcpListener> bind(SocketAddress endpoint, int backlog = 128);
    static Task<TcpListener> bind(SocketAddress endpoint, TcpBindOptions options);

    bool valid() const noexcept;
    int port() const noexcept;

    Task<SocketAddress> local_address();
    Task<void> set_simultaneous_accepts(bool enable = true);
    Task<TcpClient> accept();
    Task<void> close();

    template <typename Rep, typename Period>
    Task<TcpClient> accept_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, accept());
    }

    template <typename Clock, typename Duration>
    Task<TcpClient> accept_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, accept());
    }

    template <typename Rep, typename Period>
    Task<void> close_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, close());
    }

    template <typename Clock, typename Duration>
    Task<void> close_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, close());
    }

    Task<SocketAddress> local_endpoint() {
        return local_address();
    }

private:
    std::shared_ptr<State> state_;
};

using TcpSocket = TcpClient;
using TcpAcceptor = TcpListener;

} // namespace async_uv

