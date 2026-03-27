#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "async_uv/stream.h"
#include "async_uv/task.h"

namespace async_uv::ws {

enum class MessageType {
    open,
    close,
    text,
    binary,
    error,
    ping,
    pong,
};

struct Message {
    MessageType type = MessageType::text;
    std::string data;
    std::optional<int> close_code;
    std::string reason;

    Message() = default;
    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;
    Message(Message &&) noexcept = default;
    Message &operator=(Message &&) noexcept = default;
};

struct ClientOptions {
    std::string url;
    int connect_timeout_ms = 10000;
    int close_timeout_ms = 5000;
    int ping_interval_seconds = 30;
    bool disable_automatic_reconnection = true;

    bool tls_verify_peer = true;
    std::string tls_ca_file;
    std::string tls_cert_file;
    std::string tls_key_file;
    std::string tls_server_name;

    class Builder {
    public:
        Builder &url(std::string value);
        Builder &connect_timeout_ms(int value);
        Builder &close_timeout_ms(int value);
        Builder &ping_interval_seconds(int value);
        Builder &disable_automatic_reconnection(bool value);
        Builder &tls_verify_peer(bool value);
        Builder &tls_ca_file(std::string value);
        Builder &tls_cert_file(std::string value);
        Builder &tls_key_file(std::string value);
        Builder &tls_server_name(std::string value);
        [[nodiscard]] ClientOptions build() const;

    private:
        std::string url_;
        int connect_timeout_ms_ = 10000;
        int close_timeout_ms_ = 5000;
        int ping_interval_seconds_ = 30;
        bool disable_automatic_reconnection_ = true;
        bool tls_verify_peer_ = true;
        std::string tls_ca_file_;
        std::string tls_cert_file_;
        std::string tls_key_file_;
        std::string tls_server_name_;
    };

    static Builder builder();
};

enum class WsErrorKind {
    invalid_argument,
    runtime_missing,
    connect_failed,
    not_connected,
    send_failed,
    receive_failed,
    internal_error,
};

class WsError : public std::runtime_error {
public:
    WsError(std::string message, WsErrorKind kind)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    WsErrorKind kind() const noexcept {
        return kind_;
    }

private:
    WsErrorKind kind_ = WsErrorKind::internal_error;
};

class Client {
public:
    using stream_type = Stream<Message>;

    Client();
    ~Client();

    Client(Client &&) noexcept;
    Client &operator=(Client &&) noexcept;

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    Task<void> open(ClientOptions options);
    Task<void> close();
    Task<bool> is_open() const;

    Task<void> send_text(std::string text);
    Task<void> send_binary(std::string binary);

    Task<Message> next_message();
    Task<std::optional<Message>> next_message_for(std::chrono::milliseconds timeout);
    stream_type messages() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace async_uv::ws
