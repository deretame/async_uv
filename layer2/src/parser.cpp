#include "async_uv_http/parser.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include <llhttp.h>

#include "async_uv/fs.h"
#include "async_uv/runtime.h"

namespace async_uv::http {
namespace {

Task<void> cleanup_file_task(std::string path_str) {
    try {
        if (co_await async_uv::Fs::exists(path_str)) {
            co_await async_uv::Fs::remove(path_str);
        }
    } catch (...) {
    }
    co_return;
}

void fire_and_forget_cleanup(const std::filesystem::path &path) {
    if (auto *runtime = async_uv::Runtime::current(); runtime != nullptr) {
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

bool exceeds_limit(std::size_t limit, std::size_t value) {
    return limit > 0 && value > limit;
}

ParseErrorKind classify_llhttp_error(llhttp_errno_t code, const char *reason) {
    if (code != HPE_USER) {
        return ParseErrorKind::protocol;
    }

    if (reason == nullptr) {
        return ParseErrorKind::unknown;
    }

    const std::string text(reason);
    if (text.find("max_header_count") != std::string::npos) {
        return ParseErrorKind::header_count_exceeded;
    }
    if (text.find("max_header_line_size") != std::string::npos) {
        return ParseErrorKind::header_line_too_long;
    }
    if (text.find("max_start_line_size") != std::string::npos) {
        return ParseErrorKind::start_line_too_long;
    }
    return ParseErrorKind::unknown;
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
            co_await async_uv::Fs::create_directories(target.parent_path().string());
        }

        const std::string target_path = target.string();
        if (co_await async_uv::Fs::exists(target_path)) {
            if (!overwrite) {
                co_return false;
            }
            co_await async_uv::Fs::remove(target_path);
        }

        if (!body_file_path_.has_value()) {
            co_return false;
        }

        co_await async_uv::Fs::rename(body_file_path_->string(), target_path);
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
        if (co_await async_uv::Fs::exists(path)) {
            co_await async_uv::Fs::remove(path);
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
    explicit Impl(ParseMode mode, ParserOptions options) : options_(std::move(options)) {
        llhttp_settings_init(&settings_);
        settings_.on_message_begin = &Impl::on_message_begin_cb;
        settings_.on_url = &Impl::on_url_cb;
        settings_.on_status = &Impl::on_status_cb;
        settings_.on_header_field = &Impl::on_header_field_cb;
        settings_.on_header_value = &Impl::on_header_value_cb;
        settings_.on_headers_complete = &Impl::on_headers_complete_cb;
        settings_.on_body = &Impl::on_body_cb;
        settings_.on_message_complete = &Impl::on_message_complete_cb;

        llhttp_type_t type = HTTP_RESPONSE;
        if (mode == ParseMode::request) {
            type = HTTP_REQUEST;
        } else if (mode == ParseMode::both) {
            type = HTTP_BOTH;
        }

        llhttp_init(&parser_, type, &settings_);
        parser_.data = this;
    }

    Task<void> feed(std::string_view chunk) {
        auto *runtime = co_await async_uv::get_current_runtime();
        if (runtime == nullptr) {
            throw ParseError(
                "http parser feed requires current runtime", -10, ParseErrorKind::runtime_missing);
        }

        co_await ensure_temp_dir_ready();
        last_error_kind_ = ParseErrorKind::unknown;

        const std::size_t configured_chunk =
            options_.max_feed_chunk_size == 0 ? chunk.size() : options_.max_feed_chunk_size;
        const std::size_t step = std::max<std::size_t>(1, configured_chunk);
        const std::size_t yield_every = std::max<std::size_t>(1, options_.yield_every_chunks);

        std::size_t offset = 0;
        std::size_t chunks_since_yield = 0;
        while (offset < chunk.size()) {
            const std::size_t len = std::min(step, chunk.size() - offset);
            const llhttp_errno_t rc = llhttp_execute(&parser_, chunk.data() + offset, len);
            if (rc != HPE_OK) {
                std::ostringstream oss;
                oss << "llhttp parse failed: " << llhttp_errno_name(rc);
                const char *reason = llhttp_get_error_reason(&parser_);
                if (reason != nullptr) {
                    oss << " (" << reason << ")";
                }
                const ParseErrorKind kind = last_error_kind_ != ParseErrorKind::unknown
                                                ? last_error_kind_
                                                : classify_llhttp_error(rc, reason);
                throw ParseError(oss.str(), static_cast<int>(rc), kind);
            }

            offset += len;
            ++chunks_since_yield;
            if (chunks_since_yield >= yield_every && offset < chunk.size()) {
                chunks_since_yield = 0;
                co_await async_uv::sleep_for(std::chrono::milliseconds(0));
            }
        }

        co_await flush_pending_file_writes();
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
        pending_file_writes_.clear();
        current_file_chunks_.clear();
        current_ = HttpMessage{};
        field_buffer_.clear();
        value_buffer_.clear();
        header_state_ = HeaderState::none;
        header_count_ = 0;
        headers_complete_ = false;
        parsing_trailers_ = false;
        llhttp_reset(&parser_);
        parser_.data = this;
    }

private:
    struct PendingFileWrite {
        std::filesystem::path path;
        std::vector<std::string> chunks;
    };

    enum class HeaderState {
        none,
        field,
        value,
    };

    static Impl &self(llhttp_t *parser) {
        return *static_cast<Impl *>(parser->data);
    }

    static int on_message_begin_cb(llhttp_t *parser) {
        auto &s = self(parser);
        s.current_ = HttpMessage{};
        s.field_buffer_.clear();
        s.value_buffer_.clear();
        s.current_file_chunks_.clear();
        s.header_state_ = HeaderState::none;
        s.header_count_ = 0;
        s.headers_complete_ = false;
        s.parsing_trailers_ = false;
        return 0;
    }

    static int on_url_cb(llhttp_t *parser, const char *at, size_t length) {
        auto &s = self(parser);
        s.current_.url_.append(at, length);
        if (exceeds_limit(s.options_.max_start_line_size, s.current_.url_.size())) {
            return s.fail(parser,
                          "url exceeds parser max_start_line_size",
                          ParseErrorKind::start_line_too_long);
        }
        return 0;
    }

    static int on_status_cb(llhttp_t *parser, const char *at, size_t length) {
        auto &s = self(parser);
        s.current_.reason_.append(at, length);
        if (exceeds_limit(s.options_.max_start_line_size, s.current_.reason_.size())) {
            return s.fail(parser,
                          "status reason exceeds parser max_start_line_size",
                          ParseErrorKind::start_line_too_long);
        }
        return 0;
    }

    static int on_header_field_cb(llhttp_t *parser, const char *at, size_t length) {
        auto &s = self(parser);
        if (s.header_state_ == HeaderState::value) {
            const int rc = s.flush_header(parser);
            if (rc != 0) {
                return rc;
            }
        }
        s.parsing_trailers_ = s.headers_complete_;
        s.header_state_ = HeaderState::field;
        s.field_buffer_.append(at, length);
        const std::size_t line_size = s.field_buffer_.size() + s.value_buffer_.size() + 2;
        if (exceeds_limit(s.options_.max_header_line_size, line_size)) {
            return s.fail(parser,
                          "header line exceeds parser max_header_line_size",
                          ParseErrorKind::header_line_too_long);
        }
        return 0;
    }

    static int on_header_value_cb(llhttp_t *parser, const char *at, size_t length) {
        auto &s = self(parser);
        s.parsing_trailers_ = s.headers_complete_;
        s.header_state_ = HeaderState::value;
        s.value_buffer_.append(at, length);
        const std::size_t line_size = s.field_buffer_.size() + s.value_buffer_.size() + 2;
        if (exceeds_limit(s.options_.max_header_line_size, line_size)) {
            return s.fail(parser,
                          "header line exceeds parser max_header_line_size",
                          ParseErrorKind::header_line_too_long);
        }
        return 0;
    }

    static int on_headers_complete_cb(llhttp_t *parser) {
        auto &s = self(parser);
        const int flush_rc = s.flush_header(parser);
        if (flush_rc != 0) {
            return flush_rc;
        }

        if (!s.headers_complete_) {
            s.current_.http_major_ = parser->http_major;
            s.current_.http_minor_ = parser->http_minor;
            s.current_.status_code_ = parser->status_code;

            const llhttp_type_t type = static_cast<llhttp_type_t>(llhttp_get_type(parser));
            if (type == HTTP_REQUEST) {
                s.current_.is_request_ = true;
                if (const char *name =
                        llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
                    name != nullptr) {
                    s.current_.method_ = name;
                }
            }

            if (exceeds_limit(s.options_.max_start_line_size, s.current_start_line_size())) {
                return s.fail(parser,
                              "start line exceeds parser max_start_line_size",
                              ParseErrorKind::start_line_too_long);
            }

            s.headers_complete_ = true;
        } else {
            s.parsing_trailers_ = false;
        }
        return 0;
    }

    static int on_body_cb(llhttp_t *parser, const char *at, size_t length) {
        auto &s = self(parser);
        s.append_body(at, length);
        return 0;
    }

    static int on_message_complete_cb(llhttp_t *parser) {
        auto &s = self(parser);
        const int flush_rc = s.flush_header(parser);
        if (flush_rc != 0) {
            return flush_rc;
        }
        if (s.current_.body_file_path_.has_value()) {
            PendingFileWrite write;
            write.path = *s.current_.body_file_path_;
            write.chunks = std::move(s.current_file_chunks_);
            s.pending_file_writes_.push_back(std::move(write));
            s.current_file_chunks_.clear();
        }
        s.ready_.push_back(std::move(s.current_));
        s.current_ = HttpMessage{};
        return 0;
    }

    int flush_header(llhttp_t *parser) {
        if (field_buffer_.empty()) {
            return 0;
        }

        if (exceeds_limit(options_.max_header_count, header_count_ + 1)) {
            return fail(parser,
                        "header count exceeds parser max_header_count",
                        ParseErrorKind::header_count_exceeded);
        }

        const std::size_t line_size = field_buffer_.size() + value_buffer_.size() + 2;
        if (exceeds_limit(options_.max_header_line_size, line_size)) {
            return fail(parser,
                        "header line exceeds parser max_header_line_size",
                        ParseErrorKind::header_line_too_long);
        }

        if (parsing_trailers_) {
            current_.trailers_.push_back(
                Header{std::move(field_buffer_), std::move(value_buffer_)});
        } else {
            current_.headers_.push_back(Header{std::move(field_buffer_), std::move(value_buffer_)});
        }
        ++header_count_;
        field_buffer_.clear();
        value_buffer_.clear();
        header_state_ = HeaderState::none;
        return 0;
    }

    int fail(llhttp_t *parser, const char *reason, ParseErrorKind kind) {
        last_error_kind_ = kind;
        llhttp_set_error_reason(parser, reason);
        return -1;
    }

    std::size_t current_start_line_size() const {
        const std::string version = "HTTP/" + std::to_string(current_.http_major_) + "." +
                                    std::to_string(current_.http_minor_);
        if (current_.is_request_) {
            const std::size_t method_len = current_.method_.empty() ? 0 : current_.method_.size();
            return method_len + 1 + current_.url_.size() + 1 + version.size();
        }

        const std::string status = std::to_string(current_.status_code_);
        std::size_t size = version.size() + 1 + status.size();
        if (!current_.reason_.empty()) {
            size += 1 + current_.reason_.size();
        }
        return size;
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
            resolved_temp_dir_ = std::filesystem::path(co_await async_uv::Fs::temp_directory());
            ready = true;
        } catch (...) {
        }

        if (!ready) {
            try {
                resolved_temp_dir_ =
                    std::filesystem::path(co_await async_uv::Fs::current_directory());
                ready = true;
            } catch (...) {
            }
        }

        if (!ready) {
            resolved_temp_dir_ = std::filesystem::path(".");
        }

        temp_dir_ready_ = true;
    }

    std::filesystem::path create_temp_file_path() {
        static std::atomic_uint64_t seq{0};
        const auto id = seq.fetch_add(1, std::memory_order_relaxed);
        const auto &dir = resolved_temp_dir_;
        const auto file_name = "async_uv_http_parser_" + std::to_string(id) + ".tmp";
        return std::filesystem::path(async_uv::path::join(dir.string(), file_name));
    }

    void ensure_body_file() {
        if (current_.body_file_path_.has_value()) {
            return;
        }
        const auto path = create_temp_file_path();
        current_.body_file_state_ = std::make_shared<HttpMessage::BodyFileState>(path);
        current_.body_file_path_ = path;
    }

    void append_body(const char *at, size_t length) {
        const std::uintmax_t next_size = current_.body_size_ + static_cast<std::uintmax_t>(length);

        if (!current_.body_file_path_.has_value() &&
            next_size > static_cast<std::uintmax_t>(options_.max_body_in_memory)) {
            ensure_body_file();
            if (!current_.body_.empty()) {
                current_file_chunks_.push_back(std::move(current_.body_));
                current_.body_.clear();
            }
        }

        if (current_.body_file_path_.has_value()) {
            current_file_chunks_.emplace_back(at, length);
        } else {
            current_.body_.append(at, length);
        }

        current_.body_size_ = next_size;
    }

    Task<void> flush_pending_file_writes() {
        while (!pending_file_writes_.empty()) {
            auto pending = std::move(pending_file_writes_.front());
            pending_file_writes_.pop_front();

            if (pending.path.has_parent_path()) {
                co_await async_uv::Fs::create_directories(pending.path.parent_path().string());
            }
            const auto path = pending.path.string();
            auto file = co_await async_uv::File::open(path,
                                                      async_uv::OpenFlags::create |
                                                          async_uv::OpenFlags::write_only |
                                                          async_uv::OpenFlags::append,
                                                      0644);

            for (const auto &chunk : pending.chunks) {
                co_await file.write_all(chunk);
            }
            co_await file.close();
        }
    }

    llhttp_t parser_{};
    llhttp_settings_t settings_{};
    ParserOptions options_{};
    std::deque<HttpMessage> ready_;
    std::deque<PendingFileWrite> pending_file_writes_;
    std::vector<std::string> current_file_chunks_;
    HttpMessage current_;
    std::string field_buffer_;
    std::string value_buffer_;
    HeaderState header_state_ = HeaderState::none;
    std::size_t header_count_ = 0;
    bool headers_complete_ = false;
    bool parsing_trailers_ = false;
    std::filesystem::path resolved_temp_dir_ = ".";
    bool temp_dir_ready_ = false;
    ParseErrorKind last_error_kind_ = ParseErrorKind::unknown;
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

} // namespace async_uv::http
