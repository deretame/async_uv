#include "async_uv_redis/redis.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#if ASYNC_UV_REDIS_HAS_HIREDIS && __has_include(<hiredis/hiredis.h>)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <hiredis/hiredis.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#elif ASYNC_UV_REDIS_HAS_HIREDIS && __has_include(<hiredis.h>)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <hiredis.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#elif ASYNC_UV_REDIS_HAS_HIREDIS
#error "hiredis header not found"
#endif

#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL && __has_include(<hiredis/hiredis_ssl.h>)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <hiredis/hiredis_ssl.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#elif ASYNC_UV_REDIS_HAS_HIREDIS_SSL && __has_include(<hiredis_ssl.h>)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <hiredis_ssl.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#elif ASYNC_UV_REDIS_HAS_HIREDIS_SSL
#error "hiredis_ssl header not found"
#endif

#include "async_uv/cancel.h"
#include "async_uv/error.h"
#include "async_uv/fd.h"
#include "async_uv/runtime.h"

namespace async_uv::redis {
namespace {

class OperationGuard {
public:
    explicit OperationGuard(std::atomic_bool &flag) : flag_(flag) {
        bool expected = false;
        if (!flag_.compare_exchange_strong(expected, true)) {
            throw RedisError("redis client is busy with another operation",
                             RedisErrorKind::internal_error);
        }
    }

    ~OperationGuard() {
        flag_.store(false);
    }

private:
    std::atomic_bool &flag_;
};

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
        throw RedisError("invalid redis socket fd", RedisErrorKind::internal_error);
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
        throw RedisError("redis socket wait timeout", RedisErrorKind::command_failed);
    }
    if (!event->ok()) {
        throw RedisError("redis socket wait returned error", RedisErrorKind::command_failed);
    }
}

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

#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
redisSSLContext *create_ssl_context(const ConnectionOptions &options) {
    static std::once_flag once;
    std::call_once(once, [] {
        (void)redisInitOpenSSL();
    });

    redisSSLOptions ssl_options{};
    ssl_options.cacert_filename =
        options.tls_ca_cert_file.empty() ? nullptr : options.tls_ca_cert_file.c_str();
    ssl_options.capath =
        options.tls_ca_cert_dir.empty() ? nullptr : options.tls_ca_cert_dir.c_str();
    ssl_options.cert_filename =
        options.tls_cert_file.empty() ? nullptr : options.tls_cert_file.c_str();
    ssl_options.private_key_filename =
        options.tls_key_file.empty() ? nullptr : options.tls_key_file.c_str();
    ssl_options.server_name =
        options.tls_server_name.empty() ? nullptr : options.tls_server_name.c_str();
    ssl_options.verify_mode =
        options.tls_verify_peer ? REDIS_SSL_VERIFY_PEER : REDIS_SSL_VERIFY_NONE;

    redisSSLContextError error = REDIS_SSL_CTX_NONE;
    redisSSLContext *ctx = redisCreateSSLContextWithOptions(&ssl_options, &error);
    if (ctx == nullptr) {
        throw RedisError(std::string("redis tls context create failed: ") +
                             redisSSLContextGetError(error),
                         RedisErrorKind::connect_failed);
    }
    return ctx;
}
#endif

#if ASYNC_UV_REDIS_HAS_HIREDIS
Reply from_raw_reply(const redisReply *reply) {
    Reply out;
    if (reply == nullptr) {
        out.type = Reply::Type::null_value;
        return out;
    }

    switch (reply->type) {
        case REDIS_REPLY_STRING:
            out.type = Reply::Type::string;
            out.string =
                reply->str == nullptr ? std::string() : std::string(reply->str, reply->len);
            return out;
        case REDIS_REPLY_STATUS:
            out.type = Reply::Type::status;
            out.string =
                reply->str == nullptr ? std::string() : std::string(reply->str, reply->len);
            return out;
        case REDIS_REPLY_ERROR:
            out.type = Reply::Type::error;
            out.string =
                reply->str == nullptr ? std::string() : std::string(reply->str, reply->len);
            return out;
        case REDIS_REPLY_INTEGER:
            out.type = Reply::Type::integer;
            out.integer = reply->integer;
            return out;
        case REDIS_REPLY_NIL:
            out.type = Reply::Type::null_value;
            return out;
        case REDIS_REPLY_ARRAY:
            out.type = Reply::Type::array;
            out.elements.reserve(reply->elements);
            for (size_t i = 0; i < reply->elements; ++i) {
                out.elements.push_back(from_raw_reply(reply->element[i]));
            }
            return out;
        default:
            out.type = Reply::Type::null_value;
            return out;
    }
}
#endif

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

