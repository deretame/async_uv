#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async_uv/task.h>
#include <async_uv_layer3/router_node.hpp>

namespace async_uv::layer3 {

class Context;

using Handler = std::function<Task<void>(Context&)>;
using Next = std::function<Task<void>()>;
using Middleware = std::function<Task<void>(Context&, Next)>;

class Router {
public:
    struct RouteMeta {
        std::string name;
        std::string description;
        std::vector<std::string> tags;
        bool require_auth = false;
    };

    struct RouteInfo {
        std::string method;
        std::string pattern;
        std::vector<std::string> param_names;
        RouteMeta meta;
    };

    Router();
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) noexcept;
    Router& operator=(Router&&) noexcept;

    Router& get(std::string_view pattern, Handler handler);
    Router& post(std::string_view pattern, Handler handler);
    Router& put(std::string_view pattern, Handler handler);
    Router& del(std::string_view pattern, Handler handler);
    Router& patch(std::string_view pattern, Handler handler);
    Router& all(std::string_view pattern, Handler handler);

    Router& get(std::string_view pattern, Handler handler, RouteMeta meta);
    Router& post(std::string_view pattern, Handler handler, RouteMeta meta);
    Router& put(std::string_view pattern, Handler handler, RouteMeta meta);
    Router& del(std::string_view pattern, Handler handler, RouteMeta meta);
    Router& patch(std::string_view pattern, Handler handler, RouteMeta meta);
    Router& all(std::string_view pattern, Handler handler, RouteMeta meta);

    std::vector<std::string> allowed_methods(std::string_view path) const;
    std::vector<RouteInfo> routes() const;

    struct MatchResult {
        Handler handler;
        std::map<std::string, std::string> params;
    };

    std::optional<MatchResult> match(std::string_view method, std::string_view path) const;

    Router& route(std::string_view prefix, Router sub_router);

private:
    std::unique_ptr<RouterNode<Handler>> root_;
    std::vector<std::pair<std::string, Router>> sub_routers_;
    std::vector<RouteInfo> route_infos_;

    void add_route(
        std::string_view method,
        std::string_view pattern,
        Handler handler,
        std::optional<RouteMeta> meta = std::nullopt);
};

} // namespace async_uv::layer3
