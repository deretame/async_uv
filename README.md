# flux

`flux` is a C++23 async IO library built on top of `libuv` and `async_simple`.

It aims for:

- sync-style coroutine APIs for file, timer, TCP, UDP, watcher, and mailbox work
- a `libuv`-backed `async_simple::Executor`
- unified cancellation and timeout handling
- structured concurrency with RAII cleanup
- pluggable TLS BIO abstraction (engine provided by external SSL library)
- fd/socket readiness watcher based on `uv_poll_t`

## Build

Requirements:

- CMake 3.24+
- Clang or another modern C++23 compiler
- Ninja is recommended

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

## Project Layout

- `src/CMakeLists.txt`: library-only target definition
- `examples/CMakeLists.txt`: examples only
- `tests/CMakeLists.txt`: tests only
- `cmake/flux_dependencies.cmake`: shared dependency setup

If another project only wants the library target, it can include just the library directory:

```cmake
add_subdirectory(path/to/flux/src flux)
target_link_libraries(my_app PRIVATE flux::flux)
```

If you add the repository root instead, examples and tests are controlled by:

- `FLUX_BUILD_EXAMPLES`
- `FLUX_BUILD_TESTS`
- `FLUX_USE_MIMALLOC`
- `FLUX_ENABLE_LAYER2` (build layer2 targets: `async_uv_http` / `async_uv_sql` / `async_uv_redis` / `async_uv_ws`)
- `FLUX_ENABLE_LAYER3` (build layer3 target; automatically enables layer2)

Layer relationship:

- layer1: `flux` (base IO runtime)
- layer2: `async_uv_http` / `async_uv_sql` / `async_uv_redis` / `async_uv_ws` (depend on layer1)
- layer3: `async_uv_layer3` (depends on layer2, and transitively layer1)

Layer2 boundary (scope):

- layer2 only provides reusable primitives: connection/IO, parser/serialization, pool, timeout, TLS, error mapping, trace events
- routing, endpoint composition, auth, business middleware orchestration belong to layer3

Layer2 stable API checklist:

- `flux::http`: HTTP client + parser + server primitives only (no router)
- `flux::sql`: connection/query/transaction/pool primitives
- `flux::redis`: connection/command/pool primitives
- `flux::ws`: open/close/send/recv/messages stream primitives (`ws`/`wss`)
- `flux::layer2::ErrorKind`: cross-module error mapping contract
- `trace hook` categories: `layer2_http`, `layer2_sql`, `layer2_redis`, `layer2_ws`

Layer2 header index (for layer3 integration):

- HTTP client/parser: `layer2/include/async_uv_http/http.h`, `layer2/include/async_uv_http/parser.h`
- HTTP server primitives: `layer2/include/async_uv_http/server.h`
- SQL: `layer2/include/async_uv_sql/sql.h`
- Redis: `layer2/include/async_uv_redis/redis.h`
- WebSocket: `layer2/include/async_uv_ws/ws.h`
- unified error mapping: `layer2/include/async_uv_layer2/error.h`
- logging helper (`to_string`): `layer2/include/async_uv_layer2/to_string.h`

Layer2 integration reading order (recommended):

1. boundary/scope (`Layer2 boundary`)
2. header entry points (`Layer2 header index`)
3. module primitives (`Layer2 SQL`, `Layer2 Redis`, `Layer2 WebSocket`, `Layer2 HTTP server primitives`)
4. unified error mapping (`flux::layer2::ErrorKind` + `to_error_kind(...)`)
5. trace hooks (`layer2_http`, `layer2_sql`, `layer2_redis`, `layer2_ws`)

Example:

```bash
cmake -S . -B build -G Ninja -DFLUX_ENABLE_LAYER2=ON
cmake --build build
```

Then link:

```cmake
target_link_libraries(my_app PRIVATE async_uv_http)
# optionally add: async_uv_sql async_uv_redis async_uv_ws
```

`async_uv_http` uses libcurl. OpenSSL backend is preferred. If OpenSSL is unavailable,
the build falls back to mbedTLS when building curl from source.

## Core Pieces

### Layer2 HTTP

`async_uv_http` provides a minimal async API similar to reqwest-style ergonomics:

