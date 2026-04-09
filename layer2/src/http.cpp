#include "flux_http/http.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/encoding_opts.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/pct_string_view.hpp>
#include <boost/url/url.hpp>
#include <openssl/ssl.h>

#include "flux/fs.h"

namespace flux::http {
namespace {

namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
namespace beast_net = boost::asio;
namespace beast_ssl = boost::asio::ssl;
using BeastTcp = beast_net::ip::tcp;

std::string trim(std::string value) {
    auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    const auto first = std::find_if(value.begin(), value.end(), not_space);
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
    return std::string(first, last);
}

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool has_header(const std::vector<Header> &headers, std::string_view name) {
    const std::string target = to_upper(std::string(name));
    for (const auto &header : headers) {
        if (to_upper(header.name) == target) {
            return true;
        }
    }
    return false;
}

void set_header_if_absent(std::vector<Header> &headers,
                          std::string_view name,
                          std::string_view value) {
    if (!has_header(headers, name)) {
        headers.push_back(Header{std::string(name), std::string(value)});
    }
}

ResponseFormat parse_response_format(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lower == "json" || lower == "application/json") {
        return ResponseFormat::json;
    }
    if (lower == "form" || lower == "application/x-www-form-urlencoded") {
        return ResponseFormat::form;
    }
    if (lower == "xml" || lower == "application/xml" || lower == "text/xml") {
        return ResponseFormat::xml;
    }
    if (lower == "text" || lower == "text/plain") {
        return ResponseFormat::text;
    }

    throw HttpError("unsupported response format", HttpErrorCode::invalid_request);
}

RequestFormat parse_request_format(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lower == "json" || lower == "application/json") {
        return RequestFormat::json;
    }
    if (lower == "form" || lower == "application/x-www-form-urlencoded") {
        return RequestFormat::form;
    }
    if (lower == "xml" || lower == "application/xml" || lower == "text/xml") {
        return RequestFormat::xml;
    }
    if (lower == "text" || lower == "text/plain") {
        return RequestFormat::text;
    }
    if (lower == "binary" || lower == "application/octet-stream") {
        return RequestFormat::binary;
    }
    if (lower == "multipart" || lower == "multipart/form-data") {
        return RequestFormat::multipart;
    }

    throw HttpError("unsupported request format", HttpErrorCode::invalid_request);
}

std::string_view accept_value_for_format(ResponseFormat format) {
    switch (format) {
        case ResponseFormat::json:
            return "application/json";
        case ResponseFormat::form:
            return "application/x-www-form-urlencoded";
        case ResponseFormat::xml:
            return "application/xml";
        case ResponseFormat::text:
            return "text/plain";
    }
    return "*/*";
}