class Client::Impl {
public:
#if ASYNC_UV_REDIS_HAS_HIREDIS
    Reply command_blocking(const std::vector<std::string> &args) {
        if (redis_ == nullptr) {
            throw RedisError("redis connection is not open", RedisErrorKind::not_connected);
        }

        std::vector<const char *> argv;
        std::vector<size_t> argvlen;
        argv.reserve(args.size());
        argvlen.reserve(args.size());
        for (const auto &arg : args) {
            argv.push_back(arg.c_str());
            argvlen.push_back(arg.size());
        }

        redisReply *reply = reinterpret_cast<redisReply *>(
            redisCommandArgv(redis_, static_cast<int>(argv.size()), argv.data(), argvlen.data()));
        if (reply == nullptr) {
            throw RedisError(redis_->errstr[0] == '\0' ? "redis command failed"
                                                       : std::string(redis_->errstr),
                             RedisErrorKind::command_failed);
        }

        Reply out = from_raw_reply(reply);
        freeReplyObject(reply);
        if (out.type == Reply::Type::error) {
            throw RedisError(out.string.value_or("redis command failed"),
                             RedisErrorKind::command_failed);
        }
        return out;
    }

    Task<void> write_output(std::optional<int> timeout_ms) {
        if (redis_ == nullptr) {
            throw RedisError("redis connection is not open", RedisErrorKind::not_connected);
        }

        int done = 0;
        while (done == 0) {
            if (redisBufferWrite(redis_, &done) == REDIS_ERR) {
                throw RedisError(redis_->errstr[0] == '\0' ? "redis write failed"
                                                           : std::string(redis_->errstr),
                                 RedisErrorKind::command_failed);
            }
            if (done == 0) {
                co_await wait_fd_ready(
                    static_cast<uv_os_sock_t>(redis_->fd), false, true, timeout_ms);
            }
        }
        co_return;
    }

    Task<redisReply *> read_reply(std::optional<int> timeout_ms) {
        if (redis_ == nullptr) {
            throw RedisError("redis connection is not open", RedisErrorKind::not_connected);
        }

        while (true) {
            void *reply = nullptr;
            if (redisGetReplyFromReader(redis_, &reply) == REDIS_ERR) {
                throw RedisError(redis_->errstr[0] == '\0' ? "redis read failed"
                                                           : std::string(redis_->errstr),
                                 RedisErrorKind::command_failed);
            }
            if (reply != nullptr) {
                co_return reinterpret_cast<redisReply *>(reply);
            }

            co_await wait_fd_ready(static_cast<uv_os_sock_t>(redis_->fd), true, false, timeout_ms);
            if (redisBufferRead(redis_) == REDIS_ERR) {
                throw RedisError(redis_->errstr[0] == '\0' ? "redis socket read failed"
                                                           : std::string(redis_->errstr),
                                 RedisErrorKind::command_failed);
            }
        }
    }

    Task<Reply> command_async(const std::vector<std::string> &args, std::optional<int> timeout_ms) {
        if (redis_ == nullptr) {
            throw RedisError("redis connection is not open", RedisErrorKind::not_connected);
        }

        std::vector<const char *> argv;
        std::vector<size_t> argvlen;
        argv.reserve(args.size());
        argvlen.reserve(args.size());
        for (const auto &arg : args) {
            argv.push_back(arg.c_str());
            argvlen.push_back(arg.size());
        }

        if (redisAppendCommandArgv(
                redis_, static_cast<int>(argv.size()), argv.data(), argvlen.data()) != REDIS_OK) {
            throw RedisError(redis_->errstr[0] == '\0' ? "redis append command failed"
                                                       : std::string(redis_->errstr),
                             RedisErrorKind::command_failed);
        }

        co_await write_output(timeout_ms);
        redisReply *reply = co_await read_reply(timeout_ms);
        Reply out = from_raw_reply(reply);
        freeReplyObject(reply);

        if (out.type == Reply::Type::error) {
            throw RedisError(out.string.value_or("redis command failed"),
                             RedisErrorKind::command_failed);
        }
        co_return out;
    }
#endif

