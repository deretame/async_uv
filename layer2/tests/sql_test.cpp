#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "async_uv/async_uv.h"
#include "async_uv_sql/sql.h"

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

async_uv::Task<void> run_sqlite_checks() {
    async_uv::sql::Connection conn;

    auto options = async_uv::sql::ConnectionOptions::builder()
                       .driver(async_uv::sql::Driver::sqlite)
                       .file(":memory:")
                       .connect_timeout_ms(3000)
                       .query_timeout_ms(3000)
                       .build();

    co_await conn.open(options);
    assert(co_await conn.is_open());

    co_await conn.execute("CREATE TABLE demo(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age "
                          "INTEGER, active INTEGER)");
    auto insert = co_await conn.execute("INSERT INTO demo(name, age, active) VALUES(?, ?, ?)",
                                        {"alice", 18, true});
    assert(insert.affected_rows == 1);

    auto result =
        co_await conn.query("SELECT id, name, age, active FROM demo WHERE name = ?", {"alice"});
    assert(result.columns.size() == 4);
    assert(result.rows.size() == 1);
    assert(result.rows[0].values.size() == 4);
    assert(result.rows[0].values[1].has_value());
    assert(*result.rows[0].values[1] == "alice");

    co_await conn.begin();
    auto tx_insert = co_await conn.execute("INSERT INTO demo(name, age, active) VALUES(?, ?, ?)",
                                           {"eve", 20, false});
    assert(tx_insert.affected_rows == 1);
    co_await conn.rollback();

    auto after_rollback = co_await conn.query("SELECT COUNT(*) FROM demo WHERE name = ?", {"eve"});
    assert(after_rollback.rows.size() == 1);
    assert(after_rollback.rows[0].values[0].has_value());
    assert(*after_rollback.rows[0].values[0] == "0");

    auto timeout_options = async_uv::sql::QueryOptions::builder().timeout_ms(1000).build();
    auto fast = co_await conn.query("SELECT 1", {}, timeout_options);
    assert(fast.rows.size() == 1);

    co_await conn.close();
    assert(!(co_await conn.is_open()));
}

async_uv::Task<void> run_sqlite_pool_checks() {
    const std::string pool_file = "/tmp/async_uv_sql_pool_test.db";
    auto connection_options = async_uv::sql::ConnectionOptions::builder()
                                  .driver(async_uv::sql::Driver::sqlite)
                                  .file(pool_file)
                                  .build();

    auto pool_options = async_uv::sql::ConnectionPoolOptions::builder()
                            .connection(connection_options)
                            .max_connections(2)
                            .preconnect(true)
                            .acquire_timeout_ms(2000)
                            .max_lifetime_ms(30000)
                            .build();

    auto pool = co_await async_uv::sql::ConnectionPool::create(pool_options);

    co_await pool.execute("DROP TABLE IF EXISTS pool_demo");
    co_await pool.execute(
        "CREATE TABLE IF NOT EXISTS pool_demo(id INTEGER PRIMARY KEY, name TEXT)");
    auto insert =
        co_await pool.execute("INSERT INTO pool_demo(id, name) VALUES(?, ?)", {1, "pool"});
    assert(insert.affected_rows == 1);

    auto rows = co_await pool.query("SELECT name FROM pool_demo WHERE id = ?", {1});
    assert(rows.rows.size() == 1);
    assert(rows.rows[0].values[0].has_value());
    assert(*rows.rows[0].values[0] == "pool");

    co_await pool.close();
}

async_uv::Task<void> run_postgres_checks() {
    async_uv::sql::ConnectionOptions options;
    options.driver = async_uv::sql::Driver::postgres;
    options.host = env_or("ASYNC_UV_SQL_PG_HOST", "127.0.0.1");
    options.port = env_or_int("ASYNC_UV_SQL_PG_PORT", 5432);
    options.user = env_or("ASYNC_UV_SQL_PG_USER", "postgres");
    options.password = env_or("ASYNC_UV_SQL_PG_PASSWORD", "mysecretpassword");
    options.database = env_or("ASYNC_UV_SQL_PG_DATABASE", "postgres");

    async_uv::sql::Connection conn;
    co_await conn.open(options);

    co_await conn.execute("DROP TABLE IF EXISTS async_uv_sql_test");
    co_await conn.execute(
        "CREATE TABLE async_uv_sql_test(id SERIAL PRIMARY KEY, name TEXT NOT NULL)");
    auto insert = co_await conn.execute("INSERT INTO async_uv_sql_test(name) VALUES(?)", {"bob"});
    assert(insert.affected_rows == 1);

    auto rows = co_await conn.query("SELECT name FROM async_uv_sql_test WHERE name = ?", {"bob"});
    assert(rows.rows.size() == 1);
    assert(rows.rows[0].values[0].has_value());
    assert(*rows.rows[0].values[0] == "bob");

    co_await conn.close();
}

async_uv::Task<void> run_mysql_checks() {
    async_uv::sql::ConnectionOptions options;
    options.driver = async_uv::sql::Driver::mysql;
    options.host = env_or("ASYNC_UV_SQL_MY_HOST", "127.0.0.1");
    options.port = env_or_int("ASYNC_UV_SQL_MY_PORT", 3306);
    options.user = env_or("ASYNC_UV_SQL_MY_USER", "root");
    options.password = env_or("ASYNC_UV_SQL_MY_PASSWORD", "your_password");
    options.database = env_or("ASYNC_UV_SQL_MY_DATABASE", "mysql");

    async_uv::sql::Connection conn;
    co_await conn.open(options);

    co_await conn.execute("DROP TABLE IF EXISTS async_uv_sql_test");
    co_await conn.execute(
        "CREATE TABLE async_uv_sql_test(id BIGINT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL)");
    auto insert = co_await conn.execute("INSERT INTO async_uv_sql_test(name) VALUES(?)", {"carol"});
    assert(insert.affected_rows == 1);

    auto rows = co_await conn.query("SELECT name FROM async_uv_sql_test WHERE name = ?", {"carol"});
    assert(rows.rows.size() == 1);
    assert(rows.rows[0].values[0].has_value());
    assert(*rows.rows[0].values[0] == "carol");

    co_await conn.close();
}

async_uv::Task<void> run_all_checks() {
    co_await run_sqlite_checks();
    co_await run_sqlite_pool_checks();

    if (env_enabled("ASYNC_UV_SQL_TEST_POSTGRES")) {
        try {
            co_await run_postgres_checks();
        } catch (const std::exception &e) {
            std::cerr << "postgres test failed: " << e.what() << std::endl;
            throw;
        }
    }

    if (env_enabled("ASYNC_UV_SQL_TEST_MYSQL")) {
        try {
            co_await run_mysql_checks();
        } catch (const std::exception &e) {
            std::cerr << "mysql test failed: " << e.what() << std::endl;
            throw;
        }
    }
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().name("async_uv_sql_test"));
    runtime.block_on(run_all_checks());
    return 0;
}
