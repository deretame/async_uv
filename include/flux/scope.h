#pragma once

#include <utility>

#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

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
