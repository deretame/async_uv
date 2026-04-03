#include "async_uv_sql/sql.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if ASYNC_UV_SQL_HAS_LIBPQ
#include <libpq-fe.h>
#endif

#if ASYNC_UV_SQL_HAS_MYSQL && __has_include(<mysql.h>)
#include <mysql.h>
#elif ASYNC_UV_SQL_HAS_MYSQL && __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#elif ASYNC_UV_SQL_HAS_MYSQL && __has_include(<mariadb/mysql.h>)
#include <mariadb/mysql.h>
#elif ASYNC_UV_SQL_HAS_MYSQL
#error "MySQL client header not found"
#endif

#if ASYNC_UV_SQL_HAS_SQLITE3
#include <sqlite3.h>
#endif

#include "async_uv/fd.h"
#include "async_uv/cancel.h"
#include "async_uv/error.h"
#include "async_uv/runtime.h"

namespace async_uv::sql {

SqlParam::SqlParam(std::nullptr_t) : value_(std::monostate{}) {}

SqlParam::SqlParam(const char *value)
    : value_(value == nullptr
                 ? std::variant<std::monostate, std::string, std::int64_t, double, bool>(
                       std::monostate{})
                 : std::variant<std::monostate, std::string, std::int64_t, double, bool>(
                       std::string(value))) {}

SqlParam::SqlParam(std::string value) : value_(std::move(value)) {}

SqlParam::SqlParam(std::string_view value) : value_(std::string(value)) {}

SqlParam::SqlParam(std::optional<std::string> value)
    : value_(value.has_value()
                 ? std::variant<std::monostate, std::string, std::int64_t, double, bool>(
                       std::move(*value))
                 : std::variant<std::monostate, std::string, std::int64_t, double, bool>(
                       std::monostate{})) {}

SqlParam::SqlParam(bool value) : value_(value) {}

SqlParam::SqlParam(std::int32_t value) : value_(static_cast<std::int64_t>(value)) {}

SqlParam::SqlParam(std::uint32_t value) : value_(static_cast<std::int64_t>(value)) {}

SqlParam::SqlParam(std::int64_t value) : value_(value) {}

SqlParam::SqlParam(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw SqlError("SqlParam uint64 value exceeds int64 range", SqlErrorKind::invalid_argument);
    }
    value_ = static_cast<std::int64_t>(value);
}

SqlParam::SqlParam(double value) : value_(value) {}

SqlParam::Type SqlParam::type() const noexcept {
    if (std::holds_alternative<std::monostate>(value_)) {
        return Type::null;
    }
    if (std::holds_alternative<std::string>(value_)) {
        return Type::text;
    }
    if (std::holds_alternative<std::int64_t>(value_)) {
        return Type::int64;
    }
    if (std::holds_alternative<double>(value_)) {
        return Type::float64;
    }
    return Type::boolean;
}

bool SqlParam::is_null() const noexcept {
    return std::holds_alternative<std::monostate>(value_);
}

std::string SqlParam::as_text() const {
    switch (type()) {
        case Type::null:
            return "";
        case Type::text:
            return std::get<std::string>(value_);
        case Type::int64:
            return std::to_string(std::get<std::int64_t>(value_));
        case Type::float64: {
            std::ostringstream out;
            out << std::get<double>(value_);
            return out.str();
        }
        case Type::boolean:
            return std::get<bool>(value_) ? "1" : "0";
    }
    return "";
}

QueryOptions::Builder QueryOptions::builder() {
    return Builder{};
}

QueryOptions::Builder &QueryOptions::Builder::timeout_ms(int value) {
    timeout_ms_ = value;
    return *this;
}

QueryOptions QueryOptions::Builder::build() const {
    QueryOptions out;
    out.timeout_ms = timeout_ms_;
    return out;
}

ConnectionOptions::Builder ConnectionOptions::builder() {
    return Builder{};
}

