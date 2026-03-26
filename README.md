# async_uv

`async_uv` is a C++23 async IO library built on top of `libuv` and `async_simple`.

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
- `cmake/async_uv_dependencies.cmake`: shared dependency setup

If another project only wants the library target, it can include just the library directory:

```cmake
add_subdirectory(path/to/async_uv/src async_uv)
target_link_libraries(my_app PRIVATE async_uv::async_uv)
```

If you add the repository root instead, examples and tests are controlled by:

- `ASYNC_UV_BUILD_EXAMPLES`
- `ASYNC_UV_BUILD_TESTS`
- `ASYNC_UV_USE_MIMALLOC`
- `ASYNC_UV_ENABLE_LAYER2` (build layer2 HTTP wrapper target `async_uv_http`)
- `ASYNC_UV_ENABLE_LAYER3` (build layer3 target; automatically enables layer2)

Layer relationship:

- layer1: `async_uv` (base IO runtime)
- layer2: `async_uv_http` (depends on layer1)
- layer3: `async_uv_layer3` (depends on layer2, and transitively layer1)

Example:

```bash
cmake -S . -B build -G Ninja -DASYNC_UV_ENABLE_LAYER2=ON
cmake --build build
```

Then link:

```cmake
target_link_libraries(my_app PRIVATE async_uv_http)
```

`async_uv_http` uses libcurl. OpenSSL backend is preferred. On Windows, if OpenSSL is
unavailable, the build falls back to Schannel when building curl from source.

## Core Pieces

### Layer2 HTTP

`async_uv_http` provides a minimal async API similar to reqwest-style ergonomics:

```cpp
auto runtime = co_await async_uv::get_current_runtime();

auto client = async_uv::http::Client::build()
    .runtime(*runtime) // 可选；不传时使用当前运行时 / optional; fallback to current runtime
    .default_header("Accept", "application/json")
    .timeout(std::chrono::seconds(20))
    .build();

std::map<std::string, std::string> payload{{"hello", "world"}};
DemoPayload request_data{"world", "cpp"};
HttpBinPostJsonEcho response_json = co_await client.post("https://httpbin.org/post")
    .json(request_data)
    .request_format(async_uv::http::RequestFormat::json)   // 也支持 .request_format("json")
    .response_format(async_uv::http::ResponseFormat::json) // 也支持 .response_format("json")
    .send()
    .json<HttpBinPostJsonEcho>();

auto upload_json = co_await client.post("https://httpbin.org/post")
    .multipart({
        async_uv::http::field("kind", "demo"),
        async_uv::http::field("lang", "cpp"),
        async_uv::http::file("file", "/tmp/demo.txt")
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
        async_uv::http::file("image", "/tmp/demo.png")
            .filename("demo.png")
            .content_type("image/png"),
    })
    .send()
    .json<nlohmann::json>();
```

`Client` uses reqwest-like chained request builders (`get/post/put/del -> query/json/xml/urlencoded/multipart -> send`).
By default, non-2xx responses throw `async_uv::http::HttpError` with structured error fields.
Response format defaults to JSON if not set explicitly.
You can also set request format explicitly via `request_format(...)`.
`query(...)` supports map/unordered_map-style containers and values that are string-like,
compatible with `std::to_string`, or provide `to_string()/toString()` members.
`query(...)` also supports initializer-list pairs such as `.query({{"lang", "cpp"}, {"sort", "desc"}})`.
`urlencoded(container)` now applies percent-encoding for keys and values.

The layer2 library itself does not depend on any JSON library. You can inject your own codec.
For example, with modern JSON (`nlohmann::json`) in application code:

```cpp
#include <nlohmann/json.hpp>

class NlohmannJsonCodec : public async_uv::http::JsonCodec {
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

auto runtime = co_await async_uv::get_current_runtime();
auto client = async_uv::http::Client::build()
    .runtime(*runtime)
    .json_codec(std::make_shared<NlohmannJsonCodec>())
    .user_agent("my-app/1.0")
    .build();
```

### Runtime

`async_uv::Runtime` owns a `libuv` loop and also serves as an `async_simple::Executor`.

```cpp
async_uv::Runtime runtime;
runtime.block_on(my_task());
```

You can optionally set the `libuv` worker threadpool size when creating a runtime:

```cpp
async_uv::Runtime runtime(async_uv::Runtime::build().uv_threadpool_size(8));
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

async_uv::set_trace_hook([mailbox_events](const async_uv::TraceEvent& event) {
    if (std::string_view(event.category) == "mailbox") {
        mailbox_events->fetch_add(1, std::memory_order_relaxed);
    }

    // event.name: send / send_rejected_full / send_timeout / recv / recv_closed ...
    // event.code: error code (0 means no error)
    // event.value: extra numeric value (for mailbox, typically queue size snapshot)
});

// ... run async work ...

async_uv::reset_trace_hook();
```

