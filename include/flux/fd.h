#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/error.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <stdexec/execution.hpp>

#include "flux/runtime.h"

namespace flux {

namespace detail {

template <class Receiver>
void set_stopped_or_error(Receiver &receiver, const std::error_code &ec) noexcept {
    if (ec == exec::asio::asio_impl::error::operation_aborted ||
        ec == exec::asio::asio_impl::error::bad_descriptor) {
        stdexec::set_stopped(std::move(receiver));
        return;
    }
    stdexec::set_error(
        std::move(receiver),
        std::make_exception_ptr(std::system_error(ec, "FdStream asynchronous operation failed")));
}

template <class Receiver>
void set_not_valid(Receiver &receiver) noexcept {
    stdexec::set_error(
        std::move(receiver),
        std::make_exception_ptr(std::runtime_error("FdStream is not valid")));
}

} // namespace detail

class FdStream {
public:
    using ConstBuffer = std::span<const std::byte>;
    using MutableBuffer = std::span<std::byte>;

    struct State {
        Runtime *runtime = nullptr;
        exec::asio::asio_impl::posix::stream_descriptor descriptor;

        explicit State(Runtime *rt) : runtime(rt), descriptor(rt->executor()) {}
    };

    class WaitReadableSender;
    class WaitWritableSender;
    class WriteSomeSender;
    class WriteAllSender;
    class ReadSomeSender;
    class ReadExactlySender;

    FdStream() = default;
    explicit FdStream(std::shared_ptr<State> state) noexcept;
    ~FdStream();

    FdStream(const FdStream &) = delete;
    FdStream &operator=(const FdStream &) = delete;
    FdStream(FdStream &&other) noexcept;
    FdStream &operator=(FdStream &&other) noexcept;

    // Duplicate an existing file descriptor and manage the duplicated handle.
    static Task<FdStream> attach(int fd);

    // Adopt and manage ownership of an existing file descriptor.
    static Task<FdStream> adopt(int fd);

    bool valid() const noexcept;
    int native_handle() const noexcept;

    // Sender factories (P2300 style):
    // each sender implements connect(receiver) -> operation_state and start().
    // NOTE: ConstBuffer/MutableBuffer are non-owning views. Caller must keep
    // the referenced memory alive until the sender completes.
    [[nodiscard]] WaitReadableSender wait_readable_sender() const;
    [[nodiscard]] WaitWritableSender wait_writable_sender() const;
    [[nodiscard]] WriteSomeSender write_some_sender(ConstBuffer data) const;
    [[nodiscard]] WriteAllSender write_all_sender(ConstBuffer data) const;
    [[nodiscard]] ReadSomeSender read_some_sender(MutableBuffer output) const;
    [[nodiscard]] ReadExactlySender read_exactly_sender(MutableBuffer output) const;

    Task<void> wait_readable() const;
    Task<void> wait_writable() const;

    // Coroutine helpers built on top of sender factories.
    Task<std::size_t> write_some(ConstBuffer data) const;
    Task<std::size_t> write_all(ConstBuffer data) const;
    Task<std::size_t> read_some(MutableBuffer output) const;
    Task<std::size_t> read_exactly(MutableBuffer output) const;

    // String-view convenience wrappers (no copy, caller owns buffer lifetime).
    Task<std::size_t> write_some(std::string_view data) const;
    Task<std::size_t> write_all(std::string_view data) const;

    Task<void> close();

private:
    std::shared_ptr<State> state_;
};

class FdStream::WaitReadableSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    WaitReadableSender() = default;
    explicit WaitReadableSender(std::shared_ptr<State> state) : state_(std::move(state)) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const WaitReadableSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        std::shared_ptr<State> state;
        Receiver receiver;
        std::atomic<bool> completed{false};
        using StopToken =
            decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
        struct StopRequest {
            Operation *self = nullptr;
            void operator()() const noexcept {
                if (self != nullptr) {
                    self->request_cancel();
                }
            }
        };
        using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
        std::optional<StopCallback> on_stop;

        bool begin_operation() noexcept {
            if (!state || !state->descriptor.is_open()) {
                completed.store(true, std::memory_order_release);
                detail::set_not_valid(receiver);
                return false;
            }

            StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if (token.stop_requested()) {
                completed.store(true, std::memory_order_release);
                stdexec::set_stopped(std::move(receiver));
                return false;
            }

            on_stop.emplace(token, StopRequest{this});
            return true;
        }

