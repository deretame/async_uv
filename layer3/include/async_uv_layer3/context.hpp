#pragma once

#include <any>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <rfl.hpp>
#include <rfl/json.hpp>

#include <async_uv/fs.h>
#include <async_uv_layer3/types.hpp>
#include <async_uv_http/server.h>
#include <async_uv_http/parser.h>

namespace async_uv::layer3 {

class StreamWriter;

using StreamHandler = std::function<Task<void>(StreamWriter&)>;

struct Context : http::ServerRequest {
    http::ServerResponse response;
    std::map<std::string, std::string> params;
    std::map<std::string, std::any> locals;
    StreamHandler stream_handler;

    explicit Context(http::ServerRequest&& req)
        : ServerRequest(std::move(req)) {}

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = default;
    Context& operator=(Context&&) = default;

    std::string_view param(std::string_view name) const {
        auto it = params.find(std::string(name));
        if (it != params.end()) {
            return it->second;
        }
        return {};
    }

    template<typename T>
    std::optional<T> local(std::string_view name) const {
        auto it = locals.find(std::string(name));
        if (it != locals.end()) {
            const T* ptr = std::any_cast<T>(&it->second);
            if (ptr) {
                return *ptr;
            }
        }
        return std::nullopt;
    }

    template<typename T>
    void set_local(std::string_view name, T&& value) {
        locals.emplace(std::string(name), std::forward<T>(value));
    }

    template<typename T>
    std::optional<T> json_as() const {
        auto result = rfl::json::read<T>(body);
        if (result) {
            return std::move(*result);
        }
        return std::nullopt;
    }

    template<typename T>
    void json(const T& obj) {
        set("Content-Type", "application/json");
        response.body = rfl::json::write(obj);
    }

    void json_raw(std::string body_content) {
        set("Content-Type", "application/json");
        response.body = std::move(body_content);
    }

    void status(int code) {
        response.status_code = code;
    }

    void set(std::string_view name, std::string_view value) {
        for (auto& h : response.headers) {
            if (iequals_ascii(h.name, name)) {
                h.value = std::string(value);
                return;
            }
        }
        response.headers.push_back({std::string(name), std::string(value)});
    }

    void redirect(const std::string& url, int code = 302) {
        response.status_code = code;
        response.headers.push_back({"Location", url});
        response.body = "";
    }

    void redirect_permanent(const std::string& url) {
        redirect(url, 301);
    }

    void redirect_temporary(const std::string& url) {
        redirect(url, 307);
    }

    void attachment(std::string filename = "") {
        if (!filename.empty()) {
            response.headers.push_back({"Content-Disposition", 
                "attachment; filename=\"" + filename + "\""});
        } else {
            response.headers.push_back({"Content-Disposition", "attachment"});
        }
    }

    void attachment_inline(std::string filename = "") {
        if (!filename.empty()) {
            response.headers.push_back({"Content-Disposition", 
                "inline; filename=\"" + filename + "\""});
        }
    }

    void content_type(std::string_view type) {
        set("Content-Type", type);
    }

    void send(std::string body_content, int code = 200) {
        response.status_code = code;
        response.body = std::move(body_content);
    }

    void send_status(int code) {
        response.status_code = code;
        response.body = "";
    }

    static std::string_view mime_type(std::string_view path) {
        static const std::unordered_map<std::string_view, std::string_view> kMime = {
            {".html", "text/html"},
            {".htm", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".txt", "text/plain"},
            {".xml", "application/xml"},
            {".csv", "text/csv"},
            {".pdf", "application/pdf"},
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif", "image/gif"},
            {".svg", "image/svg+xml"},
            {".webp", "image/webp"},
            {".ico", "image/x-icon"},
            {".mp4", "video/mp4"},
            {".mp3", "audio/mpeg"},
            {".wav", "audio/wav"},
            {".woff", "font/woff"},
            {".woff2", "font/woff2"}
        };

        const auto dot = path.find_last_of('.');
        if (dot == std::string_view::npos) {
            return "application/octet-stream";
        }
        std::string ext(path.substr(dot));
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        const std::string_view ext_view(ext);
        auto it = kMime.find(ext_view);
        if (it == kMime.end()) {
            return "application/octet-stream";
        }
        return it->second;
    }

    void send_file(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            status(404);
            json_raw("{\"error\":\"Not Found\"}");
            return;
        }

        response.body.assign(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
        if (!has_response_header("Content-Type")) {
            content_type(mime_type(path.string()));
        }
    }

    void download(const std::filesystem::path& path, std::string_view filename = {}) {
        send_file(path);
        if (response.status_code >= 400) {
            return;
        }

        std::string out_name = filename.empty() ? path.filename().string() : std::string(filename);
        set("Content-Disposition", "attachment; filename=\"" + out_name + "\"");
    }

    void stream(StreamHandler handler) {
        stream_handler = std::move(handler);
        response.chunked = true;
    }

    bool is_streaming() const noexcept {
        return static_cast<bool>(stream_handler);
    }

    bool has_body_file() const noexcept {
        return body_file_path.has_value();
    }

    const std::optional<std::filesystem::path>& body_file_path_v() const noexcept {
        return body_file_path;
    }

    Task<void> save_body_to(std::filesystem::path target_path) {
        if (has_body_file()) {
            co_await async_uv::Fs::rename(body_file_path->string(), target_path.string());
            body_file_path = target_path;
        } else {
            auto file = co_await async_uv::File::open(
                target_path.string(),
                async_uv::OpenFlags::create | async_uv::OpenFlags::write_only | async_uv::OpenFlags::truncate,
                0644);
            co_await file.write_all(body);
            co_await file.close();
        }
    }

    Task<std::string> read_body() {
        if (has_body_file()) {
            auto file = co_await async_uv::File::open(
                body_file_path->string(),
                async_uv::OpenFlags::read_only);
            auto content = co_await file.read_all();
            co_await file.close();
            co_return content;
        } else {
            co_return body;
        }
    }

    std::optional<std::string> header(std::string_view name) const {
        for (const auto& h : headers) {
            if (iequals_ascii(h.name, name)) {
                return h.value;
            }
        }
        return std::nullopt;
    }

private:
    static bool iequals_ascii(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            const char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            const char y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (x != y) {
                return false;
            }
        }
        return true;
    }

    bool has_response_header(std::string_view name) const {
        for (const auto& h : response.headers) {
            if (iequals_ascii(h.name, name)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace async_uv::layer3
