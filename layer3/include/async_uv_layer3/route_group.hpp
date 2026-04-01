#pragma once

#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/types.hpp>

namespace async_uv::layer3 {

class RouteGroup {
public:
    RouteGroup() = default;
    explicit RouteGroup(std::string prefix) : prefix_(std::move(prefix)) {}
    
    RouteGroup& use(Middleware middleware) {
        middlewares_.push_back(std::move(middleware));
        return *this;
    }
    
    RouteGroup& get(std::string_view pattern, Handler handler) {
        add_route("GET", pattern, std::move(handler));
        return *this;
    }
    
    RouteGroup& post(std::string_view pattern, Handler handler) {
        add_route("POST", pattern, std::move(handler));
        return *this;
    }
    
    RouteGroup& put(std::string_view pattern, Handler handler) {
        add_route("PUT", pattern, std::move(handler));
        return *this;
    }
    
    RouteGroup& del(std::string_view pattern, Handler handler) {
        add_route("DELETE", pattern, std::move(handler));
        return *this;
    }
    
    RouteGroup& patch(std::string_view pattern, Handler handler) {
        add_route("PATCH", pattern, std::move(handler));
        return *this;
    }
    
    RouteGroup& all(std::string_view pattern, Handler handler) {
        add_route("*", pattern, std::move(handler));
        return *this;
    }
    
    const std::string& prefix() const noexcept { return prefix_; }
    const std::vector<Middleware>& middlewares() const noexcept { return middlewares_; }
    
    Router to_router() && {
        Router router;
        for (auto& [method, pattern, handler] : routes_) {
            std::string full_pattern = prefix_ + std::string(pattern);
            if (method == "GET") router.get(full_pattern, std::move(handler));
            else if (method == "POST") router.post(full_pattern, std::move(handler));
            else if (method == "PUT") router.put(full_pattern, std::move(handler));
            else if (method == "DELETE") router.del(full_pattern, std::move(handler));
            else if (method == "PATCH") router.patch(full_pattern, std::move(handler));
            else if (method == "*") router.all(full_pattern, std::move(handler));
        }
        return router;
    }
    
    void apply_to(App& app);
    
private:
    void add_route(std::string method, std::string_view pattern, Handler handler) {
        routes_.emplace_back(std::move(method), std::string(pattern), std::move(handler));
    }
    
    std::string prefix_;
    std::vector<Middleware> middlewares_;
    std::vector<std::tuple<std::string, std::string, Handler>> routes_;
};

}