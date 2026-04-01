#include "async_uv_layer3/router.hpp"
#include <algorithm>

namespace async_uv::layer3 {

Router::Router() : root_(std::make_unique<RouterNode<Handler>>()) {}

Router::~Router() = default;

Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

void Router::add_route(std::string_view method, std::string_view pattern, Handler handler) {
    std::string path(pattern);
    if (!path.empty() && path[0] == '/') {
        path = path.substr(1);
    }
    
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
    
    node->handlers[std::string(method)] = std::move(handler);
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

std::optional<Router::MatchResult> Router::match(std::string_view method, std::string_view path) const {
    std::string normalized_path(path);
    if (!normalized_path.empty() && normalized_path[0] == '/') {
        normalized_path = normalized_path.substr(1);
    }
    
    for (const auto& [prefix, sub_router] : sub_routers_) {
        if (normalized_path.starts_with(prefix)) {
            std::string_view remaining = normalized_path;
            remaining = remaining.substr(prefix.size());
            auto result = sub_router.match(method, remaining);
            if (result) {
                return result;
            }
        }
    }
    
    auto result = root_->match(method, normalized_path);
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

Router& Router::route(std::string_view prefix, Router sub_router) {
    std::string normalized_prefix(prefix);
    if (!normalized_prefix.empty() && normalized_prefix[0] == '/') {
        normalized_prefix = normalized_prefix.substr(1);
    }
    if (!normalized_prefix.empty() && normalized_prefix.back() == '/') {
        normalized_prefix.pop_back();
    }
    
    sub_routers_.emplace_back(std::move(normalized_prefix), std::move(sub_router));
    return *this;
}

} // namespace async_uv::layer3