```cpp
auto runtime = co_await flux::get_current_runtime();

auto client = flux::http::Client::build()
    .runtime(*runtime) // 可选；不传时使用当前运行时 / optional; fallback to current runtime
    .default_header("Accept", "application/json")
    .timeout(std::chrono::seconds(20))
    .build();

std::map<std::string, std::string> payload{{"hello", "world"}};
DemoPayload request_data{"world", "cpp"};
HttpBinPostJsonEcho response_json = co_await client.post("https://httpbin.org/post")
    .json(request_data)
    .request_format(flux::http::RequestFormat::json)   // 也支持 .request_format("json")
    .response_format(flux::http::ResponseFormat::json) // 也支持 .response_format("json")
    .send()
    .json<HttpBinPostJsonEcho>();

auto upload_json = co_await client.post("https://httpbin.org/post")
    .multipart({
        flux::http::field("kind", "demo"),
        flux::http::field("lang", "cpp"),
        flux::http::file("file", "/tmp/demo.txt")
            .filename("demo.txt")
            .content_type("text/plain"),
    })
    .send()
    .json<nlohmann::json>();

std::unordered_map<std::string, int> pager{{"page", 2}, {"size", 20}};
auto query_json = co_await client.get("https://httpbin.org/get")
    .query({{"lang", "cpp"}, {"sort", "desc"}})
    .query(pager)
    .send()
    .json<nlohmann::json>();

auto image_json = co_await client.post("https://httpbin.org/post")
    .form_binary("image", png_bytes, "demo.png", "image/png")
    .send()
    .json<nlohmann::json>();

// 或者文件来源（由 curl 从文件读取上传）
auto file_json = co_await client.post("https://httpbin.org/post")
    .multipart({
        flux::http::file("image", "/tmp/demo.png")
            .filename("demo.png")
            .content_type("image/png"),
    })
    .send()
    .json<nlohmann::json>();

// 大文件下载（流式落盘，避免整文件进内存）
flux::http::DownloadOptions dl;
dl.resume = true;
dl.use_temp_file = true;
dl.temp_path = "/tmp/archive.bin.part";

auto dl_result = co_await client.download_to_file(
    "https://speed.hetzner.de/100MB.bin", "/tmp/archive.bin", dl);
```

`Client` uses reqwest-like chained request builders (`get/post/put/del -> query/json/xml/urlencoded/multipart -> send`).
By default, non-2xx responses throw `flux::http::HttpError` with structured error fields.
Response format defaults to JSON if not set explicitly.
You can also set request format explicitly via `request_format(...)`.
`query(...)` supports map/unordered_map-style containers and values that are string-like,
compatible with `std::to_string`, or provide `to_string()/toString()` members.
`query(...)` also supports initializer-list pairs such as `.query({{"lang", "cpp"}, {"sort", "desc"}})`.
`urlencoded(container)` now applies percent-encoding for keys and values.

Additional HTTP features:

- interceptors (`on_request/on_response/on_error`) via `Client::Builder::interceptor(...)`
- retry policy (`RetryPolicy`) with idempotent-method guard and exponential backoff
- proxy options (`ProxyOptions`) and cookie jar options (`CookieJarOptions`)
- streaming response chunks via `stream_response(...)` / `on_response_chunk(...)`
- transport error kind (`TransportErrorKind`) for finer error handling
- llhttp parser (`HttpParser` + `HttpMessage`) for incremental raw HTTP parsing

Layer2 SQL abstraction (`flux::sql`) currently provides:

- unified async API for SQLite / MySQL / PostgreSQL (driver availability depends on source dependency build)
- `Connection::open/query/execute/close/cancel` coroutine interfaces
- parameterized query API (`query/execute` overloads with `std::vector<SqlParam>`, supports string/int/double/bool/null)
- basic transaction helpers (`begin/commit/rollback`)
- query timeout options (`ConnectionOptions::query_timeout_ms`, `QueryOptions::timeout_ms`)
- connection pool (`ConnectionPool`) with configurable pool size / acquire timeout / max lifetime / health check SQL
- builder-style options for backward-compatible extension (`ConnectionOptions::builder()`, `QueryOptions::builder()`, `ConnectionPoolOptions::builder()`)
- PostgreSQL SSL options in connection config (`postgres_ssl_mode` and cert/key/root cert fields)
- row/column normalized result model (`QueryResult`, `Row`, nullable `Cell`)