std::optional<std::string_view> content_type_for_request_format(RequestFormat format) {
    switch (format) {
        case RequestFormat::json:
            return "application/json";
        case RequestFormat::form:
            return "application/x-www-form-urlencoded";
        case RequestFormat::xml:
            return "application/xml";
        case RequestFormat::text:
            return "text/plain";
        case RequestFormat::binary:
            return "application/octet-stream";
        case RequestFormat::multipart:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string decode_component(std::string_view text, bool plus_as_space) {
    boost::urls::encoding_opts opts;
    opts.space_as_plus = plus_as_space;
    const auto decoded_input = boost::urls::make_pct_string_view(text);
    if (!decoded_input) {
        std::string fallback(text);
        if (plus_as_space) {
            std::replace(fallback.begin(), fallback.end(), '+', ' ');
        }
        return fallback;
    }

    const auto decoded = decoded_input->decode(opts);
    return std::string(decoded.begin(), decoded.end());
}

TransportErrorKind map_transport_error_kind(const boost::system::error_code &ec) {
    if (!ec) {
        return TransportErrorKind::none;
    }

    if (ec == boost::asio::error::timed_out || ec == boost::beast::error::timeout) {
        return TransportErrorKind::timeout;
    }

    if (ec == boost::asio::error::host_not_found ||
        ec == boost::asio::error::host_not_found_try_again ||
        ec == boost::asio::error::network_unreachable) {
        return TransportErrorKind::dns;
    }

    if (ec == boost::asio::error::connection_refused ||
        ec == boost::asio::error::connection_aborted ||
        ec == boost::asio::error::host_unreachable) {
        return TransportErrorKind::connect;
    }

    if (ec.category() == beast_net::error::get_ssl_category()) {
        return TransportErrorKind::tls;
    }

    if (ec == boost::asio::error::broken_pipe || ec == boost::asio::error::connection_reset ||
        ec == boost::asio::error::eof || ec == beast_http::error::end_of_stream) {
        return TransportErrorKind::reset;
    }

    return TransportErrorKind::unknown;
}

[[noreturn]] void throw_beast_transport_error(std::string_view where,
                                              const boost::system::error_code &ec) {
    std::ostringstream oss;
    oss << where << " failed: " << ec.message();
    throw HttpError(oss.str(),
                    HttpErrorCode::curl_failure,
                    ec.value(),
                    0,
                    map_transport_error_kind(ec));
}

bool is_idempotent_method(std::string_view method) {
    return method == "GET" || method == "HEAD" || method == "PUT" || method == "DELETE" ||
           method == "OPTIONS";
}

bool should_retry_error(const RetryPolicy &policy,
                        const HttpError &error,
                        std::string_view method,
                        int attempt_index) {
    if (!policy.enabled || attempt_index > policy.max_attempts) {
        return false;
    }
    if (policy.retry_idempotent_only && !is_idempotent_method(method)) {
        return false;
    }

    if (error.code() == HttpErrorCode::http_status_failure) {
        return policy.retry_on_http_5xx && error.status_code() >= 500 && error.status_code() <= 599;
    }
    if (error.code() == HttpErrorCode::curl_failure) {
        const auto kind = error.transport_kind();
        return kind == TransportErrorKind::timeout || kind == TransportErrorKind::dns ||
               kind == TransportErrorKind::connect || kind == TransportErrorKind::recv ||
               kind == TransportErrorKind::send || kind == TransportErrorKind::reset;
    }
    return false;
}

bool equals_ci(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::optional<std::string_view> find_header_value(const std::vector<Header> &headers,
                                                  std::string_view name) {
    for (const auto &header : headers) {
        if (equals_ci(header.name, name)) {
            return std::string_view(header.value);
        }
    }
    return std::nullopt;
}

void erase_header(std::vector<Header> &headers, std::string_view name) {
    headers.erase(std::remove_if(headers.begin(),
                                 headers.end(),
                                 [&](const Header &header) {
                                     return equals_ci(header.name, name);
                                 }),
                  headers.end());
}

void set_or_replace_header(std::vector<Header> &headers,
                           std::string_view name,
                           std::string value) {
    for (auto &header : headers) {
        if (equals_ci(header.name, name)) {
            header.value = std::move(value);
            return;
        }
    }
    headers.push_back(Header{std::string(name), std::move(value)});
}

bool is_redirect_status(long status_code) {
    return status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 ||
           status_code == 308;
}

std::optional<std::string> resolve_redirect_url(std::string_view base_url, std::string_view location) {
    const auto parsed_location = boost::urls::parse_uri_reference(location);
    if (!parsed_location) {
        return std::nullopt;
    }

    if (parsed_location->has_scheme()) {
        return std::string(parsed_location->buffer());
    }

    const auto parsed_base = boost::urls::parse_uri(base_url);
    if (!parsed_base) {
        return std::nullopt;
    }

    boost::urls::url resolved(*parsed_base);
    if (!resolved.resolve(*parsed_location)) {
        return std::nullopt;
    }
    return std::string(resolved.buffer());
}

std::string make_default_port(std::string_view scheme) {
    if (equals_ci(scheme, "https")) {
        return "443";
    }
    return "80";
}

std::string make_http_target(const boost::urls::url_view_base &url) {
    std::string target = std::string(url.encoded_path());
    if (target.empty()) {
        target = "/";
    }
    if (url.has_query()) {
        target.push_back('?');
        target += url.encoded_query();
    }
    return target;
}

std::string random_multipart_boundary() {
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned long long> dist;
    std::ostringstream oss;
    oss << "flux-boundary-" << std::hex << dist(rng) << dist(rng);
    return oss.str();
}

std::string quote_multipart_token(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string read_file_binary(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw HttpError("multipart file open failed: " + path.string(), HttpErrorCode::invalid_request);
    }

    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0) {
        throw HttpError("multipart file stat failed: " + path.string(), HttpErrorCode::invalid_request);
    }

    std::string data;
    data.resize(static_cast<std::size_t>(size));
    if (!data.empty()) {
        in.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (!in) {
            throw HttpError(
                "multipart file read failed: " + path.string(), HttpErrorCode::invalid_request);
        }
    }
    return data;
}

std::string build_multipart_body(const std::vector<Request::MultipartPart> &parts,
                                 const std::string &boundary) {
    std::string body;
    for (const auto &part : parts) {
        body += "--";
        body += boundary;
        body += "\r\n";
        body += "Content-Disposition: form-data; name=\"";
        body += quote_multipart_token(part.name);
        body += "\"";
        if (part.file_name.has_value()) {
            body += "; filename=\"";
            body += quote_multipart_token(*part.file_name);
            body += "\"";
        }
        body += "\r\n";
        if (part.content_type.has_value()) {
            body += "Content-Type: ";
            body += *part.content_type;
            body += "\r\n";
        }
        body += "\r\n";

        if (part.file_path.has_value()) {
            body += read_file_binary(*part.file_path);
        } else {
            body += part.value;
        }

        body += "\r\n";
    }
    body += "--";
    body += boundary;
    body += "--\r\n";
    return body;
}

struct CookieEntry {
    std::string domain;
    std::string path = "/";
    std::string name;
    std::string value;
    bool secure = false;
};

bool cookie_domain_matches(std::string_view request_host, std::string_view cookie_domain) {
    const std::string host = to_lower(request_host);
    std::string domain = to_lower(cookie_domain);
    if (!domain.empty() && domain.front() == '.') {
        domain.erase(domain.begin());
    }
    if (domain.empty()) {
        return false;
    }
    if (host == domain) {
        return true;
    }
    return host.size() > domain.size() && host.ends_with(domain) &&
           host[host.size() - domain.size() - 1] == '.';
}

bool cookie_path_matches(std::string_view request_path, std::string_view cookie_path) {
    const std::string_view normalized = cookie_path.empty() ? std::string_view("/") : cookie_path;
    return request_path.starts_with(normalized);
}

std::vector<CookieEntry> load_cookie_jar_file(const std::filesystem::path &path) {
    std::vector<CookieEntry> cookies;
    std::ifstream in(path);
    if (!in) {
        return cookies;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        CookieEntry item;
        std::string secure;
        if (!std::getline(row, item.domain, '\t') || !std::getline(row, item.path, '\t') ||
            !std::getline(row, secure, '\t') || !std::getline(row, item.name, '\t') ||
            !std::getline(row, item.value)) {
            continue;
        }
        item.secure = secure == "1";
        if (!item.domain.empty() && !item.name.empty()) {
            cookies.push_back(std::move(item));
        }
    }

    return cookies;
}

void save_cookie_jar_file(const std::filesystem::path &path, const std::vector<CookieEntry> &cookies) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# flux beast cookie jar v1\n";
    for (const auto &cookie : cookies) {
        out << cookie.domain << '\t' << cookie.path << '\t' << (cookie.secure ? '1' : '0') << '\t'
            << cookie.name << '\t' << cookie.value << '\n';
    }
}

void upsert_cookie(std::vector<CookieEntry> &cookies, CookieEntry entry) {
    auto it = std::find_if(cookies.begin(), cookies.end(), [&](const CookieEntry &existing) {
        return equals_ci(existing.domain, entry.domain) && existing.path == entry.path &&
               existing.name == entry.name;
    });
    if (it == cookies.end()) {
        cookies.push_back(std::move(entry));
    } else {
        *it = std::move(entry);
    }
}

std::optional<CookieEntry> parse_set_cookie(std::string_view header,
                                            const boost::urls::url_view_base &request_url) {
    auto next_token = [](std::string_view &rest) -> std::string_view {
        const auto pos = rest.find(';');
        const auto token = pos == std::string_view::npos ? rest : rest.substr(0, pos);
        rest = pos == std::string_view::npos ? std::string_view{} : rest.substr(pos + 1);
        return token;
    };

    std::string_view rest = header;
    const auto first = trim(std::string(next_token(rest)));
    const auto eq = first.find('=');
    if (eq == std::string::npos || eq == 0) {
        return std::nullopt;
    }

    CookieEntry cookie;
    cookie.name = first.substr(0, eq);
    cookie.value = first.substr(eq + 1);
    cookie.domain = to_lower(request_url.host());
    cookie.path = "/";

    const std::string request_path =
        request_url.encoded_path().empty() ? "/" : std::string(request_url.encoded_path());
    if (!request_path.empty() && request_path.front() == '/') {
        const auto slash = request_path.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            cookie.path = request_path.substr(0, slash);
        }
    }

    while (!rest.empty()) {
        const auto raw_token = trim(std::string(next_token(rest)));
        if (raw_token.empty()) {
            continue;
        }
        const auto attr_eq = raw_token.find('=');
        const std::string attr_name = to_lower(raw_token.substr(0, attr_eq));
        const std::string attr_value =
            attr_eq == std::string::npos ? std::string() : raw_token.substr(attr_eq + 1);

        if (attr_name == "domain" && !attr_value.empty()) {
            cookie.domain = to_lower(attr_value);
        } else if (attr_name == "path" && !attr_value.empty()) {
            cookie.path = attr_value;
        } else if (attr_name == "secure") {
            cookie.secure = true;
        }
    }

    if (cookie.path.empty()) {
        cookie.path = "/";
    }
    if (cookie.domain.empty()) {
        cookie.domain = to_lower(request_url.host());
    }
    return cookie;
}

