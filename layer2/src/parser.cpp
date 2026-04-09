#include "flux_http/parser.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/beast/http.hpp>

#include "flux/fs.h"
#include "flux/runtime.h"

namespace flux::http {
namespace {

namespace beast_http = boost::beast::http;
namespace beast_net = boost::asio;

Task<void> cleanup_file_task(std::string path_str) {
    try {
        if (co_await flux::Fs::exists(path_str)) {
            co_await flux::Fs::remove(path_str);
        }
    } catch (...) {
    }
    co_return;
}

void fire_and_forget_cleanup(const std::filesystem::path &path) {
    if (auto *runtime = flux::Runtime::current(); runtime != nullptr) {
        (void)runtime->spawn(cleanup_file_task(path.string()));
        return;
    }

    std::thread([path]() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }).detach();
}

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

bool exceeds_limit(std::size_t limit, std::size_t value) {
    return limit > 0 && value > limit;
}

} // namespace

std::optional<std::string> HttpMessage::header(std::string_view name) const {
    const std::string target = to_upper(std::string(name));
    for (const auto &item : headers_) {
        if (to_upper(item.name) == target) {
            return item.value;
        }
    }
    return std::nullopt;
}

std::vector<std::string> HttpMessage::header_values(std::string_view name) const {
    const std::string target = to_upper(std::string(name));
    std::vector<std::string> values;
    for (const auto &item : headers_) {
        if (to_upper(item.name) == target) {
            values.push_back(item.value);
        }
    }
    return values;
}

std::optional<std::string> HttpMessage::trailer(std::string_view name) const {
    const std::string target = to_upper(std::string(name));
    for (const auto &item : trailers_) {
        if (to_upper(item.name) == target) {
            return item.value;
        }
    }
    return std::nullopt;
}

std::vector<std::string> HttpMessage::trailer_values(std::string_view name) const {
    const std::string target = to_upper(std::string(name));
    std::vector<std::string> values;
    for (const auto &item : trailers_) {
        if (to_upper(item.name) == target) {
            values.push_back(item.value);
        }
    }
    return values;
}

class HttpMessage::BodyFileState {
public:
    explicit BodyFileState(std::filesystem::path path) : path_(std::move(path)) {}

    ~BodyFileState() {
        if (!cleanup_) {
            return;
        }
        fire_and_forget_cleanup(path_);
    }

    void set_path(std::filesystem::path path) {
        path_ = std::move(path);
    }

    void disable_cleanup() noexcept {
        cleanup_ = false;
    }

private:
    std::filesystem::path path_;
    bool cleanup_ = true;
};

Task<bool> HttpMessage::move_body_file_to(const std::filesystem::path &target, bool overwrite) {
    if (!body_file_state_) {
        co_return false;
    }

    try {
        if (target.has_parent_path()) {
            co_await flux::Fs::create_directories(target.parent_path().string());
        }

        const std::string target_path = target.string();
        if (co_await flux::Fs::exists(target_path)) {
            if (!overwrite) {
                co_return false;
            }
            co_await flux::Fs::remove(target_path);
        }

        if (!body_file_path_.has_value()) {
            co_return false;
        }

        co_await flux::Fs::rename(body_file_path_->string(), target_path);
        body_file_state_->set_path(target);
        body_file_state_->disable_cleanup();
        body_file_path_ = target;
        co_return true;
    } catch (...) {
        co_return false;
    }
}

Task<bool> HttpMessage::dispose_body_file() {
    if (!body_file_path_.has_value() || !body_file_state_) {
        co_return false;
    }

    try {
        const std::string path = body_file_path_->string();
        if (co_await flux::Fs::exists(path)) {
            co_await flux::Fs::remove(path);
        }
        body_file_state_->disable_cleanup();
        body_file_path_.reset();
        body_file_state_.reset();
        co_return true;
    } catch (...) {
        co_return false;
    }
}

class HttpParser::Impl {
public:
    explicit Impl(ParseMode mode, ParserOptions options)
        : mode_(mode), options_(std::move(options)) {}

