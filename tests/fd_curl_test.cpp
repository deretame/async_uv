#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/steady_timer.hpp>
#include <exec/finally.hpp>
#include <unistd.h>

#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#else
extern "C" {
typedef struct Curl_easy CURL;
typedef void CURLM;
typedef int CURLcode;
typedef int CURLMcode;
typedef int CURLoption;
typedef int CURLMoption;
typedef int CURLINFO;
typedef int curl_socket_t;

typedef struct CURLMsg {
    int msg;
    CURL *easy_handle;
    union {
        void *whatever;
        CURLcode result;
    } data;
} CURLMsg;

typedef int (*curl_socket_callback)(
    CURL *easy, curl_socket_t s, int what, void *userp, void *socketp);
typedef int (*curl_multi_timer_callback)(CURLM *multi, long timeout_ms, void *userp);

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *handle);
const char *curl_easy_strerror(CURLcode errornum);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);

CURLM *curl_multi_init(void);
CURLMcode curl_multi_add_handle(CURLM *multi_handle, CURL *curl_handle);
CURLMcode curl_multi_remove_handle(CURLM *multi_handle, CURL *curl_handle);
CURLMcode curl_multi_cleanup(CURLM *multi_handle);
CURLMcode curl_multi_setopt(CURLM *multi_handle, CURLMoption option, ...);
CURLMcode
curl_multi_socket_action(CURLM *multi_handle, curl_socket_t s, int ev_bitmask, int *running_handles);
CURLMsg *curl_multi_info_read(CURLM *multi_handle, int *msgs_in_queue);
const char *curl_multi_strerror(CURLMcode errornum);
}

static constexpr long CURL_GLOBAL_DEFAULT = 3L;
static constexpr CURLcode CURLE_OK = 0;
static constexpr CURLMcode CURLM_OK = 0;

static constexpr CURLoption CURLOPT_URL = 10002;
static constexpr CURLoption CURLOPT_NOSIGNAL = 99;
static constexpr CURLoption CURLOPT_CONNECT_ONLY = 141;
static constexpr CURLoption CURLOPT_TIMEOUT_MS = 155;

static constexpr CURLINFO CURLINFO_ACTIVESOCKET = 0x500000 + 44;

static constexpr CURLMoption CURLMOPT_SOCKETFUNCTION = 20001;
static constexpr CURLMoption CURLMOPT_SOCKETDATA = 10002;
static constexpr CURLMoption CURLMOPT_TIMERFUNCTION = 20004;
static constexpr CURLMoption CURLMOPT_TIMERDATA = 10005;

static constexpr int CURLMSG_DONE = 1;

static constexpr int CURL_POLL_IN = 1;
static constexpr int CURL_POLL_OUT = 2;
static constexpr int CURL_POLL_INOUT = 3;
static constexpr int CURL_POLL_REMOVE = 4;

static constexpr int CURL_CSELECT_IN = 0x01;
static constexpr int CURL_CSELECT_OUT = 0x02;

static constexpr curl_socket_t CURL_SOCKET_BAD = -1;
static constexpr curl_socket_t CURL_SOCKET_TIMEOUT = CURL_SOCKET_BAD;
#endif

#include "async_uv/async_uv.h"

namespace {

namespace asio = exec::asio::asio_impl;

using namespace std::chrono_literals;

std::string curl_easy_error(CURLcode rc) {
    const char *message = curl_easy_strerror(rc);
    if (message == nullptr) {
        return "unknown curl easy error";
    }
    return std::string(message);
}

std::string curl_multi_error(CURLMcode rc) {
    const char *message = curl_multi_strerror(rc);
    if (message == nullptr) {
        return "unknown curl multi error";
    }
    return std::string(message);
}

class CurlEventLoop {
public:
    using FdCallback = std::function<void(int events, std::error_code ec)>;
    using TimerCallback = std::function<void(std::error_code ec)>;

    virtual ~CurlEventLoop() = default;

    virtual bool watch_fd(curl_socket_t socket,
                          bool want_read,
                          bool want_write,
                          FdCallback callback,
                          std::string &error_message) = 0;

    virtual void unwatch_fd(curl_socket_t socket) = 0;

    virtual void arm_timer(std::optional<std::chrono::milliseconds> timeout,
                           TimerCallback callback) = 0;

