#include "flux/timer.h"

#include <stdexcept>
#include <system_error>

namespace flux {

struct SteadyTimer::State {
    Runtime *runtime = nullptr;
    exec::asio::asio_impl::steady_timer timer;
    bool closed = false;

    explicit State(Runtime *rt) : runtime(rt), timer(rt->executor()) {}
};

SteadyTimer::SteadyTimer(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

SteadyTimer::~SteadyTimer() {
    if (state_) {
        state_->timer.cancel();
        state_->closed = true;
    }
}

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
    co_return SteadyTimer(std::make_shared<State>(runtime));
}

Task<SteadyTimer> SteadyTimer::create(std::chrono::milliseconds delay) {
    auto timer = co_await create();
    (void)co_await timer.expires_after(delay);
    co_return timer;
}

bool SteadyTimer::valid() const noexcept {
    return state_ != nullptr && !state_->closed;
}

Task<std::size_t> SteadyTimer::expires_after(std::chrono::milliseconds delay) {
    if (!valid()) {
        throw std::runtime_error("SteadyTimer is not valid");
    }
    co_return state_->timer.expires_after(delay);
}

Task<std::size_t> SteadyTimer::expires_at(std::chrono::steady_clock::time_point deadline) {
    if (!valid()) {
        throw std::runtime_error("SteadyTimer is not valid");
    }
    co_return state_->timer.expires_at(deadline);
}

Task<bool> SteadyTimer::wait() {
    if (!valid()) {
        co_return false;
    }

    try {
        co_await state_->timer.async_wait(exec::asio::use_sender);
        co_return true;
    } catch (const std::system_error &error) {
        if (error.code() == exec::asio::asio_impl::error::operation_aborted) {
            co_return false;
        }
        throw;
    }
}

Task<std::size_t> SteadyTimer::cancel() {
    if (!valid()) {
        co_return 0;
    }
    co_return state_->timer.cancel();
}

Task<void> SteadyTimer::close() {
    if (!state_) {
        co_return;
    }
    (void)state_->timer.cancel();
    state_->closed = true;
    state_.reset();
}

} // namespace flux
