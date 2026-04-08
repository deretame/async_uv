#pragma once

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace flux {

class Error : public std::runtime_error {
public:
    Error(std::string where, int code)
        : std::runtime_error(std::move(where) + ": " + to_message(code)), code_(code) {}

    int code() const noexcept {
        return code_;
    }

private:
    static std::string to_message(int code) {
        const int normalized = code < 0 ? -code : code;
        return std::error_code(normalized, std::generic_category()).message();
    }

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

} // namespace flux

