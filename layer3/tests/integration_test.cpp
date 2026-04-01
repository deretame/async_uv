#include <cassert>
#include <iostream>
#include <string>
#include <map>

#include <async_uv/task.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/types.hpp>
#include <async_uv_layer3/middleware.hpp>
#include <async_uv_layer3/form_parser.hpp>
#include <async_uv_layer3/cors.hpp>
#include <async_uv_layer3/error.hpp>
#include <async_uv_http/server.h>

using namespace async_uv::layer3;
using namespace async_uv::http;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { std::cout << "Running " #name "... "; test_##name(); std::cout << "OK\n"; } while(0)

Context make_context(const std::string& method, const std::string& path, 
                     const std::string& body = "", 
                     const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    ServerRequest req;
    req.method = method;
    req.raw_target = path;
    req.target = parse_request_target(path);
    req.body = body;
    for (const auto& [name, value] : headers) {
        req.headers.push_back({name, value});
    }
    return Context(std::move(req));
}

TEST(app_basic_routing) {
    App app;
    app.get("/hello", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        ctx.send("Hello World");
        co_return;
    });
    
    app.post("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(201);
        ctx.json_raw("{\"created\":true}");
        co_return;
    });
}

TEST(router_static_routing) {
    Router router;
    router.get("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    
    router.post("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(201);
        co_return;
    });
    
    auto result = router.match("GET", "/users");
    assert(result.has_value());
    assert(result->params.empty());
    
    result = router.match("POST", "/users");
    assert(result.has_value());
    
    result = router.match("DELETE", "/users");
    assert(!result.has_value());
}

TEST(router_with_params) {
    Router router;
    router.get("/users/{id}", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    
    auto result = router.match("GET", "/users/123");
    assert(result.has_value());
    assert(result->params["id"] == "123");
    
    result = router.match("GET", "/users/abc");
    assert(result.has_value());
    assert(result->params["id"] == "abc");
}

TEST(router_with_wildcard) {
    Router router;
    router.get("/files/{path*}", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    
    auto result = router.match("GET", "/files/a/b/c.txt");
    assert(result.has_value());
    assert(result->params["path"] == "a/b/c.txt");
    
    result = router.match("GET", "/files/single.txt");
    assert(result.has_value());
    assert(result->params["path"] == "single.txt");
}

TEST(context_json_operations) {
    auto ctx = make_context("POST", "/api", R"({"name":"test","value":42})", 
                           {{"Content-Type", "application/json"}});
    
    std::string json_body = ctx.body;
    assert(json_body.find("test") != std::string::npos);
    assert(json_body.find("42") != std::string::npos);
    
    ctx.json_raw(R"({"status":"ok"})");
    assert(ctx.response.status_code == 200);
}

TEST(context_redirect_methods) {
    auto ctx = make_context("GET", "/old");
    
    ctx.redirect("/new");
    assert(ctx.response.status_code == 302);
    
    ctx.redirect_permanent("/permanent");
    assert(ctx.response.status_code == 301);
}

TEST(context_attachment_headers) {
    auto ctx = make_context("GET", "/download");
    
    ctx.attachment("report.pdf");
    bool found = false;
    for (const auto& h : ctx.response.headers) {
        if (h.name == "Content-Disposition") {
            found = true;
            assert(h.value.find("attachment") != std::string::npos);
            assert(h.value.find("report.pdf") != std::string::npos);
        }
    }
    assert(found);
}

TEST(error_codes) {
    Error err = errors::not_found("User not found", "User ID: 123");
    assert(err.code == ErrorCode::NotFound);
    assert(err.message == "User not found");
    assert(err.status_code() == 404);
    assert(err.status_text() == "Not Found");
    
    std::string json = err.to_json();
    assert(json.find("\"code\":404") != std::string::npos);
    assert(json.find("\"message\":\"User not found\"") != std::string::npos);
}

TEST(error_exceptions) {
    bool caught = false;
    try {
        throw FrameworkError(ErrorCode::BadRequest, "Invalid input");
    } catch (const FrameworkError& ex) {
        caught = true;
        assert(ex.error.code == ErrorCode::BadRequest);
        assert(ex.error.status_code() == 400);
    }
    assert(caught);
}

TEST(middleware_error_handler) {
    auto ctx = make_context("GET", "/test");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::error_handler(ctx, next));
    assert(ctx.response.status_code == 200);
}

TEST(form_parser_integration) {
    std::string body = "username=admin&password=secret123";
    auto ctx = make_context("POST", "/login", body, 
                           {{"Content-Type", "application/x-www-form-urlencoded"}});
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto username = form_field(ctx, "username");
    assert(username.has_value());
    assert(*username == "admin");
    
    auto password = form_field(ctx, "password");
    assert(password.has_value());
    assert(*password == "secret123");
}

TEST(cors_basic) {
    middleware::CorsOptions options;
    options.allow_origins = {"*"};
    options.allow_methods = {"GET", "POST", "PUT", "DELETE"};
    options.allow_headers = {"Content-Type", "Authorization"};
    
    auto ctx = make_context("GET", "/api", "", {{"Origin", "http://example.com"}});
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    auto cors_mw = middleware::cors(options);
    async_simple::coro::syncAwait(cors_mw(ctx, next));
    
    bool found_aco = false;
    for (const auto& h : ctx.response.headers) {
        if (h.name == "Access-Control-Allow-Origin") {
            found_aco = true;
            assert(h.value == "http://example.com");
        }
    }
    assert(found_aco);
}

TEST(cors_preflight) {
    middleware::CorsOptions options;
    options.allow_origins = {"http://localhost:3000"};
    options.allow_methods = {"GET", "POST"};
    
    auto ctx = make_context("OPTIONS", "/api", "", 
                           {{"Origin", "http://localhost:3000"},
                            {"Access-Control-Request-Method", "POST"}});
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    auto cors_mw = middleware::cors(options);
    async_simple::coro::syncAwait(cors_mw(ctx, next));
    
    assert(ctx.response.status_code == 204);
}

int main() {
    std::cout << "=== Layer 3 Integration Tests ===\n";
    
    RUN_TEST(app_basic_routing);
    RUN_TEST(router_static_routing);
    RUN_TEST(router_with_params);
    RUN_TEST(router_with_wildcard);
    RUN_TEST(context_json_operations);
    RUN_TEST(context_redirect_methods);
    RUN_TEST(context_attachment_headers);
    RUN_TEST(error_codes);
    RUN_TEST(error_exceptions);
    RUN_TEST(middleware_error_handler);
    RUN_TEST(form_parser_integration);
    RUN_TEST(cors_basic);
    RUN_TEST(cors_preflight);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}