#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <stdexec/execution.hpp>

#include "flux/task.h"

namespace flux::sql {

enum class Driver {
    postgres,
    mysql,
    sqlite,
};

enum class PostgresSslMode {
    disable,
    allow,
    prefer,
    require,
    verify_ca,
    verify_full,
};

class SqlParam {
public:
    enum class Type {
        null,
        text,
        int64,
        float64,
        boolean,
    };

    SqlParam() = default;
    SqlParam(std::nullptr_t);
    SqlParam(const char *value);
    SqlParam(std::string value);
    SqlParam(std::string_view value);
    SqlParam(std::optional<std::string> value);
    SqlParam(bool value);
    SqlParam(std::int32_t value);
    SqlParam(std::uint32_t value);
    SqlParam(std::int64_t value);
    SqlParam(std::uint64_t value);
    SqlParam(double value);

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] std::string as_text() const;

private:
    std::variant<std::monostate, std::string, std::int64_t, double, bool> value_{};
};

struct QueryOptions {
    int timeout_ms = 0;

    class Builder {
    public:
        Builder &timeout_ms(int value);
        [[nodiscard]] QueryOptions build() const;

    private:
        int timeout_ms_ = 0;
    };

    static Builder builder();
};

struct ConnectionOptions {
    Driver driver = Driver::sqlite;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string user;
    std::string password;
    std::string database;
    std::string file;
    int connect_timeout_ms = 0;
    int query_timeout_ms = 0;

    PostgresSslMode postgres_ssl_mode = PostgresSslMode::prefer;
    std::string postgres_ssl_root_cert_file;
    std::string postgres_ssl_cert_file;
    std::string postgres_ssl_key_file;
    std::string postgres_ssl_crl_file;
    std::string postgres_ssl_min_protocol;

    class Builder {
    public:
        Builder &driver(Driver value);
        Builder &host(std::string value);
        Builder &port(int value);
        Builder &user(std::string value);
        Builder &password(std::string value);
        Builder &database(std::string value);
        Builder &file(std::string value);
        Builder &connect_timeout_ms(int value);
        Builder &query_timeout_ms(int value);
        Builder &postgres_ssl_mode(PostgresSslMode value);
        Builder &postgres_ssl_root_cert_file(std::string value);
        Builder &postgres_ssl_cert_file(std::string value);
        Builder &postgres_ssl_key_file(std::string value);
        Builder &postgres_ssl_crl_file(std::string value);
        Builder &postgres_ssl_min_protocol(std::string value);
        [[nodiscard]] ConnectionOptions build() const;

    private:
        Driver driver_ = Driver::sqlite;
        std::string host_ = "127.0.0.1";
        int port_ = 0;
        std::string user_;
        std::string password_;
        std::string database_;
        std::string file_;
        int connect_timeout_ms_ = 0;
        int query_timeout_ms_ = 0;
        PostgresSslMode postgres_ssl_mode_ = PostgresSslMode::prefer;
        std::string postgres_ssl_root_cert_file_;
        std::string postgres_ssl_cert_file_;
        std::string postgres_ssl_key_file_;
        std::string postgres_ssl_crl_file_;
        std::string postgres_ssl_min_protocol_;
    };

    static Builder builder();
};

enum class SqlErrorKind {
    invalid_argument,
    runtime_missing,
    connect_failed,
    not_connected,
    query_failed,
    internal_error,
};

class SqlError : public std::runtime_error {
public:
    SqlError(std::string message, SqlErrorKind kind)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    SqlErrorKind kind() const noexcept {
        return kind_;
    }

private:
    SqlErrorKind kind_ = SqlErrorKind::internal_error;
};

using Cell = std::optional<std::string>;

struct Row {
    std::vector<Cell> values;
};

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<Row> rows;
    std::uint64_t affected_rows = 0;
    std::uint64_t last_insert_id = 0;
};

using VoidSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using BoolSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(bool),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using QuerySender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(QueryResult),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using IndexSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(std::size_t),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;

class Connection {
public:
    Connection();
    ~Connection();

    Connection(Connection &&) noexcept;
    Connection &operator=(Connection &&) noexcept;

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

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
    ConnectionOptions options() const;
    [[nodiscard]] auto cancel() {
        return stdexec::just() | stdexec::let_value([this] { return this->cancel_task(); });
    }

