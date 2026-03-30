#include "async_uv_ws/ws.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <libwebsockets.h>
#include <uv.h>

namespace async_uv::ws {
namespace {

template <typename T, typename Fn>
Task<T> post_to_runtime(Runtime &runtime, Fn &&fn) {
    auto promise = std::make_shared<async_simple::Promise<T>>();
    auto future = promise->getFuture().via(&runtime);
    auto fn_holder = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));

    runtime.post([promise, fn_holder]() mutable {
        try {
            if constexpr (std::is_void_v<T>) {
                (*fn_holder)();
                promise->setValue();
            } else {
                promise->setValue((*fn_holder)());
            }
        } catch (...) {
            promise->setException(std::current_exception());
        }
    });

    if constexpr (std::is_void_v<T>) {
        co_await std::move(future);
        co_return;
    } else {
        co_return co_await std::move(future);
    }
}

std::pair<int, std::string> parse_close_payload(const void *in, size_t len) {
    if (in == nullptr || len < 2) {
        return {0, {}};
    }

    const auto *bytes = static_cast<const unsigned char *>(in);
    const int code = (static_cast<int>(bytes[0]) << 8) | static_cast<int>(bytes[1]);
    std::string reason;
    if (len > 2) {
        reason.assign(reinterpret_cast<const char *>(bytes + 2), len - 2);
    }
    return {code, std::move(reason)};
}

int stream_write_flags(const StreamChunk &chunk) {
    int flags = 0;
    if (chunk.is_first_fragment) {
        flags = chunk.type == MessageType::binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
    } else {
        flags = LWS_WRITE_CONTINUATION;
    }
    if (!chunk.is_final_fragment) {
        flags |= LWS_WRITE_NO_FIN;
    }
    return flags;
}

template <typename T>
struct AsyncQueue {
    Runtime *runtime = nullptr;
    std::deque<T> pending;
    std::deque<std::shared_ptr<async_simple::Promise<std::optional<T>>>> waiters;
    bool closed = false;

    explicit AsyncQueue(Runtime *rt) : runtime(rt) {}

    void push(T value) {
        if (!waiters.empty()) {
            auto waiter = std::move(waiters.front());
            waiters.pop_front();
            waiter->setValue(std::optional<T>(std::move(value)));
            return;
        }
        pending.push_back(std::move(value));
    }

    void close() {
        if (closed) {
            return;
        }
        closed = true;
        while (!waiters.empty()) {
            auto waiter = std::move(waiters.front());
            waiters.pop_front();
            waiter->setValue(std::optional<T>{});
        }
        pending.clear();
    }

    Task<std::optional<T>> next() {
        auto promise = std::make_shared<async_simple::Promise<std::optional<T>>>();
        auto future = promise->getFuture().via(runtime);

        runtime->post([this, promise]() mutable {
            if (!pending.empty()) {
                auto value = std::move(pending.front());
                pending.pop_front();
                promise->setValue(std::optional<T>(std::move(value)));
                return;
            }

            if (closed) {
                promise->setValue(std::optional<T>{});
                return;
            }

            waiters.push_back(std::move(promise));
        });

        co_return co_await std::move(future);
    }
};

template <typename StateT>
void close_service_timer(StateT &state) {
    if (!state.service_timer_initialized || state.service_timer_closing) {
        return;
    }

    uv_timer_stop(&state.service_timer);
    if (uv_is_closing(reinterpret_cast<uv_handle_t *>(&state.service_timer)) != 0) {
        return;
    }

    state.service_timer_closing = true;
    state.timer_keepalive = state.shared_from_this();
    uv_close(reinterpret_cast<uv_handle_t *>(&state.service_timer), [](uv_handle_t *handle) {
        auto *self = static_cast<StateT *>(handle->data);
        if (self == nullptr) {
            return;
        }
        self->service_timer_initialized = false;
        self->service_timer_closing = false;
        self->timer_keepalive.reset();
    });
}

} // namespace

struct Server::State : std::enable_shared_from_this<Server::State> {
    struct PendingWrite {
        std::vector<unsigned char> payload;
        int flags = 0;
    };

    struct Connection {
        std::uint64_t id = 0;
        lws *wsi = nullptr;
        std::deque<PendingWrite> writes;
        bool close_pending = false;
        int close_code = 1000;
        std::string close_reason;
        int peer_close_code = 0;
        std::string peer_close_reason;
    };

