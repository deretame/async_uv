#pragma once

#include <any>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
        response.headers.push_back({"Content-Type", "application/json"});
        response.body = rfl::json::write(obj);
    }

    void json_raw(std::string body_content) {
        response.headers.push_back({"Content-Type", "application/json"});
        response.body = std::move(body_content);
    }

    void status(int code) {
        response.status_code = code;
    }

    void set(std::string_view name, std::string_view value) {
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
        response.headers.push_back({"Content-Type", std::string(type)});
    }

    void send(std::string body_content, int code = 200) {
        response.status_code = code;
        response.body = std::move(body_content);
    }

    void send_status(int code) {
        response.status_code = code;
        response.body = "";
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
            if (h.name.size() == name.size()) {
                bool match = true;
                for (size_t i = 0; i < name.size(); ++i) {
                    char a = std::tolower(static_cast<unsigned char>(h.name[i]));
                    char b = std::tolower(static_cast<unsigned char>(name[i]));
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) return h.value;
            }
        }
        return std::nullopt;
    }
};

} // namespace async_uv::layer3