```cpp
auto db_opts = flux::sql::ConnectionOptions::builder()
    .driver(flux::sql::Driver::sqlite)
    .file(":memory:")
    .query_timeout_ms(2000)
    .build();

flux::sql::Connection db;
co_await db.open(db_opts);
co_await db.execute("CREATE TABLE demo(id INTEGER PRIMARY KEY, name TEXT)");
co_await db.begin();
co_await db.execute("INSERT INTO demo(name) VALUES(?)", {"alice"});
co_await db.commit();

auto rows = co_await db.query("SELECT id, name FROM demo ORDER BY id");

// 参数化查询（? 占位符，支持多种类型）
auto filtered = co_await db.query(
    "SELECT id, name FROM demo WHERE name = ? AND id > ?",
    {"alice", 0},
    flux::sql::QueryOptions::builder().timeout_ms(1000).build()
);

auto pool = co_await flux::sql::ConnectionPool::create(
    flux::sql::ConnectionPoolOptions::builder()
        .connection(db_opts)
        .max_connections(4)
        .preconnect(true)
        .acquire_timeout_ms(3000)
        .build()
);

auto pooled = co_await pool.query("SELECT COUNT(*) FROM demo");
co_await pool.close();

co_await db.close();
```

SQL test toggles:

- `ASYNC_UV_SQL_TEST_POSTGRES=1` enables postgres integration checks
- `ASYNC_UV_SQL_TEST_MYSQL=1` enables mysql integration checks
- `ASYNC_UV_SQL_PG_*` / `ASYNC_UV_SQL_MY_*` can override default host/port/user/password/database

Layer2 Redis abstraction (`flux::redis`) currently provides:

- coroutine-based `Client::open/command/execute/close` API
- Redis command parameter replacement using `?` placeholders with `std::vector<RedisParam>`
- official hiredis source dependency with fd watcher driven nonblocking socket flow
- TLS/SSL support via hiredis SSL (`tls_enabled`, CA/cert/key/server_name, verify mode)
- optional auth/select on connect (`user`/`password`/`db`)
- connection pool (`ConnectionPool`) with configurable pool size / acquire timeout / max lifetime / health check command

```cpp
auto redis_opts = flux::redis::ConnectionOptions::builder()
    .host("127.0.0.1")
    .port(6379)
    // .tls_enabled(true)
    // .tls_server_name("redis.example.com")
    .build();

flux::redis::Client redis;
co_await redis.open(redis_opts);
co_await redis.command("SET ? ?", {"demo:key", "value"});
auto reply = co_await redis.command("GET ?", {"demo:key"});
co_await redis.close();

auto redis_pool = co_await flux::redis::ConnectionPool::create(
    flux::redis::ConnectionPoolOptions::builder()
        .connection(redis_opts)
        .max_connections(4)
        .preconnect(true)
        .build()
);

auto pooled_reply = co_await redis_pool.command("GET ?", {"demo:key"});
co_await redis_pool.close();
```

Redis test toggles:

- `ASYNC_UV_TEST_REDIS=1` enables redis integration checks
- `ASYNC_UV_REDIS_HOST` / `ASYNC_UV_REDIS_PORT` / `ASYNC_UV_REDIS_USER` / `ASYNC_UV_REDIS_PASSWORD` / `ASYNC_UV_REDIS_DB`
- TLS env (optional): `ASYNC_UV_REDIS_TLS=1`, `ASYNC_UV_REDIS_TLS_VERIFY_PEER=0|1`, `ASYNC_UV_REDIS_TLS_CA_CERT`, `ASYNC_UV_REDIS_TLS_CA_DIR`, `ASYNC_UV_REDIS_TLS_CERT`, `ASYNC_UV_REDIS_TLS_KEY`, `ASYNC_UV_REDIS_TLS_SERVER_NAME`

Layer2 WebSocket abstraction (`flux::ws`) currently provides:

- coroutine-style `Client::open/close/send_text/send_binary/next_message/next_message_for/messages`
- ws and wss URL support (TLS handled by IXWebSocket + OpenSSL backend)
- callback-to-coroutine bridging via mailbox stream (`next_message` and `messages().next()` awaitable pull model)
- builder-style options (`ClientOptions::builder()`) for timeout/ping/reconnect/TLS fields
- built-in trace events: `layer2_ws/open_*`, `layer2_ws/send_*`, `layer2_ws/recv_*`, `layer2_ws/close_*`