    struct PerSessionData {
        std::uint64_t connection_id = 0;
    };

    explicit State(Config init_config)
        : config(std::move(init_config)), event_queue(config.runtime) {}

    static State *from_wsi(lws *wsi) {
        if (wsi == nullptr) {
            return nullptr;
        }
        if (const auto *protocol = lws_get_protocol(wsi); protocol != nullptr && protocol->user) {
            return static_cast<State *>(protocol->user);
        }
        return nullptr;
    }

    bool queue_write(std::uint64_t connection_id, StreamChunk chunk) {
        const auto it = connections_by_id.find(connection_id);
        if (it == connections_by_id.end()) {
            return false;
        }

        auto pending = PendingWrite{};
        pending.flags = stream_write_flags(chunk);
        pending.payload.resize(LWS_PRE + chunk.payload.size());
        if (!chunk.payload.empty()) {
            std::memcpy(
                pending.payload.data() + LWS_PRE, chunk.payload.data(), chunk.payload.size());
        }

        auto &connection = *it->second;
        connection.writes.push_back(std::move(pending));
        lws_callback_on_writable(connection.wsi);
        if (auto *ctx = lws_get_context(connection.wsi); ctx != nullptr) {
            lws_cancel_service(ctx);
        }
        return true;
    }

    static int callback(lws *wsi, lws_callback_reasons reason, void *user, void *in, size_t len) {
        if (reason == LWS_CALLBACK_PROTOCOL_INIT || reason == LWS_CALLBACK_PROTOCOL_DESTROY) {
            return 0;
        }
        if (reason == LWS_CALLBACK_GET_THREAD_ID) {
#if defined(_WIN32)
            return static_cast<int>(::GetCurrentThreadId());
#else
            return 0;
#endif
        }

        State *state = from_wsi(wsi);
        if (state == nullptr) {
            return 0;
        }

        auto *pss = static_cast<PerSessionData *>(user);

        switch (reason) {
            case LWS_CALLBACK_ESTABLISHED: {
                auto connection = std::make_shared<Connection>();
                connection->id = state->next_connection_id++;
                connection->wsi = wsi;
                if (pss != nullptr) {
                    pss->connection_id = connection->id;
                }

                state->connections_by_id[connection->id] = connection;
                state->connections_by_wsi[wsi] = connection;

                if (state->bound_port < 0) {
                    if (auto *vh = lws_get_vhost(wsi)) {
                        state->bound_port = lws_get_vhost_listen_port(vh);
                    }
                }

                ServerEvent event;
                event.kind = ServerEvent::Kind::open;
                event.connection_id = connection->id;
                state->event_queue.push(std::move(event));
                break;
            }
            case LWS_CALLBACK_RECEIVE: {
                const auto it = state->connections_by_wsi.find(wsi);
                if (it == state->connections_by_wsi.end()) {
                    break;
                }

                ServerEvent event;
                event.kind = ServerEvent::Kind::message;
                event.connection_id = it->second->id;

                Message msg;
                msg.type = lws_frame_is_binary(wsi) ? MessageType::binary : MessageType::text;
                if (in != nullptr && len > 0) {
                    msg.payload.assign(static_cast<const char *>(in), len);
                }
                msg.is_first_fragment = lws_is_first_fragment(wsi) != 0;
                msg.is_final_fragment = lws_is_final_fragment(wsi) != 0;
                event.message = std::move(msg);
                state->event_queue.push(std::move(event));
                break;
            }
            case LWS_CALLBACK_SERVER_WRITEABLE: {
                const auto it = state->connections_by_wsi.find(wsi);
                if (it == state->connections_by_wsi.end()) {
                    break;
                }

                auto &conn = *it->second;
                if (conn.close_pending) {
                    lws_close_reason(wsi,
                                     static_cast<lws_close_status>(conn.close_code),
                                     reinterpret_cast<unsigned char *>(conn.close_reason.data()),
                                     conn.close_reason.size());
                    return -1;
                }

                if (!conn.writes.empty()) {
                    auto &pending = conn.writes.front();
                    const int payload_size = static_cast<int>(pending.payload.size() - LWS_PRE);
                    const int written = lws_write(wsi,
                                                  pending.payload.data() + LWS_PRE,
                                                  static_cast<unsigned int>(payload_size),
                                                  static_cast<lws_write_protocol>(pending.flags));
                    if (written != payload_size) {
                        return -1;
                    }
                    conn.writes.pop_front();
                    if (!conn.writes.empty()) {
                        lws_callback_on_writable(wsi);
                    }
                }
                break;
            }
            case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
                const auto it = state->connections_by_wsi.find(wsi);
                if (it == state->connections_by_wsi.end()) {
                    break;
                }
                auto [code, reason_text] = parse_close_payload(in, len);
                it->second->peer_close_code = code;
                it->second->peer_close_reason = std::move(reason_text);
                break;
            }
            case LWS_CALLBACK_CLOSED: {
                const auto it = state->connections_by_wsi.find(wsi);
                if (it == state->connections_by_wsi.end()) {
                    break;
                }
                auto conn = it->second;
                state->connections_by_wsi.erase(it);
                state->connections_by_id.erase(conn->id);

                ServerEvent event;
                event.kind = ServerEvent::Kind::close;
                event.connection_id = conn->id;
                event.close_code = conn->peer_close_code;
                event.close_reason = conn->peer_close_reason;
                state->event_queue.push(std::move(event));
                break;
            }
            default:
                break;
        }

