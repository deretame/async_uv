#include "flux_http/server.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <boost/url/encoding_opts.hpp>
#include <boost/url/params_view.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/pct_string_view.hpp>

#include "flux/runtime.h"

namespace flux::http {
namespace {

std::string to_lower(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string status_reason(int status_code) {
    switch (status_code) {
        case 100:
            return "Continue";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 414:
            return "URI Too Long";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            return "Unknown";
    }
}

std::string decode_component(std::string_view text, bool plus_as_space) {
    boost::urls::encoding_opts opts;
    opts.space_as_plus = plus_as_space;
    const auto decoded_input = boost::urls::make_pct_string_view(text);
    if (!decoded_input) {
        std::string fallback(text);
        if (plus_as_space) {
            std::replace(fallback.begin(), fallback.end(), '+', ' ');
        }
        return fallback;
    }

    const auto decoded = decoded_input->decode(opts);
    return std::string(decoded.begin(), decoded.end());
}

bool has_header_ci(const std::vector<Header> &headers, std::string_view name) {
    const auto target = to_lower(name);
    for (const auto &header : headers) {
        if (to_lower(header.name) == target) {
            return true;
        }
    }
    return false;
}

} // namespace

bool RequestContextMeta::expired() const noexcept {
    return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

std::optional<std::string> ServerRequest::header(std::string_view name) const {
    const auto target = to_lower(name);
    for (const auto &h : headers) {
        if (to_lower(h.name) == target) {
            return h.value;
        }
    }
    return std::nullopt;
}

ServerRequest ServerRequest::from_message(const HttpMessage &message, RequestContextMeta meta) {
    ServerRequest out;
    out.method = message.method();
    out.raw_target = message.url();
    out.target = parse_request_target(message.url());
    out.headers = message.headers();
    out.body = message.body();
    out.body_size = message.body_size();
    out.http_major = message.http_major();
    out.http_minor = message.http_minor();
    out.meta = std::move(meta);
    out.body_file_path = message.body_file_path();
    out.body_file_state = message.body_file_state_shared();
    return out;
}

InMemoryBodyReader::InMemoryBodyReader(std::vector<std::string> chunks)
    : chunks_(std::move(chunks)) {}

Task<std::optional<std::string>> InMemoryBodyReader::next_chunk() {
    if (index_ >= chunks_.size()) {
        co_return std::nullopt;
    }
    co_return chunks_[index_++];
}

Task<void> InMemoryBodyWriter::write_chunk(std::string_view chunk) {
    if (closed_) {
        co_return;
    }
    buffer_.append(chunk.data(), chunk.size());
    co_return;
}

Task<void> InMemoryBodyWriter::close() {
    closed_ = true;
    co_return;
}

const std::string &InMemoryBodyWriter::buffer() const noexcept {
    return buffer_;
}

bool InMemoryBodyWriter::closed() const noexcept {
    return closed_;
}

RequestTarget parse_request_target(std::string_view target) {
    RequestTarget out;
    if (target.empty()) {
        out.path = "/";
        return out;
    }

    if (const auto parsed = boost::urls::parse_uri_reference(target); parsed) {
        const auto &url = *parsed;
        out.path = url.encoded_path().empty() ? "/" : std::string(url.encoded_path());
        if (url.has_fragment()) {
            out.fragment = std::string(url.fragment());
        }

        boost::urls::encoding_opts opts;
        opts.space_as_plus = true;
        for (const auto &param : boost::urls::params_view(url.encoded_query(), opts)) {
            out.query_items.emplace_back(std::string(param.key),
                                         param.has_value ? std::string(param.value) : "");
        }
        return out;
    }

    // Fallback for invalid but tolerated request-target strings.
    std::string_view rest = target;
    const auto hash_pos = rest.find('#');
    if (hash_pos != std::string_view::npos) {
        out.fragment = decode_component(rest.substr(hash_pos + 1), false);
        rest = rest.substr(0, hash_pos);
    }

    const auto query_pos = rest.find('?');
    if (query_pos == std::string_view::npos) {
        out.path = rest.empty() ? "/" : std::string(rest);
        return out;
    }

    out.path = query_pos == 0 ? "/" : std::string(rest.substr(0, query_pos));
    const auto query = rest.substr(query_pos + 1);

    std::size_t begin = 0;
    while (begin <= query.size()) {
        const auto amp = query.find('&', begin);
        const auto item = query.substr(
            begin, amp == std::string_view::npos ? std::string_view::npos : amp - begin);
        if (!item.empty()) {
            const auto eq = item.find('=');
            if (eq == std::string_view::npos) {
                out.query_items.emplace_back(decode_component(item, true), "");
            } else {
                out.query_items.emplace_back(decode_component(item.substr(0, eq), true),
                                             decode_component(item.substr(eq + 1), true));
            }
        }

        if (amp == std::string_view::npos) {
            break;
        }
        begin = amp + 1;
    }

    return out;
}

ValidationResult validate_request(const ServerRequest &request, const ServerLimits &limits) {
    ValidationResult result;
    if (request.meta.cancelled || request.meta.expired()) {
        result.ok = false;
        result.status_code = 408;
        result.close_connection = true;
        result.message = "request timeout or cancelled";
        return result;
    }

    if (request.method.empty() || request.method.size() > limits.max_method_bytes) {
        result.ok = false;
        result.status_code = 400;
        result.close_connection = true;
        result.message = "invalid request method";
        return result;
    }

    if (request.raw_target.size() > limits.max_target_bytes) {
        result.ok = false;
        result.status_code = 414;
        result.close_connection = true;
        result.message = "request target too long";
        return result;
    }

    if (request.headers.size() > limits.max_header_count) {
        result.ok = false;
        result.status_code = 431;
        result.close_connection = true;
        result.message = "too many headers";
        return result;
    }

    std::size_t header_bytes = 0;
    for (const auto &header : request.headers) {
        header_bytes += header.name.size();
        header_bytes += header.value.size();
        header_bytes += 4;
        if (header_bytes > limits.max_header_bytes) {
            result.ok = false;
            result.status_code = 431;
            result.close_connection = true;
            result.message = "headers too large";
            return result;
        }
    }

    if (request.body_size > limits.max_body_bytes) {
        result.ok = false;
        result.status_code = 413;
        result.close_connection = true;
        result.message = "payload too large";
        return result;
    }

    return result;
}

std::string serialize_response(const ServerResponse &response) {
    const std::string reason =
        response.reason.empty() ? status_reason(response.status_code) : response.reason;

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status_code << ' ' << reason << "\r\n";

    const bool should_chunk = response.chunked;
    const bool has_length = has_header_ci(response.headers, "Content-Length");
    const bool has_transfer_encoding = has_header_ci(response.headers, "Transfer-Encoding");
    const bool has_connection = has_header_ci(response.headers, "Connection");
    const bool has_content_type = has_header_ci(response.headers, "Content-Type");

    for (const auto &header : response.headers) {
        out << header.name << ": " << header.value << "\r\n";
    }

    if (!has_connection) {
        out << "Connection: " << (response.keep_alive ? "keep-alive" : "close") << "\r\n";
    }
    if (should_chunk && !has_transfer_encoding) {
        out << "Transfer-Encoding: chunked\r\n";
    }
    if (!should_chunk && !has_length) {
        out << "Content-Length: " << response.body.size() << "\r\n";
    }
    if (!has_content_type) {
        out << "Content-Type: text/plain; charset=utf-8\r\n";
    }

    out << "\r\n";
    if (should_chunk) {
        if (!response.body.empty()) {
            out << serialize_chunk(response.body);
        }
        out << serialize_chunk_end();
    } else {
        out << response.body;
    }

    return out.str();
}

std::string serialize_chunk(std::string_view chunk) {
    std::ostringstream out;
    out << std::hex << chunk.size() << "\r\n";
    out << chunk << "\r\n";
    return out.str();
}

std::string serialize_chunk_end() {
    return "0\r\n\r\n";
}

TlsSocketAdapter::TlsSocketAdapter(TcpClient socket,
                                   std::unique_ptr<TlsBio> tls,
                                   std::chrono::milliseconds io_timeout)
    : socket_(std::move(socket)), tls_(std::move(tls)), io_timeout_(io_timeout) {}

Task<void> TlsSocketAdapter::flush_encrypted_outbound() {
    co_return;
}

Task<void> TlsSocketAdapter::handshake() {
    co_await flush_encrypted_outbound();
    co_return;
}

Task<std::string> TlsSocketAdapter::read_some(std::size_t max_bytes) {
    if (!socket_.valid()) {
        co_return std::string{};
    }
    co_return co_await socket_.read_some_for(io_timeout_, max_bytes);
}

Task<std::size_t> TlsSocketAdapter::write_all(std::string_view plain) {
    if (!socket_.valid()) {
        co_return 0;
    }
    co_return co_await socket_.write_all_for(plain, io_timeout_);
}

Task<void> TlsSocketAdapter::close() {
    if (socket_.valid()) {
        co_await socket_.close();
    }
    co_return;
}

SocketBodyReader::SocketBodyReader(TcpClient socket,
                                   std::chrono::milliseconds read_timeout,
                                   std::size_t chunk_size)
    : socket_(std::move(socket)), read_timeout_(read_timeout), chunk_size_(chunk_size) {}

Task<std::optional<std::string>> SocketBodyReader::next_chunk() {
    if (!socket_.valid()) {
        co_return std::nullopt;
    }

    const std::string chunk = co_await socket_.read_some_for(read_timeout_, chunk_size_);
    if (chunk.empty()) {
        co_return std::nullopt;
    }
    co_return chunk;
}

SocketBodyWriter::SocketBodyWriter(TcpClient socket, std::chrono::milliseconds write_timeout)
    : socket_(std::move(socket)), write_timeout_(write_timeout) {}

Task<void> SocketBodyWriter::write_chunk(std::string_view chunk) {
    if (closed_ || !socket_.valid()) {
        co_return;
    }
    if (chunk.empty()) {
        co_return;
    }
    (void)co_await socket_.write_all_for(chunk, write_timeout_);
    co_return;
}

Task<void> SocketBodyWriter::close() {
    if (closed_) {
        co_return;
    }
    closed_ = true;
    if (socket_.valid()) {
        co_await socket_.close();
    }
    co_return;
}

bool should_keep_alive(const ServerRequest &request,
                       const ServerResponse &response,
                       const ServerConnectionPolicy &policy,
                       const ServerConnectionState &state) {
    if (!policy.keep_alive_enabled || !response.keep_alive) {
        return false;
    }
    if (state.handled_requests + 1 >= policy.max_keep_alive_requests) {
        return false;
    }

    const auto conn = request.header("Connection");
    const std::string conn_lower = conn.has_value() ? to_lower(*conn) : "";
    if (request.http_major > 1 || (request.http_major == 1 && request.http_minor == 1)) {
        return conn_lower != "close";
    }
    return conn_lower == "keep-alive";
}

Task<std::optional<ServerRequest>> read_request_from_socket(TcpClient &socket,
                                                            HttpParser &parser,
                                                            const ServerLimits &limits,
                                                            const ServerConnectionPolicy &policy,
                                                            RequestContextMeta meta) {
    std::size_t consumed_bytes = 0;
    while (true) {
        auto message = co_await parser.next_message();
        if (message.has_value()) {
            auto request = ServerRequest::from_message(*message, std::move(meta));
            const auto check = validate_request(request, limits);
            if (!check.ok) {
                throw std::runtime_error(check.message.empty() ? "invalid http request"
                                                               : check.message);
            }
            flux::emit_trace_event(
                {"layer2_http_server", "request_ready", 0, request.body_size});
            co_return request;
        }

        const std::string chunk = co_await socket.read_some_for(policy.read_timeout, 64 * 1024);
        if (chunk.empty()) {
            co_return std::nullopt;
        }

        consumed_bytes += chunk.size();
        if (consumed_bytes > limits.max_body_bytes + limits.max_header_bytes) {
            throw std::runtime_error("http request exceeds configured read limits");
        }

        co_await parser.feed(chunk);
    }
}

Task<void> write_response_to_socket(TcpClient &socket,
                                    const ServerResponse &response,
                                    const ServerConnectionPolicy &policy) {
    const std::string wire = serialize_response(response);
    flux::emit_trace_event(
        {"layer2_http_server", "response_write", response.status_code, wire.size()});
    (void)co_await socket.write_all_for(wire, policy.write_timeout);
    co_return;
}

MiddlewareChain &MiddlewareChain::use(ServerMiddleware middleware) {
    middlewares_.push_back(std::move(middleware));
    return *this;
}

MiddlewareChain &MiddlewareChain::endpoint(ServerHandler handler) {
    endpoint_ = std::move(handler);
    return *this;
}

Task<ServerResponse> MiddlewareChain::run(ServerRequest request) const {
    if (!endpoint_) {
        throw std::runtime_error("middleware chain endpoint is not set");
    }

    ServerHandler handler = endpoint_;
    for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
        const auto middleware = *it;
        const auto next = handler;
        handler = [middleware, next](ServerRequest req) -> Task<ServerResponse> {
            co_return co_await middleware(std::move(req), next);
        };
    }

    co_return co_await handler(std::move(request));
}

} // namespace flux::http
