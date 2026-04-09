#include "flux_sql/sql.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/steady_timer.hpp>

#if FLUX_SQL_HAS_LIBPQ
#include <libpq-fe.h>
#endif

#if FLUX_SQL_HAS_MYSQL
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/field.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/src.hpp>
#endif

#if FLUX_SQL_HAS_SQLITE3
#include <sqlite3.h>
#endif

#include "flux/cancel.h"
#include "flux/async_semaphore.h"
#include "flux/error.h"
#include "flux/fd.h"
#include "flux/runtime.h"

namespace flux::sql {

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

#if FLUX_SQL_HAS_LIBPQ
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
#endif

#if FLUX_SQL_HAS_LIBPQ
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

#if FLUX_SQL_HAS_MYSQL
std::string mysql_error_message(const boost::system::error_code &error,
                                const boost::mysql::diagnostics &diagnostics) {
    std::string message = error.message();
    const auto server_message = diagnostics.server_message();
    if (!server_message.empty()) {
        if (!message.empty()) {
            message += ": ";
        }
        message += std::string(server_message);
    }
    if (!message.empty()) {
        return message;
    }
    return "mysql operation failed";
}

boost::mysql::field mysql_param_to_field(const SqlParam &param) {
    switch (param.type()) {
        case SqlParam::Type::null:
            return boost::mysql::field(nullptr);
        case SqlParam::Type::text:
            return boost::mysql::field(param.as_text());
        case SqlParam::Type::int64:
            return boost::mysql::field(std::stoll(param.as_text()));
        case SqlParam::Type::float64:
            return boost::mysql::field(std::stod(param.as_text()));
        case SqlParam::Type::boolean:
            return boost::mysql::field(param.as_text() == "1" ? 1 : 0);
    }
    return boost::mysql::field(nullptr);
}

Cell mysql_field_to_cell(boost::mysql::field_view field) {
    if (field.is_null()) {
        return std::nullopt;
    }

    if (field.is_string()) {
        return std::string(field.as_string());
    }
    if (field.is_blob()) {
        const auto blob = field.as_blob();
        return std::string(reinterpret_cast<const char *>(blob.data()), blob.size());
    }
    if (field.is_int64()) {
        return std::to_string(field.as_int64());
    }
    if (field.is_uint64()) {
        return std::to_string(field.as_uint64());
    }
    if (field.is_float()) {
        std::ostringstream out;
        out << field.as_float();
        return out.str();
    }
    if (field.is_double()) {
        std::ostringstream out;
        out << field.as_double();
        return out.str();
    }
    if (field.is_date()) {
        std::ostringstream out;
        out << field.as_date();
        return out.str();
    }
    if (field.is_datetime()) {
        std::ostringstream out;
        out << field.as_datetime();
        return out.str();
    }
    if (field.is_time()) {
        return std::to_string(field.as_time().count());
    }

    return std::string{};
}

QueryResult mysql_results_to_query_result(const boost::mysql::results &results) {
    QueryResult out;
    out.affected_rows = results.affected_rows();
    out.last_insert_id = results.last_insert_id();

    const auto meta = results.meta();
    out.columns.reserve(meta.size());
    for (const auto &column : meta) {
        out.columns.emplace_back(column.column_name());
    }

    const auto rows = results.rows();
    out.rows.reserve(rows.size());
    for (const auto row_view : rows) {
        Row row;
        row.values.reserve(row_view.size());
        for (const auto value : row_view) {
            row.values.push_back(mysql_field_to_cell(value));
        }
        out.rows.push_back(std::move(row));
    }

    return out;
}

template <typename StartFn>
class MysqlErrorSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(boost::system::error_code),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    explicit MysqlErrorSender(StartFn start) : start_(std::move(start)) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const MysqlErrorSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

private:
    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        StartFn start;
        Receiver receiver;

        void start_impl() noexcept {
            try {
                start([this](boost::system::error_code ec) mutable noexcept {
                    stdexec::set_value(std::move(receiver), ec);
                });
            } catch (...) {
                stdexec::set_error(std::move(receiver), std::current_exception());
            }
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start_impl();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, MysqlErrorSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.start_), std::move(receiver)};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const MysqlErrorSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.start_, std::move(receiver)};
    }

    StartFn start_;
};

template <typename Value, typename StartFn>
class MysqlValueSender {
public:
    using sender_concept = stdexec::sender_t;
    struct Result {
        boost::system::error_code ec{};
        std::optional<Value> value;
    };
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(Result),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    explicit MysqlValueSender(StartFn start) : start_(std::move(start)) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const MysqlValueSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

private:
    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        StartFn start;
        Receiver receiver;

        void start_impl() noexcept {
            try {
                start([this](boost::system::error_code ec, Value value) mutable noexcept {
                    Result result;
                    result.ec = ec;
                    if (!ec) {
                        result.value.emplace(std::move(value));
                    }
                    stdexec::set_value(std::move(receiver), std::move(result));
                });
            } catch (...) {
                stdexec::set_error(std::move(receiver), std::current_exception());
            }
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start_impl();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, MysqlValueSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.start_), std::move(receiver)};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const MysqlValueSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.start_, std::move(receiver)};
    }

    StartFn start_;
};

template <typename StartFn>
auto make_mysql_error_sender(StartFn &&start) {
    return MysqlErrorSender<std::remove_cvref_t<StartFn>>(std::forward<StartFn>(start));
}

template <typename Value, typename StartFn>
auto make_mysql_value_sender(StartFn &&start) {
    return MysqlValueSender<Value, std::remove_cvref_t<StartFn>>(std::forward<StartFn>(start));
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

std::string default_sqlite_file(const ConnectionOptions &options) {
    return options.file.empty() ? ":memory:" : options.file;
}

#if FLUX_SQL_HAS_LIBPQ || FLUX_SQL_HAS_MYSQL
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
#endif

class CurrentRuntimeSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(Runtime *),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>;

    explicit CurrentRuntimeSender(std::string message) : message_(std::move(message)) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const CurrentRuntimeSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

private:
    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        Receiver receiver;
        std::string message;

        void start() noexcept {
            try {
                Runtime *runtime = nullptr;
                auto env = stdexec::get_env(receiver);
                if constexpr (requires { flux::execution::get_runtime(env); }) {
                    runtime = flux::execution::get_runtime(env);
                }
                if (runtime == nullptr) {
                    runtime = Runtime::current();
                }
                if (runtime == nullptr) {
                    throw SqlError(message, SqlErrorKind::runtime_missing);
                }
                stdexec::set_value(std::move(receiver), runtime);
            } catch (...) {
                stdexec::set_error(std::move(receiver), std::current_exception());
            }
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, CurrentRuntimeSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(receiver), std::move(self.message_)};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const CurrentRuntimeSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(receiver), self.message_};
    }

    std::string message_;
};