```cpp
auto ws_opts = flux::ws::ClientOptions::builder()
    .url("wss://echo.websocket.events")
    .connect_timeout_ms(5000)
    .disable_automatic_reconnection(true)
    .tls_verify_peer(true)
    .build();

flux::ws::Client ws;
co_await ws.open(ws_opts);
co_await ws.send_text("hello ws");

auto msg = co_await ws.next_message();
if (msg.type == flux::ws::MessageType::text) {
    // msg.data
}

co_await ws.close();
```

Stream-style receive example (recommended for continuous consume):

```cpp
auto stream = ws.messages();
while (auto message = co_await stream.next()) {
    if (message->type == flux::ws::MessageType::text) {
        // process text
    }
}
```

If you only want the first N text messages, prefer "stream loop + break":

```cpp
std::vector<std::string> got;
auto stream = ws.messages();
while (auto message = co_await stream.next()) {
    if (message->type != flux::ws::MessageType::text) {
        continue;
    }
    got.push_back(message->data);
    if (got.size() == 2) {
        break;
    }
}
```

WebSocket dependency toggle:

- `ASYNC_UV_LAYER2_FETCH_IXWEBSOCKET=ON|OFF` (default ON)

Layer2 unified error mapping (`flux::layer2::ErrorKind`):

- map SQL errors by `flux::layer2::to_error_kind(flux::sql::SqlErrorKind)`
- map Redis errors by `flux::layer2::to_error_kind(flux::redis::RedisErrorKind)`
- map HTTP errors by `flux::layer2::to_error_kind(flux::http::HttpErrorCode, flux::http::TransportErrorKind)`
- map WebSocket errors by `flux::layer2::to_error_kind(flux::ws::WsErrorKind)`

```cpp
#include "async_uv_layer2/error.h"

try {
    co_await redis.command("GET ?", {"demo:key"});
} catch (const flux::redis::RedisError& e) {
    auto kind = flux::layer2::to_error_kind(e.kind());
    if (kind == flux::layer2::ErrorKind::not_connected) {
        // reconnect or fallback
    }
}
```

Common `to_string` helpers for logging (`flux::layer2::to_string`):

```cpp
#include "async_uv_layer2/to_string.h"

try {
    co_await ws.send_text("hello");
} catch (const flux::ws::WsError& e) {
    auto kind_text = flux::layer2::to_string(e.kind());
    // e.g. "not_connected"
}

auto mapped = flux::layer2::to_error_kind(flux::ws::WsErrorKind::send_failed);
auto mapped_text = flux::layer2::to_string(mapped); // "operation_failed"
```

Layer2 HTTP server primitives (`flux::http`) currently provides:

- request-target parser (`parse_request_target`) for path/query/fragment split
- server request model (`ServerRequest`) converted from `HttpMessage`
- response serializer (`serialize_response`) with content-length/chunked helpers
- request safety validation (`validate_request`) by header/body limits
- connection lifecycle helpers (`ServerConnectionPolicy`, `should_keep_alive`, socket read/write timeout wrappers)
- middleware chain primitive (`MiddlewareChain`) for request/response wrapping
- stream primitives (`BodyReader`/`BodyWriter`) with in-memory and socket implementations

```cpp
auto target = flux::http::parse_request_target("/v1/items?id=1&name=foo");

flux::http::ServerResponse res;
res.status_code = 200;
res.headers.push_back({"Content-Type", "application/json"});
res.body = R"({"ok":true})";

std::string wire = flux::http::serialize_response(res);

flux::http::MiddlewareChain chain;
chain.use([](flux::http::ServerRequest req,
             flux::http::ServerHandler next) -> flux::Task<flux::http::ServerResponse> {
    req.headers.push_back({"X-Trace", "1"});
    co_return co_await next(std::move(req));
});

chain.endpoint([](flux::http::ServerRequest req) -> flux::Task<flux::http::ServerResponse> {
    flux::http::ServerResponse out;
    out.status_code = 200;
    out.body = req.header("X-Trace").value_or("0");
    co_return out;
});
```

Behavior contracts (important defaults):

- `throw_on_http_error=true`: non-2xx throws `HttpErrorCode::http_status_failure`
- default `response_format`: JSON
- `stream_response(..., false)`: chunks are streamed, `Response.body` stays empty
- `request_format` and payload must match, conflict returns `HttpErrorCode::invalid_request`
- if no proxy is configured explicitly, `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY` env is used

