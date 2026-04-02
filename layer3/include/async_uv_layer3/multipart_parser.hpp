#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <async_uv_layer3/context.hpp>

namespace async_uv::layer3 {

struct MultipartField {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string data;
    
    bool is_file() const { return !filename.empty(); }
};

struct MultipartFormData {
    std::unordered_map<std::string, std::vector<MultipartField>> fields;
    
    std::optional<std::string> get(std::string_view name) const {
        auto it = fields.find(std::string(name));
        if (it != fields.end() && !it->second.empty()) {
            return it->second[0].data;
        }
        return std::nullopt;
    }
    
    std::optional<MultipartField> get_file(std::string_view name) const {
        auto it = fields.find(std::string(name));
        if (it != fields.end()) {
            for (const auto& field : it->second) {
                if (field.is_file()) {
                    return field;
                }
            }
        }
        return std::nullopt;
    }
    
    const std::vector<MultipartField>* get_all(std::string_view name) const {
        auto it = fields.find(std::string(name));
        return (it != fields.end()) ? &it->second : nullptr;
    }
};

namespace detail {

inline std::string_view trim_crlf(std::string_view s) {
    while (!s.empty() && (s.front() == '\r' || s.front() == '\n')) {
        s = s.substr(1);
    }
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s = s.substr(0, s.size() - 1);
    }
    return s;
}

inline std::string_view trim_space(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s = s.substr(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s = s.substr(0, s.size() - 1);
    }
    return s;
}

inline std::unordered_map<std::string, std::string> parse_content_disposition(std::string_view header) {
    std::unordered_map<std::string, std::string> params;
    
    size_t pos = header.find(';');
    if (pos == std::string_view::npos) return params;
    
    std::string_view rest = header.substr(pos + 1);
    
    while (!rest.empty()) {
        rest = trim_space(rest);
        while (!rest.empty() && rest.front() == ';') {
            rest = rest.substr(1);
            rest = trim_space(rest);
        }
        if (rest.empty()) break;
        
        pos = rest.find('=');
        if (pos == std::string_view::npos) {
            pos = rest.find(';');
            if (pos == std::string_view::npos) break;
            rest = rest.substr(pos + 1);
            continue;
        }
        
        std::string key(trim_space(rest.substr(0, pos)));
        rest = rest.substr(pos + 1);
        rest = trim_space(rest);
        
        std::string value;
        if (!rest.empty() && rest.front() == '"') {
            rest = rest.substr(1);
            pos = rest.find('"');
            if (pos != std::string_view::npos) {
                value = std::string(rest.substr(0, pos));
                rest = rest.substr(pos + 1);
            }
        } else {
            pos = rest.find(';');
            if (pos == std::string_view::npos) {
                value = std::string(rest);
                rest = {};
            } else {
                value = std::string(rest.substr(0, pos));
                rest = rest.substr(pos + 1);
            }
        }
        
        params[std::move(key)] = std::move(value);
    }
    
    return params;
}

} // namespace detail

inline std::optional<MultipartFormData> parse_multipart(
    std::string_view body,
    std::string_view boundary
) {
    MultipartFormData result;
    
    std::string delimiter = "--" + std::string(boundary);
    std::string end_delimiter = "--" + std::string(boundary) + "--";
    
    size_t pos = 0;
    
    while (pos < body.size()) {
        size_t part_start = body.find(delimiter, pos);
        if (part_start == std::string_view::npos) break;
        
        part_start += delimiter.size();
        
        while (part_start < body.size() && (body[part_start] == '\r' || body[part_start] == '\n')) {
            ++part_start;
        }
        
        if (part_start >= body.size()) break;
        
        size_t part_end = body.find(delimiter, part_start);
        if (part_end == std::string_view::npos) {
            size_t end_pos = body.find(end_delimiter, part_start);
            if (end_pos == std::string_view::npos) break;
            part_end = end_pos;
        }
        
        std::string_view part = body.substr(part_start, part_end - part_start);
        
        size_t header_end = part.find("\r\n\r\n");
        if (header_end == std::string_view::npos) {
            header_end = part.find("\n\n");
            if (header_end == std::string_view::npos) continue;
        }
        
        std::string_view headers = part.substr(0, header_end);
        std::string_view data = part.substr(header_end);
        
        size_t data_skip = (part[header_end] == '\r') ? 4 : 2;
        if (data.size() >= data_skip) {
            data = data.substr(data_skip);
        }
        
        while (!data.empty() && (data.back() == '\r' || data.back() == '\n')) {
            data = data.substr(0, data.size() - 1);
        }
        
        MultipartField field;
        
        size_t cd_pos = headers.find("Content-Disposition:");
        if (cd_pos != std::string_view::npos) {
            size_t line_end = headers.find('\n', cd_pos);
            std::string_view cd_line = (line_end == std::string_view::npos)
                ? headers.substr(cd_pos)
                : headers.substr(cd_pos, line_end - cd_pos);
            
            auto params = detail::parse_content_disposition(cd_line);
            
            if (params.count("name")) {
                field.name = params["name"];
            }
            if (params.count("filename")) {
                field.filename = params["filename"];
            }
        }
        
        size_t ct_pos = headers.find("Content-Type:");
        if (ct_pos != std::string_view::npos) {
            size_t line_end = headers.find('\n', ct_pos);
            std::string_view ct_line = (line_end == std::string_view::npos)
                ? headers.substr(ct_pos)
                : headers.substr(ct_pos, line_end - ct_pos);
            
            size_t colon = ct_line.find(':');
            if (colon != std::string_view::npos) {
                field.content_type = std::string(detail::trim_space(ct_line.substr(colon + 1)));
            }
        }
        
        field.data = std::string(data);
        
        if (!field.name.empty()) {
            result.fields[field.name].push_back(std::move(field));
        }
        
        pos = part_end;
    }
    
    return result;
}

namespace middleware {

inline Task<void> multipart_parser(Context& ctx, Next next) {
    auto content_type = ctx.header("Content-Type");
    if (!content_type) {
        co_await next();
        co_return;
    }
    
    std::string_view ct = *content_type;
    size_t boundary_pos = ct.find("boundary=");
    if (boundary_pos == std::string_view::npos) {
        co_await next();
        co_return;
    }
    
    size_t start = boundary_pos + 9;
    size_t end = ct.find(';', start);
    
    std::string boundary;
    if (end == std::string_view::npos) {
        boundary = ct.substr(start);
    } else {
        boundary = ct.substr(start, end - start);
    }
    
    if (!boundary.empty() && boundary.front() == '"') {
        boundary = boundary.substr(1);
    }
    if (!boundary.empty() && boundary.back() == '"') {
        boundary = boundary.substr(0, boundary.size() - 1);
    }
    
    auto result = parse_multipart(ctx.body, boundary);
    if (result) {
        ctx.set_local<MultipartFormData>("multipart_data", std::move(*result));
    }
    
    co_await next();
}

} // namespace middleware

inline std::optional<MultipartFormData> get_multipart(Context& ctx) {
    return ctx.local<MultipartFormData>("multipart_data");
}

} // namespace async_uv::layer3