CurrentRuntimeSender current_runtime_sender(std::string message) {
    return CurrentRuntimeSender(std::move(message));
}

#if FLUX_SQL_HAS_LIBPQ
using LibpqVoidSender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;
using LibpqQuerySender = exec::any_receiver_ref<
    stdexec::completion_signatures<stdexec::set_value_t(QueryResult),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>>::any_sender<>;

LibpqVoidSender throw_sql_error_sender(std::string message, SqlErrorKind kind) {
    return stdexec::just() | stdexec::then([message = std::move(message), kind]() {
               throw SqlError(message, kind);
           });
}

LibpqVoidSender wait_fd_ready_sender(Runtime *runtime,
                                     int fd,
                                     bool readable,
                                     bool writable,
                                     std::optional<int> timeout_ms) {
    if (fd < 0) {
        return throw_sql_error_sender(
            "invalid database socket fd", SqlErrorKind::internal_error);
    }

    int events = 0;
    if (readable) {
        events |= static_cast<int>(FdEventFlags::readable);
    }
    if (writable) {
        events |= static_cast<int>(FdEventFlags::writable);
    }
    if (events == 0) {
        return stdexec::just();
    }

    return FdWatcher::watch_sender(fd, events, runtime)
         | stdexec::let_value([timeout_ms](FdWatcher watcher) -> LibpqVoidSender {
               auto watcher_holder = std::make_shared<FdWatcher>(std::move(watcher));
               auto wait_sender = timeout_ms.has_value()
                                      ? watcher_holder->next_for_sender(
                                            std::chrono::milliseconds(std::max(0, *timeout_ms)))
                                      : watcher_holder->next_sender();
               return std::move(wait_sender)
                    | stdexec::let_value(
                          [watcher_holder](std::optional<FdEvent> event) -> LibpqVoidSender {
                              return watcher_holder->stop_sender()
                                   | stdexec::then([event = std::move(event)]() mutable {
                                         if (!event.has_value()) {
                                             throw SqlError(
                                                 "database socket wait timeout",
                                                 SqlErrorKind::query_failed);
                                         }
                                         if (!event->ok()) {
                                             if (event->error()) {
                                                 throw SqlError(event->error().message(),
                                                                SqlErrorKind::query_failed);
                                             }
                                             throw SqlError("database socket wait returned error",
                                                            SqlErrorKind::query_failed);
                                         }
                                     });
                          })
                    | stdexec::let_error(
                          [watcher_holder](std::exception_ptr error) -> LibpqVoidSender {
                              return watcher_holder->stop_sender()
                                   | stdexec::then([error] { std::rethrow_exception(error); })
                                   | stdexec::upon_error([error](std::exception_ptr) {
                                         std::rethrow_exception(error);
                                     });
                          });
           });
}
#endif

} // namespace

class Connection::Impl {
public:
    bool is_open() const noexcept {
        switch (options_.driver) {
            case Driver::postgres:
#if FLUX_SQL_HAS_LIBPQ
                return pg_ != nullptr;
#else
                return false;
#endif
            case Driver::mysql:
#if FLUX_SQL_HAS_MYSQL
                return mysql_conn_ != nullptr;
#else
                return false;
#endif
            case Driver::sqlite:
#if FLUX_SQL_HAS_SQLITE3
                return sqlite_ != nullptr;
#else
                return false;
#endif
        }
        return false;
    }

    void close_sync() noexcept {
#if FLUX_SQL_HAS_LIBPQ
        if (pg_ != nullptr) {
            PQfinish(pg_);
            pg_ = nullptr;
        }
#endif
#if FLUX_SQL_HAS_MYSQL
        if (mysql_conn_ != nullptr) {
            try {
                boost::mysql::error_code ec;
                boost::mysql::diagnostics diag;
                mysql_conn_->close(ec, diag);
            } catch (...) {
            }
            mysql_conn_.reset();
        }
        mysql_ssl_ctx_.reset();
        if (mysql_work_guard_ != nullptr) {
            mysql_work_guard_->reset();
            mysql_work_guard_.reset();
        }
        if (mysql_ioc_ != nullptr) {
            mysql_ioc_->stop();
            mysql_ioc_.reset();
        }
        if (mysql_thread_.joinable()) {
            if (mysql_thread_.get_id() == std::this_thread::get_id()) {
                mysql_thread_.detach();
            } else {
                mysql_thread_.join();
            }
        }
#endif
#if FLUX_SQL_HAS_SQLITE3
        if (sqlite_ != nullptr) {
            (void)sqlite3_close_v2(sqlite_);
            sqlite_ = nullptr;
        }
#endif
    }

#if FLUX_SQL_HAS_SQLITE3
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

    auto open_sqlite_sender(const ConnectionOptions &options, Runtime *runtime) {
        if (runtime == nullptr) {
            throw SqlError("sql::Connection::open requires current runtime",
                           SqlErrorKind::runtime_missing);
        }
        return runtime->spawn_blocking([this, options] {
            std::lock_guard<std::mutex> lock(sqlite_mutex_);
            open_sqlite_blocking(options);
        });
    }

    auto close_sqlite_sender(Runtime *runtime) {
        if (runtime == nullptr) {
            throw SqlError("sql::Connection::close requires current runtime",
                           SqlErrorKind::runtime_missing);
        }
        return runtime->spawn_blocking([this] {
            std::lock_guard<std::mutex> lock(sqlite_mutex_);
            close_sync();
        });
    }

