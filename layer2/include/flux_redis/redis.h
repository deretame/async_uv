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

#include <stdexec/execution.hpp>

#include "flux/task.h"

namespace flux::redis {

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

using VoidSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using BoolSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(bool),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using ReplySender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(Reply),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using IndexSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;

class Client {
public:
    Client();
    ~Client();

    Client(Client &&) noexcept;
    Client &operator=(Client &&) noexcept;

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    [[nodiscard]] auto open(ConnectionOptions options) {
        return stdexec::just(std::move(options))
             | stdexec::let_value(
                   [this](ConnectionOptions opts) { return this->open_task(std::move(opts)); });
    }
    [[nodiscard]] auto close() {
        return stdexec::just() | stdexec::let_value([this] { return this->close_task(); });
    }
    [[nodiscard]] auto is_open() const {
        return stdexec::just() | stdexec::let_value([this] { return this->is_open_task(); });
    }

    [[nodiscard]] auto command(std::string command_text) {
        return stdexec::just(std::move(command_text))
             | stdexec::let_value([this](std::string c) { return this->command_task(std::move(c)); });
    }
    [[nodiscard]] auto command(std::string command_text, std::vector<RedisParam> params) {
        return stdexec::just(std::move(command_text), std::move(params))
             | stdexec::let_value([this](std::string c, std::vector<RedisParam> p) {
                   return this->command_task(std::move(c), std::move(p));
               });
    }
    [[nodiscard]] auto command(std::string command_text,
                               std::vector<RedisParam> params,
                               CommandOptions options) {
        return stdexec::just(std::move(command_text), std::move(params), std::move(options))
             | stdexec::let_value(
                   [this](std::string c, std::vector<RedisParam> p, CommandOptions o) {
                       return this->command_task(std::move(c), std::move(p), std::move(o));
                   });
    }

private:
    VoidSender open_task(ConnectionOptions options);
    VoidSender close_task();
    BoolSender is_open_task() const;
    ReplySender command_task(std::string command_text);
    ReplySender command_task(std::string command_text, std::vector<RedisParam> params);
    ReplySender command_task(std::string command_text,
                             std::vector<RedisParam> params,
                             CommandOptions options);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

namespace detail {
ReplySender command_task(ConnectionOptions options, std::string command_text);
ReplySender command_task(ConnectionOptions options,
                         std::string command_text,
                         std::vector<RedisParam> params);
ReplySender command_task(ConnectionOptions options,
                         std::string command_text,
                         std::vector<RedisParam> params,
                         CommandOptions command_options);
} // namespace detail

[[nodiscard]] inline auto command(ConnectionOptions options, std::string command_text) {
    return stdexec::just(std::move(options), std::move(command_text))
         | stdexec::let_value(
               [](ConnectionOptions o, std::string c) { return detail::command_task(std::move(o), std::move(c)); });
}

[[nodiscard]] inline auto command(ConnectionOptions options,
                                  std::string command_text,
                                  std::vector<RedisParam> params) {
    return stdexec::just(std::move(options), std::move(command_text), std::move(params))
         | stdexec::let_value([](ConnectionOptions o, std::string c, std::vector<RedisParam> p) {
               return detail::command_task(std::move(o), std::move(c), std::move(p));
           });
}

[[nodiscard]] inline auto command(ConnectionOptions options,
                                  std::string command_text,
                                  std::vector<RedisParam> params,
                                  CommandOptions command_options) {
    return stdexec::just(
               std::move(options), std::move(command_text), std::move(params), std::move(command_options))
         | stdexec::let_value(
               [](ConnectionOptions o, std::string c, std::vector<RedisParam> p, CommandOptions co) {
                   return detail::command_task(std::move(o), std::move(c), std::move(p), std::move(co));
               });
}

class ConnectionPool {
public:
    using CreateSender = exec::any_receiver_ref<
        stdexec::completion_signatures<stdexec::set_value_t(ConnectionPool),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>>::any_sender<>;

    ConnectionPool();
    ~ConnectionPool();

    ConnectionPool(ConnectionPool &&) noexcept;
    ConnectionPool &operator=(ConnectionPool &&) noexcept;

    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    [[nodiscard]] static auto create(ConnectionPoolOptions options) {
        return stdexec::just(std::move(options))
             | stdexec::let_value(
                   [](ConnectionPoolOptions o) { return create_task(std::move(o)); });
    }

    [[nodiscard]] auto command(std::string command_text) {
        return stdexec::just(std::move(command_text))
             | stdexec::let_value([this](std::string c) { return this->command_task(std::move(c)); });
    }
    [[nodiscard]] auto command(std::string command_text, std::vector<RedisParam> params) {
        return stdexec::just(std::move(command_text), std::move(params))
             | stdexec::let_value([this](std::string c, std::vector<RedisParam> p) {
                   return this->command_task(std::move(c), std::move(p));
               });
    }
    [[nodiscard]] auto command(std::string command_text,
                               std::vector<RedisParam> params,
                               CommandOptions options) {
        return stdexec::just(std::move(command_text), std::move(params), std::move(options))
             | stdexec::let_value(
                   [this](std::string c, std::vector<RedisParam> p, CommandOptions o) {
                       return this->command_task(std::move(c), std::move(p), std::move(o));
                   });
    }

    [[nodiscard]] auto close() {
        return stdexec::just() | stdexec::let_value([this] { return this->close_task(); });
    }

private:
    static CreateSender create_task(ConnectionPoolOptions options);
    ReplySender command_task(std::string command_text);
    ReplySender command_task(std::string command_text, std::vector<RedisParam> params);
    ReplySender command_task(std::string command_text,
                             std::vector<RedisParam> params,
                             CommandOptions options);
    VoidSender close_task();

    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace flux::redis
