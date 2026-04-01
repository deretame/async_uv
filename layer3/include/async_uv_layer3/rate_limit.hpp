#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/error.hpp>
#include <async_uv_layer3/types.hpp>

namespace async_uv::layer3::middleware {

struct RateLimitOptions {
    std::size_t max_requests = 100;
    std::chrono::milliseconds window{60000};
    std::string key_extractor = "ip";
    bool include_headers = true;
    std::string message = "Too many requests, please try again later";
};

namespace detail {

class RateLimiter {
public:
    struct Entry {
        std::size_t count = 0;
        std::chrono::steady_clock::time_point reset_time;
    };
    
    explicit RateLimiter(const RateLimitOptions& opts) : options_(opts) {}
    
    bool try_consume(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto it = entries_.find(key);
        
        if (it == entries_.end() || now >= it->second.reset_time) {
            entries_[key] = {1, now + options_.window};
            return true;
        }
        
        if (it->second.count >= options_.max_requests) {
            return false;
        }
        
        ++it->second.count;
        return true;
    }
    
    std::size_t remaining(const std::string& key) const {
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return options_.max_requests;
        }
        if (options_.max_requests <= it->second.count) {
            return 0;
        }
        return options_.max_requests - it->second.count;
    }
    
    std::chrono::milliseconds reset_after(const std::string& key) const {
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return options_.window;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= it->second.reset_time) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(it->second.reset_time - now);
    }
    
    void cleanup() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now >= it->second.reset_time) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    RateLimitOptions options_;
    std::map<std::string, Entry> entries_;
    mutable std::mutex mutex_;
};

inline std::string extract_ip(Context& ctx) {
    auto forward = ctx.header("X-Forwarded-For");
    if (forward) {
        std::string_view sv = *forward;
        auto pos = sv.find(',');
        if (pos != std::string_view::npos) {
            return std::string(sv.substr(0, pos));
        }
        return std::string(sv);
    }
    
    auto real_ip = ctx.header("X-Real-IP");
    if (real_ip) {
        return std::string(*real_ip);
    }
    
    return ctx.meta.remote_address;
}

inline std::string extract_user(Context& ctx) {
    auto user_id = ctx.local<std::string>("user_id");
    if (user_id) {
        return *user_id;
    }
    return extract_ip(ctx);
}

}

inline Task<void> rate_limit(Context& ctx, std::shared_ptr<detail::RateLimiter> limiter, 
                            RateLimitOptions options, Next next) {
    std::string key;
    if (options.key_extractor == "user") {
        key = detail::extract_user(ctx);
    } else {
        key = detail::extract_ip(ctx);
    }
    
    if (!limiter->try_consume(key)) {
        if (options.include_headers) {
            auto remaining = limiter->remaining(key);
            auto reset_ms = limiter->reset_after(key);
            
            ctx.set("X-RateLimit-Limit", std::to_string(options.max_requests));
            ctx.set("X-RateLimit-Remaining", std::to_string(remaining));
            ctx.set("X-RateLimit-Reset", std::to_string(reset_ms.count() / 1000));
            ctx.set("Retry-After", std::to_string(reset_ms.count() / 1000));
        }
        
        throw HttpError(ErrorCode::TooManyRequests, options.message);
    }
    
    if (options.include_headers) {
        auto remaining = limiter->remaining(key);
        auto reset_ms = limiter->reset_after(key);
        ctx.set("X-RateLimit-Limit", std::to_string(options.max_requests));
        ctx.set("X-RateLimit-Remaining", std::to_string(remaining));
        ctx.set("X-RateLimit-Reset", std::to_string(reset_ms.count() / 1000));
    }
    
    co_await next();
}

inline Task<void> rate_limit(Context& ctx, std::shared_ptr<detail::RateLimiter> limiter, Next next) {
    co_await rate_limit(ctx, limiter, RateLimitOptions{}, std::move(next));
}

inline std::shared_ptr<detail::RateLimiter> create_rate_limiter(RateLimitOptions options = {}) {
    return std::make_shared<detail::RateLimiter>(options);
}

}