    auto query_sqlite_sender(Runtime *runtime, std::string sql, std::vector<SqlParam> params) {
        if (runtime == nullptr) {
            throw SqlError("sql::Connection::query requires current runtime",
                           SqlErrorKind::runtime_missing);
        }
        return runtime->spawn_blocking(
            [this, sql = std::move(sql), params = std::move(params)]() mutable {
                std::lock_guard<std::mutex> lock(sqlite_mutex_);
                return query_sqlite_blocking(sql, params);
            });
    }
#endif

#if FLUX_SQL_HAS_LIBPQ
    LibpqVoidSender open_postgres_sender(const ConnectionOptions &options, Runtime *runtime) {
        struct ConnectState {
            Connection::Impl *self = nullptr;
            Runtime *runtime = nullptr;
            ConnectionOptions options;
        };

        auto state = std::make_shared<ConnectState>();
        state->self = this;
        state->runtime = runtime;
        state->options = options;

        return stdexec::just()
             | stdexec::then([state]() {
                   state->self->close_sync();

                   if (state->runtime == nullptr) {
                       throw SqlError("sql::Connection::open requires current runtime",
                                      SqlErrorKind::runtime_missing);
                   }
                   if (state->options.database.empty()) {
                       throw SqlError(
                           "postgres requires database name", SqlErrorKind::invalid_argument);
                   }

                   std::ostringstream conninfo;
                   conninfo << "host='" << pg_quote(state->options.host) << "' "
                            << "port='" << default_port(state->options) << "' "
                            << "dbname='" << pg_quote(state->options.database) << "'";
                   if (!state->options.user.empty()) {
                       conninfo << " user='" << pg_quote(state->options.user) << "'";
                   }
                   if (!state->options.password.empty()) {
                       conninfo << " password='" << pg_quote(state->options.password) << "'";
                   }
                   conninfo << " sslmode='"
                            << postgres_ssl_mode_to_string(state->options.postgres_ssl_mode)
                            << "'";
                   if (!state->options.postgres_ssl_root_cert_file.empty()) {
                       conninfo << " sslrootcert='"
                                << pg_quote(state->options.postgres_ssl_root_cert_file) << "'";
                   }
                   if (!state->options.postgres_ssl_cert_file.empty()) {
                       conninfo << " sslcert='"
                                << pg_quote(state->options.postgres_ssl_cert_file) << "'";
                   }
                   if (!state->options.postgres_ssl_key_file.empty()) {
                       conninfo << " sslkey='"
                                << pg_quote(state->options.postgres_ssl_key_file) << "'";
                   }
                   if (!state->options.postgres_ssl_crl_file.empty()) {
                       conninfo << " sslcrl='"
                                << pg_quote(state->options.postgres_ssl_crl_file) << "'";
                   }
                   if (!state->options.postgres_ssl_min_protocol.empty()) {
                       conninfo << " ssl_min_protocol_version='"
                                << pg_quote(state->options.postgres_ssl_min_protocol) << "'";
                   }
                   if (state->options.connect_timeout_ms > 0) {
                       const int sec = std::max(1, state->options.connect_timeout_ms / 1000);
                       conninfo << " connect_timeout='" << sec << "'";
                   }

                   state->self->pg_ = PQconnectStart(conninfo.str().c_str());
                   if (state->self->pg_ == nullptr) {
                       throw SqlError(
                           "postgres connection start failed", SqlErrorKind::connect_failed);
                   }

                   if (PQsetnonblocking(state->self->pg_, 1) != 0) {
                       const std::string message =
                           PQerrorMessage(state->self->pg_) == nullptr
                               ? std::string("postgres connection failed")
                               : std::string(PQerrorMessage(state->self->pg_));
                       state->self->close_sync();
                       throw SqlError(message, SqlErrorKind::connect_failed);
                   }
               })
             | stdexec::let_value([state]() {
                   auto loop = std::make_shared<std::function<LibpqVoidSender()>>();
                   *loop = [state, loop]() -> LibpqVoidSender {
                       const PostgresPollingStatusType poll = PQconnectPoll(state->self->pg_);
                       if (poll == PGRES_POLLING_OK) {
                           return stdexec::just();
                       }
                       if (poll == PGRES_POLLING_FAILED) {
                           const std::string message =
                               PQerrorMessage(state->self->pg_) == nullptr
                                   ? std::string("postgres connect failed")
                                   : std::string(PQerrorMessage(state->self->pg_));
                           return throw_sql_error_sender(message, SqlErrorKind::connect_failed);
                       }
                       if (poll == PGRES_POLLING_READING) {
                           return wait_fd_ready_sender(
                                      state->runtime,
                                      PQsocket(state->self->pg_),
                                      true,
                                      false,
                                      std::nullopt)
                                | stdexec::let_value([loop] { return (*loop)(); });
                       }
                       if (poll == PGRES_POLLING_WRITING) {
                           return wait_fd_ready_sender(
                                      state->runtime,
                                      PQsocket(state->self->pg_),
                                      false,
                                      true,
                                      std::nullopt)
                                | stdexec::let_value([loop] { return (*loop)(); });
                       }
                       return throw_sql_error_sender(
                           "postgres connect returned unexpected polling status",
                           SqlErrorKind::connect_failed);
                   };
                   return (*loop)();
               })
             | stdexec::let_error([state](std::exception_ptr error) -> LibpqVoidSender {
                   state->self->close_sync();
                   return stdexec::just()
                        | stdexec::then([error] { std::rethrow_exception(error); });
               });
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

    LibpqQuerySender query_postgres_sender(Runtime *runtime,
                                           std::string sql,
                                           std::vector<SqlParam> params) {
        struct QueryState {
            Connection::Impl *self = nullptr;
            Runtime *runtime = nullptr;
            std::string sql;
            std::vector<SqlParam> params;
            std::string rewritten;
            std::vector<std::string> text_values;
            std::vector<const char *> values;
            QueryResult out;
        };

        auto state = std::make_shared<QueryState>();
        state->self = this;
        state->runtime = runtime;
        state->sql = std::move(sql);
        state->params = std::move(params);

        return stdexec::just()
             | stdexec::then([state]() {
                   if (state->runtime == nullptr) {
                       throw SqlError("sql::Connection::query requires current runtime",
                                      SqlErrorKind::runtime_missing);
                   }
                   if (state->self->pg_ == nullptr) {
                       throw SqlError("postgres connection is not open", SqlErrorKind::not_connected);
                   }

                   state->rewritten = state->sql;
                   if (!state->params.empty() && state->sql.find('?') != std::string::npos) {
                       state->rewritten = rewrite_qmark_to_dollar(state->sql);
                   }

                   if (count_placeholders(state->sql) != state->params.size()) {
                       throw SqlError(
                           "postgres parameter count does not match placeholders",
                           SqlErrorKind::invalid_argument);
                   }

                   state->text_values.reserve(state->params.size());
                   state->values.assign(state->params.size(), nullptr);
                   for (std::size_t i = 0; i < state->params.size(); ++i) {
                       if (!state->params[i].is_null()) {
                           state->text_values.push_back(state->params[i].as_text());
                           state->values[i] = state->text_values.back().c_str();
                       }
                   }

                   int send_ok = 0;
                   if (state->params.empty()) {
                       send_ok = PQsendQuery(state->self->pg_, state->rewritten.c_str());
                   } else {
                       send_ok = PQsendQueryParams(state->self->pg_,
                                                   state->rewritten.c_str(),
                                                   static_cast<int>(state->params.size()),
                                                   nullptr,
                                                   state->values.data(),
                                                   nullptr,
                                                   nullptr,
                                                   0);
                   }
                   if (send_ok == 0) {
                       const std::string message = PQerrorMessage(state->self->pg_) == nullptr
                                                       ? std::string("postgres query failed")
                                                       : std::string(PQerrorMessage(state->self->pg_));
                       throw SqlError(message, SqlErrorKind::query_failed);
                   }
               })
             | stdexec::let_value([state]() {
                   auto flush_loop = std::make_shared<std::function<LibpqVoidSender()>>();
                   *flush_loop = [state, flush_loop]() -> LibpqVoidSender {
                       const int flush = PQflush(state->self->pg_);
                       if (flush == 0) {
                           return stdexec::just();
                       }
                       if (flush < 0) {
                           const std::string message = PQerrorMessage(state->self->pg_) == nullptr
                                                           ? std::string("postgres flush failed")
                                                           : std::string(PQerrorMessage(state->self->pg_));
                           return throw_sql_error_sender(message, SqlErrorKind::query_failed);
                       }
                       return wait_fd_ready_sender(state->runtime,
                                                   PQsocket(state->self->pg_),
                                                   false,
                                                   true,
                                                   std::nullopt)
                            | stdexec::let_value([flush_loop] { return (*flush_loop)(); });
                   };
                   return (*flush_loop)();
               })
             | stdexec::let_value([state]() {
                   auto busy_loop = std::make_shared<std::function<LibpqVoidSender()>>();
                   *busy_loop = [state, busy_loop]() -> LibpqVoidSender {
                       if (PQisBusy(state->self->pg_) == 0) {
                           return stdexec::just();
                       }
                       return wait_fd_ready_sender(state->runtime,
                                                   PQsocket(state->self->pg_),
                                                   true,
                                                   false,
                                                   std::nullopt)
                            | stdexec::then([state] {
                                  if (PQconsumeInput(state->self->pg_) == 0) {
                                      const std::string message =
                                          PQerrorMessage(state->self->pg_) == nullptr
                                              ? std::string("postgres consume input failed")
                                              : std::string(PQerrorMessage(state->self->pg_));
                                      throw SqlError(message, SqlErrorKind::query_failed);
                                  }
                              })
                            | stdexec::let_value([busy_loop] { return (*busy_loop)(); });
                   };

                   auto drain_loop = std::make_shared<std::function<LibpqQuerySender()>>();
                   *drain_loop = [state, busy_loop, drain_loop]() -> LibpqQuerySender {
                       return (*busy_loop)()
                            | stdexec::let_value([state, drain_loop]() -> LibpqQuerySender {
                                  std::unique_ptr<PGresult, decltype(&PQclear)> result(
                                      PQgetResult(state->self->pg_), &PQclear);
                                  if (result == nullptr) {
                                      return stdexec::just(std::move(state->out));
                                  }
                                  state->out = state->self->parse_postgres_result(
                                      result.get(), std::move(state->out));
                                  return (*drain_loop)();
                              });
                   };
                   return (*drain_loop)();
               });
    }
#endif

#if FLUX_SQL_HAS_MYSQL
    auto open_mysql_sender(const ConnectionOptions &options, Runtime *runtime) {
        if (runtime == nullptr) {
            throw SqlError("sql::Connection::open requires current runtime",
                           SqlErrorKind::runtime_missing);
        }

        close_sync();

        mysql_ioc_ = std::make_shared<boost::asio::io_context>(1);
        mysql_work_guard_ = std::make_shared<
            boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(*mysql_ioc_));
        mysql_thread_ = std::thread([ioc = mysql_ioc_]() {
            ioc->run();
        });

        mysql_ssl_ctx_ =
            std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);

