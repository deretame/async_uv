#include "async_uv/cancel.h"

namespace async_uv {

CancellationSource::CancellationSource() : signal_(async_simple::Signal::create()) {}

CancellationSource::CancellationSource(std::shared_ptr<async_simple::Signal> signal)
    : signal_(std::move(signal)) {
    if (!signal_) {
        signal_ = async_simple::Signal::create();
    }
}

async_simple::Signal *CancellationSource::signal() const noexcept {
    return signal_.get();
}

const std::shared_ptr<async_simple::Signal> &CancellationSource::shared_signal() const noexcept {
    return signal_;
}

bool CancellationSource::cancellation_requested() const noexcept {
    return signal_ != nullptr && signal_->state() != async_simple::None;
}

async_simple::SignalType CancellationSource::cancel(async_simple::SignalType type) const noexcept {
    return signal_ == nullptr ? async_simple::None : signal_->emits(type);
}

Task<async_simple::Slot *> get_current_slot() {
    co_return co_await async_simple::coro::CurrentSlot{};
}

Task<std::shared_ptr<async_simple::Signal>> get_current_signal() {
    auto *slot = co_await get_current_slot();
    if (slot == nullptr || slot->signal() == nullptr) {
        co_return std::shared_ptr<async_simple::Signal>{};
    }

    co_return slot->signal()->shared_from_this();
}

Task<bool> cancellation_requested() {
    auto *slot = co_await get_current_slot();
    co_return async_simple::signalHelper{async_simple::Terminate}.hasCanceled(slot);
}

Task<void> throw_if_cancelled(std::string message) {
    auto *slot = co_await get_current_slot();
    async_simple::signalHelper{async_simple::Terminate}.checkHasCanceled(slot, std::move(message));
    co_return;
}

} // namespace async_uv
