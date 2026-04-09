#include "flux/fd.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

#include <asio/steady_timer.hpp>
#include <exec/when_any.hpp>
#include <unistd.h>

#include "flux/cancel.h"

namespace flux {

namespace {

std::optional<FdEvent> to_optional_event(FdEvent ready) {
    return std::optional<FdEvent>(std::move(ready));
}

template <typename Sender>
FdWatcher::EventSender wait_event_with_optional_timeout(Sender &&wait_sender,
                                                        std::optional<int> timeout_ms,
                                                        Runtime *runtime) {
    auto wait_optional = std::forward<Sender>(wait_sender) | stdexec::then(to_optional_event);
    if (!timeout_ms.has_value()) {
        return wait_optional;
    }

    if (runtime == nullptr) {
        throw std::runtime_error("FdWatcher::next_sender requires current runtime");
    }

    const int bounded = std::max(0, *timeout_ms);
    auto timer = std::make_shared<exec::asio::asio_impl::steady_timer>(runtime->executor());
    timer->expires_after(std::chrono::milliseconds(bounded));
    auto timeout_optional = timer->async_wait(exec::asio::use_sender)
                          | stdexec::then([timer]() { return std::optional<FdEvent>{}; });
    return exec::when_any(std::move(wait_optional), std::move(timeout_optional));
}

} // namespace

FdStream::FdStream(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

FdStream::~FdStream() = default;

FdStream::FdStream(FdStream &&other) noexcept : state_(std::move(other.state_)) {}

FdStream &FdStream::operator=(FdStream &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    state_ = std::move(other.state_);
    return *this;
}

Task<FdStream> FdStream::attach(int fd) {
    if (fd < 0) {
        throw std::runtime_error("FdStream::attach requires a valid file descriptor");
    }

    const int duplicated = ::dup(fd);
    if (duplicated < 0) {
        throw std::system_error(errno, std::generic_category(), "dup");
    }

    co_return co_await adopt(duplicated);
}

Task<FdStream> FdStream::adopt(int fd) {
    if (fd < 0) {
        throw std::runtime_error("FdStream::adopt requires a valid file descriptor");
    }

    auto *runtime = co_await get_current_runtime();
    auto state = std::make_shared<State>(runtime);

    try {
        state->descriptor.assign(fd);
    } catch (...) {
        (void)::close(fd);
        throw;
    }

    co_return FdStream(std::move(state));
}

bool FdStream::valid() const noexcept {
    return state_ != nullptr && state_->descriptor.is_open();
}

int FdStream::native_handle() const noexcept {
    if (!valid()) {
        return -1;
    }
    return state_->descriptor.native_handle();
}

FdStream::WriteSomeSender FdStream::write_some_sender(ConstBuffer data) const {
    return WriteSomeSender(state_, std::move(data));
}

FdStream::WriteAllSender FdStream::write_all_sender(ConstBuffer data) const {
    return WriteAllSender(state_, std::move(data));
}

FdStream::ReadSomeSender FdStream::read_some_sender(MutableBuffer output) const {
    return ReadSomeSender(state_, output);
}

FdStream::ReadExactlySender FdStream::read_exactly_sender(MutableBuffer output) const {
    return ReadExactlySender(state_, output);
}

FdStream::WaitReadableSender FdStream::wait_readable_sender() const {
    return WaitReadableSender(state_);
}

FdStream::WaitWritableSender FdStream::wait_writable_sender() const {
    return WaitWritableSender(state_);
}

Task<void> FdStream::close() {
    if (!state_) {
        co_return;
    }

    std::error_code ec;
    state_->descriptor.cancel(ec);
    state_->descriptor.close(ec);
}

Task<std::size_t> FdStream::write_some(ConstBuffer data) const {
    if (data.empty()) {
        co_return 0;
    }
    co_return co_await write_some_sender(data);
}

Task<void> FdStream::wait_readable() const {
    co_await wait_readable_sender();
}

Task<void> FdStream::wait_writable() const {
    co_await wait_writable_sender();
}

Task<std::size_t> FdStream::write_all(ConstBuffer data) const {
    if (data.empty()) {
        co_return 0;
    }
    co_return co_await write_all_sender(data);
}

Task<std::size_t> FdStream::read_some(MutableBuffer output) const {
    co_return co_await read_some_sender(output);
}

Task<std::size_t> FdStream::read_exactly(MutableBuffer output) const {
    co_return co_await read_exactly_sender(output);
}

Task<std::size_t> FdStream::write_some(std::string_view data) const {
    auto bytes = ConstBuffer{
        reinterpret_cast<const std::byte *>(data.data()),
        data.size()};
    co_return co_await write_some(bytes);
}

Task<std::size_t> FdStream::write_all(std::string_view data) const {
    auto bytes = ConstBuffer{
        reinterpret_cast<const std::byte *>(data.data()),
        data.size()};
    co_return co_await write_all(bytes);
}

FdWatcher::WatchSender FdWatcher::watch_sender(uv_os_sock_t fd, int events) {
    return watch_sender(fd, events, Runtime::current());
}

FdWatcher::WatchSender FdWatcher::watch_sender(uv_os_sock_t fd, int events, Runtime *runtime) {
    return stdexec::just(fd, events)
         | stdexec::then([runtime](uv_os_sock_t source_fd, int source_events) {
               if (source_fd < 0) {
                   throw std::runtime_error("FdWatcher::watch_sender requires a valid fd");
               }
               const int normalized_events =
                   source_events &
                   (static_cast<int>(FdEventFlags::readable) |
                    static_cast<int>(FdEventFlags::writable));
               if (normalized_events == 0) {
                   throw std::runtime_error(
                       "FdWatcher::watch_sender requires readable and/or writable events");
               }

               Runtime *active_runtime = runtime != nullptr ? runtime : Runtime::current();
               if (active_runtime == nullptr) {
                   throw std::runtime_error("FdWatcher::watch_sender requires current runtime");
               }

               auto state = std::make_shared<State>(active_runtime, normalized_events);
               state->descriptor.assign(source_fd);
               return FdWatcher(std::move(state));
           });
}

FdWatcher::EventSender FdWatcher::next_sender(std::optional<int> timeout_ms) const {
    auto state = state_;
    if (!state || state->stopped.load(std::memory_order_acquire)) {
        return stdexec::just(std::optional<FdEvent>{});
    }

    const bool want_read = (state->events & static_cast<int>(FdEventFlags::readable)) != 0;
    const bool want_write = (state->events & static_cast<int>(FdEventFlags::writable)) != 0;
    auto make_readable_event = [] {
        FdEvent event;
        event.ok_ = true;
        event.readable_ = true;
        return event;
    };
    auto make_writable_event = [] {
        FdEvent event;
        event.ok_ = true;
        event.writable_ = true;
        return event;
    };

    auto finalize = [state](EventSender sender) -> EventSender {
        return std::move(sender)
             | stdexec::upon_error([state](std::exception_ptr error) {
                   if (state && state->stopped.load(std::memory_order_acquire)) {
                       return std::optional<FdEvent>{};
                   }
                   try {
                       std::rethrow_exception(error);
                   } catch (const std::system_error &e) {
                       if (state &&
                           (e.code() == exec::asio::asio_impl::error::operation_aborted ||
                            e.code() == exec::asio::asio_impl::error::bad_descriptor)) {
                           return std::optional<FdEvent>{};
                       }
                       FdEvent event;
                       event.ok_ = false;
                       event.error_ = e.code();
                       return std::optional<FdEvent>(event);
                   } catch (...) {
                       FdEvent event;
                       event.ok_ = false;
                       event.error_ = std::make_error_code(std::errc::io_error);
                       return std::optional<FdEvent>(event);
                   }
               })
             | stdexec::then([state](std::optional<FdEvent> maybe_event) {
                   if (!maybe_event.has_value()) {
                       return std::optional<FdEvent>{};
                   }
                   if (state && state->stopped.load(std::memory_order_acquire)) {
                       return std::optional<FdEvent>{};
                   }
                   return maybe_event;
               });
    };

    if (want_read && want_write) {
        auto wait_readable = state->descriptor.async_wait(
                                 exec::asio::asio_impl::posix::stream_descriptor::wait_read,
                                 exec::asio::use_sender)
                           | stdexec::then(make_readable_event);
        auto wait_writable = state->descriptor.async_wait(
                                 exec::asio::asio_impl::posix::stream_descriptor::wait_write,
                                 exec::asio::use_sender)
                           | stdexec::then(make_writable_event);
        auto any_sender = exec::when_any(std::move(wait_readable), std::move(wait_writable));
        return finalize(wait_event_with_optional_timeout(
            std::move(any_sender), timeout_ms, state->runtime));
    }
    if (want_read) {
        auto wait_readable = state->descriptor.async_wait(
                                 exec::asio::asio_impl::posix::stream_descriptor::wait_read,
                                 exec::asio::use_sender)
                           | stdexec::then(make_readable_event);
        return finalize(wait_event_with_optional_timeout(
            std::move(wait_readable), timeout_ms, state->runtime));
    }
    auto wait_writable = state->descriptor.async_wait(
                             exec::asio::asio_impl::posix::stream_descriptor::wait_write,
                             exec::asio::use_sender)
                       | stdexec::then(make_writable_event);
    return finalize(
        wait_event_with_optional_timeout(std::move(wait_writable), timeout_ms, state->runtime));
}

FdWatcher::StopSender FdWatcher::stop_sender() const {
    auto state = state_;
    return stdexec::just(std::move(state)) | stdexec::then([](std::shared_ptr<State> state) {
               if (!state) {
                   return;
               }
               const bool already = state->stopped.exchange(true, std::memory_order_acq_rel);
               if (already) {
                   return;
               }
               std::error_code ec;
               state->descriptor.cancel(ec);
               if (state->descriptor.is_open()) {
                   (void)state->descriptor.release();
               }
           });
}

Task<FdWatcher> FdWatcher::watch(uv_os_sock_t fd, int events) {
    auto *runtime = co_await get_current_runtime();
    co_return co_await watch_sender(fd, events, runtime);
}

Task<std::optional<FdEvent>> FdWatcher::next() {
    co_return co_await next_sender();
}

Task<void> FdWatcher::stop() {
    co_await stop_sender();
}

} // namespace flux