    [[nodiscard]] auto query(std::string sql) {
        return stdexec::just(std::move(sql)) | stdexec::let_value([this](std::string q) {
                   return this->query_task(std::move(q));
               });
    }
    [[nodiscard]] auto query(std::string sql, std::vector<SqlParam> params) {
        return stdexec::just(std::move(sql), std::move(params))
             | stdexec::let_value([this](std::string q, std::vector<SqlParam> p) {
                   return this->query_task(std::move(q), std::move(p));
               });
    }
    [[nodiscard]] auto query(std::string sql, QueryOptions options) {
        return stdexec::just(std::move(sql), std::move(options))
             | stdexec::let_value([this](std::string q, QueryOptions o) {
                   return this->query_task(std::move(q), std::move(o));
               });
    }
    [[nodiscard]] auto query(std::string sql,
                             std::vector<SqlParam> params,
                             QueryOptions options) {
        return stdexec::just(std::move(sql), std::move(params), std::move(options))
             | stdexec::let_value(
                   [this](std::string q, std::vector<SqlParam> p, QueryOptions o) {
                       return this->query_task(std::move(q), std::move(p), std::move(o));
                   });
    }

    [[nodiscard]] auto begin() {
        return stdexec::just() | stdexec::let_value([this] { return this->begin_task(); });
    }
    [[nodiscard]] auto commit() {
        return stdexec::just() | stdexec::let_value([this] { return this->commit_task(); });
    }
    [[nodiscard]] auto rollback() {
        return stdexec::just() | stdexec::let_value([this] { return this->rollback_task(); });
    }

private:
    VoidSender open_task(ConnectionOptions options);
    VoidSender close_task();
    BoolSender is_open_task() const;
    VoidSender cancel_task();

    QuerySender query_task(std::string sql);
    QuerySender query_task(std::string sql, std::vector<SqlParam> params);
    QuerySender query_task(std::string sql, QueryOptions options);
    QuerySender query_task(std::string sql,
                           std::vector<SqlParam> params,
                           QueryOptions options);

    VoidSender begin_task();
    VoidSender commit_task();
    VoidSender rollback_task();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

namespace detail {
QuerySender query_task(ConnectionOptions options, std::string sql);
QuerySender query_task(ConnectionOptions options,
                       std::string sql,
                       std::vector<SqlParam> params);
QuerySender query_task(ConnectionOptions options,
                       std::string sql,
                       std::vector<SqlParam> params,
                       QueryOptions query_options);
} // namespace detail

[[nodiscard]] inline auto query(ConnectionOptions options, std::string sql) {
    return stdexec::just(std::move(options), std::move(sql))
         | stdexec::let_value([](ConnectionOptions o, std::string q) {
               return detail::query_task(std::move(o), std::move(q));
           });
}
[[nodiscard]] inline auto query(ConnectionOptions options,
                                std::string sql,
                                std::vector<SqlParam> params) {
    return stdexec::just(std::move(options), std::move(sql), std::move(params))
         | stdexec::let_value([](ConnectionOptions o, std::string q, std::vector<SqlParam> p) {
               return detail::query_task(std::move(o), std::move(q), std::move(p));
           });
}
[[nodiscard]] inline auto query(ConnectionOptions options,
                                std::string sql,
                                std::vector<SqlParam> params,
                                QueryOptions query_options) {
    return stdexec::just(
               std::move(options), std::move(sql), std::move(params), std::move(query_options))
         | stdexec::let_value(
               [](ConnectionOptions o, std::string q, std::vector<SqlParam> p, QueryOptions qo) {
                   return detail::query_task(std::move(o), std::move(q), std::move(p), std::move(qo));
               });
}

struct ConnectionPoolOptions {
    ConnectionOptions connection;
    std::size_t max_connections = 4;
    bool preconnect = true;
    int acquire_timeout_ms = 30000;
    int max_lifetime_ms = 0;
    std::string health_check_sql = "SELECT 1";

    class Builder {
    public:
        Builder &connection(ConnectionOptions value);
        Builder &max_connections(std::size_t value);
        Builder &preconnect(bool value);
        Builder &acquire_timeout_ms(int value);
        Builder &max_lifetime_ms(int value);
        Builder &health_check_sql(std::string value);
        [[nodiscard]] ConnectionPoolOptions build() const;

    private:
        ConnectionOptions connection_{};
        std::size_t max_connections_ = 4;
        bool preconnect_ = true;
        int acquire_timeout_ms_ = 30000;
        int max_lifetime_ms_ = 0;
        std::string health_check_sql_ = "SELECT 1";
    };