        boost::mysql::any_connection_params conn_params;
        conn_params.ssl_context = mysql_ssl_ctx_.get();
        mysql_conn_ =
            std::make_shared<boost::mysql::any_connection>(mysql_ioc_->get_executor(), conn_params);

        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(
            options.host.empty() ? "127.0.0.1" : options.host,
            static_cast<unsigned short>(default_port(options)));
        params.username = options.user.empty() ? "root" : options.user;
        params.password = options.password;
        params.database = options.database;
        params.ssl = boost::mysql::ssl_mode::enable;

        auto diagnostics = std::make_shared<boost::mysql::diagnostics>();
        return make_mysql_error_sender(
                   [conn = mysql_conn_, params = std::move(params), diagnostics](auto handler) mutable {
                       conn->async_connect(params, *diagnostics, std::move(handler));
                   })
             | stdexec::then([this, diagnostics](boost::system::error_code ec) {
                   if (ec) {
                       close_sync();
                       throw SqlError(
                           mysql_error_message(ec, *diagnostics), SqlErrorKind::connect_failed);
                   }
               });
    }

    auto close_mysql_sender() {
        auto conn = std::move(mysql_conn_);
        auto ssl_ctx = std::move(mysql_ssl_ctx_);
        auto work_guard = std::move(mysql_work_guard_);
        auto ioc = std::move(mysql_ioc_);
        auto worker = std::move(mysql_thread_);
        auto diagnostics = std::make_shared<boost::mysql::diagnostics>();

        return make_mysql_error_sender(
                   [conn, diagnostics](auto handler) mutable {
                       if (conn == nullptr) {
                           handler(boost::system::error_code{});
                           return;
                       }
                       conn->async_close(*diagnostics, std::move(handler));
                   })
             | stdexec::then(
                   [ssl_ctx = std::move(ssl_ctx),
                    work_guard = std::move(work_guard),
                    ioc = std::move(ioc),
                    worker = std::move(worker)](boost::system::error_code) mutable {
                       if (work_guard != nullptr) {
                           work_guard->reset();
                       }
                       if (ioc != nullptr) {
                           ioc->stop();
                       }
                       if (worker.joinable()) {
                           if (worker.get_id() == std::this_thread::get_id()) {
                               worker.detach();
                           } else {
                               worker.join();
                           }
                       }
                       (void)ssl_ctx;
                   });
    }

    auto open_mysql_checked_sender(const ConnectionOptions &options, Runtime *runtime) {
        return open_mysql_sender(options, runtime)
             | stdexec::let_error([this](std::exception_ptr error) {
                   return close_mysql_sender()
                        | stdexec::then([error]() -> void {
                              std::rethrow_exception(error);
                          })
                        | stdexec::upon_error([error](std::exception_ptr) -> void {
                              std::rethrow_exception(error);
                          });
               });
    }

    auto close_mysql_safe_sender() {
        return close_mysql_sender()
             | stdexec::then([]() -> void {})
             | stdexec::upon_error([](std::exception_ptr) -> void {});
    }

    auto query_mysql_sender(std::string sql, std::vector<SqlParam> params)
        -> exec::any_receiver_ref<
            stdexec::completion_signatures<stdexec::set_value_t(QueryResult),
                                           stdexec::set_error_t(std::exception_ptr),
                                           stdexec::set_stopped_t()>>::any_sender<> {
        auto conn = mysql_conn_;
        if (conn == nullptr) {
            throw SqlError("mysql connection is not open", SqlErrorKind::not_connected);
        }

        auto results = std::make_shared<boost::mysql::results>();
        auto diagnostics = std::make_shared<boost::mysql::diagnostics>();

        if (params.empty()) {
            return make_mysql_error_sender(
                       [conn, sql = std::move(sql), results, diagnostics](auto handler) mutable {
                           conn->async_execute(sql, *results, *diagnostics, std::move(handler));
                       })
                 | stdexec::then([results, diagnostics](boost::system::error_code ec) {
                       if (ec) {
                           throw SqlError(mysql_error_message(ec, *diagnostics),
                                          SqlErrorKind::query_failed);
                       }
                       return mysql_results_to_query_result(*results);
                   });
        }

        auto param_storage = std::make_shared<std::vector<boost::mysql::field>>();
        param_storage->reserve(params.size());
        for (const auto &param : params) {
            param_storage->push_back(mysql_param_to_field(param));
        }
        auto param_views = std::make_shared<std::vector<boost::mysql::field_view>>();
        param_views->reserve(param_storage->size());
        for (const auto &value : *param_storage) {
            param_views->emplace_back(value);
        }

        return make_mysql_value_sender<boost::mysql::statement>(
                   [conn, sql = std::move(sql), diagnostics](auto handler) mutable {
                       conn->async_prepare_statement(sql, *diagnostics, std::move(handler));
                   })
             | stdexec::let_value(
                   [conn, diagnostics, results, param_storage, param_views](auto prepared) mutable {
                       if (prepared.ec) {
                           throw SqlError(mysql_error_message(prepared.ec, *diagnostics),
                                          SqlErrorKind::query_failed);
                       }
                       if (!prepared.value.has_value()) {
                           throw SqlError("mysql prepare statement returned no result",
                                          SqlErrorKind::internal_error);
                       }

                       auto statement = std::move(*prepared.value);
                       if (statement.num_params() != param_storage->size()) {
                           throw SqlError("mysql parameter count does not match placeholders",
                                          SqlErrorKind::invalid_argument);
                       }

                       auto bound = statement.bind(param_views->begin(), param_views->end());
                       auto statement_holder =
                           std::make_shared<boost::mysql::statement>(std::move(statement));
                       auto close_diagnostics = std::make_shared<boost::mysql::diagnostics>();

                       return make_mysql_error_sender(
                                  [conn,
                                   bound,
                                   results,
                                   diagnostics,
                                   param_storage,
                                   param_views](auto handler) mutable {
                                      conn->async_execute(
                                          bound, *results, *diagnostics, std::move(handler));
                                  })
                            | stdexec::let_value(
                                  [conn, results, diagnostics, statement_holder, close_diagnostics](
                                      boost::system::error_code execute_ec) mutable {
                                      if (execute_ec) {
                                          throw SqlError(
                                              mysql_error_message(execute_ec, *diagnostics),
                                              SqlErrorKind::query_failed);
                                      }

                                      return make_mysql_error_sender(
                                                 [conn, statement_holder, close_diagnostics](
                                                     auto handler) mutable {
                                                     conn->async_close_statement(*statement_holder,
                                                                                 *close_diagnostics,
                                                                                 std::move(handler));
                                                 })
                                           | stdexec::then(
                                                 [results](boost::system::error_code) mutable {
                                                     return mysql_results_to_query_result(*results);
                                                 })
                                           | stdexec::upon_error(
                                                 [results](std::exception_ptr) mutable {
                                                     return mysql_results_to_query_result(*results);
                                                 });
                                  });
                   });
    }