    ConnectionOptions options_{};
    std::atomic_bool busy_{false};
    std::mutex blocking_mutex_;
#if ASYNC_UV_REDIS_HAS_HIREDIS
    redisContext *redis_ = nullptr;
    bool tls_mode_ = false;
#endif
#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
    redisSSLContext *redis_ssl_ctx_ = nullptr;
#endif
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    if (!impl_) {
        return;
    }
#if ASYNC_UV_REDIS_HAS_HIREDIS
    if (impl_->redis_ != nullptr) {
        redisFree(impl_->redis_);
        impl_->redis_ = nullptr;
    }
#endif
#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
    if (impl_->redis_ssl_ctx_ != nullptr) {
        redisFreeSSLContext(impl_->redis_ssl_ctx_);
        impl_->redis_ssl_ctx_ = nullptr;
    }
#endif
}

Client::Client(Client &&) noexcept = default;

Client &Client::operator=(Client &&) noexcept = default;

Task<void> Client::open(ConnectionOptions options) {
#if !ASYNC_UV_REDIS_HAS_HIREDIS
    (void)options;
    throw RedisError("redis driver is not enabled", RedisErrorKind::invalid_argument);
#else
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        throw RedisError("redis::Client::open requires current runtime",
                         RedisErrorKind::runtime_missing);
    }
    (void)runtime;

    OperationGuard guard(impl_->busy_);

    if (impl_->redis_ != nullptr) {
        redisFree(impl_->redis_);
        impl_->redis_ = nullptr;
    }

#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
    if (impl_->redis_ssl_ctx_ != nullptr) {
        redisFreeSSLContext(impl_->redis_ssl_ctx_);
        impl_->redis_ssl_ctx_ = nullptr;
    }
