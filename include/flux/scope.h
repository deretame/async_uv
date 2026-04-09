#pragma once

#include <utility>

#include <exec/start_detached.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include "flux/task.h"

namespace flux::scope {

template <typename... Senders>
auto all(Senders &&...senders) {
    return stdexec::when_all(std::forward<Senders>(senders)...);
}

template <typename... Senders>
auto any(Senders &&...senders) {
    return exec::when_any(std::forward<Senders>(senders)...);
}

} // namespace flux::scope

namespace flux {

class TaskScope {
public:
    template <typename Sender>
    bool spawn(Sender &&sender) {
        exec::start_detached(std::forward<Sender>(sender));
        return true;
    }

    template <typename... Senders>
    auto all(Senders &&...senders) {
        return scope::all(std::forward<Senders>(senders)...);
    }
};

template <typename Func>
auto with_task_scope(Func &&func) -> Task<TaskValue<decltype(func(std::declval<TaskScope &>()))>> {
    TaskScope scope;
    co_return co_await func(scope);
}

} // namespace flux
