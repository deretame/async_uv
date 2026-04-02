#pragma once

#include <functional>

#include <async_uv/task.h>

namespace async_uv::layer3 {

struct Context;

struct Lifecycle {
    std::function<Task<void>()> on_start;
    std::function<Task<void>()> on_stop;
    std::function<void(Context&)> on_error;
};

} // namespace async_uv::layer3
