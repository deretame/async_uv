#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <functional>

#include <async_uv/task.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_uv_layer3/form_parser.hpp>
#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/types.hpp>
#include <async_uv_http/server.h>

using namespace async_uv::layer3;
using namespace async_uv::http;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { std::cout << "Running " #name "... "; test_##name(); std::cout << "OK\n"; } while(0)

Context make_context(const std::string& body, const std::string& content_type) {
    ServerRequest req;
    req.body = body;
    req.headers.push_back({"Content-Type", content_type});
    return Context(std::move(req));
}

TEST(form_parser_basic) {
    std::string body = "name=Alice&age=30&city=Beijing";
    auto ctx = make_context(body, "application/x-www-form-urlencoded");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto form_data = ctx.local<std::map<std::string, std::string>>("form_data");
    assert(form_data.has_value());
    assert(form_data->at("name") == "Alice");
    assert(form_data->at("age") == "30");
    assert(form_data->at("city") == "Beijing");
}

TEST(form_parser_encoded) {
    std::string body = "name=Hello%20World&email=test%40example.com";
    auto ctx = make_context(body, "application/x-www-form-urlencoded");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto form_data = ctx.local<std::map<std::string, std::string>>("form_data");
    assert(form_data.has_value());
    assert(form_data->at("name") == "Hello World");
    assert(form_data->at("email") == "test@example.com");
}

TEST(form_parser_plus_as_space) {
    std::string body = "query=hello+world&name=Alice+Bob";
    auto ctx = make_context(body, "application/x-www-form-urlencoded");

    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));

    auto form_data = ctx.local<std::map<std::string, std::string>>("form_data");
    assert(form_data.has_value());
    assert(form_data->at("query") == "hello world");
    assert(form_data->at("name") == "Alice Bob");
}

TEST(form_parser_empty_value) {
    std::string body = "key1=&key2=value";
    auto ctx = make_context(body, "application/x-www-form-urlencoded");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto form_data = ctx.local<std::map<std::string, std::string>>("form_data");
    assert(form_data.has_value());
    assert(form_data->at("key1") == "");
    assert(form_data->at("key2") == "value");
}

TEST(form_parser_wrong_content_type) {
    std::string body = "name=Alice";
    auto ctx = make_context(body, "application/json");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto form_data = ctx.local<std::map<std::string, std::string>>("form_data");
    assert(!form_data.has_value());
}

TEST(form_field_helper) {
    std::string body = "username=admin&password=secret";
    auto ctx = make_context(body, "application/x-www-form-urlencoded");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto username = form_field(ctx, "username");
    assert(username.has_value());
    assert(*username == "admin");
    
    auto password = form_field(ctx, "password");
    assert(password.has_value());
    assert(*password == "secret");
    
    auto missing = form_field(ctx, "nonexistent");
    assert(!missing.has_value());

    auto all = all_form_data(ctx);
    assert(all.has_value());
    assert(all->size() == 2);
}

int main() {
    std::cout << "=== Form Parser Tests ===\n";
    
    RUN_TEST(form_parser_basic);
    RUN_TEST(form_parser_encoded);
    RUN_TEST(form_parser_plus_as_space);
    RUN_TEST(form_parser_empty_value);
    RUN_TEST(form_parser_wrong_content_type);
    RUN_TEST(form_field_helper);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
