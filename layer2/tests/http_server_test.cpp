#include <cassert>
#include <string>
#include <vector>

#include "async_uv/async_uv.h"
#include "async_uv_http/server.h"

namespace {

async_uv::Task<void> run_request_target_checks() {
    const auto target = async_uv::http::parse_request_target("/api/v1/users?id=42&name=alice#top");
    assert(target.path == "/api/v1/users");
    assert(target.query_items.size() == 2);
    assert(target.query_items[0].first == "id");
    assert(target.query_items[0].second == "42");
    assert(target.query_items[1].first == "name");
    assert(target.query_items[1].second == "alice");
    assert(target.fragment.has_value());
    assert(*target.fragment == "top");
    co_return;
}

async_uv::Task<void> run_response_serialize_checks() {
    async_uv::http::ServerResponse response;
    response.status_code = 200;
    response.body = "hello";

    const std::string wire = async_uv::http::serialize_response(response);
    assert(wire.find("HTTP/1.1 200 OK\r\n") == 0);
    assert(wire.find("Content-Length: 5\r\n") != std::string::npos);
    assert(wire.find("\r\n\r\nhello") != std::string::npos);

    async_uv::http::ServerResponse chunked;
    chunked.status_code = 200;
    chunked.chunked = true;
    chunked.body = "abc";
    const std::string chunked_wire = async_uv::http::serialize_response(chunked);
    assert(chunked_wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    assert(chunked_wire.find("3\r\nabc\r\n0\r\n\r\n") != std::string::npos);

    co_return;
}

async_uv::Task<void> run_validate_checks() {
    async_uv::http::ServerRequest request;
    request.method = "POST";
    request.raw_target = "/upload";
    request.target = async_uv::http::parse_request_target(request.raw_target);
    request.headers.push_back({"Host", "example.local"});
    request.body = "payload";
    request.body_size = request.body.size();

    async_uv::http::ServerLimits limits;
    limits.max_header_count = 8;
    limits.max_header_bytes = 256;
    limits.max_body_bytes = 16;

    const auto ok = async_uv::http::validate_request(request, limits);
    assert(ok.ok);

    request.body_size = 1024;
    const auto too_large = async_uv::http::validate_request(request, limits);
    assert(!too_large.ok);
    assert(too_large.status_code == 413);
    assert(too_large.close_connection);
    co_return;
}

async_uv::Task<void> run_keep_alive_checks() {
    async_uv::http::ServerRequest request;
    request.http_major = 1;
    request.http_minor = 1;
    request.headers.push_back({"Connection", "keep-alive"});

    async_uv::http::ServerResponse response;
    response.keep_alive = true;

    async_uv::http::ServerConnectionPolicy policy;
    policy.keep_alive_enabled = true;
    policy.max_keep_alive_requests = 2;

    async_uv::http::ServerConnectionState state;
    state.handled_requests = 0;
    assert(async_uv::http::should_keep_alive(request, response, policy, state));

    state.handled_requests = 1;
    assert(!async_uv::http::should_keep_alive(request, response, policy, state));
    co_return;
}

async_uv::Task<void> run_middleware_chain_checks() {
    async_uv::http::MiddlewareChain chain;
    chain.use(
        [](async_uv::http::ServerRequest req,
           async_uv::http::ServerHandler next) -> async_uv::Task<async_uv::http::ServerResponse> {
            req.headers.push_back({"X-Middleware", "A"});
            auto response = co_await next(std::move(req));
            response.headers.push_back({"X-Middleware-After", "A"});
            co_return response;
        });
    chain.endpoint(
        [](async_uv::http::ServerRequest req) -> async_uv::Task<async_uv::http::ServerResponse> {
            async_uv::http::ServerResponse response;
            response.status_code = 200;
            response.body = req.header("X-Middleware").value_or("none");
            co_return response;
        });

    async_uv::http::ServerRequest request;
    request.method = "GET";
    request.raw_target = "/";
    request.target = async_uv::http::parse_request_target("/");

    auto response = co_await chain.run(std::move(request));
    assert(response.status_code == 200);
    assert(response.body == "A");
    assert(!response.headers.empty());
    co_return;
}

async_uv::Task<void> run_socket_stream_checks() {
    auto listener = co_await async_uv::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    auto client = co_await async_uv::TcpClient::connect("127.0.0.1", port);
    auto server_side = co_await listener.accept_for(std::chrono::seconds(2));

    async_uv::http::SocketBodyWriter writer(std::move(client), std::chrono::seconds(2));
    async_uv::http::SocketBodyReader reader(std::move(server_side), std::chrono::seconds(2), 16);

    co_await writer.write_chunk("hello-socket");
    auto chunk = co_await reader.next_chunk();
    assert(chunk.has_value());
    assert(*chunk == "hello-socket");

    co_await writer.close();
    auto end = co_await reader.next_chunk();
    assert(!end.has_value());
    co_await listener.close();
    co_return;
}

async_uv::Task<void> run_stream_checks() {
    async_uv::http::InMemoryBodyReader reader({"ab", "cd"});
    async_uv::http::InMemoryBodyWriter writer;

    while (true) {
        auto chunk = co_await reader.next_chunk();
        if (!chunk.has_value()) {
            break;
        }
        co_await writer.write_chunk(*chunk);
    }
    co_await writer.close();

    assert(writer.closed());
    assert(writer.buffer() == "abcd");
    co_return;
}

async_uv::Task<void> run_all_checks() {
    co_await run_request_target_checks();
    co_await run_response_serialize_checks();
    co_await run_validate_checks();
    co_await run_keep_alive_checks();
    co_await run_middleware_chain_checks();
    co_await run_stream_checks();
    co_await run_socket_stream_checks();
}

} // namespace

int main() {
    async_uv::Runtime runtime(async_uv::Runtime::build().name("async_uv_http_server_test"));
    runtime.block_on(run_all_checks());
    return 0;
}
