# Layer 3 设计文档

## 1. 概述

Layer 3 是基于 Layer 2 的 HTTP 服务端框架，提供：

- **Radix Tree 路由器** - 高效的 URL 路径匹配，支持参数提取
- **洋葱模型中间件** - 灵活的前置/后置处理链
- **Context 增强** - 请求上下文、JSON 序列化等便捷方法

### 依赖关系

```
Layer 3 (async_uv_layer3)
├── Layer 2 (async_uv_http) - HTTP 服务端基础
│   └── Layer 1 (async_uv) - 异步原语
└── reflect-cpp v0.24.0 - JSON 序列化
```

## 2. 核心类型定义

### 2.1 Context

```cpp
// context.hpp
namespace async_uv::layer3 {

struct Context : http::ServerRequest {
    http::ServerResponse response;
    
    // 路径参数，如 /users/{id}
    std::map<std::string, std::string> params;
    
    // 中间件间共享数据
    std::map<std::string, std::any> locals;
    
    // 从 ServerRequest 移动构造
    explicit Context(http::ServerRequest&& req);
    
    // 便捷方法
    std::string_view param(std::string_view name) const;
    std::optional<std::string> local(std::string_view name) const;
    
    // JSON 操作
    template<typename T>
    T json_as() const;
    
    template<typename T>
    void json(const T& body);
    
    void json_raw(std::string body);
    
    // 响应设置
    void status(int code);
    void set(std::string_view name, std::string_view value);
};

} // namespace async_uv::layer3
```

### 2.2 中间件

```cpp
// middleware.hpp
namespace async_uv::layer3 {

// Next: 调用下一个中间件
using Next = std::function<Task<void>()>;

// 中间件签名
using Middleware = std::function<Task<void>(Context& ctx, Next next)>;

// 路由处理函数
using Handler = std::function<Task<void>(Context& ctx)>;

} // namespace async_uv::layer3
```

### 2.3 Router

```cpp
// router.hpp
namespace async_uv::layer3 {

class Router {
public:
    // 注册路由
    Router& get(std::string_view pattern, Handler handler);
    Router& post(std::string_view pattern, Handler handler);
    Router& put(std::string_view pattern, Handler handler);
    Router& del(std::string_view pattern, Handler handler);
    Router& all(std::string_view pattern, Handler handler);
    
    // 匹配路由
    struct MatchResult {
        Handler handler;
        std::map<std::string, std::string> params;
    };
    
    std::optional<MatchResult> match(std::string_view method, 
                                      std::string_view path) const;
    
    // 子路由挂载
    Router& route(std::string_view prefix, Router sub_router);

private:
    struct RouteNode;
    std::unique_ptr<RouteNode> root_;
    std::vector<std::pair<std::string, Router>> sub_routers_;
};

} // namespace async_uv::layer3
```

### 2.4 App

```cpp
// app.hpp
namespace async_uv::layer3 {

class App {
public:
    // 中间件
    App& use(Middleware middleware);
    
    // 路由便捷方法
    App& get(std::string_view pattern, Handler handler);
    App& post(std::string_view pattern, Handler handler);
    App& put(std::string_view pattern, Handler handler);
    App& del(std::string_view pattern, Handler handler);
    
    // 挂载子路由
    App& route(std::string_view prefix, Router router);
    
    // 启动服务
    Task<void> listen(uint16_t port, std::string_view host = "0.0.0.0");
    
    // 配置
    App& with_limits(http::ServerLimits limits);
    App& with_policy(http::ServerConnectionPolicy policy);

private:
    std::vector<Middleware> middlewares_;
    Router router_;
    http::ServerLimits limits_;
    http::ServerConnectionPolicy policy_;
};

} // namespace async_uv::layer3
```

## 3. Radix Tree 路由匹配

### 3.1 数据结构

```cpp
struct RouteNode {
    std::string prefix;                    // 公共前缀
    std::string param_name;                // 参数名，如 "id"
    std::vector<std::unique_ptr<RouteNode>> children;
    
    // 方法 -> Handler 映射
    std::unordered_map<std::string, Handler> handlers;
    
    // 节点类型
    enum class Type { STATIC, PARAM, WILDCARD } type = Type::STATIC;
};
```

### 3.2 路径模式语法

