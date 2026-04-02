#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <async_uv/task.h>
#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/router_node.hpp>

// Mock handler - 在全局定义
async_uv::Task<void> mock_handler(async_uv::layer3::Context&) {
    co_return;
}

using namespace async_uv::layer3;

// 测试辅助函数
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { std::cout << "Running " #name "... "; test_##name(); std::cout << "OK\n"; } while(0)

// 测试静态路由
TEST(router_static) {
    Router router;
    router.get("/users", mock_handler);
    router.get("/posts", mock_handler);
    router.post("/users", mock_handler);
    
    // 匹配 GET /users
    auto result = router.match("GET", "/users");
    assert(result.has_value());
    assert(result->params.empty());
    
    // 匹配 GET /posts
    result = router.match("GET", "/posts");
    assert(result.has_value());
    
    // 不匹配 POST /users (方法不同)
    result = router.match("POST", "/users");
    assert(result.has_value());  // POST /users 已注册
    
    // 不匹配 GET /unknown
    result = router.match("GET", "/unknown");
    assert(!result.has_value());
    
    // 不匹配 GET /users/123 (缺少参数)
    result = router.match("GET", "/users/123");
    assert(!result.has_value());
}

// 测试参数路由
TEST(router_param) {
    Router router;
    router.get("/users/{id}", mock_handler);
    router.get("/posts/{post_id}/comments/{comment_id}", mock_handler);
    
    // 匹配 /users/123
    auto result = router.match("GET", "/users/123");
    assert(result.has_value());
    assert(result->params.count("id") == 1);
    assert(result->params.at("id") == "123");
    
    // 匹配 /users/abc (参数可以是任意字符串)
    result = router.match("GET", "/users/abc");
    assert(result.has_value());
    assert(result->params.at("id") == "abc");
    
    // 匹配多参数路由
    result = router.match("GET", "/posts/42/comments/7");
    assert(result.has_value());
    assert(result->params.at("post_id") == "42");
    assert(result->params.at("comment_id") == "7");
}

// 测试通配符路由
TEST(router_wildcard) {
    Router router;
    router.get("/files/{path*}", mock_handler);
    router.get("/static/{file*}", mock_handler);
    
    // 匹配单层
    auto result = router.match("GET", "/files/test.txt");
    assert(result.has_value());
    assert(result->params.at("path") == "test.txt");
    
    // 匹配多层
    result = router.match("GET", "/files/a/b/c.txt");
    assert(result.has_value());
    assert(result->params.at("path") == "a/b/c.txt");
    
    // 匹配深层路径
    result = router.match("GET", "/files/deep/nested/path/file.jpg");
    assert(result.has_value());
    assert(result->params.at("path") == "deep/nested/path/file.jpg");
}

// 测试混合路由
TEST(router_mixed) {
    Router router;
    router.get("/users", mock_handler);                    // 静态
    router.get("/users/{id}", mock_handler);                // 参数
    router.get("/users/{id}/posts", mock_handler);          // 参数 + 静态
    router.get("/users/{id}/posts/{post_id}", mock_handler); // 多参数
    
    // 静态优先
    auto result = router.match("GET", "/users");
    assert(result.has_value());
    assert(result->params.empty());
    
    // 参数匹配
    result = router.match("GET", "/users/123");
    assert(result.has_value());
    assert(result->params.at("id") == "123");
    
    // 参数 + 静态
    result = router.match("GET", "/users/123/posts");
    assert(result.has_value());
    assert(result->params.at("id") == "123");
    
    // 多参数
    result = router.match("GET", "/users/123/posts/456");
    assert(result.has_value());
    assert(result->params.at("id") == "123");
    assert(result->params.at("post_id") == "456");
}

TEST(router_priority_registration_order) {
    Router router;
    router.get("/users/{id}", mock_handler);
    router.get("/users/profile", mock_handler);
    router.get("/users/{path*}", mock_handler);

    auto result = router.match("GET", "/users/profile");
    assert(result.has_value());
    assert(result->params.empty());

    result = router.match("GET", "/users/123");
    assert(result.has_value());
    assert(result->params.at("id") == "123");
}

// 测试不同 HTTP 方法
TEST(router_methods) {
    Router router;
    router.get("/resource", mock_handler);
    router.post("/resource", mock_handler);
    router.put("/resource", mock_handler);
    router.del("/resource", mock_handler);
    router.patch("/resource", mock_handler);
    
    assert(router.match("GET", "/resource").has_value());
    assert(router.match("POST", "/resource").has_value());
    assert(router.match("PUT", "/resource").has_value());
    assert(router.match("DELETE", "/resource").has_value());
    assert(router.match("PATCH", "/resource").has_value());
    
    // 未注册的方法
    assert(!router.match("HEAD", "/resource").has_value());
    assert(!router.match("OPTIONS", "/resource").has_value());
}

TEST(router_all_method) {
    Router router;
    router.all("/health", mock_handler);

    assert(router.match("GET", "/health").has_value());
    assert(router.match("POST", "/health").has_value());
    assert(router.match("HEAD", "/health").has_value());
}

// 测试子路由
TEST(router_sub_router) {
    Router api_router;
    api_router.get("/users", mock_handler);
    api_router.get("/posts", mock_handler);
    
    Router router;
    router.route("/api", std::move(api_router));
    
    // 匹配子路由
    auto result = router.match("GET", "/api/users");
    assert(result.has_value());
    
    result = router.match("GET", "/api/posts");
    assert(result.has_value());
    
    // 不匹配根路径
    result = router.match("GET", "/users");
    assert(!result.has_value());

    // 不应匹配 /api2 前缀
    result = router.match("GET", "/api2/users");
    assert(!result.has_value());
}

// 测试 Radix Tree 节点类型
TEST(router_node_types) {
    RouterNode<Handler> root;
    
    // 添加静态节点
    auto static_child = root.add_child("users", RouterNode<Handler>::Type::STATIC);
    assert(static_child != nullptr);
    assert(static_child->type == RouterNode<Handler>::Type::STATIC);
    assert(static_child->prefix == "users");
    
    // 添加参数节点
    auto param_child = static_child->add_child("", RouterNode<Handler>::Type::PARAM, "id");
    assert(param_child != nullptr);
    assert(param_child->type == RouterNode<Handler>::Type::PARAM);
    assert(param_child->param_name == "id");
    
    // 添加通配符节点
    auto wild_child = root.add_child("", RouterNode<Handler>::Type::WILDCARD, "path");
    assert(wild_child != nullptr);
    assert(wild_child->type == RouterNode<Handler>::Type::WILDCARD);
}

TEST(router_path_normalization) {
    Router router;
    router.get("/users///{id}/", mock_handler);
    router.get("/", mock_handler);

    auto result = router.match("GET", "//users//123/?a=1#part");
    assert(result.has_value());
    assert(result->params.at("id") == "123");

    assert(router.match("GET", "").has_value());
    assert(router.match("GET", "?a=1").has_value());
    assert(router.match("GET", "#frag").has_value());
}

TEST(router_duplicate_registration_rejected) {
    Router router;
    router.get("/users/", mock_handler);

    bool duplicate_thrown = false;
    try {
        router.get("/users", mock_handler);
    } catch (const std::invalid_argument&) {
        duplicate_thrown = true;
    }
    assert(duplicate_thrown);

    Router wildcard_router;
    wildcard_router.all("/health", mock_handler);
    duplicate_thrown = false;
    try {
        wildcard_router.get("/health", mock_handler);
    } catch (const std::invalid_argument&) {
        duplicate_thrown = true;
    }
    assert(duplicate_thrown);
}

TEST(router_allowed_methods) {
    Router router;
    router.get("/resource", mock_handler);
    router.post("/resource", mock_handler);

    auto methods = router.allowed_methods("/resource/");
    assert(methods.size() == 3);
    assert(methods[0] == "GET");
    assert(methods[1] == "HEAD");
    assert(methods[2] == "POST");

    Router wildcard_router;
    wildcard_router.all("/health", mock_handler);
    auto wildcard_methods = wildcard_router.allowed_methods("/health");
    assert(!wildcard_methods.empty());
    assert(std::find(wildcard_methods.begin(), wildcard_methods.end(), "GET") != wildcard_methods.end());
    assert(std::find(wildcard_methods.begin(), wildcard_methods.end(), "HEAD") != wildcard_methods.end());
    assert(std::find(wildcard_methods.begin(), wildcard_methods.end(), "OPTIONS") != wildcard_methods.end());
}

TEST(router_route_list_and_meta) {
    Router router;
    Router::RouteMeta meta;
    meta.name = "get_user";
    meta.description = "Get user by id";
    meta.tags = {"users"};
    meta.require_auth = true;

    router.get("/users/{id}", mock_handler, meta);
    router.post("/users", mock_handler);

    auto routes = router.routes();
    assert(routes.size() == 2);

    bool found_get_user = false;
    for (const auto& route : routes) {
        if (route.method == "GET" && route.pattern == "/users/{id}") {
            found_get_user = true;
            assert(route.meta.name == "get_user");
            assert(route.meta.require_auth);
            assert(route.param_names.size() == 1);
            assert(route.param_names[0] == "id");
        }
    }
    assert(found_get_user);
}

TEST(router_route_list_with_subrouter) {
    Router api;
    api.get("/health", mock_handler);

    Router parent;
    parent.route("/api", std::move(api));

    auto routes = parent.routes();
    assert(routes.size() == 1);
    assert(routes[0].pattern == "/api/health");
    assert(routes[0].method == "GET");
}

int main() {
    std::cout << "=== Router Tests ===\n";
    
    RUN_TEST(router_static);
    RUN_TEST(router_param);
    RUN_TEST(router_wildcard);
    RUN_TEST(router_mixed);
    RUN_TEST(router_priority_registration_order);
    RUN_TEST(router_methods);
    RUN_TEST(router_all_method);
    RUN_TEST(router_sub_router);
    RUN_TEST(router_node_types);
    RUN_TEST(router_path_normalization);
    RUN_TEST(router_duplicate_registration_rejected);
    RUN_TEST(router_allowed_methods);
    RUN_TEST(router_route_list_and_meta);
    RUN_TEST(router_route_list_with_subrouter);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