    virtual void shutdown() = 0;
};

class AsioCurlEventLoop final : public CurlEventLoop,
                                public std::enable_shared_from_this<AsioCurlEventLoop> {
public:
    static std::shared_ptr<AsioCurlEventLoop> create(exec::asio::asio_impl::any_io_executor ex) {
        return std::shared_ptr<AsioCurlEventLoop>(new AsioCurlEventLoop(std::move(ex)));
    }

    bool watch_fd(curl_socket_t socket,
                  bool want_read,
                  bool want_write,
                  FdCallback callback,
                  std::string &error_message) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (stopped_) {
            error_message = "event loop already stopped";
            return false;
        }

        auto watcher = ensure_watcher_locked(socket, error_message);
        if (!watcher) {
            return false;
        }

        watcher->callback = std::move(callback);
        watcher->read_enabled = want_read;
        watcher->write_enabled = want_write;

        if (!watcher->read_enabled) {
            watcher->read_armed = false;
            ++watcher->read_generation;
        }
        if (!watcher->write_enabled) {
            watcher->write_armed = false;
            ++watcher->write_generation;
        }

        arm_read_locked(watcher);
        arm_write_locked(watcher);
        return true;
    }

    void unwatch_fd(curl_socket_t socket) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        remove_watcher_locked(socket);
    }

    void arm_timer(std::optional<std::chrono::milliseconds> timeout,
                   TimerCallback callback) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        timer_callback_ = std::move(callback);

        ++timer_generation_;
        std::error_code ec;
        timer_.cancel(ec);

        if (stopped_ || !timeout.has_value()) {
            return;
        }

        const auto generation = timer_generation_;
        timer_.expires_after(*timeout);

        auto weak = weak_from_this();
        timer_.async_wait([weak, generation](const std::error_code &wait_ec) {
            if (auto self = weak.lock()) {
                self->on_timer(generation, wait_ec);
            }
        });
    }

    void shutdown() override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (stopped_) {
            return;
        }

        stopped_ = true;
        ++timer_generation_;

        std::error_code ec;
        timer_.cancel(ec);

        for (auto &[_, watcher] : watchers_) {
            watcher->close();
        }
        watchers_.clear();
        timer_callback_ = nullptr;
    }

private:
    struct Watcher {
        explicit Watcher(exec::asio::asio_impl::any_io_executor ex,
                         curl_socket_t socket_in,
                         int duplicated_fd)
            : socket(socket_in), descriptor(ex) {
            descriptor.assign(duplicated_fd);
        }

        void close() {
            std::error_code ec;
            descriptor.cancel(ec);
            descriptor.close(ec);
        }

        curl_socket_t socket = CURL_SOCKET_BAD;
        asio::posix::stream_descriptor descriptor;
        bool read_enabled = false;
        bool write_enabled = false;
        bool read_armed = false;
        bool write_armed = false;
        std::uint64_t read_generation = 0;
        std::uint64_t write_generation = 0;
        FdCallback callback;
    };

    explicit AsioCurlEventLoop(exec::asio::asio_impl::any_io_executor ex)
        : executor_(std::move(ex)), timer_(executor_) {}

    std::shared_ptr<Watcher> ensure_watcher_locked(curl_socket_t socket, std::string &error_message) {
        const auto it = watchers_.find(socket);
        if (it != watchers_.end()) {
            return it->second;
        }

        const int duplicated_fd = ::dup(static_cast<int>(socket));
        if (duplicated_fd < 0) {
            error_message = std::string("dup failed: ") + std::strerror(errno);
            return nullptr;
        }

        try {
            auto watcher = std::make_shared<Watcher>(executor_, socket, duplicated_fd);
            watchers_.emplace(socket, watcher);
            return watcher;
        } catch (...) {
            (void)::close(duplicated_fd);
            error_message = "failed to create fd watcher";
            return nullptr;
        }
    }

    void remove_watcher_locked(curl_socket_t socket) {
        const auto it = watchers_.find(socket);
        if (it == watchers_.end()) {
            return;
        }
        it->second->close();
        watchers_.erase(it);
    }

    void arm_read_locked(const std::shared_ptr<Watcher> &watcher) {
        if (!watcher || stopped_ || watcher->read_armed || !watcher->read_enabled) {
            return;
        }

        watcher->read_armed = true;
        ++watcher->read_generation;
        const auto generation = watcher->read_generation;

        auto weak = weak_from_this();
        watcher->descriptor.async_wait(
            asio::posix::stream_descriptor::wait_read,
            [weak, socket = watcher->socket, generation](const std::error_code &ec) {
                if (auto self = weak.lock()) {
                    self->on_socket_ready(socket, true, generation, ec);
                }
            });
    }

    void arm_write_locked(const std::shared_ptr<Watcher> &watcher) {
        if (!watcher || stopped_ || watcher->write_armed || !watcher->write_enabled) {
            return;
        }

        watcher->write_armed = true;
        ++watcher->write_generation;
        const auto generation = watcher->write_generation;

        auto weak = weak_from_this();
        watcher->descriptor.async_wait(
            asio::posix::stream_descriptor::wait_write,
            [weak, socket = watcher->socket, generation](const std::error_code &ec) {
                if (auto self = weak.lock()) {
                    self->on_socket_ready(socket, false, generation, ec);
                }
            });
    }

    void on_socket_ready(curl_socket_t socket,
                         bool read_side,
                         std::uint64_t generation,
                         const std::error_code &ec) {
        FdCallback callback;
        int events = 0;
        std::error_code callback_ec;

        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }

            const auto it = watchers_.find(socket);
            if (it == watchers_.end()) {
                return;
            }

            auto watcher = it->second;
            if (read_side) {
                if (watcher->read_generation != generation) {
                    return;
                }
                watcher->read_armed = false;
            } else {
                if (watcher->write_generation != generation) {
                    return;
                }
                watcher->write_armed = false;
            }

            callback = watcher->callback;
            if (!callback) {
                return;
            }

            if (ec) {
                if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
                    return;
                }
                callback_ec = ec;
            } else {
                if (read_side) {
                    if (!watcher->read_enabled) {
                        return;
                    }
                    events = CURL_CSELECT_IN;
                    arm_read_locked(watcher);
                } else {
                    if (!watcher->write_enabled) {
                        return;
                    }
                    events = CURL_CSELECT_OUT;
                    arm_write_locked(watcher);
                }
            }
        }

        callback(events, callback_ec);
    }

    void on_timer(std::uint64_t generation, const std::error_code &wait_ec) {
        TimerCallback callback;

        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_ || generation != timer_generation_) {
                return;
            }

            callback = timer_callback_;
            if (!callback) {
                return;
            }

            if (wait_ec == asio::error::operation_aborted) {
                return;
            }
        }

        callback(wait_ec);
    }

    exec::asio::asio_impl::any_io_executor executor_;
    asio::steady_timer timer_;
    std::recursive_mutex mutex_;
    std::unordered_map<curl_socket_t, std::shared_ptr<Watcher>> watchers_;
    TimerCallback timer_callback_;
    std::uint64_t timer_generation_ = 0;
    bool stopped_ = false;
};

