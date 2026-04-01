#pragma once

#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#include <async_uv_layer3/context.hpp>

namespace async_uv::layer3::middleware {

inline Task<void> logger(Context& ctx, Next next) {
    auto start = std::chrono::steady_clock::now();
    std::cout << "-> " << ctx.method << " " << ctx.target.path << "\n";
    
    co_await next();
    
    auto duration = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    std::cout << "<- " << ctx.response.status_code << " (" << ms << "ms)\n";
}

inline Task<void> error_handler(Context& ctx, Next next) {
    try {
        co_await next();
    } catch (const std::exception& e) {
        ctx.status(500);
        ctx.json_raw("{\"error\":\"" + std::string(e.what()) + "\"}");
    } catch (...) {
        ctx.status(500);
        ctx.json_raw("{\"error\":\"Unknown error\"}");
    }
}

} // namespace async_uv::layer3::middleware