```cpp
flux::http::RetryPolicy retry;
retry.enabled = true;
retry.max_attempts = 3;
retry.base_backoff = std::chrono::milliseconds(100);
retry.max_backoff = std::chrono::seconds(1);

flux::http::ProxyOptions proxy;
proxy.url = "http://127.0.0.1:7890";

flux::http::CookieJarOptions cookie;
cookie.enabled = true;
cookie.file_path = "/tmp/async_uv_cookie.jar";

std::size_t streamed = 0;
auto text_body = co_await client.get("https://example.com")
    .retry(retry)
    .proxy(proxy)
    .stream_response([&](std::string_view chunk) { streamed += chunk.size(); }, false)
    .response_format(flux::http::ResponseFormat::text)
    .send()
    .text();

try {
    auto _ = co_await client.get("http://nonexistent.async-uv.invalid/").send().raw();
} catch (const flux::http::HttpError& e) {
    if (e.transport_kind() == flux::http::TransportErrorKind::dns) {
        // handle DNS issue
    }
}

auto client_with_cookie = flux::http::Client::build()
    .cookie_jar(cookie)
    .build();

flux::http::HttpParser parser(flux::http::ParseMode::response);
co_await parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
if (auto msg = co_await parser.next_message()) {
    auto content_length = msg->header("Content-Length");
    auto body = msg->body();
}

auto one = co_await flux::http::parse_first_message(
    "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
auto all = co_await flux::http::parse_all_messages(
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");

// 大体积响应体可自动落盘（例如 multipart/form-data 或大文件）
flux::http::ParserOptions parse_opts;
parse_opts.max_body_in_memory = 1024; // 超过 1KB 自动写临时文件
parse_opts.temp_directory = "/tmp/async_uv_parser";
parse_opts.max_feed_chunk_size = 64 * 1024; // 大块输入分片解析
parse_opts.yield_every_chunks = 8; // 每处理若干分片让出一次执行权

// 推荐参数（按场景调整）
// 1) 低延迟/高并发：max_feed_chunk_size=16KB~64KB, yield_every_chunks=1~4
// 2) 高吞吐/大包优先：max_feed_chunk_size=128KB~512KB, yield_every_chunks=8~32
// 3) 默认平衡值：max_feed_chunk_size=64KB, yield_every_chunks=8

flux::http::HttpParser big_parser(flux::http::ParseMode::response, parse_opts);
co_await big_parser.feed(raw_http_data);
if (auto msg = co_await big_parser.next_message(); msg && msg->body_in_file()) {
    auto tmp = msg->body_file_path();
    auto size = msg->body_size();
    co_await msg->move_body_file_to("/tmp/final_upload_body.bin");
}

// 主动异步清理临时 body 文件
if (auto msg = co_await big_parser.next_message(); msg && msg->body_in_file()) {
    bool disposed = co_await msg->dispose_body_file();
    (void)disposed;
}
```

`download_to_file` and request execution use the current runtime context; they do not create
an internal runtime. `HttpParser` and `HttpMessage` parser file operations are async and use the
current runtime context. If a message still owns a temp body file when destructed, cleanup is
dispatched with runtime `spawn` (non-blocking fire-and-forget); when no runtime is available,
it falls back to a detached thread for non-blocking cleanup.

The layer2 library itself does not depend on any JSON library. You can inject your own codec.
For example, with modern JSON (`nlohmann::json`) in application code:

```cpp
#include <nlohmann/json.hpp>

class NlohmannJsonCodec : public flux::http::JsonCodec {
public:
    std::string serialize(const void* value, const std::type_info& type) const override {
        if (type == typeid(std::map<std::string, std::string>)) {
            const auto& map = *static_cast<const std::map<std::string, std::string>*>(value);
            return nlohmann::json(map).dump();
        }
        throw std::runtime_error("unsupported json serialize type");
    }

    void deserialize(std::string_view text, void* output, const std::type_info& type) const override {
        if (type == typeid(nlohmann::json)) {
            *static_cast<nlohmann::json*>(output) = nlohmann::json::parse(text);
            return;
        }
        throw std::runtime_error("unsupported json deserialize type");
    }
};

class MyLoggingInterceptor : public flux::http::Interceptor {
public:
    void on_request(flux::http::Request& request) const override {
        request.headers.push_back({"X-Trace-Source", "demo"});
    }
};

auto runtime = co_await flux::get_current_runtime();
auto client = flux::http::Client::build()
    .runtime(*runtime)
    .json_codec(std::make_shared<NlohmannJsonCodec>())
    .interceptor(std::make_shared<MyLoggingInterceptor>())
    .user_agent("my-app/1.0")
    .build();
```