std::string build_cookie_header(const std::vector<CookieEntry> &cookies,
                                const boost::urls::url_view_base &request_url) {
    const std::string scheme = to_lower(request_url.scheme());
    const std::string host = to_lower(request_url.host());
    const std::string path =
        request_url.encoded_path().empty() ? "/" : std::string(request_url.encoded_path());
    const bool is_https = scheme == "https";

    std::string header;
    for (const auto &cookie : cookies) {
        if (cookie.secure && !is_https) {
            continue;
        }
        if (!cookie_domain_matches(host, cookie.domain)) {
            continue;
        }
        if (!cookie_path_matches(path, cookie.path)) {
            continue;
        }
        if (!header.empty()) {
            header += "; ";
        }
        header += cookie.name;
        header += '=';
        header += cookie.value;
    }
    return header;
}

bool should_switch_to_get_on_redirect(long status_code, std::string_view method) {
    return status_code == 303 ||
           ((status_code == 301 || status_code == 302) && equals_ci(method, "POST"));
}

struct BeastExecRequest {
    std::string method;
    std::string url;
    std::string body;
    std::vector<Header> headers;
    std::optional<ProxyOptions> proxy;
    CookieJarOptions cookie_jar;
    std::string user_agent;
    std::chrono::milliseconds timeout{0};
    std::chrono::milliseconds connect_timeout{0};
    bool follow_redirects = false;
    bool aggregate_response_body = true;
    std::function<void(std::string_view)> on_chunk;
    int max_redirects = 8;
};