#endif
    impl_->tls_mode_ = false;

    if (options.tls_enabled) {
        impl_->redis_ = redisConnect(options.host.c_str(), options.port);
    } else {
        impl_->redis_ = redisConnectNonBlock(options.host.c_str(), options.port);
    }
    if (impl_->redis_ == nullptr) {
        throw RedisError("redis connect failed", RedisErrorKind::connect_failed);
    }
    if (impl_->redis_->err != 0) {
        const std::string message = impl_->redis_->errstr[0] == '\0'
                                        ? "redis connect failed"
                                        : std::string(impl_->redis_->errstr);
        redisFree(impl_->redis_);
        impl_->redis_ = nullptr;
        throw RedisError(message, RedisErrorKind::connect_failed);
    }

    if (options.tls_enabled) {
#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
        impl_->redis_ssl_ctx_ = create_ssl_context(options);
        if (redisInitiateSSLWithContext(impl_->redis_, impl_->redis_ssl_ctx_) != REDIS_OK) {
            const std::string message = impl_->redis_->errstr[0] == '\0'
                                            ? "redis tls handshake init failed"
                                            : std::string(impl_->redis_->errstr);
            redisFree(impl_->redis_);
            impl_->redis_ = nullptr;
            redisFreeSSLContext(impl_->redis_ssl_ctx_);
            impl_->redis_ssl_ctx_ = nullptr;
            throw RedisError(message, RedisErrorKind::connect_failed);
        }
#else
        redisFree(impl_->redis_);
        impl_->redis_ = nullptr;
        throw RedisError("redis tls requested but hiredis ssl is not enabled",
                         RedisErrorKind::invalid_argument);
#endif
        impl_->tls_mode_ = true;
    } else {
        impl_->tls_mode_ = false;
    }

    const std::optional<int> timeout_ms = options.connect_timeout_ms > 0
                                              ? std::optional<int>(options.connect_timeout_ms)
                                              : std::nullopt;

    auto run = [this, runtime, timeout_ms, options]() -> Task<void> {
        if (impl_->tls_mode_) {
            auto error = std::make_shared<std::exception_ptr>();
            co_await runtime->spawn_blocking([this, options, error]() {
                try {
                    std::lock_guard<std::mutex> lock(impl_->blocking_mutex_);
                    (void)impl_->command_blocking({"PING"});
                    if (!options.password.empty()) {
                        if (options.user.empty()) {
                            (void)impl_->command_blocking({"AUTH", options.password});
                        } else {
                            (void)impl_->command_blocking({"AUTH", options.user, options.password});
                        }
                    }
                    if (options.db > 0) {
                        (void)impl_->command_blocking({"SELECT", std::to_string(options.db)});
                    }
                } catch (...) {
                    *error = std::current_exception();
                }
            });
            if (*error) {
                std::rethrow_exception(*error);
            }
            co_return;
        }

        (void)co_await impl_->command_async({"PING"}, timeout_ms);
        if (!options.password.empty()) {
            if (options.user.empty()) {
                (void)co_await impl_->command_async({"AUTH", options.password}, timeout_ms);
            } else {
                (void)co_await impl_->command_async({"AUTH", options.user, options.password},
                                                    timeout_ms);
            }
        }
        if (options.db > 0) {
            (void)co_await impl_->command_async({"SELECT", std::to_string(options.db)}, timeout_ms);
        }
        co_return;
    };

    try {
        if (timeout_ms.has_value()) {
            co_await async_uv::with_timeout(std::chrono::milliseconds(*timeout_ms), run());
        } else {
            co_await run();
        }
    } catch (const async_uv::Error &e) {
        if (e.code() == UV_ETIMEDOUT) {
            redisFree(impl_->redis_);
            impl_->redis_ = nullptr;
#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
            if (impl_->redis_ssl_ctx_ != nullptr) {
                redisFreeSSLContext(impl_->redis_ssl_ctx_);
                impl_->redis_ssl_ctx_ = nullptr;
            }
#endif
            throw RedisError("redis connect timeout", RedisErrorKind::connect_failed);
        }
        throw;
    } catch (...) {
        redisFree(impl_->redis_);
        impl_->redis_ = nullptr;
#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
        if (impl_->redis_ssl_ctx_ != nullptr) {
            redisFreeSSLContext(impl_->redis_ssl_ctx_);
            impl_->redis_ssl_ctx_ = nullptr;
        }
#endif
        throw;
    }

    impl_->options_ = std::move(options);
    co_return;
#endif
}

Task<void> Client::close() {
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        throw RedisError("redis::Client::close requires current runtime",
                         RedisErrorKind::runtime_missing);
    }
    (void)runtime;

    OperationGuard guard(impl_->busy_);
#if ASYNC_UV_REDIS_HAS_HIREDIS
    if (impl_->redis_ != nullptr) {
        redisFree(impl_->redis_);
        impl_->redis_ = nullptr;
    }
    impl_->tls_mode_ = false;
#endif
#if ASYNC_UV_REDIS_HAS_HIREDIS_SSL
    if (impl_->redis_ssl_ctx_ != nullptr) {
        redisFreeSSLContext(impl_->redis_ssl_ctx_);
        impl_->redis_ssl_ctx_ = nullptr;
    }
#endif
    co_return;
}

Task<bool> Client::is_open() const {
#if ASYNC_UV_REDIS_HAS_HIREDIS
    co_return impl_ != nullptr && impl_->redis_ != nullptr;
#else
    co_return false;
#endif
}

Task<Reply> Client::command(std::string command_text) {
    co_return co_await command(std::move(command_text), {}, CommandOptions{});
}

Task<Reply> Client::command(std::string command_text, std::vector<RedisParam> params) {
    co_return co_await command(std::move(command_text), std::move(params), CommandOptions{});
}

