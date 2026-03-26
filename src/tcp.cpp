#include "async_uv/tcp.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "async_uv/cancel.h"
#include "detail/bridge.h"
#include "detail/cancel.h"

namespace async_uv {

struct PendingClose {
    detail::Completion<void> complete;
};

struct TcpClient::State {
    explicit State(Runtime *runtime_in) : runtime(runtime_in) {}

    struct PendingRead : detail::CancellableOperation<std::string> {
        std::size_t max_bytes = 0;
    };

    Runtime *runtime = nullptr;
    uv_tcp_t handle{};
    bool initialized = false;
    bool closing = false;
    bool closed = false;
    bool read_eof = false;
    std::shared_ptr<PendingRead> pending_read;
    std::shared_ptr<PendingClose> pending_close;
};

struct TcpListener::State {
    explicit State(Runtime *runtime_in) : runtime(runtime_in) {}

    struct PendingAccept : detail::CancellableOperation<TcpClient> {};

    Runtime *runtime = nullptr;
    uv_tcp_t handle{};
    bool initialized = false;
    bool closing = false;
    bool closed = false;
    int pending_error = 0;
    std::size_t pending_connections = 0;
    SocketAddress bound_address;
    std::shared_ptr<PendingAccept> pending_accept;
    std::shared_ptr<PendingClose> pending_close;
};

namespace {

using detail::Completion;

int to_native_family(AddressFamily family) {
    return static_cast<int>(family);
}

int to_native_socktype(ResolveTransport transport) {
    return transport == ResolveTransport::udp ? SOCK_DGRAM : SOCK_STREAM;
}

int to_native_protocol(ResolveTransport transport) {
    return transport == ResolveTransport::udp ? IPPROTO_UDP : IPPROTO_TCP;
}

int to_nameinfo_flags(const NameInfoOptions &options) {
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
#ifdef NI_NOFQDN
    if (options.no_fqdn) {
        flags |= NI_NOFQDN;
    }
#else
    (void)options;
#endif

    return flags;
}

void validate_bind_options(const TcpBindOptions &options) {
#ifndef UV_TCP_REUSEPORT
    if (options.reuse_port) {
        throw std::runtime_error("uv_tcp_bind reuse_port is not supported by this libuv build");
    }
#else
    (void)options;
#endif
}

unsigned int to_bind_flags(const TcpBindOptions &options) {
    unsigned int flags = 0;

    if (options.ipv6_only) {
        flags |= UV_TCP_IPV6ONLY;
    }

#ifdef UV_TCP_REUSEPORT
    if (options.reuse_port) {
        flags |= UV_TCP_REUSEPORT;
    }
#endif

    return flags;
}

SocketAddress socket_address_from_storage(const sockaddr_storage &storage, int length) {
    return SocketAddress::from_native(reinterpret_cast<const sockaddr *>(&storage), length);
}

SocketAddress get_bound_address(uv_tcp_t &handle) {
    sockaddr_storage storage{};
    int length = sizeof(storage);
    throw_if_uv_error(uv_tcp_getsockname(&handle, reinterpret_cast<sockaddr *>(&storage), &length),
                      "uv_tcp_getsockname");
    return socket_address_from_storage(storage, length);
}

template <typename State>
void finish_close(State *state) {
    state->closed = true;
    state->closing = false;
    auto pending = std::move(state->pending_close);
    if (pending) {
        pending->complete(detail::make_success());
    }
}

void close_client_handle_keep_alive(const std::shared_ptr<TcpClient::State> &state) {
    if (!state || !state->initialized ||
        uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
        return;
    }

    state->closing = true;
    auto *keep_alive = new std::shared_ptr<TcpClient::State>(state);
    state->handle.data = keep_alive;
    uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
        delete static_cast<std::shared_ptr<TcpClient::State> *>(handle->data);
    });
}

void close_listener_handle_keep_alive(const std::shared_ptr<TcpListener::State> &state) {
    if (!state || !state->initialized ||
        uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
        return;
    }

    state->closing = true;
    auto *keep_alive = new std::shared_ptr<TcpListener::State>(state);
    state->handle.data = keep_alive;
    uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
        delete static_cast<std::shared_ptr<TcpListener::State> *>(handle->data);
    });
}

void fail_client_pending_read(const std::shared_ptr<TcpClient::State> &state, std::string message) {
    if (!state || !state->pending_read) {
        return;
    }

    auto pending = std::move(state->pending_read);
    pending->finish_cancel(std::move(message));
}