Request/response format options:

- request: `RequestFormat::json/form/xml/text/binary/multipart` (or string form)
- response: `ResponseFormat::json/form/xml/text` (or string form)

### Runtime

`flux::Runtime` owns a `libuv` loop and also serves as an `async_simple::Executor`.

```cpp
flux::Runtime runtime;
runtime.block_on(my_task());
```

You can optionally set the `libuv` worker threadpool size when creating a runtime:

```cpp
flux::Runtime runtime(flux::Runtime::build().uv_threadpool_size(8));
```

`UV_THREADPOOL_SIZE` is process-wide in `libuv`, so the configured value must stay consistent
across runtimes in the same process.

`Runtime` also exposes executor stats through `runtime.stat()`, which can be used to sample
pending task count.

### Trace Hook

You can register a process-wide trace hook for lightweight runtime signals:

```cpp
#include <atomic>
#include <memory>
#include <string_view>

auto mailbox_events = std::make_shared<std::atomic_int>(0);

flux::set_trace_hook([mailbox_events](const flux::TraceEvent& event) {
    if (std::string_view(event.category) == "mailbox") {
        mailbox_events->fetch_add(1, std::memory_order_relaxed);
    }

    // event.name: send / send_rejected_full / send_timeout / recv / recv_closed ...
    // event.code: error code (0 means no error)
    // event.value: extra numeric value (for mailbox, typically queue size snapshot)
});

// ... run async work ...

flux::reset_trace_hook();
```

Use `flux::reset_trace_hook()` to clear it. Hook callbacks should stay lightweight and
non-blocking.

### Stable Event Names

Current built-in trace events (stable names in current version):

| category | name | meaning | value |
| --- | --- | --- | --- |
| `mailbox` | `send` | message queued and notify sent | queue size snapshot after enqueue |
| `mailbox` | `send_rejected_closed` | send rejected because mailbox is closing/closed | queue size snapshot |
| `mailbox` | `send_rejected_full` | send rejected because bounded queue is full | queue size snapshot |
| `mailbox` | `send_async_error` | `uv_async_send` failed | queue size snapshot |
| `mailbox` | `send_timeout` | `send_for/send_until` timed out waiting for capacity | queue size snapshot |
| `mailbox` | `send_drop_oldest` | queue full and overflow policy dropped oldest message | queue size snapshot before drop |
| `mailbox` | `recv` | `recv()` got a message | currently `0` |
| `mailbox` | `recv_closed` | `recv()` reached end-of-stream after close+drain | currently `0` |
| `tcp` | `connect` / `connect_error` / `read` / `read_eof` / `read_error` / `write` / `write_error` | TCP key I/O lifecycle | bytes or `0` |
| `udp` | `send` / `send_error` / `recv` / `recv_error` | UDP send/receive lifecycle | bytes or `0` |
| `timer` | `wait_start` / `wait_fired` / `wait_canceled` / `wait_error` / `close` | timer lifecycle | due ms or `0` |
| `watch` | `fs_event_start` / `fs_event` / `fs_event_stop` / `fs_poll_start` / `fs_poll` / `fs_poll_stop` (+ `_error`) | watcher lifecycle | event flags or `0` |
| `fd` | `poll_start` / `poll_event` / `poll_stop` (+ `_error`) | fd/socket readiness watcher lifecycle | polled event flags |
| `fs` | `<uv_fs_* api name>` | fs request finished (success/error) | `uv_fs_t.result` snapshot |

### Cancellation

Cancellation is built on `async_simple::Signal` and `Slot`.

Public helpers:

- `CancellationSource`
- `spawn(lazy, source)`
- `with_timeout(...)`
- `with_deadline(...)`
- `get_current_signal()`

Internal operations install cancellation handlers with operation-local `Slot` objects, so they do not keep raw parent `Slot*` pointers across async boundaries.

### Structured Concurrency

`TaskScope` is the main structured-concurrency primitive.

