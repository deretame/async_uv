# Layer 3 设计文档 - 开发清单

> **状态说明**：
> - ✅ 已实现
> - ⏳ 待实现
> - 📋 设计完成，待编码
> - ❌ 不做（说明原因）

---

## 目录

1. [概述](#1-概述)
2. [核心类型定义](#2-核心类型定义)
3. [路由系统](#3-路由系统)
4. [中间件系统](#4-中间件系统)
5. [请求体解析](#5-请求体解析)
6. [响应处理](#6-响应处理)
7. [错误处理](#7-错误处理)
8. [内置中间件](#8-内置中间件)
9. [安全考虑](#9-安全考虑)
10. [性能优化](#10-性能优化)
11. [测试策略](#11-测试策略)
12. [完整使用示例](#12-完整使用示例)
13. [未来扩展](#13-未来扩展)
14. [文件结构](#14-文件结构)
15. [编译和使用](#15-编译和使用)

---

## 1. 概述

### 1.1 设计目标

| 目标 | 说明 | 状态 |
|------|------|------|
| 零拷贝 | 尽量避免不必要的字符串拷贝 | ✅ |
| 类型安全 | 使用模板进行 JSON 序列化/反序列化 | ✅ |
| 协程原生 | 所有异步操作使用 C++20 协程 | ✅ |
| 易于扩展 | 中间件机制支持后置逻辑 | ✅ |
| 高性能路由 | Radix Tree O(k) 复杂度 | ✅ |
| 易用 API | 类似 Express/Koa 的风格 | ✅ |

### 1.2 依赖关系

```
Layer 3 (async_uv_layer3)
├── Layer 2 (async_uv_http)
│   ├── ServerRequest / ServerResponse
│   ├── HttpParser
│   └── read_request_from_socket / write_response_to_socket
├── Layer 1 (async_uv)
│   ├── Task<T>
│   └── TcpListener
└── reflect-cpp v0.24.0
    └── rfl::json::read<T> / rfl::json::write<T>
```

### 1.3 与其他层的关系

```
                    ┌─────────────────────────┐
                    │      Application         │
                    └───────────┬─────────────┘
                                │
                    ┌───────────▼─────────────┐
                    │    Layer 3: Framework   │  ← 路由、中间件、Context
                    │  Router, Middleware, App │
                    └───────────┬─────────────┘
                                │
                    ┌───────────▼─────────────┐
                    │   Layer 2: HTTP Server   │  ← HTTP 协议处理
                    │  ServerRequest/Response  │
                    └───────────┬─────────────┘
                                │
                    ┌───────────▼─────────────┐
                    │   Layer 1: Async Prims   │  ← 异步原语
                    │  Task, TcpListener       │
                    └─────────────────────────┘
```

---

## 2. 核心类型定义

### 2.1 Context ✅

```cpp
struct Context : http::ServerRequest {
    // === 响应对象 ===
    http::ServerResponse response;
    
    // === 路径参数 ===
    // 从 URL 提取: /users/{id} → params["id"] = "123"
    std::map<std::string, std::string> params;
    
    // === 中间件数据共享 ===
    // 前置中间件存储，后置中间件/handler读取
    std::map<std::string, std::any> locals;
    
    // === 构造函数 ===
    explicit Context(http::ServerRequest&& req);
    
    // === 路径参数访问 ===
    std::string_view param(std::string_view name) const;
    
    // === 中间件数据访问 ===
    template<typename T>
    std::optional<T> local(std::string_view name) const;
    
    template<typename T>
    void set_local(std::string_view name, T&& value);
    
    // === JSON 操作 ===
    template<typename T>
    std::optional<T> json_as() const;
    
    template<typename T>
    void json(const T& obj);
    
    void json_raw(std::string body);
    
    // === 响应设置 ===
    void status(int code);
    void set(std::string_view name, std::string_view value);
    std::optional<std::string> header(std::string_view name) const;
};
```

#### 参数提取规则 ✅

| 路由模式 | 请求路径 | `params` |
|---------|---------|----------|
| `/users/{id}` | `/users/123` | `{"id": "123"}` |
| `/posts/{id}/comments/{cid}` | `/posts/42/comments/7` | `{"id": "42", "cid": "7"}` |
| `/files/{path*}` | `/files/a/b/c.txt` | `{"path": "a/b/c.txt"}` |

### 2.2 Handler 和 Middleware ✅

```cpp
// Next: 调用下一个中间件或最终 handler
using Next = std::function<Task<void>()>;

// 中间件签名
using Middleware = std::function<Task<void>(Context& ctx, Next next)>;

// 路由处理函数
using Handler = std::function<Task<void>(Context& ctx)>;
```

### 2.3 Router ✅

```cpp
class Router {
public:
    // === 路由注册 ===
    Router& get(std::string_view pattern, Handler handler);
    Router& post(std::string_view pattern, Handler handler);
    Router& put(std::string_view pattern, Handler handler);
    Router& del(std::string_view pattern, Handler handler);
    Router& patch(std::string_view pattern, Handler handler);
    Router& all(std::string_view pattern, Handler handler);  // 匹配所有方法
    
    // === 路由匹配 ===
    struct MatchResult {
        Handler handler;
        std::map<std::string, std::string> params;
    };
    
    std::optional<MatchResult> match(std::string_view method, 
                                      std::string_view path) const;
    
    // === 子路由挂载 ===
    Router& route(std::string_view prefix, Router sub_router);

private:
    std::unique_ptr<RouterNode<Handler>> root_;
    std::vector<std::pair<std::string, Router>> sub_routers_;
};
```

### 2.4 App ✅

```cpp
class App {
public:
    // === 中间件注册 ===
    App& use(Middleware middleware);
    
    // === 路由注册 ===
    App& get(std::string_view pattern, Handler handler);
    App& post(std::string_view pattern, Handler handler);
    App& put(std::string_view pattern, Handler handler);
    App& del(std::string_view pattern, Handler handler);
    App& patch(std::string_view pattern, Handler handler);
    
    // === 子路由 ===
    App& route(std::string_view prefix, Router sub_router);
    
    // === 配置 ===
    App& with_limits(http::ServerLimits limits);
    App& with_policy(http::ServerConnectionPolicy policy);
    
    // === 启动 ===
    Task<void> listen(uint16_t port, std::string_view host = "0.0.0.0");

private:
    std::vector<Middleware> middlewares_;
    Router router_;
    http::ServerLimits limits_;
    http::ServerConnectionPolicy policy_;
};
```

### 2.5 待添加的类型 ⏳

#### 2.5.1 RouteGroup ⏳

```cpp
// 路由组 - 对一组路由应用相同的中间件
class RouteGroup {
public:
    RouteGroup& use(Middleware middleware);
    RouteGroup& get(std::string_view pattern, Handler handler);
    RouteGroup& post(std::string_view pattern, Handler handler);
    // ...
    
    Router to_router() const;  // 转换为普通 Router

private:
    std::vector<Middleware> middlewares_;
    Router router_;
};
```

**使用示例**：

```cpp
// 计划中的 API
auto api = RouteGroup();
api.use(auth_middleware);  // 只对这组路由生效
api.get("/users", list_users);
api.get("/posts", list_posts);

app.route("/api", api.to_router());
```

#### 2.5.2 RequestLifeCycle ⏳

```cpp
// 生命周期钩子
struct LifeCycle {
    std::function<Task<void>()> on_start;      // 服务器启动时
    std::function<Task<void>()> on_stop;       // 服务器停止时
    std::function<void(Context&)> on_error;    // 错误处理
};

App& with_lifecycle(LifeCycle lifecycle);
```

---

## 3. 路由系统

### 3.1 Radix Tree 实现 ✅

```cpp
template<typename Handler>
class RouterNode {
public:
    enum class Type : uint8_t {
        STATIC,    // /users
        PARAM,      // {id}
        WILDCARD    // {path*}
    };
    
    std::string prefix;
    Type type = Type::STATIC;
    std::string param_name;
    std::vector<std::unique_ptr<RouterNode>> children;
    std::unordered_map<std::string, Handler> handlers;
    
    RouterNode* add_child(std::string_view path, Type type, std::string param_name);
    std::optional<MatchResult> match(std::string_view method, std::string_view path) const;
};
```

### 3.2 路径模式语法 ✅

| 语法 | 类型 | 匹配 | 提取 |
|------|------|------|------|
| `/users` | STATIC | 精确匹配 | 无 |
| `/users/{id}` | PARAM | 单段 | `"id": "123"` |
| `/files/{path*}` | WILDCARD | 多段 | `"path": "a/b/c"` |

### 3.3 匹配优先级 ✅

```
优先级: STATIC > PARAM > WILDCARD
```

示例：
```
路由：
  GET /users/profile      (STATIC)
  GET /users/{id}         (PARAM)
  GET /users/{id}/posts   (PARAM + STATIC)
  GET /users/{id}/*       (PARAM + WILDCARD)

请求匹配：
  /users/profile      → STATIC
  /users/123          → PARAM
  /users/123/posts    → PARAM + STATIC
  /users/123/extra    → PARAM + WILDCARD
```

### 3.4 插入算法 ✅

```
add_route("GET", "/users/{id}/posts"):

1. 规范化路径: ["users", "{id}", "posts"]
2. 从根节点遍历
3. 对每段：
   - STATIC: 找公共前缀，可能分裂节点
   - PARAM: 创建参数节点
   - WILDCARD: 创建通配节点
4. 在最终节点存储 handler
```

### 3.5 待添加的路由功能 ⏳

#### 3.5.1 路由元数据 ⏳

```cpp
// 每个路由可以附加元数据
struct RouteMeta {
    std::string name;           // 路由名称
    std::string description;    // 描述
    std::vector<std::string> tags;  // OpenAPI tags
    bool require_auth;          // 是否需要认证
    // ...
};

Router& get(std::string_view pattern, Handler handler, RouteMeta meta);
```

#### 3.5.2 路由列表 ⏳

```cpp
// 获取所有已注册路由
std::vector<RouteInfo> Router::routes() const;

struct RouteInfo {
    std::string method;
    std::string pattern;
    std::vector<std::string> param_names;
    RouteMeta meta;
};

// 用于生成 OpenAPI 文档
```

#### 3.5.3 方法检查 ⏳

```cpp
// 检查某路径支持哪些方法
std::vector<std::string> Router::allowed_methods(std::string_view path) const;

// 用于 405 Method Not Allowed 响应
```

---

## 4. 中间件系统

### 4.1 洋葱模型 ✅

```
请求 ──────────────────────────────────────────────────────►
      │                                                      │
      ▼                                                      │
┌──────────────┐                                            │
│ Middleware 1 │ ──── 前置逻辑 ────┐                         │
└──────────────┘                  │                          │
                                 ▼                          │
                      ┌──────────────┐                    │
                      │ Middleware 2 │ ──── 前置逻辑 ────┐ │
                      └──────────────┘                  │ │
                                                       ▼ │
                                           ┌──────────────┐
                                           │   Handler    │
                                           └──────────────┘
                                                       │ │
                                 ◄──── 后置逻辑 ◄─────┘ │
                      ┌──────────────┐                  │
                      │ Middleware 2 │ ◄──── 后置逻辑 ◄─┘
                      └──────────────┘
                                 │
┌──────────────┐                 │
│ Middleware 1 │ ◄───────────────┘
└──────────────┘
      │
◄───── 响应
```

### 4.2 执行原理 ✅

```cpp
Task<void> App::run_middleware_chain(Context& ctx, size_t index) {
    if (index < middlewares_.size()) {
        co_await middlewares_[index](ctx, [this, &ctx, index]() -> Task<void> {
            co_await run_middleware_chain(ctx, index + 1);
        });
    } else {
        // 所有中间件执行完毕，匹配路由
        auto result = router_.match(ctx.method, ctx.target.path);
        if (result) {
            for (const auto& [key, value] : result->params) {
                ctx.params[key] = value;
            }
            co_await result->handler(ctx);
        } else {
            ctx.status(404);
            ctx.json_raw("{\"error\":\"Not Found\"}");
        }
    }
}
```

### 4.3 中间件注册顺序 ✅

```cpp
// 执行顺序取决于注册顺序
app.use(middleware_a);  // 最外层
app.use(middleware_b);  // 中间层
app.use(middleware_c);  // 最内层

// 请求流程：
// A 前置 → B 前置 → C 前置 → Handler → C 后置 → B 后置 → A 后置
```

---

## 5. 请求体解析

### 5.1 Content-Type 处理流程 ✅

```
┌─────────────────────────────────────────────────────────┐
│                      请求到达                            │
└─────────────────────────┬─────────────────────────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │ 检查 Content-Type 头   │
              └───────────┬───────────┘
                          │
         ┌────────────────┼────────────────┐
         │                │                │
         ▼                ▼                ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ application/json│ │  x-www-form-   │ │ multipart/form- │
│                 │ │   urlencoded    │ │     data        │
└────────┬────────┘ └────────┬────────┘ └────────┬────────┘
         │                   │                   │
         ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ ctx.json_as<T>()│ │ form_parser     │ │ multipart_parser│
│                 │ │ 中间件           │ │ 中间件           │
└─────────────────┘ └─────────────────┘ └─────────────────┘
```

### 5.2 JSON 解析 ✅

```cpp
// 反序列化
auto user = ctx.json_as<User>();
if (!user) {
    ctx.status(400);
    ctx.json({{"error", "Invalid JSON"}});
    co_return;
}

// 序列化
ctx.json({
    {"id", 1},
    {"name", "Alice"},
    {"email", "alice@example.com"}
});
```

### 5.3 Form 解析 📋

#### 5.3.1 application/x-www-form-urlencoded 📋

```cpp
// form_parser.hpp
namespace async_uv::layer3::middleware {

Task<void> form_parser(Context& ctx, Next next);
std::optional<std::string> form_field(Context& ctx, std::string_view name);

} // namespace async_uv::layer3::middleware
```

**实现要点**：
- 解析 `key1=value1&key2=value2` 格式
- 调用 `ada::unicode::percent_decode` 解码
- 存入 `ctx.locals["form_data"]`

#### 5.3.2 multipart/form-data 📋

```cpp
// multipart_parser.hpp
namespace async_uv::layer3 {

struct MultipartField {
    std::string name;
    std::string filename;      // 仅文件字段
    std::string content_type;
    std::string data;
    
    bool is_file() const;
};

struct MultipartFormData {
    std::unordered_map<std::string, std::vector<MultipartField>> fields;
    
    std::optional<std::string> get(std::string_view name) const;
    std::optional<MultipartField> get_file(std::string_view name) const;
    const std::vector<MultipartField>* get_all(std::string_view name) const;
};

std::optional<MultipartFormData> parse_multipart(
    std::string_view body,
    std::string_view boundary
);

namespace middleware {
Task<void> multipart_parser(Context& ctx, Next next);
}

std::optional<MultipartFormData> get_multipart(Context& ctx);

} // namespace async_uv::layer3
```

**解析步骤**：
1. 从 `Content-Type` 提取 `boundary`
2. 按 `--boundary` 分割各部分
3. 解析每部分的 header（`Content-Disposition`, `Content-Type`）
4. 提取 `name`, `filename` 等参数
5. 读取 body 数据

### 5.4 请求体大小限制 ⏳

```cpp
// body_limit.hpp
namespace async_uv::layer3::middleware {

Middleware body_limit(size_t max_bytes);

} // namespace async_uv::layer3::middleware
```

**实现要点**：
- 使用 ServerLimits.max_body_bytes
- 在中间件中检查 `ctx.body_size`
- 返回 413 Payload Too Large

---

## 6. 响应处理

### 6.1 响应类型 ✅

```cpp
struct ServerResponse {
    int status_code = 200;
    std::string reason;
    std::vector<Header> headers;
    std::string body;
    bool keep_alive = true;
    bool chunked = false;
};
```

### 6.2 响应便捷方法 ✅

```cpp
// Context 中的响应方法
void status(int code);                              // 设置状态码
void set(std::string_view name, std::string_view value);  // 设置头
void json(const T& obj);                            // JSON 响应
void json_raw(std::string body);                    // 原始 JSON 字符串
void send(std::string body);                        // 纯文本响应
void send_file(std::filesystem::path path);         // ⏳ 发送文件
```

### 6.3 待添加的响应方法 ⏳

#### 6.3.1 重定向 ⏳

```cpp
// 重定向
void redirect(std::string_view url, int code = 302);
void redirect_permanent(std::string_view url);  // 301
void redirect_temporary(std::string_view url);  // 307
```

#### 6.3.2 文件下载 ⏳

```cpp
// 发送文件
void send_file(std::filesystem::path path);
void download(std::filesystem::path path, std::string_view filename);

// 设置 Content-Disposition: attachment; filename="..."
```

#### 6.3.3 流式响应 ⏳

```cpp
// 流式响应（用于大文件）
Task<void> stream(Context& ctx, std::function<Task<void>(std::function<Task<void>(std::string_view)>)> generator);

// 示例：流式返回日志
app.get("/logs", [](Context& ctx) -> Task<> {
    co_await ctx.stream([](auto write) -> Task<void> {
        for (int i = 0; i < 100; ++i) {
            co_await write("log line " + std::to_string(i) + "\n");
        }
    });
});
```

### 6.4 Content-Type 自动设置 📋

```cpp
// 根据文件扩展名自动设置 Content-Type
std::string_view mime_type(std::string_view path) {
    static const std::unordered_map<std::string_view, std::string_view> types = {
        {".html", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".gif", "image/gif"},
        {".pdf", "application/pdf"},
        // ...
    };
    // ...
}
```

---

## 7. 错误处理

### 7.1 HTTP 错误码 ✅

```cpp
// 框架自动返回的错误
404 Not Found    - 路由不匹配
405 Method Not Allowed - 路由存在但方法不对 ⏳
413 Payload Too Large - 请求体过大 ⏳
415 Unsupported Media Type - Content-Type 不支持 ⏳
500 Internal Server Error - 内部错误
```

### 7.2 错误响应格式 ✅

```json
{
    "error": "Not Found",
    "path": "/users/123",
    "method": "GET",
    "request_id": "abc123"
}
```

### 7.3 自定义错误处理 ⏳

```cpp
// 自定义错误处理中间件
app.use([](Context& ctx, Next next) -> Task<> {
    try {
        co_await next();
    } catch (const ValidationError& e) {
        ctx.status(400);
        ctx.json({{"error", e.what()}});
    } catch (const AuthError& e) {
        ctx.status(401);
        ctx.json({{"error", e.what()}});
    } catch (const std::exception& e) {
        ctx.status(500);
        ctx.json({{"error", "Internal Server Error"}});
        // 记录日志
    }
});
```

### 7.4 错误码定义 ⏳

```cpp
// error_codes.hpp
namespace async_uv::layer3 {

enum class ErrorCode : int {
    // 400xx - 客户端错误
    Bad_Request = 40000,
    Invalid_JSON = 40001,
    Missing_Field = 40002,
    Invalid_Field = 40003,
    
    // 401xx - 认证错误
    Unauthorized = 40100,
    Invalid_Token = 40101,
    Token_Expired = 40102,
    
    // 403xx - 权限错误
    Forbidden = 40300,
    Insufficient_Permissions = 40301,
    
    // 404xx - 资源不存在
    Not_Found = 40400,
    Resource_Not_Found = 40401,
    
    // 500xx - 服务端错误
    Internal_Error = 50000,
    Database_Error = 50001,
    External_Service_Error = 50002,
};

struct Error {
    ErrorCode code;
    std::string message;
    std::optional<std::string> detail;
    
    void to_json(Context& ctx) const;
};

} // namespace async_uv::layer3
```

---

## 8. 内置中间件

### 8.1 已实现 ✅

| 中间件 | 功能 | 状态 |
|--------|------|------|
| `logger()` | 请求日志 | ✅ |
| `error_handler()` | 错误捕获 | ✅ |

### 8.2 待实现 ⏳

| 中间件 | 功能 | 优先级 |
|--------|------|--------|
| `cors()` | CORS 处理 | 高 |
| `body_limit(size)` | 请求体大小限制 | 高 |
| `form_parser()` | 表单解析 | 中 |
| `multipart_parser()` | 文件上传解析 | 中 |
| `rate_limit()` | 限流 | 中 |
| `compress()` | 响应压缩 | 低 |
| `etag()` | ETag 缓存 | 低 |
| `static_files()` | 静态文件服务 | ❌ (交给 Nginx) |

### 8.3 CORS 中间件 ⏳

```cpp
// cors.hpp
namespace async_uv::layer3::middleware {

struct CorsOptions {
    std::vector<std::string> allow_origins = {"*"};
    std::vector<std::string> allow_methods = {"GET", "POST", "PUT", "DELETE", "PATCH"};
    std::vector<std::string> allow_headers = {"Content-Type", "Authorization"};
    std::vector<std::string> expose_headers;
    bool allow_credentials = false;
    int max_age = 86400;
};

Middleware cors(CorsOptions options = {});

} // namespace async_uv::layer3::middleware
```

**实现**：

```cpp
inline Middleware cors(CorsOptions options) {
    return [opts = std::move(options)](Context& ctx, Next next) -> Task<void> {
        auto origin = ctx.header("Origin");
        if (!origin) {
            co_await next();
            co_return;
        }
        
        // 检查 origin 是否允许
        bool allowed = (opts.allow_origins.size() == 1 && opts.allow_origins[0] == "*") ||
                       std::find(opts.allow_origins.begin(), opts.allow_origins.end(), *origin) != opts.allow_origins.end();
        
        if (!allowed) {
            co_await next();
            co_return;
        }
        
        ctx.set("Access-Control-Allow-Origin", *origin);
        
        if (opts.allow_credentials) {
            ctx.set("Access-Control-Allow-Credentials", "true");
        }
        
        // 预检请求
        if (ctx.method == "OPTIONS") {
            ctx.set("Access-Control-Allow-Methods", join(opts.allow_methods, ", "));
            ctx.set("Access-Control-Allow-Headers", join(opts.allow_headers, ", "));
            ctx.set("Access-Control-Max-Age", std::to_string(opts.max_age));
            ctx.status(204);
            co_return;
        }
        
        // 实际请求
        if (!opts.expose_headers.empty()) {
            ctx.set("Access-Control-Expose-Headers", join(opts.expose_headers, ", "));
        }
        
        co_await next();
    };
}
```

### 8.4 Rate Limit 中间件 ⏳

```cpp
// rate_limit.hpp
namespace async_uv::layer3::middleware {

struct RateLimitOptions {
    int requests;              // 请求数限制
    std::chrono::seconds window; // 时间窗口
    std::function<std::string(Context&)> key_selector;  // 键选择器（默认按 IP）
    std::function<Task<void>(Context&)> on_limited;    // 超限回调
};

Middleware rate_limit(RateLimitOptions options);

// 使用
app.use(rate_limit({
    .requests = 100,
    .window = std::chrono::seconds(60),
    .key_selector = [](Context& ctx) { return ctx.meta.remote_address; },
    .on_limited = [](Context& ctx) -> Task<> {
        ctx.status(429);
        ctx.json({{"error", "Too Many Requests"}});
        co_return;
    }
}));
```

### 8.5 Compression 中间件 ⏳

```cpp
// compress.hpp
namespace async_uv::layer3::middleware {

struct CompressOptions {
    int level = 6;  // 压缩级别 (1-9)
    size_t min_size = 1024;  // 最小压缩字节数
    std::vector<std::string> types = {"text/*", "application/json"};
};

Middleware compress(CompressOptions options = {});

} // namespace async_uv::layer3::middleware
```

---

## 9. 安全考虑

### 9.1 HTTPS 支持 📋

Layer 3 依赖 Layer 2 的 TLS 支持：

```cpp
// 启动 HTTPS 服务（计划）
app.with_tls(cert_path, key_path);
co_await app.listen(443);
```

### 9.2 输入验证 ⏳

```cpp
// 验证中间件
template<typename T>
Middleware validate(std::function<bool(const T&)> validator);

// 使用
app.post("/users", 
    validate<UserCreateRequest>([](const auto& req) {
        return !req.name.empty() && req.name.size() <= 100;
    }),
    [](Context& ctx) -> Task<> {
        auto user = ctx.json_as<UserCreateRequest>();
        // ...
    }
);
```

### 9.3 XSS 防护 📋

- 自动转义 JSON 输出中的 HTML 特殊字符
- Content-Type 正确设置防止 MIME 嗅探

### 9.4 CSRF 防护 📋

```cpp
// CSRF 中间件（计划）
Middleware csrf_protection(std::string secret);

// 在表单中使用
// <input type="hidden" name="_csrf" value="{{ csrf_token }}">
```

### 9.5 安全头 📋

```cpp
// 安全头中间件
Middleware security_headers() {
    return [](Context& ctx, Next next) -> Task<void> {
        ctx.set("X-Content-Type-Options", "nosniff");
        ctx.set("X-Frame-Options", "DENY");
        ctx.set("X-XSS-Protection", "1; mode=block");
        ctx.set("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
        co_await next();
    };
}
```

### 9.6 SQL 注入防护 📋

Layer 3 不直接处理 SQL，建议：
- 使用参数化查询（Layer 2 SQL 模块）
- 不拼接用户输入到 SQL

### 9.7 为什么不做静态文件服务 ❌

**原因**：Nginx 更擅长：
- 更高的性能（sendfile、零拷贝）
- 更好的缓存控制
- 更安全（隔离应用层）
- 更灵活的 URL 重写

**最佳实践**：
```
Nginx 配置：
location /static/ {
    alias /var/www/static/;
    expires 30d;
}

location /api/ {
    proxy_pass http://127.0.0.1:8080;
}
```

---

## 10. 性能优化

### 10.1 内存分配 ✅

| 对象 | 优化策略 |
|------|---------|
| `Context` | 继承 `ServerRequest`，移动语义避免拷贝 |
| `params` | `std::map` 稳态无分配 |
| `locals` | `std::map` + `std::any` 按需分配 |
| JSON 字符串 | reflect-cpp 使用 SSO |

### 10.2 路由性能 ✅

| 操作 | 时间复杂度 |
|------|-----------|
| 路由注册 | O(k), k=路径段数 |
| 路由匹配 | O(k), k=路径段数 |
| 参数提取 | O(k), 与匹配同时完成 |

### 10.3 基准测试 ⏳

```cpp
// benchmarks/router_bench.cpp
// 预期性能

// 路由匹配：< 1μs/op
// 中间件开销：< 100ns/op
// JSON 解析 10KB: < 50μs/op
// JSON 序列化 10KB: < 30μs/op

// QPS 预期（简单路由）:
// - 单核心: > 50,000 req/s
// - 4核心: > 150,000 req/s
```

### 10.4 优化建议 ✅

1. **路由数量 > 1000**：考虑按前缀分组
2. **高频路由**：放在前面注册
3. **中间件数量**：每增加一个增加调用栈深度
4. **JSON 请求体**：使用 `json_as<T>()` 避免二次解析

---

## 11. 测试策略

### 11.1 单元测试 ⏳

```cpp
// tests/router_test.cpp
TEST_CASE("Router static path") {
    Router router;
    int called = 0;
    router.get("/users", [&](Context& ctx) -> Task<> {
        called++;
        co_return;
    });
    
    auto result = router.match("GET", "/users");
    REQUIRE(result.has_value());
    REQUIRE(called == 0);  // 还没调用
}

TEST_CASE("Router param extraction") {
    Router router;
    router.get("/users/{id}", [](Context& ctx) -> Task<> { co_return; });
    
    auto result = router.match("GET", "/users/123");
    REQUIRE(result.has_value());
    REQUIRE(result->params["id"] == "123");
}

TEST_CASE("Router wildcard") {
    Router router;
    router.get("/files/{path*}", [](Context& ctx) -> Task<> { co_return; });
    
    auto result = router.match("GET", "/files/a/b/c.txt");
    REQUIRE(result.has_value());
    REQUIRE(result->params["path"] == "a/b/c.txt");
}
```

### 11.2 中间件测试 ⏳

```cpp
// tests/middleware_test.cpp
TEST_CASE("Middleware onion model") {
    std::vector<int> order;
    
    auto app = App();
    app.use([&](Context& ctx, Next next) -> Task<> {
        order.push_back(1);
        co_await next();
        order.push_back(3);
    });
    app.use([&](Context& ctx, Next next) -> Task<> {
        order.push_back(2);
        co_await next();
        order.push_back(4);
    });
    app.get("/", [&](Context& ctx) -> Task<> {
        order.push_back(5);
        co_return;
    });
    
    // 执行请求...
    REQUIRE(order == std::vector<int>{1, 2, 5, 4, 3});
}
```

### 11.3 集成测试 ⏳

```cpp
// tests/integration_test.cpp
TEST_CASE("Full request cycle") {
    auto app = App();
    app.get("/hello", [](Context& ctx) -> Task<> {
        ctx.json({{"message", "Hello"}});
        co_return;
    });
    
    // 模拟 HTTP 请求
    auto response = co_await simulate_request(app, "GET", "/hello");
    REQUIRE(response.status_code == 200);
    REQUIRE(response.body == R"({"message":"Hello"})");
}
```

### 11.4 性能测试 ⏳

```cpp
// benchmarks/bench.cpp
void benchmark_router_matching(benchmark::State& state) {
    Router router;
    for (int i = 0; i < 100; ++i) {
        router.get("/users/" + std::to_string(i), [](Context&) -> Task<> { co_return; });
    }
    
    for (auto _ : state) {
        auto result = router.match("GET", "/users/50");
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(benchmark_router_matching);
```

---

## 12. 完整使用示例

### 12.1 REST API ✅

```cpp
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/middleware.hpp>

struct User {
    int id;
    std::string name;
    std::string email;
};

Task<void> main_task() {
    auto app = async_uv::layer3::app();
    
    // 全局中间件
    app.use(async_uv::layer3::middleware::logger());
    app.use(async_uv::layer3::middleware::error_handler());
    
    // CRUD
    app.get("/users", [](Context& ctx) -> Task<> {
        std::vector<User> users = co_await db.get_all_users();
        ctx.json(users);
        co_return;
    });
    
    app.get("/users/{id}", [](Context& ctx) -> Task<> {
        auto id = ctx.param("id");
        auto user = co_await db.get_user(id);
        if (!user) {
            ctx.status(404);
            ctx.json({{"error", "User not found"}});
            co_return;
        }
        ctx.json(*user);
        co_return;
    });
    
    app.post("/users", [](Context& ctx) -> Task<> {
        auto user = ctx.json_as<User>();
        if (!user) {
            ctx.status(400);
            co_return;
        }
        co_await db.create_user(*user);
        ctx.status(201);
        ctx.json(*user);
        co_return;
    });
    
    app.put("/users/{id}", [](Context& ctx) -> Task<> {
        auto id = ctx.param("id");
        auto user = ctx.json_as<User>();
        co_await db.update_user(id, *user);
        ctx.json(*user);
        co_return;
    });
    
    app.del("/users/{id}", [](Context& ctx) -> Task<> {
        auto id = ctx.param("id");
        co_await db.delete_user(id);
        ctx.status(204);
        co_return;
    });
    
    co_await app.listen(8080);
}
```

### 12.2 认证与授权 ⏳

```cpp
// 认证中间件
Task<void> require_auth(Context& ctx, Next next) {
    auto auth = ctx.header("Authorization");
    if (!auth || !auth->starts_with("Bearer ")) {
        ctx.status(401);
        ctx.json({{"error", "Missing or invalid Authorization header"}});
        co_return;
    }
    
    std::string token = auth->substr(7);
    auto user = co_await verify_token(token);
    if (!user) {
        ctx.status(401);
        ctx.json({{"error", "Invalid or expired token"}});
        co_return;
    }
    
    ctx.set_local("user", std::move(*user));
    co_await next();
}

// 授权中间件
Task<void> require_role(std::string role) {
    return [role](Context& ctx, Next next) -> Task<void> {
        auto user = ctx.local<User>("user");
        if (!user || user->role != role) {
            ctx.status(403);
            ctx.json({{"error", "Insufficient permissions"}});
            co_return;
        }
        co_await next();
    };
}

// 使用
auto admin_router = Router();
admin_router.use(require_auth);
admin_router.use(require_role("admin"));
admin_router.del("/users/{id}", [](Context& ctx) -> Task<> {
    // 只有 admin 可以删除用户
});
```

### 12.3 文件上传 ⏳

```cpp
app.post("/upload", 
    async_uv::layer3::middleware::multipart_parser(),
    [](Context& ctx) -> Task<> {
        auto multipart = async_uv::layer3::get_multipart(ctx);
        if (!multipart) {
            ctx.status(400);
            co_return;
        }
        
        // 保存文件
        auto file = multipart->get_file("avatar");
        if (file) {
            std::filesystem::create_directories("uploads");
            std::ofstream out("uploads/" + file->filename, std::ios::binary);
            out.write(file->data.data(), file->data.size());
        }
        
        ctx.json({
            {"success", true},
            {"filename", file ? file->filename : ""}
        });
        co_return;
    }
);
```

### 12.4 WebSocket 升级 ⏳

```cpp
// 计划中的 API
app.websocket("/ws", [](WebSocket& ws) -> Task<> {
    ws.on_message([](std::string_view msg) -> Task<> {
        co_await ws.send("Echo: " + std::string(msg));
    });
    
    ws.on_close([]() -> Task<> {
        std::cout << "Client disconnected\n";
        co_return;
    });
    
    co_await ws.run();
});
```

---

## 13. 未来扩展

### 13.1 HTTP/2 支持 📋

- 使用 Layer 2 的 HTTP/2 支持
- 复用现有路由和中间件
- 服务器推送

### 13.2 WebSocket 支持 📋

```cpp
class WebSocket {
public:
    Task<void> send(std::string_view data);
    Task<std::optional<std::string>> receive();
    void on_message(std::function<Task<void>(std::string_view)> handler);
    void on_close(std::function<Task<void>()> handler);
};
```

### 13.3 OpenAPI 文档生成 ⏳

```cpp
// 从路由生成 OpenAPI 文档
auto openapi = app.generate_openapi({
    .title = "My API",
    .version = "1.0.0"
});

app.get("/openapi.json", [&](Context& ctx) -> Task<> {
    ctx.json(openapi);
    co_return;
});
```

### 13.4 请求验证 ⏳

```cpp
// 使用 reflect-cpp 的验证功能
struct UserCreate {
    std::string name;        // rfl::Pattern正则验证
    std::string email;
    int age;                 // rfl::GreaterThan<0>
};

app.post("/users", validate<UserCreate>(), [](Context& ctx) -> Task<> {
    // 验证通过，user 已验证
    auto user = ctx.json_as<UserCreate>();
    co_return;
});
```

### 13.5 依赖注入 ⏳

```cpp
// 服务注入
class Database { /* ... */ };

app.use([](Context& ctx, Next next) -> Task<void> {
    ctx.set_local("db", Database{});
    co_await next();
});

app.get("/users", [](Context& ctx) -> Task<> {
    auto& db = ctx.local<Database>("db");
    // ...
});
```

---

## 14. 文件结构 ✅

```
layer3/
├── CMakeLists.txt
├── docs/
│   └── design.md                  # 本文档
├── include/
│   └── async_uv_layer3/
│       ├── app.hpp                # ✅ App 组装器
│       ├── context.hpp             # ✅ Context
│       ├── router.hpp             # ✅ Router 接口
│       ├── router_node.hpp        # ✅ Radix Tree
│       ├── middleware.hpp         # ✅ 内置中间件
│       ├── body_parser.hpp        # ⏳ 请求体解析
│       ├── multipart.hpp          # ⏳ Multipart 解析
│       └── error.hpp              # ⏳ 错误定义
├── src/
│   ├── app.cpp                    # ✅
│   └── router.cpp                 # ✅
└── tests/
    ├── router_test.cpp            # ⏳
    ├── middleware_test.cpp        # ⏳
    ├── multipart_test.cpp         # ⏳
    └── integration_test.cpp       # ⏳
```

---

## 15. 编译和使用 ✅

### 15.1 CMake 配置

```cmake
# 启用 Layer 3
cmake -DASYNC_UV_ENABLE_LAYER3=ON ..

# 编译
make async_uv_layer3
```

### 15.2 依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| async_uv | 本项目 | 异步原语 |
| async_uv_http | 本项目 | HTTP 服务端 |
| reflect-cpp | v0.24.0 | JSON 序列化 |
| ada-url | v2.9.1 | URL 解析 |

### 15.3 链接

```cmake
target_link_libraries(your_app PRIVATE async_uv::layer3)
```

### 15.4 头文件

```cpp
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/middleware.hpp>
```

---

## 实现进度总览

| 模块 | 状态 | 完成度 |
|------|------|--------|
| Router (Radix Tree) | ✅ | 100% |
| Context | ✅ | 100% |
| App | ✅ | 100% |
| 洋葱模型 | ✅ | 100% |
| JSON 序列化 | ✅ | 100% |
| logger 中间件 | ✅ | 100% |
| error_handler 中间件 | ✅ | 100% |
| form_parser 中间件 | ⏳ | 0% |
| multipart_parser | ⏳ | 0% |
| CORS 中间件 | ⏳ | 0% |
| Rate limit 中间件 | ⏳ | 0% |
| 单元测试 | ⏳ | 0% |
| 集成测试 | ⏳ | 0% |
| 性能基准 | ⏳ | 0% |
| 错误码定义 | ⏳ | 0% |
| 路由组 | ⏳ | 0% |
| 生命周期钩子 | ⏳ | 0% |
| WebSocket | 📋 | 规划中 |