        bool finish_operation() noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }
            on_stop.reset();
            return true;
        }

        void request_cancel() noexcept {
            if (completed.load(std::memory_order_acquire)) {
                return;
            }
            if (!state || !state->descriptor.is_open()) {
                return;
            }
            std::error_code ignored;
            state->descriptor.cancel(ignored);
        }

        void start() noexcept {
            if (!begin_operation()) {
                return;
            }

            state->descriptor.async_wait(
                exec::asio::asio_impl::posix::stream_descriptor::wait_read,
                [this](const std::error_code &ec) noexcept {
                    if (!finish_operation()) {
                        return;
                    }
                    if (ec) {
                        detail::set_stopped_or_error(receiver, ec);
                        return;
                    }
                    stdexec::set_value(std::move(receiver));
                });
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const WaitReadableSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.state_, std::move(receiver), {}, std::nullopt};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, WaitReadableSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.state_), std::move(receiver), {}, std::nullopt};
    }

private:
    std::shared_ptr<State> state_;
};

class FdStream::WaitWritableSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    WaitWritableSender() = default;
    explicit WaitWritableSender(std::shared_ptr<State> state) : state_(std::move(state)) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const WaitWritableSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        std::shared_ptr<State> state;
        Receiver receiver;
        std::atomic<bool> completed{false};
        using StopToken =
            decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
        struct StopRequest {
            Operation *self = nullptr;
            void operator()() const noexcept {
                if (self != nullptr) {
                    self->request_cancel();
                }
            }
        };
        using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
        std::optional<StopCallback> on_stop;

        bool begin_operation() noexcept {
            if (!state || !state->descriptor.is_open()) {
                completed.store(true, std::memory_order_release);
                detail::set_not_valid(receiver);
                return false;
            }

            StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if (token.stop_requested()) {
                completed.store(true, std::memory_order_release);
                stdexec::set_stopped(std::move(receiver));
                return false;
            }

            on_stop.emplace(token, StopRequest{this});
            return true;
        }

        bool finish_operation() noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }
            on_stop.reset();
            return true;
        }

        void request_cancel() noexcept {
            if (completed.load(std::memory_order_acquire)) {
                return;
            }
            if (!state || !state->descriptor.is_open()) {
                return;
            }
            std::error_code ignored;
            state->descriptor.cancel(ignored);
        }

        void start() noexcept {
            if (!begin_operation()) {
                return;
            }

            state->descriptor.async_wait(
                exec::asio::asio_impl::posix::stream_descriptor::wait_write,
                [this](const std::error_code &ec) noexcept {
                    if (!finish_operation()) {
                        return;
                    }
                    if (ec) {
                        detail::set_stopped_or_error(receiver, ec);
                        return;
                    }
                    stdexec::set_value(std::move(receiver));
                });
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const WaitWritableSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.state_, std::move(receiver), {}, std::nullopt};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, WaitWritableSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.state_), std::move(receiver), {}, std::nullopt};
    }

private:
    std::shared_ptr<State> state_;
};

class FdStream::WriteSomeSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    WriteSomeSender() = default;
    WriteSomeSender(std::shared_ptr<State> state, ConstBuffer data)
        : state_(std::move(state)), data_(data.data()), size_(data.size()) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const WriteSomeSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        std::shared_ptr<State> state;
        const std::byte *data = nullptr;
        std::size_t size = 0;
        Receiver receiver;
        std::atomic<bool> completed{false};
        using StopToken =
            decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
        struct StopRequest {
            Operation *self = nullptr;
            void operator()() const noexcept {
                if (self != nullptr) {
                    self->request_cancel();
                }
            }
        };
        using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
        std::optional<StopCallback> on_stop;

        bool begin_operation() noexcept {
            if (!state || !state->descriptor.is_open()) {
                completed.store(true, std::memory_order_release);
                detail::set_not_valid(receiver);
                return false;
            }

            StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if (token.stop_requested()) {
                completed.store(true, std::memory_order_release);
                stdexec::set_stopped(std::move(receiver));
                return false;
            }

            on_stop.emplace(token, StopRequest{this});
            return true;
        }

        bool finish_operation() noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }
            on_stop.reset();
            return true;
        }

        void request_cancel() noexcept {
            if (completed.load(std::memory_order_acquire)) {
                return;
            }
            if (!state || !state->descriptor.is_open()) {
                return;
            }
            std::error_code ignored;
            state->descriptor.cancel(ignored);
        }

        void start() noexcept {
            if (!begin_operation()) {
                return;
            }

            state->descriptor.async_write_some(
                exec::asio::asio_impl::buffer(static_cast<const void *>(data), size),
                [this](const std::error_code &ec, std::size_t written) noexcept {
                    if (!finish_operation()) {
                        return;
                    }
                    if (ec) {
                        detail::set_stopped_or_error(receiver, ec);
                        return;
                    }
                    stdexec::set_value(std::move(receiver), written);
                });
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const WriteSomeSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.state_,
                self.data_,
                self.size_,
                std::move(receiver),
                {},
                std::nullopt};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, WriteSomeSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.state_),
                self.data_,
                self.size_,
                std::move(receiver),
                {},
                std::nullopt};
    }

