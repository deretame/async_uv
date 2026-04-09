#include <cassert>
#include <string>
#include <vector>

#include "flux/flux.h"
#include "flux_http/server.h"

namespace {

flux::Task<void> run_request_target_checks() {
    const auto target = flux::http::parse_request_target("/api/v1/users?id=42&name=alice#top");
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

flux::Task<void> run_response_serialize_checks() {
    flux::http::ServerResponse response;
    response.status_code = 200;
    response.body = "hello";

    const std::string wire = flux::http::serialize_response(response);
    assert(wire.find("HTTP/1.1 200 OK\r\n") == 0);
    assert(wire.find("Content-Length: 5\r\n") != std::string::npos);
    assert(wire.find("\r\n\r\nhello") != std::string::npos);

    flux::http::ServerResponse chunked;
    chunked.status_code = 200;
    chunked.chunked = true;
    chunked.body = "abc";
    const std::string chunked_wire = flux::http::serialize_response(chunked);
    assert(chunked_wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    assert(chunked_wire.find("3\r\nabc\r\n0\r\n\r\n") != std::string::npos);

    co_return;
}

flux::Task<void> run_validate_checks() {
    flux::http::ServerRequest request;
    request.method = "POST";
    request.raw_target = "/upload";
    request.target = flux::http::parse_request_target(request.raw_target);
    request.headers.push_back({"Host", "example.local"});
    request.body = "payload";
    request.body_size = request.body.size();

    flux::http::ServerLimits limits;
    limits.max_header_count = 8;
    limits.max_header_bytes = 256;
    limits.max_body_bytes = 16;

    const auto ok = flux::http::validate_request(request, limits);
    assert(ok.ok);

    request.body_size = 1024;
    const auto too_large = flux::http::validate_request(request, limits);
    assert(!too_large.ok);
    assert(too_large.status_code == 413);
    assert(too_large.close_connection);
    co_return;
}

flux::Task<void> run_keep_alive_checks() {
    flux::http::ServerRequest request;
    request.http_major = 1;
    request.http_minor = 1;
    request.headers.push_back({"Connection", "keep-alive"});

    flux::http::ServerResponse response;
    response.keep_alive = true;

    flux::http::ServerConnectionPolicy policy;
    policy.keep_alive_enabled = true;
    policy.max_keep_alive_requests = 2;

    flux::http::ServerConnectionState state;
    state.handled_requests = 0;
    assert(flux::http::should_keep_alive(request, response, policy, state));

    state.handled_requests = 1;
    assert(!flux::http::should_keep_alive(request, response, policy, state));
    co_return;
}

flux::Task<void> run_middleware_chain_checks() {
    flux::http::MiddlewareChain chain;
    chain.use(
        [](flux::http::ServerRequest req,
           flux::http::ServerHandler next) -> flux::Task<flux::http::ServerResponse> {
            req.headers.push_back({"X-Middleware", "A"});
            auto response = co_await next(std::move(req));
            response.headers.push_back({"X-Middleware-After", "A"});
            co_return response;
        });
    chain.endpoint(
        [](flux::http::ServerRequest req) -> flux::Task<flux::http::ServerResponse> {
            flux::http::ServerResponse response;
            response.status_code = 200;
            response.body = req.header("X-Middleware").value_or("none");
            co_return response;
        });

    flux::http::ServerRequest request;
    request.method = "GET";
    request.raw_target = "/";
    request.target = flux::http::parse_request_target("/");

    auto response = co_await chain.run(std::move(request));
    assert(response.status_code == 200);
    assert(response.body == "A");
    assert(!response.headers.empty());
    co_return;
}

flux::Task<void> run_socket_stream_checks() {
    auto listener = co_await flux::TcpListener::bind("127.0.0.1", 0);
    const int port = listener.port();
    auto client = co_await flux::TcpClient::connect("127.0.0.1", port);
    auto server_side = co_await listener.accept_for(std::chrono::seconds(2));

    flux::http::SocketBodyWriter writer(std::move(client), std::chrono::seconds(2));
    flux::http::SocketBodyReader reader(std::move(server_side), std::chrono::seconds(2), 16);

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

flux::Task<void> run_stream_checks() {
    flux::http::InMemoryBodyReader reader({"ab", "cd"});
    flux::http::InMemoryBodyWriter writer;

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

flux::Task<void> run_all_checks() {
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
    flux::Runtime runtime(flux::Runtime::build().name("flux_http_server_test"));
    runtime.block_on(run_all_checks());
    return 0;
}