        return 0;
    }

    Config config;
    lws_context *context = nullptr;
    std::array<lws_protocols, 2> protocols{};
    std::array<void *, 1> foreign_loops{};
    uv_timer_t service_timer{};
    bool service_timer_initialized = false;
    bool service_timer_closing = false;
    std::shared_ptr<State> timer_keepalive;

    std::atomic<bool> is_started{false};
    std::atomic<int> bound_port{-1};
    std::uint64_t next_connection_id = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections_by_id;
    std::unordered_map<lws *, std::shared_ptr<Connection>> connections_by_wsi;
    AsyncQueue<ServerEvent> event_queue;
};

struct Client::State : std::enable_shared_from_this<Client::State> {
    struct PendingWrite {
        std::vector<unsigned char> payload;
        int flags = 0;
    };

    struct PerSessionData {
        int peer_close_code = 0;
        std::string peer_close_reason;
    };

    explicit State(Config init_config)
        : config(std::move(init_config)), event_queue(config.runtime) {}

    static State *from_wsi(lws *wsi) {
        if (wsi == nullptr) {
            return nullptr;
        }
        if (const auto *protocol = lws_get_protocol(wsi); protocol != nullptr && protocol->user) {
            return static_cast<State *>(protocol->user);
        }
        return nullptr;
    }

    bool queue_write(StreamChunk chunk) {
        if (!is_connected.load() || active_wsi == nullptr) {
            return false;
        }

        PendingWrite pending;
        pending.flags = stream_write_flags(chunk);
        pending.payload.resize(LWS_PRE + chunk.payload.size());
        if (!chunk.payload.empty()) {
            std::memcpy(
                pending.payload.data() + LWS_PRE, chunk.payload.data(), chunk.payload.size());
        }

        writes.push_back(std::move(pending));
        lws_callback_on_writable(active_wsi);
        if (auto *ctx = lws_get_context(active_wsi); ctx != nullptr) {
            lws_cancel_service(ctx);
        }
        return true;
    }

    void finish_connect_success() {
        if (auto promise = std::move(connect_promise); promise) {
            promise->setValue();
        }
    }

    void finish_connect_error(std::string message) {
        if (auto promise = std::move(connect_promise); promise) {
            promise->setException(std::make_exception_ptr(WebSocketError(std::move(message))));
        }
    }

    void ensure_context() {
        if (context != nullptr) {
            return;
        }

        protocols[0] = lws_protocols{
            "async_uv_layer2_ws",
            +[](lws *wsi, lws_callback_reasons reason, void *user, void *in, size_t len) -> int {
                return State::callback(wsi, reason, user, in, len);
            },
            sizeof(PerSessionData),
            0,
            0,
            this,
            0,
        };
        protocols[1] = lws_protocols{nullptr, nullptr, 0, 0, 0, nullptr, 0};

        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols.data();
        info.pvo = nullptr;
        info.timeout_secs = 1;
        info.options = LWS_SERVER_OPTION_VALIDATE_UTF8 | LWS_SERVER_OPTION_LIBUV;
        foreign_loops[0] = config.runtime->loop();
        info.foreign_loops = foreign_loops.data();
        if (config.use_tls) {
#if defined(LWS_WITH_TLS)
            info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            if (!config.tls_ca_file.empty()) {
                info.client_ssl_ca_filepath = config.tls_ca_file.c_str();
            }
#endif
        }
        info.fd_limit_per_thread = 4;
        info.user = this;

        context = lws_create_context(&info);
        if (context == nullptr) {
            throw WebSocketError("libwebsockets client context creation failed");
        }
    }

