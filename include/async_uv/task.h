#pragma once

#include <type_traits>

#include <async_simple/Future.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/FutureAwaiter.h>

namespace async_uv {

template <class T = void>
using Task = async_simple::coro::Lazy<T>;

template <class T = void>
using Future = async_simple::Future<T>;

template <class Awaitable>
using TaskValue = typename std::decay_t<Awaitable>::ValueType;

} // namespace async_uv
