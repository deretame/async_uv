#pragma once

#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/router.hpp>

namespace async_uv::layer3 {

class RouteGroup {
public:
    using Builder = std::function<void(RouteGroup&)>;

    RouteGroup() = default;
    explicit RouteGroup(std::string prefix) : prefix_(normalize_path(prefix)) {}

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

    RouteGroup& get(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
        add_route("GET", pattern, std::move(handler), std::move(meta));
        return *this;
    }

    RouteGroup& post(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
        add_route("POST", pattern, std::move(handler), std::move(meta));
        return *this;
    }

    RouteGroup& put(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
        add_route("PUT", pattern, std::move(handler), std::move(meta));
        return *this;
    }

    RouteGroup& del(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
        add_route("DELETE", pattern, std::move(handler), std::move(meta));
        return *this;
    }

    RouteGroup& patch(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
        add_route("PATCH", pattern, std::move(handler), std::move(meta));
        return *this;
    }

    RouteGroup& all(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
        add_route("*", pattern, std::move(handler), std::move(meta));
        return *this;
    }

    RouteGroup& router(std::string_view prefix, Builder builder) {
        RouteGroup child(join_paths(prefix_, prefix));
        child.middlewares_ = middlewares_;
        builder(child);
        children_.push_back(std::move(child));
        return *this;
    }

    const std::string& prefix() const noexcept { return prefix_; }
    const std::vector<Middleware>& middlewares() const noexcept { return middlewares_; }

    Router to_router() const & {
        Router router_value;
        materialize_to_router(router_value);
        return router_value;
    }

    Router to_router() && {
        return static_cast<const RouteGroup&>(*this).to_router();
    }

    void apply_to(App& app) const {
        materialize_to_app(app);
    }

private:
    struct RouteEntry {
        std::string method;
        std::string pattern;
        Handler handler;
        std::optional<Router::RouteMeta> meta;
    };

    static std::string normalize_path(std::string_view input) {
        if (input.empty()) {
            return "";
        }

        std::string out;
        out.reserve(input.size() + 1);
        if (input.front() != '/') {
            out.push_back('/');
        }

        bool prev_slash = !out.empty() && out.back() == '/';
        for (char ch : input) {
            if (ch == '/') {
                if (!prev_slash) {
                    out.push_back('/');
                    prev_slash = true;
                }
            } else {
                out.push_back(ch);
                prev_slash = false;
            }
        }

        if (out.size() > 1 && out.back() == '/') {
            out.pop_back();
        }
        return out;
    }

    static std::string join_paths(std::string_view lhs, std::string_view rhs) {
        const std::string left = normalize_path(lhs);
        const std::string right = normalize_path(rhs);

        if (left.empty()) {
            return right;
        }
        if (right.empty() || right == "/") {
            return left;
        }
        if (left == "/") {
            return right;
        }
        return left + right;
    }

    void add_route(
        std::string method,
        std::string_view pattern,
        Handler handler,
        std::optional<Router::RouteMeta> meta = std::nullopt) {
        routes_.push_back(RouteEntry{
            .method = std::move(method),
            .pattern = normalize_path(pattern),
            .handler = std::move(handler),
            .meta = std::move(meta)
        });
    }

    Handler wrap_handler(Handler handler) const {
        if (middlewares_.empty()) {
            return handler;
        }
        return [middlewares = middlewares_, handler = std::move(handler)](Context& ctx) -> Task<void> {
            std::function<Task<void>(std::size_t)> run;
            run = [&](std::size_t index) -> Task<void> {
                if (index >= middlewares.size()) {
                    co_await handler(ctx);
                    co_return;
                }
                co_await middlewares[index](ctx, [&]() -> Task<void> {
                    co_await run(index + 1);
                });
            };
            co_await run(0);
        };
    }

    std::string full_pattern(std::string_view pattern) const {
        return join_paths(prefix_, pattern);
    }

    void register_route_to_app(App& app, const RouteEntry& entry) const {
        Handler wrapped = wrap_handler(entry.handler);
        const std::string pattern = full_pattern(entry.pattern);

        if (entry.method == "GET") {
            if (entry.meta) app.get(pattern, std::move(wrapped), *entry.meta);
            else app.get(pattern, std::move(wrapped));
        } else if (entry.method == "POST") {
            if (entry.meta) app.post(pattern, std::move(wrapped), *entry.meta);
            else app.post(pattern, std::move(wrapped));
        } else if (entry.method == "PUT") {
            if (entry.meta) app.put(pattern, std::move(wrapped), *entry.meta);
            else app.put(pattern, std::move(wrapped));
        } else if (entry.method == "DELETE") {
            if (entry.meta) app.del(pattern, std::move(wrapped), *entry.meta);
            else app.del(pattern, std::move(wrapped));
        } else if (entry.method == "PATCH") {
            if (entry.meta) app.patch(pattern, std::move(wrapped), *entry.meta);
            else app.patch(pattern, std::move(wrapped));
        } else if (entry.method == "*") {
            if (entry.meta) app.all(pattern, std::move(wrapped), *entry.meta);
            else app.all(pattern, std::move(wrapped));
        }
    }

    static void register_route_to_router(
        Router& router_value,
        std::string_view prefix,
        const RouteEntry& entry) {
        const std::string pattern = join_paths(prefix, entry.pattern);
        if (entry.method == "GET") {
            if (entry.meta) router_value.get(pattern, entry.handler, *entry.meta);
            else router_value.get(pattern, entry.handler);
        } else if (entry.method == "POST") {
            if (entry.meta) router_value.post(pattern, entry.handler, *entry.meta);
            else router_value.post(pattern, entry.handler);
        } else if (entry.method == "PUT") {
            if (entry.meta) router_value.put(pattern, entry.handler, *entry.meta);
            else router_value.put(pattern, entry.handler);
        } else if (entry.method == "DELETE") {
            if (entry.meta) router_value.del(pattern, entry.handler, *entry.meta);
            else router_value.del(pattern, entry.handler);
        } else if (entry.method == "PATCH") {
            if (entry.meta) router_value.patch(pattern, entry.handler, *entry.meta);
            else router_value.patch(pattern, entry.handler);
        } else if (entry.method == "*") {
            if (entry.meta) router_value.all(pattern, entry.handler, *entry.meta);
            else router_value.all(pattern, entry.handler);
        }
    }

    void materialize_to_router(Router& router_value) const {
        for (const auto& entry : routes_) {
            register_route_to_router(router_value, prefix_, entry);
        }
        for (const auto& child : children_) {
            child.materialize_to_router(router_value);
        }
    }

    void materialize_to_app(App& app) const {
        for (const auto& entry : routes_) {
            register_route_to_app(app, entry);
        }
        for (const auto& child : children_) {
            child.materialize_to_app(app);
        }
    }

    std::string prefix_;
    std::vector<Middleware> middlewares_;
    std::vector<RouteEntry> routes_;
    std::vector<RouteGroup> children_;
};

} // namespace async_uv::layer3