Response execute_with_beast_blocking(BeastExecRequest request) {
    std::string current_url = std::move(request.url);
    std::string current_method = std::move(request.method);
    std::string current_body = std::move(request.body);
    std::vector<Header> current_headers = std::move(request.headers);
    std::vector<CookieEntry> cookies;
    if (request.cookie_jar.enabled && request.cookie_jar.file_path.has_value()) {
        cookies = load_cookie_jar_file(*request.cookie_jar.file_path);
    }

    for (int redirect_count = 0; redirect_count <= request.max_redirects; ++redirect_count) {
        const auto parsed = boost::urls::parse_uri(current_url);
        if (!parsed) {
            throw HttpError("invalid request url for beast backend", HttpErrorCode::invalid_request);
        }
        const bool use_tls = equals_ci(parsed->scheme(), "https");
        if (!equals_ci(parsed->scheme(), "http") && !use_tls) {
            throw HttpError("unsupported url scheme for beast backend",
                            HttpErrorCode::invalid_request);
        }

        const std::string host(parsed->host());
        const std::string port = parsed->has_port() ? std::string(parsed->port())
                                                    : make_default_port(parsed->scheme());
        std::string connect_host = host;
        std::string connect_port = port;
        std::string target = make_http_target(*parsed);

        if (request.proxy.has_value() && !request.proxy->url.empty()) {
            const auto proxy_url = boost::urls::parse_uri(request.proxy->url);
            if (!proxy_url || !equals_ci(proxy_url->scheme(), "http")) {
                throw HttpError("unsupported proxy url for beast backend",
                                HttpErrorCode::curl_failure,
                                0,
                                0,
                                TransportErrorKind::connect);
            }
            if (use_tls) {
                throw HttpError("https over proxy is not supported in beast backend yet",
                                HttpErrorCode::curl_failure,
                                0,
                                0,
                                TransportErrorKind::connect);
            }
            connect_host = std::string(proxy_url->host());
            connect_port =
                proxy_url->has_port() ? std::string(proxy_url->port()) : std::string("80");
            target = current_url;
        }

        const bool non_default_port = parsed->has_port() && parsed->port() != make_default_port(parsed->scheme());
        const std::string host_header = non_default_port ? (host + ":" + port) : host;

        boost::system::error_code ec;
        beast_net::io_context io_context;
        BeastTcp::resolver resolver(io_context);

        std::vector<Header> sending_headers = current_headers;
        if (request.cookie_jar.enabled) {
            erase_header(sending_headers, "Cookie");
            const auto cookie_header = build_cookie_header(cookies, *parsed);
            if (!cookie_header.empty()) {
                sending_headers.push_back(Header{"Cookie", cookie_header});
            }
        }

        const auto endpoints = resolver.resolve(connect_host, connect_port, ec);
        if (ec) {
            throw_beast_transport_error("dns resolve", ec);
        }

        beast_http::request<beast_http::string_body> req;
        req.version(11);
        req.method_string(current_method);
        req.target(target);

        for (const auto &header : sending_headers) {
            req.set(header.name, header.value);
        }
        if (!has_header(sending_headers, "Host")) {
            req.set(beast_http::field::host, host_header);
        }
        if (!has_header(sending_headers, "User-Agent") && !request.user_agent.empty()) {
            req.set(beast_http::field::user_agent, request.user_agent);
        }

        req.body() = current_body;
        req.prepare_payload();

        beast::flat_buffer buffer;
        beast_http::response<beast_http::string_body> res;

        if (use_tls) {
            beast_ssl::context ssl_ctx(beast_ssl::context::tls_client);
            bool verify_peer = true;
            try {
                ssl_ctx.set_default_verify_paths();
            } catch (...) {
                verify_peer = false;
            }

            beast::ssl_stream<beast::tcp_stream> stream(io_context, ssl_ctx);
            if (verify_peer) {
                stream.set_verify_mode(beast_ssl::verify_peer);
                stream.set_verify_callback(beast_ssl::host_name_verification(host));
            } else {
                stream.set_verify_mode(beast_ssl::verify_none);
            }

            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
                throw HttpError("tls sni setup failed",
                                HttpErrorCode::curl_failure,
                                0,
                                0,
                                TransportErrorKind::tls);
            }

            beast::get_lowest_layer(stream).expires_after(request.connect_timeout);
            beast::get_lowest_layer(stream).connect(endpoints, ec);
            if (ec) {
                throw_beast_transport_error("tcp connect", ec);
            }

            beast::get_lowest_layer(stream).expires_after(request.timeout);
            stream.handshake(beast_ssl::stream_base::client, ec);
            if (ec) {
                throw_beast_transport_error("tls handshake", ec);
            }

            beast_http::write(stream, req, ec);
            if (ec) {
                throw_beast_transport_error("http write", ec);
            }

            beast_http::read(stream, buffer, res, ec);
            if (ec) {
                throw_beast_transport_error("http read", ec);
            }

            stream.shutdown(ec);
            if (ec && ec != beast::errc::not_connected &&
                ec != beast_net::error::eof) {
                throw_beast_transport_error("tls shutdown", ec);
            }
        } else {
            beast::tcp_stream stream(io_context);
            stream.expires_after(request.connect_timeout);
            stream.connect(endpoints, ec);
            if (ec) {
                throw_beast_transport_error("tcp connect", ec);
            }

            stream.expires_after(request.timeout);
            beast_http::write(stream, req, ec);
            if (ec) {
                throw_beast_transport_error("http write", ec);
            }

            beast_http::read(stream, buffer, res, ec);
            if (ec) {
                throw_beast_transport_error("http read", ec);
            }

            boost::system::error_code ignored;
            stream.socket().shutdown(BeastTcp::socket::shutdown_both, ignored);
        }

        Response response;
        response.status_code = static_cast<long>(res.result_int());
        response.effective_url = current_url;
        for (const auto &field : res.base()) {
            response.headers.push_back(
                Header{std::string(field.name_string()), std::string(field.value())});
        }

        if (request.aggregate_response_body) {
            response.body = res.body();
        }
        if (request.on_chunk && !res.body().empty()) {
            request.on_chunk(res.body());
        }

        if (request.cookie_jar.enabled) {
            for (const auto &header : response.headers) {
                if (!equals_ci(header.name, "Set-Cookie")) {
                    continue;
                }
                auto parsed_cookie = parse_set_cookie(header.value, *parsed);
                if (parsed_cookie.has_value()) {
                    upsert_cookie(cookies, std::move(*parsed_cookie));
                }
            }
            if (request.cookie_jar.file_path.has_value()) {
                save_cookie_jar_file(*request.cookie_jar.file_path, cookies);
            }
        }

        if (!request.follow_redirects || !is_redirect_status(response.status_code)) {
            return response;
        }

        const auto location = find_header_value(response.headers, "Location");
        if (!location.has_value() || location->empty()) {
            return response;
        }
        if (redirect_count >= request.max_redirects) {
            throw HttpError("too many redirects", HttpErrorCode::curl_failure);
        }

        auto resolved = resolve_redirect_url(current_url, *location);
        if (!resolved.has_value()) {
            return response;
        }

        current_url = std::move(*resolved);
        if (should_switch_to_get_on_redirect(response.status_code, current_method)) {
            current_method = "GET";
            current_body.clear();
            erase_header(current_headers, "Content-Length");
            erase_header(current_headers, "Content-Type");
        }
    }

    throw HttpError("too many redirects", HttpErrorCode::curl_failure);
}