    Task<void> feed(std::string_view chunk) {
        Runtime *runtime = nullptr;
        try {
            runtime = co_await flux::get_current_runtime();
        } catch (...) {
            throw ParseError("http parser feed requires current runtime",
                             -10,
                             ParseErrorKind::runtime_missing);
        }
        if (runtime == nullptr) {
            throw ParseError(
                "http parser feed requires current runtime", -10, ParseErrorKind::runtime_missing);
        }

        co_await ensure_temp_dir_ready();
        pending_.append(chunk.data(), chunk.size());

        while (true) {
            auto parsed = co_await try_parse_message();
            if (!parsed.has_value()) {
                break;
            }
            ready_.push_back(std::move(*parsed));
        }

        co_return;
    }

    bool has_message() const noexcept {
        return !ready_.empty();
    }

    std::optional<HttpMessage> next_message() {
        if (ready_.empty()) {
            return std::nullopt;
        }
        HttpMessage message = std::move(ready_.front());
        ready_.pop_front();
        return message;
    }

    void reset() {
        ready_.clear();
        pending_.clear();
        temp_dir_ready_ = false;
        resolved_temp_dir_ = ".";
    }

private:
    ParseError protocol_error(std::string message,
                              ParseErrorKind kind = ParseErrorKind::protocol,
                              int code = -1) const {
        return ParseError(std::move(message), code, kind);
    }

    std::pair<std::string, std::string> parse_header_line(std::string_view line,
                                                          ParseErrorKind kind) const {
        if (exceeds_limit(options_.max_header_line_size, line.size())) {
            throw protocol_error("header line exceeds parser max_header_line_size",
                                 kind);
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            throw protocol_error("invalid header line");
        }

        auto name = trim_copy(line.substr(0, colon));
        auto value = trim_copy(line.substr(colon + 1));
        if (name.empty()) {
            throw protocol_error("invalid header name");
        }
        return {std::move(name), std::move(value)};
    }

    void validate_start_line(std::string_view line, bool is_response) const {
        if (exceeds_limit(options_.max_start_line_size, line.size())) {
            throw protocol_error("start line exceeds parser max_start_line_size",
                                 ParseErrorKind::start_line_too_long);
        }

        if (mode_ == ParseMode::request && is_response) {
            throw protocol_error("response message is not allowed in request mode");
        }
        if (mode_ == ParseMode::response && !is_response) {
            throw protocol_error("request message is not allowed in response mode");
        }
    }

    std::vector<Header> parse_initial_headers(std::string_view block,
                                              std::size_t &header_count) const {
        std::vector<Header> output;
        std::size_t begin = 0;
        while (begin < block.size()) {
            const auto end = block.find("\r\n", begin);
            const auto line = end == std::string_view::npos
                                  ? block.substr(begin)
                                  : block.substr(begin, end - begin);
            if (!line.empty()) {
                if (exceeds_limit(options_.max_header_count, header_count + 1)) {
                    throw protocol_error("header count exceeds parser max_header_count",
                                         ParseErrorKind::header_count_exceeded);
                }

                auto [name, value] = parse_header_line(line, ParseErrorKind::header_line_too_long);
                output.push_back(Header{name, value});
                ++header_count;
            }

            if (end == std::string_view::npos) {
                break;
            }
            begin = end + 2;
        }
        return output;
    }

