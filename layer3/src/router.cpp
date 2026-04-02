#include "async_uv_layer3/router.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <async_uv_http/server.h>

namespace {

std::string to_upper_ascii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

std::string normalize_path(std::string_view raw_path) {
    // Reuse Layer2 request-target parsing to keep URL semantics aligned.
    const auto parsed = async_uv::http::parse_request_target(raw_path);
    std::string_view path(parsed.path);
    std::string normalized;
    normalized.reserve(path.size() + 1);

    if (path.empty() || path.front() != '/') {
        normalized.push_back('/');
    }

    bool prev_slash = !normalized.empty() && normalized.back() == '/';
    for (char ch : path) {
        if (ch == '/') {
            if (!prev_slash) {
                normalized.push_back('/');
                prev_slash = true;
            }
            continue;
        }
        normalized.push_back(ch);
        prev_slash = false;
    }

    if (normalized.empty()) {
        normalized = "/";
    }

    if (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }

    return normalized;
}

std::string to_storage_path(std::string_view raw_path) {
    std::string normalized = normalize_path(raw_path);
    if (normalized == "/") {
        return "";
    }
    return normalized.substr(1);
}

bool prefix_matches(std::string_view normalized_path, std::string_view prefix) {
    if (prefix.empty()) {
        return true;
    }
    return normalized_path.starts_with(prefix)
        && (normalized_path.size() == prefix.size() || normalized_path[prefix.size()] == '/');
}

std::vector<std::string> wildcard_methods() {
    return {"DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT"};
}

std::vector<std::string> extract_param_names(std::string_view normalized_pattern) {
    std::vector<std::string> names;
    std::string path = to_storage_path(normalized_pattern);
    size_t pos = 0;
    while (pos < path.size()) {
        size_t slash_pos = path.find('/', pos);
        std::string_view segment = (slash_pos == std::string::npos)
            ? std::string_view(path).substr(pos)
            : std::string_view(path).substr(pos, slash_pos - pos);
        pos = (slash_pos == std::string::npos) ? path.size() : slash_pos + 1;

        if (segment.empty()) {
            continue;
        }
        const auto type = async_uv::layer3::RouterNode<async_uv::layer3::Handler>::classify(segment);
        if (type == async_uv::layer3::RouterNode<async_uv::layer3::Handler>::Type::PARAM
            || type == async_uv::layer3::RouterNode<async_uv::layer3::Handler>::Type::WILDCARD) {
            auto [_, name] = async_uv::layer3::RouterNode<async_uv::layer3::Handler>::parse_param_segment(segment);
            if (!name.empty()) {
                names.emplace_back(name);
            }
        }
    }
    return names;
}

std::string join_prefix_pattern(std::string_view prefix, std::string_view pattern) {
    std::string out = "/";
    if (!prefix.empty()) {
        out += std::string(prefix);
    }

    if (pattern.empty() || pattern == "/") {
        return out;
    }

    std::string pattern_copy(pattern);
    if (!pattern_copy.empty() && pattern_copy.front() == '/') {
        pattern_copy.erase(pattern_copy.begin());
    }

    if (!out.empty() && out.back() != '/' && !pattern_copy.empty()) {
        out.push_back('/');
    }
    out += pattern_copy;
    return normalize_path(out);
}

} // namespace