    static int callback(lws *wsi, lws_callback_reasons reason, void *user, void *in, size_t len) {
        if (reason == LWS_CALLBACK_PROTOCOL_INIT || reason == LWS_CALLBACK_PROTOCOL_DESTROY) {
            return 0;
        }
        if (reason == LWS_CALLBACK_GET_THREAD_ID) {
#if defined(_WIN32)
            return static_cast<int>(::GetCurrentThreadId());
#else
            return 0;
#endif
        }

        auto *state = from_wsi(wsi);
        if (state == nullptr) {
            return 0;
        }
        auto *pss = static_cast<PerSessionData *>(user);

        switch (reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED: {
                state->active_wsi = wsi;
                state->is_connected.store(true);

                Client::Event event;
                event.kind = Client::Event::Kind::open;
                state->event_queue.push(std::move(event));
                state->finish_connect_success();
                break;
            }
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
                state->active_wsi = nullptr;
                state->is_connected.store(false);

                std::string message =
                    in ? static_cast<const char *>(in) : "websocket client connection error";
                Client::Event event;
                event.kind = Client::Event::Kind::error;
                event.error = message;
                state->event_queue.push(std::move(event));
                state->finish_connect_error(message);
                break;
            }
            case LWS_CALLBACK_CLIENT_RECEIVE: {
                Client::Event event;
                event.kind = Client::Event::Kind::message;
                Message msg;
                msg.type = lws_frame_is_binary(wsi) ? MessageType::binary : MessageType::text;
                if (in != nullptr && len > 0) {
                    msg.payload.assign(static_cast<const char *>(in), len);
                }
                msg.is_first_fragment = lws_is_first_fragment(wsi) != 0;
                msg.is_final_fragment = lws_is_final_fragment(wsi) != 0;
                event.message = std::move(msg);
                state->event_queue.push(std::move(event));
                break;
            }
            case LWS_CALLBACK_CLIENT_WRITEABLE: {
                if (state->close_pending) {
                    lws_close_reason(wsi,
                                     static_cast<lws_close_status>(state->close_code),
                                     reinterpret_cast<unsigned char *>(state->close_reason.data()),
                                     state->close_reason.size());
                    return -1;
                }

                if (!state->writes.empty()) {
                    auto &pending = state->writes.front();
                    const int payload_size = static_cast<int>(pending.payload.size() - LWS_PRE);
                    const int written = lws_write(wsi,
                                                  pending.payload.data() + LWS_PRE,
                                                  static_cast<unsigned int>(payload_size),
                                                  static_cast<lws_write_protocol>(pending.flags));
                    if (written != payload_size) {
                        return -1;
                    }
                    state->writes.pop_front();
                    if (!state->writes.empty()) {
                        lws_callback_on_writable(wsi);
                    }
                }
                break;
            }
            case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
                if (pss != nullptr) {
                    auto [code, reason_text] = parse_close_payload(in, len);
                    pss->peer_close_code = code;
                    pss->peer_close_reason = std::move(reason_text);
                }
                break;
            }
            case LWS_CALLBACK_CLOSED: {
                state->active_wsi = nullptr;
                state->is_connected.store(false);
                state->close_pending = false;
                state->writes.clear();

                Client::Event event;
                event.kind = Client::Event::Kind::close;
                if (pss != nullptr) {
                    event.close_code = pss->peer_close_code;
                    event.close_reason = pss->peer_close_reason;
                }
                state->event_queue.push(std::move(event));

                state->finish_connect_error("connection closed");
                break;
            }
            default:
                break;
        }

        return 0;
    }

    Config config;
    lws_context *context = nullptr;
    lws *active_wsi = nullptr;
    std::array<lws_protocols, 2> protocols{};
    std::array<void *, 1> foreign_loops{};
    uv_timer_t service_timer{};
    bool service_timer_initialized = false;
    bool service_timer_closing = false;
    std::shared_ptr<State> timer_keepalive;

    std::atomic<bool> is_connected{false};
    bool close_pending = false;
    int close_code = 1000;
    std::string close_reason;
    std::deque<PendingWrite> writes;
    std::shared_ptr<async_simple::Promise<void>> connect_promise;
    AsyncQueue<Client::Event> event_queue;
};

