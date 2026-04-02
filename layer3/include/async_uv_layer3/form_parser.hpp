#pragma once

#include <map>
#include <string>
#include <string_view>

#include <ada.h>
#include <async_uv_layer3/context.hpp>

namespace async_uv::layer3 {

namespace detail {

inline std::string percent_decode_component(std::string_view text) {
    size_t first_percent = text.find('%');
    size_t first_plus = text.find('+');
    if (first_percent == std::string_view::npos && first_plus == std::string_view::npos) {
        return std::string(text);
    }

    std::string normalized(text);
    if (first_plus != std::string_view::npos) {
        for (char& ch : normalized) {
            if (ch == '+') {
                ch = ' ';
            }
        }
    }

    first_percent = normalized.find('%');
    if (first_percent == std::string::npos) {
        return normalized;
    }
    return ada::unicode::percent_decode(normalized, first_percent);
}

inline std::map<std::string, std::string> parse_form_data(std::string_view body) {
    std::map<std::string, std::string> result;
    
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        std::string_view pair;
        
        if (amp == std::string_view::npos) {
            pair = body.substr(pos);
            pos = body.size();
        } else {
            pair = body.substr(pos, amp - pos);
            pos = amp + 1;
        }
        
        if (pair.empty()) continue;
        
        size_t eq = pair.find('=');
        if (eq == std::string_view::npos) {
            std::string key = percent_decode_component(pair);
            result[std::move(key)] = "";
        } else {
            std::string key = percent_decode_component(pair.substr(0, eq));
            std::string value = percent_decode_component(pair.substr(eq + 1));
            result[std::move(key)] = std::move(value);
        }
    }
    
    return result;
}

} // namespace detail

namespace middleware {

inline Task<void> form_parser(Context& ctx, Next next) {
    auto content_type = ctx.header("Content-Type");
    if (content_type) {
        std::string_view ct = *content_type;
        if (ct.find("application/x-www-form-urlencoded") != std::string_view::npos) {
            auto form_data = async_uv::layer3::detail::parse_form_data(ctx.body);
            ctx.set_local<std::map<std::string, std::string>>("form_data", std::move(form_data));
        }
    }
    
    co_await next();
}

} // namespace middleware

inline std::optional<std::string> form_field(Context& ctx, std::string_view name) {
    auto form_data = ctx.local<std::map<std::string, std::string>>("form_data");
    if (!form_data) {
        return std::nullopt;
    }
    
    auto it = form_data->find(std::string(name));
    if (it != form_data->end()) {
        return it->second;
    }
    
    return std::nullopt;
}

inline bool has_form_data(Context& ctx) {
    return ctx.local<std::map<std::string, std::string>>("form_data").has_value();
}

inline std::optional<std::map<std::string, std::string>> all_form_data(Context& ctx) {
    return ctx.local<std::map<std::string, std::string>>("form_data");
}

} // namespace async_uv::layer3
