#pragma once

#include <functional>
#include <async_uv/task.h>

namespace async_uv::layer3 {

using Next = std::function<async_uv::Task<void>()>;

} // namespace async_uv::layer3