Task<Reply>
Client::command(std::string command_text, std::vector<RedisParam> params, CommandOptions options) {
#if !ASYNC_UV_REDIS_HAS_HIREDIS
    (void)command_text;
    (void)params;
    (void)options;
    throw RedisError("redis driver is not enabled", RedisErrorKind::invalid_argument);
#else
    auto *runtime = co_await async_uv::get_current_runtime();
    if (runtime == nullptr) {
        throw RedisError("redis::Client::command requires current runtime",
                         RedisErrorKind::runtime_missing);
    }
    (void)runtime;

    OperationGuard guard(impl_->busy_);
    const auto started = std::chrono::steady_clock::now();
    async_uv::emit_trace_event({"layer2_redis", "command_start", 0, params.size()});
    if (impl_->redis_ == nullptr) {
        throw RedisError("redis connection is not open", RedisErrorKind::not_connected);
    }

    const auto args = materialize_args(command_text, params);
    const auto timeout_ms = choose_timeout_ms(impl_->options_, options);

    if (impl_->tls_mode_) {
        auto run_blocking = [this, runtime, args]() -> Task<Reply> {
            auto error = std::make_shared<std::exception_ptr>();
            auto result = std::make_shared<Reply>();
            co_await runtime->spawn_blocking([this, args, error, result]() {
                try {
                    std::lock_guard<std::mutex> lock(impl_->blocking_mutex_);
                    *result = impl_->command_blocking(args);
                } catch (...) {
                    *error = std::current_exception();
                }
            });
            if (*error) {
                std::rethrow_exception(*error);
            }
            co_return *result;
        };

        try {
            if (timeout_ms.has_value()) {
                auto out = co_await async_uv::with_timeout(std::chrono::milliseconds(*timeout_ms),
                                                           run_blocking());
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                async_uv::emit_trace_event(
                    {"layer2_redis", "command_done", 0, static_cast<std::size_t>(elapsed.count())});
                co_return out;
            }
            auto out = co_await run_blocking();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            async_uv::emit_trace_event(
                {"layer2_redis", "command_done", 0, static_cast<std::size_t>(elapsed.count())});
            co_return out;
        } catch (const async_uv::Error &e) {
            if (e.code() == UV_ETIMEDOUT) {
                throw RedisError("redis command timeout", RedisErrorKind::command_failed);
            }
            throw;
        }
    }

    auto run = [this, args, timeout_ms]() -> Task<Reply> {
        co_return co_await impl_->command_async(args, timeout_ms);
    };

    try {
        if (timeout_ms.has_value()) {
            auto out =
                co_await async_uv::with_timeout(std::chrono::milliseconds(*timeout_ms), run());
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            async_uv::emit_trace_event(
                {"layer2_redis", "command_done", 0, static_cast<std::size_t>(elapsed.count())});
            co_return out;
        }
        auto out = co_await run();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        async_uv::emit_trace_event(
            {"layer2_redis", "command_done", 0, static_cast<std::size_t>(elapsed.count())});
        co_return out;
    } catch (const async_uv::Error &e) {
        if (e.code() == UV_ETIMEDOUT) {
            throw RedisError("redis command timeout", RedisErrorKind::command_failed);
        }
        throw;
    }
#endif
}

Task<Reply> Client::execute(std::string command_text) {
    co_return co_await command(std::move(command_text), {}, CommandOptions{});
}

Task<Reply> Client::execute(std::string command_text, std::vector<RedisParam> params) {
    co_return co_await command(std::move(command_text), std::move(params), CommandOptions{});
}

Task<Reply>
Client::execute(std::string command_text, std::vector<RedisParam> params, CommandOptions options) {
    co_return co_await command(std::move(command_text), std::move(params), std::move(options));
}

Task<Reply> command(ConnectionOptions options, std::string command_text) {
    co_return co_await command(std::move(options), std::move(command_text), {}, CommandOptions{});
}

Task<Reply>
command(ConnectionOptions options, std::string command_text, std::vector<RedisParam> params) {
    co_return co_await command(
        std::move(options), std::move(command_text), std::move(params), CommandOptions{});
}

