#pragma once

#include "flux_http/http.h"
#include "flux_redis/redis.h"
#include "flux_sql/sql.h"
#include "flux_ws/ws.h"

namespace flux::layer2 {

enum class ErrorKind {
    invalid_argument,
    runtime_missing,
    connect_failed,
    not_connected,
    timeout,
    cancelled,
    operation_failed,
    internal_error,
};

inline ErrorKind to_error_kind(sql::SqlErrorKind kind) {
    switch (kind) {
        case sql::SqlErrorKind::invalid_argument:
            return ErrorKind::invalid_argument;
        case sql::SqlErrorKind::runtime_missing:
            return ErrorKind::runtime_missing;
        case sql::SqlErrorKind::connect_failed:
            return ErrorKind::connect_failed;
        case sql::SqlErrorKind::not_connected:
            return ErrorKind::not_connected;
        case sql::SqlErrorKind::query_failed:
            return ErrorKind::operation_failed;
        case sql::SqlErrorKind::internal_error:
            return ErrorKind::internal_error;
    }
    return ErrorKind::internal_error;
}

inline ErrorKind to_error_kind(redis::RedisErrorKind kind) {
    switch (kind) {
        case redis::RedisErrorKind::invalid_argument:
            return ErrorKind::invalid_argument;
        case redis::RedisErrorKind::runtime_missing:
            return ErrorKind::runtime_missing;
        case redis::RedisErrorKind::connect_failed:
            return ErrorKind::connect_failed;
        case redis::RedisErrorKind::not_connected:
            return ErrorKind::not_connected;
        case redis::RedisErrorKind::command_failed:
            return ErrorKind::operation_failed;
        case redis::RedisErrorKind::internal_error:
            return ErrorKind::internal_error;
    }
    return ErrorKind::internal_error;
}

inline ErrorKind to_error_kind(http::HttpErrorCode code, http::TransportErrorKind transport_kind) {
    if (code == http::HttpErrorCode::invalid_request) {
        return ErrorKind::invalid_argument;
    }
    if (code == http::HttpErrorCode::http_status_failure) {
        return ErrorKind::operation_failed;
    }
    if (transport_kind == http::TransportErrorKind::timeout) {
        return ErrorKind::timeout;
    }
    if (transport_kind == http::TransportErrorKind::connect) {
        return ErrorKind::connect_failed;
    }
    if (transport_kind == http::TransportErrorKind::none) {
        return ErrorKind::operation_failed;
    }
    return ErrorKind::internal_error;
}

inline ErrorKind to_error_kind(const ws::WebSocketError &) {
    return ErrorKind::operation_failed;
}

} // namespace flux::layer2
