#include "async_uv_ws/ws.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#if ASYNC_UV_WS_HAS_IXWEBSOCKET
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#endif

#include "async_uv/cancel.h"
#include "async_uv/message.h"
#include "async_uv/runtime.h"

namespace async_uv::ws {

ClientOptions::Builder ClientOptions::builder() {
    return Builder{};
}

ClientOptions::Builder &ClientOptions::Builder::url(std::string value) {
    url_ = std::move(value);
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::connect_timeout_ms(int value) {
    connect_timeout_ms_ = value;
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::close_timeout_ms(int value) {
    close_timeout_ms_ = value;
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::ping_interval_seconds(int value) {
    ping_interval_seconds_ = value;
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::disable_automatic_reconnection(bool value) {
    disable_automatic_reconnection_ = value;
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::tls_verify_peer(bool value) {
    tls_verify_peer_ = value;
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::tls_ca_file(std::string value) {
    tls_ca_file_ = std::move(value);
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::tls_cert_file(std::string value) {
    tls_cert_file_ = std::move(value);
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::tls_key_file(std::string value) {
    tls_key_file_ = std::move(value);
    return *this;
}

ClientOptions::Builder &ClientOptions::Builder::tls_server_name(std::string value) {
    tls_server_name_ = std::move(value);
    return *this;
}

ClientOptions ClientOptions::Builder::build() const {
    ClientOptions out;
    out.url = url_;
    out.connect_timeout_ms = connect_timeout_ms_;
    out.close_timeout_ms = close_timeout_ms_;
    out.ping_interval_seconds = ping_interval_seconds_;
    out.disable_automatic_reconnection = disable_automatic_reconnection_;
    out.tls_verify_peer = tls_verify_peer_;
    out.tls_ca_file = tls_ca_file_;
    out.tls_cert_file = tls_cert_file_;
    out.tls_key_file = tls_key_file_;
    out.tls_server_name = tls_server_name_;
    return out;
}

class Client::Impl {
public:
    ClientOptions options_{};
    std::atomic_bool opened_{false};

#if ASYNC_UV_WS_HAS_IXWEBSOCKET
    ix::WebSocket socket_;
    std::optional<Mailbox<Message>> mailbox_;
    std::optional<MessageSender<Message>> sender_;
#endif
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() = default;

Client::Client(Client &&) noexcept = default;

Client &Client::operator=(Client &&) noexcept = default;

Task<void> Client::open(ClientOptions options) {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    (void)options;
    throw WsError("ws driver is not enabled", WsErrorKind::invalid_argument);
#else
    const auto started = std::chrono::steady_clock::now();
    async_uv::emit_trace_event({"layer2_ws", "open_start", 0, 0});
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::runtime_missing), 0});
        throw WsError("ws::Client::open requires current runtime", WsErrorKind::runtime_missing);
    }

    if (options.url.empty()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::invalid_argument), 0});
        throw WsError("websocket url cannot be empty", WsErrorKind::invalid_argument);
    }
    if (impl_->opened_.load()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::invalid_argument), 0});
        throw WsError("websocket is already connected", WsErrorKind::invalid_argument);
    }

    static std::once_flag once;
    std::call_once(once, [] {
        ix::initNetSystem();
    });

    impl_->mailbox_ = co_await Mailbox<Message>::create(*runtime);
    impl_->sender_ = impl_->mailbox_->sender();

    impl_->socket_.setUrl(options.url);
    impl_->socket_.setPingInterval(options.ping_interval_seconds);
    if (options.disable_automatic_reconnection) {
        impl_->socket_.disableAutomaticReconnection();
    } else {
        impl_->socket_.enableAutomaticReconnection();
    }

    ix::SocketTLSOptions tls_options;
    tls_options.tls = options.url.rfind("wss://", 0) == 0;
    tls_options.disable_hostname_validation = !options.tls_verify_peer;
    if (!options.tls_verify_peer) {
        tls_options.caFile = "NONE";
    } else if (!options.tls_ca_file.empty()) {
        tls_options.caFile = options.tls_ca_file;
    }
    if (!options.tls_cert_file.empty()) {
        tls_options.certFile = options.tls_cert_file;
    }
    if (!options.tls_key_file.empty()) {
        tls_options.keyFile = options.tls_key_file;
    }
    impl_->socket_.setTLSOptions(tls_options);

