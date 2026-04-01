# Layer 3 设计文档

## 1. 概述

Layer 3 是基于 Layer 2 的 HTTP 服务端框架，提供：

- **Radix Tree 路由器** - 高效的 URL 路径匹配，支持参数提取
- **洋葱模型中间件** - 灵活的前置/后置处理链
- **Context 增强** - 请求上下文、JSON 序列化等便捷方法

### 1.1 设计目标

| 目标 | 说明 |
|------|------|
| 零拷贝 | 尽量避免不必要的字符串拷贝 |
| 类型安全 | 使用模板进行 JSON 序列化/反序列化 |
| 协程原生 | 所有异步操作使用 C++20 协程 |
| 易于扩展 | 中间件机制支持后置逻辑 |

### 1.2 依赖关系

```
Layer 3 (async_uv_layer3)
├── Layer 2 (async_uv_http) - HTTP 服务端基础
│   ├── ServerRequest/ServerResponse
│   ├── HttpParser
│   └── read_request_from_socket / write_response_to_socket
├── Layer 1 (async_uv) - 异步原语
│   ├── Task<T>
│   └── TcpListener / TcpClient
└── reflect-cpp v0.24.0 - JSON 序列化
    └── rfl::json::read<T> / rfl::json::write<T>
```

---

## 2. 核心类型定义

### 2.1 Context

`Context` 是请求处理的核心数据结构，继承自 `http::ServerRequest`。

```cpp
// context.hpp
#include <any>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rfl.hpp>
#include <rfl/json.hpp>

#include "async_uv_http/server.h"

namespace async_uv::layer3 {

struct Context : http::ServerRequest {
    // ========== 响应对象 ==========
    http::ServerResponse response;
    
    // ========== 路径参数 ==========
    // 从 URL 中提取，如 /users/{id} -> params["id"] = "123"
    std::map<std::string, std::string> params;
    
    // ========== 中间件数据共享 ==========
    // 前置中间件可以存储数据，后置中间件可以读取
    // 例如：认证中间件存储 user_id，后续 handler 可以读取
    std::map<std::string, std::any> locals;

    // ========== 构造函数 ==========
    explicit Context(http::ServerRequest&& req);
    
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = default;
    Context& operator=(Context&&) = default;

    // ========== 路径参数访问 ==========
    // 返回参数值，若不存在返回空 string_view
    std::string_view param(std::string_view name) const;

    // ========== 中间件数据访问 ==========
    template<typename T>
    std::optional<T> local(std::string_view name) const;
    
    template<typename T>
    void set_local(std::string_view name, T&& value);

    // ========== JSON 操作 ==========
    // 从请求体解析 JSON，失败返回 nullopt
    template<typename T>
    std::optional<T> json_as() const;
    
    // 将对象序列化为 JSON 并设置为响应体
    // 自动设置 Content-Type: application/json
    template<typename T>
    void json(const T& obj);
    
    // 直接设置 JSON 字符串作为响应体
    void json_raw(std::string body);

    // ========== 响应设置 ==========
    void status(int code);
    void set(std::string_view name, std::string_view value);
    
    // 大小写不敏感的 header 查找
    std::optional<std::string> header(std::string_view name) const;
};

} // namespace async_uv::layer3
```

#### 参数提取规则

| 路由模式 | 请求路径 | `params` |
|---------|---------|----------|
| `/users/{id}` | `/users/123` | `{"id": "123"}` |
| `/posts/{id}/comments/{cid}` | `/posts/42/comments/7` | `{"id": "42", "cid": "7"}` |
| `/files/{path*}` | `/files/a/b/c.txt` | `{"path": "a/b/c.txt"}` |

### 2.2 中间件类型

```cpp
// middleware.hpp
namespace async_uv::layer3 {

// Next: 调用下一个中间件或最终 handler
// 调用 co_await next() 后会执行后续中间件链
// 当 next() 返回后，执行后置逻辑
using Next = std::function<Task<void>()>;

// 中间件签名
// ctx: 请求上下文
// next: 调用后续中间件链的函数
using Middleware = std::function<Task<void>(Context& ctx, Next next)>;

// 路由处理函数
// 最终的请求处理器，不调用 next()
using Handler = std::function<Task<void>(Context& ctx)>;

} // namespace async_uv::layer3
```

