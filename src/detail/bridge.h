#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>

#include "async_uv/error.h"
#include "async_uv/runtime.h"

namespace async_uv::detail {

template <typename T>
struct Outcome {
    std::optional<T> value;
    std::exception_ptr error;
};

template <>
struct Outcome<void> {
    std::exception_ptr error;
};

template <typename T>
using Completion = std::function<void(Outcome<T>)>;

template <typename T>
Outcome<T> make_success(T value) {
    Outcome<T> outcome;
    outcome.value = std::move(value);
    return outcome;
}

inline Outcome<void> make_success() {
    return {};
}

template <typename T>
Outcome<T> make_exception(std::exception_ptr error) {
    Outcome<T> outcome;
    outcome.error = std::move(error);
    return outcome;
}

template <typename T>
Outcome<T> make_runtime_error(std::string message) {
    return make_exception<T>(std::make_exception_ptr(std::runtime_error(std::move(message))));
}

template <typename T>
Outcome<T> make_uv_error(const char *where, int code) {
    return make_exception<T>(std::make_exception_ptr(Error(where, code)));
}

template <typename T>
void fulfill(async_simple::Promise<T> &promise, Outcome<T> &&outcome) {
    if (outcome.error) {
        promise.setException(std::move(outcome.error));
        return;
    }
    promise.setValue(std::move(*outcome.value));
}

inline void fulfill(async_simple::Promise<void> &promise, Outcome<void> &&outcome) {
    if (outcome.error) {
        promise.setException(std::move(outcome.error));
        return;
    }
    promise.setValue();
}

template <typename T, typename Starter>
Task<T> post_task(Runtime &runtime, Starter starter) {
    auto promise = std::make_shared<async_simple::Promise<T>>();
    auto future = promise->getFuture().via(&runtime);

    runtime.post([promise, starter = std::move(starter)]() mutable {
        try {
            starter([promise](Outcome<T> outcome) mutable {
                fulfill(*promise, std::move(outcome));
            });
        } catch (...) {
            promise->setException(std::current_exception());
        }
    });

    co_return co_await std::move(future);
}

} // namespace async_uv::detail
