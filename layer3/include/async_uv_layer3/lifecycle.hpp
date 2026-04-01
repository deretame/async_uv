#pragma once

#include <functional>
#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/error.hpp>
#include <async_uv_layer3/types.hpp>

namespace async_uv::layer3 {

struct LifecycleHooks {
    std::function<Task<void>(Context&, Error)> on_error;
    std::function<void(Context&)> before_handler;
    std::function<void(Context&)> after_handler;
    std::function<void(Context&, std::exception_ptr)> on_exception;
    
    LifecycleHooks& set_on_error(std::function<Task<void>(Context&, Error)> fn) {
        on_error = std::move(fn);
        return *this;
    }
    
    LifecycleHooks& set_before_handler(std::function<void(Context&)> fn) {
        before_handler = std::move(fn);
        return *this;
    }
    
    LifecycleHooks& set_after_handler(std::function<void(Context&)> fn) {
        after_handler = std::move(fn);
        return *this;
    }
    
    LifecycleHooks& set_on_exception(std::function<void(Context&, std::exception_ptr)> fn) {
        on_exception = std::move(fn);
        return *this;
    }
};

}