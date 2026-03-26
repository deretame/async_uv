#include "async_uv/udp.h"

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

namespace {

using detail::Completion;

struct PendingClose {
    Completion<void> complete;
};

struct UdpState {
    struct PendingReceive : detail::CancellableOperation<UdpDatagram> {
        std::size_t max_bytes = 0;
    };
};

int sockaddr_length(const sockaddr *address) {
    if (address == nullptr) {
        return 0;
    }

    switch (address->sa_family) {
        case AF_INET:
            return sizeof(sockaddr_in);
        case AF_INET6:
            return sizeof(sockaddr_in6);
        default:
            return 0;
    }
}

void validate_bind_options(const UdpBindOptions &options) {
#ifndef UV_UDP_REUSEPORT
    if (options.reuse_port) {
        throw std::runtime_error("uv_udp_bind reuse_port is not supported by this libuv build");
    }
#else
    (void)options;
#endif
}

unsigned int to_bind_flags(const UdpBindOptions &options) {
    unsigned int flags = 0;

    if (options.ipv6_only) {
        flags |= UV_UDP_IPV6ONLY;
    }
    if (options.reuse_addr) {
        flags |= UV_UDP_REUSEADDR;
    }

#ifdef UV_UDP_REUSEPORT
    if (options.reuse_port) {
        flags |= UV_UDP_REUSEPORT;
    }
#endif

    return flags;
}

SocketAddress socket_address_from_native(const sockaddr *address) {
    return SocketAddress::from_native(address, sockaddr_length(address));
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

} // namespace

struct UdpSocket::State {
    explicit State(Runtime *runtime_in) : runtime(runtime_in) {}

    Runtime *runtime = nullptr;
    uv_udp_t handle{};
    bool initialized = false;
    bool closing = false;
    bool closed = false;
    bool is_connected = false;
    SocketAddress local_address;
    SocketAddress connected_endpoint;
    std::shared_ptr<UdpState::PendingReceive> pending_receive;
    std::shared_ptr<PendingClose> pending_close;
};

namespace {

void recv_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    auto *state = static_cast<UdpSocket::State *>(handle->data);
    std::size_t size = suggested_size;