Use `async_uv::reset_trace_hook()` to clear it. Hook callbacks should stay lightweight and
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
auto value = co_await async_uv::with_task_scope(
    [](async_uv::TaskScope& scope) -> async_uv::Task<int> {
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

`spawn_blocking(...)` uses `uv_queue_work`.

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
- `FsEventWatcher::next_for(...)`
- `FsPollWatcher::next_for(...)`

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
- `FsEventWatcher::events()`
- `FsPollWatcher::events()`

## Mailbox Backpressure

`Mailbox<T>` is move-only-message only, and it can optionally be bounded:

```cpp
async_uv::MailboxOptions options;
options.max_buffered_messages = 1024;
options.overflow_policy = async_uv::MailboxOptions::OverflowPolicy::reject_new;
auto mailbox = co_await async_uv::Mailbox<std::unique_ptr<MyMessage>>::create(options);
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

`async_uv` does not implement TLS protocol internals. It exposes a BIO-style interface so
you can plug an external TLS engine:

- `async_uv::TlsBio`
- `async_uv::TlsBioResult`
- `async_uv::TlsBioStatus`

Typical usage:

1. provide an adapter around your SSL library context (for example mbedTLS/OpenSSL)
2. feed encrypted bytes with `write_encrypted(...)`
3. pull encrypted bytes with `read_encrypted(...)`
4. feed plain bytes with `write_plain(...)`
5. pull plain bytes with `read_plain(...)`

`tests/async_uv_tls_bio_test` uses mbedTLS as a lightweight reference implementation.

## FD Watcher

`FdWatcher` wraps `uv_poll_t` and provides readiness events as task/stream APIs.

```cpp
auto watcher = co_await async_uv::FdWatcher::watch(fd,
    async_uv::FdEventFlags::readable | async_uv::FdEventFlags::writable);

auto event = co_await watcher.next_for(std::chrono::milliseconds(500));
if (event && event->ok() && event->readable()) {
    // fd becomes readable
}
```

`tests/async_uv_fd_curl_test` uses libcurl (`CURLOPT_CONNECT_ONLY`) to obtain an active socket
and validates `FdWatcher` behavior.

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
co_await async_uv::Fs::write_file("demo.txt", "hello\n");
auto text = co_await async_uv::Fs::read_file_for("demo.txt", std::chrono::milliseconds(50));
```

### TCP

```cpp
auto client = co_await async_uv::TcpSocket::connect_for(
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
auto socket = co_await async_uv::UdpSocket::connect_for(
    async_uv::SocketAddress::ipv4("127.0.0.1", 9000), std::chrono::seconds(1));
co_await socket.send("ping");
auto reply = co_await socket.receive_for(std::chrono::seconds(1));
```

### Scope

```cpp
auto result = co_await async_uv::with_task_scope(
    [](async_uv::TaskScope& scope) -> async_uv::Task<int> {
        std::vector<async_uv::Task<int>> tasks;
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
auto mailbox = co_await async_uv::Mailbox<std::unique_ptr<int>>::create();
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
build/async_uv_demo
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
build/async_uv_smoke_test
build/async_uv_scope_test
build/async_uv_constraints_test
build/async_uv_tls_bio_test
build/async_uv_fd_curl_test   # built on non-Windows when libcurl is available
```

`async_uv_smoke_test` focuses on:

- filesystem APIs
- TCP and UDP flows
- timeouts
- cancellation
- mailbox and watcher behavior

`async_uv_scope_test` focuses on:

- scope lifecycle
- `spawn_blocking`
- `all`, `race`, and `any_success`
- timeout behavior inside scopes
- RAII cleanup

`async_uv_constraints_test` focuses on:

- move-only mailbox concept constraints
- compile-time guardrails (including `try_compile` rejection for `Mailbox<int>`)

`async_uv_tls_bio_test` focuses on:

- TLS BIO interface contract
- mbedTLS-based in-memory client/server handshake and encrypted payload exchange

`async_uv_fd_curl_test` focuses on:

- `FdWatcher` readiness events
- libcurl socket integration (`CURLINFO_ACTIVESOCKET`)
- real-network HTTPS connectivity with fallback URLs (`https://example.com/`, `https://www.cloudflare.com/`, `https://www.wikipedia.org/`)
- `ASYNC_UV_CURL_TEST_URL` can override candidates (single URL or comma-separated URL list)
- when DNS/egress is unavailable, the test prints a skip diagnostic and exits without failing

Watcher note:

- on WSL, watcher tests are recommended on Linux filesystem paths (for example `/tmp` or `/home/...`)
- avoid running watcher-sensitive tests on DrvFS mount paths such as `/mnt/c/...` or `/mnt/d/...`
