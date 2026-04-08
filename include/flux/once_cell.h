#pragma once

#include <mutex>
#include <optional>
#include <utility>

namespace flux {

template <typename T>
class OnceCell {
public:
    OnceCell() = default;

    OnceCell(const OnceCell &) = delete;
    OnceCell &operator=(const OnceCell &) = delete;

    template <typename F>
    const T &get_or_init(F &&init_func) {
        std::call_once(flag_, [&]() {
            value_.emplace(std::forward<F>(init_func)());
        });
        return *value_;
    }

private:
    std::once_flag flag_;
    std::optional<T> value_;
};

} // namespace flux
