#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

#include "async_uv/runtime.h"

namespace async_uv {

class SteadyTimer {
public:
    struct State;

    SteadyTimer() = default;
    explicit SteadyTimer(std::shared_ptr<State> state) noexcept;
    ~SteadyTimer();

    SteadyTimer(const SteadyTimer &) = delete;
    SteadyTimer &operator=(const SteadyTimer &) = delete;
    SteadyTimer(SteadyTimer &&other) noexcept;
    SteadyTimer &operator=(SteadyTimer &&other) noexcept;

    static Task<SteadyTimer> create();
    static Task<SteadyTimer> create(std::chrono::milliseconds delay);

    bool valid() const noexcept;

    Task<std::size_t> expires_after(std::chrono::milliseconds delay);
    Task<std::size_t> expires_at(std::chrono::steady_clock::time_point deadline);
    Task<bool> wait();
    Task<std::size_t> cancel();
    Task<void> close();

private:
    std::shared_ptr<State> state_;
};

} // namespace async_uv