    if (state->pending_receive) {
        size = state->pending_receive->max_bytes == 0 ? suggested_size
                                                      : state->pending_receive->max_bytes;
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

void close_udp_handle_keep_alive(const std::shared_ptr<UdpSocket::State> &state) {
    if (!state || !state->initialized ||
        uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
        return;
    }

    state->closing = true;
    auto *keep_alive = new std::shared_ptr<UdpSocket::State>(state);
    state->handle.data = keep_alive;
    uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
        delete static_cast<std::shared_ptr<UdpSocket::State> *>(handle->data);
    });
}

void fail_udp_pending_receive(const std::shared_ptr<UdpSocket::State> &state, std::string message) {
    if (!state || !state->pending_receive) {
        return;
    }

    auto pending = std::move(state->pending_receive);
    pending->finish_cancel(std::move(message));
}

void recv_cb(
    uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const sockaddr *address, unsigned flags) {
    std::unique_ptr<char[]> guard(buf->base);
    auto *state = static_cast<UdpSocket::State *>(handle->data);

    if (nread == 0 && address == nullptr) {
        return;
    }

    auto pending = std::move(state->pending_receive);
    if (!pending) {
        return;
    }

    uv_udp_recv_stop(handle);

    if (nread < 0) {
        emit_trace_event({"udp", "recv_error", static_cast<int>(nread), 0});
        pending->finish_uv_error("uv_udp_recv_start", static_cast<int>(nread));
        return;
    }

    UdpDatagram datagram;
    datagram.payload.assign(buf->base, static_cast<std::size_t>(nread));
    datagram.flags = flags;

    if (address != nullptr) {
        datagram.remote_endpoint = socket_address_from_native(address);
    } else if (state->connected_endpoint.valid()) {
        datagram.remote_endpoint = state->connected_endpoint;
    }

    emit_trace_event({"udp", "recv", 0, static_cast<std::size_t>(nread)});
    pending->finish_value(std::move(datagram));
}

Task<UdpSocket> bind_endpoint(Runtime &runtime, SocketAddress endpoint, UdpBindOptions options) {
    validate_bind_options(options);
    co_await throw_if_cancelled("uv_udp_bind canceled");

    if (!endpoint.valid()) {
        throw std::runtime_error("udp bind requires a valid socket address");
    }

    auto state = std::make_shared<UdpSocket::State>(&runtime);

    co_await detail::post_task<void>(
        runtime,
        [&runtime, state, endpoint = std::move(endpoint), options](
            Completion<void> complete) mutable {
            int rc = uv_udp_init(runtime.loop(), &state->handle);
            if (rc < 0) {
                complete(detail::make_uv_error<void>("uv_udp_init", rc));
                return;
            }

            state->initialized = true;
            state->handle.data = state.get();

            rc = uv_udp_bind(&state->handle, endpoint.data(), to_bind_flags(options));
            if (rc < 0) {
                close_udp_handle_keep_alive(state);
                complete(detail::make_uv_error<void>("uv_udp_bind", rc));
                return;
            }

            sockaddr_storage storage{};
            int length = sizeof(storage);
            rc =
                uv_udp_getsockname(&state->handle, reinterpret_cast<sockaddr *>(&storage), &length);
            if (rc < 0) {
                close_udp_handle_keep_alive(state);
                complete(detail::make_uv_error<void>("uv_udp_getsockname", rc));
                return;
            }

            state->local_address =
                SocketAddress::from_native(reinterpret_cast<const sockaddr *>(&storage), length);
            complete(detail::make_success());
        });

    co_return UdpSocket(std::move(state));
}

Task<UdpSocket> connect_endpoint(Runtime &runtime, SocketAddress endpoint) {
    if (!endpoint.valid()) {
        throw std::runtime_error("udp connect requires a valid socket address");
    }

    co_await throw_if_cancelled("uv_udp_connect canceled");

    auto state = std::make_shared<UdpSocket::State>(&runtime);

    co_await detail::post_task<void>(
        runtime,
        [&runtime, state, endpoint = std::move(endpoint)](Completion<void> complete) mutable {
            int rc = uv_udp_init(runtime.loop(), &state->handle);
            if (rc < 0) {
                complete(detail::make_uv_error<void>("uv_udp_init", rc));
                return;
            }

            state->initialized = true;
            state->handle.data = state.get();

            rc = uv_udp_connect(&state->handle, endpoint.data());
            if (rc < 0) {
                close_udp_handle_keep_alive(state);
                complete(detail::make_uv_error<void>("uv_udp_connect", rc));
                return;
            }

            state->is_connected = true;
            state->connected_endpoint = endpoint;

            sockaddr_storage storage{};
            int length = sizeof(storage);
            rc =
                uv_udp_getsockname(&state->handle, reinterpret_cast<sockaddr *>(&storage), &length);
            if (rc == 0) {
                state->local_address = SocketAddress::from_native(
                    reinterpret_cast<const sockaddr *>(&storage), length);
            }

            complete(detail::make_success());
        });

    co_return UdpSocket(std::move(state));
}

template <typename Getter>
Task<SocketAddress> query_socket_address(Runtime &runtime,
                                         const std::shared_ptr<UdpSocket::State> &state,
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

            complete(detail::make_success(
                SocketAddress::from_native(reinterpret_cast<const sockaddr *>(&storage), length)));
        });
}

template <typename Setter>
Task<void> apply_socket_option(Runtime &runtime,
                               const std::shared_ptr<UdpSocket::State> &state,
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
    auto *runtime = co_await get_current_runtime();
    ResolveOptions resolve_options;
    resolve_options.family = options.family;
    resolve_options.passive = true;
    resolve_options.transport = ResolveTransport::udp;

    auto endpoints = co_await resolve(std::move(host), port, resolve_options);
    if (endpoints.empty()) {
        throw std::runtime_error("resolve did not return any UDP bind endpoints");
    }

    std::exception_ptr last_error;
    for (const auto &endpoint : endpoints) {
        co_await throw_if_cancelled("uv_udp_bind canceled");
        try {
            auto socket = co_await bind_endpoint(*runtime, endpoint, options);
            co_return socket;
        } catch (...) {
            last_error = std::current_exception();
        }
    }

    if (last_error) {
        std::rethrow_exception(last_error);
    }

    throw std::runtime_error("failed to bind any resolved UDP endpoint");
}

Task<UdpSocket> UdpSocket::bind(SocketAddress endpoint) {
    co_return co_await bind(std::move(endpoint), {});
}