### 2.3 Router

```cpp
// router.hpp
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <async_uv/task.h>
#include <async_uv_layer3/router_node.hpp>

namespace async_uv::layer3 {

class Router {
public:
    Router();
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) noexcept;
    Router& operator=(Router&&) noexcept;

    // ========== 路由注册 ==========
    Router& get(std::string_view pattern, Handler handler);
    Router& post(std::string_view pattern, Handler handler);
    Router& put(std::string_view pattern, Handler handler);
    Router& del(std::string_view pattern, Handler handler);    // DELETE
    Router& patch(std::string_view pattern, Handler handler);
    Router& all(std::string_view pattern, Handler handler);    // 匹配所有方法

    // ========== 路由匹配 ==========
    struct MatchResult {
        Handler handler;
        std::map<std::string, std::string> params;
    };
    
    // 匹配路由，返回 handler 和参数
    // 若不匹配返回 nullopt
    std::optional<MatchResult> match(std::string_view method, 
                                      std::string_view path) const;

    // ========== 子路由挂载 ==========
    // 将 sub_router 挂载到 prefix 路径下
    Router& route(std::string_view prefix, Router sub_router);

private:
    std::unique_ptr<RouterNode<Handler>> root_;
    std::vector<std::pair<std::string, Router>> sub_routers_;

    void add_route(std::string_view method, std::string_view pattern, Handler handler);
};

} // namespace async_uv::layer3
```

### 2.4 RouterNode (Radix Tree)

```cpp
// router_node.hpp
namespace async_uv::layer3 {

template<typename Handler>
class RouterNode {
public:
    // 节点类型
    enum class Type : uint8_t {
        STATIC,    // 静态路径段：/users
        PARAM,     // 参数：{id}
        WILDCARD   // 通配符：{path*}
    };

    std::string prefix;                    // 公共前缀
    Type type = Type::STATIC;
    std::string param_name;                // 参数名
    
    std::vector<std::unique_ptr<RouterNode>> children;
    std::unordered_map<std::string, Handler> handlers;  // method -> handler

    // ========== 节点分类 ==========
    static constexpr Type classify(std::string_view segment);
    
    // ========== 路径匹配 ==========
    struct MatchResult {
        Handler handler;
        std::vector<std::pair<std::string, std::string>> params;
    };
    
    std::optional<MatchResult> match(std::string_view method, 
                                      std::string_view path) const;

    // ========== 节点添加 ==========
    RouterNode* add_child(std::string_view path, Type type, std::string param_name = "");

private:
    static size_t common_prefix(std::string_view a, std::string_view b);
};

} // namespace async_uv::layer3
```

### 2.5 App

```cpp
// app.hpp
namespace async_uv::layer3 {

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    // ========== 中间件注册 ==========
    // 中间件按注册顺序执行（洋葱模型）
    App& use(Middleware middleware);

    // ========== 路由注册（便捷方法） ==========
    App& get(std::string_view pattern, Handler handler);
    App& post(std::string_view pattern, Handler handler);
    App& put(std::string_view pattern, Handler handler);
    App& del(std::string_view pattern, Handler handler);
    App& patch(std::string_view pattern, Handler handler);

    // ========== 子路由挂载 ==========
    App& route(std::string_view prefix, Router sub_router);

    // ========== 配置 ==========
    App& with_limits(http::ServerLimits limits);
    App& with_policy(http::ServerConnectionPolicy policy);

    // ========== 启动服务 ==========
    Task<void> listen(uint16_t port, std::string_view host = "0.0.0.0");

private:
    Task<http::ServerResponse> handle_request(http::ServerRequest request);
    Task<void> run_middleware_chain(Context& ctx, size_t index);

    std::vector<Middleware> middlewares_;
    Router router_;
    http::ServerLimits limits_;
    http::ServerConnectionPolicy policy_;
};

// 工厂函数
App app();

} // namespace async_uv::layer3
```

---

## 3. Radix Tree 路由匹配

### 3.1 为什么选择 Radix Tree