class AsyncCurlConnector {
public:
    static async_uv::Task<async_uv::FdStream>
    connect_only(std::string url, std::chrono::milliseconds timeout = 8s);

private:
    struct State;

    static int on_socket(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp);
    static int on_timer(CURLM *multi, long timeout_ms, void *userp);
};

struct AsyncCurlConnector::State : std::enable_shared_from_this<AsyncCurlConnector::State> {
    explicit State(async_uv::Runtime *rt, std::shared_ptr<CurlEventLoop> io_loop)
        : runtime(rt), io(std::move(io_loop)) {}

    void fail_locked(std::string message) {
        if (done) {
            return;
        }
        failed = true;
        done = true;
        error_message = std::move(message);
    }

    void drain_messages_locked() {
        if (done || multi == nullptr) {
            return;
        }

        int queued = 0;
        while (auto *msg = curl_multi_info_read(multi, &queued)) {
            if (msg->msg != CURLMSG_DONE) {
                continue;
            }

            done = true;
            const auto easy_result = msg->data.result;
            if (easy_result != CURLE_OK) {
                failed = true;
                error_message = std::string("curl transfer failed: ") + curl_easy_error(easy_result);
                return;
            }

            curl_socket_t socket = CURL_SOCKET_BAD;
            const auto info_rc = curl_easy_getinfo(msg->easy_handle, CURLINFO_ACTIVESOCKET, &socket);
            if (info_rc != CURLE_OK || socket == CURL_SOCKET_BAD) {
                failed = true;
                error_message = "curl finished but no active socket available";
                return;
            }

            active_socket = socket;
            return;
        }
    }

    void perform_locked(curl_socket_t socket, int events) {
        if (done || failed || shutting_down || multi == nullptr) {
            return;
        }

        const auto mrc = curl_multi_socket_action(multi, socket, events, &running_handles);
        if (mrc != CURLM_OK) {
            fail_locked(std::string("curl_multi_socket_action failed: ") + curl_multi_error(mrc));
            return;
        }
        drain_messages_locked();
    }

    void apply_socket_interest_locked(curl_socket_t socket, int what) {
        if (done || failed || shutting_down || !io) {
            return;
        }

        if (what == CURL_POLL_REMOVE) {
            io->unwatch_fd(socket);
            return;
        }

        const bool want_read = (what == CURL_POLL_IN || what == CURL_POLL_INOUT);
        const bool want_write = (what == CURL_POLL_OUT || what == CURL_POLL_INOUT);

        auto weak = weak_from_this();
        std::string watch_error;
        const bool ok = io->watch_fd(
            socket,
            want_read,
            want_write,
            [weak, socket](int events, std::error_code ec) {
                auto self = weak.lock();
                if (!self) {
                    return;
                }
                std::lock_guard<std::recursive_mutex> lock(self->mutex);
                if (ec) {
                    self->fail_locked(std::string("fd event error: ") + ec.message());
                    return;
                }
                self->perform_locked(socket, events);
            },
            watch_error);

        if (!ok) {
            fail_locked(std::move(watch_error));
        }
    }

