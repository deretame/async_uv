#include "flux_redis/redis.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ssl.hpp>
#include <boost/redis.hpp>
#include <boost/redis/src.hpp>

#include <exec/when_any.hpp>

#include "flux/async_semaphore.h"
#include "flux/runtime.h"

namespace flux::redis {
namespace {

namespace ssl = boost::asio::ssl;

class OperationGuard {
public:
    explicit OperationGuard(std::atomic_bool &flag) : flag_(flag) {
        bool expected = false;
        if (!flag_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            throw RedisError("redis client is busy with another operation",
                             RedisErrorKind::internal_error);
        }
    }

    ~OperationGuard() {
        flag_.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool &flag_;
};

std::optional<int> choose_timeout_ms(const ConnectionOptions &connection_options,
                                     const CommandOptions &command_options) {
    if (command_options.timeout_ms > 0) {
        return command_options.timeout_ms;
    }
    if (connection_options.command_timeout_ms > 0) {
        return connection_options.command_timeout_ms;
    }
    return std::nullopt;
}

std::vector<std::string> split_command(std::string_view input) {
    std::vector<std::string> out;
    std::string token;
    bool in_single = false;
    bool in_double = false;
    bool escape = false;

    for (const char ch : input) {
        if (escape) {
            token.push_back(ch);
            escape = false;
            continue;
        }

        if (ch == '\\') {
            escape = true;
            continue;
        }

        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (ch == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) && !in_single && !in_double) {
            if (!token.empty()) {
                out.push_back(std::move(token));
                token.clear();
            }
            continue;
        }

        token.push_back(ch);
    }

    if (escape || in_single || in_double) {
        throw RedisError("redis command has invalid quoting", RedisErrorKind::invalid_argument);
    }

    if (!token.empty()) {
        out.push_back(std::move(token));
    }

    if (out.empty()) {
        throw RedisError("redis command cannot be empty", RedisErrorKind::invalid_argument);
    }

    return out;
}

std::vector<std::string> materialize_args(std::string_view command,
                                          const std::vector<RedisParam> &params) {
    auto tokens = split_command(command);

    std::size_t index = 0;
    for (auto &token : tokens) {
        if (token == "?") {
            if (index >= params.size()) {
                throw RedisError("redis parameter count does not match placeholders",
                                 RedisErrorKind::invalid_argument);
            }
            token = params[index++].as_text();
        }
    }

    if (index != params.size()) {
        throw RedisError("redis parameter count does not match placeholders",
                         RedisErrorKind::invalid_argument);
    }

    return tokens;
}

std::int64_t parse_i64(std::string_view text) {
    if (text.empty()) {
        return 0;
    }

    bool neg = false;
    std::size_t i = 0;
    if (text.front() == '-') {
        neg = true;
        i = 1;
    }

    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(ch)) {
            return 0;
        }
        value = value * 10 + static_cast<std::int64_t>(ch - '0');
    }

    return neg ? -value : value;
}

Reply parse_resp_node(const std::vector<boost::redis::resp3::node> &nodes, std::size_t &index) {
    Reply out;
    if (index >= nodes.size()) {
        out.type = Reply::Type::null_value;
        return out;
    }

    const auto &node = nodes[index++];
    using Type = boost::redis::resp3::type;

    switch (node.data_type) {
        case Type::null:
            out.type = Reply::Type::null_value;
            return out;

        case Type::simple_string:
            out.type = Reply::Type::status;
            out.string = node.value;
            return out;

        case Type::blob_string:
        case Type::verbatim_string:
        case Type::big_number:
        case Type::streamed_string:
        case Type::streamed_string_part:
            out.type = Reply::Type::string;
            out.string = node.value;
            return out;

        case Type::simple_error:
        case Type::blob_error:
            out.type = Reply::Type::error;
            out.string = node.value;
            return out;

        case Type::number:
            out.type = Reply::Type::integer;
            out.integer = parse_i64(node.value);
            return out;

        case Type::boolean:
            out.type = Reply::Type::integer;
            out.integer = (node.value == "t" || node.value == "true" || node.value == "1") ? 1
                                                                                               : 0;
            return out;

        case Type::doublean:
            out.type = Reply::Type::string;
            out.string = node.value;
            return out;

        case Type::array:
        case Type::push:
        case Type::set:
        case Type::map:
        case Type::attribute: {
            out.type = Reply::Type::array;
            const std::size_t multiplier = boost::redis::resp3::element_multiplicity(node.data_type);
            const std::size_t children = node.aggregate_size * multiplier;
            out.elements.reserve(children);
            for (std::size_t i = 0; i < children; ++i) {
                out.elements.push_back(parse_resp_node(nodes, index));
            }
            return out;
        }

        case Type::invalid:
            out.type = Reply::Type::null_value;
            return out;
    }

    out.type = Reply::Type::null_value;
    return out;
}