Server::Server() = default;
Server::Server(std::shared_ptr<State> state) : state_(std::move(state)) {}
Server::Server(Server &&other) noexcept : state_(std::move(other.state_)) {}

Server &Server::operator=(Server &&other) noexcept {
    state_ = std::move(other.state_);
    return *this;
}

Server::~Server() {
    if (!state_ || state_->config.runtime == nullptr) {
        return;
    }
    auto state = state_;
    state->config.runtime->post([state]() {
        if (state->service_timer_initialized) {
            close_service_timer(*state);
        }
        state->connections_by_id.clear();
        state->connections_by_wsi.clear();
        if (state->context != nullptr) {
            lws_context_destroy(state->context);
            state->context = nullptr;
        }
        state->bound_port.store(-1);
        state->is_started.store(false);
        state->event_queue.close();
    });
}

Server Server::create(Config config) {
    if (config.runtime == nullptr) {
        throw WebSocketError("websocket server requires a valid async_uv::Runtime");
    }
    if (config.use_tls) {
#if !defined(LWS_WITH_TLS)
        throw WebSocketError("websocket server TLS requested but libwebsockets TLS is unavailable");
#else
        if (config.tls_cert_file.empty()) {
            throw WebSocketError("websocket server TLS requires tls_cert_file");
        }
        if (config.tls_private_key_file.empty()) {
            throw WebSocketError("websocket server TLS requires tls_private_key_file");
        }
#endif
    }
    return Server(std::make_shared<State>(std::move(config)));
}

Task<void> Server::start() {
    if (!state_) {
        throw WebSocketError("websocket server is not initialized");
    }
    auto state = state_;
    auto &runtime = *state->config.runtime;

    co_await post_to_runtime<void>(runtime, [state]() {
        if (state->is_started.load()) {
            return;
        }

        state->protocols[0] = lws_protocols{
            "async_uv_layer2_ws",
            +[](lws *wsi, lws_callback_reasons reason, void *user, void *in, size_t len) -> int {
                return State::callback(wsi, reason, user, in, len);
            },
            sizeof(State::PerSessionData),
            0,
            0,
            state.get(),
            0};
        state->protocols[1] = lws_protocols{nullptr, nullptr, 0, 0, 0, nullptr, 0};

        lws_context_creation_info info{};
        info.port = state->config.port;
#ifdef _WIN32
        info.iface = state->config.host.empty() ? nullptr : state->config.host.c_str();
#else
        info.iface = nullptr;
#endif
        info.protocols = state->protocols.data();
        info.timeout_secs = 1;
        info.options = LWS_SERVER_OPTION_VALIDATE_UTF8 | LWS_SERVER_OPTION_LIBUV;
        state->foreign_loops[0] = state->config.runtime->loop();
        info.foreign_loops = state->foreign_loops.data();
        if (state->config.use_tls) {
#if defined(LWS_WITH_TLS)
            info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            info.ssl_cert_filepath = state->config.tls_cert_file.c_str();
            info.ssl_private_key_filepath = state->config.tls_private_key_file.c_str();
            if (!state->config.tls_ca_file.empty()) {
                info.ssl_ca_filepath = state->config.tls_ca_file.c_str();
            }
#endif
        }
        info.user = state.get();
        info.pt_serv_buf_size = static_cast<unsigned int>(std::max<std::size_t>(
            static_cast<std::size_t>(state->config.max_payload_length), 4096));

        state->context = lws_create_context(&info);
        if (state->context == nullptr) {
            throw WebSocketError("libwebsockets context creation failed");
        }

        if (state->config.port != 0) {
            state->bound_port.store(state->config.port);
        } else {
            auto *vh = lws_get_vhost_by_name(state->context, "default");
            state->bound_port.store(vh ? lws_get_vhost_listen_port(vh) : -1);
        }

        state->is_started.store(true);
    });
}