ConnectionOptions::Builder &ConnectionOptions::Builder::driver(Driver value) {
    driver_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::host(std::string value) {
    host_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::port(int value) {
    port_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::user(std::string value) {
    user_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::password(std::string value) {
    password_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::database(std::string value) {
    database_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::file(std::string value) {
    file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::connect_timeout_ms(int value) {
    connect_timeout_ms_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::query_timeout_ms(int value) {
    query_timeout_ms_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::postgres_ssl_mode(PostgresSslMode value) {
    postgres_ssl_mode_ = value;
    return *this;
}

ConnectionOptions::Builder &
ConnectionOptions::Builder::postgres_ssl_root_cert_file(std::string value) {
    postgres_ssl_root_cert_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::postgres_ssl_cert_file(std::string value) {
    postgres_ssl_cert_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::postgres_ssl_key_file(std::string value) {
    postgres_ssl_key_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::postgres_ssl_crl_file(std::string value) {
    postgres_ssl_crl_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &
ConnectionOptions::Builder::postgres_ssl_min_protocol(std::string value) {
    postgres_ssl_min_protocol_ = std::move(value);
    return *this;
}

ConnectionOptions ConnectionOptions::Builder::build() const {
    ConnectionOptions out;
    out.driver = driver_;
    out.host = host_;
    out.port = port_;
    out.user = user_;
    out.password = password_;
    out.database = database_;
    out.file = file_;
    out.connect_timeout_ms = connect_timeout_ms_;
    out.query_timeout_ms = query_timeout_ms_;
    out.postgres_ssl_mode = postgres_ssl_mode_;
    out.postgres_ssl_root_cert_file = postgres_ssl_root_cert_file_;
    out.postgres_ssl_cert_file = postgres_ssl_cert_file_;
    out.postgres_ssl_key_file = postgres_ssl_key_file_;
    out.postgres_ssl_crl_file = postgres_ssl_crl_file_;
    out.postgres_ssl_min_protocol = postgres_ssl_min_protocol_;
    return out;
}

ConnectionPoolOptions::Builder ConnectionPoolOptions::builder() {
    return Builder{};
}

ConnectionPoolOptions::Builder &
ConnectionPoolOptions::Builder::connection(ConnectionOptions value) {
    connection_ = std::move(value);
    return *this;
}

ConnectionPoolOptions::Builder &ConnectionPoolOptions::Builder::max_connections(std::size_t value) {
    max_connections_ = value;
    return *this;
}

ConnectionPoolOptions::Builder &ConnectionPoolOptions::Builder::preconnect(bool value) {
    preconnect_ = value;
    return *this;
}

ConnectionPoolOptions::Builder &ConnectionPoolOptions::Builder::acquire_timeout_ms(int value) {
    acquire_timeout_ms_ = value;
    return *this;
}

ConnectionPoolOptions::Builder &ConnectionPoolOptions::Builder::max_lifetime_ms(int value) {
    max_lifetime_ms_ = value;
    return *this;
}

ConnectionPoolOptions::Builder &
ConnectionPoolOptions::Builder::health_check_sql(std::string value) {
    health_check_sql_ = std::move(value);
    return *this;
}

ConnectionPoolOptions ConnectionPoolOptions::Builder::build() const {
    ConnectionPoolOptions out;
    out.connection = connection_;
    out.max_connections = max_connections_;
    out.preconnect = preconnect_;
    out.acquire_timeout_ms = acquire_timeout_ms_;
    out.max_lifetime_ms = max_lifetime_ms_;
    out.health_check_sql = health_check_sql_;
    return out;
}

namespace {

class OperationGuard {
public:
    explicit OperationGuard(std::atomic_bool &flag) : flag_(flag) {
        bool expected = false;
        if (!flag_.compare_exchange_strong(expected, true)) {
            throw SqlError("sql connection is busy with another operation",
                           SqlErrorKind::internal_error);
        }
    }

    ~OperationGuard() {
        flag_.store(false);
    }

private:
    std::atomic_bool &flag_;
};

std::uint64_t parse_u64(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    std::uint64_t value = 0;
    for (const unsigned char ch : text) {
        if (!std::isdigit(ch)) {
            return 0;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

#if ASYNC_UV_SQL_HAS_LIBPQ
std::string postgres_ssl_mode_to_string(PostgresSslMode mode) {
    switch (mode) {
        case PostgresSslMode::disable:
            return "disable";
        case PostgresSslMode::allow:
            return "allow";
        case PostgresSslMode::prefer:
            return "prefer";
        case PostgresSslMode::require:
            return "require";
        case PostgresSslMode::verify_ca:
            return "verify-ca";
        case PostgresSslMode::verify_full:
            return "verify-full";
    }
    return "prefer";
}

std::string pg_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\'' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string rewrite_qmark_to_dollar(std::string_view sql) {
    std::string out;
    out.reserve(sql.size() + 16);

    bool in_single = false;
    bool in_double = false;
    bool escape = false;
    int index = 1;

    for (const char ch : sql) {
        if (escape) {
            out.push_back(ch);
            escape = false;
            continue;
        }

        if (ch == '\\' && in_single) {
            out.push_back(ch);
            escape = true;
            continue;
        }

        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            out.push_back(ch);
            continue;
        }

        if (ch == '"' && !in_single) {
            in_double = !in_double;
            out.push_back(ch);
            continue;
        }

        if (ch == '?' && !in_single && !in_double) {
            out.push_back('$');
            out += std::to_string(index++);
            continue;
        }

        out.push_back(ch);
    }

    return out;
}

std::size_t count_placeholders(std::string_view sql) {
    bool in_single = false;
    bool in_double = false;
    bool in_backtick = false;
    bool escape = false;
    std::size_t count = 0;

    for (const char ch : sql) {
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\' && (in_single || in_double)) {
            escape = true;
            continue;
        }
        if (ch == '\'' && !in_double && !in_backtick) {
            in_single = !in_single;
            continue;
        }
        if (ch == '"' && !in_single && !in_backtick) {
            in_double = !in_double;
            continue;
        }
        if (ch == '`' && !in_single && !in_double) {
            in_backtick = !in_backtick;
            continue;
        }
        if (ch == '?' && !in_single && !in_double && !in_backtick) {
            ++count;
        }
    }
    return count;
}
#endif

#if ASYNC_UV_SQL_HAS_MYSQL
int mysql_socket_fd(MYSQL *mysql) {
    return static_cast<int>(mysql->net.fd);
}
#endif

std::optional<int> choose_timeout_ms(const ConnectionOptions &connection_options,
                                     const QueryOptions &query_options) {
    if (query_options.timeout_ms > 0) {
        return query_options.timeout_ms;
    }
    if (connection_options.query_timeout_ms > 0) {
        return connection_options.query_timeout_ms;
    }
    return std::nullopt;
}

template <typename Fn>
Task<QueryResult> run_with_query_timeout(const ConnectionOptions &connection_options,
                                         const QueryOptions &query_options,
                                         Fn &&fn) {
    const auto timeout_ms = choose_timeout_ms(connection_options, query_options);
    try {
        if (timeout_ms.has_value()) {
            co_return co_await async_uv::with_timeout(std::chrono::milliseconds(*timeout_ms),
                                                      std::forward<Fn>(fn)());
        }
        co_return co_await std::forward<Fn>(fn)();
    } catch (const async_uv::Error &e) {
        if (e.code() == UV_ETIMEDOUT) {
            throw SqlError("sql query timeout", SqlErrorKind::query_failed);
        }
        throw;
    }
}

std::string default_sqlite_file(const ConnectionOptions &options) {
    return options.file.empty() ? ":memory:" : options.file;
}

int default_port(const ConnectionOptions &options) {
    if (options.port > 0) {
        return options.port;
    }
    switch (options.driver) {
        case Driver::postgres:
            return 5432;
        case Driver::mysql:
            return 3306;
        case Driver::sqlite:
            return 0;
    }
    return 0;
}

Task<void> safe_stop_watcher(FdWatcher &watcher) {
    try {
        co_await watcher.stop();
    } catch (...) {
    }
    co_return;
}

Task<void>
wait_fd_ready(uv_os_sock_t fd, bool readable, bool writable, std::optional<int> timeout_ms) {
    if (fd < 0) {
        throw SqlError("invalid database socket fd", SqlErrorKind::internal_error);
    }

    int events = 0;
    if (readable) {
        events |= UV_READABLE;
    }
    if (writable) {
        events |= UV_WRITABLE;
    }
    if (events == 0) {
        co_return;
    }

    auto watcher = co_await FdWatcher::watch(fd, events);
    std::optional<FdEvent> event;

    if (timeout_ms.has_value()) {
        event = co_await watcher.next_for(std::chrono::milliseconds(*timeout_ms));
    } else {
        event = co_await watcher.next();
    }

    co_await safe_stop_watcher(watcher);

    if (!event.has_value()) {
        throw SqlError("database socket wait timeout", SqlErrorKind::query_failed);
    }
    if (!event->ok()) {
        throw SqlError("database socket wait returned error", SqlErrorKind::query_failed);
    }
}

#if ASYNC_UV_SQL_HAS_MYSQL
void ensure_mysql_library_initialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        (void)mysql_library_init(0, nullptr, nullptr);
    });
}
#endif

} // namespace

class Connection::Impl {
public:
    bool is_open() const noexcept {
        switch (options_.driver) {
            case Driver::postgres:
#if ASYNC_UV_SQL_HAS_LIBPQ
                return pg_ != nullptr;
#else
                return false;
#endif
            case Driver::mysql:
#if ASYNC_UV_SQL_HAS_MYSQL
                return mysql_ != nullptr;
#else
                return false;
#endif
            case Driver::sqlite:
#if ASYNC_UV_SQL_HAS_SQLITE3
                return sqlite_ != nullptr;
#else
                return false;
#endif
        }
        return false;
    }

    void close_sync() noexcept {
#if ASYNC_UV_SQL_HAS_LIBPQ
        if (pg_ != nullptr) {
            PQfinish(pg_);
            pg_ = nullptr;
        }
#endif
#if ASYNC_UV_SQL_HAS_MYSQL
        if (mysql_ != nullptr) {
            mysql_close(mysql_);
            mysql_ = nullptr;
        }
#endif
#if ASYNC_UV_SQL_HAS_SQLITE3
        if (sqlite_ != nullptr) {
            (void)sqlite3_close_v2(sqlite_);
            sqlite_ = nullptr;
        }
#endif
    }

#if ASYNC_UV_SQL_HAS_SQLITE3
    void open_sqlite_blocking(const ConnectionOptions &options) {
        close_sync();
        const std::string file = default_sqlite_file(options);
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(file.c_str(), &sqlite_, flags, nullptr) != SQLITE_OK) {
            const std::string message =
                sqlite_ == nullptr ? "sqlite open failed" : std::string(sqlite3_errmsg(sqlite_));
            close_sync();
            throw SqlError(message, SqlErrorKind::connect_failed);
        }
    }

    QueryResult query_sqlite_blocking(const std::string &sql, const std::vector<SqlParam> &params) {
        if (sqlite_ == nullptr) {
            throw SqlError("sqlite connection is not open", SqlErrorKind::not_connected);
        }

        sqlite3_stmt *stmt = nullptr;
        const int prepare_rc = sqlite3_prepare_v2(sqlite_, sql.c_str(), -1, &stmt, nullptr);
        if (prepare_rc != SQLITE_OK) {
            throw SqlError(sqlite3_errmsg(sqlite_), SqlErrorKind::query_failed);
        }

        const int expected = sqlite3_bind_parameter_count(stmt);
        if (expected != static_cast<int>(params.size())) {
            sqlite3_finalize(stmt);
            throw SqlError("sqlite parameter count does not match placeholders",
                           SqlErrorKind::invalid_argument);
        }

        for (int i = 0; i < expected; ++i) {
            const auto &param = params[static_cast<std::size_t>(i)];
            int rc = SQLITE_OK;
            switch (param.type()) {
                case SqlParam::Type::null:
                    rc = sqlite3_bind_null(stmt, i + 1);
                    break;
                case SqlParam::Type::text: {
                    const std::string text = param.as_text();
                    rc = sqlite3_bind_text(
                        stmt, i + 1, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
                    break;
                }
                case SqlParam::Type::int64: {
                    const auto value = std::stoll(param.as_text());
                    rc = sqlite3_bind_int64(stmt, i + 1, static_cast<sqlite3_int64>(value));
                    break;
                }
                case SqlParam::Type::float64: {
                    const auto value = std::stod(param.as_text());
                    rc = sqlite3_bind_double(stmt, i + 1, value);
                    break;
                }
                case SqlParam::Type::boolean:
                    rc = sqlite3_bind_int(stmt, i + 1, param.as_text() == "1" ? 1 : 0);
                    break;
            }
            if (rc != SQLITE_OK) {
                sqlite3_finalize(stmt);
                throw SqlError(sqlite3_errmsg(sqlite_), SqlErrorKind::query_failed);
            }
        }

        QueryResult out;
        const int columns = sqlite3_column_count(stmt);
        out.columns.reserve(static_cast<std::size_t>(columns));
        for (int col = 0; col < columns; ++col) {
            const char *name = sqlite3_column_name(stmt, col);
            out.columns.push_back(name == nullptr ? "" : name);
        }

        while (true) {
            const int step = sqlite3_step(stmt);
            if (step == SQLITE_DONE) {
                break;
            }
            if (step != SQLITE_ROW) {
                const std::string message = sqlite3_errmsg(sqlite_);
                sqlite3_finalize(stmt);
                throw SqlError(message, SqlErrorKind::query_failed);
            }

            Row row;
            row.values.reserve(static_cast<std::size_t>(columns));
            for (int col = 0; col < columns; ++col) {
                if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
                    row.values.push_back(std::nullopt);
                } else {
                    const auto *text =
                        reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
                    const int len = sqlite3_column_bytes(stmt, col);
                    row.values.push_back(text == nullptr ? std::string() : std::string(text, len));
                }
            }
            out.rows.push_back(std::move(row));
        }

        sqlite3_finalize(stmt);
        out.affected_rows = static_cast<std::uint64_t>(sqlite3_changes(sqlite_));
        out.last_insert_id = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(sqlite_));
        return out;
    }
#endif

#if ASYNC_UV_SQL_HAS_LIBPQ
    Task<void> open_postgres_async(const ConnectionOptions &options) {
        close_sync();

        if (options.database.empty()) {
            throw SqlError("postgres requires database name", SqlErrorKind::invalid_argument);
        }

        std::ostringstream conninfo;
        conninfo << "host='" << pg_quote(options.host) << "' "
                 << "port='" << default_port(options) << "' "
                 << "dbname='" << pg_quote(options.database) << "'";
        if (!options.user.empty()) {
            conninfo << " user='" << pg_quote(options.user) << "'";
        }
        if (!options.password.empty()) {
            conninfo << " password='" << pg_quote(options.password) << "'";
        }
        conninfo << " sslmode='" << postgres_ssl_mode_to_string(options.postgres_ssl_mode) << "'";
        if (!options.postgres_ssl_root_cert_file.empty()) {
            conninfo << " sslrootcert='" << pg_quote(options.postgres_ssl_root_cert_file) << "'";
        }
        if (!options.postgres_ssl_cert_file.empty()) {
            conninfo << " sslcert='" << pg_quote(options.postgres_ssl_cert_file) << "'";
        }
        if (!options.postgres_ssl_key_file.empty()) {
            conninfo << " sslkey='" << pg_quote(options.postgres_ssl_key_file) << "'";
        }
        if (!options.postgres_ssl_crl_file.empty()) {
            conninfo << " sslcrl='" << pg_quote(options.postgres_ssl_crl_file) << "'";
        }
        if (!options.postgres_ssl_min_protocol.empty()) {
            conninfo << " ssl_min_protocol_version='" << pg_quote(options.postgres_ssl_min_protocol)
                     << "'";
        }
        if (options.connect_timeout_ms > 0) {
            const int sec = std::max(1, options.connect_timeout_ms / 1000);
            conninfo << " connect_timeout='" << sec << "'";
        }

        pg_ = PQconnectStart(conninfo.str().c_str());
        if (pg_ == nullptr) {
            throw SqlError("postgres connection start failed", SqlErrorKind::connect_failed);
        }

        if (PQsetnonblocking(pg_, 1) != 0) {
            const std::string message = PQerrorMessage(pg_);
            close_sync();
            throw SqlError(message, SqlErrorKind::connect_failed);
        }

        while (true) {
            const PostgresPollingStatusType poll = PQconnectPoll(pg_);
            if (poll == PGRES_POLLING_OK) {
                break;
            }
            if (poll == PGRES_POLLING_FAILED) {
                const std::string message = PQerrorMessage(pg_);
                close_sync();
                throw SqlError(message, SqlErrorKind::connect_failed);
            }
            if (poll == PGRES_POLLING_READING) {
                co_await wait_fd_ready(PQsocket(pg_), true, false, std::nullopt);
                continue;
            }
            if (poll == PGRES_POLLING_WRITING) {
                co_await wait_fd_ready(PQsocket(pg_), false, true, std::nullopt);
                continue;
            }

            co_await async_uv::sleep_for(std::chrono::milliseconds(1));
        }
    }

    QueryResult parse_postgres_result(PGresult *result, QueryResult out) {
        const ExecStatusType status = PQresultStatus(result);
        if (status == PGRES_TUPLES_OK || status == PGRES_SINGLE_TUPLE) {
            const int columns = PQnfields(result);
            if (out.columns.empty()) {
                out.columns.reserve(static_cast<std::size_t>(columns));
                for (int col = 0; col < columns; ++col) {
                    out.columns.push_back(PQfname(result, col) == nullptr ? ""
                                                                          : PQfname(result, col));
                }
            }

            const int rows = PQntuples(result);
            out.rows.reserve(out.rows.size() + static_cast<std::size_t>(rows));
            for (int row = 0; row < rows; ++row) {
                Row item;
                item.values.reserve(static_cast<std::size_t>(columns));
                for (int col = 0; col < columns; ++col) {
                    if (PQgetisnull(result, row, col) == 1) {
                        item.values.push_back(std::nullopt);
                    } else {
                        item.values.push_back(std::string(PQgetvalue(result, row, col) == nullptr
                                                              ? ""
                                                              : PQgetvalue(result, row, col)));
                    }
                }
                out.rows.push_back(std::move(item));
            }
        } else if (status == PGRES_COMMAND_OK) {
            out.affected_rows +=
                parse_u64(PQcmdTuples(result) == nullptr ? "" : PQcmdTuples(result));
        } else {
            const std::string message = PQresultErrorMessage(result) == nullptr
                                            ? "postgres query failed"
                                            : std::string(PQresultErrorMessage(result));
            throw SqlError(message, SqlErrorKind::query_failed);
        }

        const auto oid = static_cast<std::uint64_t>(PQoidValue(result));
        if (oid != static_cast<std::uint64_t>(InvalidOid)) {
            out.last_insert_id = oid;
        }
        return out;
    }

    Task<QueryResult> query_postgres_async(std::string sql, std::vector<SqlParam> params) {
        if (pg_ == nullptr) {
            throw SqlError("postgres connection is not open", SqlErrorKind::not_connected);
        }

        std::string rewritten = sql;
        if (!params.empty() && sql.find('?') != std::string::npos) {
            rewritten = rewrite_qmark_to_dollar(sql);
        }

        if (count_placeholders(sql) != params.size()) {
            throw SqlError("postgres parameter count does not match placeholders",
                           SqlErrorKind::invalid_argument);
        }

        std::vector<std::string> text_values;
        text_values.reserve(params.size());
        std::vector<const char *> values(params.size(), nullptr);
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (!params[i].is_null()) {
                text_values.push_back(params[i].as_text());
                values[i] = text_values.back().c_str();
            }
        }

        int send_ok = 0;
        if (params.empty()) {
            send_ok = PQsendQuery(pg_, rewritten.c_str());
        } else {
            send_ok = PQsendQueryParams(pg_,
                                        rewritten.c_str(),
                                        static_cast<int>(params.size()),
                                        nullptr,
                                        values.data(),
                                        nullptr,
                                        nullptr,
                                        0);
        }

        if (send_ok == 0) {
            throw SqlError(PQerrorMessage(pg_), SqlErrorKind::query_failed);
        }

        while (true) {
            const int flush = PQflush(pg_);
            if (flush == 0) {
                break;
            }
            if (flush < 0) {
                throw SqlError(PQerrorMessage(pg_), SqlErrorKind::query_failed);
            }
            co_await wait_fd_ready(PQsocket(pg_), false, true, std::nullopt);
        }

        QueryResult out;
        while (true) {
            while (PQisBusy(pg_) != 0) {
                co_await wait_fd_ready(PQsocket(pg_), true, false, std::nullopt);
                if (PQconsumeInput(pg_) == 0) {
                    throw SqlError(PQerrorMessage(pg_), SqlErrorKind::query_failed);
                }
            }

            PGresult *result = PQgetResult(pg_);
            if (result == nullptr) {
                break;
            }

            try {
                out = parse_postgres_result(result, std::move(out));
            } catch (...) {
                PQclear(result);
                throw;
            }
            PQclear(result);
        }

        co_return out;
    }
#endif

#if ASYNC_UV_SQL_HAS_MYSQL
    Task<void> wait_mysql_socket_ready() {
        if (mysql_ == nullptr) {
            throw SqlError("mysql connection is not open", SqlErrorKind::not_connected);
        }

        const int fd = mysql_socket_fd(mysql_);
        if (fd < 0) {
            co_await async_uv::sleep_for(std::chrono::milliseconds(1));
            co_return;
        }

        co_await wait_fd_ready(static_cast<uv_os_sock_t>(fd), true, true, std::nullopt);
    }

    Task<void> open_mysql_async(const ConnectionOptions &options) {
        close_sync();
        ensure_mysql_library_initialized();

        mysql_ = mysql_init(nullptr);
        if (mysql_ == nullptr) {
            throw SqlError("mysql_init failed", SqlErrorKind::connect_failed);
        }

        const char *host = options.host.empty() ? nullptr : options.host.c_str();
        const char *user = options.user.empty() ? nullptr : options.user.c_str();
        const char *password = options.password.empty() ? nullptr : options.password.c_str();
        const char *database = options.database.empty() ? nullptr : options.database.c_str();

        enum net_async_status status =
            mysql_real_connect_nonblocking(mysql_,
                                           host,
                                           user,
                                           password,
                                           database,
                                           static_cast<unsigned int>(default_port(options)),
                                           nullptr,
                                           0);
        while (status == NET_ASYNC_NOT_READY) {
            co_await wait_mysql_socket_ready();
            status =
                mysql_real_connect_nonblocking(mysql_,
                                               host,
                                               user,
                                               password,
                                               database,
                                               static_cast<unsigned int>(default_port(options)),
                                               nullptr,
                                               0);
        }

        if (status == NET_ASYNC_ERROR) {
            std::string message = mysql_error(mysql_);
            if (message.empty()) {
                message = "mysql connect failed";
            }
            close_sync();
            throw SqlError(message, SqlErrorKind::connect_failed);
        }
        co_return;
    }

    QueryResult query_mysql_stmt_blocking(const std::string &sql,
                                          const std::vector<SqlParam> &params) {
        if (mysql_ == nullptr) {
            throw SqlError("mysql connection is not open", SqlErrorKind::not_connected);
        }

        if (count_placeholders(sql) != params.size()) {
            throw SqlError("mysql parameter count does not match placeholders",
                           SqlErrorKind::invalid_argument);
        }

        MYSQL_STMT *stmt = mysql_stmt_init(mysql_);
        if (stmt == nullptr) {
            throw SqlError(mysql_error(mysql_), SqlErrorKind::query_failed);
        }

        auto close_stmt = [&]() {
            if (stmt != nullptr) {
                mysql_stmt_close(stmt);
                stmt = nullptr;
            }
        };

        if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
            const std::string message = mysql_stmt_error(stmt);
            close_stmt();
            throw SqlError(message, SqlErrorKind::query_failed);
        }

        const unsigned long expected = mysql_stmt_param_count(stmt);
        if (expected != params.size()) {
            close_stmt();
            throw SqlError("mysql parameter count does not match placeholders",
                           SqlErrorKind::invalid_argument);
        }

        std::vector<MYSQL_BIND> binds(params.size());
        std::vector<std::string> text_values(params.size());
        std::vector<std::int64_t> int_values(params.size(), 0);
        std::vector<double> double_values(params.size(), 0.0);
        std::vector<signed char> bool_values(params.size(), 0);
        std::vector<unsigned long> lengths(params.size(), 0);
        std::vector<char> nulls(params.size(), 0);
        std::vector<char> errors(params.size(), 0);

        for (std::size_t i = 0; i < params.size(); ++i) {
            MYSQL_BIND bind{};
            bind.length = &lengths[i];
            bind.is_null = reinterpret_cast<bool *>(&nulls[i]);
            bind.error = reinterpret_cast<bool *>(&errors[i]);

            const SqlParam &param = params[i];
            switch (param.type()) {
                case SqlParam::Type::null:
                    bind.buffer_type = MYSQL_TYPE_NULL;
                    nulls[i] = 1;
                    break;
                case SqlParam::Type::text:
                    text_values[i] = param.as_text();
                    bind.buffer_type = MYSQL_TYPE_STRING;
                    bind.buffer = text_values[i].data();
                    lengths[i] = static_cast<unsigned long>(text_values[i].size());
                    bind.buffer_length = lengths[i];
                    break;
                case SqlParam::Type::int64:
                    int_values[i] = std::stoll(param.as_text());
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.buffer = &int_values[i];
                    bind.is_unsigned = false;
                    break;
                case SqlParam::Type::float64:
                    double_values[i] = std::stod(param.as_text());
                    bind.buffer_type = MYSQL_TYPE_DOUBLE;
                    bind.buffer = &double_values[i];
                    break;
                case SqlParam::Type::boolean:
                    bool_values[i] = param.as_text() == "1" ? 1 : 0;
                    bind.buffer_type = MYSQL_TYPE_TINY;
                    bind.buffer = &bool_values[i];
                    bind.buffer_length = sizeof(signed char);
                    break;
            }

            binds[i] = bind;
        }

        if (!binds.empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            const std::string message = mysql_stmt_error(stmt);
            close_stmt();
            throw SqlError(message, SqlErrorKind::query_failed);
        }

        bool update_max_length = true;
        (void)mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length);

        if (mysql_stmt_execute(stmt) != 0) {
            const std::string message = mysql_stmt_error(stmt);
            close_stmt();
            throw SqlError(message, SqlErrorKind::query_failed);
        }

        QueryResult out;
        const unsigned int column_count = mysql_stmt_field_count(stmt);
        if (column_count == 0) {
            out.affected_rows = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
            out.last_insert_id = static_cast<std::uint64_t>(mysql_insert_id(mysql_));
            close_stmt();
            return out;
        }

        if (mysql_stmt_store_result(stmt) != 0) {
            const std::string message = mysql_stmt_error(stmt);
            close_stmt();
            throw SqlError(message, SqlErrorKind::query_failed);
        }

        MYSQL_RES *meta = mysql_stmt_result_metadata(stmt);
        if (meta == nullptr) {
            const std::string message = mysql_stmt_error(stmt);
            close_stmt();
            throw SqlError(message.empty() ? "mysql metadata fetch failed" : message,
                           SqlErrorKind::query_failed);
        }

        MYSQL_FIELD *fields = mysql_fetch_fields(meta);
        out.columns.reserve(column_count);
        for (unsigned int col = 0; col < column_count; ++col) {
            out.columns.push_back(fields[col].name == nullptr ? "" : fields[col].name);
        }

        std::vector<MYSQL_BIND> result_binds(column_count);
        std::vector<std::vector<char>> result_buffers(column_count);
        std::vector<unsigned long> result_lengths(column_count, 0);
        std::vector<char> result_nulls(column_count, 0);
        std::vector<char> result_errors(column_count, 0);

        for (unsigned int col = 0; col < column_count; ++col) {
            const auto capacity = static_cast<std::size_t>(std::max<std::uint64_t>(
                64, static_cast<std::uint64_t>(fields[col].max_length + 1)));
            result_buffers[col].assign(capacity, '\0');

            MYSQL_BIND bind{};
            bind.buffer_type = MYSQL_TYPE_STRING;
            bind.buffer = result_buffers[col].data();
            bind.buffer_length = static_cast<unsigned long>(result_buffers[col].size());
            bind.length = &result_lengths[col];
            bind.is_null = reinterpret_cast<bool *>(&result_nulls[col]);
            bind.error = reinterpret_cast<bool *>(&result_errors[col]);
            result_binds[col] = bind;
        }

        if (mysql_stmt_bind_result(stmt, result_binds.data()) != 0) {
            const std::string message = mysql_stmt_error(stmt);
            mysql_free_result(meta);
            close_stmt();
            throw SqlError(message, SqlErrorKind::query_failed);
        }

        while (true) {
            const int rc = mysql_stmt_fetch(stmt);
            if (rc == MYSQL_NO_DATA) {
                break;
            }
            if (rc == 1) {
                const std::string message = mysql_stmt_error(stmt);
                mysql_free_result(meta);
                close_stmt();
                throw SqlError(message, SqlErrorKind::query_failed);
            }

            Row row;
            row.values.reserve(column_count);
            for (unsigned int col = 0; col < column_count; ++col) {
                if (result_nulls[col] != 0) {
                    row.values.push_back(std::nullopt);
                } else {
                    row.values.push_back(
                        std::string(result_buffers[col].data(), result_lengths[col]));
                }
            }
            out.rows.push_back(std::move(row));
        }

        out.affected_rows = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
        out.last_insert_id = static_cast<std::uint64_t>(mysql_insert_id(mysql_));
        mysql_free_result(meta);
        close_stmt();
        return out;
    }

    Task<QueryResult> query_mysql_async(std::string sql, std::vector<SqlParam> params) {
        if (mysql_ == nullptr) {
            throw SqlError("mysql connection is not open", SqlErrorKind::not_connected);
        }
        if (!params.empty()) {
            throw SqlError("mysql async query only supports empty params",
                           SqlErrorKind::invalid_argument);
        }

        enum net_async_status status = mysql_real_query_nonblocking(
            mysql_, sql.c_str(), static_cast<unsigned long>(sql.size()));
        while (status == NET_ASYNC_NOT_READY) {
            co_await wait_mysql_socket_ready();
            status = mysql_real_query_nonblocking(
                mysql_, sql.c_str(), static_cast<unsigned long>(sql.size()));
        }

        if (status == NET_ASYNC_ERROR) {
            throw SqlError(mysql_error(mysql_), SqlErrorKind::query_failed);
        }

        QueryResult out;
        MYSQL_RES *result = nullptr;
        status = mysql_store_result_nonblocking(mysql_, &result);
        while (status == NET_ASYNC_NOT_READY) {
            co_await wait_mysql_socket_ready();
            status = mysql_store_result_nonblocking(mysql_, &result);
        }

        if (status == NET_ASYNC_ERROR) {
            throw SqlError(mysql_error(mysql_), SqlErrorKind::query_failed);
        }

        if (result == nullptr) {
            if (mysql_field_count(mysql_) != 0) {
                throw SqlError(mysql_error(mysql_), SqlErrorKind::query_failed);
            }

            out.affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(mysql_));
            out.last_insert_id = static_cast<std::uint64_t>(mysql_insert_id(mysql_));
            co_return out;
        }

        const unsigned int columns = mysql_num_fields(result);
        MYSQL_FIELD *fields = mysql_fetch_fields(result);
        out.columns.reserve(columns);
        for (unsigned int col = 0; col < columns; ++col) {
            out.columns.push_back(fields[col].name == nullptr ? "" : fields[col].name);
        }

        MYSQL_ROW row = nullptr;
        status = mysql_fetch_row_nonblocking(result, &row);
        while (status == NET_ASYNC_NOT_READY) {
            co_await wait_mysql_socket_ready();
            status = mysql_fetch_row_nonblocking(result, &row);
        }

        while (status == NET_ASYNC_COMPLETE && row != nullptr) {
            unsigned long *lengths = mysql_fetch_lengths(result);
            Row item;
            item.values.reserve(columns);
            for (unsigned int col = 0; col < columns; ++col) {
                if (row[col] == nullptr) {
                    item.values.push_back(std::nullopt);
                } else {
                    item.values.push_back(std::string(row[col], lengths[col]));
                }
            }
            out.rows.push_back(std::move(item));

            status = mysql_fetch_row_nonblocking(result, &row);
            while (status == NET_ASYNC_NOT_READY) {
                co_await wait_mysql_socket_ready();
                status = mysql_fetch_row_nonblocking(result, &row);
            }
        }

        if (status == NET_ASYNC_ERROR) {
            mysql_free_result(result);
            throw SqlError(mysql_error(mysql_), SqlErrorKind::query_failed);
        }

        mysql_free_result(result);
        out.affected_rows = static_cast<std::uint64_t>(mysql_affected_rows(mysql_));
        out.last_insert_id = static_cast<std::uint64_t>(mysql_insert_id(mysql_));
        co_return out;
    }
#endif

    ConnectionOptions options_{};
    std::atomic_bool busy_{false};
    std::mutex sqlite_mutex_;
    std::mutex mysql_mutex_;

#if ASYNC_UV_SQL_HAS_LIBPQ
    PGconn *pg_ = nullptr;
#endif
#if ASYNC_UV_SQL_HAS_MYSQL
    MYSQL *mysql_ = nullptr;
#endif
#if ASYNC_UV_SQL_HAS_SQLITE3
    sqlite3 *sqlite_ = nullptr;
#endif
};

Connection::Connection() : impl_(std::make_unique<Impl>()) {}

Connection::~Connection() {
    if (impl_) {
        impl_->close_sync();
    }
}

Connection::Connection(Connection &&) noexcept = default;

Connection &Connection::operator=(Connection &&) noexcept = default;

Task<void> Connection::open(ConnectionOptions options) {
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        throw SqlError("sql::Connection::open requires current runtime",
                       SqlErrorKind::runtime_missing);
    }

    OperationGuard guard(impl_->busy_);

    switch (options.driver) {
        case Driver::sqlite:
#if ASYNC_UV_SQL_HAS_SQLITE3
        {
            auto error = std::make_shared<std::exception_ptr>();
            auto open_task = [runtime, impl = impl_.get(), options, error]() -> Task<void> {
                co_await runtime->spawn_blocking([impl, options, error] {
                    try {
                        std::lock_guard<std::mutex> lock(impl->sqlite_mutex_);
                        impl->open_sqlite_blocking(options);
                    } catch (...) {
                        *error = std::current_exception();
                    }
                });
                co_return;
            };

            try {
                if (options.connect_timeout_ms > 0) {
                    co_await async_uv::with_timeout(
                        std::chrono::milliseconds(options.connect_timeout_ms), open_task());
                } else {
                    co_await open_task();
                }
            } catch (const async_uv::Error &e) {
                if (e.code() == UV_ETIMEDOUT) {
                    throw SqlError("sql connect timeout", SqlErrorKind::connect_failed);
                }
                throw;
            }

            if (*error) {
                std::rethrow_exception(*error);
            }
        }
            impl_->options_ = std::move(options);
            co_return;
#else
            throw SqlError("sqlite driver is not enabled", SqlErrorKind::invalid_argument);
#endif

        case Driver::postgres:
#if ASYNC_UV_SQL_HAS_LIBPQ
            try {
                if (options.connect_timeout_ms > 0) {
                    co_await async_uv::with_timeout(
                        std::chrono::milliseconds(options.connect_timeout_ms),
                        impl_->open_postgres_async(options));
                } else {
                    co_await impl_->open_postgres_async(options);
                }
            } catch (const async_uv::Error &e) {
                if (e.code() == UV_ETIMEDOUT) {
                    throw SqlError("sql connect timeout", SqlErrorKind::connect_failed);
                }
                throw;
            }
            impl_->options_ = std::move(options);
            co_return;
#else
            throw SqlError("postgres driver is not enabled", SqlErrorKind::invalid_argument);
#endif

        case Driver::mysql:
#if ASYNC_UV_SQL_HAS_MYSQL
            try {
                if (options.connect_timeout_ms > 0) {
                    co_await async_uv::with_timeout(
                        std::chrono::milliseconds(options.connect_timeout_ms),
                        impl_->open_mysql_async(options));
                } else {
                    co_await impl_->open_mysql_async(options);
                }
            } catch (const async_uv::Error &e) {
                if (e.code() == UV_ETIMEDOUT) {
                    throw SqlError("sql connect timeout", SqlErrorKind::connect_failed);
                }
                throw;
            }
            impl_->options_ = std::move(options);
            co_return;
#else
            throw SqlError("mysql driver is not enabled", SqlErrorKind::invalid_argument);
#endif
    }

    throw SqlError("unsupported sql driver", SqlErrorKind::internal_error);
}

Task<void> Connection::close() {
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        throw SqlError("sql::Connection::close requires current runtime",
                       SqlErrorKind::runtime_missing);
    }

    OperationGuard guard(impl_->busy_);

    if (impl_->options_.driver == Driver::sqlite) {
#if ASYNC_UV_SQL_HAS_SQLITE3
        co_await runtime->spawn_blocking([impl = impl_.get()] {
            std::lock_guard<std::mutex> lock(impl->sqlite_mutex_);
            impl->close_sync();
        });
        co_return;
#endif
    }

    impl_->close_sync();
    co_return;
}

Task<bool> Connection::is_open() const {
    co_return impl_->is_open();
}

ConnectionOptions Connection::options() const {
    return impl_->options_;
}

Task<QueryResult> Connection::query(std::string sql) {
    co_return co_await query(std::move(sql), {}, QueryOptions{});
}

Task<QueryResult> Connection::query(std::string sql, std::vector<SqlParam> params) {
    co_return co_await query(std::move(sql), std::move(params), QueryOptions{});
}

Task<QueryResult> Connection::query(std::string sql, QueryOptions options) {
    co_return co_await query(std::move(sql), {}, std::move(options));
}

Task<QueryResult>
Connection::query(std::string sql, std::vector<SqlParam> params, QueryOptions query_options) {
    const auto started = std::chrono::steady_clock::now();
    async_uv::emit_trace_event({"layer2_sql", "query_start", 0, params.size()});
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        throw SqlError("sql::Connection::query requires current runtime",
                       SqlErrorKind::runtime_missing);
    }

    OperationGuard guard(impl_->busy_);
    const Driver driver = impl_->options_.driver;

    switch (driver) {
        case Driver::sqlite:
#if ASYNC_UV_SQL_HAS_SQLITE3
        {
            auto run = [runtime,
                        impl = impl_.get(),
                        sql = std::move(sql),
                        params = std::move(params)]() mutable -> Task<QueryResult> {
                auto error = std::make_shared<std::exception_ptr>();
                auto result = std::make_shared<QueryResult>();
                co_await runtime->spawn_blocking([impl,
                                                  sql = std::move(sql),
                                                  params = std::move(params),
                                                  error,
                                                  result]() mutable {
                    try {
                        std::lock_guard<std::mutex> lock(impl->sqlite_mutex_);
                        *result = impl->query_sqlite_blocking(sql, params);
                    } catch (...) {
                        *error = std::current_exception();
                    }
                });
                if (*error) {
                    std::rethrow_exception(*error);
                }
                co_return *result;
            };
            auto out = co_await run_with_query_timeout(impl_->options_, query_options, run);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            async_uv::emit_trace_event(
                {"layer2_sql", "query_done", 0, static_cast<std::size_t>(elapsed.count())});
            co_return out;
        }
#else
            throw SqlError("sqlite driver is not enabled", SqlErrorKind::invalid_argument);
#endif

        case Driver::postgres:
#if ASYNC_UV_SQL_HAS_LIBPQ
        {
            auto run =
                [impl = impl_.get(), sql = std::move(sql), params = std::move(params)]() mutable {
                    return impl->query_postgres_async(std::move(sql), std::move(params));
                };
            auto out = co_await run_with_query_timeout(impl_->options_, query_options, run);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            async_uv::emit_trace_event(
                {"layer2_sql", "query_done", 0, static_cast<std::size_t>(elapsed.count())});
            co_return out;
        }
#else
            throw SqlError("postgres driver is not enabled", SqlErrorKind::invalid_argument);
#endif

        case Driver::mysql:
#if ASYNC_UV_SQL_HAS_MYSQL
        {
            if (!params.empty()) {
                auto run = [runtime,
                            impl = impl_.get(),
                            sql = std::move(sql),
                            params = std::move(params)]() mutable -> Task<QueryResult> {
                    auto error = std::make_shared<std::exception_ptr>();
                    auto result = std::make_shared<QueryResult>();
                    co_await runtime->spawn_blocking([impl,
                                                      sql = std::move(sql),
                                                      params = std::move(params),
                                                      error,
                                                      result]() mutable {
                        try {
                            std::lock_guard<std::mutex> lock(impl->mysql_mutex_);
                            *result = impl->query_mysql_stmt_blocking(sql, params);
                        } catch (...) {
                            *error = std::current_exception();
                        }
                    });
                    if (*error) {
                        std::rethrow_exception(*error);
                    }
                    co_return *result;
                };
                auto out = co_await run_with_query_timeout(impl_->options_, query_options, run);
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                async_uv::emit_trace_event(
                    {"layer2_sql", "query_done", 0, static_cast<std::size_t>(elapsed.count())});
                co_return out;
            }

            auto run = [impl = impl_.get(), sql = std::move(sql)]() mutable {
                return impl->query_mysql_async(std::move(sql), {});
            };
            auto out = co_await run_with_query_timeout(impl_->options_, query_options, run);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            async_uv::emit_trace_event(
                {"layer2_sql", "query_done", 0, static_cast<std::size_t>(elapsed.count())});
            co_return out;
        }
#else
            throw SqlError("mysql driver is not enabled", SqlErrorKind::invalid_argument);
#endif
    }

    throw SqlError("unsupported sql driver", SqlErrorKind::internal_error);
}

Task<QueryResult> Connection::execute(std::string sql) {
    co_return co_await query(std::move(sql), {}, QueryOptions{});
}

Task<QueryResult> Connection::execute(std::string sql, std::vector<SqlParam> params) {
    co_return co_await query(std::move(sql), std::move(params), QueryOptions{});
}

Task<QueryResult> Connection::execute(std::string sql, QueryOptions options) {
    co_return co_await query(std::move(sql), {}, std::move(options));
}

Task<QueryResult>
Connection::execute(std::string sql, std::vector<SqlParam> params, QueryOptions options) {
    co_return co_await query(std::move(sql), std::move(params), std::move(options));
}

Task<void> Connection::begin() {
    (void)co_await execute("BEGIN");
    co_return;
}

Task<void> Connection::commit() {
    (void)co_await execute("COMMIT");
    co_return;
}

Task<void> Connection::rollback() {
    (void)co_await execute("ROLLBACK");
    co_return;
}

Task<void> Connection::cancel() {
    switch (impl_->options_.driver) {
        case Driver::sqlite:
#if ASYNC_UV_SQL_HAS_SQLITE3
            if (impl_->sqlite_ != nullptr) {
                sqlite3_interrupt(impl_->sqlite_);
            }
#endif
            co_return;
        case Driver::postgres:
#if ASYNC_UV_SQL_HAS_LIBPQ
            if (impl_->pg_ != nullptr) {
                PGcancel *cancel = PQgetCancel(impl_->pg_);
                if (cancel != nullptr) {
                    char errbuf[256] = {0};
                    if (PQcancel(cancel, errbuf, sizeof(errbuf)) == 0) {
                        PQfreeCancel(cancel);
                        throw SqlError(errbuf[0] == '\0' ? "postgres cancel failed" : errbuf,
                                       SqlErrorKind::query_failed);
                    }
                    PQfreeCancel(cancel);
                }
            }
#endif
            co_return;
        case Driver::mysql:
#if ASYNC_UV_SQL_HAS_MYSQL
            if (impl_->mysql_ != nullptr) {
                mysql_close(impl_->mysql_);
                impl_->mysql_ = nullptr;
            }
#endif
            co_return;
    }
    co_return;
}

Task<QueryResult> query(ConnectionOptions options, std::string sql) {
    co_return co_await query(std::move(options), std::move(sql), {}, QueryOptions{});
}

Task<QueryResult> query(ConnectionOptions options, std::string sql, std::vector<SqlParam> params) {
    co_return co_await query(std::move(options), std::move(sql), std::move(params), QueryOptions{});
}

Task<QueryResult> query(ConnectionOptions options,
                        std::string sql,
                        std::vector<SqlParam> params,
                        QueryOptions query_options) {
    Connection conn;
    co_await conn.open(std::move(options));

    std::exception_ptr error;
    QueryResult result;
    try {
        result = co_await conn.query(std::move(sql), std::move(params), std::move(query_options));
    } catch (...) {
        error = std::current_exception();
    }

    try {
        co_await conn.close();
    } catch (...) {
    }

    if (error) {
        std::rethrow_exception(error);
    }
    co_return result;
}

class ConnectionPool::Impl {
public:
    explicit Impl(ConnectionPoolOptions options) : options_(std::move(options)) {}

    Task<void> init() {
        if (options_.max_connections == 0) {
            throw SqlError("connection pool max_connections must be > 0",
                           SqlErrorKind::invalid_argument);
        }

        connections_.resize(options_.max_connections);
        born_at_.resize(options_.max_connections, std::chrono::steady_clock::now());
        if (!options_.preconnect) {
            for (std::size_t i = 0; i < options_.max_connections; ++i) {
                available_.push_back(i);
            }
            co_return;
        }

        for (std::size_t i = 0; i < options_.max_connections; ++i) {
            co_await connections_[i].open(options_.connection);
            born_at_[i] = std::chrono::steady_clock::now();
            available_.push_back(i);
        }
    }

    Task<std::size_t> acquire_index() {
        const auto started = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    throw SqlError("connection pool is closed", SqlErrorKind::not_connected);
                }
                if (!available_.empty()) {
                    const std::size_t index = available_.front();
                    available_.pop_front();
                    co_return index;
                }
            }
            if (options_.acquire_timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                if (elapsed.count() >= options_.acquire_timeout_ms) {
                    throw SqlError("connection pool acquire timeout", SqlErrorKind::query_failed);
                }
            }
            co_await async_uv::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Task<void> ensure_healthy(std::size_t index) {
        auto &conn = connections_[index];
        bool reopen = !(co_await conn.is_open());

        if (!reopen && options_.max_lifetime_ms > 0) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - born_at_[index]);
            if (age.count() >= options_.max_lifetime_ms) {
                reopen = true;
            }
        }

        if (reopen) {
            try {
                co_await conn.close();
            } catch (...) {
            }
            co_await conn.open(options_.connection);
            born_at_[index] = std::chrono::steady_clock::now();
            co_return;
        }

        if (!options_.health_check_sql.empty()) {
            bool healthy = true;
            try {
                (void)co_await conn.query(options_.health_check_sql);
            } catch (...) {
                healthy = false;
            }
            if (!healthy) {
                try {
                    co_await conn.close();
                } catch (...) {
                }
                co_await conn.open(options_.connection);
                born_at_[index] = std::chrono::steady_clock::now();
            }
        }
    }

    void release_index(std::size_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) {
            available_.push_back(index);
        }
    }

    ConnectionPoolOptions options_;
    std::vector<Connection> connections_;
    std::vector<std::chrono::steady_clock::time_point> born_at_;
    std::deque<std::size_t> available_;
    std::mutex mutex_;
    bool closed_ = false;
};

