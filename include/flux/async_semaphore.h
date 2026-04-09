#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

namespace flux {

class AsyncSemaphore {
public:
    explicit AsyncSemaphore(std::ptrdiff_t permits = 0)
        : permits_(permits > 0 ? permits : 0) {}

    AsyncSemaphore(const AsyncSemaphore &) = delete;
    AsyncSemaphore &operator=(const AsyncSemaphore &) = delete;

    class AcquireSender;

    [[nodiscard]] AcquireSender acquire_sender() noexcept {
        return AcquireSender(this);
    }

    // Sender-first API. Kept as `acquire()` for ergonomic `co_await`.
    [[nodiscard]] AcquireSender acquire() noexcept {
        return acquire_sender();
    }

    void release(std::ptrdiff_t permits = 1) {
        if (permits <= 0) {
            return;
        }

        std::vector<WaiterNode *> ready;
        ready.reserve(static_cast<std::size_t>(permits));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }

            while (permits-- > 0) {
                if (head_ != nullptr) {
                    auto *waiter = pop_front_waiter_locked();
                    if (waiter != nullptr) {
                        ready.push_back(waiter);
                    }
                } else {
                    ++permits_;
                }
            }
        }

        for (auto *waiter : ready) {
            waiter->complete(waiter->owner, true);
        }
    }

    void close() noexcept {
        std::vector<WaiterNode *> waiting;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            permits_ = 0;
            while (head_ != nullptr) {
                auto *waiter = pop_front_waiter_locked();
                if (waiter != nullptr) {
                    waiting.push_back(waiter);
                }
            }
        }

        for (auto *waiter : waiting) {
            waiter->complete(waiter->owner, false);
        }
    }

    [[nodiscard]] bool is_closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    struct WaiterNode {
        WaiterNode *prev = nullptr;
        WaiterNode *next = nullptr;
        void *owner = nullptr;
        void (*complete)(void *owner, bool acquired) noexcept = nullptr;
        bool queued = false;
    };

    void enqueue_waiter_locked(WaiterNode *waiter) noexcept {
        waiter->queued = true;
        waiter->prev = tail_;
        waiter->next = nullptr;
        if (tail_ != nullptr) {
            tail_->next = waiter;
        } else {
            head_ = waiter;
        }
        tail_ = waiter;
    }

    void remove_waiter_locked(WaiterNode *waiter) noexcept {
        if (!waiter->queued) {
            return;
        }
        if (waiter->prev != nullptr) {
            waiter->prev->next = waiter->next;
        } else {
            head_ = waiter->next;
        }
        if (waiter->next != nullptr) {
            waiter->next->prev = waiter->prev;
        } else {
            tail_ = waiter->prev;
        }
        waiter->prev = nullptr;
        waiter->next = nullptr;
        waiter->queued = false;
    }

    [[nodiscard]] WaiterNode *pop_front_waiter_locked() noexcept {
        auto *waiter = head_;
        if (waiter == nullptr) {
            return nullptr;
        }
        head_ = waiter->next;
        if (head_ != nullptr) {
            head_->prev = nullptr;
        } else {
            tail_ = nullptr;
        }
        waiter->prev = nullptr;
        waiter->next = nullptr;
        waiter->queued = false;
        return waiter;
    }

public:
    class AcquireSender {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures =
            stdexec::completion_signatures<stdexec::set_value_t(bool), stdexec::set_stopped_t()>;

        explicit AcquireSender(AsyncSemaphore *semaphore = nullptr) noexcept : semaphore_(semaphore) {}

        template <class Env>
        friend auto tag_invoke(stdexec::get_completion_signatures_t,
                               const AcquireSender &,
                               Env &&) noexcept -> completion_signatures {
            return {};
        }

        template <stdexec::receiver Receiver>
        struct Operation {
            using operation_state_concept = stdexec::operation_state_t;

            AsyncSemaphore *semaphore = nullptr;
            Receiver receiver;
            WaiterNode waiter{};
            std::atomic<bool> completed{false};
            using StopToken =
                decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
            struct StopRequest {
                Operation *self = nullptr;
                void operator()() const noexcept {
                    if (self != nullptr) {
                        self->request_stop();
                    }
                }
            };
            using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
            std::optional<StopCallback> on_stop;

            Operation(AsyncSemaphore *sem, Receiver rcv)
                : semaphore(sem),
                  receiver(std::move(rcv)) {
                waiter.owner = this;
                waiter.complete = [](void *owner, bool acquired) noexcept {
                    static_cast<Operation *>(owner)->finish(acquired);
                };
            }

            ~Operation() {
                detach_from_queue();
                on_stop.reset();
            }

            void detach_from_queue() noexcept {
                if (semaphore == nullptr) {
                    return;
                }
                std::lock_guard<std::mutex> lock(semaphore->mutex_);
                semaphore->remove_waiter_locked(&waiter);
            }

            bool mark_completed() noexcept {
                bool expected = false;
                if (!completed.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return false;
                }
                on_stop.reset();
                return true;
            }

            void finish(bool acquired) noexcept {
                if (!mark_completed()) {
                    return;
                }
                if (acquired) {
                    stdexec::set_value(std::move(receiver), true);
                } else {
                    stdexec::set_value(std::move(receiver), false);
                }
            }

            void finish_stopped() noexcept {
                if (!mark_completed()) {
                    return;
                }
                stdexec::set_stopped(std::move(receiver));
            }

            void request_stop() noexcept {
                if (completed.load(std::memory_order_acquire)) {
                    return;
                }
                detach_from_queue();
                finish_stopped();
            }

            void start() noexcept {
                if (semaphore == nullptr) {
                    finish(false);
                    return;
                }

                StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
                if (token.stop_requested()) {
                    finish_stopped();
                    return;
                }

                bool immediate = false;
                bool acquired = false;
                bool queued = false;

                {
                    std::lock_guard<std::mutex> lock(semaphore->mutex_);
                    if (semaphore->closed_) {
                        immediate = true;
                        acquired = false;
                    } else if (semaphore->permits_ > 0) {
                        --semaphore->permits_;
                        immediate = true;
                        acquired = true;
                    } else {
                        semaphore->enqueue_waiter_locked(&waiter);
                        queued = true;
                    }
                }

                if (immediate) {
                    finish(acquired);
                    return;
                }

                if (queued) {
                    on_stop.emplace(token, StopRequest{this});
                    if (completed.load(std::memory_order_acquire)) {
                        on_stop.reset();
                    }
                }
            }

            friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
                self.start();
            }
        };

        template <stdexec::receiver Receiver>
        friend auto tag_invoke(stdexec::connect_t, const AcquireSender &self, Receiver receiver)
            -> Operation<std::remove_cvref_t<Receiver>> {
            return {self.semaphore_, std::move(receiver)};
        }

        template <stdexec::receiver Receiver>
        friend auto tag_invoke(stdexec::connect_t, AcquireSender &&self, Receiver receiver)
            -> Operation<std::remove_cvref_t<Receiver>> {
            return {self.semaphore_, std::move(receiver)};
        }

    private:
        AsyncSemaphore *semaphore_ = nullptr;
    };

private:
    friend class AcquireSender;

    mutable std::mutex mutex_;
    std::ptrdiff_t permits_ = 0;
    bool closed_ = false;
    WaiterNode *head_ = nullptr;
    WaiterNode *tail_ = nullptr;
};

} // namespace flux
