#pragma once

#include <type_traits>

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace flux {

class Runtime;

namespace execution {

struct get_runtime_t : stdexec::__query<get_runtime_t> {
    using stdexec::__query<get_runtime_t>::operator();

    constexpr auto operator()() const noexcept {
        return stdexec::read_env(*this);
    }

    static consteval auto query(stdexec::forwarding_query_t) noexcept -> bool {
        return true;
    }
};

inline constexpr get_runtime_t get_runtime{};

} // namespace execution

template <class T>
class TaskContext final : public exec::default_task_context<T> {
    using Base = exec::default_task_context<T>;

public:
    template <class ThisPromise>
    using promise_context_t = TaskContext;

    template <class ThisPromise, class ParentPromise = void>
    using awaiter_context_t = typename Base::template awaiter_context_t<ThisPromise, ParentPromise>;

    template <class ParentPromise>
    constexpr explicit TaskContext(ParentPromise &parent) noexcept
        : Base(parent) {
        if constexpr (requires { execution::get_runtime(stdexec::get_env(parent)); }) {
            runtime_ = execution::get_runtime(stdexec::get_env(parent));
        }
    }

    template <stdexec::scheduler Scheduler>
    constexpr explicit TaskContext(Scheduler &&scheduler)
        : Base(static_cast<Scheduler &&>(scheduler)) {}

    using Base::query;

    [[nodiscard]] constexpr auto query(execution::get_runtime_t) const noexcept -> Runtime * {
        return runtime_;
    }

private:
    Runtime *runtime_ = nullptr;
};

template <class T = void>
using Task = exec::basic_task<T, TaskContext<T>>;

template <class T>
struct task_value;

template <class T, class Context>
struct task_value<exec::basic_task<T, Context>> {
    using type = T;
};

template <class Awaitable>
using TaskValue = typename task_value<std::decay_t<Awaitable>>::type;

} // namespace flux