std::chrono::milliseconds retry_backoff(const RetryPolicy &policy, int attempt_index) {
    std::uint64_t factor = 1;
    for (int i = 1; i < attempt_index; ++i) {
        factor *= 2;
        if (factor > (1u << 20)) {
            break;
        }
    }

    auto value = policy.base_backoff * static_cast<int>(factor);
    if (value > policy.max_backoff) {
        value = policy.max_backoff;
    }
    return value;
}

std::map<std::string, std::string> parse_form_body(std::string_view text) {
    std::map<std::string, std::string> out;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto amp = text.find('&', begin);
        const auto token = text.substr(
            begin, amp == std::string_view::npos ? std::string_view::npos : amp - begin);
        if (!token.empty()) {
            const auto eq = token.find('=');
            if (eq == std::string_view::npos) {
                out.emplace(decode_component(token, true), "");
            } else {
                out.emplace(decode_component(token.substr(0, eq), true),
                            decode_component(token.substr(eq + 1), true));
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        begin = amp + 1;
    }
    return out;
}

std::optional<ProxyOptions> proxy_from_env_for_url(std::string_view url) {
    const bool is_https = url.rfind("https://", 0) == 0;
    const char *primary = std::getenv(is_https ? "HTTPS_PROXY" : "HTTP_PROXY");
    const char *fallback = std::getenv(is_https ? "https_proxy" : "http_proxy");
    const char *all_proxy = std::getenv("ALL_PROXY");
    const char *all_proxy_lower = std::getenv("all_proxy");

    const char *value = primary;
    if (value == nullptr || *value == '\0') {
        value = fallback;
    }
    if (value == nullptr || *value == '\0') {
        value = all_proxy;
    }
    if (value == nullptr || *value == '\0') {
        value = all_proxy_lower;
    }

    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    ProxyOptions options;
    options.url = value;
    return options;
}

void apply_request_interceptors(const Client::Config &config, Request &request) {
    for (const auto &interceptor : config.interceptors) {
        if (interceptor) {
            interceptor->on_request(request);
        }
    }
}

void apply_response_interceptors(const Client::Config &config,
                                 const Request &request,
                                 Response &response) {
    for (const auto &interceptor : config.interceptors) {
        if (interceptor) {
            interceptor->on_response(request, response);
        }
    }
}

void notify_error_interceptors(const Client::Config &config,
                               const Request &request,
                               const HttpError &error) {
    for (const auto &interceptor : config.interceptors) {
        if (interceptor) {
            interceptor->on_error(request, error);
        }
    }
}

std::string build_url_with_query(std::string_view base_url,
                                 const std::vector<std::pair<std::string, std::string>> &query_items) {
    if (query_items.empty()) {
        return std::string(base_url);
    }

    if (const auto parsed = boost::urls::parse_uri_reference(base_url); parsed) {
        boost::urls::url url(*parsed);
        auto params = url.params();
        for (const auto &[k, v] : query_items) {
            params.append(boost::urls::param_view{k, v});
        }
        return std::string(url.buffer());
    }

    std::string result(base_url);
    result.push_back(result.find('?') != std::string::npos ? '&' : '?');

    bool first = true;
    for (const auto &[k, v] : query_items) {
        if (!first) {
            result.push_back('&');
        }
        first = false;
        result += detail::percent_encode_component(k);
        result.push_back('=');
        result += detail::percent_encode_component(v);
    }
    return result;
}

Task<Response> execute_async(const Client::Config &config, Request request) {
    try {
        auto *current_runtime = co_await flux::get_current_runtime();
        if (current_runtime == nullptr) {
            throw HttpError("http client requires a current runtime",
                            HttpErrorCode::invalid_request);
        }
        if (config.runtime != nullptr && config.runtime != current_runtime) {
            throw HttpError("http client runtime mismatch", HttpErrorCode::invalid_request);
        }

        if (request.url.empty()) {
            throw HttpError("request url is empty", HttpErrorCode::invalid_request);
        }

        if (request.method.empty()) {
            request.method = "GET";
        }

        apply_request_interceptors(config, request);

        std::vector<Header> final_headers = config.default_headers;
        final_headers.insert(final_headers.end(), request.headers.begin(), request.headers.end());

        const auto timeout = request.timeout.value_or(config.timeout);
        const auto connect_timeout = request.connect_timeout.value_or(config.connect_timeout);
        const bool follow_redirects = request.follow_redirects.value_or(config.follow_redirects);
        const std::string method = to_upper(request.method);

        const RequestFormat effective_request_format = request.request_format.value_or(
            !request.multipart_parts.empty() ? RequestFormat::multipart : RequestFormat::text);

        if (!request.multipart_parts.empty() && !request.body.empty()) {
            throw HttpError("request body conflicts with multipart payload",
                            HttpErrorCode::invalid_request);
        }

        if (effective_request_format == RequestFormat::multipart &&
            request.multipart_parts.empty()) {
            throw HttpError("request_format is multipart but no multipart part is provided",
                            HttpErrorCode::invalid_request);
        }

        if (effective_request_format != RequestFormat::multipart &&
            !request.multipart_parts.empty()) {
            throw HttpError("multipart payload requires request_format multipart",
                            HttpErrorCode::invalid_request);
        }

        if (const auto content_type = content_type_for_request_format(effective_request_format);
            content_type.has_value()) {
            set_header_if_absent(final_headers, "Content-Type", *content_type);
        }

        if (!request.multipart_parts.empty() && method != "POST" && method != "PUT") {
            throw HttpError("multipart form-data requires POST or PUT",
                            HttpErrorCode::invalid_request);
        }

        std::string request_body = request.body;
        if (!request.multipart_parts.empty()) {
            const auto boundary = random_multipart_boundary();
            request_body = build_multipart_body(request.multipart_parts, boundary);
            set_or_replace_header(
                final_headers, "Content-Type", "multipart/form-data; boundary=" + boundary);
        }

        auto proxy_options = request.proxy ? request.proxy : config.proxy;
        const std::string effective_request_url =
            build_url_with_query(request.url, request.query_items);
        if (!proxy_options.has_value()) {
            proxy_options = proxy_from_env_for_url(effective_request_url);
        }

        BeastExecRequest beast_request;
        beast_request.method = method;
        beast_request.url = effective_request_url;
        beast_request.body = std::move(request_body);
        beast_request.headers = final_headers;
        beast_request.proxy = std::move(proxy_options);
        beast_request.cookie_jar = config.cookie_jar;
        beast_request.user_agent = request.user_agent.value_or(config.user_agent);
        beast_request.timeout = timeout;
        beast_request.connect_timeout = connect_timeout;
        beast_request.follow_redirects = follow_redirects;
        beast_request.aggregate_response_body =
            request.aggregate_response_body.value_or(config.aggregate_response_body);
        beast_request.on_chunk =
            request.on_response_chunk ? request.on_response_chunk : config.on_response_chunk;

        auto response = co_await current_runtime->spawn_blocking(
            [beast_request = std::move(beast_request)]() mutable {
                return execute_with_beast_blocking(std::move(beast_request));
            });

        apply_response_interceptors(config, request, response);

        if (config.throw_on_http_error && !response.ok()) {
            std::ostringstream oss;
            oss << "http status is not success: " << response.status_code;
            throw HttpError(oss.str(), HttpErrorCode::http_status_failure, 0, response.status_code);
        }

        co_return response;
    } catch (const HttpError &error) {
        notify_error_interceptors(config, request, error);
        throw;
    }
}

} // namespace