    void arm_timer_locked(long timeout_ms) {
        if (done || failed || shutting_down || !io) {
            return;
        }

        if (timeout_ms < 0) {
            io->arm_timer(std::nullopt, {});
            return;
        }

        const auto delay = timeout_ms <= 0 ? 0ms : std::chrono::milliseconds(timeout_ms);
        auto weak = weak_from_this();
        io->arm_timer(delay, [weak](std::error_code ec) {
            auto self = weak.lock();
            if (!self) {
                return;
            }
            std::lock_guard<std::recursive_mutex> lock(self->mutex);
            if (ec) {
                self->fail_locked(std::string("timer error: ") + ec.message());
                return;
            }
            self->perform_locked(CURL_SOCKET_TIMEOUT, 0);
        });
    }

    void shutdown_locked() {
        if (shutting_down) {
            return;
        }
        shutting_down = true;

        if (io) {
            io->shutdown();
            io.reset();
        }

        if (multi && easy) {
            (void)curl_multi_remove_handle(multi, easy);
        }
        if (easy) {
            curl_easy_cleanup(easy);
            easy = nullptr;
        }
        if (multi) {
            (void)curl_multi_cleanup(multi);
            multi = nullptr;
        }
    }

    async_uv::Runtime *runtime = nullptr;
    std::shared_ptr<CurlEventLoop> io;
    std::recursive_mutex mutex;

    CURLM *multi = nullptr;
    CURL *easy = nullptr;
    int running_handles = 0;
    curl_socket_t active_socket = CURL_SOCKET_BAD;
    bool done = false;
    bool failed = false;
    bool shutting_down = false;
    std::string error_message;
};

int AsyncCurlConnector::on_socket(CURL *, curl_socket_t s, int what, void *userp, void *) {
    auto *state = static_cast<State *>(userp);
    if (state == nullptr) {
        return 0;
    }

    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    state->apply_socket_interest_locked(s, what);
    return 0;
}

int AsyncCurlConnector::on_timer(CURLM *, long timeout_ms, void *userp) {
    auto *state = static_cast<State *>(userp);
    if (state == nullptr) {
        return 0;
    }

    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    state->arm_timer_locked(timeout_ms);
    return 0;
}