#endif

    ConnectionOptions options_{};
    std::atomic_bool busy_{false};
    std::mutex sqlite_mutex_;

#if FLUX_SQL_HAS_LIBPQ
    PGconn *pg_ = nullptr;
#endif
#if FLUX_SQL_HAS_MYSQL
    std::shared_ptr<boost::asio::io_context> mysql_ioc_;
    std::shared_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        mysql_work_guard_;
    std::thread mysql_thread_;
    std::shared_ptr<boost::asio::ssl::context> mysql_ssl_ctx_;
    std::shared_ptr<boost::mysql::any_connection> mysql_conn_;
#endif
#if FLUX_SQL_HAS_SQLITE3
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

VoidSender Connection::open_task(ConnectionOptions options) {
    auto guard = std::make_shared<OperationGuard>(impl_->busy_);
    auto options_state = std::make_shared<ConnectionOptions>(std::move(options));

    return current_runtime_sender("sql::Connection::open requires current runtime")
         | stdexec::let_value([this, guard, options_state](Runtime *runtime) -> VoidSender {
               auto with_connect_timeout = [runtime, options_state](VoidSender work_sender)
                   -> VoidSender {
                   if (options_state->connect_timeout_ms <= 0) {
                       return work_sender;
                   }
                   auto work_done = std::move(work_sender) | stdexec::then([] { return true; });
                   auto timer =
                       std::make_shared<exec::asio::asio_impl::steady_timer>(runtime->executor());
                   timer->expires_after(
                       std::chrono::milliseconds(std::max(0, options_state->connect_timeout_ms)));
                   auto timeout_done =
                       timer->async_wait(exec::asio::use_sender)
                     | stdexec::then([timer]() { return false; });
                   return exec::when_any(std::move(work_done), std::move(timeout_done))
                        | stdexec::then([](bool completed) {
                              if (!completed) {
                                  throw SqlError("sql connect timeout", SqlErrorKind::connect_failed);
                              }
                          });
               };

               switch (options_state->driver) {
                   case Driver::sqlite:
#if FLUX_SQL_HAS_SQLITE3
                       return with_connect_timeout(impl_->open_sqlite_sender(*options_state, runtime))
                            | stdexec::then([this, options_state]() {
                                  impl_->options_ = *options_state;
                              });
#else
                       throw SqlError("sqlite driver is not enabled", SqlErrorKind::invalid_argument);
#endif

                   case Driver::postgres:
#if FLUX_SQL_HAS_LIBPQ
                       return with_connect_timeout(impl_->open_postgres_sender(*options_state, runtime))
                            | stdexec::then([this, options_state]() {
                                  impl_->options_ = *options_state;
                              });
#else
                       throw SqlError("postgres driver is not enabled", SqlErrorKind::invalid_argument);
#endif

                   case Driver::mysql:
#if FLUX_SQL_HAS_MYSQL
                       return with_connect_timeout(
                                  impl_->open_mysql_checked_sender(*options_state, runtime))
                            | stdexec::then([this, options_state]() {
                                  impl_->options_ = *options_state;
                              });
#else
                       throw SqlError("mysql driver is not enabled", SqlErrorKind::invalid_argument);
#endif
               }

               throw SqlError("unsupported sql driver", SqlErrorKind::internal_error);
           });
}

VoidSender Connection::close_task() {
    auto guard = std::make_shared<OperationGuard>(impl_->busy_);
    return current_runtime_sender("sql::Connection::close requires current runtime")
         | stdexec::let_value([this, guard](Runtime *runtime) -> VoidSender {
               if (impl_->options_.driver == Driver::sqlite) {
#if FLUX_SQL_HAS_SQLITE3
                   return impl_->close_sqlite_sender(runtime);
#endif
               }
               if (impl_->options_.driver == Driver::mysql) {
#if FLUX_SQL_HAS_MYSQL
                   return impl_->close_mysql_safe_sender();
#endif
               }
               return stdexec::just() | stdexec::then([this]() { impl_->close_sync(); });
           });
}

