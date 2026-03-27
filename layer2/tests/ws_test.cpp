#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if ASYNC_UV_WS_HAS_IXWEBSOCKET
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <ixwebsocket/IXWebSocketServer.h>
#endif

#include "async_uv/async_uv.h"
#include "async_uv_layer2/to_string.h"
#include "async_uv_ws/ws.h"

namespace {

async_uv::Task<void> run_connect_failed_check() {
    async_uv::ws::Client client;

    auto options = async_uv::ws::ClientOptions::builder()
                       .url("ws://127.0.0.1:1")
                       .connect_timeout_ms(300)
                       .build();

    bool failed = false;
    try {
        co_await client.open(options);
    } catch (const async_uv::ws::WsError &e) {
        failed = true;
#if ASYNC_UV_WS_HAS_IXWEBSOCKET
        assert(e.kind() == async_uv::ws::WsErrorKind::connect_failed);
#else
        assert(e.kind() == async_uv::ws::WsErrorKind::invalid_argument);
#endif
    }
    assert(failed);

    assert(async_uv::layer2::to_string(async_uv::layer2::ErrorKind::timeout) == "timeout");
    assert(async_uv::layer2::to_string(async_uv::ws::MessageType::text) == "text");
    async_uv::ws::Message sample;
    sample.type = async_uv::ws::MessageType::text;
    sample.data = "abc";
    const auto text = async_uv::layer2::to_string(sample);
    assert(text.find("type=text") != std::string::npos);
    assert(text.find("data_size=3") != std::string::npos);
    co_return;
}

#if ASYNC_UV_WS_HAS_IXWEBSOCKET

#ifndef ASYNC_UV_WS_TEST_CERT_DIR
#define ASYNC_UV_WS_TEST_CERT_DIR ""
#endif

#ifndef ASYNC_UV_WS_TEST_TRUSTED_CA
#define ASYNC_UV_WS_TEST_TRUSTED_CA ""
#endif

#ifndef ASYNC_UV_WS_TEST_SERVER_CERT
#define ASYNC_UV_WS_TEST_SERVER_CERT ""
#endif

#ifndef ASYNC_UV_WS_TEST_SERVER_KEY
#define ASYNC_UV_WS_TEST_SERVER_KEY ""
#endif

class EchoServer {
public:
    explicit EchoServer(bool tls) : port_(ix::getFreePort()), server_(port_, "127.0.0.1") {
        if (tls) {
            ix::SocketTLSOptions tls_options;
            tls_options.tls = true;
            tls_options.caFile = "NONE";
            tls_options.certFile = ASYNC_UV_WS_TEST_SERVER_CERT;
            tls_options.keyFile = ASYNC_UV_WS_TEST_SERVER_KEY;
            server_.setTLSOptions(tls_options);
        }

        server_.setOnClientMessageCallback([](std::shared_ptr<ix::ConnectionState>,
                                              ix::WebSocket &socket,
                                              const ix::WebSocketMessagePtr &msg) {
            if (msg->type == ix::WebSocketMessageType::Message) {
                (void)socket.send(msg->str, msg->binary);
            }
        });

        const auto result = server_.listen();
        if (!result.first) {
            throw std::runtime_error("failed to listen websocket test server: " + result.second);
        }
        server_.start();
    }

    ~EchoServer() {
        server_.stop();
    }

    [[nodiscard]] std::string url(bool tls) const {
        return std::string(tls ? "wss://localhost:" : "ws://127.0.0.1:") + std::to_string(port_);
    }

private:
    int port_;
    ix::WebSocketServer server_;
};

async_uv::Task<async_uv::ws::Message> wait_for_message_type(async_uv::ws::Client &client,
                                                            async_uv::ws::MessageType expected,
                                                            std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto next = co_await client.next_message_for(remaining);
        if (!next.has_value()) {
            continue;
        }

        auto message = std::move(*next);
        if (message.type == expected) {
            co_return std::move(message);
        }
        if (message.type == async_uv::ws::MessageType::error) {
            throw std::runtime_error("unexpected websocket error message: " + message.reason);
        }
    }

    throw std::runtime_error("timed out while waiting for websocket message");
}