| 数据结构 | 时间复杂度 | 空间复杂度 | 适用场景 |
|---------|-----------|-----------|---------|
| 线性列表 | O(n) | O(n) | 路由数量 < 50 |
| Hash Map | O(1) | O(n) | 只能精确匹配 |
| **Radix Tree** | O(k) | O(n) | 支持 parameter/exact/wildcard |
| 正则表达式 | O(n×k) | O(1) | 复杂模式，性能差 |

Radix Tree 的优势：
1. 时间复杂度 O(k)，k 为路径长度，与路由数量无关
2. 支持参数提取
3. 内存高效（共享公共前缀）

### 3.2 路径模式语法

| 语法 | 类型 | 匹配规则 | 示例 |
|------|------|---------|------|
| `/users` | STATIC | 精确匹配 | `/users` |
| `/users/{id}` | PARAM | 单段参数 | `/users/123` → `id=123` |
| `/files/{path*}` | WILDCARD | 多段通配 | `/files/a/b/c` → `path=a/b/c` |

**语法细节**：
- `{name}` - 捕获单个路径段
- `{name*}` - 捕获剩余所有路径段
- 参数名必须是有效的标识符 `[a-zA-Z_][a-zA-Z0-9_]*`

**冲突规则**：
- `/users/{id}` 和 `/users/{user_id}` - 冲突，无法区分
- `/users/{id}` 和 `/users/profile` - 不冲突，静态优先

### 3.3 匹配优先级

```
优先级: STATIC > PARAM > WILDCARD
```

示例：
```
路由：
  GET /users/profile
  GET /users/{id}
  GET /users/{id}/settings
  GET /users/{id}/*

请求：
  /users/profile     -> GET /users/profile (STATIC)
  /users/123         -> GET /users/{id}   (PARAM)
  /users/123/settings -> GET /users/{id}/settings (STATIC 子路径)
  /users/123/extra   -> GET /users/{id}/* (WILDCARD)
```

### 3.4 插入算法

```
add_route("/users/{id}/posts", handler):

1. 分割路径: ["users", "{id}", "posts"]
2. 从根节点开始遍历
3. 对每个段：
   - 若段以 { 开头：创建 PARAM 节点
   - 若段以 * 结尾：创建 WILDCARD 节点
   - 否则：创建 STATIC 节点
4. 在最终节点存储 handler
```

### 3.5 查找算法

```
match("GET", "/users/123/posts"):

1. 规范化路径: "users/123/posts"
2. 从根节点开始
3. 对每个段尝试匹配：
   a. 优先尝试 STATIC 子节点（前缀匹配）
   b. 若失败，尝试 PARAM 子节点（消耗一个段，记录参数）
   c. 若失败，尝试 WILDCARD 子节点（消耗剩余所有段）
4. 若匹配完成且有 handler，返回成功
5. 否则返回 nullopt
```

### 3.6 节点分裂

当插入新路径与现有节点共享前缀时，需要分裂：

```
已有路径: /users
插入路径: /user/profile

步骤：
1. 找到公共前缀: "user" (长度 4)
2. 分裂节点 "users":
   - 创建新节点 "user" (STATIC)
   - 原 "users" 成为 "user" 的子节点，前缀改为 "s"
3. 插入 "profile" 作为 "user" 的另一个子节点

结果树：
  user
  ├── s [GET /users handler]
  └── /profile [GET /user/profile handler]
```

---

## 4. 洋葱模型中间件

### 4.1 执行流程图

```
请求 ──────────────────────────────────────────────────────────►
      │                                                        │
      ▼                                                        │
┌──────────────┐                                              │
│ Middleware 1 │ ──── 前置逻辑 ────┐                           │
└──────────────┘                  │                            │
                                 ▼                            │
                      ┌──────────────┐                        │
                      │ Middleware 2 │ ──── 前置逻辑 ────┐    │
                      └──────────────┘                  │     │
                                                       ▼     │
                                           ┌──────────────┐ │
                                           │   Handler    │ │
                                           └──────────────┘ │
                                                       │     │
                                 ◄──── 后置逻辑 ◄─────┘     │
                      ┌──────────────┐                        │
                      │ Middleware 2 │ ◄──── 后置逻辑 ◄───────┘
                      └──────────────┘
                                 │
┌──────────────┐                 │
│ Middleware 1 │ ◄───────────────┘
└──────────────┘
      │
◄───── 响应

时间线：
  T1: Middleware 1 前置
  T2: Middleware 2 前置
  T3: Handler 执行
  T4: Middleware 2 后置
  T5: Middleware 1 后置
```