Task<UdpSocket> UdpSocket::bind(SocketAddress endpoint, UdpBindOptions options) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await bind_endpoint(*runtime, std::move(endpoint), options);
}

Task<UdpSocket> UdpSocket::connect(std::string host, int port) {
    co_return co_await connect(std::move(host), port, {});
}

Task<UdpSocket> UdpSocket::connect(std::string host, int port, UdpConnectOptions options) {
    auto *runtime = co_await get_current_runtime();
    ResolveOptions resolve_options;
    resolve_options.family = options.family;
    resolve_options.transport = ResolveTransport::udp;

    auto endpoints = co_await resolve(std::move(host), port, resolve_options);
    if (endpoints.empty()) {
        throw std::runtime_error("resolve did not return any UDP endpoints");
    }

    co_await throw_if_cancelled("uv_udp_connect canceled");
    auto socket = co_await connect_endpoint(*runtime, endpoints.front());
    co_return socket;
}

Task<UdpSocket> UdpSocket::connect(SocketAddress endpoint) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await connect_endpoint(*runtime, std::move(endpoint));
}

bool UdpSocket::valid() const noexcept {
    return state_ && state_->initialized && !state_->closing && !state_->closed;
}

bool UdpSocket::connected() const noexcept {
    return valid() && state_->is_connected;
}

