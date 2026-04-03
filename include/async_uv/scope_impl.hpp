#pragma once

namespace async_uv {

template <typename Lazy>
Future<TaskValue<Lazy>> TaskScope::spawn(Lazy &&lazy) {
    using ValueType = TaskValue<Lazy>;
    using LazyType = std::decay_t<Lazy>;

    auto state = require_spawnable_state(state_);
    auto *runtime = state_runtime(state);

    auto result_promise = std::make_shared<async_simple::Promise<ValueType>>();
    auto result_future = result_promise->getFuture();
    auto signal = state_source(state).shared_signal();

    track_completion(state,
                     runtime->spawn(run_scoped_child<ValueType, LazyType>(
                         state,
                         std::move(signal),
                         std::move(result_promise),
                         LazyType(std::forward<Lazy>(lazy)))));

    return result_future;
}

template <typename Func>
Future<BlockingValue<Func>> TaskScope::spawn_blocking(Func &&func) {
    using ValueType = BlockingValue<Func>;
    using FuncType = std::decay_t<Func>;

    auto state = require_spawnable_state(state_);
    auto *runtime = state_runtime(state);

    auto result_promise = std::make_shared<async_simple::Promise<ValueType>>();
    auto result_future = result_promise->getFuture();

    auto completion =
        runtime->spawn_blocking(FuncType(std::forward<Func>(func)))
            .thenTry([state, result_promise](async_simple::Try<ValueType> result) mutable {
                if (state_source_const(state).cancellation_requested()) {
                    result_promise->setException(make_scope_cancel_error());
                    return;
                }

                if (result.hasError()) {
                    auto error = result.getException();
                    if (!is_scope_cancellation(state, error)) {
                        record_failure(state, error);
                    }
                    result_promise->setException(error);
                    return;
                }

                if constexpr (std::is_void_v<ValueType>) {
                    result.value();
                    result_promise->setValue();
                } else {
                    result_promise->setValue(std::move(result).value());
                }
            });

    track_completion(state, std::move(completion));
    return result_future;
}

template <typename... Lazies>
Task<std::tuple<TaskScope::ScopedValue<TaskValue<Lazies>>...>> TaskScope::all(Lazies &&...lazies) {
    static_assert(sizeof...(Lazies) > 0, "TaskScope::all requires at least one task");

    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_all_task(state, this, std::forward<Lazies>(lazies)...)
        .setLazyLocal(signal.get());
}

template <typename Range>
    requires(std::ranges::input_range<Range> &&
             requires { typename TaskValue<TaskScope::RangeTask<Range>>; })
Task<std::vector<TaskScope::ScopedValue<TaskValue<TaskScope::RangeTask<Range>>>>> TaskScope::all(
    Range &&range) {
    using Lazy = RangeTask<Range>;

    auto tasks = materialize_task_range<Lazy>(std::forward<Range>(range));
    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_all_range_task(this, std::move(tasks)).setLazyLocal(signal.get());
}

template <typename... Lazies>
Task<TaskScope::RaceResult<std::variant<TaskScope::ScopedValue<TaskValue<Lazies>>...>>>
TaskScope::race(Lazies &&...lazies) {
    static_assert(sizeof...(Lazies) > 0, "TaskScope::race requires at least one task");

    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_race_task<Lazies...>(std::forward<Lazies>(lazies)...)
        .setLazyLocal(signal.get());
}

template <typename Range>
    requires(std::ranges::input_range<Range> &&
             requires { typename TaskValue<TaskScope::RangeTask<Range>>; })
Task<TaskScope::RaceResult<TaskScope::ScopedValue<TaskValue<TaskScope::RangeTask<Range>>>>>
TaskScope::race(Range &&range) {
    using Lazy = RangeTask<Range>;

    auto tasks = materialize_task_range<Lazy>(std::forward<Range>(range));
    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_race_range_task(std::move(tasks)).setLazyLocal(signal.get());
}

template <typename Rep, typename Period, typename Lazy>
Task<TaskValue<Lazy>>
TaskScope::with_timeout(std::chrono::duration<Rep, Period> timeout, Lazy &&lazy) {
    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_timeout_task(timeout, std::forward<Lazy>(lazy)).setLazyLocal(signal.get());
}

template <typename First, typename... Rest>
Task<TaskValue<First>> TaskScope::any_success(First &&first, Rest &&...rest) {
    using ValueType = TaskValue<First>;

    static_assert((std::is_same_v<ValueType, TaskValue<Rest>> && ...),
                  "TaskScope::any_success requires all tasks to have the same value type");

    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_any_success_task<ValueType>(
        state_runtime(state), std::forward<First>(first), std::forward<Rest>(rest)...)
        .setLazyLocal(signal.get());
}

template <typename Range>
    requires(std::ranges::input_range<Range> &&
             requires { typename TaskValue<TaskScope::RangeTask<Range>>; })
Task<TaskValue<TaskScope::RangeTask<Range>>> TaskScope::any_success(Range &&range) {
    using Lazy = RangeTask<Range>;
    using ValueType = TaskValue<Lazy>;

    auto tasks = materialize_task_range<Lazy>(std::forward<Range>(range));
    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_any_success_range_task<ValueType>(state_runtime(state), std::move(tasks))
        .setLazyLocal(signal.get());
}

template <typename Clock, typename Duration, typename Lazy>
Task<TaskValue<Lazy>>
TaskScope::with_deadline(std::chrono::time_point<Clock, Duration> deadline, Lazy &&lazy) {
    auto state = require_spawnable_state(state_);
    auto signal = state_source(state).shared_signal();
    co_return co_await run_deadline_task(deadline, std::forward<Lazy>(lazy)).setLazyLocal(signal.get());
}

template <typename ValueType, typename Lazy>
Task<void> TaskScope::run_scoped_child(std::shared_ptr<State> state,
                                       std::shared_ptr<async_simple::Signal> signal,
                                       std::shared_ptr<async_simple::Promise<ValueType>> result_promise,
                                       Lazy lazy) {
    try {
        if constexpr (std::is_void_v<ValueType>) {
            co_await std::move(lazy).setLazyLocal(signal.get());
            result_promise->setValue();
        } else {
            auto value = co_await std::move(lazy).setLazyLocal(signal.get());
            result_promise->setValue(std::move(value));
        }
    } catch (...) {
        auto error = std::current_exception();
        if (!is_scope_cancellation(state, error)) {
            record_failure(state, error);
        }
        result_promise->setException(error);
    }
}

template <typename Lazy, typename Range>
std::vector<Lazy> TaskScope::materialize_task_range(Range &&range) {
    std::vector<Lazy> tasks;
    if constexpr (requires { std::ranges::size(range); }) {
        tasks.reserve(static_cast<std::size_t>(std::ranges::size(range)));
    }
    for (auto &&task : range) {
        tasks.push_back(std::move(task));
    }
    return tasks;
}

template <std::size_t Index, typename StorageTuple, typename FutureTuple>
Task<void> TaskScope::fill_all_results(StorageTuple &storage, FutureTuple &futures) {
    if constexpr (Index < std::tuple_size_v<StorageTuple>) {
        using OptionalType = std::tuple_element_t<Index, StorageTuple>;
        using ValueType = typename OptionalType::value_type;

        if constexpr (std::is_same_v<ValueType, std::monostate>) {
            co_await std::move(std::get<Index>(futures));
            std::get<Index>(storage).emplace();
        } else {
            std::get<Index>(storage).emplace(co_await std::move(std::get<Index>(futures)));
        }

        co_await fill_all_results<Index + 1>(storage, futures);
    }
}

template <typename ResultTuple, typename StorageTuple, std::size_t... Index>
ResultTuple TaskScope::materialize_all_results(StorageTuple &storage,
                                               std::index_sequence<Index...>) {
    return ResultTuple{std::move(*std::get<Index>(storage))...};
}

template <typename... Lazies>
Task<std::tuple<TaskScope::ScopedValue<TaskValue<Lazies>>...>>
TaskScope::run_all_task(std::shared_ptr<State>, TaskScope *scope, Lazies... lazies) {
    using ResultTuple = std::tuple<ScopedValue<TaskValue<Lazies>>...>;
    using StorageTuple = std::tuple<std::optional<ScopedValue<TaskValue<Lazies>>>...>;

    auto futures = std::make_tuple(scope->spawn(std::move(lazies))...);
    StorageTuple storage;

    co_await fill_all_results(storage, futures);
    co_return materialize_all_results<ResultTuple>(storage, std::index_sequence_for<Lazies...>{});
}

template <typename Lazy>
Task<std::vector<TaskScope::ScopedValue<TaskValue<Lazy>>>>
TaskScope::run_all_range_task(TaskScope *scope, std::vector<Lazy> lazies) {
    using ValueType = TaskValue<Lazy>;
    using ResultType = ScopedValue<ValueType>;

    std::vector<Future<ValueType>> futures;
    futures.reserve(lazies.size());
    for (auto &lazy : lazies) {
        futures.push_back(scope->spawn(std::move(lazy)));
    }

    std::vector<ResultType> results;
    results.reserve(futures.size());
    for (auto &future : futures) {
        if constexpr (std::is_void_v<ValueType>) {
            co_await std::move(future);
            results.emplace_back();
        } else {
            results.push_back(co_await std::move(future));
        }
    }

    co_return results;
}

template <typename... Lazies>
Task<TaskScope::RaceResult<std::variant<TaskScope::ScopedValue<TaskValue<Lazies>>...>>>
TaskScope::run_race_task(Lazies... lazies) {
    using RawVariant = std::variant<async_simple::Try<TaskValue<Lazies>>...>;
    using ResultVariant = std::variant<ScopedValue<TaskValue<Lazies>>...>;

    auto result =
        co_await async_simple::coro::collectAny<async_simple::Terminate>(std::move(lazies)...);
    co_return unwrap_race_result<RawVariant, ResultVariant>(std::move(result));
}

template <typename Lazy>
Task<TaskScope::RaceResult<TaskScope::ScopedValue<TaskValue<Lazy>>>>
TaskScope::run_race_range_task(std::vector<Lazy> lazies) {
    if (lazies.empty()) {
        throw std::runtime_error("TaskScope::race requires at least one task");
    }

    using ValueType = TaskValue<Lazy>;
    using ResultType = ScopedValue<ValueType>;

    auto result =
        co_await async_simple::coro::collectAny<async_simple::Terminate>(std::move(lazies));
    if (result.hasError()) {
        std::rethrow_exception(result.getException());
    }

    if constexpr (std::is_void_v<ValueType>) {
        result.value();
        co_return RaceResult<ResultType>{result.index(), std::monostate{}};
    } else {
        co_return RaceResult<ResultType>{result.index(), std::move(result).value()};
    }
}

template <typename Rep, typename Period, typename Lazy>
Task<TaskValue<Lazy>> TaskScope::run_timeout_task(std::chrono::duration<Rep, Period> timeout,
                                                  Lazy lazy) {
    co_return co_await async_uv::with_timeout(timeout, std::move(lazy));
}

template <typename ValueType, typename... Lazies>
Task<ValueType> TaskScope::run_any_success_task(Runtime *runtime, Lazies... lazies) {
    if (runtime == nullptr) {
        throw std::runtime_error("TaskScope::any_success requires a valid runtime");
    }

    struct Completion {
        async_simple::Try<ValueType> result;

        explicit Completion(async_simple::Try<ValueType> result_in)
            : result(std::move(result_in)) {}
        Completion(const Completion &) = delete;
        Completion &operator=(const Completion &) = delete;
        Completion(Completion &&) noexcept = default;
        Completion &operator=(Completion &&) noexcept = default;
    };

    CancellationSource source;
    auto mailbox = co_await Mailbox<Completion>::create(*runtime);
    auto sender = mailbox.sender();
    std::vector<Future<void>> completions;
    completions.reserve(sizeof...(Lazies));

    auto subscribe = [&]<typename Lazy>(Lazy &&lazy) {
        using LazyType = std::decay_t<Lazy>;
        completions.push_back(runtime->spawn(
            run_any_success_child<ValueType, Completion, LazyType>(
                source.shared_signal(), sender, LazyType(std::forward<Lazy>(lazy)))));
    };
    (subscribe(std::move(lazies)), ...);

    std::exception_ptr first_error;
    std::exception_ptr first_non_cancel_error;

    constexpr std::size_t kTotal = sizeof...(Lazies);
    std::exception_ptr body_error;
    std::optional<ScopedValue<ValueType>> success;

    try {
        for (std::size_t completed = 0; completed < kTotal; ++completed) {
            auto message = co_await mailbox.recv();
            if (!message) {
                break;
            }

            if (!message->result.hasError()) {
                if constexpr (std::is_void_v<ValueType>) {
                    message->result.value();
                    success.emplace();
                } else {
                    success.emplace(std::move(message->result).value());
                }
                (void)source.cancel();
                break;
            }

            auto error = message->result.getException();
            if (!first_error) {
                first_error = error;
            }
            if (!first_non_cancel_error && !is_cancel_exception(error)) {
                first_non_cancel_error = error;
            }
        }
    } catch (...) {
        body_error = std::current_exception();
    }

    (void)source.cancel();
    co_await mailbox.close();
    if (!completions.empty()) {
        (void)co_await async_simple::collectAll(completions.begin(), completions.end());
    }

    if (body_error) {
        std::rethrow_exception(body_error);
    }
    if (success.has_value()) {
        if constexpr (std::is_void_v<ValueType>) {
            co_return;
        } else {
            co_return std::move(*success);
        }
    }
    if (first_non_cancel_error) {
        std::rethrow_exception(first_non_cancel_error);
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }

    throw std::runtime_error("TaskScope::any_success completed without a result");
}

template <typename ValueType, typename Lazy>
Task<ValueType> TaskScope::run_any_success_range_task(Runtime *runtime, std::vector<Lazy> lazies) {
    if (lazies.empty()) {
        throw std::runtime_error("TaskScope::any_success requires at least one task");
    }

    if (runtime == nullptr) {
        throw std::runtime_error("TaskScope::any_success requires a valid runtime");
    }

    struct Completion {
        async_simple::Try<ValueType> result;

        explicit Completion(async_simple::Try<ValueType> result_in)
            : result(std::move(result_in)) {}
        Completion(const Completion &) = delete;
        Completion &operator=(const Completion &) = delete;
        Completion(Completion &&) noexcept = default;
        Completion &operator=(Completion &&) noexcept = default;
    };

    CancellationSource source;
    auto mailbox = co_await Mailbox<Completion>::create(*runtime);
    auto sender = mailbox.sender();
    std::vector<Future<void>> completions;
    completions.reserve(lazies.size());

    for (auto &lazy : lazies) {
        completions.push_back(runtime->spawn(
            run_any_success_child<ValueType, Completion, Lazy>(
                source.shared_signal(), sender, std::move(lazy))));
    }

    std::exception_ptr first_error;
    std::exception_ptr first_non_cancel_error;
    std::exception_ptr body_error;
    std::optional<ScopedValue<ValueType>> success;

    try {
        for (std::size_t completed = 0; completed < lazies.size(); ++completed) {
            auto message = co_await mailbox.recv();
            if (!message) {
                break;
            }

            if (!message->result.hasError()) {
                if constexpr (std::is_void_v<ValueType>) {
                    message->result.value();
                    success.emplace();
                } else {
                    success.emplace(std::move(message->result).value());
                }
                (void)source.cancel();
                break;
            }

            auto error = message->result.getException();
            if (!first_error) {
                first_error = error;
            }
            if (!first_non_cancel_error && !is_cancel_exception(error)) {
                first_non_cancel_error = error;
            }
        }
    } catch (...) {
        body_error = std::current_exception();
    }

    (void)source.cancel();
    co_await mailbox.close();
    if (!completions.empty()) {
        (void)co_await async_simple::collectAll(completions.begin(), completions.end());
    }

    if (body_error) {
        std::rethrow_exception(body_error);
    }
    if (success.has_value()) {
        if constexpr (std::is_void_v<ValueType>) {
            co_return;
        } else {
            co_return std::move(*success);
        }
    }
    if (first_non_cancel_error) {
        std::rethrow_exception(first_non_cancel_error);
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }

    throw std::runtime_error("TaskScope::any_success completed without a result");
}

template <typename ValueType, typename Completion, typename Lazy>
Task<void> TaskScope::run_any_success_child(std::shared_ptr<async_simple::Signal> signal,
                                            MessageSender<Completion> sender,
                                            Lazy lazy) {
    try {
        if constexpr (std::is_void_v<ValueType>) {
            co_await std::move(lazy).setLazyLocal(signal.get());
            (void)sender.send(Completion{async_simple::Try<void>()});
        } else {
            auto value = co_await std::move(lazy).setLazyLocal(signal.get());
            (void)sender.send(Completion{async_simple::Try<ValueType>(std::move(value))});
        }
    } catch (...) {
        (void)sender.send(Completion{async_simple::Try<ValueType>(std::current_exception())});
    }
}

template <typename Clock, typename Duration, typename Lazy>
Task<TaskValue<Lazy>> TaskScope::run_deadline_task(std::chrono::time_point<Clock, Duration> deadline,
                                                    Lazy lazy) {
    co_return co_await async_uv::with_deadline(deadline, std::move(lazy));
}

template <typename RawVariant, typename ResultVariant, std::size_t Index>
TaskScope::RaceResult<ResultVariant> TaskScope::unwrap_race_result(RawVariant &&result) {
    if constexpr (Index >= std::variant_size_v<std::remove_cvref_t<RawVariant>>) {
        throw std::logic_error("TaskScope::race received an empty result");
    } else {
        if (result.index() == Index) {
            auto &&entry = std::get<Index>(std::forward<RawVariant>(result));
            using RawTry = std::variant_alternative_t<Index, std::remove_cvref_t<RawVariant>>;
            using ValueType = typename TryValue<RawTry>::type;

            if (entry.hasError()) {
                std::rethrow_exception(entry.getException());
            }

            if constexpr (std::is_void_v<ValueType>) {
                entry.value();
                return {Index, ResultVariant(std::in_place_index<Index>, std::monostate{})};
            } else {
                return {Index,
                        ResultVariant(std::in_place_index<Index>, std::move(entry).value())};
            }
        }

        return unwrap_race_result<RawVariant, ResultVariant, Index + 1>(
            std::forward<RawVariant>(result));
    }
}

template <typename Func>
Task<TaskValue<ScopedTask<Func>>> with_task_scope(Func &&func) {
    using TaskType = ScopedTask<Func>;
    using ValueType = TaskValue<TaskType>;

    auto scope = co_await TaskScope::create();
    auto fn = std::decay_t<Func>(std::forward<Func>(func));
    std::exception_ptr body_error;

    if constexpr (std::is_void_v<ValueType>) {
        try {
            co_await std::invoke(fn, scope);
        } catch (...) {
            body_error = std::current_exception();
        }

        if (body_error) {
            try {
                co_await scope.close();
            } catch (...) {
            }
            std::rethrow_exception(body_error);
        }

        co_await scope.join();
        co_return;
    } else {
        std::optional<ValueType> value;

        try {
            value.emplace(co_await std::invoke(fn, scope));
        } catch (...) {
            body_error = std::current_exception();
        }

        if (body_error) {
            try {
                co_await scope.close();
            } catch (...) {
            }
            std::rethrow_exception(body_error);
        }

        co_await scope.join();
        co_return std::move(*value);
    }
}

} // namespace async_uv