BoolSender Connection::is_open_task() const {
    return stdexec::just(impl_->is_open());
}

ConnectionOptions Connection::options() const {
    return impl_->options_;
}

QuerySender Connection::query_task(std::string sql) {
    return query_task(std::move(sql), {}, QueryOptions{});
}

QuerySender Connection::query_task(std::string sql, std::vector<SqlParam> params) {
    return query_task(std::move(sql), std::move(params), QueryOptions{});
}

QuerySender Connection::query_task(std::string sql, QueryOptions options) {
    return query_task(std::move(sql), {}, std::move(options));
}

QuerySender Connection::query_task(std::string sql,
                                   std::vector<SqlParam> params,
                                   QueryOptions query_options) {
    const auto started = std::chrono::steady_clock::now();
    flux::emit_trace_event({"layer2_sql", "query_start", 0, params.size()});

    auto guard = std::make_shared<OperationGuard>(impl_->busy_);
    return current_runtime_sender("sql::Connection::query requires current runtime")
         | stdexec::let_value(
               [this, guard, sql = std::move(sql), params = std::move(params), query_options](
                   Runtime *runtime) mutable -> QuerySender {
                   const Driver driver = impl_->options_.driver;
                   const auto timeout_ms = choose_timeout_ms(impl_->options_, query_options);

                   auto make_query_sender =
                       [this, runtime, sql = std::move(sql), params = std::move(params), driver]() mutable
                       -> QuerySender {
                       switch (driver) {
                           case Driver::sqlite:
#if FLUX_SQL_HAS_SQLITE3
                               return impl_->query_sqlite_sender(
                                   runtime, std::move(sql), std::move(params));
#else
                               throw SqlError("sqlite driver is not enabled",
                                              SqlErrorKind::invalid_argument);
#endif
                           case Driver::postgres:
#if FLUX_SQL_HAS_LIBPQ
                               return impl_->query_postgres_sender(
                                   runtime, std::move(sql), std::move(params));
#else
                               throw SqlError("postgres driver is not enabled",
                                              SqlErrorKind::invalid_argument);
#endif
                           case Driver::mysql:
#if FLUX_SQL_HAS_MYSQL
                               return impl_->query_mysql_sender(std::move(sql), std::move(params));
#else
                               throw SqlError("mysql driver is not enabled",
                                              SqlErrorKind::invalid_argument);
#endif
                       }
                       throw SqlError("unsupported sql driver", SqlErrorKind::internal_error);
                   };

                   auto query_sender = make_query_sender();
                   if (!timeout_ms.has_value()) {
                       return query_sender;
                   }

                   auto timer =
                       std::make_shared<exec::asio::asio_impl::steady_timer>(runtime->executor());
                   timer->expires_after(std::chrono::milliseconds(std::max(0, *timeout_ms)));

                   auto query_optional =
                       std::move(query_sender)
                     | stdexec::then(
                           [](QueryResult value) { return std::optional<QueryResult>(std::move(value)); });
                   auto timeout_optional =
                       timer->async_wait(exec::asio::use_sender)
                     | stdexec::then([timer]() { return std::optional<QueryResult>{}; });
                   return exec::when_any(std::move(query_optional), std::move(timeout_optional))
                        | stdexec::then([](std::optional<QueryResult> maybe_result) {
                              if (!maybe_result.has_value()) {
                                  throw SqlError("sql query timeout", SqlErrorKind::query_failed);
                              }
                              return std::move(*maybe_result);
                          });
               })
         | stdexec::then([started, guard](QueryResult out) {
               const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started);
               flux::emit_trace_event(
                   {"layer2_sql", "query_done", 0, static_cast<std::size_t>(elapsed.count())});
               return out;
           })
         | stdexec::upon_error([started, guard](std::exception_ptr error) -> QueryResult {
               const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started);
               flux::emit_trace_event(
                   {"layer2_sql", "query_done", 0, static_cast<std::size_t>(elapsed.count())});
               try {
                   std::rethrow_exception(error);
               } catch (const flux::Error &e) {
                   if (e.code() == UV_ETIMEDOUT) {
                       throw SqlError("sql query timeout", SqlErrorKind::query_failed);
                   }
                   throw;
               }
           });
}

VoidSender Connection::begin_task() {
    return query_task("BEGIN", {}, QueryOptions{}) | stdexec::then([](QueryResult) {});
}

VoidSender Connection::commit_task() {
    return query_task("COMMIT", {}, QueryOptions{}) | stdexec::then([](QueryResult) {});
}

VoidSender Connection::rollback_task() {
    return query_task("ROLLBACK", {}, QueryOptions{}) | stdexec::then([](QueryResult) {});
}

VoidSender Connection::cancel_task() {
    return stdexec::just() | stdexec::let_value([this]() -> VoidSender {
               switch (impl_->options_.driver) {
                   case Driver::sqlite:
#if FLUX_SQL_HAS_SQLITE3
                       return stdexec::just() | stdexec::then([this]() {
                                  if (impl_->sqlite_ != nullptr) {
                                      sqlite3_interrupt(impl_->sqlite_);
                                  }
                              });
#else
                       return stdexec::just();
#endif
                   case Driver::postgres:
#if FLUX_SQL_HAS_LIBPQ
                       return stdexec::just() | stdexec::then([this]() {
                                  if (impl_->pg_ == nullptr) {
                                      return;
                                  }
                                  PGcancel *cancel = PQgetCancel(impl_->pg_);
                                  if (cancel == nullptr) {
                                      return;
                                  }
                                  char errbuf[256] = {0};
                                  if (PQcancel(cancel, errbuf, sizeof(errbuf)) == 0) {
                                      PQfreeCancel(cancel);
                                      throw SqlError(
                                          errbuf[0] == '\0' ? "postgres cancel failed" : errbuf,
                                          SqlErrorKind::query_failed);
                                  }
                                  PQfreeCancel(cancel);
                              });
#else
                       return stdexec::just();
#endif
                   case Driver::mysql:
#if FLUX_SQL_HAS_MYSQL
                       if (impl_->busy_.load(std::memory_order_acquire)) {
                           return stdexec::just();
                       }
                       return impl_->close_mysql_safe_sender();
#else
                       return stdexec::just();
#endif
               }
               return stdexec::just();
           });
}

QuerySender detail::query_task(ConnectionOptions options, std::string sql) {
    return detail::query_task(std::move(options), std::move(sql), {}, QueryOptions{});
}

QuerySender detail::query_task(ConnectionOptions options,
                               std::string sql,
                               std::vector<SqlParam> params) {
    return detail::query_task(
        std::move(options), std::move(sql), std::move(params), QueryOptions{});
}