Client::RequestBuilder &Client::RequestBuilder::header(std::string name, std::string value) {
    request_.headers.push_back(Header{std::move(name), std::move(value)});
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::body(std::string value) {
    request_.body = std::move(value);
    request_.request_format = RequestFormat::text;
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::user_agent(std::string value) {
    request_.user_agent = std::move(value);
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::json(std::string value) {
    request_.body = std::move(value);
    request_.request_format = RequestFormat::json;
    set_header_if_absent(request_.headers, "Content-Type", "application/json");
    request_.response_format = ResponseFormat::json;
    set_header_if_absent(request_.headers, "Accept", "application/json");
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::xml(std::string value) {
    request_.body = std::move(value);
    request_.request_format = RequestFormat::xml;
    set_header_if_absent(request_.headers, "Content-Type", "application/xml");
    request_.response_format = ResponseFormat::xml;
    set_header_if_absent(request_.headers, "Accept", "application/xml");
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::form_data(std::string name, std::string value) {
    request_.multipart_parts.push_back(field(std::move(name), std::move(value)).part);
    request_.request_format = RequestFormat::multipart;
    request_.response_format = ResponseFormat::json;
    set_header_if_absent(request_.headers, "Accept", "application/json");
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::form_file(std::string name,
                                                          std::string file_path,
                                                          std::optional<std::string> file_name,
                                                          std::optional<std::string> content_type) {
    auto part = file(std::move(name), std::move(file_path));
    if (file_name.has_value()) {
        part.filename(*file_name);
    }
    if (content_type.has_value()) {
        part.content_type(*content_type);
    }
    request_.multipart_parts.push_back(std::move(part).operator MultipartItem().part);
    request_.request_format = RequestFormat::multipart;
    request_.response_format = ResponseFormat::json;
    set_header_if_absent(request_.headers, "Accept", "application/json");
    return *this;
}

Client::RequestBuilder &
Client::RequestBuilder::form_binary(std::string name,
                                    std::string data,
                                    std::optional<std::string> file_name,
                                    std::optional<std::string> content_type) {
    Request::MultipartPart part;
    part.name = std::move(name);
    part.value = std::move(data);
    part.file_name = std::move(file_name);
    part.content_type = std::move(content_type);
    if (!part.content_type.has_value()) {
        part.content_type = "application/octet-stream";
    }
    request_.multipart_parts.push_back(std::move(part));
    request_.request_format = RequestFormat::multipart;
    request_.response_format = ResponseFormat::json;
    set_header_if_absent(request_.headers, "Accept", "application/json");
    return *this;
}

Client::RequestBuilder &
Client::RequestBuilder::multipart(std::initializer_list<MultipartItem> items) {
    for (const auto &item : items) {
        request_.multipart_parts.push_back(item.part);
    }
    request_.request_format = RequestFormat::multipart;
    request_.response_format = ResponseFormat::json;
    set_header_if_absent(request_.headers, "Accept", "application/json");
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::form_urlencoded(std::string value) {
    request_.body = std::move(value);
    request_.request_format = RequestFormat::form;
    set_header_if_absent(request_.headers, "Content-Type", "application/x-www-form-urlencoded");
    request_.response_format = ResponseFormat::form;
    set_header_if_absent(request_.headers, "Accept", "application/x-www-form-urlencoded");
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::urlencoded(std::string value) {
    return form_urlencoded(std::move(value));
}

Client::RequestBuilder &Client::RequestBuilder::response_format(ResponseFormat format) {
    request_.response_format = format;
    set_header_if_absent(request_.headers, "Accept", accept_value_for_format(format));
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::response_format(std::string_view format) {
    return response_format(parse_response_format(format));
}

Client::RequestBuilder &Client::RequestBuilder::request_format(RequestFormat format) {
    request_.request_format = format;
    if (const auto content_type = content_type_for_request_format(format);
        content_type.has_value()) {
        set_header_if_absent(request_.headers, "Content-Type", *content_type);
    }
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::request_format(std::string_view format) {
    return request_format(parse_request_format(format));
}

Client::RequestBuilder &Client::RequestBuilder::retry(RetryPolicy value) {
    request_.retry = std::move(value);
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::proxy(ProxyOptions value) {
    request_.proxy = std::move(value);
    return *this;
}

Client::RequestBuilder &
Client::RequestBuilder::stream_response(std::function<void(std::string_view)> on_chunk,
                                        bool aggregate) {
    request_.on_response_chunk = std::move(on_chunk);
    request_.aggregate_response_body = aggregate;
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::timeout(std::chrono::milliseconds value) {
    request_.timeout = value;
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::connect_timeout(std::chrono::milliseconds value) {
    request_.connect_timeout = value;
    return *this;
}

Client::RequestBuilder &Client::RequestBuilder::follow_redirects(bool value) {
    request_.follow_redirects = value;
    return *this;
}

Task<std::map<std::string, std::string>> Client::RequestBuilder::SentResponse::form() {
    auto response = co_await raw();
    co_return parse_form_body(response.body);
}

Task<std::string> Client::RequestBuilder::SentResponse::xml() {
    auto response = co_await raw();
    co_return response.body;
}

Task<std::string> Client::RequestBuilder::SentResponse::text() {
    auto response = co_await raw();
    co_return response.body;
}

Client::RequestBuilder::SentResponse Client::RequestBuilder::send() {
    if (client_ == nullptr) {
        throw HttpError("request builder has no bound client", HttpErrorCode::invalid_request);
    }

    const ResponseFormat format = request_.response_format.value_or(ResponseFormat::json);
    if (!has_header(request_.headers, "Accept")) {
        set_header_if_absent(request_.headers, "Accept", accept_value_for_format(format));
    }

    return SentResponse(client_, client_->execute(std::move(request_)), format);
}

Client Client::Builder::build() {
    return Client(std::move(config_));
}

Task<Response> Client::execute(Request request) const {
    const RetryPolicy policy = request.retry.value_or(config_.retry);
    const std::string method =
        to_upper(request.method.empty() ? std::string("GET") : request.method);

    std::optional<HttpError> last_error;
    for (int attempt = 1; attempt <= std::max(1, policy.max_attempts); ++attempt) {
        bool retry = false;
        try {
            co_return co_await execute_async(config_, request);
        } catch (const HttpError &error) {
            last_error = error;
            retry = should_retry_error(policy, error, method, attempt + 1);
            if (!retry) {
                throw;
            }
        }
        co_await flux::sleep_for(retry_backoff(policy, attempt));
    }
    throw last_error.value_or(HttpError("request not executed", HttpErrorCode::invalid_request));
}

Task<DownloadResult> Client::download_to_file(std::string url,
                                              std::filesystem::path output,
                                              DownloadOptions options) const {
    auto *runtime = co_await flux::get_current_runtime();
    if (runtime == nullptr) {
        throw HttpError("download_to_file requires a current runtime",
                        HttpErrorCode::invalid_request);
    }
    if (config_.runtime != nullptr && config_.runtime != runtime) {
        throw HttpError("download_to_file runtime mismatch", HttpErrorCode::invalid_request);
    }

    if (url.empty() || output.empty()) {
        throw HttpError("download url or output path is empty", HttpErrorCode::invalid_request);
    }

    if (output.has_parent_path()) {
        co_await flux::Fs::create_directories(output.parent_path().string());
    }

    const std::filesystem::path temp_path = [&]() -> std::filesystem::path {
        if (!options.use_temp_file) {
            return output;
        }
        if (options.temp_path.has_value()) {
            return *options.temp_path;
        }

        const std::string base = output.parent_path().string();
        const std::string name = output.filename().string() + ".part";
        return std::filesystem::path(flux::path::join(base, name));
    }();

    std::uintmax_t offset = 0;
    if (options.resume && co_await flux::Fs::exists(temp_path.string())) {
        const auto info = co_await flux::Fs::stat(temp_path.string());
        offset = info.size;
    }

    auto run_once =
        [&](std::uintmax_t start_offset, bool append_mode, bool use_range) -> Task<Response> {
        std::string streamed;

        auto builder =
            get(url)
                .response_format(ResponseFormat::text)
                .stream_response(
                    [&](std::string_view chunk) {
                        streamed.append(chunk.data(), chunk.size());
                    },
                    false)
                .follow_redirects(true);

        if (use_range && start_offset > 0) {
            builder.header("Range", "bytes=" + std::to_string(start_offset) + "-");
        }

        auto response = co_await builder.send().raw();

        const auto flags =
            append_mode ? (flux::OpenFlags::create | flux::OpenFlags::write_only |
                           flux::OpenFlags::append)
                        : (flux::OpenFlags::create | flux::OpenFlags::write_only |
                           flux::OpenFlags::truncate);
        auto file = co_await flux::File::open(temp_path.string(), flags, 0644);
        if (!streamed.empty()) {
            (void)co_await file.write_all(streamed);
        }
        co_await file.close();

        co_return response;
    };

    bool resumed = false;
    auto response = co_await run_once(offset, offset > 0, offset > 0);

    if (offset > 0 && response.status_code == 200) {
        response = co_await run_once(0, false, false);
    } else if (offset > 0 && response.status_code == 206) {
        resumed = true;
    }

    if (options.use_temp_file) {
        if (co_await flux::Fs::exists(output.string())) {
            if (!options.overwrite) {
                throw HttpError("output file exists and overwrite is disabled",
                                HttpErrorCode::invalid_request);
            }
            co_await flux::Fs::remove(output.string());
        }

        co_await flux::Fs::rename(temp_path.string(), output.string());
    }

    DownloadResult result;
    result.output_path = output;
    result.resumed = resumed;
    result.status_code = response.status_code;
    if (co_await flux::Fs::exists(output.string())) {
        result.size = (co_await flux::Fs::stat(output.string())).size;
    }
    co_return result;
}

Client::RequestBuilder Client::get(std::string url) const {
    Request request;
    request.method = "GET";
    request.url = std::move(url);
    return RequestBuilder(this, std::move(request));
}

Client::RequestBuilder Client::post(std::string url) const {
    Request request;
    request.method = "POST";
    request.url = std::move(url);
    return RequestBuilder(this, std::move(request));
}

Client::RequestBuilder Client::put(std::string url) const {
    Request request;
    request.method = "PUT";
    request.url = std::move(url);
    return RequestBuilder(this, std::move(request));
}

Client::RequestBuilder Client::del(std::string url) const {
    Request request;
    request.method = "DELETE";
    request.url = std::move(url);
    return RequestBuilder(this, std::move(request));
}

} // namespace flux::http