private:
    std::shared_ptr<State> state_;
    const std::byte *data_ = nullptr;
    std::size_t size_ = 0;
};

class FdStream::WriteAllSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    WriteAllSender() = default;
    WriteAllSender(std::shared_ptr<State> state, ConstBuffer data)
        : state_(std::move(state)), data_(data.data()), size_(data.size()) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const WriteAllSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        std::shared_ptr<State> state;
        const std::byte *data = nullptr;
        std::size_t size = 0;
        Receiver receiver;
        std::atomic<bool> completed{false};
        using StopToken =
            decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
        struct StopRequest {
            Operation *self = nullptr;
            void operator()() const noexcept {
                if (self != nullptr) {
                    self->request_cancel();
                }
            }
        };
        using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
        std::optional<StopCallback> on_stop;

        bool begin_operation() noexcept {
            if (!state || !state->descriptor.is_open()) {
                completed.store(true, std::memory_order_release);
                detail::set_not_valid(receiver);
                return false;
            }

            StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if (token.stop_requested()) {
                completed.store(true, std::memory_order_release);
                stdexec::set_stopped(std::move(receiver));
                return false;
            }

            on_stop.emplace(token, StopRequest{this});
            return true;
        }

        bool finish_operation() noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }
            on_stop.reset();
            return true;
        }

        void request_cancel() noexcept {
            if (completed.load(std::memory_order_acquire)) {
                return;
            }
            if (!state || !state->descriptor.is_open()) {
                return;
            }
            std::error_code ignored;
            state->descriptor.cancel(ignored);
        }

        void start() noexcept {
            if (!begin_operation()) {
                return;
            }

            exec::asio::asio_impl::async_write(
                state->descriptor,
                exec::asio::asio_impl::buffer(static_cast<const void *>(data), size),
                [this](const std::error_code &ec, std::size_t written) noexcept {
                    if (!finish_operation()) {
                        return;
                    }
                    if (ec) {
                        detail::set_stopped_or_error(receiver, ec);
                        return;
                    }
                    stdexec::set_value(std::move(receiver), written);
                });
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const WriteAllSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.state_,
                self.data_,
                self.size_,
                std::move(receiver),
                {},
                std::nullopt};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, WriteAllSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.state_),
                self.data_,
                self.size_,
                std::move(receiver),
                {},
                std::nullopt};
    }

private:
    std::shared_ptr<State> state_;
    const std::byte *data_ = nullptr;
    std::size_t size_ = 0;
};

