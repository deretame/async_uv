#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <async_simple/Signal.h>

#include "bridge.h"

namespace async_uv::detail {

template <typename Result>
inline Outcome<Result> make_signal_cancel(std::string message) {
    return make_exception<Result>(std::make_exception_ptr(
        async_simple::SignalException(async_simple::Terminate, std::move(message))));
}

template <typename Result>
struct CancellableOperation {
    Runtime *runtime = nullptr;
    std::unique_ptr<async_simple::Slot> slot;
    Completion<Result> complete;
    bool completed = false;
    bool cancel_requested = false;

    void bind_signal(const std::shared_ptr<async_simple::Signal> &signal) {
        if (signal == nullptr) {
            slot.reset();
            return;
        }

        slot = std::make_unique<async_simple::Slot>(signal.get());
    }

    void finish(Outcome<Result> outcome) {
        if (completed) {
            return;
        }

        completed = true;
        auto done = std::move(complete);
        done(std::move(outcome));
    }

    void finish_cancel(std::string message) {
        cancel_requested = true;
        finish(make_signal_cancel<Result>(std::move(message)));
    }

    void finish_exception(std::exception_ptr error) {
        finish(make_exception<Result>(std::move(error)));
    }

    void finish_uv_error(const char *where, int code) {
        finish(make_uv_error<Result>(where, code));
    }

    template <typename Value>
    void finish_value(Value &&value)
        requires(!std::is_void_v<Result>)
    {
        finish(make_success(std::forward<Value>(value)));
    }

    void finish_value()
        requires(std::is_void_v<Result>)
    {
        finish(make_success());
    }
};

template <typename Result, typename Op, typename CancelFn>
bool install_terminate_handler(const std::shared_ptr<Op> &op, CancelFn &&cancel_fn) {
    if (op->slot == nullptr) {
        return true;
    }

    if (!async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
            op->slot.get(),
            [runtime = op->runtime,
             weak = std::weak_ptr<Op>(op),
             cancel_fn = std::decay_t<CancelFn>(std::forward<CancelFn>(cancel_fn))](
                async_simple::SignalType, async_simple::Signal *) mutable {
                if (runtime == nullptr) {
                    return;
                }

                runtime->post([weak, cancel_fn = std::move(cancel_fn)]() mutable {
                    auto op = weak.lock();
                    if (!op || op->completed) {
                        return;
                    }

                    cancel_fn(*op);
                });
            })) {
        return false;
    }

    return true;
}

template <typename Request, typename Op>
void attach_shared(Request &request, const std::shared_ptr<Op> &op) {
    request.data = new std::shared_ptr<Op>(op);
}

template <typename Op, typename Request>
std::shared_ptr<Op> detach_shared(Request *request) {
    auto *holder = static_cast<std::shared_ptr<Op> *>(request->data);
    auto op = *holder;
    delete holder;
    request->data = nullptr;
    return op;
}

} // namespace async_uv::detail