```cpp
auto value = co_await flux::with_task_scope(
    [](flux::TaskScope& scope) -> flux::Task<int> {
        auto [a, b] = co_await scope.all(task_a(), task_b());
        auto fastest = co_await scope.any_success(task_c(), task_d(), task_e());
        co_return a + b + fastest;
    });
```

Current scope operations:

- `scope.spawn(...)`
- `scope.spawn_blocking(...)`
- `scope.all(...)`
- `scope.race(...)`
- `scope.any_success(...)`
- `scope.with_timeout(...)`
- `scope.with_deadline(...)`
- `scope.cancel()`
- `scope.join()`
- `scope.close()`

The collection helpers support both variadic inputs and moved ranges like `std::vector<Task<T>>`.

### Scope Semantics

`with_task_scope(...)` is the strict form:

- normal return: auto `join()`
- body exception: auto `close()`, which means `cancel() + join()`

`TaskScope` destructor is best-effort RAII cleanup:

- off runtime thread: blocks and closes
- on runtime thread: schedules async cleanup to avoid deadlock

If you need strict lifetime guarantees, prefer `with_task_scope(...)`.

### Blocking Work

`spawn_blocking(...)` runs on the runtime's blocking thread pool.

Important behavior:

- it participates in structured concurrency through `scope.spawn_blocking(...)`
- scope cancellation does not kill an already-running blocking function
- scope exit still waits for the blocking work to finish
- if the scope has already been canceled when the blocking work completes, the user-facing future resolves as cancellation instead of a normal value

## Timeout Convenience APIs

Common TCP, UDP, file, mailbox, and watcher APIs have timeout and deadline wrappers.

Examples:

- `TcpSocket::connect_for(...)`
- `TcpClient::receive_exactly_for(...)`
- `TcpListener::accept_for(...)`
- `UdpSocket::receive_from_for(...)`
- `Fs::read_file_for(...)`
- `File::read_all_for(...)`
- `Mailbox<T>::recv_for(...)`
- `FsWatcher::next_for(...)`

These wrappers all forward to the same `with_timeout(...)` and `with_deadline(...)` behavior.

## Streaming APIs

Streaming APIs now use `Stream<T>` and expose a single async pull method:

- `co_await stream.next()` returns `std::optional<T>`
- `std::nullopt` means end-of-stream
- only one `next()` call may be in flight per stream instance

Available stream sources include:

- `Directory::entries()`
- `Mailbox<T>::messages()`
- `TcpClient::receive_chunks()`
- `UdpSocket::receive_packets()`
- `FsWatcher::events()`

## Mailbox Backpressure

`Mailbox<T>` is move-only-message only, and it can optionally be bounded:

```cpp
flux::MailboxOptions options;
options.max_buffered_messages = 1024;
options.overflow_policy = flux::MailboxOptions::OverflowPolicy::reject_new;
auto mailbox = co_await flux::Mailbox<std::unique_ptr<MyMessage>>::create(options);
```

Overflow policy:

- `reject_new`: queue full means reject new message (`send`/`try_send` return `false`)
- `drop_oldest`: queue full means drop oldest buffered message, then enqueue new one

`MessageSender<T>` also exposes:

- `try_send(...)`: explicit non-blocking send
- `send_for(...)` / `send_until(...)`: async wait for capacity when bounded
- `sync_send_for(...)` / `sync_send_until(...)`: blocking wait for external threads

Example split:

```cpp
// coroutine context
bool queued = co_await sender.send_for(std::make_unique<MyMessage>(), 50ms);

// external thread context
bool queued_sync = sender.sync_send_for(std::make_unique<MyMessage>(), 50ms);
```

`Mailbox<T>` runtime metrics:

- `buffered_size()`
- `is_bounded()`
- `max_buffered_messages()`

## TLS BIO Interface

`flux` does not implement TLS protocol internals. It exposes a BIO-style interface so
you can plug an external TLS engine:

- `flux::TlsBio`
- `flux::TlsBioResult`
- `flux::TlsBioStatus`

Typical usage:

1. provide an adapter around your SSL library context (for example mbedTLS/OpenSSL)
2. feed encrypted bytes with `write_encrypted(...)`
3. pull encrypted bytes with `read_encrypted(...)`
4. feed plain bytes with `write_plain(...)`
5. pull plain bytes with `read_plain(...)`

`tests/flux_trace_test` uses mbedTLS as a lightweight reference implementation.

