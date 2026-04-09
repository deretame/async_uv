#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "async_uv/cancel.h"
#include "async_uv/runtime.h"
#include "async_uv/stream.h"
#include "async_uv/task.h"

namespace async_uv::ws {

enum class MessageType {
    text,
    binary,
};

struct Message {
    MessageType type = MessageType::text;
    std::string payload;
    bool is_first_fragment = true;
    bool is_final_fragment = true;

    Message() = default;
    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;
    Message(Message &&) noexcept = default;
    Message &operator=(Message &&) noexcept = default;
};

struct StreamChunk {
    MessageType type = MessageType::text;
    std::string payload;
    bool is_first_fragment = true;
    bool is_final_fragment = true;

    StreamChunk() = default;
    StreamChunk(const StreamChunk &) = delete;
    StreamChunk &operator=(const StreamChunk &) = delete;
    StreamChunk(StreamChunk &&) noexcept = default;
    StreamChunk &operator=(StreamChunk &&) noexcept = default;
};

struct ServerEvent {
    enum class Kind {
        open,
        message,
        close,
    };

    Kind kind = Kind::message;
    std::uint64_t connection_id = 0;
    std::optional<Message> message;
    int close_code = 0;
    std::string close_reason;

    ServerEvent() = default;
    ServerEvent(const ServerEvent &) = delete;
    ServerEvent &operator=(const ServerEvent &) = delete;
    ServerEvent(ServerEvent &&) noexcept = default;
    ServerEvent &operator=(ServerEvent &&) noexcept = default;
};

class WebSocketError : public std::runtime_error {
public:
    explicit WebSocketError(std::string message) : std::runtime_error(std::move(message)) {}
};

class Server {
public:
    using next_type = std::optional<ServerEvent>;
    using task_type = Task<next_type>;
    using stream_type = Stream<ServerEvent>;

    struct Config {
        Runtime *runtime = nullptr;
        std::string host = "127.0.0.1";
        int port = 0;
        std::string pattern;
        int listen_options = 0;
        unsigned int max_payload_length = 16 * 1024;
        bool use_tls = false;
        std::string tls_cert_file;
        std::string tls_private_key_file;
        std::string tls_ca_file;
    };

    static Server create(Config config);

    Server();
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&other) noexcept;
    Server &operator=(Server &&other) noexcept;

    Task<void> start();
    Task<void> stop();

    int port() const noexcept;
    bool started() const noexcept;

    task_type next();

    template <typename Rep, typename Period>
    task_type next_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, next());
    }

    template <typename Clock, typename Duration>
    task_type next_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, next());
    }

    stream_type events();

    Task<bool> send_text(std::uint64_t connection_id, std::string_view payload);
    Task<bool> send_binary(std::uint64_t connection_id, std::string_view payload);
    Task<bool> send_stream(std::uint64_t connection_id, StreamChunk chunk);
    Task<bool> close(std::uint64_t connection_id, int code = 1000, std::string reason = {});

private:
    struct State;

    explicit Server(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

class Client {
public:
    struct Config {
        Runtime *runtime = nullptr;
        std::string address = "127.0.0.1";
        int port = 0;
        std::string path = "/";
        std::string host;
        std::string origin;
        bool use_tls = false;
        std::string tls_ca_file;
        bool tls_allow_insecure = false;
    };

    struct Event {
        enum class Kind {
            open,
            message,
            close,
            error,
        };

        Kind kind = Kind::message;
        std::optional<Message> message;
        int close_code = 0;
        std::string close_reason;
        std::string error;

        Event() = default;
        Event(const Event &) = delete;
        Event &operator=(const Event &) = delete;
        Event(Event &&) noexcept = default;
        Event &operator=(Event &&) noexcept = default;
    };

    using next_type = std::optional<Event>;
    using task_type = Task<next_type>;
    using stream_type = Stream<Event>;

    static Client create(Config config);

    Client();
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&other) noexcept;
    Client &operator=(Client &&other) noexcept;

    Task<void> connect();
    Task<void> close(int code = 1000, std::string reason = {});

    bool connected() const noexcept;

    task_type next();

    template <typename Rep, typename Period>
    task_type next_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await async_uv::with_timeout(timeout, next());
    }

    template <typename Clock, typename Duration>
    task_type next_until(std::chrono::time_point<Clock, Duration> deadline) {
        co_return co_await async_uv::with_deadline(deadline, next());
    }

    stream_type events();

    Task<bool> send_text(std::string_view payload);
    Task<bool> send_binary(std::string_view payload);
    Task<bool> send_stream(StreamChunk chunk);

private:
    struct State;

    explicit Client(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace async_uv::ws