async_uv::Task<async_uv::FdStream>
AsyncCurlConnector::connect_only(std::string url, std::chrono::milliseconds timeout) {
    auto *runtime = co_await async_uv::get_current_runtime();
    auto event_loop = AsioCurlEventLoop::create(runtime->executor());
    auto state = std::make_shared<State>(runtime, std::move(event_loop));

    try {
        state->multi = curl_multi_init();
        if (state->multi == nullptr) {
            throw std::runtime_error("curl_multi_init failed");
        }

        state->easy = curl_easy_init();
        if (state->easy == nullptr) {
            throw std::runtime_error("curl_easy_init failed");
        }

        if (curl_easy_setopt(state->easy, CURLOPT_URL, url.c_str()) != CURLE_OK) {
            throw std::runtime_error("curl_easy_setopt CURLOPT_URL failed");
        }
        if (curl_easy_setopt(state->easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK) {
            throw std::runtime_error("curl_easy_setopt CURLOPT_NOSIGNAL failed");
        }
        if (curl_easy_setopt(state->easy, CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK) {
            throw std::runtime_error("curl_easy_setopt CURLOPT_CONNECT_ONLY failed");
        }
        if (curl_easy_setopt(state->easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count())) !=
            CURLE_OK) {
            throw std::runtime_error("curl_easy_setopt CURLOPT_TIMEOUT_MS failed");
        }

        if (curl_multi_setopt(state->multi, CURLMOPT_SOCKETFUNCTION, &AsyncCurlConnector::on_socket) !=
            CURLM_OK) {
            throw std::runtime_error("curl_multi_setopt CURLMOPT_SOCKETFUNCTION failed");
        }
        if (curl_multi_setopt(state->multi, CURLMOPT_SOCKETDATA, state.get()) != CURLM_OK) {
            throw std::runtime_error("curl_multi_setopt CURLMOPT_SOCKETDATA failed");
        }
        if (curl_multi_setopt(state->multi, CURLMOPT_TIMERFUNCTION, &AsyncCurlConnector::on_timer) !=
            CURLM_OK) {
            throw std::runtime_error("curl_multi_setopt CURLMOPT_TIMERFUNCTION failed");
        }
        if (curl_multi_setopt(state->multi, CURLMOPT_TIMERDATA, state.get()) != CURLM_OK) {
            throw std::runtime_error("curl_multi_setopt CURLMOPT_TIMERDATA failed");
        }

        {
            std::lock_guard<std::recursive_mutex> lock(state->mutex);
            const auto add_rc = curl_multi_add_handle(state->multi, state->easy);
            if (add_rc != CURLM_OK) {
                throw std::runtime_error(std::string("curl_multi_add_handle failed: ") +
                                         curl_multi_error(add_rc));
            }
            state->perform_locked(CURL_SOCKET_TIMEOUT, 0);
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout + 1s;
        while (true) {
            {
                std::lock_guard<std::recursive_mutex> lock(state->mutex);
                if (state->done || state->failed) {
                    break;
                }
            }

            if (std::chrono::steady_clock::now() > deadline) {
                std::lock_guard<std::recursive_mutex> lock(state->mutex);
                state->fail_locked("timed out waiting for non-blocking curl connect-only");
                break;
            }

            co_await async_uv::sleep_for(1ms);
        }

        curl_socket_t active_socket = CURL_SOCKET_BAD;
        {
            std::lock_guard<std::recursive_mutex> lock(state->mutex);
            if (state->failed) {
                throw std::runtime_error(state->error_message);
            }
            if (state->active_socket == CURL_SOCKET_BAD) {
                throw std::runtime_error("curl connect-only finished without active socket");
            }
            active_socket = state->active_socket;
        }

        auto stream = co_await async_uv::FdStream::attach(static_cast<int>(active_socket));

        {
            std::lock_guard<std::recursive_mutex> lock(state->mutex);
            state->shutdown_locked();
        }

        co_return stream;
    } catch (...) {
        std::lock_guard<std::recursive_mutex> lock(state->mutex);
        state->shutdown_locked();
        throw;
    }
}

async_uv::Task<void> run_server(async_uv::TcpListener listener) {
    auto client = co_await listener.accept();

    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
        auto chunk = co_await client.read_some(1024);
        if (chunk.empty() && client.eof()) {
            break;
        }
        request += chunk;
    }

    assert(request.find("GET / HTTP/1.1") != std::string::npos);
    assert(request.find("Host: 127.0.0.1") != std::string::npos);

    const std::string body = "hello from async_uv fd stream";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Connection: close\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n" + body;

    const std::size_t written = co_await client.write_all(response);
    assert(written == response.size());

    co_await client.shutdown();
    co_await client.close();
    co_await listener.close();
}

async_uv::Task<std::string> receive_response(async_uv::FdStream &stream) {
    std::string response;
    while (true) {
        auto chunk = co_await stream.read_some_sender(2048);
        if (chunk.empty()) {
            break;
        }
        response += std::move(chunk);
    }
    co_return response;
}

async_uv::Task<void>
close_managed_stream(std::shared_ptr<std::optional<async_uv::FdStream>> managed_stream) {
    if (managed_stream && managed_stream->has_value() && managed_stream->value().valid()) {
        co_await (managed_stream->value().close() | stdexec::unstoppable);
    }
    if (managed_stream) {
        managed_stream->reset();
    }
}

async_uv::Task<void> close_fd_stream_unstoppable(async_uv::FdStream &stream) {
    if (stream.valid()) {
        co_await (stream.close() | stdexec::unstoppable);
    }
}

std::string http_body_from_response(std::string response) {
    const auto sep = response.find("\r\n\r\n");
    if (sep == std::string::npos) {
        throw std::runtime_error("HTTP response missing header/body separator");
    }
    return response.substr(sep + 4);
}

async_uv::Task<std::string> fetch_http_body_with_curl(int port, std::string path) {
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
    const std::string request = "GET " + path +
                                " HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Connection: close\r\n\r\n";

    auto managed_stream = std::make_shared<std::optional<async_uv::FdStream>>();
    auto pipeline = AsyncCurlConnector::connect_only(url, 8s)
                  | stdexec::then(
                        [managed_stream](async_uv::FdStream stream) {
                            assert(stream.valid());
                            managed_stream->emplace(std::move(stream));
                        })
                  | stdexec::let_value(
                        [request, managed_stream]() mutable {
                            return managed_stream->value().write_all_sender(std::move(request));
                        })
                  | stdexec::then([expected = request.size()](std::size_t sent) {
                        assert(sent == expected);
                    })
                  | stdexec::let_value(
                        [managed_stream]() {
                            return receive_response(managed_stream->value());
                        })
                  | stdexec::then([](std::string response) {
                        assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
                        return http_body_from_response(std::move(response));
                    })
                  | exec::finally(close_managed_stream(managed_stream));

    co_return co_await std::move(pipeline);
}

enum class RedisRespType {
    simple_string,
    bulk_string,
    integer,
};

struct RedisRespValue {
    RedisRespType type = RedisRespType::simple_string;
    std::optional<std::string> value;
    std::optional<long long> integer;
};

std::string make_redis_command(std::initializer_list<std::string_view> parts) {
    std::string payload;
    payload.reserve(128);
    payload += "*";
    payload += std::to_string(parts.size());
    payload += "\r\n";
    for (std::string_view part : parts) {
        payload += "$";
        payload += std::to_string(part.size());
        payload += "\r\n";
        payload.append(part.data(), part.size());
        payload += "\r\n";
    }
    return payload;
}

async_uv::Task<async_uv::FdStream> connect_redis_stream() {
    auto ex = co_await async_uv::get_current_loop();
    asio::ip::tcp::resolver resolver(ex);
    auto endpoints = co_await resolver.async_resolve("127.0.0.1", "6379", exec::asio::use_sender);

    asio::ip::tcp::socket socket(ex);
    co_await asio::async_connect(socket, endpoints, exec::asio::use_sender);

    auto stream = co_await async_uv::FdStream::attach(socket.native_handle());
    std::error_code ec;
    socket.close(ec);
    co_return stream;
}

async_uv::Task<std::string> read_redis_line(async_uv::FdStream &stream, std::string &buffer) {
    while (true) {
        const auto pos = buffer.find("\r\n");
        if (pos != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            co_return line;
        }

        auto chunk = co_await stream.read_some_sender(2048);
        if (chunk.empty()) {
            throw std::runtime_error("redis connection closed while reading line");
        }
        buffer += std::move(chunk);
    }
}

async_uv::Task<void>
read_redis_exact(async_uv::FdStream &stream, std::string &buffer, std::size_t bytes) {
    while (buffer.size() < bytes) {
        auto chunk = co_await stream.read_some_sender(2048);
        if (chunk.empty()) {
            throw std::runtime_error("redis connection closed while reading payload");
        }
        buffer += std::move(chunk);
    }
}

async_uv::Task<RedisRespValue> read_redis_reply(async_uv::FdStream &stream, std::string &buffer) {
    const std::string line = co_await read_redis_line(stream, buffer);
    if (line.empty()) {
        throw std::runtime_error("invalid empty redis response line");
    }

    const char prefix = line.front();
    const std::string payload = line.substr(1);

    if (prefix == '+') {
        co_return RedisRespValue{RedisRespType::simple_string, payload};
    }

    if (prefix == '-') {
        throw std::runtime_error("redis error reply: " + payload);
    }

    if (prefix == '$') {
        const long long length = std::stoll(payload);
        if (length < -1) {
            throw std::runtime_error("invalid redis bulk length");
        }
        if (length == -1) {
            co_return RedisRespValue{RedisRespType::bulk_string, std::nullopt};
        }

        const std::size_t bulk_len = static_cast<std::size_t>(length);
        co_await read_redis_exact(stream, buffer, bulk_len + 2);
        if (buffer[bulk_len] != '\r' || buffer[bulk_len + 1] != '\n') {
            throw std::runtime_error("invalid redis bulk terminator");
        }

        std::string value = buffer.substr(0, bulk_len);
        buffer.erase(0, bulk_len + 2);
        co_return RedisRespValue{RedisRespType::bulk_string, std::move(value), std::nullopt};
    }

    if (prefix == ':') {
        const long long count = std::stoll(payload);
        co_return RedisRespValue{RedisRespType::integer, std::nullopt, count};
    }

    throw std::runtime_error("unsupported redis response type");
}

async_uv::Task<RedisRespValue> run_redis_command(std::string command) {
    auto stream = co_await connect_redis_stream();
    std::exception_ptr pending_error;
    RedisRespValue reply;
    std::string buffer;

    try {
        auto flow = stream.write_all_sender(std::move(command))
                  | stdexec::then([](std::size_t sent) {
                        assert(sent > 0);
                    })
                  | stdexec::let_value([&] {
                        return read_redis_reply(stream, buffer);
                    });
        reply = co_await std::move(flow);
    } catch (...) {
        pending_error = std::current_exception();
    }

    co_await close_fd_stream_unstoppable(stream);
    if (pending_error) {
        std::rethrow_exception(pending_error);
    }
    co_return reply;
}

async_uv::Task<void> redis_set_value(std::string key, std::string value) {
    auto reply = co_await run_redis_command(
        make_redis_command({"SET", std::string_view(key), std::string_view(value)}));
    assert(reply.type == RedisRespType::simple_string);
    assert(reply.value.has_value());
    assert(*reply.value == "OK");
}

async_uv::Task<std::optional<std::string>> redis_get_value(std::string key) {
    auto reply = co_await run_redis_command(make_redis_command({"GET", std::string_view(key)}));
    assert(reply.type == RedisRespType::bulk_string);
    co_return reply.value;
}

async_uv::Task<long long> redis_del_key(std::string key) {
    auto reply = co_await run_redis_command(make_redis_command({"DEL", std::string_view(key)}));
    assert(reply.type == RedisRespType::integer);
    assert(reply.integer.has_value());
    co_return *reply.integer;
}

async_uv::Task<bool> redis_ping_available() {
    try {
        auto reply = co_await run_redis_command(make_redis_command({"PING"}));
        co_return reply.type == RedisRespType::simple_string && reply.value.has_value() &&
                  *reply.value == "PONG";
    } catch (...) {
        co_return false;
    }
}

std::string request_target_from_http_request(std::string_view request) {
    const auto line_end = request.find("\r\n");
    if (line_end == std::string_view::npos) {
        throw std::runtime_error("invalid HTTP request line");
    }

    const std::string_view first_line = request.substr(0, line_end);
    if (!first_line.starts_with("GET ")) {
        throw std::runtime_error("unexpected HTTP method in pipeline server");
    }

    const auto target_start = static_cast<std::size_t>(4);
    const auto target_end = first_line.find(' ', target_start);
    if (target_end == std::string_view::npos || target_end <= target_start) {
        throw std::runtime_error("invalid HTTP request target");
    }
    return std::string(first_line.substr(target_start, target_end - target_start));
}

std::string response_body_for_target(std::string_view target) {
    if (target == "/first") {
        return "curl-first-body";
    }
    if (target.starts_with("/second/")) {
        return std::string("curl-second-body:") + std::string(target.substr(8));
    }
    if (target == "/third") {
        return "curl-third-body";
    }
    throw std::runtime_error("unexpected request target in pipeline server");
}

async_uv::Task<void> run_pipeline_server(async_uv::TcpListener listener) {
    constexpr int expected_requests = 3;
    for (int i = 0; i < expected_requests; ++i) {
        auto client = co_await listener.accept();

        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto chunk = co_await client.read_some(1024);
            if (chunk.empty() && client.eof()) {
                break;
            }
            request += chunk;
        }

        assert(request.find("Host: 127.0.0.1") != std::string::npos);
        const std::string target = request_target_from_http_request(request);
        const std::string body = response_body_for_target(target);
        const std::string response = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Connection: close\r\n"
                                     "Content-Length: " +
                                     std::to_string(body.size()) + "\r\n\r\n" + body;

        const std::size_t written = co_await client.write_all(response);
        assert(written == response.size());
        co_await client.shutdown();
        co_await client.close();
    }

    co_await listener.close();
}

