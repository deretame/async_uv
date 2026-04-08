#include "flux/cancel.h"

namespace flux {

CancellationSource::CancellationSource()
    : source_(std::make_shared<stdexec::inplace_stop_source>()) {}

CancellationSource::CancellationSource(std::shared_ptr<stdexec::inplace_stop_source> source)
    : source_(source ? std::move(source) : std::make_shared<stdexec::inplace_stop_source>()) {}

stdexec::inplace_stop_source *CancellationSource::signal() const noexcept {
    return source_.get();
}

const std::shared_ptr<stdexec::inplace_stop_source> &
CancellationSource::shared_signal() const noexcept {
    return source_;
}

stdexec::inplace_stop_token CancellationSource::token() const noexcept {
    return source_ ? source_->get_token() : stdexec::inplace_stop_token{};
}

bool CancellationSource::cancellation_requested() const noexcept {
    return source_ != nullptr && source_->stop_requested();
}

bool CancellationSource::cancel() const noexcept {
    if (source_ == nullptr) {
        return false;
    }
    (void)source_->request_stop();
    return source_->stop_requested();
}

Task<stdexec::inplace_stop_token> get_current_slot() {
    co_return co_await stdexec::get_stop_token();
}

Task<std::shared_ptr<stdexec::inplace_stop_source>> get_current_signal() {
    auto token = co_await get_current_slot();
    auto source = std::make_shared<stdexec::inplace_stop_source>();
    if (token.stop_requested()) {
        source->request_stop();
    }
    co_return source;
}

Task<bool> cancellation_requested() {
    auto token = co_await get_current_slot();
    co_return token.stop_requested();
}

Task<void> throw_if_cancelled(std::string message) {
    if (co_await cancellation_requested()) {
        throw std::runtime_error(std::move(message));
    }
}

} // namespace flux