Reply convert_response(const boost::redis::generic_response &response) {
    if (!response.has_value()) {
        const auto &err = response.error();
        throw RedisError(err.diagnostic.empty() ? "redis command failed" : err.diagnostic,
                         RedisErrorKind::command_failed);
    }

    const auto &nodes = response.value();
    if (nodes.empty()) {
        Reply out;
        out.type = Reply::Type::null_value;
        return out;
    }

    std::size_t index = 0;
    return parse_resp_node(nodes, index);
}

boost::redis::config make_redis_config(const ConnectionOptions &options) {
    boost::redis::config cfg;
    cfg.use_ssl = options.tls_enabled;
    cfg.addr.host = options.host.empty() ? "127.0.0.1" : options.host;
    cfg.addr.port = std::to_string(options.port > 0 ? options.port : 6379);
    cfg.username = options.user.empty() ? "default" : options.user;
    cfg.password = options.password;
    cfg.database_index = options.db;
    if (options.connect_timeout_ms > 0) {
        cfg.resolve_timeout = std::chrono::milliseconds(options.connect_timeout_ms);
        cfg.connect_timeout = std::chrono::milliseconds(options.connect_timeout_ms);
        cfg.ssl_handshake_timeout = std::chrono::milliseconds(options.connect_timeout_ms);
    }
    return cfg;
}

ssl::context make_tls_context(const ConnectionOptions &options) {
    ssl::context ctx(ssl::context::tls_client);
    boost::system::error_code ec;

    if (options.tls_verify_peer) {
        ctx.set_verify_mode(ssl::verify_peer, ec);
        if (ec) {
            throw RedisError("redis tls verify mode failed: " + ec.message(),
                             RedisErrorKind::connect_failed);
        }

        if (!options.tls_ca_cert_file.empty()) {
            ctx.load_verify_file(options.tls_ca_cert_file, ec);
            if (ec) {
                throw RedisError("redis tls load ca file failed: " + ec.message(),
                                 RedisErrorKind::connect_failed);
            }
        } else {
            ctx.set_default_verify_paths(ec);
            if (ec) {
                throw RedisError("redis tls default verify paths failed: " + ec.message(),
                                 RedisErrorKind::connect_failed);
            }
        }
    } else {
        ctx.set_verify_mode(ssl::verify_none, ec);
        if (ec) {
            throw RedisError("redis tls disable verify failed: " + ec.message(),
                             RedisErrorKind::connect_failed);
        }
    }

    if (!options.tls_cert_file.empty() && !options.tls_key_file.empty()) {
        ctx.use_certificate_chain_file(options.tls_cert_file, ec);
        if (ec) {
            throw RedisError("redis tls load cert file failed: " + ec.message(),
                             RedisErrorKind::connect_failed);
        }

        ctx.use_private_key_file(options.tls_key_file, ssl::context::pem, ec);
        if (ec) {
            throw RedisError("redis tls load key file failed: " + ec.message(),
                             RedisErrorKind::connect_failed);
        }
    }

    return ctx;
}

template <typename StartFn>
class RedisExecSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(boost::system::error_code, std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    explicit RedisExecSender(StartFn start) : start_(std::move(start)) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const RedisExecSender &,
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
                start([this](boost::system::error_code ec, std::size_t transferred) mutable noexcept {
                    stdexec::set_value(std::move(receiver), ec, transferred);
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
    friend auto tag_invoke(stdexec::connect_t, RedisExecSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.start_), std::move(receiver)};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const RedisExecSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.start_, std::move(receiver)};
    }

    StartFn start_;
};

template <typename StartFn>
auto make_redis_exec_sender(StartFn &&start) {
    return RedisExecSender<std::remove_cvref_t<StartFn>>(std::forward<StartFn>(start));
}

} // namespace

RedisParam::RedisParam(std::nullptr_t) : value_(std::nullopt) {}

RedisParam::RedisParam(const char *value)
    : value_(value == nullptr ? std::optional<std::string>{} : std::optional<std::string>{value}) {}