async_uv::Task<void> run_client_curl_redis_pipeline(int port) {
    const std::string redis_key = "async_uv:curl:redis:pipeline";
    auto pipeline = fetch_http_body_with_curl(port, "/first")
                  | stdexec::then([](std::string body) {
                        assert(body == "curl-first-body");
                        return body;
                    })
                  | stdexec::let_value([&](std::string body) {
                        std::string carried = body;
                        return redis_set_value(redis_key, body)
                             | stdexec::then([carried = std::move(carried)]() mutable {
                                   return std::move(carried);
                               });
                    })
                  | stdexec::let_value([port](std::string body) {
                        std::string carried = body;
                        std::string second_path = "/second/" + body;
                        return fetch_http_body_with_curl(port, std::move(second_path))
                             | stdexec::then([carried = std::move(carried)](
                                                 std::string second_body) mutable {
                                   const std::string expected = "curl-second-body:" + carried;
                                   if (second_body != expected) {
                                       throw std::runtime_error(
                                           "second response mismatch, expected=[" + expected +
                                           "], actual=[" + second_body + "]");
                                   }
                                   return std::move(carried);
                               });
                    })
                  | stdexec::let_value([&](std::string body) {
                        return redis_del_key(redis_key)
                             | stdexec::then([body = std::move(body)](long long removed) mutable {
                                   assert(removed == 1);
                                   return std::move(body);
                               });
                    })
                  | stdexec::let_value([port](std::string body) {
                        return fetch_http_body_with_curl(port, "/third")
                             | stdexec::then([body = std::move(body)](std::string third_body) mutable {
                                   assert(third_body == "curl-third-body");
                                   return std::move(body);
                               });
                    })
                  | stdexec::let_value([&](std::string body) {
                        return redis_get_value(redis_key)
                             | stdexec::then([body = std::move(body)](
                                                 std::optional<std::string> maybe_deleted) mutable {
                                   assert(!maybe_deleted.has_value());
                                   return std::move(body);
                               });
                    })
                  | stdexec::then([](std::string body) {
                        assert(body == "curl-first-body");
                    });

    co_await std::move(pipeline);
}

