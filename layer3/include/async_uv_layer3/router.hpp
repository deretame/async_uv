#pragma once

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

class Context;

using Handler = std::function<Task<void>(Context&)>;
using Next = std::function<Task<void>()>;
using Middleware = std::function<Task<void>(Context&, Next)>;

class Router {
public:
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

    struct MatchResult {
        Handler handler;
        std::map<std::string, std::string> params;
    };

    std::optional<MatchResult> match(std::string_view method, std::string_view path) const;

    Router& route(std::string_view prefix, Router sub_router);

private:
    std::unique_ptr<RouterNode<Handler>> root_;
    std::vector<std::pair<std::string, Router>> sub_routers_;

    void add_route(std::string_view method, std::string_view pattern, Handler handler);
};

} // namespace async_uv::layer3