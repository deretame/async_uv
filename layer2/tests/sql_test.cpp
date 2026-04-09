#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

#include "flux/flux.h"
#include "flux_sql/sql.h"

namespace {

static_assert(stdexec::sender<decltype(std::declval<flux::sql::Connection &>().open(
    std::declval<flux::sql::ConnectionOptions>()))>);
static_assert(stdexec::sender<decltype(
    std::declval<flux::sql::Connection &>().query(std::declval<std::string>()))>);
static_assert(stdexec::sender<decltype(
    std::declval<flux::sql::ConnectionPool &>().acquire())>);
static_assert(stdexec::sender<decltype(
    std::declval<flux::sql::ConnectionPool &>().query(std::declval<std::string>()))>);

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

flux::Task<void> run_sqlite_checks() {
    flux::sql::Connection conn;

    auto options = flux::sql::ConnectionOptions::builder()
                       .driver(flux::sql::Driver::sqlite)
                       .file(":memory:")
                       .connect_timeout_ms(3000)
                       .query_timeout_ms(3000)
                       .build();

    co_await conn.open(options);
    assert(co_await conn.is_open());

    co_await conn.query("CREATE TABLE demo(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age "
                        "INTEGER, active INTEGER)");
    auto insert =
        co_await conn.query("INSERT INTO demo(name, age, active) VALUES(?, ?, ?)", {"alice", 18, true});
    assert(insert.affected_rows == 1);

    auto result =
        co_await conn.query("SELECT id, name, age, active FROM demo WHERE name = ?", {"alice"});
    assert(result.columns.size() == 4);
    assert(result.rows.size() == 1);
    assert(result.rows[0].values.size() == 4);
    assert(result.rows[0].values[1].has_value());
    assert(*result.rows[0].values[1] == "alice");

    co_await conn.begin();
    auto tx_insert = co_await conn.query("INSERT INTO demo(name, age, active) VALUES(?, ?, ?)",
                                         {"eve", 20, false});
    assert(tx_insert.affected_rows == 1);
    co_await conn.rollback();

    auto after_rollback = co_await conn.query("SELECT COUNT(*) FROM demo WHERE name = ?", {"eve"});
    assert(after_rollback.rows.size() == 1);
    assert(after_rollback.rows[0].values[0].has_value());
    assert(*after_rollback.rows[0].values[0] == "0");

    auto timeout_options = flux::sql::QueryOptions::builder().timeout_ms(1000).build();
    auto fast = co_await conn.query("SELECT 1", {}, timeout_options);
    assert(fast.rows.size() == 1);

    co_await conn.close();
    assert(!(co_await conn.is_open()));
}

flux::Task<void> run_sqlite_pool_checks() {
    const std::string pool_file = "/tmp/flux_sql_pool_test.db";
    auto connection_options = flux::sql::ConnectionOptions::builder()
                                  .driver(flux::sql::Driver::sqlite)
                                  .file(pool_file)
                                  .build();

    auto pool_options = flux::sql::ConnectionPoolOptions::builder()
                            .connection(connection_options)
                            .max_connections(2)
                            .preconnect(true)
                            .acquire_timeout_ms(2000)
                            .max_lifetime_ms(30000)
                            .build();

    auto pool = co_await flux::sql::ConnectionPool::create(pool_options);

    co_await pool.query("DROP TABLE IF EXISTS pool_demo");
    co_await pool.query(
        "CREATE TABLE IF NOT EXISTS pool_demo(id INTEGER PRIMARY KEY, name TEXT)");
    auto insert =
        co_await pool.query("INSERT INTO pool_demo(id, name) VALUES(?, ?)", {1, "pool"});
    assert(insert.affected_rows == 1);

    auto rows = co_await pool.query("SELECT name FROM pool_demo WHERE id = ?", {1});
    assert(rows.rows.size() == 1);
    assert(rows.rows[0].values[0].has_value());
    assert(*rows.rows[0].values[0] == "pool");

    auto sender_insert =
        co_await pool.query("INSERT INTO pool_demo(id, name) VALUES(?, ?)", {3, "sender"});
    assert(sender_insert.affected_rows == 1);

    auto sender_rows = co_await pool.query("SELECT name FROM pool_demo WHERE id = ?", {3});
    assert(sender_rows.rows.size() == 1);
    assert(sender_rows.rows[0].values[0].has_value());
    assert(*sender_rows.rows[0].values[0] == "sender");

    auto sender_lease = co_await pool.acquire();
    assert(sender_lease.valid());
    co_await sender_lease.begin();
    auto sender_tx_insert =
        co_await sender_lease.query("INSERT INTO pool_demo(id, name) VALUES(?, ?)",
                                    {4, "sender_tx"});
    assert(sender_tx_insert.affected_rows == 1);
    co_await sender_lease.rollback();
    sender_lease.release();

    auto sender_after_tx =
        co_await pool.query("SELECT COUNT(*) FROM pool_demo WHERE id = ?", {4});
    assert(sender_after_tx.rows.size() == 1);
    assert(sender_after_tx.rows[0].values[0].has_value());
    assert(*sender_after_tx.rows[0].values[0] == "0");

    auto leased = co_await pool.acquire();
    assert(leased.valid());
    co_await leased.begin();
    auto tx_insert =
        co_await leased.query("INSERT INTO pool_demo(id, name) VALUES(?, ?)", {2, "tx"});
    assert(tx_insert.affected_rows == 1);
    co_await leased.rollback();
    leased.release();

    auto after_tx = co_await pool.query("SELECT COUNT(*) FROM pool_demo WHERE id = ?", {2});
    assert(after_tx.rows.size() == 1);
    assert(after_tx.rows[0].values[0].has_value());
    assert(*after_tx.rows[0].values[0] == "0");

    auto timeout_pool_options = flux::sql::ConnectionPoolOptions::builder()
                                    .connection(connection_options)
                                    .max_connections(1)
                                    .preconnect(true)
                                    .acquire_timeout_ms(80)
                                    .build();
    auto timeout_pool = co_await flux::sql::ConnectionPool::create(timeout_pool_options);
    auto held_for_timeout = co_await timeout_pool.acquire();
    bool timeout_hit = false;
    try {
        (void)co_await timeout_pool.acquire();
    } catch (const flux::sql::SqlError &e) {
        timeout_hit = (e.kind() == flux::sql::SqlErrorKind::query_failed);
    }
    assert(timeout_hit);
    held_for_timeout.release();
    co_await timeout_pool.close();

    co_await pool.close();
}

flux::Task<void> run_postgres_checks() {
    flux::sql::ConnectionOptions options;
    options.driver = flux::sql::Driver::postgres;
    options.host = env_or("FLUX_SQL_PG_HOST", "127.0.0.1");
    options.port = env_or_int("FLUX_SQL_PG_PORT", 5432);
    options.user = env_or("FLUX_SQL_PG_USER", "postgres");
    options.password = env_or("FLUX_SQL_PG_PASSWORD", "mysecretpassword");
    options.database = env_or("FLUX_SQL_PG_DATABASE", "postgres");

    flux::sql::Connection conn;
    co_await conn.open(options);

    co_await conn.query("DROP TABLE IF EXISTS flux_sql_test");
    co_await conn.query(
        "CREATE TABLE flux_sql_test(id SERIAL PRIMARY KEY, name TEXT NOT NULL)");
    auto insert = co_await conn.query("INSERT INTO flux_sql_test(name) VALUES(?)", {"bob"});
    assert(insert.affected_rows == 1);

    auto rows = co_await conn.query("SELECT name FROM flux_sql_test WHERE name = ?", {"bob"});
    assert(rows.rows.size() == 1);
    assert(rows.rows[0].values[0].has_value());
    assert(*rows.rows[0].values[0] == "bob");

    co_await conn.close();
}

flux::Task<void> run_mysql_checks() {
    flux::sql::ConnectionOptions options;
    options.driver = flux::sql::Driver::mysql;
    options.host = env_or("FLUX_SQL_MY_HOST", "127.0.0.1");
    options.port = env_or_int("FLUX_SQL_MY_PORT", 3306);
    options.user = env_or("FLUX_SQL_MY_USER", "root");
    options.password = env_or("FLUX_SQL_MY_PASSWORD", "your_password");
    options.database = env_or("FLUX_SQL_MY_DATABASE", "mysql");

    flux::sql::Connection conn;
    co_await conn.open(options);

    co_await conn.query("DROP TABLE IF EXISTS flux_sql_test");
    co_await conn.query(
        "CREATE TABLE flux_sql_test(id BIGINT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL)");
    auto insert = co_await conn.query("INSERT INTO flux_sql_test(name) VALUES(?)", {"carol"});
    assert(insert.affected_rows == 1);

    auto rows = co_await conn.query("SELECT name FROM flux_sql_test WHERE name = ?", {"carol"});
    assert(rows.rows.size() == 1);
    assert(rows.rows[0].values[0].has_value());
    assert(*rows.rows[0].values[0] == "carol");

    co_await conn.close();
}

flux::Task<void> run_all_checks() {
    co_await run_sqlite_checks();
    co_await run_sqlite_pool_checks();

    if (env_enabled("FLUX_SQL_TEST_POSTGRES")) {
        try {
            co_await run_postgres_checks();
        } catch (const std::exception &e) {
            std::cerr << "postgres test failed: " << e.what() << std::endl;
            throw;
        }
    }

    if (env_enabled("FLUX_SQL_TEST_MYSQL")) {
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
    flux::Runtime runtime(flux::Runtime::build().name("flux_sql_test"));
    runtime.block_on(run_all_checks());
    return 0;
}