Task<Reply> command(ConnectionOptions options,
                    std::string command_text,
                    std::vector<RedisParam> params,
                    CommandOptions command_options) {
    Client client;
    co_await client.open(std::move(options));

    std::exception_ptr error;
    Reply result;
    try {
        result = co_await client.command(
            std::move(command_text), std::move(params), std::move(command_options));
    } catch (...) {
        error = std::current_exception();
    }

    try {
        co_await client.close();
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
            throw RedisError("redis pool max_connections must be > 0",
                             RedisErrorKind::invalid_argument);
        }

        clients_.resize(options_.max_connections);
        born_at_.resize(options_.max_connections, std::chrono::steady_clock::now());
        if (!options_.preconnect) {
            for (std::size_t i = 0; i < options_.max_connections; ++i) {
                available_.push_back(i);
            }
            co_return;
        }

        for (std::size_t i = 0; i < options_.max_connections; ++i) {
            co_await clients_[i].open(options_.connection);
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
                    throw RedisError("redis pool is closed", RedisErrorKind::not_connected);
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
                    throw RedisError("redis pool acquire timeout", RedisErrorKind::command_failed);
                }
            }
            co_await async_uv::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Task<void> ensure_healthy(std::size_t index) {
        auto &client = clients_[index];
        bool reopen = !(co_await client.is_open());

        if (!reopen && options_.max_lifetime_ms > 0) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - born_at_[index]);
            if (age.count() >= options_.max_lifetime_ms) {
                reopen = true;
            }
        }

        if (reopen) {
            try {
                co_await client.close();
            } catch (...) {
            }
            co_await client.open(options_.connection);
            born_at_[index] = std::chrono::steady_clock::now();
            co_return;
        }

        if (!options_.health_check_command.empty()) {
            bool healthy = true;
            try {
                (void)co_await client.command(options_.health_check_command);
            } catch (...) {
                healthy = false;
            }
            if (!healthy) {
                try {
                    co_await client.close();
                } catch (...) {
                }
                co_await client.open(options_.connection);
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
    std::vector<Client> clients_;
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

Task<Reply> ConnectionPool::command(std::string command_text) {
    co_return co_await command(std::move(command_text), {}, CommandOptions{});
}

Task<Reply> ConnectionPool::command(std::string command_text, std::vector<RedisParam> params) {
    co_return co_await command(std::move(command_text), std::move(params), CommandOptions{});
}

Task<Reply> ConnectionPool::command(std::string command_text,
                                    std::vector<RedisParam> params,
                                    CommandOptions options) {
    if (impl_ == nullptr) {
        throw RedisError("redis pool is not initialized", RedisErrorKind::not_connected);
    }

    const std::size_t index = co_await impl_->acquire_index();
    std::exception_ptr error;
    Reply result;
    try {
        auto &client = impl_->clients_[index];
        co_await impl_->ensure_healthy(index);
        result =
            co_await client.command(std::move(command_text), std::move(params), std::move(options));
    } catch (...) {
        error = std::current_exception();
    }

    impl_->release_index(index);
    if (error) {
        std::rethrow_exception(error);
    }
    co_return result;
}

Task<Reply> ConnectionPool::execute(std::string command_text) {
    co_return co_await execute(std::move(command_text), {}, CommandOptions{});
}

Task<Reply> ConnectionPool::execute(std::string command_text, std::vector<RedisParam> params) {
    co_return co_await execute(std::move(command_text), std::move(params), CommandOptions{});
}

Task<Reply> ConnectionPool::execute(std::string command_text,
                                    std::vector<RedisParam> params,
                                    CommandOptions options) {
    co_return co_await command(std::move(command_text), std::move(params), std::move(options));
}

Task<void> ConnectionPool::close() {
    if (impl_ == nullptr) {
        co_return;
    }

    std::vector<std::size_t> indices;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->closed_ = true;
        indices.resize(impl_->clients_.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = i;
        }
        impl_->available_.clear();
    }

    for (const auto index : indices) {
        try {
            co_await impl_->clients_[index].close();
        } catch (...) {
        }
    }
    co_return;
}

} // namespace async_uv::redis