QuerySender detail::query_task(ConnectionOptions options,
                               std::string sql,
                               std::vector<SqlParam> params,
                               QueryOptions query_options) {
    auto conn = std::make_shared<Connection>();
    return conn->open(std::move(options))
         | stdexec::let_value([conn,
                               sql = std::move(sql),
                               params = std::move(params),
                               query_options = std::move(query_options)]() mutable -> QuerySender {
               return conn->query(std::move(sql), std::move(params), std::move(query_options))
                    | stdexec::let_value([conn](QueryResult result) -> QuerySender {
                          auto result_holder = std::make_shared<QueryResult>(std::move(result));
                          return conn->close()
                               | stdexec::then([result_holder]() mutable {
                                     return std::move(*result_holder);
                                 })
                               | stdexec::upon_error([result_holder](std::exception_ptr) mutable {
                                     return std::move(*result_holder);
                                 });
                      })
                    | stdexec::let_error([conn](std::exception_ptr error) -> QuerySender {
                          return conn->close()
                               | stdexec::then([error]() -> QueryResult {
                                     std::rethrow_exception(error);
                                 })
                               | stdexec::upon_error([error](std::exception_ptr) -> QueryResult {
                                     std::rethrow_exception(error);
                                 });
                      });
           });
}

class ConnectionPool::Impl {
public:
    explicit Impl(ConnectionPoolOptions options) : options_(std::move(options)) {}

    VoidSender init() {
        return stdexec::just()
             | stdexec::then([this]() {
                   if (options_.max_connections == 0) {
                       throw SqlError("connection pool max_connections must be > 0",
                                      SqlErrorKind::invalid_argument);
                   }
                   connections_.resize(options_.max_connections);
                   born_at_.resize(options_.max_connections, std::chrono::steady_clock::now());
                   available_.clear();
               })
             | stdexec::let_value([this]() -> VoidSender {
                   if (!options_.preconnect) {
                       for (std::size_t i = 0; i < options_.max_connections; ++i) {
                           available_.push_back(i);
                       }
                       available_signal_.release(
                           static_cast<std::ptrdiff_t>(options_.max_connections));
                       return stdexec::just();
                   }

                   auto loop = std::make_shared<std::function<VoidSender(std::size_t)>>();
                   *loop = [this, loop](std::size_t index) -> VoidSender {
                       if (index >= options_.max_connections) {
                           available_signal_.release(
                               static_cast<std::ptrdiff_t>(options_.max_connections));
                           return stdexec::just();
                       }
                       return connections_[index].open(options_.connection)
                            | stdexec::then([this, index]() {
                                  born_at_[index] = std::chrono::steady_clock::now();
                                  available_.push_back(index);
                              })
                            | stdexec::let_value([loop, index]() {
                                  return (*loop)(index + 1);
                              });
                   };
                   return (*loop)(0);
               });
    }

    IndexSender acquire_index() {
        auto take_index = [this](bool acquired) -> std::size_t {
            if (!acquired) {
                throw SqlError("connection pool is closed", SqlErrorKind::not_connected);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                throw SqlError("connection pool is closed", SqlErrorKind::not_connected);
            }
            if (available_.empty()) {
                throw SqlError(
                    "connection pool internal state invalid", SqlErrorKind::internal_error);
            }
            const std::size_t index = available_.front();
            available_.pop_front();
            return index;
        };

        if (options_.acquire_timeout_ms > 0) {
            return current_runtime_sender(
                       "connection pool acquire requires current runtime")
                 | stdexec::let_value([this](Runtime *runtime) {
                       auto wait_optional =
                           available_signal_.acquire_sender()
                         | stdexec::then([](bool ok) { return std::optional<bool>(ok); });

                       auto timer = std::make_shared<exec::asio::asio_impl::steady_timer>(
                           runtime->executor());
                       timer->expires_after(
                           std::chrono::milliseconds(std::max(0, options_.acquire_timeout_ms)));
                       auto timeout_optional =
                           timer->async_wait(exec::asio::use_sender)
                         | stdexec::then([timer]() { return std::optional<bool>{}; });

                       return exec::when_any(std::move(wait_optional), std::move(timeout_optional))
                            | stdexec::then([](std::optional<bool> maybe_acquired) {
                                  if (!maybe_acquired.has_value()) {
                                      throw SqlError("connection pool acquire timeout",
                                                     SqlErrorKind::query_failed);
                                  }
                                  return *maybe_acquired;
                              });
                   })
                 | stdexec::then(std::move(take_index));
        }

        return available_signal_.acquire_sender() | stdexec::then(std::move(take_index));
    }

    VoidSender ensure_healthy(std::size_t index) {
        auto reopen_connection = [this, index]() -> VoidSender {
            return connections_[index].close()
                 | stdexec::then([] {})
                 | stdexec::upon_error([](std::exception_ptr) {})
                 | stdexec::let_value([this, index]() {
                       return connections_[index].open(options_.connection)
                            | stdexec::then([this, index]() {
                                  born_at_[index] = std::chrono::steady_clock::now();
                              });
                   });
        };

        return connections_[index].is_open()
             | stdexec::let_value([this, index, reopen_connection](bool open) mutable -> VoidSender {
                   bool reopen = !open;
                   if (!reopen && options_.max_lifetime_ms > 0) {
                       const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - born_at_[index]);
                       if (age.count() >= options_.max_lifetime_ms) {
                           reopen = true;
                       }
                   }

                   if (reopen) {
                       return reopen_connection();
                   }

                   if (options_.health_check_sql.empty()) {
                       return stdexec::just();
                   }

                   return connections_[index].query(options_.health_check_sql)
                        | stdexec::then([](QueryResult) { return true; })
                        | stdexec::upon_error([](std::exception_ptr) { return false; })
                        | stdexec::let_value([reopen_connection](bool healthy) mutable -> VoidSender {
                              if (healthy) {
                                  return stdexec::just();
                              }
                              return reopen_connection();
                          });
               });
    }

    void release_index(std::size_t index) {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!closed_) {
                available_.push_back(index);
                should_notify = true;
            }
        }
        if (should_notify) {
            available_signal_.release(1);
        }
    }

    ConnectionPoolOptions options_;
    std::vector<Connection> connections_;
    std::vector<std::chrono::steady_clock::time_point> born_at_;
    std::deque<std::size_t> available_;
    flux::AsyncSemaphore available_signal_{0};
    std::mutex mutex_;
    bool closed_ = false;
};

ConnectionPool::ConnectionPool() = default;

ConnectionPool::~ConnectionPool() = default;

ConnectionPool::ConnectionPool(ConnectionPool &&) noexcept = default;

ConnectionPool &ConnectionPool::operator=(ConnectionPool &&) noexcept = default;