    bool headers_indicate_chunked(const std::vector<Header> &headers) const {
        for (const auto &header : headers) {
            if (to_lower(header.name) != "transfer-encoding") {
                continue;
            }
            if (to_lower(header.value).find("chunked") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void parse_chunked_trailers(std::string_view raw_message,
                                std::size_t body_offset,
                                std::size_t &header_count,
                                std::vector<Header> &trailers) const {
        std::size_t pos = body_offset;
        while (true) {
            const auto line_end = raw_message.find("\r\n", pos);
            if (line_end == std::string_view::npos) {
                throw protocol_error("invalid chunk size line");
            }

            const auto size_line = raw_message.substr(pos, line_end - pos);
            if (exceeds_limit(options_.max_header_line_size, size_line.size())) {
                throw protocol_error("header line exceeds parser max_header_line_size",
                                     ParseErrorKind::header_line_too_long);
            }

            const auto semicolon = size_line.find(';');
            const auto hex_text = trim_copy(size_line.substr(0, semicolon));
            if (hex_text.empty()) {
                throw protocol_error("invalid chunk size line");
            }

            std::size_t chunk_size = 0;
            try {
                chunk_size = static_cast<std::size_t>(std::stoull(hex_text, nullptr, 16));
            } catch (...) {
                throw protocol_error("invalid chunk size");
            }

            pos = line_end + 2;
            if (chunk_size == 0) {
                while (true) {
                    const auto trailer_end = raw_message.find("\r\n", pos);
                    if (trailer_end == std::string_view::npos) {
                        throw protocol_error("invalid trailer line");
                    }
                    if (trailer_end == pos) {
                        return;
                    }
                    if (exceeds_limit(options_.max_header_count, header_count + 1)) {
                        throw protocol_error("header count exceeds parser max_header_count",
                                             ParseErrorKind::header_count_exceeded);
                    }
                    auto [name, value] = parse_header_line(
                        raw_message.substr(pos, trailer_end - pos),
                        ParseErrorKind::header_line_too_long);
                    trailers.push_back(Header{std::move(name), std::move(value)});
                    ++header_count;
                    pos = trailer_end + 2;
                }
            }

            if (raw_message.size() < pos + chunk_size + 2) {
                throw protocol_error("incomplete chunk data");
            }
            pos += chunk_size;
            if (raw_message.substr(pos, 2) != "\r\n") {
                throw protocol_error("invalid chunk delimiter");
            }
            pos += 2;
        }
    }

    Task<void> attach_body(HttpMessage &message, std::string body) {
        message.body_size_ = static_cast<std::uintmax_t>(body.size());
        if (!exceeds_limit(options_.max_body_in_memory, body.size())) {
            message.body_ = std::move(body);
            co_return;
        }

        const auto path = create_temp_file_path();
        if (path.has_parent_path()) {
            co_await flux::Fs::create_directories(path.parent_path().string());
        }

        co_await flux::Fs::write_file(path, body);
        message.body_.clear();
        message.body_file_state_ = std::make_shared<HttpMessage::BodyFileState>(path);
        message.body_file_path_ = path;
    }

    template <bool IsRequest>
    Task<std::optional<HttpMessage>>
    parse_with_beast(std::size_t header_count,
                     std::vector<Header> initial_headers,
                     std::size_t body_offset) {
        boost::system::error_code ec;
        beast_http::parser<IsRequest, beast_http::string_body> parser;
        parser.eager(true);
        parser.body_limit(std::numeric_limits<std::uint64_t>::max());

        const auto consumed = parser.put(beast_net::buffer(pending_.data(), pending_.size()), ec);
        if (ec == beast_http::error::need_more || !parser.is_done()) {
            co_return std::nullopt;
        }

        if (ec) {
            throw protocol_error("invalid HTTP message", ParseErrorKind::protocol, ec.value());
        }

        auto message = parser.release();
        HttpMessage out;
        out.is_request_ = IsRequest;
        out.http_major_ = message.version() / 10;
        out.http_minor_ = message.version() % 10;

        if constexpr (IsRequest) {
            out.method_ = std::string(message.method_string());
            out.url_ = std::string(message.target());
            out.status_code_ = 0;
            out.reason_.clear();
        } else {
            out.status_code_ = static_cast<int>(message.result_int());
            out.reason_ = std::string(message.reason());
        }

        const bool chunked = headers_indicate_chunked(initial_headers);
        out.headers_ = std::move(initial_headers);
        if (chunked) {
            parse_chunked_trailers(std::string_view(pending_.data(), consumed),
                                   body_offset,
                                   header_count,
                                   out.trailers_);
        }

        co_await attach_body(out, std::move(message.body()));
        pending_.erase(0, consumed);
        co_return std::optional<HttpMessage>(std::move(out));
    }

    Task<std::optional<HttpMessage>> try_parse_message() {
        const auto header_end = pending_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (mode_ == ParseMode::response && pending_.size() >= 5 &&
                pending_.compare(0, 5, "HTTP/") != 0) {
                throw protocol_error("invalid response start line");
            }
            co_return std::nullopt;
        }

        const auto head = std::string_view(pending_.data(), header_end);
        const auto first_line_end = head.find("\r\n");
        const auto start_line = first_line_end == std::string_view::npos
                                    ? head
                                    : head.substr(0, first_line_end);
        const bool is_response = start_line.starts_with("HTTP/");
        validate_start_line(start_line, is_response);

        std::vector<Header> initial_headers;
        std::size_t header_count = 0;
        if (first_line_end != std::string_view::npos) {
            const auto header_block = head.substr(first_line_end + 2);
            initial_headers = parse_initial_headers(header_block, header_count);
        }

        const std::size_t body_offset = header_end + 4;
        if (is_response) {
            co_return co_await parse_with_beast<false>(
                header_count, std::move(initial_headers), body_offset);
        }
        co_return co_await parse_with_beast<true>(
            header_count, std::move(initial_headers), body_offset);
    }

    Task<void> ensure_temp_dir_ready() {
        if (temp_dir_ready_) {
            co_return;
        }

        if (options_.temp_directory.has_value()) {
            resolved_temp_dir_ = *options_.temp_directory;
            temp_dir_ready_ = true;
            co_return;
        }

        bool ready = false;
        try {
            resolved_temp_dir_ = std::filesystem::path(co_await flux::Fs::temp_directory());
            ready = true;
        } catch (...) {
        }

        if (!ready) {
            try {
                resolved_temp_dir_ =
                    std::filesystem::path(co_await flux::Fs::current_directory());
                ready = true;
            } catch (...) {
            }
        }

        if (!ready) {
            resolved_temp_dir_ = std::filesystem::path(".");
        }

        temp_dir_ready_ = true;
    }

    std::filesystem::path create_temp_file_path() const {
        static std::atomic_uint64_t seq{0};
        const auto id = seq.fetch_add(1, std::memory_order_relaxed);
        const auto file_name = "flux_http_parser_" + std::to_string(id) + ".tmp";
        return std::filesystem::path(flux::path::join(resolved_temp_dir_.string(), file_name));
    }

    ParseMode mode_ = ParseMode::response;
    ParserOptions options_{};
    std::deque<HttpMessage> ready_;
    std::string pending_;
    std::filesystem::path resolved_temp_dir_ = ".";
    bool temp_dir_ready_ = false;
};

HttpParser::HttpParser(ParseMode mode, ParserOptions options)
    : impl_(std::make_unique<Impl>(mode, std::move(options))) {}

HttpParser::~HttpParser() = default;

HttpParser::HttpParser(HttpParser &&) noexcept = default;

HttpParser &HttpParser::operator=(HttpParser &&) noexcept = default;

Task<void> HttpParser::feed(std::string_view chunk) {
    co_return co_await impl_->feed(chunk);
}

Task<bool> HttpParser::has_message() const {
    co_return impl_->has_message();
}

Task<std::optional<HttpMessage>> HttpParser::next_message() {
    co_return impl_->next_message();
}

Task<void> HttpParser::reset() {
    impl_->reset();
    co_return;
}

Task<std::optional<HttpMessage>> parse_first_message(std::string_view raw, ParseMode mode) {
    HttpParser parser(mode);
    co_await parser.feed(raw);
    co_return co_await parser.next_message();
}

Task<std::vector<HttpMessage>> parse_all_messages(std::string_view raw, ParseMode mode) {
    HttpParser parser(mode);
    co_await parser.feed(raw);

    std::vector<HttpMessage> out;
    while (auto message = co_await parser.next_message()) {
        out.push_back(std::move(*message));
    }
    co_return out;
}

} // namespace flux::http