    auto sender = *impl_->sender_;
    impl_->socket_.setOnMessageCallback([sender = std::move(sender), impl = impl_.get()](
                                            const ix::WebSocketMessagePtr &msg) mutable {
        Message out;
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                out.type = MessageType::open;
                impl->opened_.store(true);
                break;
            case ix::WebSocketMessageType::Close:
                out.type = MessageType::close;
                out.close_code = msg->closeInfo.code;
                out.reason = msg->closeInfo.reason;
                impl->opened_.store(false);
                break;
            case ix::WebSocketMessageType::Message:
                out.type = msg->binary ? MessageType::binary : MessageType::text;
                out.data = msg->str;
                break;
            case ix::WebSocketMessageType::Error:
                out.type = MessageType::error;
                out.reason = msg->errorInfo.reason;
                impl->opened_.store(false);
                break;
            case ix::WebSocketMessageType::Ping:
                out.type = MessageType::ping;
                out.data = msg->str;
                break;
            case ix::WebSocketMessageType::Pong:
                out.type = MessageType::pong;
                out.data = msg->str;
                break;
            default:
                return;
        }
        (void)sender.try_send(std::move(out));
    });

    impl_->socket_.start();
    impl_->options_ = std::move(options);

    std::optional<Message> first;
    bool open_timed_out = false;
    try {
        first = co_await impl_->mailbox_->recv_for(
            std::chrono::milliseconds(std::max(1, impl_->options_.connect_timeout_ms)));
    } catch (const async_uv::Error &e) {
        if (e.code() == UV_ETIMEDOUT) {
            open_timed_out = true;
        } else {
            throw;
        }
    }

    if (open_timed_out) {
        impl_->socket_.stop(static_cast<uint16_t>(1000), "open timeout");
        if (impl_->mailbox_.has_value()) {
            co_await impl_->mailbox_->close();
            impl_->mailbox_.reset();
        }
        impl_->sender_.reset();
        impl_->opened_.store(false);
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::connect_failed), 0});
        throw WsError("websocket open timed out", WsErrorKind::connect_failed);
    }

    if (!first.has_value()) {
        impl_->socket_.stop(static_cast<uint16_t>(1000), "open failed");
        if (impl_->mailbox_.has_value()) {
            co_await impl_->mailbox_->close();
            impl_->mailbox_.reset();
        }
        impl_->sender_.reset();
        impl_->opened_.store(false);
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::connect_failed), 0});
        throw WsError("websocket open failed", WsErrorKind::connect_failed);
    }
    if (first->type == MessageType::open) {
        impl_->opened_.store(true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", 0, static_cast<std::size_t>(elapsed.count())});
        co_return;
    }
    if (first->type == MessageType::error) {
        impl_->socket_.stop(static_cast<uint16_t>(1000), "open failed");
        if (impl_->mailbox_.has_value()) {
            co_await impl_->mailbox_->close();
            impl_->mailbox_.reset();
        }
        impl_->sender_.reset();
        impl_->opened_.store(false);
        async_uv::emit_trace_event(
            {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::connect_failed), 0});
        throw WsError(first->reason.empty() ? "websocket open failed" : first->reason,
                      WsErrorKind::connect_failed);
    }
    impl_->socket_.stop(static_cast<uint16_t>(1000), "open failed");
    if (impl_->mailbox_.has_value()) {
        co_await impl_->mailbox_->close();
        impl_->mailbox_.reset();
    }
    impl_->sender_.reset();
    impl_->opened_.store(false);
    async_uv::emit_trace_event(
        {"layer2_ws", "open_done", static_cast<int>(WsErrorKind::connect_failed), 0});
    throw WsError("websocket open failed", WsErrorKind::connect_failed);
#endif
}

Task<void> Client::close() {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    co_return;
#else
    const auto started = std::chrono::steady_clock::now();
    async_uv::emit_trace_event({"layer2_ws", "close_start", 0, 0});
    const auto timeout = std::chrono::milliseconds(std::max(1, impl_->options_.close_timeout_ms));
    const bool was_open = impl_->opened_.exchange(false);
    if (was_open) {
        impl_->socket_.stop(static_cast<uint16_t>(1000), "normal closure");
        if (impl_->mailbox_.has_value()) {
            try {
                while (true) {
                    auto item = co_await impl_->mailbox_->recv_for(timeout);
                    if (!item.has_value()) {
                        break;
                    }
                    if (item->type == MessageType::close || item->type == MessageType::error) {
                        break;
                    }
                }
            } catch (const async_uv::Error &e) {
                if (e.code() != UV_ETIMEDOUT) {
                    throw;
                }
            }
        }
    }
    if (impl_->mailbox_.has_value()) {
        try {
            co_await impl_->mailbox_->close_for(timeout);
        } catch (const async_uv::Error &e) {
            if (e.code() != UV_ETIMEDOUT) {
                throw;
            }
        }
        impl_->mailbox_.reset();
    }
    impl_->sender_.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    async_uv::emit_trace_event(
        {"layer2_ws", "close_done", 0, static_cast<std::size_t>(elapsed.count())});
    co_return;
#endif
}