## File Watcher

`FsWatcher` wraps Linux `inotify` and provides filesystem change events as task/stream APIs.

```cpp
auto watcher = co_await flux::FsWatcher::watch("/tmp/demo.txt");

auto event = co_await watcher.next_for(std::chrono::milliseconds(500));
if (event && event->ok() && event->modified()) {
    // file is modified
}
```

`tests/flux_fs_watch_test` validates `FsWatcher` behavior.

## Behavior Contracts

- `Mailbox<T>::recv()` returns `std::nullopt` only after close and queue drain.
- `MessageSender<T>::send/try_send` return `false` on closed mailbox, full bounded queue, or notify failure.
- When overflow policy is `drop_oldest`, full queue keeps newest message by dropping oldest buffered message.
- `MessageSender<T>::send_for/send_until` are coroutine APIs and do not block the runtime thread.
- `MessageSender<T>::sync_send_for/sync_send_until` are blocking APIs for non-coroutine external threads.
- `MessageSender<T>::send_for/send_until` use adaptive async retry while waiting for capacity.
- `Stream<T>::next()` returns `std::nullopt` for end-of-stream and allows only one in-flight `next()` call per stream instance.

## Usage Samples

### File IO

```cpp
co_await flux::Fs::write_file("demo.txt", "hello\n");
auto text = co_await flux::Fs::read_file_for("demo.txt", std::chrono::milliseconds(50));
```

### TCP

```cpp
auto client = co_await flux::TcpSocket::connect_for(
    "127.0.0.1", 8080, std::chrono::seconds(1));
co_await client.send_all("ping");

std::string reply;
auto chunks = client.receive_chunks(4);
while (auto chunk = co_await chunks.next()) {
    reply += *chunk;
}
```

### UDP

```cpp
auto socket = co_await flux::UdpSocket::connect_for(
    flux::SocketAddress::ipv4("127.0.0.1", 9000), std::chrono::seconds(1));
co_await socket.send("ping");
auto reply = co_await socket.receive_for(std::chrono::seconds(1));
```

### Scope

```cpp
auto result = co_await flux::with_task_scope(
    [](flux::TaskScope& scope) -> flux::Task<int> {
        std::vector<flux::Task<int>> tasks;
        tasks.push_back(fetch_a());
        tasks.push_back(fetch_b());

        auto values = co_await scope.all(tasks);
        auto fastest = co_await scope.any_success(fetch_c(), fetch_d());
        co_return values[0] + values[1] + fastest;
    });
```

### Mailbox

`Mailbox<T>` requires move-only `T` (non-copyable), so sending always transfers ownership.

```cpp
auto mailbox = co_await flux::Mailbox<std::unique_ptr<int>>::create();
auto sender = mailbox.sender();
sender.send(std::make_unique<int>(42));
sender.close();

std::vector<int> values;
auto stream = mailbox.messages();
while (auto value = co_await stream.next()) {
    values.push_back(**value);
}
```

## Demo

Run:

```bash
build/flux_demo
```

The demo covers:

- file IO
- DNS helpers
- timer usage
- TCP echo
- UDP echo
- mailbox messaging
- scope `all(...)` and `any_success(...)`

## Testing

Main test binaries:

```bash
build/flux_smoke_test
build/flux_scope_test
build/flux_constraints_test
build/flux_trace_test
build/flux_fs_watch_test
```

`flux_smoke_test` focuses on:

- filesystem APIs
- TCP and UDP flows
- timeouts
- cancellation
- mailbox and watcher behavior

`flux_scope_test` focuses on:

- scope lifecycle
- `spawn_blocking`
- `all`, `race`, and `any_success`
- timeout behavior inside scopes
- RAII cleanup

`flux_constraints_test` focuses on:

- move-only mailbox concept constraints
- compile-time guardrails (including `try_compile` rejection for `Mailbox<int>`)

`flux_trace_test` focuses on:

- TLS BIO interface contract
- mbedTLS-based in-memory client/server handshake and encrypted payload exchange

`flux_fs_watch_test` focuses on:

- `FsWatcher` change events (`modified`, `renamed`, `removed`)
- filesystem change observation on native Linux paths

Watcher note:

- watcher tests are recommended on native Linux filesystem paths (for example `/tmp` or `/home/...`)
- avoid running watcher-sensitive tests on virtual mount paths such as `/mnt/*`