### 4.2 实现原理

```cpp
Task<void> App::run_middleware_chain(Context& ctx, size_t index) {
    if (index < middlewares_.size()) {
        // 执行当前中间件
        co_await middlewares_[index](ctx, [this, &ctx, index]() -> Task<void> {
            // next() 调用时执行下一个中间件
            co_await run_middleware_chain(ctx, index + 1);
        });
    } else {
        // 所有中间件执行完毕，匹配路由
        auto result = router_.match(ctx.method, ctx.target.path);
        if (result) {
            // 设置路径参数
            for (const auto& [key, value] : result->params) {
                ctx.params[key] = value;
            }
            // 执行 handler
            co_await result->handler(ctx);
        } else {
            // 404 Not Found
            ctx.status(404);
            ctx.json_raw("{\"error\":\"Not Found\"}");
        }
    }
}
```

### 4.3 内置中间件

#### 4.3.1 日志中间件

```cpp
inline Task<void> logger(Context& ctx, Next next) {
    auto start = std::chrono::steady_clock::now();
    
    // ===== 前置逻辑 =====
    std::cout << "[REQUEST] " << ctx.method << " " << ctx.target.path;
    if (!ctx.target.query_items.empty()) {
        std::cout << "?";
        for (const auto& [k, v] : ctx.target.query_items) {
            std::cout << k << "=" << v << "&";
        }
    }
    std::cout << "\n";
    
    // ===== 执行后续链 =====
    co_await next();
    
    // ===== 后置逻辑 =====
    auto duration = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    
    std::cout << "[RESPONSE] " << ctx.response.status_code 
              << " (" << ms << "ms)\n";
}
```

#### 4.3.2 错误处理中间件

```cpp
inline Task<void> error_handler(Context& ctx, Next next) {
    try {
        co_await next();
    } catch (const std::exception& e) {
        ctx.status(500);
        ctx.json({
            {"error", e.what()},
            {"path", std::string(ctx.target.path)}
        });
    } catch (...) {
        ctx.status(500);
        ctx.json_raw("{\"error\":\"Unknown internal error\"}");
    }
}
```

### 4.4 自定义中间件示例

#### 4.4.1 认证中间件

```cpp
Task<void> auth_middleware(Context& ctx, Next next) {
    auto auth_header = ctx.header("Authorization");
    
    if (!auth_header) {
        ctx.status(401);
        ctx.json({{"error", "Missing Authorization header"}});
        co_return;  // 不调用 next()，中断中间件链
    }
    
    // 验证 token
    auto user_id = co_await verify_token(*auth_header);
    if (!user_id) {
        ctx.status(403);
        ctx.json({{"error", "Invalid or expired token"}});
        co_return;
    }
    
    // 存储用户信息供后续使用
    ctx.set_local<std::string>("user_id", *user_id);
    
    // 继续执行
    co_await next();
}
```

#### 4.4.2 请求 ID 中间件

```cpp
#include <uuid/uuid.h>

Task<void> request_id_middleware(Context& ctx, Next next) {
    // 生成唯一请求 ID
    uuid_t uuid;
    uuid_generate(uuid);
    char uuid_str[37];
    uuid_unparse(uuid, uuid_str);
    
    ctx.set_local<std::string>("request_id", uuid_str);
    ctx.set("X-Request-ID", uuid_str);
    
    co_await next();
}
```

---

## 5. JSON 序列化

### 5.1 使用 reflect-cpp