Task<void> Server::stop() {
    if (!state_ || state_->config.runtime == nullptr) {
        co_return;
    }

    auto state = state_;
    auto &runtime = *state->config.runtime;
    co_await post_to_runtime<void>(runtime, [state]() {
        if (!state->is_started.load()) {
            return;
        }
        if (state->service_timer_initialized) {
            close_service_timer(*state);
        }
        state->connections_by_id.clear();
        state->connections_by_wsi.clear();
        if (state->context != nullptr) {
            lws_context_destroy(state->context);
            state->context = nullptr;
        }
        state->bound_port.store(-1);
        state->is_started.store(false);
        state->event_queue.close();
    });
}

int Server::port() const noexcept {
    return state_ ? state_->bound_port.load() : -1;
}

bool Server::started() const noexcept {
    return state_ && state_->is_started.load();
}

Server::task_type Server::next() {
    if (!state_ || state_->config.runtime == nullptr) {
        throw WebSocketError("websocket server is not initialized");
    }
    co_return co_await state_->event_queue.next();
}

Server::stream_type Server::events() {
    auto state = state_;
    if (!state || state->config.runtime == nullptr) {
        return {};
    }
    return stream_type([state]() -> task_type {
        co_return co_await state->event_queue.next();
    });
}

Task<bool> Server::send_text(std::uint64_t connection_id, std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::text;
    chunk.payload = std::string(payload);
    co_return co_await send_stream(connection_id, std::move(chunk));
}

Task<bool> Server::send_binary(std::uint64_t connection_id, std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::binary;
    chunk.payload = std::string(payload);
    co_return co_await send_stream(connection_id, std::move(chunk));
}

Task<bool> Server::send_stream(std::uint64_t connection_id, StreamChunk chunk) {
    if (!state_ || state_->config.runtime == nullptr) {
        co_return false;
    }
    auto state = state_;
    auto &runtime = *state->config.runtime;
    co_return co_await post_to_runtime<bool>(
        runtime, [state, connection_id, chunk = std::move(chunk)]() mutable {
            if (!state->is_started.load()) {
                return false;
            }
            return state->queue_write(connection_id, std::move(chunk));
        });
}

Task<bool> Server::close(std::uint64_t connection_id, int code, std::string reason) {
    if (!state_ || state_->config.runtime == nullptr) {
        co_return false;
    }
    auto state = state_;
    auto &runtime = *state->config.runtime;
    co_return co_await post_to_runtime<bool>(
        runtime, [state, connection_id, code, reason = std::move(reason)]() mutable {
            if (!state->is_started.load()) {
                return false;
            }
            const auto it = state->connections_by_id.find(connection_id);
            if (it == state->connections_by_id.end()) {
                return false;
            }
            it->second->close_pending = true;
            it->second->close_code = code;
            it->second->close_reason = std::move(reason);
            lws_callback_on_writable(it->second->wsi);
            if (auto *ctx = lws_get_context(it->second->wsi); ctx != nullptr) {
                lws_cancel_service(ctx);
            }
            return true;
        });
}

Client::Client() = default;
Client::Client(std::shared_ptr<State> state) : state_(std::move(state)) {}
Client::Client(Client &&other) noexcept : state_(std::move(other.state_)) {}

Client &Client::operator=(Client &&other) noexcept {
    state_ = std::move(other.state_);
    return *this;
}

Client::~Client() {
    if (!state_ || state_->config.runtime == nullptr) {
        return;
    }
    auto state = state_;
    state->config.runtime->post([state]() {
        if (state->service_timer_initialized) {
            close_service_timer(*state);
        }
        state->writes.clear();
        state->active_wsi = nullptr;
        state->is_connected.store(false);
        if (state->context != nullptr) {
            lws_context_destroy(state->context);
            state->context = nullptr;
        }
        state->event_queue.close();
        state->finish_connect_error("websocket client destroyed");
    });
}

Client Client::create(Config config) {
    if (config.runtime == nullptr) {
        throw WebSocketError("websocket client requires a valid async_uv::Runtime");
    }
    if (config.port <= 0 || config.port > 65535) {
        throw WebSocketError("websocket client requires a valid destination port");
    }
#if !defined(LWS_WITH_TLS)
    if (config.use_tls) {
        throw WebSocketError("websocket client TLS requested but libwebsockets TLS is unavailable");
    }
#endif
    if (config.use_tls && config.host.empty()) {
        config.host = config.address;
    }
    return Client(std::make_shared<State>(std::move(config)));
}

