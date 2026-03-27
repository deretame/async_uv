#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

#include "async_uv_http/http.h"
#include "async_uv_layer2/error.h"
#include "async_uv_redis/redis.h"
#include "async_uv_sql/sql.h"
#include "async_uv_ws/ws.h"

namespace async_uv::layer2 {

inline const char *to_string_view(ErrorKind value) {
    switch (value) {
        case ErrorKind::invalid_argument:
            return "invalid_argument";
        case ErrorKind::runtime_missing:
            return "runtime_missing";
        case ErrorKind::connect_failed:
            return "connect_failed";
        case ErrorKind::not_connected:
            return "not_connected";
        case ErrorKind::timeout:
            return "timeout";
        case ErrorKind::cancelled:
            return "cancelled";
        case ErrorKind::operation_failed:
            return "operation_failed";
        case ErrorKind::internal_error:
            return "internal_error";
    }
    return "internal_error";
}

inline const char *to_string_view(sql::SqlErrorKind value) {
    switch (value) {
        case sql::SqlErrorKind::invalid_argument:
            return "invalid_argument";
        case sql::SqlErrorKind::runtime_missing:
            return "runtime_missing";
        case sql::SqlErrorKind::connect_failed:
            return "connect_failed";
        case sql::SqlErrorKind::not_connected:
            return "not_connected";
        case sql::SqlErrorKind::query_failed:
            return "query_failed";
        case sql::SqlErrorKind::internal_error:
            return "internal_error";
    }
    return "internal_error";
}

inline const char *to_string_view(redis::RedisErrorKind value) {
    switch (value) {
        case redis::RedisErrorKind::invalid_argument:
            return "invalid_argument";
        case redis::RedisErrorKind::runtime_missing:
            return "runtime_missing";
        case redis::RedisErrorKind::connect_failed:
            return "connect_failed";
        case redis::RedisErrorKind::not_connected:
            return "not_connected";
        case redis::RedisErrorKind::command_failed:
            return "command_failed";
        case redis::RedisErrorKind::internal_error:
            return "internal_error";
    }
    return "internal_error";
}

inline const char *to_string_view(http::TransportErrorKind value) {
    switch (value) {
        case http::TransportErrorKind::none:
            return "none";
        case http::TransportErrorKind::timeout:
            return "timeout";
        case http::TransportErrorKind::dns:
            return "dns";
        case http::TransportErrorKind::tls:
            return "tls";
        case http::TransportErrorKind::connect:
            return "connect";
        case http::TransportErrorKind::send:
            return "send";
        case http::TransportErrorKind::recv:
            return "recv";
        case http::TransportErrorKind::reset:
            return "reset";
        case http::TransportErrorKind::unknown:
            return "unknown";
    }
    return "unknown";
}

inline const char *to_string_view(http::HttpErrorCode value) {
    switch (value) {
        case http::HttpErrorCode::invalid_request:
            return "invalid_request";
        case http::HttpErrorCode::curl_failure:
            return "curl_failure";
        case http::HttpErrorCode::http_status_failure:
            return "http_status_failure";
    }
    return "invalid_request";
}

inline const char *to_string_view(ws::WsErrorKind value) {
    switch (value) {
        case ws::WsErrorKind::invalid_argument:
            return "invalid_argument";
        case ws::WsErrorKind::runtime_missing:
            return "runtime_missing";
        case ws::WsErrorKind::connect_failed:
            return "connect_failed";
        case ws::WsErrorKind::not_connected:
            return "not_connected";
        case ws::WsErrorKind::send_failed:
            return "send_failed";
        case ws::WsErrorKind::receive_failed:
            return "receive_failed";
        case ws::WsErrorKind::internal_error:
            return "internal_error";
    }
    return "internal_error";
}

inline const char *to_string_view(ws::MessageType value) {
    switch (value) {
        case ws::MessageType::open:
            return "open";
        case ws::MessageType::close:
            return "close";
        case ws::MessageType::text:
            return "text";
        case ws::MessageType::binary:
            return "binary";
        case ws::MessageType::error:
            return "error";
        case ws::MessageType::ping:
            return "ping";
        case ws::MessageType::pong:
            return "pong";
    }
    return "text";
}

inline std::string to_string(ErrorKind value) {
    return to_string_view(value);
}

inline std::string to_string(sql::SqlErrorKind value) {
    return to_string_view(value);
}

inline std::string to_string(redis::RedisErrorKind value) {
    return to_string_view(value);
}

inline std::string to_string(http::TransportErrorKind value) {
    return to_string_view(value);
}

inline std::string to_string(http::HttpErrorCode value) {
    return to_string_view(value);
}

inline std::string to_string(ws::WsErrorKind value) {
    return to_string_view(value);
}

inline std::string to_string(ws::MessageType value) {
    return to_string_view(value);
}

inline std::string to_string(const ws::Message &message) {
    std::string out;
    out.reserve(96 + message.data.size());
    out += "{type=";
    out += to_string_view(message.type);
    out += ",data_size=";
    out += std::to_string(message.data.size());
    if (message.close_code.has_value()) {
        out += ",close_code=";
        out += std::to_string(*message.close_code);
    }
    if (!message.reason.empty()) {
        out += ",reason=";
        out += message.reason;
    }
    out += '}';
    return out;
}

template <typename T>
inline std::string to_string(const T &value) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char *> ||
                         std::is_same_v<std::decay_t<T>, char *>) {
        return value == nullptr ? std::string("<null>") : std::string(value);
    } else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
        return std::to_string(value);
    } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
        using U = std::underlying_type_t<std::decay_t<T>>;
        return std::to_string(static_cast<U>(value));
    } else {
        return "<unsupported>";
    }
}

} // namespace async_uv::layer2
