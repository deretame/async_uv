#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <async_simple/Collect.h>
#include <async_simple/Promise.h>
#include <async_simple/coro/Collect.h>

#include "async_uv/cancel.h"
#include "async_uv/message.h"

namespace async_uv {

class TaskScope {
public:
    struct State;

    template <typename Value>
    using ScopedValue = std::conditional_t<std::is_void_v<Value>, std::monostate, Value>;

    template <typename Variant>
    struct RaceResult {
        std::size_t index = 0;
        Variant value;
    };

    template <typename Range>
    using RangeTask = std::remove_cvref_t<std::ranges::range_reference_t<Range>>;

    TaskScope() = default;
    explicit TaskScope(std::shared_ptr<State> state) noexcept;
    ~TaskScope();

    TaskScope(const TaskScope &) = delete;
    TaskScope &operator=(const TaskScope &) = delete;
    TaskScope(TaskScope &&other) noexcept;
    TaskScope &operator=(TaskScope &&other) noexcept;

    static Task<TaskScope> create();
    static Task<TaskScope> create(Runtime &runtime);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Runtime *runtime() const noexcept;

    CancellationSource &cancellation_source();
    const CancellationSource &cancellation_source() const;

    void cancel() noexcept;
    Task<void> join();
    Task<void> close();

    template <typename Lazy>
    Future<TaskValue<Lazy>> spawn(Lazy &&lazy);

    template <typename Func>
    Future<BlockingValue<Func>> spawn_blocking(Func &&func);

    template <typename... Lazies>
    Task<std::tuple<ScopedValue<TaskValue<Lazies>>...>> all(Lazies &&...lazies);

    template <typename Range>
        requires(std::ranges::input_range<Range> &&
                 requires { typename TaskValue<RangeTask<Range>>; })
    Task<std::vector<ScopedValue<TaskValue<RangeTask<Range>>>>> all(Range &&range);

    template <typename... Lazies>
    Task<RaceResult<std::variant<ScopedValue<TaskValue<Lazies>>...>>> race(Lazies &&...lazies);

    template <typename Range>
        requires(std::ranges::input_range<Range> &&
                 requires { typename TaskValue<RangeTask<Range>>; })
    Task<RaceResult<ScopedValue<TaskValue<RangeTask<Range>>>>> race(Range &&range);

    template <typename Rep, typename Period, typename Lazy>
    Task<TaskValue<Lazy>> with_timeout(std::chrono::duration<Rep, Period> timeout, Lazy &&lazy);

    template <typename First, typename... Rest>
    Task<TaskValue<First>> any_success(First &&first, Rest &&...rest);

    template <typename Range>
        requires(std::ranges::input_range<Range> &&
                 requires { typename TaskValue<RangeTask<Range>>; })
    Task<TaskValue<RangeTask<Range>>> any_success(Range &&range);

    template <typename Clock, typename Duration, typename Lazy>
    Task<TaskValue<Lazy>> with_deadline(std::chrono::time_point<Clock, Duration> deadline,
                                        Lazy &&lazy);

private:
    template <typename ValueType, typename Lazy>
    static Task<void>
    run_scoped_child(std::shared_ptr<State> state,
                     std::shared_ptr<async_simple::Signal> signal,
                     std::shared_ptr<async_simple::Promise<ValueType>> result_promise,
                     Lazy lazy);

    template <typename Try>
    struct TryValue;

    template <typename Value>
    struct TryValue<async_simple::Try<Value>> {
        using type = Value;
    };

    template <typename Lazy, typename Range>
    static std::vector<Lazy> materialize_task_range(Range &&range);

    template <std::size_t Index = 0, typename StorageTuple, typename FutureTuple>
    static Task<void> fill_all_results(StorageTuple &storage, FutureTuple &futures);

    template <typename ResultTuple, typename StorageTuple, std::size_t... Index>
    static ResultTuple materialize_all_results(StorageTuple &storage,
                                               std::index_sequence<Index...>);

    template <typename... Lazies>
    static Task<std::tuple<ScopedValue<TaskValue<Lazies>>...>>
    run_all_task(std::shared_ptr<State> state, TaskScope *scope, Lazies... lazies);

    template <typename Lazy>
    static Task<std::vector<ScopedValue<TaskValue<Lazy>>>>
    run_all_range_task(TaskScope *scope, std::vector<Lazy> lazies);

    template <typename... Lazies>
    static Task<RaceResult<std::variant<ScopedValue<TaskValue<Lazies>>...>>>
    run_race_task(Lazies... lazies);

    template <typename Lazy>
    static Task<RaceResult<ScopedValue<TaskValue<Lazy>>>> run_race_range_task(std::vector<Lazy> lazies);

    template <typename Rep, typename Period, typename Lazy>
    static Task<TaskValue<Lazy>> run_timeout_task(std::chrono::duration<Rep, Period> timeout,
                                                  Lazy lazy);

    template <typename ValueType, typename... Lazies>
    static Task<ValueType> run_any_success_task(Runtime *runtime, Lazies... lazies);

    template <typename ValueType, typename Lazy>
    static Task<ValueType> run_any_success_range_task(Runtime *runtime, std::vector<Lazy> lazies);

    template <typename ValueType, typename Completion, typename Lazy>
    static Task<void> run_any_success_child(std::shared_ptr<async_simple::Signal> signal,
                                            MessageSender<Completion> sender,
                                            Lazy lazy);

    template <typename Clock, typename Duration, typename Lazy>
    static Task<TaskValue<Lazy>>
    run_deadline_task(std::chrono::time_point<Clock, Duration> deadline, Lazy lazy);

    template <typename RawVariant, typename ResultVariant, std::size_t Index = 0>
    static RaceResult<ResultVariant> unwrap_race_result(RawVariant &&result);

    static std::shared_ptr<State> require_state(const std::shared_ptr<State> &state);
    static std::shared_ptr<State> require_spawnable_state(const std::shared_ptr<State> &state);
    static Runtime *state_runtime(const std::shared_ptr<State> &state) noexcept;
    static CancellationSource &state_source(const std::shared_ptr<State> &state) noexcept;
    static const CancellationSource &
    state_source_const(const std::shared_ptr<State> &state) noexcept;
    static void track_completion(const std::shared_ptr<State> &state, Future<void> completion);
    static void record_failure(const std::shared_ptr<State> &state,
                               std::exception_ptr error) noexcept;
    static std::exception_ptr make_scope_cancel_error();
    static bool is_cancel_exception(const std::exception_ptr &error) noexcept;
    static bool is_scope_cancellation(const std::shared_ptr<State> &state,
                                      const std::exception_ptr &error) noexcept;
    static Task<void> join_state(std::shared_ptr<State> state);
    static Task<void> close_state(std::shared_ptr<State> state);
    static Task<void> cleanup_state_task(std::shared_ptr<State> state);
    static void cleanup_state(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;
};

template <typename Func>
using ScopedTask = std::invoke_result_t<std::decay_t<Func> &, TaskScope &>;

template <typename Func>
Task<TaskValue<ScopedTask<Func>>> with_task_scope(Func &&func);

} // namespace async_uv

#include "async_uv/scope_impl.hpp"