ConnectionPool::ConnectionPool() = default;

ConnectionPool::~ConnectionPool() = default;

ConnectionPool::ConnectionPool(ConnectionPool &&) noexcept = default;

ConnectionPool &ConnectionPool::operator=(ConnectionPool &&) noexcept = default;

Task<ConnectionPool> ConnectionPool::create(ConnectionPoolOptions options) {
    ConnectionPool pool;
    pool.impl_ = std::make_shared<Impl>(std::move(options));
    co_await pool.impl_->init();
    co_return pool;
}

Task<QueryResult> ConnectionPool::query(std::string sql) {
    co_return co_await query(std::move(sql), {}, QueryOptions{});
}

Task<QueryResult> ConnectionPool::query(std::string sql, std::vector<SqlParam> params) {
    co_return co_await query(std::move(sql), std::move(params), QueryOptions{});
}

Task<QueryResult>
ConnectionPool::query(std::string sql, std::vector<SqlParam> params, QueryOptions options) {
    if (impl_ == nullptr) {
        throw SqlError("connection pool is not initialized", SqlErrorKind::not_connected);
    }

    const std::size_t index = co_await impl_->acquire_index();
    std::exception_ptr error;
    QueryResult result;
    try {
        co_await impl_->ensure_healthy(index);
        auto &conn = impl_->connections_[index];
        result = co_await conn.query(std::move(sql), std::move(params), std::move(options));
    } catch (...) {
        error = std::current_exception();
    }

    impl_->release_index(index);
    if (error) {
        std::rethrow_exception(error);
    }
    co_return result;
}

Task<QueryResult> ConnectionPool::execute(std::string sql) {
    co_return co_await execute(std::move(sql), {}, QueryOptions{});
}

Task<QueryResult> ConnectionPool::execute(std::string sql, std::vector<SqlParam> params) {
    co_return co_await execute(std::move(sql), std::move(params), QueryOptions{});
}

Task<QueryResult>
ConnectionPool::execute(std::string sql, std::vector<SqlParam> params, QueryOptions options) {
    co_return co_await query(std::move(sql), std::move(params), std::move(options));
}

Task<void> ConnectionPool::close() {
    if (impl_ == nullptr) {
        co_return;
    }

    std::vector<std::size_t> indices;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->closed_ = true;
        indices.resize(impl_->connections_.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = i;
        }
        impl_->available_.clear();
    }

    for (const auto index : indices) {
        try {
            co_await impl_->connections_[index].close();
        } catch (...) {
        }
    }
    co_return;
}

} // namespace async_uv::sql