Task<bool> Client::is_open() const {
    co_return impl_->opened_.load();
}

Task<void> Client::send_text(std::string text) {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    (void)text;
    throw WsError("ws driver is not enabled", WsErrorKind::invalid_argument);
#else
    async_uv::emit_trace_event({"layer2_ws", "send_start", 0, text.size()});
    if (!impl_->opened_.load()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "send_done", static_cast<int>(WsErrorKind::not_connected), 0});
        throw WsError("websocket is not connected", WsErrorKind::not_connected);
    }
    const auto info = impl_->socket_.send(text);
    if (!info.success) {
        async_uv::emit_trace_event(
            {"layer2_ws", "send_done", static_cast<int>(WsErrorKind::send_failed), 0});
        throw WsError("websocket send failed", WsErrorKind::send_failed);
    }
    async_uv::emit_trace_event({"layer2_ws", "send_done", 0, info.wireSize});
    co_return;
#endif
}

Task<void> Client::send_binary(std::string binary) {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    (void)binary;
    throw WsError("ws driver is not enabled", WsErrorKind::invalid_argument);
#else
    async_uv::emit_trace_event({"layer2_ws", "send_start", 0, binary.size()});
    if (!impl_->opened_.load()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "send_done", static_cast<int>(WsErrorKind::not_connected), 0});
        throw WsError("websocket is not connected", WsErrorKind::not_connected);
    }
    const auto info = impl_->socket_.sendBinary(binary);
    if (!info.success) {
        async_uv::emit_trace_event(
            {"layer2_ws", "send_done", static_cast<int>(WsErrorKind::send_failed), 0});
        throw WsError("websocket send failed", WsErrorKind::send_failed);
    }
    async_uv::emit_trace_event({"layer2_ws", "send_done", 0, info.wireSize});
    co_return;
#endif
}

Task<Message> Client::next_message() {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    throw WsError("ws driver is not enabled", WsErrorKind::invalid_argument);
#else
    async_uv::emit_trace_event({"layer2_ws", "recv_start", 0, 0});
    if (!impl_->mailbox_.has_value()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "recv_done", static_cast<int>(WsErrorKind::not_connected), 0});
        throw WsError("websocket mailbox is not initialized", WsErrorKind::not_connected);
    }
    auto next = co_await impl_->mailbox_->recv();
    if (!next.has_value()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "recv_done", static_cast<int>(WsErrorKind::receive_failed), 0});
        throw WsError("websocket stream is closed", WsErrorKind::receive_failed);
    }
    async_uv::emit_trace_event({"layer2_ws", "recv_done", 0, next->data.size()});
    co_return std::move(*next);
#endif
}

Task<std::optional<Message>> Client::next_message_for(std::chrono::milliseconds timeout) {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    (void)timeout;
    throw WsError("ws driver is not enabled", WsErrorKind::invalid_argument);
#else
    async_uv::emit_trace_event({"layer2_ws", "recv_start", 0, 0});
    if (!impl_->mailbox_.has_value()) {
        async_uv::emit_trace_event(
            {"layer2_ws", "recv_done", static_cast<int>(WsErrorKind::not_connected), 0});
        throw WsError("websocket mailbox is not initialized", WsErrorKind::not_connected);
    }
    try {
        auto out = co_await impl_->mailbox_->recv_for(timeout);
        async_uv::emit_trace_event(
            {"layer2_ws", "recv_done", 0, out.has_value() ? out->data.size() : 0});
        co_return out;
    } catch (const async_uv::Error &e) {
        if (e.code() == UV_ETIMEDOUT) {
            async_uv::emit_trace_event({"layer2_ws", "recv_done", UV_ETIMEDOUT, 0});
            co_return std::nullopt;
        }
        throw;
    }
#endif
}

Client::stream_type Client::messages() const {
#if !ASYNC_UV_WS_HAS_IXWEBSOCKET
    throw WsError("ws driver is not enabled", WsErrorKind::invalid_argument);
#else
    if (!impl_->mailbox_.has_value()) {
        throw WsError("websocket mailbox is not initialized", WsErrorKind::not_connected);
    }
    return impl_->mailbox_->messages();
#endif
}

} // namespace async_uv::ws
