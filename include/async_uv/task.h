#pragma once

#include <future>
#include <type_traits>

#include <exec/task.hpp>

namespace async_uv {

template <class T = void>
using Task = exec::task<T>;

template <class T = void>
using Future = std::shared_future<T>;

template <class T>
struct task_value;

template <class T>
struct task_value<exec::task<T>> {
    using type = T;
};

template <class Awaitable>
using TaskValue = typename task_value<std::decay_t<Awaitable>>::type;

} // namespace async_uv