Task<void> Client::connect() {
    if (!state_ || state_->config.runtime == nullptr) {
        throw WebSocketError("websocket client is not initialized");
    }

    auto state = state_;
    auto &runtime = *state->config.runtime;
    auto connect_promise = std::make_shared<async_simple::Promise<void>>();
    auto connect_future = connect_promise->getFuture().via(&runtime);

    co_await post_to_runtime<void>(runtime, [state, connect_promise]() mutable {
        state->ensure_context();

        if (state->is_connected.load()) {
            connect_promise->setValue();
            return;
        }
        if (state->connect_promise) {
            connect_promise->setException(std::make_exception_ptr(
                WebSocketError("websocket client connect already in progress")));
            return;
        }

        state->connect_promise = connect_promise;

        lws_client_connect_info info{};
        info.context = state->context;
        info.address = state->config.address.c_str();
        info.port = state->config.port;
        info.path = state->config.path.c_str();
        info.host =
            state->config.host.empty() ? state->config.address.c_str() : state->config.host.c_str();
        info.origin = state->config.origin.empty() ? info.host : state->config.origin.c_str();
        info.protocol = state->protocols[0].name;
        info.local_protocol_name = state->protocols[0].name;
        info.ietf_version_or_minus_one = -1;
        info.pwsi = &state->active_wsi;
        info.vhost = lws_get_vhost_by_name(state->context, "default");
        if (state->config.use_tls) {
#if defined(LWS_WITH_TLS)
            info.ssl_connection = LCCSCF_USE_SSL;
            if (state->config.tls_allow_insecure) {
                info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED;
                info.ssl_connection |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
                info.ssl_connection |= LCCSCF_ALLOW_INSECURE;
            }
#endif
        }

        if (!lws_client_connect_via_info(&info)) {
            state->finish_connect_error("libwebsockets client connect creation failed");
            Client::Event event;
            event.kind = Client::Event::Kind::error;
            event.error = "libwebsockets client connect creation failed";
            state->event_queue.push(std::move(event));
        }
    });

    co_await std::move(connect_future);
}

Task<void> Client::close(int code, std::string reason) {
    if (!state_ || state_->config.runtime == nullptr) {
        co_return;
    }
    auto state = state_;
    auto &runtime = *state->config.runtime;
    co_await post_to_runtime<void>(runtime, [state, code, reason = std::move(reason)]() mutable {
        if (!state->is_connected.load() || state->active_wsi == nullptr) {
            return;
        }
        state->close_pending = true;
        state->close_code = code;
        state->close_reason = std::move(reason);
        lws_callback_on_writable(state->active_wsi);
        if (auto *ctx = lws_get_context(state->active_wsi); ctx != nullptr) {
            lws_cancel_service(ctx);
        }
    });
}

bool Client::connected() const noexcept {
    return state_ && state_->is_connected.load();
}

Client::task_type Client::next() {
    if (!state_ || state_->config.runtime == nullptr) {
        throw WebSocketError("websocket client is not initialized");
    }
    co_return co_await state_->event_queue.next();
}

Client::stream_type Client::events() {
    auto state = state_;
    if (!state || state->config.runtime == nullptr) {
        return {};
    }
    return stream_type([state]() -> task_type {
        co_return co_await state->event_queue.next();
    });
}

Task<bool> Client::send_text(std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::text;
    chunk.payload = std::string(payload);
    co_return co_await send_stream(std::move(chunk));
}

Task<bool> Client::send_binary(std::string_view payload) {
    StreamChunk chunk;
    chunk.type = MessageType::binary;
    chunk.payload = std::string(payload);
    co_return co_await send_stream(std::move(chunk));
}

Task<bool> Client::send_stream(StreamChunk chunk) {
    if (!state_ || state_->config.runtime == nullptr) {
        co_return false;
    }
    auto state = state_;
    auto &runtime = *state_->config.runtime;
    co_return co_await post_to_runtime<bool>(runtime, [state, chunk = std::move(chunk)]() mutable {
        return state->queue_write(std::move(chunk));
    });
}

} // namespace async_uv::ws
