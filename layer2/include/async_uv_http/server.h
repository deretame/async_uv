#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async_uv/task.h"
#include "async_uv/tcp.h"
#include "async_uv/tls.h"
#include "async_uv_http/http.h"
#include "async_uv_http/parser.h"

namespace async_uv::http {

struct RequestTarget {
    std::string path;
    std::vector<std::pair<std::string, std::string>> query_items;
    std::optional<std::string> fragment;
};

struct RequestContextMeta {
    std::string request_id;
    std::string remote_address;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool cancelled = false;

    [[nodiscard]] bool expired() const noexcept;
};

struct ServerRequest {
    std::string method;
    std::string raw_target;
    RequestTarget target;
    std::vector<Header> headers;
    std::string body;
    std::uintmax_t body_size = 0;
    int http_major = 1;
    int http_minor = 1;
    RequestContextMeta meta;

    [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
    static ServerRequest from_message(const HttpMessage &message, RequestContextMeta meta = {});
};

struct ServerResponse {
    int status_code = 200;
    std::string reason;
    std::vector<Header> headers;
    std::string body;
    bool keep_alive = true;
    bool chunked = false;
};

struct ServerLimits {
    std::size_t max_header_count = 256;
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t max_target_bytes = 8 * 1024;
    std::size_t max_method_bytes = 16;
};

struct ServerConnectionPolicy {
    bool keep_alive_enabled = true;
    std::size_t max_keep_alive_requests = 100;
    std::chrono::milliseconds read_timeout = std::chrono::seconds(15);
    std::chrono::milliseconds write_timeout = std::chrono::seconds(15);
    std::chrono::milliseconds idle_timeout = std::chrono::seconds(30);
};

struct ServerConnectionState {
    std::size_t handled_requests = 0;
};

struct ValidationResult {
    bool ok = true;
    int status_code = 200;
    bool close_connection = false;
    std::string message;
};

class BodyReader {
public:
    virtual ~BodyReader() = default;
    virtual Task<std::optional<std::string>> next_chunk() = 0;
};

class BodyWriter {
public:
    virtual ~BodyWriter() = default;
    virtual Task<void> write_chunk(std::string_view chunk) = 0;
    virtual Task<void> close() = 0;
};

class InMemoryBodyReader final : public BodyReader {
public:
    explicit InMemoryBodyReader(std::vector<std::string> chunks);
    Task<std::optional<std::string>> next_chunk() override;

private:
    std::vector<std::string> chunks_;
    std::size_t index_ = 0;
};

class InMemoryBodyWriter final : public BodyWriter {
public:
    Task<void> write_chunk(std::string_view chunk) override;
    Task<void> close() override;

    [[nodiscard]] const std::string &buffer() const noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    std::string buffer_;
    bool closed_ = false;
};

class SocketBodyReader final : public BodyReader {
public:
    SocketBodyReader(TcpClient socket,
                     std::chrono::milliseconds read_timeout,
                     std::size_t chunk_size = 64 * 1024);

    Task<std::optional<std::string>> next_chunk() override;

private:
    TcpClient socket_;
    std::chrono::milliseconds read_timeout_;
    std::size_t chunk_size_ = 64 * 1024;
};

class SocketBodyWriter final : public BodyWriter {
public:
    SocketBodyWriter(TcpClient socket, std::chrono::milliseconds write_timeout);

    Task<void> write_chunk(std::string_view chunk) override;
    Task<void> close() override;

private:
    TcpClient socket_;
    std::chrono::milliseconds write_timeout_;
    bool closed_ = false;
};

class TlsSocketAdapter {
public:
    TlsSocketAdapter(TcpClient socket,
                     std::unique_ptr<TlsBio> tls,
                     std::chrono::milliseconds io_timeout);

    Task<void> handshake();
    Task<std::string> read_some(std::size_t max_bytes = 64 * 1024);
    Task<std::size_t> write_all(std::string_view plain);
    Task<void> close();

private:
    Task<void> flush_encrypted_outbound();

    TcpClient socket_;
    std::unique_ptr<TlsBio> tls_;
    std::chrono::milliseconds io_timeout_;
};

RequestTarget parse_request_target(std::string_view target);
ValidationResult validate_request(const ServerRequest &request, const ServerLimits &limits);

bool should_keep_alive(const ServerRequest &request,
                       const ServerResponse &response,
                       const ServerConnectionPolicy &policy,
                       const ServerConnectionState &state);

Task<std::optional<ServerRequest>> read_request_from_socket(TcpClient &socket,
                                                            HttpParser &parser,
                                                            const ServerLimits &limits,
                                                            const ServerConnectionPolicy &policy,
                                                            RequestContextMeta meta = {});
Task<void> write_response_to_socket(TcpClient &socket,
                                    const ServerResponse &response,
                                    const ServerConnectionPolicy &policy);

using ServerHandler = std::function<Task<ServerResponse>(ServerRequest)>;
using ServerMiddleware = std::function<Task<ServerResponse>(ServerRequest, ServerHandler)>;

class MiddlewareChain {
public:
    MiddlewareChain &use(ServerMiddleware middleware);
    MiddlewareChain &endpoint(ServerHandler handler);
    Task<ServerResponse> run(ServerRequest request) const;

private:
    std::vector<ServerMiddleware> middlewares_;
    ServerHandler endpoint_;
};

std::string serialize_response(const ServerResponse &response);
std::string serialize_chunk(std::string_view chunk);
std::string serialize_chunk_end();

} // namespace async_uv::http