Task<std::size_t> UdpSocket::send(std::string_view data) {
    if (!connected()) {
        throw std::runtime_error("udp socket is not connected");
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<std::size_t>(
        *state->runtime,
        [state, signal = std::move(signal), payload = std::string(data)](
            Completion<std::size_t> complete) mutable {
            struct SendOp : detail::CancellableOperation<std::size_t> {
                uv_udp_send_t req{};
                std::shared_ptr<State> state;
                std::string payload;
            };

            auto op = std::make_shared<SendOp>();
            op->runtime = state->runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->state = state;
            op->payload = std::move(payload);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_udp_send canceled";
            if (!detail::install_terminate_handler<std::size_t>(op, [](SendOp &op) {
                    fail_udp_pending_receive(op.state, "uv_udp_send canceled");
                    close_udp_handle_keep_alive(op.state);
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<SendOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            uv_buf_t buffer =
                uv_buf_init(op->payload.data(), static_cast<unsigned int>(op->payload.size()));
            const int rc = uv_udp_send(
                &op->req, &state->handle, &buffer, 1, nullptr, [](uv_udp_send_t *req, int status) {
                    auto op = detail::detach_shared<SendOp>(req);
                    if (op->completed) {
                        return;
                    }

                    if (status < 0) {
                        emit_trace_event({"udp", "send_error", status, 0});
                        op->finish_uv_error("uv_udp_send", status);
                        return;
                    }

                    emit_trace_event({"udp", "send", 0, op->payload.size()});
                    op->finish_value(op->payload.size());
                });

            if (rc < 0) {
                auto active = detail::detach_shared<SendOp>(&op->req);
                emit_trace_event({"udp", "send_error", rc, 0});
                active->finish_uv_error("uv_udp_send", rc);
            }
        });
}

Task<std::size_t> UdpSocket::send_all(std::string_view data) {
    co_return co_await send(data);
}

Task<std::size_t> UdpSocket::send_to(std::string_view data, SocketAddress endpoint) {
    if (!valid()) {
        throw std::runtime_error("udp socket is closed");
    }
    if (!endpoint.valid()) {
        throw std::runtime_error("udp send_to requires a valid remote endpoint");
    }
    if (state_->is_connected) {
        const auto same_endpoint = endpoint.family() == state_->connected_endpoint.family() &&
                                   endpoint.port() == state_->connected_endpoint.port() &&
                                   endpoint.ip() == state_->connected_endpoint.ip();
        if (same_endpoint) {
            co_return co_await send(data);
        }
        throw std::runtime_error("udp socket is connected; use send() for its remote endpoint");
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<std::size_t>(
        *state->runtime,
        [state,
         signal = std::move(signal),
         payload = std::string(data),
         endpoint = std::move(endpoint)](Completion<std::size_t> complete) mutable {
            struct SendOp : detail::CancellableOperation<std::size_t> {
                uv_udp_send_t req{};
                std::shared_ptr<State> state;
                std::string payload;
                SocketAddress endpoint;
            };

            auto op = std::make_shared<SendOp>();
            op->runtime = state->runtime;
            op->bind_signal(signal);
            op->complete = std::move(complete);
            op->state = state;
            op->payload = std::move(payload);
            op->endpoint = std::move(endpoint);
            detail::attach_shared(op->req, op);

            constexpr auto kCancelMessage = "uv_udp_send canceled";
            if (!detail::install_terminate_handler<std::size_t>(op, [](SendOp &op) {
                    fail_udp_pending_receive(op.state, "uv_udp_send canceled");
                    close_udp_handle_keep_alive(op.state);
                    op.finish_cancel(kCancelMessage);
                })) {
                auto active = detail::detach_shared<SendOp>(&op->req);
                active->finish_cancel(kCancelMessage);
                return;
            }

            uv_buf_t buffer =
                uv_buf_init(op->payload.data(), static_cast<unsigned int>(op->payload.size()));
            const int rc = uv_udp_send(&op->req,
                                       &state->handle,
                                       &buffer,
                                       1,
                                       op->endpoint.data(),
                                       [](uv_udp_send_t *req, int status) {
                                           auto op = detail::detach_shared<SendOp>(req);
                                           if (op->completed) {
                                               return;
                                           }

                                           if (status < 0) {
                                               emit_trace_event({"udp", "send_error", status, 0});
                                               op->finish_uv_error("uv_udp_send", status);
                                               return;
                                           }

                                           emit_trace_event({"udp", "send", 0, op->payload.size()});
                                           op->finish_value(op->payload.size());
                                       });

            if (rc < 0) {
                auto active = detail::detach_shared<SendOp>(&op->req);
                emit_trace_event({"udp", "send_error", rc, 0});
                active->finish_uv_error("uv_udp_send", rc);
            }
        });
}

Task<UdpDatagram> UdpSocket::receive_from(std::size_t max_bytes) {
    if (!valid()) {
        throw std::runtime_error("udp socket is closed");
    }
    if (max_bytes == 0) {
        throw std::runtime_error("udp receive size must be greater than zero");
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<UdpDatagram>(
        *state->runtime,
        [state, signal = std::move(signal), max_bytes](Completion<UdpDatagram> complete) mutable {
            if (state->pending_receive) {
                complete(detail::make_runtime_error<UdpDatagram>(
                    "only one pending udp receive is supported"));
                return;
            }

            state->pending_receive = std::make_shared<UdpState::PendingReceive>();
            state->pending_receive->runtime = state->runtime;
            state->pending_receive->bind_signal(signal);
            state->pending_receive->complete = std::move(complete);
            state->pending_receive->max_bytes = max_bytes;

            constexpr auto kCancelMessage = "uv_udp_recv_start canceled";
            if (!detail::install_terminate_handler<UdpDatagram>(
                    state->pending_receive, [state](UdpState::PendingReceive &pending) {
                        uv_udp_recv_stop(&state->handle);
                        if (state->pending_receive && state->pending_receive.get() == &pending) {
                            state->pending_receive.reset();
                        }
                        pending.finish_cancel("uv_udp_recv_start canceled");
                    })) {
                auto pending = std::move(state->pending_receive);
                pending->finish_cancel(kCancelMessage);
                return;
            }

            const int rc = uv_udp_recv_start(&state->handle, recv_alloc, recv_cb);
            if (rc < 0) {
                auto pending = std::move(state->pending_receive);
                pending->finish_uv_error("uv_udp_recv_start", rc);
            }
        });
}

Task<std::string> UdpSocket::receive(std::size_t max_bytes) {
    if (!connected()) {
        throw std::runtime_error("udp socket is not connected");
    }

    auto datagram = co_await receive_from(max_bytes);
    co_return datagram.payload;
}

UdpSocket::task_type UdpSocket::next(std::size_t max_bytes) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }
    co_return co_await next_impl(state_, max_bytes);
}

UdpSocket::stream_type UdpSocket::packets(std::size_t max_bytes) {
    if (!state_) {
        return {};
    }

    auto state = state_;
    return stream_type([state = std::move(state), max_bytes]() -> task_type {
        co_return co_await next_impl(state, max_bytes);
    });
}

UdpSocket::task_type UdpSocket::next_impl(const std::shared_ptr<State> &state,
                                          std::size_t max_bytes) {
    if (max_bytes == 0) {
        throw std::runtime_error("udp packet size must be greater than zero");
    }

    if (!state || state->closed || state->closing) {
        co_return std::nullopt;
    }

    UdpSocket socket(state);
    try {
        co_return co_await socket.receive_from(max_bytes);
    } catch (...) {
        if (state->closed || state->closing) {
            co_return std::nullopt;
        }
        throw;
    }
}

Task<SocketAddress> UdpSocket::local_endpoint() {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    if (state_->local_address.valid()) {
        co_return state_->local_address;
    }

    co_return co_await query_socket_address(*state_->runtime,
                                            state_,
                                            "udp socket is closed",
                                            "uv_udp_getsockname",
                                            [](uv_udp_t *handle, sockaddr *address, int *length) {
                                                return uv_udp_getsockname(handle, address, length);
                                            });
}

Task<SocketAddress> UdpSocket::remote_endpoint() {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }
    if (!state_->is_connected) {
        throw std::runtime_error("udp socket is not connected");
    }

    if (state_->connected_endpoint.valid()) {
        co_return state_->connected_endpoint;
    }

    co_return co_await query_socket_address(*state_->runtime,
                                            state_,
                                            "udp socket is closed",
                                            "uv_udp_getpeername",
                                            [](uv_udp_t *handle, sockaddr *address, int *length) {
                                                return uv_udp_getpeername(handle, address, length);
                                            });
}

Task<void> UdpSocket::set_broadcast(bool enable) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "udp socket is closed",
                                 "uv_udp_set_broadcast",
                                 [enable](uv_udp_t *handle) {
                                     return uv_udp_set_broadcast(handle, enable ? 1 : 0);
                                 });
}

Task<void> UdpSocket::set_ttl(int ttl) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "udp socket is closed",
                                 "uv_udp_set_ttl",
                                 [ttl](uv_udp_t *handle) {
                                     return uv_udp_set_ttl(handle, ttl);
                                 });
}

