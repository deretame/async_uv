# Layer 3 设计文档 - 开发清单

> **状态说明**：
> - ✅ 已实现
> - ⏳ 待实现
> - 📋 设计完成，待编码
> - ❌ 不做（说明原因）

> **维护说明（2026-04-02）**：
> - 本文档定位为“设计规范 + 开发 TODO”，优先保证语义完整、一致、可执行。
> - 本文档只定义目标行为，不绑定某次具体实现细节。
> - 若实现与本文档冲突，应先更新本文档中的决策，再落地代码。
> - 文档当前版本：`v0.4`（本次修订补齐 `trace_id` 规范与重复路由注册约束）。

## 0. 设计审查结论与 TODO（文档维度）

### 0.1 已修订的关键规范

| 项目 | 当前结论 | 备注 |
|------|----------|------|
| 路由语法 | 统一使用 `{name}` / `{name*}` | 禁用混合写法 `/*` |
| Keep-Alive | 按 HTTP/1.0/1.1 语义分别处理 | 明确 `Connection` 规则 |
| 错误码体系 | 分层定义 HTTP 状态码与业务码 | 增加映射策略 |
| CORS | 增加 `Vary: Origin` 与凭证约束 | 避免缓存与安全歧义 |
| 示例一致性 | 修复签名与 API 用法冲突 | 示例可直接作为设计参考 |
| 错误可观测性 | 错误体字段契约覆盖 `request_id`/`trace_id` | 明确日志关联策略 |
| 路由冲突策略 | 同 method+pattern 重复注册视为配置错误 | 禁止隐式覆盖 |

### 0.2 后续 TODO（按优先级）

- [x] P1 补充 `RouteGroup` 与 `App::route` 组合使用的边界示例
- [x] P2 补充路径规范化规则的反例与边界样例
- [x] P2 为错误响应增加可选的追踪字段规范（如 `trace_id`）
- [ ] P3 定义错误标识注册表的“新增/废弃”流程（避免长期演进漂移）

### 0.3 术语与示例约定

- 本文档中的 C++ 片段默认是“规范示例”，用于定义行为，不保证可直接编译。
- 若示例中出现 `Task<void>`、`Middleware`、`Context` 等类型，语义以第 2 节定义为准。
- 未展开的辅助函数（如 `to_lower`、`join`）视为已存在工具函数，重点在行为约束。
- 规范强度词：
  - `必须`：实现必须满足
  - `建议`：默认推荐，若不采用应有明确理由
  - `可选`：按项目需求决定

---

## 目录