RedisParam::RedisParam(std::string value) : value_(std::move(value)) {}

RedisParam::RedisParam(std::string_view value) : value_(std::string(value)) {}

RedisParam::RedisParam(std::optional<std::string> value) : value_(std::move(value)) {}

RedisParam::RedisParam(bool value) : value_(value ? "1" : "0") {}

RedisParam::RedisParam(std::int32_t value) : value_(std::to_string(value)) {}

RedisParam::RedisParam(std::uint32_t value) : value_(std::to_string(value)) {}

RedisParam::RedisParam(std::int64_t value) : value_(std::to_string(value)) {}

RedisParam::RedisParam(std::uint64_t value) : value_(std::to_string(value)) {}

RedisParam::RedisParam(double value) {
    std::ostringstream out;
    out << value;
    value_ = out.str();
}

bool RedisParam::is_null() const noexcept {
    return !value_.has_value();
}

std::string RedisParam::as_text() const {
    return value_.value_or("");
}

CommandOptions::Builder CommandOptions::builder() {
    return Builder{};
}

CommandOptions::Builder &CommandOptions::Builder::timeout_ms(int value) {
    timeout_ms_ = value;
    return *this;
}

CommandOptions CommandOptions::Builder::build() const {
    CommandOptions out;
    out.timeout_ms = timeout_ms_;
    return out;
}

