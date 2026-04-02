#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace async_uv::layer3 {

enum class ErrorCode {
    Ok = 0,
    
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    Conflict = 409,
    PayloadTooLarge = 413,
    UnsupportedMediaType = 415,
    UnprocessableEntity = 422,
    TooManyRequests = 429,
    
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
};

enum class HttpError : int {
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    PayloadTooLarge = 413,
    UnsupportedMediaType = 415,
    TooManyRequests = 429,
    InternalServerError = 500,
};

enum class BizError : int {
    InvalidJson = 10001,
    MissingField = 10002,
    InvalidField = 10003,
    InvalidToken = 11001,
    TokenExpired = 11002,
    ResourceNotFound = 14001,
    DatabaseError = 15001,
    ExternalServiceError = 15002,
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    std::string details;
    
    Error() = default;
    
    Error(ErrorCode c, std::string msg = "", std::string det = "")
        : code(c), message(std::move(msg)), details(std::move(det)) {}
    
    explicit operator bool() const noexcept {
        return code != ErrorCode::Ok;
    }
    
    int status_code() const noexcept {
        return static_cast<int>(code);
    }
    
    std::string_view status_text() const noexcept {
        switch (code) {
            case ErrorCode::BadRequest: return "Bad Request";
            case ErrorCode::Unauthorized: return "Unauthorized";
            case ErrorCode::Forbidden: return "Forbidden";
            case ErrorCode::NotFound: return "Not Found";
            case ErrorCode::MethodNotAllowed: return "Method Not Allowed";
            case ErrorCode::Conflict: return "Conflict";
            case ErrorCode::PayloadTooLarge: return "Payload Too Large";
            case ErrorCode::UnsupportedMediaType: return "Unsupported Media Type";
            case ErrorCode::UnprocessableEntity: return "Unprocessable Entity";
            case ErrorCode::TooManyRequests: return "Too Many Requests";
            case ErrorCode::InternalServerError: return "Internal Server Error";
            case ErrorCode::NotImplemented: return "Not Implemented";
            case ErrorCode::ServiceUnavailable: return "Service Unavailable";
            case ErrorCode::GatewayTimeout: return "Gateway Timeout";
            default: return "OK";
        }
    }
    
    std::string to_json() const {
        std::string result = R"({"code":)" + std::to_string(static_cast<int>(code));
        result += R"(,"message":")";
        for (char c : message) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        result += '"';
        if (!details.empty()) {
            result += R"(,"details":")";
            for (char c : details) {
                switch (c) {
                    case '"': result += "\\\""; break;
                    case '\\': result += "\\\\"; break;
                    case '\n': result += "\\n"; break;
                    case '\r': result += "\\r"; break;
                    case '\t': result += "\\t"; break;
                    default: result += c; break;
                }
            }
            result += '"';
        }
        result += '}';
        return result;
    }
};

inline std::string error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "OK";
        case ErrorCode::BadRequest: return "BAD_REQUEST";
        case ErrorCode::Unauthorized: return "UNAUTHORIZED";
        case ErrorCode::Forbidden: return "FORBIDDEN";
        case ErrorCode::NotFound: return "ROUTE_NOT_FOUND";
        case ErrorCode::MethodNotAllowed: return "METHOD_NOT_ALLOWED";
        case ErrorCode::Conflict: return "CONFLICT";
        case ErrorCode::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
        case ErrorCode::UnsupportedMediaType: return "UNSUPPORTED_MEDIA_TYPE";
        case ErrorCode::UnprocessableEntity: return "INVALID_FIELD";
        case ErrorCode::TooManyRequests: return "TOO_MANY_REQUESTS";
        case ErrorCode::InternalServerError: return "INTERNAL_ERROR";
        case ErrorCode::NotImplemented: return "NOT_IMPLEMENTED";
        case ErrorCode::ServiceUnavailable: return "SERVICE_UNAVAILABLE";
        case ErrorCode::GatewayTimeout: return "GATEWAY_TIMEOUT";
    }
    return "INTERNAL_ERROR";
}

inline std::optional<int> error_biz_code(ErrorCode code) {
    switch (code) {
        case ErrorCode::BadRequest:
            return static_cast<int>(BizError::InvalidJson);
        case ErrorCode::UnprocessableEntity:
            return static_cast<int>(BizError::InvalidField);
        case ErrorCode::Unauthorized:
            return static_cast<int>(BizError::InvalidToken);
        default:
            return std::nullopt;
    }
}

struct AppError {
    HttpError http_error;
    std::string code;
    std::optional<BizError> biz_error;
    std::string message;
    std::optional<std::string> detail;
};

namespace errors {

inline Error bad_request(std::string message = "", std::string details = "") {
    return Error(ErrorCode::BadRequest, std::move(message), std::move(details));
}

inline Error unauthorized(std::string message = "", std::string details = "") {
    return Error(ErrorCode::Unauthorized, std::move(message), std::move(details));
}

inline Error forbidden(std::string message = "", std::string details = "") {
    return Error(ErrorCode::Forbidden, std::move(message), std::move(details));
}

inline Error not_found(std::string message = "", std::string details = "") {
    return Error(ErrorCode::NotFound, std::move(message), std::move(details));
}

inline Error method_not_allowed(std::string message = "", std::string details = "") {
    return Error(ErrorCode::MethodNotAllowed, std::move(message), std::move(details));
}

inline Error conflict(std::string message = "", std::string details = "") {
    return Error(ErrorCode::Conflict, std::move(message), std::move(details));
}

inline Error payload_too_large(std::string message = "", std::string details = "") {
    return Error(ErrorCode::PayloadTooLarge, std::move(message), std::move(details));
}

inline Error unsupported_media_type(std::string message = "", std::string details = "") {
    return Error(ErrorCode::UnsupportedMediaType, std::move(message), std::move(details));
}

inline Error unprocessable_entity(std::string message = "", std::string details = "") {
    return Error(ErrorCode::UnprocessableEntity, std::move(message), std::move(details));
}

inline Error too_many_requests(std::string message = "", std::string details = "") {
    return Error(ErrorCode::TooManyRequests, std::move(message), std::move(details));
}

inline Error internal_server_error(std::string message = "", std::string details = "") {
    return Error(ErrorCode::InternalServerError, std::move(message), std::move(details));
}

inline Error not_implemented(std::string message = "", std::string details = "") {
    return Error(ErrorCode::NotImplemented, std::move(message), std::move(details));
}

inline Error service_unavailable(std::string message = "", std::string details = "") {
    return Error(ErrorCode::ServiceUnavailable, std::move(message), std::move(details));
}

}

struct FrameworkError : public std::runtime_error {
    Error error;
    
    FrameworkError(ErrorCode code, std::string message = "", std::string details = "")
        : std::runtime_error(message.empty() ? "HTTP error" : message)
        , error(code, std::move(message), std::move(details)) {}
    
    explicit FrameworkError(Error err)
        : std::runtime_error(err.message.empty() ? "HTTP error" : err.message)
        , error(std::move(err)) {}
};

struct AppErrorException : public std::runtime_error {
    AppError error;

    explicit AppErrorException(AppError err)
        : std::runtime_error(err.message.empty() ? "Application error" : err.message)
        , error(std::move(err)) {}
};

} // namespace async_uv::layer3