0. [设计审查结论与 TODO](#0-设计审查结论与-todo文档维度)
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
16. [附录：错误标识注册表](#16-附录错误标识注册表)

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
    App& router(std::string_view prefix, std::function<void(RouteGroup&)> builder); // DSL 写法（支持嵌套）
    
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

---

## 2.6 配置详解

### 2.6.1 ServerLimits

```cpp
struct ServerLimits {
    std::size_t max_header_count = 256;      // 最大 header 数量
    std::size_t max_header_bytes = 16 * 1024;  // 最大 header 总字节数 (16KB)
    std::size_t max_body_bytes = 8 * 1024 * 1024;  // 最大 body 字节数 (8MB)
    std::size_t max_target_bytes = 8 * 1024;  // 最大 URL 长度 (8KB)
    std::size_t max_method_bytes = 16;        // 最大 HTTP 方法长度
};
```

**使用**：

```cpp
app.with_limits({
    .max_body_bytes = 50 * 1024 * 1024,  // 50MB，适合文件上传
    .max_header_bytes = 32 * 1024,        // 32KB，支持大 cookie
});
```

### 2.6.2 ServerConnectionPolicy

```cpp
struct ServerConnectionPolicy {
    bool keep_alive_enabled = true;           // 是否启用 keep-alive
    std::size_t max_keep_alive_requests = 100; // 单连接最大请求数
    std::chrono::milliseconds read_timeout = std::chrono::seconds(15);   // 读超时
    std::chrono::milliseconds write_timeout = std::chrono::seconds(15);  // 写超时
    std::chrono::milliseconds idle_timeout = std::chrono::seconds(30);    // 空闲超时
};
```

**使用**：

```cpp
app.with_policy({
    .keep_alive_enabled = true,
    .max_keep_alive_requests = 1000,     // 每个连接最多 1000 请求
    .read_timeout = std::chrono::seconds(30),
    .write_timeout = std::chrono::seconds(30),
    .idle_timeout = std::chrono::seconds(60),
});
```

### 2.6.3 配置最佳实践

| 场景 | max_body_bytes | read_timeout | idle_timeout |
|------|-----------------|--------------|---------------|
| API 服务 | 1-10MB | 15s | 30s |
| 文件上传 | 50-100MB | 60s | 120s |
| 长轮询 | 1MB | 300s | 300s |
| WebSocket 升级 | 64KB | 300s | 0 (不超时) |

---

## 2.7 请求生命周期

### 2.7.1 完整流程图

```
TCP 连接建立
    │
    ▼
┌────────────────────────────────────────────────────────────────────┐
│                         请求读取阶段                                │
│  1. 读取 HTTP 请求行 (method, path, version)                       │
│  2. 读取 Headers (检查 max_header_count/bytes)                     │
│  3. 读取 Body (检查 max_body_bytes)                                │
│  4. 解析 URL (protocol://host:port/path?query#fragment)             │
│  5. 构建 ServerRequest 对象                                        │
└──────────────────────── ┬───────────────────────────────────────────┘
                          │
                          ▼
┌────────────────────────────────────────────────────────────────────┐
│                     中间件链执行阶段                                │
│                                                                    │
│  Context ctx(std::move(request))                                   │
│       │                                                            │
│       ▼                                                            │
│  ┌─────────────┐                                                   │
│  │ Middleware 1│ ────── 前置逻辑 ──────┐                           │
│  └─────────────┘                      │                            │
│                                      ▼                            │
│                           ┌─────────────┐                          │
│                           │ Middleware 2│ ── 前置逻辑 ─┐           │
│                           └─────────────┘            │           │
│                                                     ▼           │
│                                          ┌─────────────┐         │
│                                          │   Handler   │         │
│                                          └─────────────┘         │
│                                                     │           │
│                           ◄──── 后置逻辑 ◄─────────┘           │
│                           ┌─────────────┐                       │
│                           │ Middleware 2│ ◄─── 后置逻辑 ◄────────┤
│                           └─────────────┘                       │
│  ◄─────────────────────── 后置逻辑 ◄────────────────────────────┤
│  ┌─────────────┐                                                 │
│  │ Middleware 1│                                                 │
│  └─────────────┘                                                 │
└──────────────────────── ┬─────────────────────────────────────────┘
                          │
                          ▼
┌────────────────────────────────────────────────────────────────────┐
│                         响应发送阶段                                │
│  1. 序列化响应 (status line + headers + body)                      │
│  2. 写入 socket                                                   │
│  3. 检查 keep-alive                                                │
│     - 如果 keep-alive 且未超限 → 回到请求读取阶段                   │
│     - 否则 → 关闭连接                                              │
└────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
                    TCP 连接关闭
```

### 2.7.2 关键检查点

| 阶段 | 检查项 | 失败响应 |
|------|--------|---------|
| Header 读取 | `max_header_count`, `max_header_bytes` | 431 Request Header Fields Too Large |
| URL 解析 | `max_target_bytes` | 414 URI Too Long |
| Body 读取 | `max_body_bytes` | 413 Payload Too Large |
| 路由匹配 | 路由是否存在 | 404 Not Found |
| 方法检查 | 方法是否允许 | 405 Method Not Allowed（含 `Allow`） |
| 中间件 | 业务逻辑验证 | 自定义错误 |
| Handler | 未捕获异常 | 500 Internal Server Error |

### 2.7.3 连接状态管理

```cpp
struct ServerConnectionState {
    std::size_t handled_requests = 0;  // 已处理请求数
};

// Keep-alive 检查逻辑
bool should_keep_alive(
    const ServerRequest& request,      // 请求头 Connection
    const ServerResponse& response,    // 响应头 Connection
    const ServerConnectionPolicy& policy,  // keep_alive_enabled, max_keep_alive_requests
    const ServerConnectionState& state      // handled_requests
) {
    if (!policy.keep_alive_enabled) return false;
    if (!response.keep_alive) return false;
    if (state.handled_requests + 1 >= policy.max_keep_alive_requests) return false;

    auto conn = to_lower(request.header("Connection").value_or(""));
    // HTTP/1.1: 默认 keep-alive，除非显式 close
    if (request.http_major > 1 || (request.http_major == 1 && request.http_minor == 1)) {
        return conn != "close";
    }
    // HTTP/1.0: 默认 close，只有显式 keep-alive 才复用
    return conn == "keep-alive";
}
```

### 2.8 RouteGroup ✅

```cpp
// 路由组 - 对一组路由应用相同的中间件
class RouteGroup {
public:
    RouteGroup& use(Middleware middleware);
    RouteGroup& get(std::string_view pattern, Handler handler);
    RouteGroup& post(std::string_view pattern, Handler handler);
    // ...
    RouteGroup& router(std::string_view prefix, std::function<void(RouteGroup&)> builder); // 嵌套 DSL
    
    Router to_router() const;  // 仅导出路由定义，不携带组中间件
    void apply_to(App& app);   // 将组中间件 + 路由一起挂载到 App

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

api.apply_to(app);         // 推荐：保留组中间件语义
```

```cpp
// DSL 风格（支持嵌套）：/login 不鉴权，其它路由鉴权
app.router("/api", [](RouteGroup& r) {
    r.post("/login", login_handler);  // 不加 auth

    r.router("", [](RouteGroup& protected_routes) {
        protected_routes.use(auth_middleware); // 仅对子作用域生效
        protected_routes.get("/profile", profile_handler);

        protected_routes.router("/admin", [](RouteGroup& admin) {
            admin.use(admin_only_middleware);
            admin.get("/users", list_users);
        });
    });
});
```

### 2.8.1 `RouteGroup` 与 `App::route` 组合边界 ✅

为避免重复挂载和中间件语义歧义，约定如下：

1. `RouteGroup::apply_to(app)` 为首选，适用于“组中间件 + 路由”整体挂载。
2. `RouteGroup::to_router()` 仅导出路由定义，不包含组中间件。
3. 禁止同一组路由同时 `apply_to(app)` 且再通过 `app.route(prefix, group.to_router())` 二次挂载。
4. 若必须组合 `App::route`，应显式说明该分支不需要组中间件。

反例（禁止）：

```cpp
auto admin = RouteGroup("/admin");
admin.use(auth_middleware);
admin.get("/users", list_users);

admin.apply_to(app);
app.route("/admin", std::move(admin).to_router()); // 禁止：重复挂载，语义冲突
```

### 2.9 Lifecycle ✅（基础版）

```cpp
// 生命周期钩子
struct Lifecycle {
    std::function<Task<void>()> on_start;      // 服务器启动时
    std::function<Task<void>()> on_stop;       // 服务器停止时
    std::function<void(Context&)> on_error;    // 错误处理
};

App& with_lifecycle(Lifecycle lifecycle);
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

### 3.2.1 路径规范化规则 ✅

为保证路由可预测性，匹配前统一执行以下规范化：

1. 去掉 query 与 fragment，仅用 path 参与匹配
2. 合并连续 `/` 为单个 `/`
3. 去掉尾部 `/`（根路径 `/` 除外）
4. 不做大小写折叠（路径默认大小写敏感）
5. URL decode 仅对参数值生效，不对路由模板做 decode

### 3.2.2 规范化示例 ✅

| 原始请求目标 | 参与匹配的 path |
|-------------|-----------------|
| `/users?id=1` | `/users` |
| `/users///123/` | `/users/123` |
| `/Users/123` | `/Users/123`（大小写保留） |
| `/files/a%2Fb` | `/files/a%2Fb`（模板不 decode） |

### 3.2.3 反例与边界样例 ✅

| 场景 | 输入 | 期望 |
|------|------|------|
| 空 path | `` | 规范化为 `/` |
| 仅 query | `?a=1` | 规范化为 `/` |
| 多个尾部 `/` | `/users///` | 规范化为 `/users` |
| fragment-only | `#part` | 规范化为 `/` |
| 编码斜杠参数 | `/files/a%2Fb` + `/files/{path*}` | `path` 值保持 `a%2Fb`，不在模板阶段 decode |

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
  GET /users/{id}/{rest*} (PARAM + WILDCARD)

请求匹配：
  /users/profile      → STATIC
  /users/123          → PARAM
  /users/123/posts    → PARAM + STATIC
  /users/123/extra    → PARAM + WILDCARD
```

### 3.3.1 重复注册约束 ✅

为避免运行期出现“最后一次注册覆盖前一次注册”的隐式行为，约定如下：

1. 路由唯一键定义为：`(method, normalized_pattern)`。
2. 同一 `Router` 内注册到相同唯一键时，`必须`判定为配置错误（抛异常或返回显式错误），禁止静默覆盖。
3. 规范化后等价的路径也视为冲突（如 `/users/` 与 `/users`）。
4. `all()` 语义若展开为多个方法，`必须`逐个方法执行冲突检查。
5. 错误信息`建议`包含冲突的 method/pattern，便于启动期快速定位。

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

### 3.5 路由附加能力 ✅（基础版）

#### 3.5.1 路由元数据 ✅

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

#### 3.5.2 路由列表 ✅

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

#### 3.5.3 方法检查 ✅

```cpp
// 检查某路径支持哪些方法
std::vector<std::string> Router::allowed_methods(std::string_view path) const;

// 用于 405 Method Not Allowed 响应
// 约定：
// 1. 返回值需去重并按字典序稳定输出
// 2. 若存在 GET，建议自动包含 HEAD
// 3. 生成 405 时必须附带 Allow 头
// 4. 对于 OPTIONS，请优先返回能力探测（200/204）而不是 405
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

### 5.3 Form 解析 ✅

#### 5.3.1 application/x-www-form-urlencoded ✅

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

#### 5.3.2 multipart/form-data ✅

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

### 5.4 请求体大小限制 ✅

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

### 6.2 已有响应方法 ✅

```cpp
// Context 中的响应方法
void status(int code);                              // 设置状态码
void set(std::string_view name, std::string_view value);  // 设置头
void json(const T& obj);                            // JSON 响应
void json_raw(std::string body);                    // 原始 JSON 字符串
void send(std::string body);                        // 纯文本响应
```

### 6.3 待添加的响应方法 ⏳

#### 6.3.1 重定向 ✅

```cpp
// 重定向
void redirect(std::string_view url, int code = 302);
void redirect_permanent(std::string_view url);  // 301
void redirect_temporary(std::string_view url);  // 307
```

#### 6.3.2 文件下载 ✅

```cpp
// 发送文件
void send_file(std::filesystem::path path);
void download(std::filesystem::path path, std::string_view filename);

// 设置 Content-Disposition: attachment; filename="..."
```

#### 6.3.3 流式响应 ✅

```cpp
// 流式响应（用于大文件）
Task<void> stream(Context& ctx, std::function<Task<void>(std::function<Task<void>(std::string_view)>)> generator);

// 示例：流式返回日志
app.get("/logs", [](Context& ctx) -> Task<void> {
    co_await ctx.stream([](auto write) -> Task<void> {
        for (int i = 0; i < 100; ++i) {
            co_await write("log line " + std::to_string(i) + "\n");
        }
    });
});
```

### 6.4 Content-Type 自动设置 ✅（文件响应）

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
404 Not Found               - 路由不匹配
405 Method Not Allowed      - 路由存在但方法不允许（必须返回 Allow 头）
413 Payload Too Large       - 请求体过大
415 Unsupported Media Type  - Content-Type 不支持
500 Internal Server Error   - 内部错误
```

### 7.2 错误响应格式 ✅

```json
{
    "status": 404,
    "error": "Not Found",
    "code": "ROUTE_NOT_FOUND",
    "biz_code": null,
    "path": "/users/123",
    "method": "GET",
    "request_id": "req-abc123",
    "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736"
}
```

字段契约（规范）：

| 字段 | 类型 | 是否必须 | 约束 |
|------|------|----------|------|
| `status` | number | 必须 | HTTP 状态码 |
| `error` | string | 必须 | 面向调用方的简短错误描述 |
| `code` | string | 必须 | 机器可读稳定标识，推荐全大写下划线 |
| `biz_code` | number \| null | 必须 | 业务码；无业务码时必须为 `null` |
| `path` | string \| null | 建议 | 参与路由匹配的请求 path |
| `method` | string \| null | 建议 | 请求方法（`GET`/`POST`...） |
| `request_id` | string \| null | 建议 | 单次请求标识，用于日志关联 |
| `trace_id` | string \| null | 可选 | 分布式链路标识；无链路追踪时为 `null` |

补充约定：

1. 对外错误响应`必须`至少包含：`status`、`error`、`code`、`biz_code`。
2. 若上下文可获得 `request_id`/`trace_id`，`建议`始终回传，便于跨系统排障。
3. `request_id` 与 `trace_id` 可以相同，也可以独立；两者语义不同，不应混用命名。
4. `trace_id` 格式`建议`兼容 W3C Trace Context 的 32 位十六进制表示。

### 7.3 自定义错误处理 ✅

```cpp
// 自定义错误处理中间件
app.use([](Context& ctx, Next next) -> Task<void> {
    // 语义化伪接口：ctx.get_local_or_null("k") 表示“存在则取值，不存在返回 null”
    auto write_error = [&](int status,
                           std::string_view message,
                           std::string_view code,
                           std::optional<int> biz_code = std::nullopt) {
        if (biz_code) {
            ctx.status(status);
            ctx.json({
                {"status", status},
                {"error", message},
                {"code", code},
                {"biz_code", *biz_code},
                {"path", ctx.path},
                {"method", ctx.method},
                {"request_id", ctx.get_local_or_null("request_id")},
                {"trace_id", ctx.get_local_or_null("trace_id")}
            });
        } else {
            ctx.status(status);
            ctx.json({
                {"status", status},
                {"error", message},
                {"code", code},
                {"biz_code", nullptr},
                {"path", ctx.path},
                {"method", ctx.method},
                {"request_id", ctx.get_local_or_null("request_id")},
                {"trace_id", ctx.get_local_or_null("trace_id")}
            });
        }
    };

    try {
        co_await next();
    } catch (const AppErrorException& e) {
        write_error(
            static_cast<int>(e.error.http_error),
            e.error.message,
            e.error.code,
            e.error.biz_error ? std::optional<int>(static_cast<int>(*e.error.biz_error))
                              : std::nullopt);
    } catch (const ValidationError& e) {
        write_error(400, e.what(), "INVALID_FIELD", 10003);
    } catch (const AuthError& e) {
        write_error(401, e.what(), "INVALID_TOKEN", 11001);
    } catch (const std::exception& e) {
        write_error(500, "Internal Server Error", "INTERNAL_ERROR");
        // 记录日志
    }
});
```

### 7.4 错误码定义 ✅

```cpp
// error_codes.hpp
namespace async_uv::layer3 {

// 第一层：HTTP 状态码（协议语义）
enum class HttpError : int {
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    PayloadTooLarge = 413,
    UnsupportedMediaType = 415,
    TooManyRequests = 429,
    InternalServerError = 500,
};

// 第二层：业务错误码（应用语义）
enum class BizError : int {
    InvalidJson = 10001,
    MissingField = 10002,
    InvalidField = 10003,
    InvalidToken = 11001,
    TokenExpired = 11002,
    ResourceNotFound = 14001,
    DatabaseError = 15001,
    ExternalServiceError = 15002,
};

struct AppError {
    HttpError http_error;
    std::string code;
    std::optional<BizError> biz_error;
    std::string message;
    std::optional<std::string> detail;
    
    // 统一错误输出
    void write(Context& ctx) const;
};

struct AppErrorException : std::runtime_error {
    AppError error;
};

} // namespace async_uv::layer3
```

### 7.5 错误映射策略 ✅

| 场景 | HTTP 状态码 | 业务码 |
|------|-------------|--------|
| 路由不存在 | `404` | 可空（`code=ROUTE_NOT_FOUND`） |
| 方法不允许 | `405` | 可空（`code=METHOD_NOT_ALLOWED`） |
| JSON 解析失败 | `400` | `InvalidJson`（`code=INVALID_JSON`） |
| 认证失败 | `401` | `InvalidToken` / `TokenExpired` |
| 参数校验失败 | `400` | `MissingField` / `InvalidField` |
| 未知异常 | `500` | 可空（`code=INTERNAL_ERROR`） |

### 7.6 405 响应规范 ✅

当路径存在但方法不允许时，响应必须满足：

1. 状态码为 `405 Method Not Allowed`
2. 必须包含 `Allow` 头，值为该路径允许的方法集合（逗号分隔）
3. 错误体沿用统一错误结构（至少包含 `status`、`error`、`code`、`biz_code`）
4. 若上下文有 `request_id`/`trace_id`，建议一并返回

示例：

```http
HTTP/1.1 405 Method Not Allowed
Allow: GET, HEAD, POST
Content-Type: application/json

{"status":405,"error":"Method Not Allowed","code":"METHOD_NOT_ALLOWED","biz_code":null}
```

---

## 8. 内置中间件

### 8.1 已实现 ✅

| 中间件 | 功能 | 状态 |
|--------|------|------|
| `logger()` | 请求日志 | ✅ |
| `error_handler()` | 错误捕获 | ✅ |

### 8.2 实现状态更新 ✅

| 中间件 | 功能 | 设计状态 | 优先级 |
|--------|------|----------|--------|
| `cors()` | CORS 处理 | ✅ | 高 |
| `body_limit(size)` | 请求体大小限制 | ✅ | 高 |
| `form_parser()` | 表单解析 | ✅ | 中 |
| `multipart_parser()` | 文件上传解析 | ✅ | 中 |
| `rate_limit()` | 限流 | ✅ | 中 |
| `compress()` | 响应压缩 | ✅ | 低 |
| `etag()` | ETag 缓存 | ✅ | 低 |
| `static_files()` | 静态文件服务 | ❌ (交给 Nginx) | 低 |

### 8.3 CORS 中间件 ✅

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
        
        // CORS 缓存安全：允许跨域值依赖 Origin 时必须带 Vary
        ctx.set("Vary", "Origin");

        // 当 allow_credentials=true 时，不能返回 *
        if (opts.allow_credentials) {
            ctx.set("Access-Control-Allow-Origin", *origin);
            ctx.set("Access-Control-Allow-Credentials", "true");
        } else {
            ctx.set("Access-Control-Allow-Origin",
                (opts.allow_origins.size() == 1 && opts.allow_origins[0] == "*") ? "*" : *origin);
        }
        
        // 预检请求
        if (ctx.method == "OPTIONS") {
            ctx.set("Vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");
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

**约束**：
- `allow_credentials=true` 时，`Access-Control-Allow-Origin` 不能为 `*`
- 建议始终设置 `Vary: Origin`，预检请求再追加方法/头相关 `Vary`
- 预检响应建议使用 `204 No Content`

### 8.4 Rate Limit 中间件 ✅

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
    .on_limited = [](Context& ctx) -> Task<void> {
        ctx.status(429);
        ctx.json({{"error", "Too Many Requests"}});
        co_return;
    }
}));
```

### 8.5 Compression 中间件 ✅（gzip）

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

### 9.1 HTTPS 和 HTTP/2 ❌

**不做原因**：交给 Nginx 处理

```
Nginx 负责终结：
- HTTPS 加密
- HTTP/2 支持
- 静态文件服务
- 负载均衡
- 限流

Layer 3 只负责：
- 路由
- 中间件
- 业务逻辑

架构：
浏览器 → Nginx (HTTPS/HTTP/2) → 反向代理 → Layer 3 (HTTP)
```

### 9.2 输入验证 ✅（基础版）

```cpp
// 验证中间件（两种重载）
template<typename T>
Middleware validate(std::function<bool(const T&)> validator);

template<typename T>
Middleware validate();  // 使用 reflect-cpp 元数据校验

// 使用
app.post("/users", 
    validate<UserCreateRequest>([](const auto& req) {
        return !req.name.empty() && req.name.size() <= 100;
    }),
    [](Context& ctx) -> Task<void> {
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

### 9.5 安全头 ✅

```cpp
// 安全头中间件
Middleware security_headers() {
    return [](Context& ctx, Next next) -> Task<void> {
        ctx.set("X-Content-Type-Options", "nosniff");
        ctx.set("X-Frame-Options", "DENY");
        ctx.set("Referrer-Policy", "strict-origin-when-cross-origin");
        ctx.set("Content-Security-Policy", "default-src 'self'");
        ctx.set("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
        // X-XSS-Protection 已过时，不再作为默认安全头
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

> 说明：以下测试片段使用 Catch2 风格（`TEST_CASE` / `REQUIRE`）描述预期行为。

### 11.1 单元测试 ✅

```cpp
// tests/router_test.cpp
TEST_CASE("Router static path") {
    Router router;
    int called = 0;
    router.get("/users", [&](Context& ctx) -> Task<void> {
        called++;
        co_return;
    });
    
    auto result = router.match("GET", "/users");
    REQUIRE(result.has_value());
    REQUIRE(called == 0);  // 还没调用
}

TEST_CASE("Router param extraction") {
    Router router;
    router.get("/users/{id}", [](Context& ctx) -> Task<void> { co_return; });
    
    auto result = router.match("GET", "/users/123");
    REQUIRE(result.has_value());
    REQUIRE(result->params["id"] == "123");
}

TEST_CASE("Router wildcard") {
    Router router;
    router.get("/files/{path*}", [](Context& ctx) -> Task<void> { co_return; });
    
    auto result = router.match("GET", "/files/a/b/c.txt");
    REQUIRE(result.has_value());
    REQUIRE(result->params["path"] == "a/b/c.txt");
}
```

### 11.2 中间件测试 ✅

```cpp
// tests/middleware_test.cpp
TEST_CASE("Middleware onion model") {
    std::vector<int> order;
    
    auto app = App();
    app.use([&](Context& ctx, Next next) -> Task<void> {
        order.push_back(1);
        co_await next();
        order.push_back(3);
    });
    app.use([&](Context& ctx, Next next) -> Task<void> {
        order.push_back(2);
        co_await next();
        order.push_back(4);
    });
    app.get("/", [&](Context& ctx) -> Task<void> {
        order.push_back(5);
        co_return;
    });
    
    // 执行请求...
    REQUIRE(order == std::vector<int>{1, 2, 5, 4, 3});
}
```

### 11.3 集成测试 ✅

```cpp
// tests/integration_test.cpp
TEST_CASE("Full request cycle") {
    auto app = App();
    app.get("/hello", [](Context& ctx) -> Task<void> {
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
        router.get("/users/" + std::to_string(i), [](Context&) -> Task<void> { co_return; });
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
    app.get("/users", [](Context& ctx) -> Task<void> {
        std::vector<User> users = co_await db.get_all_users();
        ctx.json(users);
        co_return;
    });
    
    app.get("/users/{id}", [](Context& ctx) -> Task<void> {
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
    
    app.post("/users", [](Context& ctx) -> Task<void> {
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
    
    app.put("/users/{id}", [](Context& ctx) -> Task<void> {
        auto id = ctx.param("id");
        auto user = ctx.json_as<User>();
        co_await db.update_user(id, *user);
        ctx.json(*user);
        co_return;
    });
    
    app.del("/users/{id}", [](Context& ctx) -> Task<void> {
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
Middleware require_role(std::string role) {
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
RouteGroup admin("/admin");
admin.use(require_auth);
admin.use(require_role("admin"));
admin.del("/users/{id}", [](Context& ctx) -> Task<void> {
    // 只有 admin 可以删除用户
});
admin.apply_to(app);
```

### 12.3 文件上传 ⏳

```cpp
app.post("/upload", 
    async_uv::layer3::middleware::multipart_parser(),
    [](Context& ctx) -> Task<void> {
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
app.websocket("/ws", [](WebSocket& ws) -> Task<void> {
    ws.on_message([](std::string_view msg) -> Task<void> {
        co_await ws.send("Echo: " + std::string(msg));
    });
    
    ws.on_close([]() -> Task<void> {
        std::cout << "Client disconnected\n";
        co_return;
    });
    
    co_await ws.run();
});
```

---

## 13. 未来扩展

### 13.1 WebSocket 支持 📋

```cpp
class WebSocket {
public:
    Task<void> send(std::string_view data);
    Task<std::optional<std::string>> receive();
    void on_message(std::function<Task<void>(std::string_view)> handler);
    void on_close(std::function<Task<void>()> handler);
};

// 使用
app.websocket("/ws", [](WebSocket& ws) -> Task<void> {
    ws.on_message([&](std::string_view msg) -> Task<void> {
        co_await ws.send("Echo: " + std::string(msg));
    });
});
```

**实现要点**：
- HTTP 升级协议处理
- Frame 解析（PING/PONG/CLOSE/TEXT/BINARY）
- 连接状态管理
- 与现有中间件链集成

### 13.2 OpenAPI 文档生成 ⏳

```cpp
// 从路由自动生成 OpenAPI 文档
auto openapi = app.generate_openapi({
    .title = "My API",
    .version = "1.0.0",
    .description = "API 文档"
});

// 暴露文档端点
app.get("/openapi.json", [&](Context& ctx) -> Task<void> {
    ctx.json(openapi);
    co_return;
});
```

**生成内容**：
- 路径和操作列表
- 参数定义（path/query/header/body）
- 响应模式
- 从 `reflect-cpp` 类型自动推导 schema

### 13.3 请求验证（reflect-cpp 版本）⏳

```cpp
// 类型 + 约束验证
struct UserCreate {
    rfl::Field<"name", std::string> name;        // 非空
    rfl::Field<"email", std::string> email;       // 邮箱格式
    rfl::Field<"age", int> age;                   // > 0
};

// 验证中间件（对应 9.2 的无参重载）
template<typename T>
Middleware validate() {
    return [](Context& ctx, Next next) -> Task<void> {
        auto obj = ctx.json_as<T>();
        if (!obj) {
            ctx.status(400);
            ctx.json({{"error", "Invalid request body"}});
            co_return;
        }
        
        // 使用 reflect-cpp 验证
        auto errors = rfl::validate(*obj);
        if (errors) {
            ctx.status(400);
            ctx.json({{"errors", *errors}});
            co_return;
        }
        
        ctx.set_local("validated", std::move(*obj));
        co_await next();
    };
}

// 使用
app.post("/users", validate<UserCreate>(), [](Context& ctx) -> Task<void> {
    auto user = ctx.local<UserCreate>("validated");
    if (!user) {
        ctx.status(500);
        ctx.json({{"error", "validated payload missing"}});
        co_return;
    }
    // ...
});
```

### 13.4 依赖注入 ✅（增强版）

```cpp
// 简易 DI 容器（基于 reflect-cpp 字段反射）
class Container {
public:
    template<typename T> Container& singleton(std::shared_ptr<T> instance);
    template<typename T> Container& singleton(std::string key, std::shared_ptr<T> instance);
    template<typename T, typename Factory> Container& singleton(Factory factory);
    template<typename T, typename Factory> Container& singleton(std::string key, Factory factory);
    template<typename T, typename Factory> Container& transient(Factory factory);
    template<typename T, typename Factory> Container& transient(std::string key, Factory factory);

    template<typename T> std::shared_ptr<T> resolve_shared();
    template<typename T> std::shared_ptr<T> resolve_shared(std::string_view key);
    template<typename T> T& resolve();
    template<typename T> T& resolve(std::string_view key);
    template<typename T> void inject(T& object);
    template<typename T> void inject(T& object, std::string_view key);
};

struct ControllerMountOptions {
    std::string prefix; // 可选：覆盖控制器默认前缀
    std::string key;    // 可选：指定命名控制器实例
};

class RouteBinder {
public:
    template<typename Controller>
    RouteBinder& mount_controller(ControllerMountOptions options = {});
};

template<typename Controller>
RouteBinder mount_controller(App& app, Container& di, ControllerMountOptions options = {});

template<typename Controller>
RouteBinder mount_controller(RouteGroup& group, Container& di, ControllerMountOptions options = {});

// 用法：通过反射把字段依赖自动装配
struct Logger {
    int level = 0;
};

struct Repo {
    std::shared_ptr<Logger> logger; // 自动注入
};

struct Service {
    std::shared_ptr<Logger> logger; // 自动注入
    Repo repo;                      // 自动注入（值类型）
};

async_uv::layer3::di::Container di;
di.singleton<Logger>([] {
    auto logger = std::make_shared<Logger>();
    logger->level = 7;
    return logger;
});

auto service = di.resolve_shared<Service>();

// 路由封装：控制器方法直接绑定（无需手写 resolve）
struct UserController {
    std::shared_ptr<Logger> logger;
    Task<void> profile(Context& ctx);
};

App app;
di::bind_routes(app, di)
    .router("/api", [](di::RouteBinder& r) {
        r.get<UserController>("/profile", &UserController::profile);
    });

// 路由封装：按 key 选择命名控制器
di::bind_routes(app, di)
    .with_key("auth")
    .get<UserController>("/auth/profile", &UserController::profile);

// 方式 1：控制器自描述挂载（推荐）
struct AuthController {
    static constexpr std::string_view prefix = "/auth";

    static void map(di::ControllerBinder<AuthController>& r) {
        r.post("/login", &AuthController::login);
        r.router("/v1", [](di::ControllerBinder<AuthController>& v1) {
            v1.get("/profile", &AuthController::profile);
        });
    }
};

di::mount_controller<AuthController>(app, di);
// 实际挂载：
// POST /auth/login
// GET  /auth/v1/profile

// 方式 2：显式覆盖前缀 + 指定 key（如多租户/多角色控制器）
di::mount_controller<AuthController>(app, di, {
    .prefix = "/api/secure/auth",
    .key = "tenant-a"
});

// 方式 3：兼容旧映射签名（无需立即迁移）
struct LegacyController {
    static void map(di::RouteBinder& r) {
        r.get<LegacyController>("/ping", &LegacyController::ping);
    }
};
di::mount_controller<LegacyController>(app, di, {.prefix = "/legacy"});
```

当前实现约束：

1. 默认按 `singleton` 自动构造未注册类型（要求可默认构造）。
2. 自动注入优先支持 `std::shared_ptr<T>`、`T*`、聚合值类型字段。
3. 检测循环依赖并抛出错误，避免递归构造导致栈溢出。
4. 非聚合类型（如显式构造函数复杂对象）建议显式注册 `singleton/transient` 工厂。
5. 容器内部使用互斥锁保护注册与解析，支持并发场景下的基本安全访问。
6. 支持字符串 key 的命名绑定，可管理同类型多实例（如 `singleton<RedisClient>("main", ...)` 与 `singleton<RedisClient>("cache", ...)`）。
7. `RouteBinder::with_key("...")` 可让路由按命名实例解析控制器；未命中 key 时回退默认绑定。
8. `inject(obj, key)` 支持按 key 注入字段依赖，未命中 key 时回退默认绑定。
9. 已补充并发压力测试（多线程重复 `resolve_shared`）验证单例稳定性。
10. `mount_controller` 的前缀解析优先级：`options.prefix` > `Controller::prefix` > 当前挂载点（空前缀）。
11. `mount_controller` 的 key 行为：指定 `options.key` 时优先解析命名绑定，未命中时回退默认绑定。
12. 控制器映射函数支持两种签名：`static map(di::ControllerBinder<Controller>&)`（推荐）与 `static map(di::RouteBinder&)`（兼容）。
13. `ControllerBinder<Controller>` 固定控制器类型后可省略重复模板参数，适合嵌套路由声明。

示例（命名绑定）：

```cpp
di::Container c;
c.singleton<Logger>("auth", [] { return std::make_shared<Logger>(); });
c.singleton<Logger>("admin", [] { return std::make_shared<Logger>(); });

auto auth_logger = c.resolve_shared<Logger>("auth");
auto admin_logger = c.resolve_shared<Logger>("admin");
```

示例（全局 / 线程局部容器）：

```cpp
auto& global = di::global_di();         // 进程级唯一容器
auto& local = di::thread_local_di();    // 线程局部容器
```

### 13.5 生命周期钩子 ✅（基础版）

```cpp
// 生命周期结构定义与接口见 2.9

// 使用
app.with_lifecycle({
    .on_start = []() -> Task<void> {
        std::cout << "Server started\n";
        co_return;
    },
    .on_stop = []() -> Task<void> {
        std::cout << "Server shutting down\n";
        co_return;
    }
});
```

### 13.6 路由元数据 ✅

```cpp
// 每个路由附加元数据（用于 OpenAPI、权限等）
app.get("/users/{id}", get_user_handler, {
    .name = "getUser",
    .description = "Get user by ID",
    .tags = {"users"},
    .require_auth = true
});
```

### 13.7 不做的功能 ❌

| 功能 | 原因 | 替代方案 |
|------|------|---------|
| HTTPS | Nginx 更专业 | Nginx 终结 TLS |
| HTTP/2 服务端 | Nginx 已支持 | Nginx 反向代理 |
| 静态文件 | Nginx 性能更好 | Nginx `location /static` |
| 负载均衡 | Nginx/云服务更好 | Nginx upstream |
| 客户端请求 | libcurl 已满足 | Layer 2 HTTP Client |

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
│       ├── di.hpp                 # ✅ 简易依赖注入容器
│       ├── body_limit.hpp         # ✅ 请求体大小限制
│       ├── multipart_parser.hpp   # ✅ Multipart 解析
│       └── error.hpp              # ✅ 错误定义
├── src/
│   ├── app.cpp                    # ✅
│   └── router.cpp                 # ✅
└── tests/
    ├── router_test.cpp            # ✅
    ├── form_parser_test.cpp       # ✅
    ├── multipart_parser_test.cpp  # ✅
    ├── integration_test.cpp       # ✅
    └── middleware_test.cpp        # ⏳（计划补充）
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
#include <async_uv_layer3/di.hpp>
```

---

## 16. 附录：错误标识注册表

> 用于统一 `code` 字段命名，避免不同模块重复定义或语义漂移。

| code | HTTP 状态码 | biz_code | 含义 |
|------|-------------|----------|------|
| `ROUTE_NOT_FOUND` | 404 | `null` | 路由不存在 |
| `METHOD_NOT_ALLOWED` | 405 | `null` | 路径存在但方法不允许 |
| `INVALID_JSON` | 400 | `10001` | JSON 解析失败 |
| `MISSING_FIELD` | 400 | `10002` | 缺少必填字段 |
| `INVALID_FIELD` | 400 | `10003` | 字段格式或约束不合法 |
| `INVALID_TOKEN` | 401 | `11001` | 鉴权令牌无效 |
| `TOKEN_EXPIRED` | 401 | `11002` | 鉴权令牌已过期 |
| `RESOURCE_NOT_FOUND` | 404 | `14001` | 业务资源不存在 |
| `DATABASE_ERROR` | 500 | `15001` | 数据库异常 |
| `EXTERNAL_SERVICE_ERROR` | 500 | `15002` | 外部依赖服务异常 |
| `INTERNAL_ERROR` | 500 | `null` | 未分类的内部错误 |

---

## 设计进度总览（文档视角）

| 模块 | 状态 | 完成度 |
|------|------|--------|
| Router (Radix Tree) | ✅ | 核心规范已定义 |
| Context | ✅ | 核心规范已定义 |
| App | ✅ | 核心规范已定义 |
| 洋葱模型 | ✅ | 规范已定义 |
| JSON 序列化 | ✅ | 规范已定义 |
| logger 中间件 | ✅ | 规范已定义 |
| error_handler 中间件 | ✅ | 规范已定义（含 `request_id`/`trace_id` 约束） |
| form_parser 中间件 | ✅ | 规范已定义 |
| multipart_parser | ✅ | 规范已定义 |
| CORS 中间件 | ✅ | 规范已定义（含缓存与凭证约束） |
| Rate limit 中间件 | ✅ | 规范已定义 |
| 单元测试 | ✅ | 核心测试策略已定义 |
| 集成测试 | ✅ | 关键链路测试策略已定义 |
| 性能基准 | ⏳ | 0% |
| 错误码定义 | ✅ | 分层模型已定义 |
| 路由组 | ✅ | 核心语义与组合边界已定义 |
| 依赖注入 | ✅ | 基础容器已接入（singleton/transient/自动注入/循环检测） |
| 生命周期钩子 | ✅ | 基础钩子已接入（`on_start`/`on_stop`/`on_error`） |
| WebSocket | 📋 | 规划中 |