ConnectionPool::CreateSender ConnectionPool::create_task(ConnectionPoolOptions options) {
    auto impl = std::make_shared<Impl>(std::move(options));
    auto init_sender = impl->init();
    return std::move(init_sender) | stdexec::then([impl]() mutable {
               ConnectionPool pool;
               pool.impl_ = impl;
               return pool;
           });
}

ConnectionPool::PooledConnectionSender ConnectionPool::acquire_task() {
    if (impl_ == nullptr) {
        throw SqlError("connection pool is not initialized", SqlErrorKind::not_connected);
    }

    auto impl = impl_;
    return impl->acquire_index()
         | stdexec::let_value([impl](std::size_t index) -> PooledConnectionSender {
               return impl->ensure_healthy(index)
                    | stdexec::then([impl, index]() { return PooledConnection(impl, index); })
                    | stdexec::upon_error([impl, index](std::exception_ptr error)
                                              -> ConnectionPool::PooledConnection {
                          impl->release_index(index);
                          std::rethrow_exception(error);
                      });
           });
}

QuerySender ConnectionPool::query_task(std::string sql) {
    return query_task(std::move(sql), {}, QueryOptions{});
}

QuerySender ConnectionPool::query_task(std::string sql, std::vector<SqlParam> params) {
    return query_task(std::move(sql), std::move(params), QueryOptions{});
}

QuerySender ConnectionPool::query_task(std::string sql,
                                       std::vector<SqlParam> params,
                                       QueryOptions options) {
    return acquire_task()
         | stdexec::let_value([sql = std::move(sql),
                               params = std::move(params),
                               options = std::move(options)](auto &&lease) mutable
                                   -> QuerySender {
               auto lease_holder =
                   std::make_shared<PooledConnection>(std::move(lease));
               return lease_holder->query(std::move(sql), std::move(params), std::move(options))
                    | stdexec::then([lease_holder](QueryResult result) mutable { return result; })
                    | stdexec::upon_error(
                          [lease_holder](std::exception_ptr error) -> QueryResult {
                              std::rethrow_exception(error);
                          });
           });
}

VoidSender ConnectionPool::close_task() {
    auto impl = impl_;
    if (impl == nullptr) {
        return stdexec::just();
    }

    return stdexec::just() | stdexec::let_value([impl]() -> VoidSender {
               auto indices = std::make_shared<std::vector<std::size_t>>();
               {
                   std::lock_guard<std::mutex> lock(impl->mutex_);
                   impl->closed_ = true;
                   indices->resize(impl->connections_.size());
                   for (std::size_t i = 0; i < indices->size(); ++i) {
                       (*indices)[i] = i;
                   }
                   impl->available_.clear();
               }
               impl->available_signal_.close();

               auto loop = std::make_shared<std::function<VoidSender(std::size_t)>>();
               *loop = [impl, indices, loop](std::size_t pos) -> VoidSender {
                   if (pos >= indices->size()) {
                       return stdexec::just();
                   }
                   const std::size_t index = (*indices)[pos];
                   return impl->connections_[index].close()
                        | stdexec::then([] {})
                        | stdexec::upon_error([](std::exception_ptr) {})
                        | stdexec::let_value([loop, pos]() { return (*loop)(pos + 1); });
               };
               return (*loop)(0);
           });
}

ConnectionPool::PooledConnection::PooledConnection(std::shared_ptr<ConnectionPool::Impl> impl,
                                                   std::size_t index) noexcept
    : impl_(std::move(impl)),
      index_(index),
      held_(impl_ != nullptr) {}

ConnectionPool::PooledConnection::~PooledConnection() {
    release();
}

ConnectionPool::PooledConnection::PooledConnection(PooledConnection &&other) noexcept
    : impl_(std::move(other.impl_)),
      index_(other.index_),
      held_(other.held_) {
    other.index_ = 0;
    other.held_ = false;
}

ConnectionPool::PooledConnection &
ConnectionPool::PooledConnection::operator=(PooledConnection &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    impl_ = std::move(other.impl_);
    index_ = other.index_;
    held_ = other.held_;
    other.index_ = 0;
    other.held_ = false;
    return *this;
}

bool ConnectionPool::PooledConnection::valid() const noexcept {
    return held_ && impl_ != nullptr;
}

void ConnectionPool::PooledConnection::release() noexcept {
    if (!held_ || impl_ == nullptr) {
        return;
    }
    impl_->release_index(index_);
    held_ = false;
}

QuerySender ConnectionPool::PooledConnection::query_task(std::string sql) {
    return query_task(std::move(sql), {}, QueryOptions{});
}

QuerySender ConnectionPool::PooledConnection::query_task(std::string sql,
                                                         std::vector<SqlParam> params) {
    return query_task(std::move(sql), std::move(params), QueryOptions{});
}

QuerySender ConnectionPool::PooledConnection::query_task(std::string sql, QueryOptions options) {
    return query_task(std::move(sql), {}, std::move(options));
}

QuerySender ConnectionPool::PooledConnection::query_task(std::string sql,
                                                         std::vector<SqlParam> params,
                                                         QueryOptions options) {
    if (!valid()) {
        throw SqlError("pooled connection is not valid", SqlErrorKind::not_connected);
    }

    auto impl = impl_;
    const auto index = index_;
    return impl->connections_[index].query(std::move(sql), std::move(params), std::move(options))
         | stdexec::then([impl](QueryResult result) { return result; })
         | stdexec::upon_error([impl](std::exception_ptr error) -> QueryResult {
               std::rethrow_exception(error);
           });
}

VoidSender ConnectionPool::PooledConnection::begin_task() {
    if (!valid()) {
        throw SqlError("pooled connection is not valid", SqlErrorKind::not_connected);
    }

    auto impl = impl_;
    const auto index = index_;
    return impl->connections_[index].begin()
         | stdexec::then([impl] {})
         | stdexec::upon_error([impl](std::exception_ptr error) -> void {
               std::rethrow_exception(error);
           });
}

VoidSender ConnectionPool::PooledConnection::commit_task() {
    if (!valid()) {
        throw SqlError("pooled connection is not valid", SqlErrorKind::not_connected);
    }

    auto impl = impl_;
    const auto index = index_;
    return impl->connections_[index].commit()
         | stdexec::then([impl] {})
         | stdexec::upon_error([impl](std::exception_ptr error) -> void {
               std::rethrow_exception(error);
           });
}

VoidSender ConnectionPool::PooledConnection::rollback_task() {
    if (!valid()) {
        throw SqlError("pooled connection is not valid", SqlErrorKind::not_connected);
    }

    auto impl = impl_;
    const auto index = index_;
    return impl->connections_[index].rollback()
         | stdexec::then([impl] {})
         | stdexec::upon_error([impl](std::exception_ptr error) -> void {
               std::rethrow_exception(error);
           });
}

} // namespace flux::sql
