#include "async_uv/timer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include "async_uv/cancel.h"
#include "detail/bridge.h"
#include "detail/cancel.h"

namespace async_uv {

namespace {

using detail::Completion;

struct PendingClose {
    Completion<void> complete;
};

} // namespace

struct SteadyTimer::State {
    explicit State(Runtime *runtime_in) : runtime(runtime_in) {}

    struct PendingWait : detail::CancellableOperation<bool> {};

    Runtime *runtime = nullptr;
    uv_timer_t handle{};
    bool initialized = false;
    bool closing = false;
    bool closed = false;
    std::uint64_t due_ms = 0;
    std::shared_ptr<PendingWait> pending_wait;
    std::shared_ptr<PendingClose> pending_close;
};

namespace {

template <typename State>
void finish_close(State *state) {
    state->closed = true;
    state->closing = false;
    auto pending = std::move(state->pending_close);
    if (pending) {
        pending->complete(detail::make_success());
    }
}

void complete_pending_wait(SteadyTimer::State *state, bool fired) {
    auto pending = std::move(state->pending_wait);
    if (pending) {
        pending->finish_value(fired);
    }
}

Task<SteadyTimer> create_timer(Runtime &runtime, std::uint64_t due_ms) {
    auto state = std::make_shared<SteadyTimer::State>(&runtime);
    state->due_ms = due_ms;

    co_await detail::post_task<void>(runtime, [state](Completion<void> complete) mutable {
        const int rc = uv_timer_init(state->runtime->loop(), &state->handle);
        if (rc < 0) {
            complete(detail::make_uv_error<void>("uv_timer_init", rc));
            return;
        }

        state->initialized = true;
        state->handle.data = state.get();
        complete(detail::make_success());
    });

    co_return SteadyTimer(std::move(state));
}

Task<std::size_t> set_due_ms(const std::shared_ptr<SteadyTimer::State> &state,
                             std::uint64_t due_ms) {
    co_return co_await detail::post_task<std::size_t>(
        *state->runtime, [state, due_ms](Completion<std::size_t> complete) mutable {
            if (!state->initialized || state->closed ||
                uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
                complete(detail::make_runtime_error<std::size_t>("timer is closed"));
                return;
            }

            std::size_t canceled = 0;
            if (state->pending_wait) {
                uv_timer_stop(&state->handle);
                complete_pending_wait(state.get(), false);
                canceled = 1;
            }

            state->due_ms = due_ms;
            complete(detail::make_success(canceled));
        });
}

} // namespace

SteadyTimer::SteadyTimer(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

SteadyTimer::~SteadyTimer() = default;

SteadyTimer::SteadyTimer(SteadyTimer &&other) noexcept : state_(std::move(other.state_)) {}

SteadyTimer &SteadyTimer::operator=(SteadyTimer &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<SteadyTimer> SteadyTimer::create() {
    auto *runtime = co_await get_current_runtime();
    co_return co_await create_timer(*runtime, 0);
}

Task<SteadyTimer> SteadyTimer::create(std::chrono::milliseconds delay) {
    auto *runtime = co_await get_current_runtime();
    const auto due_ms = static_cast<std::uint64_t>(std::max<long long>(delay.count(), 0));
    co_return co_await create_timer(*runtime, due_ms);
}

bool SteadyTimer::valid() const noexcept {
    return state_ && state_->initialized && !state_->closing && !state_->closed;
}

Task<std::size_t> SteadyTimer::expires_after(std::chrono::milliseconds delay) {
    if (!state_) {
        throw std::runtime_error("timer is empty");
    }

    const auto due_ms = static_cast<std::uint64_t>(std::max<long long>(delay.count(), 0));
    co_return co_await set_due_ms(state_, due_ms);
}

Task<std::size_t> SteadyTimer::expires_at(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
        co_return co_await expires_after(std::chrono::milliseconds(0));
    }

    co_return co_await expires_after(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

Task<bool> SteadyTimer::wait() {
    if (!state_) {
        throw std::runtime_error("timer is empty");
    }

    auto state = state_;
    auto signal = co_await get_current_signal();
    co_return co_await detail::post_task<bool>(
        *state->runtime, [state, signal = std::move(signal)](Completion<bool> complete) mutable {
            if (!state->initialized || state->closed ||
                uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
                complete(detail::make_runtime_error<bool>("timer is closed"));
                return;
            }

            if (state->pending_wait) {
                complete(
                    detail::make_runtime_error<bool>("only one pending timer wait is supported"));
                return;
            }

            state->pending_wait = std::make_shared<State::PendingWait>();
            state->pending_wait->runtime = state->runtime;
            state->pending_wait->bind_signal(signal);
            state->pending_wait->complete = std::move(complete);

            constexpr auto kCancelMessage = "uv_timer_start canceled";
            if (!detail::install_terminate_handler<bool>(
                    state->pending_wait, [state](State::PendingWait &pending) {
                        uv_timer_stop(&state->handle);
                        if (state->pending_wait && state->pending_wait.get() == &pending) {
                            state->pending_wait.reset();
                        }
                        pending.finish_cancel("uv_timer_start canceled");
                    })) {
                auto pending = std::move(state->pending_wait);
                pending->finish_cancel(kCancelMessage);
                return;
            }

            const int rc = uv_timer_start(
                &state->handle,
                [](uv_timer_t *handle) {
                    auto *state = static_cast<SteadyTimer::State *>(handle->data);
                    uv_timer_stop(handle);
                    complete_pending_wait(state, true);
                },
                state->due_ms,
                0);
            if (rc < 0) {
                auto pending = std::move(state->pending_wait);
                pending->finish_uv_error("uv_timer_start", rc);
            }
        });
}

Task<std::size_t> SteadyTimer::cancel() {
    if (!state_) {
        co_return 0;
    }

    auto state = state_;
    co_return co_await detail::post_task<std::size_t>(
        *state->runtime, [state](Completion<std::size_t> complete) mutable {
            if (!state->initialized || state->closed ||
                uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
                complete(detail::make_success<std::size_t>(0));
                return;
            }

            std::size_t canceled = 0;
            if (state->pending_wait) {
                uv_timer_stop(&state->handle);
                complete_pending_wait(state.get(), false);
                canceled = 1;
            }

            complete(detail::make_success(canceled));
        });
}

Task<void> SteadyTimer::close() {
    if (!state_ || state_->closed) {
        co_return;
    }

    auto state = state_;
    co_await detail::post_task<void>(*state->runtime, [state](Completion<void> complete) mutable {
        if (state->closed || !state->initialized ||
            uv_is_closing(reinterpret_cast<uv_handle_t *>(&state->handle))) {
            complete(detail::make_success());
            return;
        }

        if (state->pending_wait) {
            uv_timer_stop(&state->handle);
            complete_pending_wait(state.get(), false);
        }

        state->closing = true;
        state->pending_close = std::make_shared<PendingClose>(PendingClose{std::move(complete)});
        uv_close(reinterpret_cast<uv_handle_t *>(&state->handle), [](uv_handle_t *handle) {
            finish_close(static_cast<SteadyTimer::State *>(handle->data));
        });
    });
}

Task<void> sleep_until(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
        co_return;
    }

    co_await sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

} // namespace async_uv
