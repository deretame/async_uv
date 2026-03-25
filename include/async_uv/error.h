#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <uv.h>

namespace async_uv {

class Error : public std::runtime_error {
public:
    Error(std::string where, int code)
        : std::runtime_error(std::move(where) + ": " + uv_err_name(code) + " - " +
                             uv_strerror(code)),
          code_(code) {}

    int code() const noexcept {
        return code_;
    }

private:
    int code_;
};

[[noreturn]] inline void throw_uv_error(std::string_view where, int code) {
    throw Error(std::string(where), code);
}

inline void throw_if_uv_error(int code, std::string_view where) {
    if (code < 0) {
        throw_uv_error(where, code);
    }
}

} // namespace async_uv
