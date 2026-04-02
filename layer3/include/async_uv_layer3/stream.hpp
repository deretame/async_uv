#pragma once

#include <chrono>
#include <functional>
#include <string_view>

#include <async_uv/task.h>
#include <async_uv/tcp.h>
#include <async_uv_http/server.h>

namespace async_uv::layer3 {

class StreamWriter {
public:
    virtual ~StreamWriter() = default;
    virtual Task<void> write(std::string_view chunk) = 0;
    virtual Task<void> close() = 0;
};

using StreamHandler = std::function<Task<void>(StreamWriter&)>;

namespace detail {

class SocketStreamWriter final : public StreamWriter {
public:
    explicit SocketStreamWriter(TcpClient& socket, std::chrono::milliseconds timeout)
        : socket_(&socket), write_timeout_(timeout) {}
    
    Task<void> write(std::string_view chunk) override {
        if (closed_ || chunk.empty() || socket_ == nullptr || !socket_->valid()) {
            co_return;
        }

        std::string chunked = http::serialize_chunk(chunk);
        co_await socket_->write_all_for(chunked, write_timeout_);
    }

    Task<void> close() override {
        if (closed_ || socket_ == nullptr || !socket_->valid()) {
            closed_ = true;
            co_return;
        }
        closed_ = true;
        std::string end = http::serialize_chunk_end();
        co_await socket_->write_all_for(end, write_timeout_);
    }

private:
    TcpClient* socket_;
    std::chrono::milliseconds write_timeout_;
    bool closed_ = false;
};

}

}
