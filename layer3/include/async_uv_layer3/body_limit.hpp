#pragma once

#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/error.hpp>
#include <async_uv_layer3/types.hpp>

namespace async_uv::layer3::middleware {

struct BodyLimitOptions {
    std::size_t max_bytes = 1024 * 1024;
    bool reject_on_limit = true;
};

inline Task<void> body_limit(Context& ctx, BodyLimitOptions options, Next next) {
    if (ctx.body_size > options.max_bytes) {
        if (options.reject_on_limit) {
            throw HttpError(ErrorCode::PayloadTooLarge,
                "Request body too large",
                "Maximum allowed: " + std::to_string(options.max_bytes) + " bytes");
        }
        ctx.status(413);
        ctx.json_raw("{\"error\":\"Payload Too Large\"}");
        co_return;
    }
    co_await next();
}

inline Task<void> body_limit(Context& ctx, std::size_t max_bytes, Next next) {
    co_await body_limit(ctx, BodyLimitOptions{max_bytes}, std::move(next));
}

}

namespace async_uv::layer3 {

inline void set_body_limit(Context& ctx, std::size_t max_bytes) {
    ctx.set_local<std::size_t>("body_limit", max_bytes);
}

inline std::optional<std::size_t> get_body_limit(Context& ctx) {
    return ctx.local<std::size_t>("body_limit");
}

}