async_uv::Task<void> run_echo_checks(const std::string &url) {
    async_uv::ws::Client client;
    auto options = async_uv::ws::ClientOptions::builder()
                       .url(url)
                       .connect_timeout_ms(2000)
                       .close_timeout_ms(1000)
                       .ping_interval_seconds(30)
                       .disable_automatic_reconnection(true)
                       .build();

    co_await client.open(options);
    assert(co_await client.is_open());

    co_await client.send_text("hello async_uv ws");
    auto text_message = co_await wait_for_message_type(
        client, async_uv::ws::MessageType::text, std::chrono::milliseconds(2000));
    assert(text_message.data == "hello async_uv ws");

    std::string binary_payload;
    binary_payload.push_back(static_cast<char>(0x01));
    binary_payload.push_back(static_cast<char>(0x7f));
    binary_payload.push_back(static_cast<char>(0x00));
    co_await client.send_binary(binary_payload);

    auto binary_message = co_await wait_for_message_type(
        client, async_uv::ws::MessageType::binary, std::chrono::milliseconds(2000));
    assert(binary_message.data == binary_payload);

    co_await client.close();
    assert(!(co_await client.is_open()));

    bool send_failed = false;
    try {
        co_await client.send_text("after-close");
    } catch (const async_uv::ws::WsError &e) {
        send_failed = true;
        assert(e.kind() == async_uv::ws::WsErrorKind::not_connected);
    }
    assert(send_failed);

    bool receive_failed = false;
    try {
        (void)co_await client.next_message_for(std::chrono::milliseconds(100));
    } catch (const async_uv::ws::WsError &e) {
        receive_failed = true;
        assert(e.kind() == async_uv::ws::WsErrorKind::not_connected);
    }
    assert(receive_failed);
    co_return;
}

async_uv::Task<void> run_stream_receive_checks(const std::string &url) {
    async_uv::ws::Client client;
    auto options = async_uv::ws::ClientOptions::builder()
                       .url(url)
                       .connect_timeout_ms(2000)
                       .close_timeout_ms(1000)
                       .build();

    co_await client.open(options);
    auto stream = client.messages();

    co_await client.send_text("stream-msg-1");
    co_await client.send_text("stream-msg-2");

    std::vector<std::string> got;
    while (auto next = co_await stream.next()) {
        auto message = std::move(*next);
        if (message.type != async_uv::ws::MessageType::text) {
            continue;
        }

        got.push_back(message.data);
        if (got.size() == 2) {
            break;
        }
    }

    assert(got[0] == "stream-msg-1");
    assert(got[1] == "stream-msg-2");

    co_await client.close();

    auto end = co_await stream.next();
    assert(!end.has_value());
    co_return;
}

async_uv::Task<void> run_wss_checks() {
    const auto cert_dir = std::filesystem::path(ASYNC_UV_WS_TEST_CERT_DIR);
    const auto trusted_ca = std::filesystem::path(ASYNC_UV_WS_TEST_TRUSTED_CA);
    const auto server_cert = std::filesystem::path(ASYNC_UV_WS_TEST_SERVER_CERT);
    const auto server_key = std::filesystem::path(ASYNC_UV_WS_TEST_SERVER_KEY);
    assert(!cert_dir.empty());
    assert(std::filesystem::exists(trusted_ca));
    assert(std::filesystem::exists(server_cert));
    assert(std::filesystem::exists(server_key));

    EchoServer tls_server(true);
    const auto tls_url = tls_server.url(true);

    {
        async_uv::ws::Client client;
        auto options = async_uv::ws::ClientOptions::builder()
                           .url(tls_url)
                           .connect_timeout_ms(1500)
                           .tls_verify_peer(true)
                           .tls_ca_file(trusted_ca.string())
                           .build();

        bool failed = false;
        try {
            co_await client.open(options);
        } catch (const async_uv::ws::WsError &e) {
            failed = true;
            assert(e.kind() == async_uv::ws::WsErrorKind::connect_failed);
        }
        assert(failed);
    }

    {
        async_uv::ws::Client client;
        auto options = async_uv::ws::ClientOptions::builder()
                           .url(tls_url)
                           .connect_timeout_ms(3000)
                           .close_timeout_ms(1000)
                           .tls_verify_peer(false)
                           .build();

        co_await client.open(options);
        co_await client.send_text("wss-insecure");
        auto echoed = co_await wait_for_message_type(
            client, async_uv::ws::MessageType::text, std::chrono::milliseconds(2000));
        assert(echoed.data == "wss-insecure");
        co_await client.close();
    }

    co_return;
}

#endif

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().name("async_uv_ws_test"));

#if ASYNC_UV_WS_HAS_IXWEBSOCKET
    static std::once_flag once;
    std::call_once(once, [] {
        ix::initNetSystem();
    });

    EchoServer server(false);
    runtime.block_on(run_connect_failed_check());
    runtime.block_on(run_echo_checks(server.url(false)));
    runtime.block_on(run_stream_receive_checks(server.url(false)));
    runtime.block_on(run_wss_checks());
#else
    runtime.block_on(run_connect_failed_check());
#endif

    return 0;
}