async_uv::Task<void> run_client_curl_redis_pipeline_coroutine(int port) {
    const std::string redis_key = "async_uv:curl:redis:coroutine";

    const std::string first_body = co_await fetch_http_body_with_curl(port, "/first");
    assert(first_body == "curl-first-body");

    co_await redis_set_value(redis_key, first_body);

    const std::string second_body = co_await fetch_http_body_with_curl(port, "/second/" + first_body);
    const std::string expected_second = "curl-second-body:" + first_body;
    if (second_body != expected_second) {
        throw std::runtime_error(
            "coroutine second response mismatch, expected=[" + expected_second + "], actual=[" +
            second_body + "]");
    }

    const long long removed = co_await redis_del_key(redis_key);
    assert(removed == 1);

    const std::string third_body = co_await fetch_http_body_with_curl(port, "/third");
    assert(third_body == "curl-third-body");

    auto maybe_deleted = co_await redis_get_value(redis_key);
    assert(!maybe_deleted.has_value());
}

async_uv::Task<void> run_client_coroutine(int port) {
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    const std::string request = "GET / HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Connection: close\r\n\r\n";

    auto stream = co_await AsyncCurlConnector::connect_only(url, 8s);
    assert(stream.valid());

    std::exception_ptr pending_error;
    try {
        const std::size_t sent = co_await async_uv::with_timeout(2s, stream.write_all(request));
        assert(sent == request.size());

        std::string response;
        while (true) {
            auto chunk = co_await async_uv::with_timeout(2s, stream.read_some(2048));
            if (chunk.empty()) {
                break;
            }
            response += std::move(chunk);
        }

        assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(response.find("hello from async_uv fd stream") != std::string::npos);
    } catch (...) {
        pending_error = std::current_exception();
    }

    co_await (stream.close() | stdexec::unstoppable);
    if (pending_error) {
        std::rethrow_exception(pending_error);
    }
}

