#include "async_uv/scope.h"

#include <mutex>
#include <utility>
#include <vector>

#include <async_simple/Collect.h>
#include <async_simple/Signal.h>

namespace async_uv {

struct TaskScope::State {
    explicit State(Runtime *runtime_in) : runtime(runtime_in) {}

    Runtime *runtime = nullptr;
    CancellationSource source;
    std::mutex mutex;
    std::vector<Future<void>> completions;
    std::exception_ptr first_error;
    bool closed = false;
};

namespace {

bool is_terminate_signal(const std::exception_ptr &error) noexcept {
    if (!error) {
        return false;
    }

    try {
        std::rethrow_exception(error);
    } catch (const async_simple::SignalException &signal) {
        return signal.value() == async_simple::Terminate;
    } catch (...) {
        return false;
    }
}

} // namespace

TaskScope::TaskScope(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TaskScope::~TaskScope() {
    cleanup_state(std::move(state_));
}

TaskScope::TaskScope(TaskScope &&other) noexcept : state_(std::move(other.state_)) {}

TaskScope &TaskScope::operator=(TaskScope &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    cleanup_state(std::move(state_));
    state_ = std::move(other.state_);
    return *this;
}

Task<TaskScope> TaskScope::create() {
    auto *runtime = co_await get_current_runtime();
    co_return TaskScope(std::make_shared<State>(runtime));
}

Task<TaskScope> TaskScope::create(Runtime &runtime) {
    co_return TaskScope(std::make_shared<State>(&runtime));
}

bool TaskScope::valid() const noexcept {
    return state_ != nullptr;
}

Runtime *TaskScope::runtime() const noexcept {
    return state_runtime(state_);
}

CancellationSource &TaskScope::cancellation_source() {
    return state_source(require_state(state_));
}

const CancellationSource &TaskScope::cancellation_source() const {
    return state_source_const(require_state(state_));
}

void TaskScope::cancel() noexcept {
    if (state_) {
        (void)state_->source.cancel();
    }
}

Task<void> TaskScope::join() {
    co_await join_state(require_state(state_));
}

Task<void> TaskScope::close() {
    co_await close_state(require_state(state_));
}

std::shared_ptr<TaskScope::State> TaskScope::require_state(const std::shared_ptr<State> &state) {
    if (!state) {
        throw std::runtime_error("task scope is empty");
    }

    return state;
}

std::shared_ptr<TaskScope::State>
TaskScope::require_spawnable_state(const std::shared_ptr<State> &state) {
    auto current = require_state(state);

    std::lock_guard<std::mutex> lock(current->mutex);
    if (current->closed) {
        throw std::runtime_error("task scope is closed");
    }

    return current;
}

Runtime *TaskScope::state_runtime(const std::shared_ptr<State> &state) noexcept {
    return state ? state->runtime : nullptr;
}

CancellationSource &TaskScope::state_source(const std::shared_ptr<State> &state) noexcept {
    return state->source;
}

const CancellationSource &
TaskScope::state_source_const(const std::shared_ptr<State> &state) noexcept {
    return state->source;
}

void TaskScope::track_completion(const std::shared_ptr<State> &state, Future<void> completion) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        throw std::runtime_error("task scope is closed");
    }

    state->completions.push_back(std::move(completion));
}

void TaskScope::record_failure(const std::shared_ptr<State> &state,
                               std::exception_ptr error) noexcept {
    if (!state || !error) {
        return;
    }

    bool should_cancel = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->first_error) {
            state->first_error = error;
            should_cancel = true;
        }
    }

    if (should_cancel) {
        (void)state->source.cancel();
    }
}

std::exception_ptr TaskScope::make_scope_cancel_error() {
    return std::make_exception_ptr(
        async_simple::SignalException(async_simple::Terminate, "task scope canceled"));
}

bool TaskScope::is_cancel_exception(const std::exception_ptr &error) noexcept {
    return is_terminate_signal(error);
}

bool TaskScope::is_scope_cancellation(const std::shared_ptr<State> &state,
                                      const std::exception_ptr &error) noexcept {
    return state && state->source.cancellation_requested() && is_terminate_signal(error);
}

Task<void> TaskScope::join_state(std::shared_ptr<State> state) {
    std::vector<Future<void>> completions;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed = true;
        completions = std::move(state->completions);
    }

    if (!completions.empty()) {
        (void)co_await async_simple::collectAll(completions.begin(), completions.end());
    }

    std::exception_ptr first_error;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        first_error = state->first_error;
        state->first_error = nullptr;
    }

    if (first_error) {
        std::rethrow_exception(first_error);
    }
}

Task<void> TaskScope::close_state(std::shared_ptr<State> state) {
    (void)state->source.cancel();
    co_await join_state(std::move(state));
}

Task<void> TaskScope::cleanup_state_task(std::shared_ptr<State> state) {
    try {
        co_await close_state(std::move(state));
    } catch (...) {
    }
}

void TaskScope::cleanup_state(std::shared_ptr<State> state) noexcept {
    if (!state || !state->runtime) {
        return;
    }

    auto *runtime = state->runtime;

    try {
        if (runtime->currentThreadInExecutor()) {
            (void)runtime->spawn(cleanup_state_task(std::move(state)));
        } else {
            runtime->block_on(cleanup_state_task(std::move(state)));
        }
    } catch (...) {
    }
}

} // namespace async_uv