| 语法 | 类型 | 示例 |
|------|------|------|
| `/users/{id}` | 参数 | 匹配 `/users/123`，提取 `id=123` |
| `/files/{path*}` | 通配符 | 匹配 `/files/a/b/c`，提取 `path=a/b/c` |
| `/health` | 静态 | 精确匹配 |

### 3.3 匹配优先级

1. 静态节点 → 2. 参数节点 → 3. 通配符节点

### 3.4 示例路由树

```
注册路由:
  GET /users
  GET /users/{id}
  POST /users
  GET /users/{id}/posts
  GET /users/{id}/posts/{post_id}

树结构:
  /users
  ├── [GET, POST handlers]
  └── /{id}
      ├── [GET handler]
      └── /posts
          ├── [GET handler]
          └── /{post_id}
              └── [GET handler]
```

## 4. 洋葱模型中间件

### 4.1 执行流程

```
请求 → [middleware1] → [middleware2] → [handler] → [middleware2] → [middleware1] → 响应
         ↓                 ↓               ↑                ↑                 ↑
       前置逻辑          前置逻辑        后置逻辑         后置逻辑          后置逻辑
                        co_await next()
```

### 4.2 示例

```cpp
// 日志中间件
Task<void> logger(Context& ctx, Next next) {
    auto start = std::chrono::steady_clock::now();
    std::cout << "-> " << ctx.method << " " << ctx.target.path << "\n";
    
    co_await next();  // 执行后续中间件和 handler
    
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "<- " << ctx.response.status_code << " (" << ms << "ms)\n";
}

// 错误处理中间件
Task<void> error_handler(Context& ctx, Next next) {
    try {
        co_await next();
    } catch (const std::exception& e) {
        ctx.status(500);
        ctx.json({{"error", e.what()}});
    }
}

// JSON 解析中间件
Task<void> json_parser(Context& ctx, Next next) {
    auto content_type = ctx.header("Content-Type");
    if (content_type && content_type->find("application/json") != std::string::npos) {
        // body 已经在 ctx.body 中
    }
    co_await next();
}
```

## 5. 使用示例

```cpp
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/middleware.hpp>

Task<void> main_task() {
    auto app = async_uv::layer3::app();
    
    // 全局中间件
    app.use(async_uv::layer3::middleware::logger());
    app.use(async_uv::layer3::middleware::error_handler());
    
    // 路由
    app.get("/", [](Context& ctx) -> Task<> {
        ctx.json({{"message", "Hello World"}});
        co_return;
    });
    
    app.get("/users/{id}", [](Context& ctx) -> Task<> {
        auto id = ctx.param("id");
        // auto user = co_await db.find_user(id);
        ctx.json({{"id", std::string(id)}, {"name", "test"}});
        co_return;
    });
    
    app.post("/users", [](Context& ctx) -> Task<> {
        auto body = ctx.json_as<User>().value();
        // co_await db.create_user(body);
        ctx.status(201);
        ctx.json(body);
        co_return;
    });
    
    // 子路由
    auto api_router = async_uv::layer3::router();
    api_router.get("/status", [](Context& ctx) -> Task<> {
        ctx.json({{"status", "ok"}});
        co_return;
    });
    app.route("/api", api_router);
    
    co_await app.listen(8080);
}
```

## 6. 文件结构

```
layer3/
├── CMakeLists.txt
├── docs/
│   └── design.md
├── include/
│   └── async_uv_layer3/
│       ├── context.hpp       # Context 定义
│       ├── router.hpp        # Router 定义
│       ├── router_node.hpp   # Radix Tree 节点
│       ├── middleware.hpp    # 中间件接口 + 内置中间件
│       └── app.hpp           # App 组装器
└── src/
    ├── context.cpp
    ├── router.cpp
    └── app.cpp
```

## 7. 实现计划

1. **Phase 1**: Radix Tree 路由器
   - router_node.hpp/cpp - 节点定义和匹配算法
   - router.hpp/cpp - 路由注册和匹配接口

2. **Phase 2**: Context 和中间件
   - context.hpp/cpp - Context 实现
   - middleware.hpp - 中间件类型定义
   - 内置中间件: logger, error_handler

3. **Phase 3**: App 组装器
   - app.hpp/cpp - 整合路由和中间件
   - HTTP 服务启动逻辑

4. **Phase 4**: 测试和文档
   - 单元测试
   - 示例代码