    static Builder builder();
};

class ConnectionPool {
public:
    class PooledConnection;
    using CreateSender = exec::any_receiver_ref<
        stdexec::completion_signatures<stdexec::set_value_t(ConnectionPool),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>>::any_sender<>;
    using PooledConnectionSender = exec::any_receiver_ref<
        stdexec::completion_signatures<stdexec::set_value_t(PooledConnection),
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
                   [](ConnectionPoolOptions opts) { return create_task(std::move(opts)); });
    }

    [[nodiscard]] auto acquire();
    [[nodiscard]] auto query(std::string sql) {
        return stdexec::just(std::move(sql)) | stdexec::let_value([this](std::string q) {
                   return this->query_task(std::move(q));
               });
    }
    [[nodiscard]] auto query(std::string sql, std::vector<SqlParam> params) {
        return stdexec::just(std::move(sql), std::move(params))
             | stdexec::let_value([this](std::string q, std::vector<SqlParam> p) {
                   return this->query_task(std::move(q), std::move(p));
               });
    }
    [[nodiscard]] auto query(std::string sql,
                             std::vector<SqlParam> params,
                             QueryOptions options) {
        return stdexec::just(std::move(sql), std::move(params), std::move(options))
             | stdexec::let_value(
                   [this](std::string q, std::vector<SqlParam> p, QueryOptions o) {
                       return this->query_task(std::move(q), std::move(p), std::move(o));
                   });
    }
    [[nodiscard]] auto close() {
        return stdexec::just() | stdexec::let_value([this] { return this->close_task(); });
    }

private:
    static CreateSender create_task(ConnectionPoolOptions options);

    PooledConnectionSender acquire_task();
    QuerySender query_task(std::string sql);
    QuerySender query_task(std::string sql, std::vector<SqlParam> params);
    QuerySender query_task(std::string sql,
                           std::vector<SqlParam> params,
                           QueryOptions options);
    VoidSender close_task();

    class Impl;
    std::shared_ptr<Impl> impl_;
};

class ConnectionPool::PooledConnection {
public:
    PooledConnection() = default;
    ~PooledConnection();

    PooledConnection(const PooledConnection &) = delete;
    PooledConnection &operator=(const PooledConnection &) = delete;
    PooledConnection(PooledConnection &&other) noexcept;
    PooledConnection &operator=(PooledConnection &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void release() noexcept;

    [[nodiscard]] auto query(std::string sql) {
        return stdexec::just(std::move(sql)) | stdexec::let_value([this](std::string q) {
                   return this->query_task(std::move(q));
               });
    }
    [[nodiscard]] auto query(std::string sql, std::vector<SqlParam> params) {
        return stdexec::just(std::move(sql), std::move(params))
             | stdexec::let_value([this](std::string q, std::vector<SqlParam> p) {
                   return this->query_task(std::move(q), std::move(p));
               });
    }
    [[nodiscard]] auto query(std::string sql, QueryOptions options) {
        return stdexec::just(std::move(sql), std::move(options))
             | stdexec::let_value([this](std::string q, QueryOptions o) {
                   return this->query_task(std::move(q), std::move(o));
               });
    }
    [[nodiscard]] auto query(std::string sql,
                             std::vector<SqlParam> params,
                             QueryOptions options) {
        return stdexec::just(std::move(sql), std::move(params), std::move(options))
             | stdexec::let_value(
                   [this](std::string q, std::vector<SqlParam> p, QueryOptions o) {
                       return this->query_task(std::move(q), std::move(p), std::move(o));
                   });
    }
    [[nodiscard]] auto begin() {
        return stdexec::just() | stdexec::let_value([this] { return this->begin_task(); });
    }
    [[nodiscard]] auto commit() {
        return stdexec::just() | stdexec::let_value([this] { return this->commit_task(); });
    }
    [[nodiscard]] auto rollback() {
        return stdexec::just() | stdexec::let_value([this] { return this->rollback_task(); });
    }

private:
    friend class ConnectionPool;

    QuerySender query_task(std::string sql);
    QuerySender query_task(std::string sql, std::vector<SqlParam> params);
    QuerySender query_task(std::string sql, QueryOptions options);
    QuerySender query_task(std::string sql,
                           std::vector<SqlParam> params,
                           QueryOptions options);

    VoidSender begin_task();
    VoidSender commit_task();
    VoidSender rollback_task();

    explicit PooledConnection(std::shared_ptr<ConnectionPool::Impl> impl, std::size_t index) noexcept;

    std::shared_ptr<ConnectionPool::Impl> impl_;
    std::size_t index_ = 0;
    bool held_ = false;
};

inline auto ConnectionPool::acquire() {
    return stdexec::just() | stdexec::let_value([this] { return this->acquire_task(); });
}

} // namespace flux::sql
