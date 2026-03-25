#include "async_uv/runtime.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <async_simple/coro/Sleep.h>

#include "async_uv/error.h"
#include "detail/bridge.h"

namespace async_uv {

namespace {

thread_local Runtime *tls_current_runtime = nullptr;

struct TimerOp {
    Runtime::Func func;
    uv_timer_t timer{};
    bool completed = false;
};

void release_timer_holder(uv_handle_t *handle) {
    delete static_cast<std::shared_ptr<TimerOp> *>(handle->data);
}

} // namespace

Runtime::Runtime(std::string name) : async_simple::Executor(std::move(name)) {
    throw_if_uv_error(uv_loop_init(&loop_), "uv_loop_init");

    async_.data = this;
    const int rc = uv_async_init(&loop_, &async_, [](uv_async_t *handle) {
        static_cast<Runtime *>(handle->data)->drain();
    });

    if (rc < 0) {
        uv_loop_close(&loop_);
        throw_uv_error("uv_async_init", rc);
    }

    loop_thread_ = std::thread([this] {
        loop_thread_id_ = std::this_thread::get_id();
        tls_current_runtime = this;
        uv_run(&loop_, UV_RUN_DEFAULT);
        tls_current_runtime = nullptr;
    });
}

Runtime::~Runtime() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.emplace_back([this] {
            uv_walk(
                &loop_,
                [](uv_handle_t *handle, void *) {
                    if (!uv_is_closing(handle)) {
                        uv_close(handle, nullptr);
                    }
                },
                nullptr);
        });
    }

    uv_async_send(&async_);

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }

    while (uv_loop_close(&loop_) == UV_EBUSY) {
        uv_run(&loop_, UV_RUN_DEFAULT);
    }
}

bool Runtime::schedule(Func func) {
    post(std::move(func));
    return true;
}

bool Runtime::currentThreadInExecutor() const {
    return std::this_thread::get_id() == loop_thread_id_;
}

async_simple::ExecutorStat Runtime::stat() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    async_simple::ExecutorStat stat;
    stat.pendingTaskCount = queue_.size();
    return stat;
}

size_t Runtime::currentContextId() const {
    return currentThreadInExecutor() ? 1 : 0;
}

Runtime::Context Runtime::checkout() {
    return currentThreadInExecutor() ? reinterpret_cast<Context>(this)
                                     : async_simple::Executor::NULLCTX;
}

bool Runtime::checkin(Func func, Context ctx, async_simple::ScheduleOptions opts) {
    if (ctx == reinterpret_cast<Context>(this) && currentThreadInExecutor() && opts.prompt) {
        func();
        return true;
    }
    return schedule(std::move(func));
}

void Runtime::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(fn));
    }

    uv_async_send(&async_);
}

Runtime *Runtime::current() noexcept {
    return tls_current_runtime;
}

uv_loop_t *Runtime::current_loop() noexcept {
    auto *runtime = current();
    return runtime == nullptr ? nullptr : runtime->loop();
}

Runtime *try_current_runtime() noexcept {
    return Runtime::current();
}

uv_loop_t *try_current_loop() noexcept {
    return Runtime::current_loop();
}

void Runtime::schedule(Func func, Duration dur, uint64_t, async_simple::Slot *slot) {
    auto op = std::make_shared<TimerOp>();
    op->func = std::move(func);

    if (slot != nullptr &&
        !async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
            slot,
            [this, weak = std::weak_ptr<TimerOp>(op)](async_simple::SignalType,
                                                      async_simple::Signal *) mutable {
                auto op = weak.lock();
                if (op == nullptr) {
                    return;
                }

                post([op = std::move(op)]() mutable {
                    if (op->completed) {
                        return;
                    }

                    op->completed = true;
                    auto done = std::move(op->func);
                    if (op->timer.data != nullptr) {
                        uv_timer_stop(&op->timer);
                        uv_close(reinterpret_cast<uv_handle_t *>(&op->timer), release_timer_holder);
                    }
                    done();
                });
            })) {
        schedule(std::move(op->func));
        return;
    }

    post([this, op = std::move(op), dur]() mutable {
        if (op->completed) {
            return;
        }

        int rc = uv_timer_init(loop(), &op->timer);
        if (rc < 0) {
            op->completed = true;
            auto done = std::move(op->func);
            done();
            return;
        }

        op->timer.data = new std::shared_ptr<TimerOp>(op);

        const auto timeout = static_cast<uint64_t>(std::max<int64_t>(dur.count(), 0) / 1000);
        rc = uv_timer_start(
            &op->timer,
            [](uv_timer_t *handle) {
                auto holder = static_cast<std::shared_ptr<TimerOp> *>(handle->data);
                auto op = *holder;
                if (op->completed) {
                    return;
                }

                op->completed = true;
                auto done = std::move(op->func);
                uv_timer_stop(handle);
                uv_close(reinterpret_cast<uv_handle_t *>(handle), release_timer_holder);
                done();
            },
            timeout,
            0);

        if (rc < 0) {
            if (!op->completed) {
                op->completed = true;
                auto done = std::move(op->func);
                uv_close(reinterpret_cast<uv_handle_t *>(&op->timer), release_timer_holder);
                done();
                return;
            }

            uv_close(reinterpret_cast<uv_handle_t *>(&op->timer), release_timer_holder);
        }
    });
}

void Runtime::drain() {
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending.swap(queue_);
    }

    for (auto &fn : pending) {
        fn();
    }
}

Task<Runtime *> get_current_runtime() {
    auto *executor = co_await async_simple::CurrentExecutor{};
    auto *runtime = dynamic_cast<Runtime *>(executor);
    if (runtime == nullptr) {
        throw std::runtime_error("current executor is not async_uv::Runtime");
    }
    co_return runtime;
}

Task<uv_loop_t *> get_current_loop() {
    auto *runtime = co_await get_current_runtime();
    co_return runtime->loop();
}

Task<void> sleep_for(std::chrono::milliseconds delay) {
    co_await async_simple::coro::sleep(delay);
}

} // namespace async_uv