void read_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    auto *state = static_cast<TcpClient::State *>(handle->data);
    std::size_t size = suggested_size;

    if (state->pending_read) {
        size =
            state->pending_read->max_bytes == 0 ? suggested_size : state->pending_read->max_bytes;
        if (suggested_size != 0) {
            size = std::min(size, suggested_size);
        }
    }

    if (size == 0) {
        size = 1;
    }

    char *memory = new char[size];
    *buf = uv_buf_init(memory, static_cast<unsigned int>(size));
}

void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    std::unique_ptr<char[]> guard(buf->base);
    auto *state = static_cast<TcpClient::State *>(stream->data);

    if (nread == 0) {
        return;
    }

    auto pending = std::move(state->pending_read);
    if (!pending) {
        return;
    }

    uv_read_stop(stream);

    if (nread == UV_EOF) {
        state->read_eof = true;
        emit_trace_event({"tcp", "read_eof", 0, 0});
        pending->finish_value(std::string{});
        return;
    }

    if (nread < 0) {
        emit_trace_event({"tcp", "read_error", static_cast<int>(nread), 0});
        pending->finish_uv_error("uv_read_start", static_cast<int>(nread));
        return;
    }

    emit_trace_event({"tcp", "read", 0, static_cast<std::size_t>(nread)});
    pending->finish_value(std::string(buf->base, static_cast<std::size_t>(nread)));
}

void maybe_accept(TcpListener::State *state) {
    if (!state->pending_accept || state->closed) {
        return;
    }

    if (state->pending_error != 0) {
        auto pending = std::move(state->pending_accept);
        const int error = state->pending_error;
        state->pending_error = 0;
        pending->finish_uv_error("uv_listen", error);
        return;
    }

    if (state->pending_connections == 0) {
        return;
    }

    auto pending = std::move(state->pending_accept);
    auto client_state = std::make_shared<TcpClient::State>(state->runtime);

    int rc = uv_tcp_init(state->runtime->loop(), &client_state->handle);
    if (rc < 0) {
        pending->finish_uv_error("uv_tcp_init", rc);
        return;
    }

    client_state->initialized = true;
    client_state->handle.data = client_state.get();

    rc = uv_accept(reinterpret_cast<uv_stream_t *>(&state->handle),
                   reinterpret_cast<uv_stream_t *>(&client_state->handle));
    if (rc < 0) {
        close_client_handle_keep_alive(client_state);
        pending->finish_uv_error("uv_accept", rc);
        return;
    }

    client_state->closing = false;
    client_state->closed = false;
    client_state->read_eof = false;
    --state->pending_connections;
    pending->finish_value(TcpClient(std::move(client_state)));
}

Task<std::vector<SocketAddress>>
resolve_impl(Runtime &runtime, std::string host, std::string service, ResolveOptions options) {
    if (host.empty() && !options.passive) {
        throw std::runtime_error("resolve requires a host unless passive mode is enabled");
    }

    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<std::vector<SocketAddress>>(
        runtime,
        [&runtime,
         signal = std::move(signal),
         host = std::move(host),
         service = std::move(service),
         options](Completion<std::vector<SocketAddress>> complete) mutable {
            struct ResolveOp : detail::CancellableOperation<std::vector<SocketAddress>> {
                uv_getaddrinfo_t req{};
                std::string host;
                std::string service;
            };

            auto op = std::make_shared<ResolveOp>();
            op->runtime = &runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->host = std::move(host);
            op->service = std::move(service);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_getaddrinfo canceled";
            if (!detail::install_terminate_handler<std::vector<SocketAddress>>(
                    op, [](ResolveOp &op) {
                        (void)uv_cancel(reinterpret_cast<uv_req_t *>(&op.req));
                        op.finish_cancel(kCancelMessage);
                    })) {
                auto active = detail::detach_shared<ResolveOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            addrinfo hints{};
            hints.ai_family = to_native_family(options.family);
            hints.ai_socktype = to_native_socktype(options.transport);
            hints.ai_protocol = to_native_protocol(options.transport);
            hints.ai_flags = options.passive ? AI_PASSIVE : 0;

            const char *node = op->host.empty() ? nullptr : op->host.c_str();
            const int rc = uv_getaddrinfo(
                runtime.loop(),
                &op->req,
                [](uv_getaddrinfo_t *req, int status, addrinfo *result) {
                    auto op = detail::detach_shared<ResolveOp>(req);
                    if (op->completed) {
                        if (result != nullptr) {
                            uv_freeaddrinfo(result);
                        }
                        return;
                    }

                    if (status < 0) {
                        if (result != nullptr) {
                            uv_freeaddrinfo(result);
                        }
                        op->finish_uv_error("uv_getaddrinfo", status);
                        return;
                    }

                    std::vector<SocketAddress> endpoints;
                    for (addrinfo *entry = result; entry != nullptr; entry = entry->ai_next) {
                        if (entry->ai_addr == nullptr) {
                            continue;
                        }
                        if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6) {
                            continue;
                        }
                        endpoints.push_back(SocketAddress::from_native(
                            entry->ai_addr, static_cast<int>(entry->ai_addrlen)));
                    }

                    if (result != nullptr) {
                        uv_freeaddrinfo(result);
                    }

                    op->finish_value(std::move(endpoints));
                },
                node,
                op->service.c_str(),
                &hints);

            if (rc < 0) {
                auto active = detail::detach_shared<ResolveOp>(&op->req);
                active->finish_uv_error("uv_getaddrinfo", rc);
            }
        });
}