Layer 3 使用 [reflect-cpp](https://github.com/getml/reflect-cpp) 进行 JSON 序列化/反序列化。

```cpp
#include <rfl.hpp>
#include <rfl/json.hpp>

// 定义结构体
struct User {
    int id;
    std::string name;
    std::string email;
};

// 序列化
User user{.id = 1, .name = "Alice", .email = "alice@example.com"};
std::string json_str = rfl::json::write(user);
// {"id":1,"name":"Alice","email":"alice@example.com"}

// 反序列化
auto result = rfl::json::read<User>(json_str);
if (result) {
    User user = *result;
}
```

### 5.2 在 Context 中使用

```cpp
// 响应 JSON
app.get("/users/{id}", [](Context& ctx) -> Task<> {
    auto id = ctx.param("id");
    User user = co_await db.find_user(id);
    ctx.json(user);  // 自动序列化
    co_return;
});

// 解析请求体
app.post("/users", [](Context& ctx) -> Task<> {
    auto user = ctx.json_as<User>();
    if (!user) {
        ctx.status(400);
        ctx.json({{"error", "Invalid JSON"}});
        co_return;
    }
    co_await db.create_user(*user);
    ctx.status(201);
    ctx.json(*user);
    co_return;
});
```

### 5.3 字段重命名和可选字段

```cpp
struct User {
    int id;
    rfl::Field<"user_name", std::string> name;  // JSON 中为 "user_name"
    rfl::Field<"email_address", std::string> email;
    std::optional<std::string> phone;  // 可选字段
};
```

---

## 6. 完整使用示例

### 6.1 基础 HTTP 服务

```cpp
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/middleware.hpp>
#include <async_uv/runtime.hpp>

Task<void> main_task() {
    auto app = async_uv::layer3::app();
    
    // 全局中间件
    app.use(async_uv::layer3::middleware::logger());
    app.use(async_uv::layer3::middleware::error_handler());
    
    // 基础路由
    app.get("/", [](Context& ctx) -> Task<> {
        ctx.json({{"message", "Hello World"}});
        co_return;
    });
    
    app.get("/health", [](Context& ctx) -> Task<> {
        ctx.json({{"status", "ok"}, {"timestamp", std::time(nullptr)}});
        co_return;
    });
    
    // 参数路由
    app.get("/users/{id}", [](Context& ctx) -> Task<> {
        auto id = ctx.param("id");
        ctx.json({
            {"id", std::string(id)},
            {"name", "User " + std::string(id)}
        });
        co_return;
    });
    
    // POST 请求
    app.post("/users", [](Context& ctx) -> Task<> {
        auto body = ctx.json_as<UserCreateRequest>();
        if (!body) {
            ctx.status(400);
            ctx.json({{"error", "Invalid request body"}});
            co_return;
        }
        
        // co_await db.create_user(*body);
        ctx.status(201);
        ctx.json({
            {"id", 1},
            {"name", body->name}
        });
        co_return;
    });
    
    co_await app.listen(8080, "0.0.0.0");
}

int main() {
    async_uv::Runtime runtime;
    runtime.spawn(main_task());
    return runtime.run();
}
```

### 6.2 子路由挂载

```cpp
Task<void> main_task() {
    auto app = async_uv::layer3::app();
    
    // API v1 路由
    auto api_v1 = async_uv::layer3::router();
    api_v1.get("/status", [](Context& ctx) -> Task<> {
        ctx.json({{"version", "1.0"}, {"status", "ok"}});
        co_return;
    });
    api_v1.get("/users", [](Context& ctx) -> Task<> {
        ctx.json({{"users", std::vector<std::string>{"Alice", "Bob"}}});
        co_return;
    });
    
    // API v2 路由
    auto api_v2 = async_uv::layer3::router();
    api_v2.get("/status", [](Context& ctx) -> Task<> {
        ctx.json({{"version", "2.0"}, {"status", "ok"}});
        co_return;
    });
    
    // 挂载子路由
    app.route("/api/v1", api_v1);
    app.route("/api/v2", api_v2);
    
    // 实际路由：
    // GET /api/v1/status -> v1 status handler
    // GET /api/v1/users  -> v1 users handler
    // GET /api/v2/status -> v2 status handler
    
    co_await app.listen(8080);
}
```

### 6.3 认证中间件

```cpp
Task<void> auth_middleware(Context& ctx, Next next) {
    auto auth = ctx.header("Authorization");
    if (!auth || !auth->starts_with("Bearer ")) {
        ctx.status(401);
        ctx.json({{"error", "Unauthorized"}});
        co_return;  // 不调用 next()，中断链
    }
    
    std::string token = auth->substr(7);
    auto user_id = co_await verify_token(token);
    if (!user_id) {
        ctx.status(403);
        ctx.json({{"error", "Invalid token"}});
        co_return;
    }
    
    ctx.set_local<std::string>("user_id", *user_id);
    co_await next();
}

Task<void> main_task() {
    auto app = async_uv::layer3::app();
    
    app.use(async_uv::layer3::middleware::logger());
    app.use(auth_middleware);  // 所有后续路由都需要认证
    
    app.get("/profile", [](Context& ctx) -> Task<> {
        auto user_id = ctx.local<std::string>("user_id").value();
        ctx.json({{"user_id", user_id}});
        co_return;
    });
    
    co_await app.listen(8080);
}
```

---

## 7. 错误处理

### 7.1 404 Not Found

当路由不匹配时，框架自动返回 404：

```cpp
// 在 run_middleware_chain 中：
if (!result) {
    ctx.status(404);
    ctx.json_raw("{\"error\":\"Not Found\"}");
}
```

自定义 404 处理：

```cpp
app.use([](Context& ctx, Next next) -> Task<> {
    co_await next();
    
    if (ctx.response.status_code == 404) {
        ctx.status(404);
        ctx.json({
            {"error", "Not Found"},
            {"path", std::string(ctx.target.path)},
            {"method", ctx.method}
        });
    }
});
```

### 7.2 405 Method Not Allowed

```cpp
// 在 app.cpp 中添加
app.use([](Context& ctx, Next next) -> Task<> {
    co_await next();
    
    if (ctx.response.status_code == 404) {
        // 检查是否有其他方法匹配此路径
        auto allowed = router_.get_allowed_methods(ctx.target.path);
        if (!allowed.empty()) {
            ctx.status(405);
            ctx.set("Allow", join(allowed, ", "));
            ctx.json({{"error", "Method Not Allowed"}});
        }
    }
});
```

---

## 8. 性能考虑

### 8.1 内存分配

- `Context` 继承自 `ServerRequest`，使用移动语义避免拷贝
- `std::map` 用于 `params` 和 `locals`，稳态情况下 allocation-free
- `rfl::json::write` 使用 SSO (Small String Optimization)

### 8.2 路由匹配性能

| 操作 | 时间复杂度 |
|------|-----------|
| 路由注册 | O(k) - k 为路径段数 |
| 路由匹配 | O(k) - k 为路径段数 |
| 参数提取 | O(k) - 与匹配同时完成 |

### 8.3 建议优化

1. **路由数量 > 1000**：考虑使用 Hash Map 预筛选
2. **高频路由**：放在前面注册，减少遍历深度
3. **中间件数量**：每增加一个中间件都会增加调用栈深度

---

## 9. 未来扩展

### 9.1 路由组

```cpp
// 计划中的 API
auto api = app.group("/api");
api.use(auth_middleware);  // 只对 /api/* 生效
api.get("/users", list_users);
api.get("/posts", list_posts);
```

### 9.2 WebSocket 支持

```cpp
// 计划中的 API
app.websocket("/ws", [](WebSocket& ws) -> Task<> {
    while (auto msg = co_await ws.receive()) {
        co_await ws.send("Echo: " + msg);
    }
});
```

### 9.3 内置中间件

- `cors()` - CORS 处理
- `body_limit(size)` - 请求体大小限制
- `rate_limit(requests, window)` - 限流
- `compress()` - 响应压缩

---

## 10. 文件结构

```
layer3/
├── CMakeLists.txt
├── docs/
│   └── design.md              # 本文档
├── include/
│   └── async_uv_layer3/
│       ├── app.hpp            # App 组装器
│       ├── context.hpp        # Context 定义
│       ├── middleware.hpp     # 内置中间件
│       ├── router.hpp         # Router 接口
│       └── router_node.hpp    # Radix Tree 节点
├── src/
│   ├── app.cpp
│   └── router.cpp
└── tests/
    ├── router_test.cpp        # 路由匹配测试
    ├── middleware_test.cpp    # 中间件测试
    └── integration_test.cpp   # 集成测试
```

## 11. 编译和使用

### 11.1 CMake 配置

```cmake
# 启用 Layer 3
cmake -DASYNC_UV_ENABLE_LAYER3=ON ..

# 编译
make async_uv_layer3
```

### 11.2 链接

```cmake
target_link_libraries(your_target PRIVATE async_uv::layer3)
```

### 11.3 头文件

```cpp
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/middleware.hpp>
#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/context.hpp>
```