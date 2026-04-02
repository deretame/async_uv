#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/lifecycle.hpp>
#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/stream.hpp>
#include <async_uv/task.h>
#include <async_uv/tcp.h>
#include <async_uv_http/server.h>

namespace async_uv::layer3 {

class RouteGroup;

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    App& use(Middleware middleware);

    App& get(std::string_view pattern, Handler handler);
    App& post(std::string_view pattern, Handler handler);
    App& put(std::string_view pattern, Handler handler);
    App& del(std::string_view pattern, Handler handler);
    App& patch(std::string_view pattern, Handler handler);
    App& all(std::string_view pattern, Handler handler);

    App& get(std::string_view pattern, Handler handler, Router::RouteMeta meta);
    App& post(std::string_view pattern, Handler handler, Router::RouteMeta meta);
    App& put(std::string_view pattern, Handler handler, Router::RouteMeta meta);
    App& del(std::string_view pattern, Handler handler, Router::RouteMeta meta);
    App& patch(std::string_view pattern, Handler handler, Router::RouteMeta meta);
    App& all(std::string_view pattern, Handler handler, Router::RouteMeta meta);

    App& route(std::string_view prefix, Router sub_router);
    App& router(std::string_view prefix, std::function<void(RouteGroup&)> builder);

    std::vector<Router::RouteInfo> routes() const;

    App& with_limits(http::ServerLimits limits);
    App& with_policy(http::ServerConnectionPolicy policy);
    App& with_lifecycle(Lifecycle lifecycle);

    Task<void> listen(uint16_t port, std::string_view host = "0.0.0.0");

private:
    struct NormalResponse {
        http::ServerResponse response;
    };
    
    struct StreamingResponse {
        http::ServerResponse response;
        StreamHandler handler;
    };
    
    using Response = std::variant<NormalResponse, StreamingResponse>;
    
    Task<Response> handle_request(http::ServerRequest request);
    Task<void> run_middleware_chain(Context& ctx, size_t index);
    
    Task<void> send_response(TcpClient& client, Response&& resp);

    std::vector<Middleware> middlewares_;
    Router router_;
    http::ServerLimits limits_;
    http::ServerConnectionPolicy policy_;
    std::optional<Lifecycle> lifecycle_;
};

App app();

} // namespace async_uv::layer3
