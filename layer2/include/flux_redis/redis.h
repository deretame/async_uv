#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "async_uv/task.h"

namespace async_uv::redis {

class RedisParam {
public:
    RedisParam() = default;
    RedisParam(std::nullptr_t);
    RedisParam(const char *value);
    RedisParam(std::string value);
    RedisParam(std::string_view value);
    RedisParam(std::optional<std::string> value);
    RedisParam(bool value);
    RedisParam(std::int32_t value);
    RedisParam(std::uint32_t value);
    RedisParam(std::int64_t value);
    RedisParam(std::uint64_t value);
    RedisParam(double value);

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] std::string as_text() const;

private:
    std::optional<std::string> value_{};
};

struct CommandOptions {
    int timeout_ms = 0;

    class Builder {
    public:
        Builder &timeout_ms(int value);
        [[nodiscard]] CommandOptions build() const;

    private:
        int timeout_ms_ = 0;
    };

    static Builder builder();
};

struct ConnectionOptions {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string user;
    std::string password;
    int db = 0;
    int connect_timeout_ms = 0;
    int command_timeout_ms = 0;
    bool tls_enabled = false;
    bool tls_verify_peer = true;
    std::string tls_ca_cert_file;
    std::string tls_ca_cert_dir;
    std::string tls_cert_file;
    std::string tls_key_file;
    std::string tls_server_name;

    class Builder {
    public:
        Builder &host(std::string value);
        Builder &port(int value);
        Builder &user(std::string value);
        Builder &password(std::string value);
        Builder &db(int value);
        Builder &connect_timeout_ms(int value);
        Builder &command_timeout_ms(int value);
        Builder &tls_enabled(bool value);
        Builder &tls_verify_peer(bool value);
        Builder &tls_ca_cert_file(std::string value);
        Builder &tls_ca_cert_dir(std::string value);
        Builder &tls_cert_file(std::string value);
        Builder &tls_key_file(std::string value);
        Builder &tls_server_name(std::string value);
        [[nodiscard]] ConnectionOptions build() const;

    private:
        std::string host_ = "127.0.0.1";
        int port_ = 6379;
        std::string user_;
        std::string password_;
        int db_ = 0;
        int connect_timeout_ms_ = 0;
        int command_timeout_ms_ = 0;
        bool tls_enabled_ = false;
        bool tls_verify_peer_ = true;
        std::string tls_ca_cert_file_;
        std::string tls_ca_cert_dir_;
        std::string tls_cert_file_;
        std::string tls_key_file_;
        std::string tls_server_name_;
    };

    static Builder builder();
};

struct ConnectionPoolOptions {
    ConnectionOptions connection;
    std::size_t max_connections = 4;
    bool preconnect = true;
    int acquire_timeout_ms = 30000;
    int max_lifetime_ms = 0;
    std::string health_check_command = "PING";

    class Builder {
    public:
        Builder &connection(ConnectionOptions value);
        Builder &max_connections(std::size_t value);
        Builder &preconnect(bool value);
        Builder &acquire_timeout_ms(int value);
        Builder &max_lifetime_ms(int value);
        Builder &health_check_command(std::string value);
        [[nodiscard]] ConnectionPoolOptions build() const;

    private:
        ConnectionOptions connection_{};
        std::size_t max_connections_ = 4;
        bool preconnect_ = true;
        int acquire_timeout_ms_ = 30000;
        int max_lifetime_ms_ = 0;
        std::string health_check_command_ = "PING";
    };

    static Builder builder();
};

enum class RedisErrorKind {
    invalid_argument,
    runtime_missing,
    connect_failed,
    not_connected,
    command_failed,
    internal_error,
};

class RedisError : public std::runtime_error {
public:
    RedisError(std::string message, RedisErrorKind kind)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    RedisErrorKind kind() const noexcept {
        return kind_;
    }

private:
    RedisErrorKind kind_ = RedisErrorKind::internal_error;
};

struct Reply {
    enum class Type {
        null_value,
        string,
        integer,
        array,
        status,
        error,
    };

    Type type = Type::null_value;
    std::optional<std::string> string;
    std::int64_t integer = 0;
    std::vector<Reply> elements;
};

class Client {
public:
    Client();
    ~Client();

    Client(Client &&) noexcept;
    Client &operator=(Client &&) noexcept;

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    Task<void> open(ConnectionOptions options);
    Task<void> close();
    Task<bool> is_open() const;

    Task<Reply> command(std::string command);
    Task<Reply> command(std::string command, std::vector<RedisParam> params);
    Task<Reply>
    command(std::string command, std::vector<RedisParam> params, CommandOptions options);

    Task<Reply> execute(std::string command);
    Task<Reply> execute(std::string command, std::vector<RedisParam> params);
    Task<Reply>
    execute(std::string command, std::vector<RedisParam> params, CommandOptions options);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

Task<Reply> command(ConnectionOptions options, std::string command);
Task<Reply> command(ConnectionOptions options, std::string command, std::vector<RedisParam> params);
Task<Reply> command(ConnectionOptions options,
                    std::string command,
                    std::vector<RedisParam> params,
                    CommandOptions command_options);

class ConnectionPool {
public:
    ConnectionPool();
    ~ConnectionPool();

    ConnectionPool(ConnectionPool &&) noexcept;
    ConnectionPool &operator=(ConnectionPool &&) noexcept;

    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    static Task<ConnectionPool> create(ConnectionPoolOptions options);

    Task<Reply> command(std::string command);
    Task<Reply> command(std::string command, std::vector<RedisParam> params);
    Task<Reply>
    command(std::string command, std::vector<RedisParam> params, CommandOptions options);

    Task<Reply> execute(std::string command);
    Task<Reply> execute(std::string command, std::vector<RedisParam> params);
    Task<Reply>
    execute(std::string command, std::vector<RedisParam> params, CommandOptions options);

    Task<void> close();

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace async_uv::redis
