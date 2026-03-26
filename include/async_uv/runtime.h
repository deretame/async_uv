#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <async_simple/Executor.h>
#include <async_simple/Promise.h>
#include <async_simple/coro/SyncAwait.h>
#include <uv.h>

#include "async_uv/error.h"
#include "async_uv/task.h"

namespace async_uv {

class Runtime;

struct RuntimeOptions {
    std::string name = "async_uv";
    // libuv 线程池是进程级配置；同进程内多个 Runtime 必须保持一致。
    std::optional<unsigned int> uv_threadpool_size = std::nullopt;
};

struct TraceEvent {
    const char *category = "";
    const char *name = "";
    int code = 0;
    std::size_t value = 0;
};

using TraceHook = std::function<void(const TraceEvent &)>;

void set_trace_hook(TraceHook hook);
void reset_trace_hook();
void emit_trace_event(TraceEvent event) noexcept;

template <typename Func>
using BlockingValue =
    std::conditional_t<std::is_void_v<std::invoke_result_t<std::decay_t<Func> &>>,
                       void,
                       std::remove_cvref_t<std::invoke_result_t<std::decay_t<Func> &>>>;

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Runtime &runtime, Func &&func);

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Func &&func);

class Runtime final : public async_simple::Executor {
public:
    class Builder {
    public:
        Builder &name(std::string value) {
            options_.name = std::move(value);
            return *this;
        }

        Builder &uv_threadpool_size(unsigned int value) {
            options_.uv_threadpool_size = value;
            return *this;
        }

        Builder &uv_threadpool_size(std::optional<unsigned int> value) {
            options_.uv_threadpool_size = value;
            return *this;
        }

        operator RuntimeOptions() const & {
            return options_;
        }

        operator RuntimeOptions() && {
            return std::move(options_);
        }

    private:
        RuntimeOptions options_;
    };

    using Func = async_simple::Executor::Func;
    using Context = async_simple::Executor::Context;
    using Duration = async_simple::Executor::Duration;

    // Builder 风格入口：默认值可演进，便于保持 API 稳定。
    static Builder build() {
        return Builder{};
    }

    explicit Runtime(RuntimeOptions options = {});
    ~Runtime() override;

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Runtime(Runtime &&) = delete;
    Runtime &operator=(Runtime &&) = delete;

    uv_loop_t *loop() noexcept {
        return &loop_;
    }
    const uv_loop_t *loop() const noexcept {
        return &loop_;
    }
    async_simple::Executor *executor() noexcept {
        return this;
    }
    const async_simple::Executor *executor() const noexcept {
        return this;
    }

    template <typename Lazy>
    decltype(auto) block_on(Lazy &&lazy) {
        return async_simple::coro::syncAwait(std::forward<Lazy>(lazy), this);
    }

    template <typename Lazy>
    Future<TaskValue<Lazy>> spawn(Lazy &&lazy) {
        using ValueType = TaskValue<Lazy>;

        async_simple::Promise<ValueType> promise;
        auto future = promise.getFuture();

        std::forward<Lazy>(lazy).via(this).start(
            [promise = std::move(promise)](async_simple::Try<ValueType> result) mutable {
                if (result.hasError()) {
                    promise.setException(result.getException());
                    return;
                }

                if constexpr (std::is_void_v<ValueType>) {
                    promise.setValue();
                } else {
                    promise.setValue(std::move(result).value());
                }
            });

        return future;
    }

    template <typename Func>
    Future<BlockingValue<Func>> spawn_blocking(Func &&func) {
        return ::async_uv::spawn_blocking(*this, std::forward<Func>(func));
    }

    bool schedule(Func func) override;
    bool currentThreadInExecutor() const override;
    async_simple::ExecutorStat stat() const override;
    size_t currentContextId() const override;
    Context checkout() override;
    bool checkin(Func func, Context ctx, async_simple::ScheduleOptions opts) override;

    void post(std::function<void()> fn);

    static Runtime *current() noexcept;
    static uv_loop_t *current_loop() noexcept;

protected:
    void
    schedule(Func func, Duration dur, uint64_t schedule_info, async_simple::Slot *slot) override;

private:
    void drain();

    uv_loop_t loop_{};
    uv_async_t async_{};
    mutable std::mutex queue_mutex_;
    std::deque<std::function<void()>> queue_;
    std::thread loop_thread_;
    std::thread::id loop_thread_id_{};
};

Runtime *try_current_runtime() noexcept;
uv_loop_t *try_current_loop() noexcept;

template <typename Lazy>
Future<TaskValue<Lazy>> spawn(Runtime &runtime, Lazy &&lazy) {
    return runtime.spawn(std::forward<Lazy>(lazy));
}

template <typename Lazy>
Future<TaskValue<Lazy>> spawn(Lazy &&lazy) {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error("async_uv::spawn requires a current async_uv::Runtime");
    }
    return runtime->spawn(std::forward<Lazy>(lazy));
}

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Runtime &runtime, Func &&func) {
    using Work = std::decay_t<Func>;
    using ValueType = BlockingValue<Func>;

    struct WorkOp {
        uv_work_t req{};
        Work work;
        std::shared_ptr<async_simple::Promise<ValueType>> promise;
        async_simple::Try<ValueType> result;
    };

    auto promise = std::make_shared<async_simple::Promise<ValueType>>();
    auto future = promise->getFuture().via(&runtime);

    runtime.post(
        [&runtime, work = Work(std::forward<Func>(func)), promise = std::move(promise)]() mutable {
            auto *op = new WorkOp{{}, std::move(work), std::move(promise), {}};
            op->req.data = op;

            const int rc = uv_queue_work(
                runtime.loop(),
                &op->req,
                [](uv_work_t *req) {
                    auto *op = static_cast<WorkOp *>(req->data);
                    try {
                        if constexpr (std::is_void_v<ValueType>) {
                            std::invoke(op->work);
                            op->result = async_simple::Try<void>();
                        } else {
                            op->result = async_simple::Try<ValueType>(std::invoke(op->work));
                        }
                    } catch (...) {
                        op->result = async_simple::Try<ValueType>(std::current_exception());
                    }
                },
                [](uv_work_t *req, int status) {
                    std::unique_ptr<WorkOp> op(static_cast<WorkOp *>(req->data));

                    if (status < 0) {
                        op->promise->setException(
                            std::make_exception_ptr(Error("uv_queue_work", status)));
                        return;
                    }

                    if (op->result.hasError()) {
                        op->promise->setException(op->result.getException());
                        return;
                    }

                    if constexpr (std::is_void_v<ValueType>) {
                        op->promise->setValue();
                    } else {
                        op->promise->setValue(std::move(op->result).value());
                    }
                });

            if (rc < 0) {
                promise->setException(std::make_exception_ptr(Error("uv_queue_work", rc)));
                delete op;
            }
        });

    return future;
}

template <typename Func>
Future<BlockingValue<Func>> spawn_blocking(Func &&func) {
    auto *runtime = Runtime::current();
    if (runtime == nullptr) {
        throw std::runtime_error("async_uv::spawn_blocking requires a current async_uv::Runtime");
    }
    return spawn_blocking(*runtime, std::forward<Func>(func));
}

Task<Runtime *> get_current_runtime();
Task<uv_loop_t *> get_current_loop();
Task<void> sleep_for(std::chrono::milliseconds delay);

} // namespace async_uv