Task<void> UdpSocket::set_multicast_loop(bool enable) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "udp socket is closed",
                                 "uv_udp_set_multicast_loop",
                                 [enable](uv_udp_t *handle) {
                                     return uv_udp_set_multicast_loop(handle, enable ? 1 : 0);
                                 });
}

Task<void> UdpSocket::set_multicast_ttl(int ttl) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    co_await apply_socket_option(*state_->runtime,
                                 state_,
                                 "udp socket is closed",
                                 "uv_udp_set_multicast_ttl",
                                 [ttl](uv_udp_t *handle) {
                                     return uv_udp_set_multicast_ttl(handle, ttl);
                                 });
}

Task<void> UdpSocket::join_multicast_group(std::string multicast_address,
                                           std::string interface_address) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    co_await apply_socket_option(
        *state_->runtime,
        state_,
        "udp socket is closed",
        "uv_udp_set_membership",
        [multicast_address = std::move(multicast_address),
         interface_address = std::move(interface_address)](uv_udp_t *handle) {
            const char *iface = interface_address.empty() ? nullptr : interface_address.c_str();
            return uv_udp_set_membership(handle, multicast_address.c_str(), iface, UV_JOIN_GROUP);
        });
}

Task<void> UdpSocket::leave_multicast_group(std::string multicast_address,
                                            std::string interface_address) {
    if (!state_) {
        throw std::runtime_error("udp socket is empty");
    }

    co_await apply_socket_option(
        *state_->runtime,
        state_,
        "udp socket is closed",
        "uv_udp_set_membership",
        [multicast_address = std::move(multicast_address),
         interface_address = std::move(interface_address)](uv_udp_t *handle) {
            const char *iface = interface_address.empty() ? nullptr : interface_address.c_str();
            return uv_udp_set_membership(handle, multicast_address.c_str(), iface, UV_LEAVE_GROUP);
        });
}

Task<void> UdpSocket::close() {
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

        if (state->pending_receive) {
            auto pending = std::move(state->pending_receive);
            pending->finish_exception(
                std::make_exception_ptr(std::runtime_error("udp socket closed while receiving")));
        }

        state->closing = true;
        state->pending_close = std::make_shared<PendingClose>(PendingClose{std::move(complete)});
        uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
            finish_close(static_cast<UdpSocket::State *>(handle->data));
        });
    });
}

} // namespace async_uv
