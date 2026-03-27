#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "async_uv_http/http.h"
#include "async_uv/task.h"

namespace async_uv::http {

enum class ParseMode {
    request,
    response,
    both,
};

class HttpMessage {
public:
    bool is_request() const noexcept {
        return is_request_;
    }

    const std::string &method() const noexcept {
        return method_;
    }

    const std::string &url() const noexcept {
        return url_;
    }

    int status_code() const noexcept {
        return status_code_;
    }

    const std::string &reason() const noexcept {
        return reason_;
    }

    int http_major() const noexcept {
        return http_major_;
    }

    int http_minor() const noexcept {
        return http_minor_;
    }

    const std::vector<Header> &headers() const noexcept {
        return headers_;
    }

    std::optional<std::string> header(std::string_view name) const;
    std::vector<std::string> header_values(std::string_view name) const;

    const std::vector<Header> &trailers() const noexcept {
        return trailers_;
    }

    std::optional<std::string> trailer(std::string_view name) const;
    std::vector<std::string> trailer_values(std::string_view name) const;

    const std::string &body() const noexcept {
        return body_;
    }

    bool body_in_file() const noexcept {
        return body_file_path_.has_value();
    }

    const std::optional<std::filesystem::path> &body_file_path() const noexcept {
        return body_file_path_;
    }

    std::uintmax_t body_size() const noexcept {
        return body_size_;
    }

    Task<bool> move_body_file_to(const std::filesystem::path &target, bool overwrite = true);
    Task<bool> dispose_body_file();

private:
    friend class HttpParser;
    class Builder;
    class BodyFileState;

    bool is_request_ = false;
    std::string method_;
    std::string url_;
    int status_code_ = 0;
    std::string reason_;
    int http_major_ = 0;
    int http_minor_ = 0;
    std::vector<Header> headers_;
    std::vector<Header> trailers_;
    std::string body_;
    std::optional<std::filesystem::path> body_file_path_;
    std::uintmax_t body_size_ = 0;
    std::shared_ptr<BodyFileState> body_file_state_;
};

struct ParserOptions {
    std::size_t max_body_in_memory = 4 * 1024 * 1024;
    std::optional<std::filesystem::path> temp_directory;
    std::size_t max_header_count = 512;
    std::size_t max_header_line_size = 16 * 1024;
    std::size_t max_start_line_size = 8 * 1024;
    std::size_t max_feed_chunk_size = 64 * 1024;
    std::size_t yield_every_chunks = 8;
};

enum class ParseErrorKind {
    unknown,
    runtime_missing,
    protocol,
    header_count_exceeded,
    header_line_too_long,
    start_line_too_long,
};

class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, int code, ParseErrorKind kind = ParseErrorKind::unknown)
        : std::runtime_error(std::move(message)), code_(code), kind_(kind) {}

    int code() const noexcept {
        return code_;
    }

    ParseErrorKind kind() const noexcept {
        return kind_;
    }

private:
    int code_ = 0;
    ParseErrorKind kind_ = ParseErrorKind::unknown;
};

class HttpParser {
public:
    explicit HttpParser(ParseMode mode = ParseMode::response, ParserOptions options = {});
    ~HttpParser();

    HttpParser(HttpParser &&) noexcept;
    HttpParser &operator=(HttpParser &&) noexcept;

    HttpParser(const HttpParser &) = delete;
    HttpParser &operator=(const HttpParser &) = delete;

    Task<void> feed(std::string_view chunk);
    Task<bool> has_message() const;
    Task<std::optional<HttpMessage>> next_message();
    Task<void> reset();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

Task<std::optional<HttpMessage>> parse_first_message(std::string_view raw,
                                                     ParseMode mode = ParseMode::response);
Task<std::vector<HttpMessage>> parse_all_messages(std::string_view raw,
                                                  ParseMode mode = ParseMode::response);

} // namespace async_uv::http
