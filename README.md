# async_uv

`async_uv` is a C++23 async IO library built on top of `libuv` and `async_simple`.

It aims for:

- sync-style coroutine APIs for file, timer, TCP, UDP, watcher, and mailbox work
- a `libuv`-backed `async_simple::Executor`
- unified cancellation and timeout handling
- structured concurrency with RAII cleanup

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

## Core Pieces

### Runtime

`async_uv::Runtime` owns a `libuv` loop and also serves as an `async_simple::Executor`.

```cpp
async_uv::Runtime runtime;
runtime.block_on(my_task());
```

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
auto reply = co_await client.receive_exactly_for(4, std::chrono::seconds(1));
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

```cpp
auto mailbox = co_await async_uv::Mailbox<int>::create();
auto sender = mailbox.sender();
sender.send(42);

auto value = co_await mailbox.recv_until(
    std::chrono::steady_clock::now() + std::chrono::seconds(1));
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