Task<std::vector<SocketAddress>>
resolve_impl(Runtime &runtime, std::string host, int port, ResolveOptions options) {
    co_return co_await resolve_impl(runtime, std::move(host), std::to_string(port), options);
}

Task<NameInfo> lookup_name_impl(Runtime &runtime, SocketAddress endpoint, NameInfoOptions options) {
    if (!endpoint.valid()) {
        throw std::runtime_error("lookup_name requires a valid socket address");
    }

    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<NameInfo>(
        runtime,
        [&runtime, signal = std::move(signal), endpoint = std::move(endpoint), options](
            Completion<NameInfo> complete) mutable {
            struct LookupOp : detail::CancellableOperation<NameInfo> {
                uv_getnameinfo_t req{};
                SocketAddress endpoint;
            };

            auto op = std::make_shared<LookupOp>();
            op->runtime = &runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->endpoint = std::move(endpoint);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_getnameinfo canceled";
            if (!detail::install_terminate_handler<NameInfo>(op, [](LookupOp &op) {
                    (void)uv_cancel(reinterpret_cast<uv_req_t *>(&op.req));
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<LookupOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            const int rc = uv_getnameinfo(
                runtime.loop(),
                &op->req,
                [](uv_getnameinfo_t *req, int status, const char *hostname, const char *service) {
                    auto op = detail::detach_shared<LookupOp>(req);
                    if (op->completed) {
                        return;
                    }

                    if (status < 0) {
                        op->finish_uv_error("uv_getnameinfo", status);
                        return;
                    }

                    NameInfo info;
                    if (hostname != nullptr) {
                        info.host = hostname;
                    }
                    if (service != nullptr) {
                        info.service = service;
                    }

                    op->finish_value(std::move(info));
                },
                op->endpoint.data(),
                to_nameinfo_flags(options));

            if (rc < 0) {
                auto active = detail::detach_shared<LookupOp>(&op->req);
                active->finish_uv_error("uv_getnameinfo", rc);
            }
        });
}

Task<TcpClient> connect_endpoint(Runtime &runtime, SocketAddress endpoint) {
    if (!endpoint.valid()) {
        throw std::runtime_error("tcp connect requires a valid socket address");
    }

    auto signal = co_await get_current_signal();
    const auto endpoint_text = endpoint.to_string();
    auto state = std::make_shared<TcpClient::State>(&runtime);

    co_await detail::post_task<void>(
        runtime,
        [&runtime, state, signal = std::move(signal), endpoint = std::move(endpoint)](
            Completion<void> complete) mutable {
            int rc = uv_tcp_init(runtime.loop(), &state->handle);
            if (rc < 0) {
                complete(detail::make_uv_error<void>("uv_tcp_init", rc));
                return;
            }

            state->initialized = true;
            state->handle.data = state.get();

            struct ConnectOp : detail::CancellableOperation<void> {
                uv_connect_t req{};
                std::shared_ptr<TcpClient::State> state;
                SocketAddress endpoint;
            };

            auto op = std::make_shared<ConnectOp>();
            op->runtime = &runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->state = state;
            op->endpoint = std::move(endpoint);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_tcp_connect canceled";
            if (!detail::install_terminate_handler<void>(op, [](ConnectOp &op) {
                    fail_client_pending_read(op.state, "uv_tcp_connect canceled");
                    close_client_handle_keep_alive(op.state);
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<ConnectOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            rc = uv_tcp_connect(
                &op->req, &state->handle, op->endpoint.data(), [](uv_connect_t *req, int status) {
                    auto op = detail::detach_shared<ConnectOp>(req);
                    if (op->completed) {
                        return;
                    }

                    if (status < 0) {
                        close_client_handle_keep_alive(op->state);
                        emit_trace_event({"tcp", "connect_error", status, 0});
                        op->finish_uv_error("uv_tcp_connect", status);
                        return;
                    }

                    op->state->closing = false;
                    op->state->closed = false;
                    op->state->read_eof = false;
                    emit_trace_event({"tcp", "connect", 0, 0});
                    op->finish_value();
                });

            if (rc < 0) {
                auto active = detail::detach_shared<ConnectOp>(&op->req);
                close_client_handle_keep_alive(state);
                emit_trace_event({"tcp", "connect_error", rc, 0});
                active->finish_uv_error("uv_tcp_connect", rc);
            }
        });

    if (!state->initialized || state->closing || state->closed ||
        uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
        throw std::runtime_error("tcp connect returned a closed socket for endpoint " +
                                 endpoint_text);
    }
    co_return TcpClient(std::move(state));
}

Task<TcpListener> bind_endpoint(Runtime &runtime, SocketAddress endpoint, TcpBindOptions options) {
    validate_bind_options(options);
    co_await throw_if_cancelled("uv_tcp_bind canceled");

    if (!endpoint.valid()) {
        throw std::runtime_error("tcp bind requires a valid socket address");
    }

    auto state = std::make_shared<TcpListener::State>(&runtime);

    co_await detail::post_task<void>(
        runtime,
        [&runtime, state, endpoint = std::move(endpoint), options](
            Completion<void> complete) mutable {
            int rc = uv_tcp_init(runtime.loop(), &state->handle);
            if (rc < 0) {
                complete(detail::make_uv_error<void>("uv_tcp_init", rc));
                return;
            }

            state->initialized = true;
            state->handle.data = state.get();

            rc = uv_tcp_bind(&state->handle, endpoint.data(), to_bind_flags(options));
            if (rc < 0) {
                close_listener_handle_keep_alive(state);
                complete(detail::make_uv_error<void>("uv_tcp_bind", rc));
                return;
            }

            rc = uv_listen(reinterpret_cast<uv_stream_t *>(&state->handle),
                           options.backlog,
                           [](uv_stream_t *server, int status) {
                               auto *state = static_cast<TcpListener::State *>(server->data);
                               if (status < 0) {
                                   if (state->pending_accept) {
                                       auto pending = std::move(state->pending_accept);
                                       pending->finish_uv_error("uv_listen", status);
                                   } else {
                                       state->pending_error = status;
                                   }
                                   return;
                               }

                               ++state->pending_connections;
                               maybe_accept(state);
                           });

            if (rc < 0) {
                close_listener_handle_keep_alive(state);
                complete(detail::make_uv_error<void>("uv_listen", rc));
                return;
            }

            try {
                state->bound_address = get_bound_address(state->handle);
            } catch (...) {
                close_listener_handle_keep_alive(state);
                complete(detail::make_exception<void>(std::current_exception()));
                return;
            }

            complete(detail::make_success());
        });

    co_return TcpListener(std::move(state));
}

template <typename State, typename Getter>
Task<SocketAddress> query_socket_address(Runtime &runtime,
                                         const std::shared_ptr<State> &state,
                                         const char *closed_message,
                                         const char *where,
                                         Getter getter) {
    co_await throw_if_cancelled(std::string(where) + " canceled");

    co_return co_await detail::post_task<SocketAddress>(
        runtime,
        [state, closed_message, where, getter = std::move(getter)](
            Completion<SocketAddress> complete) mutable {
            if (!state->initialized || state->closed ||
                uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
                complete(detail::make_runtime_error<SocketAddress>(closed_message));
                return;
            }

            sockaddr_storage storage{};
            int length = sizeof(storage);
            const int rc = getter(&state->handle, reinterpret_cast<sockaddr *>(&storage), &length);
            if (rc < 0) {
                complete(detail::make_uv_error<SocketAddress>(where, rc));
                return;
            }

            complete(detail::make_success(socket_address_from_storage(storage, length)));
        });
}

template <typename State, typename Setter>
Task<void> apply_socket_option(Runtime &runtime,
                               const std::shared_ptr<State> &state,
                               const char *closed_message,
                               const char *where,
                               Setter setter) {
    co_await throw_if_cancelled(std::string(where) + " canceled");

    co_await detail::post_task<void>(
        runtime,
        [state, closed_message, where, setter = std::move(setter)](
            Completion<void> complete) mutable {
            if (!state->initialized || state->closed ||
                uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
                complete(detail::make_runtime_error<void>(closed_message));
                return;
            }

            const int rc = setter(&state->handle);
            if (rc < 0) {
                complete(detail::make_uv_error<void>(where, rc));
                return;
            }

            complete(detail::make_success());
        });
}

} // namespace

SocketAddress::SocketAddress(const sockaddr *address, int length) {
    if (address == nullptr || length <= 0 || length > static_cast<int>(sizeof(storage_))) {
        return;
    }

    std::memcpy(&storage_, address, static_cast<std::size_t>(length));
    length_ = length;
}

bool SocketAddress::valid() const noexcept {
    return length_ > 0 && (family() == AF_INET || family() == AF_INET6);
}

int SocketAddress::family() const noexcept {
    return length_ == 0 ? AF_UNSPEC : storage_.ss_family;
}

int SocketAddress::port() const noexcept {
    if (family() == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in *>(&storage_)->sin_port);
    }
    if (family() == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_port);
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

    const void *source = nullptr;
    char buffer[INET6_ADDRSTRLEN] = {};

    if (is_ipv4()) {
        source = &reinterpret_cast<const sockaddr_in *>(&storage_)->sin_addr;
    } else {
        source = &reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_addr;
    }

    const int rc = uv_inet_ntop(family(), source, buffer, sizeof(buffer));
    if (rc < 0) {
        throw_uv_error("uv_inet_ntop", rc);
    }

    return buffer;
}

std::string SocketAddress::to_string() const {
    if (!valid()) {
        return {};
    }

    auto host = ip();
    if (is_ipv6()) {
        return "[" + host + "]:" + std::to_string(port());
    }
    return host + ":" + std::to_string(port());
}

const sockaddr *SocketAddress::data() const noexcept {
    return reinterpret_cast<const sockaddr *>(&storage_);
}

sockaddr *SocketAddress::data() noexcept {
    return reinterpret_cast<sockaddr *>(&storage_);
}

int SocketAddress::size() const noexcept {
    return length_;
}

SocketAddress SocketAddress::from_native(const sockaddr *address, int length) {
    return SocketAddress(address, length);
}

SocketAddress SocketAddress::ipv4(std::string ip, int port) {
    sockaddr_in address{};
    throw_if_uv_error(uv_ip4_addr(ip.c_str(), port, &address), "uv_ip4_addr");
    return SocketAddress(reinterpret_cast<const sockaddr *>(&address), sizeof(address));
}

SocketAddress SocketAddress::ipv6(std::string ip, int port) {
    sockaddr_in6 address{};
    throw_if_uv_error(uv_ip6_addr(ip.c_str(), port, &address), "uv_ip6_addr");
    return SocketAddress(reinterpret_cast<const sockaddr *>(&address), sizeof(address));
}

Task<std::vector<SocketAddress>> resolve(std::string host, int port, ResolveOptions options) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await resolve_impl(*runtime, std::move(host), port, options);
}

Task<std::vector<SocketAddress>>
resolve(std::string host, std::string service, ResolveOptions options) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await resolve_impl(*runtime, std::move(host), std::move(service), options);
}

Task<std::vector<SocketAddress>> resolve_tcp(std::string host, int port, AddressFamily family) {
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

Task<std::vector<SocketAddress>> resolve_udp(std::string host, int port, AddressFamily family) {
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
    auto *runtime = co_await get_current_runtime();
    co_return co_await lookup_name_impl(*runtime, std::move(endpoint), options);
}

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
    auto *runtime = co_await get_current_runtime();
    auto endpoints = co_await resolve_impl(
        *runtime, std::move(host), port, ResolveOptions{options.family, false});
    if (endpoints.empty()) {
        throw std::runtime_error("resolve did not return any TCP endpoints");
    }

    std::exception_ptr last_error;
    for (const auto &endpoint : endpoints) {
        co_await throw_if_cancelled("uv_tcp_connect canceled");
        try {
            auto client = co_await connect_endpoint(*runtime, endpoint);
            co_return client;
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }

    throw std::runtime_error("failed to connect to any resolved TCP endpoint");
}

Task<TcpClient> TcpClient::connect(SocketAddress endpoint) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await connect_endpoint(*runtime, std::move(endpoint));
}

bool TcpClient::valid() const noexcept {
    return state_ && state_->initialized && !state_->closing && !state_->closed;
}

bool TcpClient::eof() const noexcept {
    return state_ && state_->read_eof;
}

Task<std::size_t> TcpClient::write(std::string_view data) {
    if (!valid()) {
        throw std::runtime_error("tcp client is not connected");
    }

    if (data.empty()) {
        co_return 0;
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<std::size_t>(
        *state->runtime,
        [state, signal = std::move(signal), payload = std::string(data)](
            Completion<std::size_t> complete) mutable {
            struct WriteOp : detail::CancellableOperation<std::size_t> {
                uv_write_t req{};
                std::shared_ptr<State> state;
                std::string payload;
            };

            auto op = std::make_shared<WriteOp>();
            op->runtime = state->runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->state = state;
            op->payload = std::move(payload);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_write canceled";
            if (!detail::install_terminate_handler<std::size_t>(op, [](WriteOp &op) {
                    fail_client_pending_read(op.state, "uv_write canceled");
                    close_client_handle_keep_alive(op.state);
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<WriteOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            uv_buf_t buf =
                uv_buf_init(op->payload.data(), static_cast<unsigned int>(op->payload.size()));
            const int rc = uv_write(&op->req,
                                    reinterpret_cast<uv_stream_t *>(&state->handle),
                                    &buf,
                                    1,
                                    [](uv_write_t *req, int status) {
                                        auto op = detail::detach_shared<WriteOp>(req);
                                        if (op->completed) {
                                            return;
                                        }

                                        if (status < 0) {
                                            emit_trace_event({"tcp", "write_error", status, 0});
                                            op->finish_uv_error("uv_write", status);
                                            return;
                                        }

                                        emit_trace_event({"tcp", "write", 0, op->payload.size()});
                                        op->finish_value(op->payload.size());
                                    });

            if (rc < 0) {
                auto active = detail::detach_shared<WriteOp>(&op->req);
                emit_trace_event({"tcp", "write_error", rc, 0});
                active->finish_uv_error("uv_write", rc);
            }
        });
}

Task<std::size_t> TcpClient::write_all(std::string_view data) {
    co_return co_await write(data);
}

Task<std::string> TcpClient::read_once(std::size_t max_bytes) {
    co_return co_await read_some(max_bytes);
}

Task<std::string> TcpClient::read_some(std::size_t max_bytes) {
    if (!valid()) {
        throw std::runtime_error("tcp client is not connected");
    }

    if (max_bytes == 0) {
        co_return std::string{};
    }

    if (state_->read_eof) {
        co_return std::string{};
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<std::string>(
        *state->runtime,
        [state, signal = std::move(signal), max_bytes](Completion<std::string> complete) mutable {
            if (state->pending_read) {
                complete(
                    detail::make_runtime_error<std::string>("only one pending read is supported"));
                return;
            }

            if (state->read_eof) {
                complete(detail::make_success(std::string{}));
                return;
            }

            state->pending_read = std::make_shared<State::PendingRead>();
            state->pending_read->runtime = state->runtime;
            state->pending_read->bind_signal(signal);
            state->pending_read->complete = std::move(complete);
            state->pending_read->max_bytes = max_bytes;

            constexpr auto kCancelMessage = "uv_read_start canceled";
            if (!detail::install_terminate_handler<std::string>(
                    state->pending_read, [state](State::PendingRead &pending) {
                        uv_read_stop(reinterpret_cast<uv_stream_t *>(&state->handle));
                        if (state->pending_read && state->pending_read.get() == &pending) {
                            state->pending_read.reset();
                        }
                        pending.finish_cancel("uv_read_start canceled");
                    })) {
                auto pending = std::move(state->pending_read);
                pending->finish_cancel(kCancelMessage);
                return;
            }

            const int rc =
                uv_read_start(reinterpret_cast<uv_stream_t *>(&state->handle), read_alloc, read_cb);
            if (rc < 0) {
                auto pending = std::move(state->pending_read);
                pending->finish_uv_error("uv_read_start", rc);
            }
        });
}

Task<std::string> TcpClient::read_exactly(std::size_t bytes) {
    std::string result;
    result.reserve(bytes);

    while (result.size() < bytes) {
        co_await throw_if_cancelled("uv_read_start canceled");
        auto chunk = co_await read_some(bytes - result.size());
        if (chunk.empty()) {
            throw std::runtime_error("unexpected EOF while reading exact TCP payload");
        }
        result += chunk;
    }

    co_return result;
}

Task<std::string> TcpClient::read_all(std::size_t chunk_size) {
    std::string result;

    while (true) {
        co_await throw_if_cancelled("uv_read_start canceled");
        auto chunk = co_await read_some(chunk_size);
        if (chunk.empty()) {
            break;
        }
        result += chunk;
    }

    co_return result;
}

TcpClient::task_type TcpClient::next(std::size_t max_bytes) {
    if (!state_) {
        throw std::runtime_error("tcp client is empty");
    }
    co_return co_await next_impl(state_, max_bytes);
}

TcpClient::stream_type TcpClient::chunks(std::size_t max_bytes) {
    if (!state_) {
        return {};
    }

    auto state = state_;
    return stream_type([state = std::move(state), max_bytes]() -> task_type {
        co_return co_await next_impl(state, max_bytes);
    });
}

TcpClient::task_type TcpClient::next_impl(const std::shared_ptr<State> &state,
                                          std::size_t max_bytes) {
    if (max_bytes == 0) {
        throw std::runtime_error("tcp chunk size must be greater than zero");
    }

    if (!state || state->closed || state->closing) {
        co_return std::nullopt;
    }

    TcpClient client(state);
    try {
        auto chunk = co_await client.read_some(max_bytes);
        if (chunk.empty() && state->read_eof) {
            co_return std::nullopt;
        }
        co_return chunk;
    } catch (...) {
        if (state->closed || state->closing) {
            co_return std::nullopt;
        }
        throw;
    }
}

Task<SocketAddress> TcpClient::local_address() {
    if (!state_) {
        throw std::runtime_error("tcp client is empty");
    }

    co_return co_await query_socket_address(*state_->runtime,
                                            state_,
                                            "tcp client is closed",
                                            "uv_tcp_getsockname",
                                            [](uv_tcp_t *handle, sockaddr *address, int *length) {
                                                return uv_tcp_getsockname(handle, address, length);
                                            });
}

Task<SocketAddress> TcpClient::peer_address() {
    if (!state_) {
        throw std::runtime_error("tcp client is empty");
    }

    co_return co_await query_socket_address(*state_->runtime,
                                            state_,
                                            "tcp client is closed",
                                            "uv_tcp_getpeername",
                                            [](uv_tcp_t *handle, sockaddr *address, int *length) {
                                                return uv_tcp_getpeername(handle, address, length);
                                            });
}

Task<void> TcpClient::set_nodelay(bool enable) {
    if (!state_) {
        throw std::runtime_error("tcp client is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "tcp client is closed",
                                 "uv_tcp_nodelay",
                                 [enable](uv_tcp_t *handle) {
                                     return uv_tcp_nodelay(handle, enable ? 1 : 0);
                                 });
}

Task<void> TcpClient::set_keepalive(bool enable, unsigned int delay_seconds) {
    if (!state_) {
        throw std::runtime_error("tcp client is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "tcp client is closed",
                                 "uv_tcp_keepalive",
                                 [enable, delay_seconds](uv_tcp_t *handle) {
                                     return uv_tcp_keepalive(handle, enable ? 1 : 0, delay_seconds);
                                 });
}

Task<void> TcpClient::shutdown() {
    if (!valid()) {
        co_return;
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_await detail::post_task<void>(
        *state->runtime, [state, signal = std::move(signal)](Completion<void> complete) mutable {
            struct ShutdownOp : detail::CancellableOperation<void> {
                uv_shutdown_t req{};
                std::shared_ptr<State> state;
            };

            auto op = std::make_shared<ShutdownOp>();
            op->state = state;
            op->runtime = state->runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_shutdown canceled";
            if (!detail::install_terminate_handler<void>(op, [](ShutdownOp &op) {
                    fail_client_pending_read(op.state, "uv_shutdown canceled");
                    close_client_handle_keep_alive(op.state);
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<ShutdownOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            const int rc = uv_shutdown(&op->req,
                                       reinterpret_cast<uv_stream_t *>(&state->handle),
                                       [](uv_shutdown_t *req, int status) {
                                           auto op = detail::detach_shared<ShutdownOp>(req);
                                           if (op->completed) {
                                               return;
                                           }

                                           if (status < 0) {
                                               op->finish_uv_error("uv_shutdown", status);
                                               return;
                                           }

                                           op->finish_value();
                                       });

            if (rc < 0) {
                auto active = detail::detach_shared<ShutdownOp>(&op->req);
                active->finish_uv_error("uv_shutdown", rc);
            }
        });
}

Task<void> TcpClient::close() {
    if (!state_ || state_->closed) {
        co_return;
    }

    auto state = state_;
    co_await detail::post_task<void>(*state->runtime, [state](Completion<void> complete) mutable {
        if (state->closed || !state->initialized ||
            uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
            complete(detail::make_success());
            return;
        }

        if (state->pending_read) {
            auto pending = std::move(state->pending_read);
            pending->finish_exception(
                std::make_exception_ptr(std::runtime_error("socket closed while reading")));
        }

        state->closing = true;
        state->pending_close = std::make_shared<PendingClose>(PendingClose{std::move(complete)});
        uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
            finish_close(static_cast<TcpClient::State *>(handle->data));
        });
    });
}

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
    co_return co_await bind(std::move(host), port, TcpBindOptions{backlog});
}

Task<TcpListener> TcpListener::bind(std::string host, int port, TcpBindOptions options) {
    auto *runtime = co_await get_current_runtime();
    auto endpoints = co_await resolve_impl(
        *runtime, std::move(host), port, ResolveOptions{options.family, true});
    if (endpoints.empty()) {
        throw std::runtime_error("resolve did not return any TCP bind endpoints");
    }

    std::exception_ptr last_error;
    for (const auto &endpoint : endpoints) {
        co_await throw_if_cancelled("uv_tcp_bind canceled");
        try {
            auto listener = co_await bind_endpoint(*runtime, endpoint, options);
            co_return listener;
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }

    throw std::runtime_error("failed to bind any resolved TCP endpoint");
}

Task<TcpListener> TcpListener::bind(SocketAddress endpoint, int backlog) {
    co_return co_await bind(std::move(endpoint), TcpBindOptions{backlog});
}

Task<TcpListener> TcpListener::bind(SocketAddress endpoint, TcpBindOptions options) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await bind_endpoint(*runtime, std::move(endpoint), options);
}

bool TcpListener::valid() const noexcept {
    return state_ && state_->initialized && !state_->closing && !state_->closed;
}

int TcpListener::port() const noexcept {
    return state_ ? state_->bound_address.port() : 0;
}

Task<SocketAddress> TcpListener::local_address() {
    if (!state_) {
        throw std::runtime_error("tcp listener is empty");
    }

    if (state_->bound_address.valid()) {
        co_return state_->bound_address;
    }

    co_return co_await query_socket_address(*state_->runtime,
                                            state_,
                                            "tcp listener is closed",
                                            "uv_tcp_getsockname",
                                            [](uv_tcp_t *handle, sockaddr *address, int *length) {
                                                return uv_tcp_getsockname(handle, address, length);
                                            });
}

Task<void> TcpListener::set_simultaneous_accepts(bool enable) {
    if (!state_) {
        throw std::runtime_error("tcp listener is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "tcp listener is closed",
                                 "uv_tcp_simultaneous_accepts",
                                 [enable](uv_tcp_t *handle) {
                                     return uv_tcp_simultaneous_accepts(handle, enable ? 1 : 0);
                                 });
}

Task<TcpClient> TcpListener::accept() {
    if (!valid()) {
        throw std::runtime_error("listener is closed");
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<TcpClient>(
        *state->runtime,
        [state, signal = std::move(signal)](Completion<TcpClient> complete) mutable {
            if (state->pending_accept) {
                complete(
                    detail::make_runtime_error<TcpClient>("only one pending accept is supported"));
                return;
            }

            state->pending_accept = std::make_shared<State::PendingAccept>();
            state->pending_accept->runtime = state->runtime;
            state->pending_accept->bind_signal(signal);
            state->pending_accept->complete = std::move(complete);

            constexpr auto kCancelMessage = "uv_accept canceled";
            if (!detail::install_terminate_handler<TcpClient>(
                    state->pending_accept, [state](State::PendingAccept &pending) {
                        if (state->pending_accept && state->pending_accept.get() == &pending) {
                            state->pending_accept.reset();
                        }
                        pending.finish_cancel("uv_accept canceled");
                    })) {
                auto pending = std::move(state->pending_accept);
                pending->finish_cancel(kCancelMessage);
                return;
            }

            maybe_accept(state.get());
        });
}

Task<void> TcpListener::close() {
    if (!state_ || state_->closed) {
        co_return;
    }

    auto state = state_;
    co_await detail::post_task<void>(*state->runtime, [state](Completion<void> complete) mutable {
        if (state->closed || !state->initialized ||
            uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
            complete(detail::make_success());
            return;
        }

        if (state->pending_accept) {
            auto pending = std::move(state->pending_accept);
            pending->finish_exception(
                std::make_exception_ptr(std::runtime_error("listener closed while accepting")));
        }

        state->closing = true;
        state->pending_close = std::make_shared<PendingClose>(PendingClose{std::move(complete)});
        uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
            finish_close(static_cast<TcpListener::State *>(handle->data));
        });
    });
}

} // namespace async_uv
