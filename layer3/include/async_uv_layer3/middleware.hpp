#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/error.hpp>

#if defined(ASYNC_UV_LAYER3_HAS_ZLIB) && ASYNC_UV_LAYER3_HAS_ZLIB
#include <zlib.h>
#endif

namespace async_uv::layer3::middleware {

struct CompressOptions {
    int level = 6;
    std::size_t min_size = 1024;
    std::vector<std::string> types = {"text/*", "application/json"};
};

namespace detail {

inline std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

inline std::optional<std::string> local_string(const Context& ctx, std::string_view key) {
    if (auto value = ctx.local<std::string>(key)) {
        return *value;
    }
    return std::nullopt;
}

inline std::string error_body_json(
    int status,
    std::string_view error,
    std::string_view code,
    std::optional<int> biz_code,
    const Context& ctx) {
    std::string payload = "{";
    payload += "\"status\":" + std::to_string(status);
    payload += ",\"error\":\"" + json_escape(error) + "\"";
    payload += ",\"code\":\"" + json_escape(code) + "\"";
    if (biz_code) {
        payload += ",\"biz_code\":" + std::to_string(*biz_code);
    } else {
        payload += ",\"biz_code\":null";
    }

    const std::string_view path = ctx.target.path.empty() ? std::string_view("/") : std::string_view(ctx.target.path);
    payload += ",\"path\":\"" + json_escape(path) + "\"";
    payload += ",\"method\":\"" + json_escape(ctx.method) + "\"";

    if (auto request_id = local_string(ctx, "request_id")) {
        payload += ",\"request_id\":\"" + json_escape(*request_id) + "\"";
    } else {
        payload += ",\"request_id\":null";
    }

    if (auto trace_id = local_string(ctx, "trace_id")) {
        payload += ",\"trace_id\":\"" + json_escape(*trace_id) + "\"";
    } else {
        payload += ",\"trace_id\":null";
    }

    payload += "}";
    return payload;
}

inline void write_error_json(
    Context& ctx,
    int status,
    std::string_view error,
    std::string_view code,
    std::optional<int> biz_code = std::nullopt) {
    ctx.status(status);
    ctx.json_raw(error_body_json(status, error, code, biz_code, ctx));
}

inline std::string weak_etag(std::string_view body) {
    const std::size_t hashed = std::hash<std::string_view>{}(body);
    std::ostringstream oss;
    oss << "W/\"" << std::hex << hashed << '"';
    return oss.str();
}

inline std::optional<std::string> response_header_value(const Context& ctx, std::string_view name) {
    for (const auto& h : ctx.response.headers) {
        if (h.name.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (size_t i = 0; i < name.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(h.name[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            return h.value;
        }
    }
    return std::nullopt;
}

inline bool content_type_matches(std::string_view content_type, const std::vector<std::string>& allowed) {
    if (content_type.empty()) {
        return false;
    }

    const auto semicolon = content_type.find(';');
    if (semicolon != std::string_view::npos) {
        content_type = content_type.substr(0, semicolon);
    }

    for (const auto& rule : allowed) {
        if (rule.ends_with("/*")) {
            const std::string_view prefix(rule.data(), rule.size() - 1);
            if (content_type.starts_with(prefix)) {
                return true;
            }
        } else if (content_type == rule) {
            return true;
        }
    }
    return false;
}

#if defined(ASYNC_UV_LAYER3_HAS_ZLIB) && ASYNC_UV_LAYER3_HAS_ZLIB
inline std::optional<std::string> gzip_compress(std::string_view input, int level) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::nullopt;
    }

    std::string output;
    output.resize(deflateBound(&zs, static_cast<uLong>(input.size())));

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = reinterpret_cast<Bytef*>(output.data());
    zs.avail_out = static_cast<uInt>(output.size());

    const int ret = deflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&zs);
        return std::nullopt;
    }

    output.resize(zs.total_out);
    deflateEnd(&zs);
    return output;
}
#endif

} // namespace detail

inline Task<void> logger(Context& ctx, Next next) {
    auto start = std::chrono::steady_clock::now();
    std::cout << "-> " << ctx.method << " " << ctx.target.path << "\n";

    co_await next();

    const auto duration = std::chrono::steady_clock::now() - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    std::cout << "<- " << ctx.response.status_code << " (" << ms << "ms)\n";
}

