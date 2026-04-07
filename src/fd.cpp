#include "async_uv/fd.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace async_uv {

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

FdStream::WriteSomeSender FdStream::write_some_sender(std::string data) const {
    return WriteSomeSender(state_, std::move(data));
}

FdStream::WriteAllSender FdStream::write_all_sender(std::string data) const {
    return WriteAllSender(state_, std::move(data));
}

FdStream::ReadSomeSender FdStream::read_some_sender(std::size_t max_bytes) const {
    return ReadSomeSender(state_, max_bytes);
}

FdStream::ReadExactlySender FdStream::read_exactly_sender(std::size_t bytes) const {
    return ReadExactlySender(state_, bytes);
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

Task<std::size_t> FdStream::write_some(std::string_view data) const {
    if (data.empty()) {
        co_return 0;
    }
    co_return co_await write_some_sender(std::string(data));
}

Task<void> FdStream::wait_readable() const {
    co_await wait_readable_sender();
}

Task<void> FdStream::wait_writable() const {
    co_await wait_writable_sender();
}

Task<std::size_t> FdStream::write_all(std::string_view data) const {
    if (data.empty()) {
        co_return 0;
    }
    co_return co_await write_all_sender(std::string(data));
}

Task<std::string> FdStream::read_some(std::size_t max_bytes) const {
    co_return co_await read_some_sender(max_bytes);
}

Task<std::string> FdStream::read_exactly(std::size_t bytes) const {
    if (bytes == 0) {
        co_return std::string{};
    }
    co_return co_await read_exactly_sender(bytes);
}

} // namespace async_uv
