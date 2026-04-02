#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <async_uv_layer3/context.hpp>

namespace async_uv::layer3::middleware {

struct CorsOptions {
    std::vector<std::string> allow_origins = {"*"};
    std::vector<std::string> allow_methods = {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};
    std::vector<std::string> allow_headers = {"Content-Type", "Authorization", "X-Requested-With"};
    std::vector<std::string> expose_headers;
    bool allow_credentials = false;
    int max_age = 86400;
};

namespace detail {

inline std::string join_strings(const std::vector<std::string>& strs, const std::string& delim) {
    if (strs.empty()) return "";
    
    std::string result = strs[0];
    for (size_t i = 1; i < strs.size(); ++i) {
        result += delim + strs[i];
    }
    return result;
}

inline bool is_origin_allowed(std::string_view origin, const std::vector<std::string>& allowed) {
    if (allowed.size() == 1 && allowed[0] == "*") {
        return true;
    }
    for (const auto& o : allowed) {
        if (o == origin) return true;
    }
    return false;
}

} // namespace detail

inline auto cors(CorsOptions options = {}) {
    return [opts = std::move(options)](Context& ctx, Next next) -> Task<void> {
        auto origin = ctx.header("Origin");
        
        if (!origin) {
            co_await next();
            co_return;
        }

        const bool wildcard_origin = (opts.allow_origins.size() == 1 && opts.allow_origins[0] == "*");
        
        if (!detail::is_origin_allowed(*origin, opts.allow_origins)) {
            co_await next();
            co_return;
        }

        ctx.set("Vary", "Origin");

        if (opts.allow_credentials) {
            // Credentials + wildcard is invalid in CORS; echo request origin instead.
            ctx.set("Access-Control-Allow-Origin", *origin);
        } else {
            ctx.set("Access-Control-Allow-Origin", wildcard_origin ? "*" : *origin);
        }

        if (opts.allow_credentials) {
            ctx.set("Access-Control-Allow-Credentials", "true");
        }
        
        if (ctx.method == "OPTIONS") {
            ctx.set("Vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");
            ctx.set("Access-Control-Allow-Methods", detail::join_strings(opts.allow_methods, ", "));
            ctx.set("Access-Control-Allow-Headers", detail::join_strings(opts.allow_headers, ", "));
            ctx.set("Access-Control-Max-Age", std::to_string(opts.max_age));
            ctx.status(204);
            co_return;
        }
        
        if (!opts.expose_headers.empty()) {
            ctx.set("Access-Control-Expose-Headers", detail::join_strings(opts.expose_headers, ", "));
        }
        
        co_await next();
    };
}

} // namespace async_uv::layer3::middleware