class FdStream::ReadSomeSender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    ReadSomeSender() = default;
    ReadSomeSender(std::shared_ptr<State> state, MutableBuffer output)
        : state_(std::move(state)), output_(output) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const ReadSomeSender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        std::shared_ptr<State> state;
        std::byte *output = nullptr;
        std::size_t capacity = 0;
        Receiver receiver;
        std::atomic<bool> completed{false};
        using StopToken =
            decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
        struct StopRequest {
            Operation *self = nullptr;
            void operator()() const noexcept {
                if (self != nullptr) {
                    self->request_cancel();
                }
            }
        };
        using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
        std::optional<StopCallback> on_stop;

        bool begin_operation() noexcept {
            if (!state || !state->descriptor.is_open()) {
                completed.store(true, std::memory_order_release);
                detail::set_not_valid(receiver);
                return false;
            }

            StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if (token.stop_requested()) {
                completed.store(true, std::memory_order_release);
                stdexec::set_stopped(std::move(receiver));
                return false;
            }

            on_stop.emplace(token, StopRequest{this});
            return true;
        }

        bool finish_operation() noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }
            on_stop.reset();
            return true;
        }

        void request_cancel() noexcept {
            if (completed.load(std::memory_order_acquire)) {
                return;
            }
            if (!state || !state->descriptor.is_open()) {
                return;
            }
            std::error_code ignored;
            state->descriptor.cancel(ignored);
        }

        void start() noexcept {
            if (!begin_operation()) {
                return;
            }

            if (capacity == 0 || output == nullptr) {
                if (!finish_operation()) {
                    return;
                }
                stdexec::set_value(std::move(receiver), static_cast<std::size_t>(0));
                return;
            }

            state->descriptor.async_read_some(
                exec::asio::asio_impl::buffer(static_cast<void *>(output), capacity),
                [this](const std::error_code &ec, std::size_t n) noexcept {
                    if (!finish_operation()) {
                        return;
                    }

                    if (ec) {
                        if (ec == exec::asio::asio_impl::error::eof) {
                            stdexec::set_value(
                                std::move(receiver), static_cast<std::size_t>(0));
                            return;
                        }
                        detail::set_stopped_or_error(receiver, ec);
                        return;
                    }

                    stdexec::set_value(std::move(receiver), n);
                });
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const ReadSomeSender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.state_,
                self.output_.data(),
                self.output_.size(),
                std::move(receiver),
                {},
                std::nullopt};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, ReadSomeSender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.state_),
                self.output_.data(),
                self.output_.size(),
                std::move(receiver),
                {},
                std::nullopt};
    }

private:
    std::shared_ptr<State> state_;
    MutableBuffer output_{};
};

class FdStream::ReadExactlySender {
public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::size_t),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    ReadExactlySender() = default;
    ReadExactlySender(std::shared_ptr<State> state, MutableBuffer output)
        : state_(std::move(state)), output_(output) {}

    template <class Env>
    friend auto tag_invoke(stdexec::get_completion_signatures_t,
                           const ReadExactlySender &,
                           Env &&) noexcept -> completion_signatures {
        return {};
    }

    template <stdexec::receiver Receiver>
    struct Operation {
        using operation_state_concept = stdexec::operation_state_t;

        std::shared_ptr<State> state;
        std::byte *output = nullptr;
        std::size_t size = 0;
        Receiver receiver;
        std::atomic<bool> completed{false};
        using StopToken =
            decltype(stdexec::get_stop_token(stdexec::get_env(std::declval<Receiver &>())));
        struct StopRequest {
            Operation *self = nullptr;
            void operator()() const noexcept {
                if (self != nullptr) {
                    self->request_cancel();
                }
            }
        };
        using StopCallback = stdexec::stop_callback_for_t<StopToken, StopRequest>;
        std::optional<StopCallback> on_stop;

        bool begin_operation() noexcept {
            if (!state || !state->descriptor.is_open()) {
                completed.store(true, std::memory_order_release);
                detail::set_not_valid(receiver);
                return false;
            }

            StopToken token = stdexec::get_stop_token(stdexec::get_env(receiver));
            if (token.stop_requested()) {
                completed.store(true, std::memory_order_release);
                stdexec::set_stopped(std::move(receiver));
                return false;
            }

            on_stop.emplace(token, StopRequest{this});
            return true;
        }

        bool finish_operation() noexcept {
            bool expected = false;
            if (!completed.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }
            on_stop.reset();
            return true;
        }

        void request_cancel() noexcept {
            if (completed.load(std::memory_order_acquire)) {
                return;
            }
            if (!state || !state->descriptor.is_open()) {
                return;
            }
            std::error_code ignored;
            state->descriptor.cancel(ignored);
        }

        void start() noexcept {
            if (!begin_operation()) {
                return;
            }

            if (size == 0 || output == nullptr) {
                if (!finish_operation()) {
                    return;
                }
                stdexec::set_value(std::move(receiver), static_cast<std::size_t>(0));
                return;
            }

            exec::asio::asio_impl::async_read(
                state->descriptor,
                exec::asio::asio_impl::buffer(static_cast<void *>(output), size),
                [this](const std::error_code &ec, std::size_t n) noexcept {
                    if (!finish_operation()) {
                        return;
                    }
                    if (ec) {
                        detail::set_stopped_or_error(receiver, ec);
                        return;
                    }
                    stdexec::set_value(std::move(receiver), n);
                });
        }

        friend void tag_invoke(stdexec::start_t, Operation &self) noexcept {
            self.start();
        }
    };

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, const ReadExactlySender &self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {self.state_,
                self.output_.data(),
                self.output_.size(),
                std::move(receiver),
                {},
                std::nullopt};
    }

    template <stdexec::receiver Receiver>
    friend auto tag_invoke(stdexec::connect_t, ReadExactlySender &&self, Receiver receiver)
        -> Operation<std::remove_cvref_t<Receiver>> {
        return {std::move(self.state_),
                self.output_.data(),
                self.output_.size(),
                std::move(receiver),
                {},
                std::nullopt};
    }

