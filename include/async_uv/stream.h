#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "async_uv/task.h"

namespace async_uv {

template <typename T>
class Stream {
public:
    using value_type = T;
    using next_type = std::optional<value_type>;
    using task_type = Task<next_type>;
    using next_fn_type = std::function<task_type()>;

    Stream() = default;

    explicit Stream(next_fn_type next_fn) : state_(std::make_shared<State>(std::move(next_fn))) {}

    bool valid() const noexcept {
        return state_ != nullptr;
    }

    // 拉取下一个元素；返回 std::nullopt 表示流结束。
    // 同一个 Stream 实例同一时刻只允许一个 next() 在执行。
    task_type next() const {
        auto state = state_;
        if (!state || !state->next_fn) {
            co_return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->ended) {
                co_return std::nullopt;
            }
            if (state->in_flight) {
                throw std::runtime_error("stream next() already in progress");
            }
            state->in_flight = true;
        }

        try {
            auto value = co_await state->next_fn();

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->in_flight = false;
                if (!value) {
                    state->ended = true;
                }
            }

            co_return value;
        } catch (...) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->in_flight = false;
            throw;
        }
    }

private:
    struct State {
        explicit State(next_fn_type next_fn_in) : next_fn(std::move(next_fn_in)) {}

        next_fn_type next_fn;
        std::mutex mutex;
        bool in_flight = false;
        bool ended = false;
    };

    std::shared_ptr<State> state_;
};

} // namespace async_uv