ConnectionOptions::Builder ConnectionOptions::builder() {
    return Builder{};
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

ConnectionOptions::Builder &ConnectionOptions::Builder::db(int value) {
    db_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::connect_timeout_ms(int value) {
    connect_timeout_ms_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::command_timeout_ms(int value) {
    command_timeout_ms_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_enabled(bool value) {
    tls_enabled_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_verify_peer(bool value) {
    tls_verify_peer_ = value;
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_ca_cert_file(std::string value) {
    tls_ca_cert_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_ca_cert_dir(std::string value) {
    tls_ca_cert_dir_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_cert_file(std::string value) {
    tls_cert_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_key_file(std::string value) {
    tls_key_file_ = std::move(value);
    return *this;
}

ConnectionOptions::Builder &ConnectionOptions::Builder::tls_server_name(std::string value) {
    tls_server_name_ = std::move(value);
    return *this;
}

ConnectionOptions ConnectionOptions::Builder::build() const {
    ConnectionOptions out;
    out.host = host_;
    out.port = port_;
    out.user = user_;
    out.password = password_;
    out.db = db_;
    out.connect_timeout_ms = connect_timeout_ms_;
    out.command_timeout_ms = command_timeout_ms_;
    out.tls_enabled = tls_enabled_;
    out.tls_verify_peer = tls_verify_peer_;
    out.tls_ca_cert_file = tls_ca_cert_file_;
    out.tls_ca_cert_dir = tls_ca_cert_dir_;
    out.tls_cert_file = tls_cert_file_;
    out.tls_key_file = tls_key_file_;
    out.tls_server_name = tls_server_name_;
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
ConnectionPoolOptions::Builder::health_check_command(std::string value) {
    health_check_command_ = std::move(value);
    return *this;
}

ConnectionPoolOptions ConnectionPoolOptions::Builder::build() const {
    ConnectionPoolOptions out;
    out.connection = connection_;
    out.max_connections = max_connections_;
    out.preconnect = preconnect_;
    out.acquire_timeout_ms = acquire_timeout_ms_;
    out.max_lifetime_ms = max_lifetime_ms_;
    out.health_check_command = health_check_command_;
    return out;
}

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
                    throw RedisError(message, RedisErrorKind::runtime_missing);
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

class Client::Impl {
public:
    void close_async_state() noexcept {
        open_.store(false, std::memory_order_release);
        if (conn_) {
            try {
                conn_->cancel(boost::redis::operation::all);
            } catch (...) {
            }
        }
        conn_.reset();
        runtime_ = nullptr;
    }

    void start_run_loop(const ConnectionOptions &options) {
        if (!conn_) {
            return;
        }
        const auto cfg = make_redis_config(options);
        conn_->async_run(cfg, boost::redis::logger{}, [](boost::system::error_code) {});
    }

    ReplySender execute_sender(std::vector<std::string> args,
                               std::optional<int> timeout_ms,
                               Runtime *runtime) {
        auto conn = conn_;
        if (!open_.load(std::memory_order_acquire) || !conn) {
            throw RedisError("redis connection is not open", RedisErrorKind::not_connected);
        }
        if (args.empty()) {
            throw RedisError("redis command cannot be empty", RedisErrorKind::invalid_argument);
        }

        auto request = std::make_shared<boost::redis::request>();
        if (args.size() == 1) {
            request->push(args.front());
        } else {
            std::vector<std::string> rest(args.begin() + 1, args.end());
            request->push_range(args.front(), rest);
        }

        auto response = std::make_shared<boost::redis::generic_response>();
        auto execute = make_redis_exec_sender(
                           [conn, request, response](auto handler) mutable {
                               conn->async_exec(*request, *response, std::move(handler));
                           })
                     | stdexec::then([response](boost::system::error_code ec, std::size_t) {
                           if (ec) {
                               throw RedisError("redis command failed: " + ec.message(),
                                                RedisErrorKind::command_failed);
                           }
                           Reply out = convert_response(*response);
                           if (out.type == Reply::Type::error) {
                               throw RedisError(out.string.value_or("redis command failed"),
                                                RedisErrorKind::command_failed);
                           }
                           return out;
                       });

        if (!timeout_ms.has_value()) {
            return execute;
        }
        if (runtime == nullptr) {
            throw RedisError("redis::Client::command requires current runtime",
                             RedisErrorKind::runtime_missing);
        }

        auto timer = std::make_shared<exec::asio::asio_impl::steady_timer>(runtime->executor());
        timer->expires_after(std::chrono::milliseconds(std::max(0, *timeout_ms)));

        auto execute_optional =
            std::move(execute)
          | stdexec::then([](Reply reply) { return std::optional<Reply>(std::move(reply)); });
        auto timeout_optional = timer->async_wait(exec::asio::use_sender)
                              | stdexec::then([timer, conn]() {
                                    conn->cancel(boost::redis::operation::exec);
                                    return std::optional<Reply>{};
                                });
        return exec::when_any(std::move(execute_optional), std::move(timeout_optional))
             | stdexec::then([](std::optional<Reply> maybe_reply) {
                   if (!maybe_reply.has_value()) {
                       throw RedisError("redis command timeout", RedisErrorKind::command_failed);
                   }
                   return std::move(*maybe_reply);
               });
    }

    ConnectionOptions options_{};
    std::atomic_bool busy_{false};
    std::atomic_bool open_{false};
    Runtime *runtime_ = nullptr;
    std::shared_ptr<boost::redis::connection> conn_;
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    if (impl_) {
        impl_->close_async_state();
    }
}

Client::Client(Client &&) noexcept = default;

Client &Client::operator=(Client &&) noexcept = default;

VoidSender Client::open_task(ConnectionOptions options) {
    auto guard = std::make_shared<OperationGuard>(impl_->busy_);
    auto options_state = std::make_shared<ConnectionOptions>(std::move(options));

    return current_runtime_sender("redis::Client::open requires current runtime")
         | stdexec::let_value([this, guard, options_state](Runtime *runtime) -> VoidSender {
               return stdexec::just()
                    | stdexec::then([this, runtime, options_state] {
                          impl_->close_async_state();
                          impl_->runtime_ = runtime;

                          ssl::context tls_ctx = options_state->tls_enabled
                                                   ? make_tls_context(*options_state)
                                                   : ssl::context(ssl::context::tls_client);
                          impl_->conn_ = std::make_shared<boost::redis::connection>(
                              runtime->io_context().get_executor(), std::move(tls_ctx));
                          impl_->options_ = *options_state;
                          impl_->open_.store(true, std::memory_order_release);
                          impl_->start_run_loop(*options_state);
                      })
                    | stdexec::let_value([this, runtime, options_state] -> ReplySender {
                          std::optional<int> timeout_ms;
                          if (options_state->connect_timeout_ms > 0) {
                              timeout_ms = options_state->connect_timeout_ms;
                          }
                          return impl_->execute_sender({"PING"}, timeout_ms, runtime);
                      })
                    | stdexec::then([](Reply) {})
                    | stdexec::let_error([this](std::exception_ptr error) -> VoidSender {
                          impl_->close_async_state();
                          return stdexec::just() | stdexec::then([error] {
                                     std::rethrow_exception(error);
                                 });
                      });
           });
}

VoidSender Client::close_task() {
    auto guard = std::make_shared<OperationGuard>(impl_->busy_);
    return stdexec::just() | stdexec::then([this, guard] { impl_->close_async_state(); });
}

BoolSender Client::is_open_task() const {
    return stdexec::just(impl_ != nullptr && impl_->open_.load(std::memory_order_acquire));
}

ReplySender Client::command_task(std::string command_text) {
    return command_task(std::move(command_text), {}, CommandOptions{});
}

ReplySender Client::command_task(std::string command_text, std::vector<RedisParam> params) {
    return command_task(std::move(command_text), std::move(params), CommandOptions{});
}

ReplySender Client::command_task(std::string command_text,
                                 std::vector<RedisParam> params,
                                 CommandOptions options) {
    auto guard = std::make_shared<OperationGuard>(impl_->busy_);
    const auto started = std::chrono::steady_clock::now();
    flux::emit_trace_event({"layer2_redis", "command_start", 0, params.size()});

    return current_runtime_sender("redis::Client::command requires current runtime")
         | stdexec::let_value([this,
                               guard,
                               command_text = std::move(command_text),
                               params = std::move(params),
                               options](Runtime *runtime) mutable -> ReplySender {
               const auto args = materialize_args(command_text, params);
               const auto timeout_ms = choose_timeout_ms(impl_->options_, options);
               return impl_->execute_sender(std::move(args), timeout_ms, runtime);
           })
         | stdexec::then([started, guard](Reply out) {
               const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started);
               flux::emit_trace_event(
                   {"layer2_redis", "command_done", 0, static_cast<std::size_t>(elapsed.count())});
               return out;
           })
         | stdexec::upon_error([started, guard](std::exception_ptr error) -> Reply {
               const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started);
               flux::emit_trace_event(
                   {"layer2_redis", "command_done", 0, static_cast<std::size_t>(elapsed.count())});
               std::rethrow_exception(error);
           });
}

ReplySender detail::command_task(ConnectionOptions options, std::string command_text) {
    return detail::command_task(std::move(options), std::move(command_text), {}, CommandOptions{});
}

ReplySender detail::command_task(ConnectionOptions options,
                                 std::string command_text,
                                 std::vector<RedisParam> params) {
    return detail::command_task(
        std::move(options), std::move(command_text), std::move(params), CommandOptions{});
}

ReplySender detail::command_task(ConnectionOptions options,
                                 std::string command_text,
                                 std::vector<RedisParam> params,
                                 CommandOptions command_options) {
    auto client = std::make_shared<Client>();
    return client->open(std::move(options))
         | stdexec::let_value([client,
                               command_text = std::move(command_text),
                               params = std::move(params),
                               command_options = std::move(command_options)]() mutable -> ReplySender {
               return client->command(
                                std::move(command_text), std::move(params), std::move(command_options))
                    | stdexec::let_value([client](Reply result) -> ReplySender {
                          auto result_holder = std::make_shared<Reply>(std::move(result));
                          return client->close()
                               | stdexec::then([result_holder]() { return std::move(*result_holder); })
                               | stdexec::upon_error(
                                     [result_holder](std::exception_ptr) -> Reply {
                                         return std::move(*result_holder);
                                     });
                      })
                    | stdexec::let_error([client](std::exception_ptr error) -> ReplySender {
                          return client->close()
                               | stdexec::then([error]() -> Reply {
                                     std::rethrow_exception(error);
                                 })
                               | stdexec::upon_error([error](std::exception_ptr) -> Reply {
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
             | stdexec::then([this] {
                   if (options_.max_connections == 0) {
                       throw RedisError("redis pool max_connections must be > 0",
                                        RedisErrorKind::invalid_argument);
                   }
                   clients_.resize(options_.max_connections);
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
                       return clients_[index].open(options_.connection)
                            | stdexec::then([this, index] {
                                  born_at_[index] = std::chrono::steady_clock::now();
                                  available_.push_back(index);
                              })
                            | stdexec::let_value([loop, index] { return (*loop)(index + 1); });
                   };
                   return (*loop)(0);
               });
    }

    IndexSender acquire_index() {
        auto take_index = [this](bool acquired) -> std::size_t {
            if (!acquired) {
                throw RedisError("redis pool is closed", RedisErrorKind::not_connected);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                throw RedisError("redis pool is closed", RedisErrorKind::not_connected);
            }
            if (available_.empty()) {
                throw RedisError("redis pool internal state invalid", RedisErrorKind::internal_error);
            }
            const std::size_t index = available_.front();
            available_.pop_front();
            return index;
        };

        if (options_.acquire_timeout_ms > 0) {
            return current_runtime_sender("redis pool acquire requires current runtime")
                 | stdexec::let_value([this](Runtime *runtime) {
                       auto wait_optional = available_signal_.acquire_sender()
                                          | stdexec::then(
                                                [](bool ok) { return std::optional<bool>(ok); });
                       auto timer = std::make_shared<exec::asio::asio_impl::steady_timer>(
                           runtime->executor());
                       timer->expires_after(
                           std::chrono::milliseconds(std::max(0, options_.acquire_timeout_ms)));
                       auto timeout_optional = timer->async_wait(exec::asio::use_sender)
                                             | stdexec::then(
                                                   [timer]() { return std::optional<bool>{}; });

                       return exec::when_any(std::move(wait_optional), std::move(timeout_optional))
                            | stdexec::then([](std::optional<bool> maybe_acquired) {
                                  if (!maybe_acquired.has_value()) {
                                      throw RedisError("redis pool acquire timeout",
                                                       RedisErrorKind::command_failed);
                                  }
                                  return *maybe_acquired;
                              });
                   })
                 | stdexec::then(std::move(take_index));
        }

        return available_signal_.acquire_sender() | stdexec::then(std::move(take_index));
    }

    VoidSender ensure_healthy(std::size_t index) {
        auto reopen_client = [this, index]() -> VoidSender {
            return clients_[index].close()
                 | stdexec::then([] {})
                 | stdexec::upon_error([](std::exception_ptr) {})
                 | stdexec::let_value([this, index]() {
                       return clients_[index].open(options_.connection)
                            | stdexec::then([this, index] {
                                  born_at_[index] = std::chrono::steady_clock::now();
                              });
                   });
        };

        return clients_[index].is_open()
             | stdexec::let_value([this, index, reopen_client](bool open) mutable -> VoidSender {
                   bool reopen = !open;
                   if (!reopen && options_.max_lifetime_ms > 0) {
                       const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - born_at_[index]);
                       if (age.count() >= options_.max_lifetime_ms) {
                           reopen = true;
                       }
                   }
                   if (reopen) {
                       return reopen_client();
                   }
                   if (options_.health_check_command.empty()) {
                       return stdexec::just();
                   }

                   return clients_[index].command(options_.health_check_command)
                        | stdexec::then([](Reply) { return true; })
                        | stdexec::upon_error([](std::exception_ptr) { return false; })
                        | stdexec::let_value([reopen_client](bool healthy) mutable -> VoidSender {
                              if (healthy) {
                                  return stdexec::just();
                              }
                              return reopen_client();
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
    std::vector<Client> clients_;
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
    return std::move(init_sender) | stdexec::then([impl] {
               ConnectionPool pool;
               pool.impl_ = impl;
               return pool;
           });
}

ReplySender ConnectionPool::command_task(std::string command_text) {
    return command_task(std::move(command_text), {}, CommandOptions{});
}

ReplySender ConnectionPool::command_task(std::string command_text,
                                         std::vector<RedisParam> params) {
    return command_task(std::move(command_text), std::move(params), CommandOptions{});
}

ReplySender ConnectionPool::command_task(std::string command_text,
                                         std::vector<RedisParam> params,
                                         CommandOptions options) {
    if (impl_ == nullptr) {
        throw RedisError("redis pool is not initialized", RedisErrorKind::not_connected);
    }

    auto impl = impl_;
    struct CommandPayload {
        std::string command_text;
        std::vector<RedisParam> params;
        CommandOptions options;
    };
    auto payload = std::make_shared<CommandPayload>(
        CommandPayload{std::move(command_text), std::move(params), std::move(options)});

    return impl->acquire_index()
         | stdexec::let_value([impl, payload](std::size_t index) -> ReplySender {
               return impl->ensure_healthy(index)
                    | stdexec::let_value([impl, payload, index]() -> ReplySender {
                          return impl->clients_[index].command(
                              payload->command_text, payload->params, payload->options);
                      })
                    | stdexec::then([impl, index](Reply reply) {
                          impl->release_index(index);
                          return reply;
                      })
                    | stdexec::upon_error([impl, index](std::exception_ptr error) -> Reply {
                          impl->release_index(index);
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
                   indices->resize(impl->clients_.size());
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
                   return impl->clients_[index].close()
                        | stdexec::then([] {})
                        | stdexec::upon_error([](std::exception_ptr) {})
                        | stdexec::let_value([loop, pos] { return (*loop)(pos + 1); });
               };
               return (*loop)(0);
           });
}

} // namespace flux::redis