private:
    std::shared_ptr<State> state_;
    MutableBuffer output_{};
};

using uv_os_sock_t = int;

enum class FdEventFlags : int {
    none = 0,
    readable = 1 << 0,
    writable = 1 << 1,
};

constexpr FdEventFlags operator|(FdEventFlags lhs, FdEventFlags rhs) noexcept {
    return static_cast<FdEventFlags>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

constexpr FdEventFlags operator&(FdEventFlags lhs, FdEventFlags rhs) noexcept {
    return static_cast<FdEventFlags>(static_cast<int>(lhs) & static_cast<int>(rhs));
}

constexpr FdEventFlags &operator|=(FdEventFlags &lhs, FdEventFlags rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

class FdEvent {
public:
    FdEvent() = default;

    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }

    [[nodiscard]] bool readable() const noexcept {
        return readable_;
    }

    [[nodiscard]] bool writable() const noexcept {
        return writable_;
    }

    [[nodiscard]] const std::error_code &error() const noexcept {
        return error_;
    }

private:
    friend class FdWatcher;
    bool ok_ = false;
    bool readable_ = false;
    bool writable_ = false;
    std::error_code error_{};
};

class FdWatcher {
public:
    using WatchCompletionSignatures = stdexec::completion_signatures<
        stdexec::set_value_t(FdWatcher),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;
    using EventCompletionSignatures = stdexec::completion_signatures<
        stdexec::set_value_t(std::optional<FdEvent>),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;
    using StopCompletionSignatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;
    using WatchSender = exec::any_receiver_ref<WatchCompletionSignatures>::any_sender<>;
    using EventSender = exec::any_receiver_ref<EventCompletionSignatures>::any_sender<>;
    using StopSender = exec::any_receiver_ref<StopCompletionSignatures>::any_sender<>;

    FdWatcher() = default;
    ~FdWatcher() = default;

    FdWatcher(const FdWatcher &) = default;
    FdWatcher &operator=(const FdWatcher &) = default;
    FdWatcher(FdWatcher &&other) noexcept : state_(std::move(other.state_)) {}
    FdWatcher &operator=(FdWatcher &&other) noexcept = default;

    // Sender-first API.
    static WatchSender watch_sender(uv_os_sock_t fd, int events);
    static WatchSender watch_sender(uv_os_sock_t fd, int events, Runtime *runtime);
    [[nodiscard]] EventSender next_sender(std::optional<int> timeout_ms = std::nullopt) const;
    template <typename Rep, typename Period>
    [[nodiscard]] EventSender next_for_sender(std::chrono::duration<Rep, Period> timeout) const {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        const auto clamped = std::max<std::int64_t>(0, ms.count());
        return next_sender(static_cast<int>(clamped));
    }
    [[nodiscard]] StopSender stop_sender() const;

    static Task<FdWatcher> watch(uv_os_sock_t fd, int events);

    Task<std::optional<FdEvent>> next();

    template <typename Rep, typename Period>
    Task<std::optional<FdEvent>> next_for(std::chrono::duration<Rep, Period> timeout) {
        co_return co_await next_for_sender(timeout);
    }

    Task<void> stop();

private:
    struct State {
        Runtime *runtime = nullptr;
        int events = 0;
        exec::asio::asio_impl::posix::stream_descriptor descriptor;
        std::atomic<bool> stopped{false};

        explicit State(Runtime *rt, int requested_events)
            : runtime(rt),
              events(requested_events),
              descriptor(rt->executor()) {}

        ~State() {
            // FdWatcher does not own the original fd lifecycle.
            // Ensure descriptor destruction never closes user-owned fd.
            if (descriptor.is_open()) {
                (void)descriptor.release();
            }
        }
    };

    explicit FdWatcher(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

} // namespace flux