inline Task<void> error_handler(Context& ctx, Next next) {
    try {
        co_await next();
    } catch (const AppErrorException& e) {
        std::optional<int> biz_code;
        if (e.error.biz_error) {
            biz_code = static_cast<int>(*e.error.biz_error);
        }
        const std::string_view message = e.error.message.empty()
            ? std::string_view("Internal Server Error")
            : std::string_view(e.error.message);
        detail::write_error_json(
            ctx,
            static_cast<int>(e.error.http_error),
            message,
            e.error.code,
            biz_code);
    } catch (const FrameworkError& e) {
        const std::string message = e.error.message.empty()
            ? std::string(e.error.status_text())
            : e.error.message;
        detail::write_error_json(
            ctx,
            e.error.status_code(),
            message,
            error_code_name(e.error.code),
            error_biz_code(e.error.code));
    } catch (const std::exception&) {
        detail::write_error_json(ctx, 500, "Internal Server Error", "INTERNAL_ERROR");
    } catch (...) {
        detail::write_error_json(ctx, 500, "Internal Server Error", "INTERNAL_ERROR");
    }
}

inline auto security_headers() {
    return [](Context& ctx, Next next) -> Task<void> {
        ctx.set("X-Content-Type-Options", "nosniff");
        ctx.set("X-Frame-Options", "DENY");
        ctx.set("Referrer-Policy", "strict-origin-when-cross-origin");
        ctx.set("Content-Security-Policy", "default-src 'self'");
        ctx.set("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
        co_await next();
    };
}

template<typename T>
inline auto validate(std::function<bool(const T&)> validator) {
    return [validator = std::move(validator)](Context& ctx, Next next) -> Task<void> {
        auto parsed = ctx.json_as<T>();
        if (!parsed) {
            throw FrameworkError(
                ErrorCode::BadRequest,
                "Invalid JSON payload",
                "Failed to parse request body into target type");
        }

        if (!validator(*parsed)) {
            throw FrameworkError(
                ErrorCode::UnprocessableEntity,
                "Validation failed",
                "Payload does not satisfy validator constraints");
        }

        ctx.set_local<T>("validated", *parsed);
        co_await next();
    };
}

template<typename T>
inline auto validate() {
    return [](Context& ctx, Next next) -> Task<void> {
        auto parsed = ctx.json_as<T>();
        if (!parsed) {
            throw FrameworkError(
                ErrorCode::BadRequest,
                "Invalid JSON payload",
                "Failed to parse request body into target type");
        }
        ctx.set_local<T>("validated", *parsed);
        co_await next();
    };
}

inline auto etag() {
    return [](Context& ctx, Next next) -> Task<void> {
        co_await next();

        if (ctx.response.chunked || ctx.response.body.empty()) {
            co_return;
        }
        if (ctx.response.status_code < 200 || ctx.response.status_code >= 300) {
            co_return;
        }

        const std::string tag = detail::weak_etag(ctx.response.body);
        ctx.set("ETag", tag);

        if (auto inm = ctx.header("If-None-Match"); inm && *inm == tag) {
            ctx.status(304);
            ctx.response.body.clear();
        }
    };
}

inline auto compress(CompressOptions options = {}) {
    return [opts = std::move(options)](Context& ctx, Next next) -> Task<void> {
        co_await next();

        if (ctx.response.chunked || ctx.response.body.size() < opts.min_size) {
            co_return;
        }
        if (ctx.response.status_code < 200 || ctx.response.status_code >= 300) {
            co_return;
        }
        if (detail::response_header_value(ctx, "Content-Encoding").has_value()) {
            co_return;
        }

        auto accept = ctx.header("Accept-Encoding");
        if (!accept || accept->find("gzip") == std::string_view::npos) {
            co_return;
        }

        auto content_type = detail::response_header_value(ctx, "Content-Type");
        if (!content_type || !detail::content_type_matches(*content_type, opts.types)) {
            co_return;
        }

#if defined(ASYNC_UV_LAYER3_HAS_ZLIB) && ASYNC_UV_LAYER3_HAS_ZLIB
        const int level = std::clamp(opts.level, 1, 9);
        auto compressed = detail::gzip_compress(ctx.response.body, level);
        if (!compressed || compressed->size() >= ctx.response.body.size()) {
            co_return;
        }

        ctx.response.body = std::move(*compressed);
        ctx.set("Content-Encoding", "gzip");
        ctx.set("Vary", "Accept-Encoding");
#else
        (void)opts;
#endif
    };
}

} // namespace async_uv::layer3::middleware
