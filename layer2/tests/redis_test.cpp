#include <cassert>
#include <cstdlib>
#include <string>

#include "async_uv/async_uv.h"
#include "async_uv_redis/redis.h"

namespace {

bool env_enabled(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    return std::string(value) == "1";
}

std::string env_or(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return fallback;
    }
    return value;
}

int env_or_int(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

async_uv::Task<void> run_redis_checks() {
    auto builder = async_uv::redis::ConnectionOptions::builder()
                       .host(env_or("ASYNC_UV_REDIS_HOST", "127.0.0.1"))
                       .port(env_or_int("ASYNC_UV_REDIS_PORT", 6379))
                       .user(env_or("ASYNC_UV_REDIS_USER", ""))
                       .password(env_or("ASYNC_UV_REDIS_PASSWORD", ""))
                       .db(env_or_int("ASYNC_UV_REDIS_DB", 0))
                       .connect_timeout_ms(3000)
                       .command_timeout_ms(3000);
    if (env_enabled("ASYNC_UV_REDIS_TLS")) {
        builder.tls_enabled(true)
            .tls_verify_peer(env_or("ASYNC_UV_REDIS_TLS_VERIFY_PEER", "1") != "0")
            .tls_ca_cert_file(env_or("ASYNC_UV_REDIS_TLS_CA_CERT", ""))
            .tls_ca_cert_dir(env_or("ASYNC_UV_REDIS_TLS_CA_DIR", ""))
            .tls_cert_file(env_or("ASYNC_UV_REDIS_TLS_CERT", ""))
            .tls_key_file(env_or("ASYNC_UV_REDIS_TLS_KEY", ""))
            .tls_server_name(env_or("ASYNC_UV_REDIS_TLS_SERVER_NAME", ""));
    }
    auto options = builder.build();

    async_uv::redis::Client client;
    co_await client.open(options);

    auto pong = co_await client.command("PING");
    assert(pong.type == async_uv::redis::Reply::Type::status ||
           pong.type == async_uv::redis::Reply::Type::string);

    (void)co_await client.command("SET ? ?", {"async_uv:redis:test:key", "ok"});
    auto get = co_await client.command("GET ?", {"async_uv:redis:test:key"});
    assert(get.type == async_uv::redis::Reply::Type::string);
    assert(get.string.has_value());
    assert(*get.string == "ok");

    (void)co_await client.command("DEL ?", {"async_uv:redis:test:key"});
    co_await client.close();
}

async_uv::Task<void> run_redis_pool_checks() {
    auto builder = async_uv::redis::ConnectionOptions::builder()
                       .host(env_or("ASYNC_UV_REDIS_HOST", "127.0.0.1"))
                       .port(env_or_int("ASYNC_UV_REDIS_PORT", 6379))
                       .user(env_or("ASYNC_UV_REDIS_USER", ""))
                       .password(env_or("ASYNC_UV_REDIS_PASSWORD", ""))
                       .db(env_or_int("ASYNC_UV_REDIS_DB", 0))
                       .connect_timeout_ms(3000)
                       .command_timeout_ms(3000);
    if (env_enabled("ASYNC_UV_REDIS_TLS")) {
        builder.tls_enabled(true)
            .tls_verify_peer(env_or("ASYNC_UV_REDIS_TLS_VERIFY_PEER", "1") != "0")
            .tls_ca_cert_file(env_or("ASYNC_UV_REDIS_TLS_CA_CERT", ""))
            .tls_ca_cert_dir(env_or("ASYNC_UV_REDIS_TLS_CA_DIR", ""))
            .tls_cert_file(env_or("ASYNC_UV_REDIS_TLS_CERT", ""))
            .tls_key_file(env_or("ASYNC_UV_REDIS_TLS_KEY", ""))
            .tls_server_name(env_or("ASYNC_UV_REDIS_TLS_SERVER_NAME", ""));
    }
    auto options = builder.build();

    auto pool_options = async_uv::redis::ConnectionPoolOptions::builder()
                            .connection(options)
                            .max_connections(2)
                            .preconnect(true)
                            .acquire_timeout_ms(2000)
                            .max_lifetime_ms(30000)
                            .health_check_command("PING")
                            .build();

    auto pool = co_await async_uv::redis::ConnectionPool::create(pool_options);
    (void)co_await pool.command("SET ? ?", {"async_uv:redis:pool:key", "pool_ok"});

    auto get = co_await pool.command("GET ?", {"async_uv:redis:pool:key"});
    assert(get.type == async_uv::redis::Reply::Type::string);
    assert(get.string.has_value());
    assert(*get.string == "pool_ok");

    (void)co_await pool.command("DEL ?", {"async_uv:redis:pool:key"});
    co_await pool.close();
}

async_uv::Task<void> run_all_checks() {
    if (env_enabled("ASYNC_UV_TEST_REDIS")) {
        co_await run_redis_checks();
        co_await run_redis_pool_checks();
    }
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().name("async_uv_redis_test"));
    runtime.block_on(run_all_checks());
    return 0;
}