async_uv::Task<void> run_client(int port) {
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    const std::string request = "GET / HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Connection: close\r\n\r\n";

    // Keep the connected stream alive through the whole pipeline so that `finally`
    // can close it at the very end without leaking resources on errors/timeouts.
    auto managed_stream = std::make_shared<std::optional<async_uv::FdStream>>();

    // Full pipeline demo:
    // 1) connect_only (non-blocking curl multi + fd/timer driving)
    // 2) store the connected stream handle for downstream stages
    // 3) send full HTTP request with timeout
    // 4) receive full HTTP response with timeout
    // 5) validate response payload
    // 6) always close stream in finally (success/failure/cancel all covered)
    auto pipeline = AsyncCurlConnector::connect_only(url, 8s)
                  | stdexec::then(
                        [managed_stream](async_uv::FdStream stream) {
                            assert(stream.valid());
                            managed_stream->emplace(std::move(stream));
                        })
                  | stdexec::let_value(
                        [request, managed_stream]() mutable {
                            return managed_stream->value().write_all_sender(std::move(request));
                        })
                  | stdexec::then([expected = request.size()](std::size_t sent) {
                        assert(sent == expected);
                        })
                  | stdexec::let_value(
                        [managed_stream]() {
                            return receive_response(managed_stream->value());
                        })
                  | stdexec::then([](std::string response) {
                        assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
                        assert(response.find("hello from async_uv fd stream") != std::string::npos);
                        return response;
                    })
                  | exec::finally(close_managed_stream(managed_stream));

    [[maybe_unused]] std::string response = co_await std::move(pipeline);

    // Response assertions are already part of the pipeline to keep the full
    // request/response lifecycle in one composable sender chain.
}

async_uv::Task<void> run_fd_curl_sender_test() {
    auto listener = co_await async_uv::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    assert(port > 0);

    co_await async_uv::scope::all(run_server(std::move(listener)), run_client(port));
}

async_uv::Task<void> run_fd_curl_coroutine_test() {
    auto listener = co_await async_uv::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    assert(port > 0);

    co_await async_uv::scope::all(run_server(std::move(listener)), run_client_coroutine(port));
}

async_uv::Task<void> run_fd_curl_redis_pipeline_test() {
    if (!(co_await redis_ping_available())) {
        std::cerr << "[skip] redis unavailable at 127.0.0.1:6379, skip curl+redis pipeline test\n";
        co_return;
    }

    auto listener = co_await async_uv::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    assert(port > 0);

    co_await async_uv::scope::all(
        run_pipeline_server(std::move(listener)),
        run_client_curl_redis_pipeline(port));
}

async_uv::Task<void> run_fd_curl_redis_pipeline_coroutine_test() {
    if (!(co_await redis_ping_available())) {
        std::cerr
            << "[skip] redis unavailable at 127.0.0.1:6379, skip curl+redis coroutine pipeline test\n";
        co_return;
    }

    auto listener = co_await async_uv::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    assert(port > 0);

    co_await async_uv::scope::all(
        run_pipeline_server(std::move(listener)),
        run_client_curl_redis_pipeline_coroutine(port));
}

async_uv::Task<void> run_fd_curl_test() {
    co_await run_fd_curl_sender_test();
    co_await run_fd_curl_coroutine_test();
    co_await run_fd_curl_redis_pipeline_test();
    co_await run_fd_curl_redis_pipeline_coroutine_test();
}

} // namespace

int main() {
    const CURLcode init_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init_rc != CURLE_OK) {
        return 1;
    }

    try {
        async_uv::Runtime runtime(async_uv::Runtime::build().io_threads(2).blocking_threads(2));
        runtime.block_on(run_fd_curl_test());
    } catch (...) {
        curl_global_cleanup();
        throw;
    }

    curl_global_cleanup();
    return 0;
}