namespace async_uv::layer3 {

Router::Router() : root_(std::make_unique<RouterNode<Handler>>()) {}

Router::~Router() = default;

Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

void Router::add_route(
    std::string_view method,
    std::string_view pattern,
    Handler handler,
    std::optional<RouteMeta> meta) {
    std::string method_key;
    if (method == "*") {
        method_key = "*";
    } else {
        method_key = to_upper_ascii(method);
        if (method_key.empty()) {
            throw std::invalid_argument("Route method cannot be empty");
        }
    }

    const std::string normalized_pattern = normalize_path(pattern);
    std::string path = to_storage_path(normalized_pattern);

    auto* node = root_.get();
    size_t pos = 0;

    while (pos < path.size()) {
        size_t slash_pos = path.find('/', pos);
        std::string segment;

        if (slash_pos == std::string::npos) {
            segment = path.substr(pos);
            pos = path.size();
        } else {
            segment = path.substr(pos, slash_pos - pos);
            pos = slash_pos + 1;
        }

        if (segment.empty()) continue;

        auto type = RouterNode<Handler>::classify(segment);
        std::string param_name;

        if (type == RouterNode<Handler>::Type::PARAM) {
            auto [_, name] = RouterNode<Handler>::parse_param_segment(segment);
            param_name = std::string(name);
            node = node->add_child("", type, param_name);
        } else if (type == RouterNode<Handler>::Type::WILDCARD) {
            auto [_, name] = RouterNode<Handler>::parse_param_segment(segment);
            param_name = std::string(name);
            node = node->add_child("", type, param_name);
        } else {
            node = node->add_child(segment, type);
        }
    }

    const bool duplicate = [&] {
        if (method_key == "*") {
            return !node->handlers.empty();
        }
        return node->handlers.contains(method_key) || node->handlers.contains("*");
    }();
    if (duplicate) {
        throw std::invalid_argument(
            "Duplicate route registration: " + method_key + " " + normalized_pattern);
    }

    node->handlers[method_key] = std::move(handler);
    route_infos_.push_back(RouteInfo{
        .method = method_key,
        .pattern = normalized_pattern,
        .param_names = extract_param_names(normalized_pattern),
        .meta = meta.value_or(RouteMeta{})
    });
}

Router& Router::get(std::string_view pattern, Handler handler) {
    add_route("GET", pattern, std::move(handler));
    return *this;
}

Router& Router::post(std::string_view pattern, Handler handler) {
    add_route("POST", pattern, std::move(handler));
    return *this;
}

Router& Router::put(std::string_view pattern, Handler handler) {
    add_route("PUT", pattern, std::move(handler));
    return *this;
}

Router& Router::del(std::string_view pattern, Handler handler) {
    add_route("DELETE", pattern, std::move(handler));
    return *this;
}

Router& Router::patch(std::string_view pattern, Handler handler) {
    add_route("PATCH", pattern, std::move(handler));
    return *this;
}

Router& Router::all(std::string_view pattern, Handler handler) {
    add_route("*", pattern, std::move(handler));
    return *this;
}

Router& Router::get(std::string_view pattern, Handler handler, RouteMeta meta) {
    add_route("GET", pattern, std::move(handler), std::move(meta));
    return *this;
}

Router& Router::post(std::string_view pattern, Handler handler, RouteMeta meta) {
    add_route("POST", pattern, std::move(handler), std::move(meta));
    return *this;
}

Router& Router::put(std::string_view pattern, Handler handler, RouteMeta meta) {
    add_route("PUT", pattern, std::move(handler), std::move(meta));
    return *this;
}

Router& Router::del(std::string_view pattern, Handler handler, RouteMeta meta) {
    add_route("DELETE", pattern, std::move(handler), std::move(meta));
    return *this;
}

Router& Router::patch(std::string_view pattern, Handler handler, RouteMeta meta) {
    add_route("PATCH", pattern, std::move(handler), std::move(meta));
    return *this;
}

Router& Router::all(std::string_view pattern, Handler handler, RouteMeta meta) {
    add_route("*", pattern, std::move(handler), std::move(meta));
    return *this;
}

std::optional<Router::MatchResult> Router::match(std::string_view method, std::string_view path) const {
    const std::string normalized_method = to_upper_ascii(method);
    const std::string normalized_path = to_storage_path(path);

    for (const auto& [prefix, sub_router] : sub_routers_) {
        if (prefix_matches(normalized_path, prefix)) {
            std::string_view remaining = std::string_view(normalized_path).substr(prefix.size());
            auto result = sub_router.match(normalized_method, remaining);
            if (result) {
                return result;
            }
        }
    }

    auto result = root_->match(normalized_method, normalized_path);
    if (result) {
        MatchResult match_result;
        match_result.handler = std::move(result->handler);
        for (const auto& [key, value] : result->params) {
            match_result.params[key] = value;
        }
        return match_result;
    }

    return std::nullopt;
}

std::vector<std::string> Router::allowed_methods(std::string_view path) const {
    const std::string normalized_path = to_storage_path(path);

    for (const auto& [prefix, sub_router] : sub_routers_) {
        if (!prefix_matches(normalized_path, prefix)) {
            continue;
        }
        std::string_view remaining = std::string_view(normalized_path).substr(prefix.size());
        auto methods = sub_router.allowed_methods(remaining);
        if (!methods.empty()) {
            return methods;
        }
    }

    const RouterNode<Handler>* node = root_->match_node(normalized_path);
    if (!node) {
        return {};
    }

    std::vector<std::string> methods;
    methods.reserve(node->handlers.size() + 1);
    if (node->handlers.contains("*")) {
        methods = wildcard_methods();
    } else {
        for (const auto& [key, _] : node->handlers) {
            methods.push_back(key);
        }
        if (std::find(methods.begin(), methods.end(), "GET") != methods.end()
            && std::find(methods.begin(), methods.end(), "HEAD") == methods.end()) {
            methods.push_back("HEAD");
        }
        std::sort(methods.begin(), methods.end());
        methods.erase(std::unique(methods.begin(), methods.end()), methods.end());
    }

    return methods;
}

std::vector<Router::RouteInfo> Router::routes() const {
    std::vector<RouteInfo> out = route_infos_;

    for (const auto& [prefix, sub_router] : sub_routers_) {
        const auto child_routes = sub_router.routes();
        out.reserve(out.size() + child_routes.size());
        for (auto info : child_routes) {
            info.pattern = join_prefix_pattern(prefix, info.pattern);
            out.push_back(std::move(info));
        }
    }

    return out;
}

Router& Router::route(std::string_view prefix, Router sub_router) {
    std::string normalized_prefix = to_storage_path(prefix);
    sub_routers_.emplace_back(std::move(normalized_prefix), std::move(sub_router));
    return *this;
}

} // namespace async_uv::layer3
