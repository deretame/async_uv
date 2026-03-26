#include "async_uv_http/http.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <curl/curl.h>

#include "async_uv/fd.h"
#include "async_uv/once_cell.h"

namespace async_uv::http {
namespace {

struct CurlInitState {
    CURLcode code = CURLE_OK;
};

void ensure_curl_global_init() {
    static async_uv::OnceCell<CurlInitState> cell;
    const auto &state = cell.get_or_init([]() {
        CurlInitState init;
        init.code = curl_global_init(CURL_GLOBAL_DEFAULT);
        return init;
    });

    if (state.code != CURLE_OK) {
        throw HttpError(
            "curl_global_init failed", HttpErrorCode::curl_failure, static_cast<int>(state.code));
    }
}

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

int from_hex(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '+') {
            out.push_back(' ');
            continue;
        }
        if (ch == '%' && i + 2 < text.size()) {
            const int hi = from_hex(text[i + 1]);
            const int lo = from_hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(ch);
    }
    return out;
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
                out.emplace(percent_decode(token), "");
            } else {
                out.emplace(percent_decode(token.substr(0, eq)),
                            percent_decode(token.substr(eq + 1)));
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        begin = amp + 1;
    }
    return out;
}

bool is_unreserved_char(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string percent_encode(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char ch : text) {
        if (is_unreserved_char(ch)) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0x0F]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string
append_query_items(std::string url,
                   const std::vector<std::pair<std::string, std::string>> &query_items) {
    if (query_items.empty()) {
        return url;
    }

    const bool has_query = url.find('?') != std::string::npos;
    url.push_back(has_query ? '&' : '?');

    bool first = true;
    for (const auto &[k, v] : query_items) {
        if (!first) {
            url.push_back('&');
        }
        first = false;
        url += percent_encode(k);
        url.push_back('=');
        url += percent_encode(v);
    }

    return url;
}

std::size_t
write_body_callback(char *buffer, std::size_t size, std::size_t nitems, void *userdata) {
    const std::size_t total = size * nitems;
    auto *body = static_cast<std::string *>(userdata);
    body->append(buffer, total);
    return total;
}

std::size_t
write_header_callback(char *buffer, std::size_t size, std::size_t nitems, void *userdata) {
    const std::size_t total = size * nitems;
    auto *headers = static_cast<std::vector<Header> *>(userdata);

    std::string line(buffer, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }

    if (line.empty()) {
        return total;
    }

    const auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        return total;
    }

    headers->push_back(Header{trim(line.substr(0, colon_pos)), trim(line.substr(colon_pos + 1))});
    return total;
}

struct SocketState {
    int what = CURL_POLL_NONE;
    std::optional<async_uv::FdWatcher> watcher;
};

struct MultiContext {
    std::unordered_map<curl_socket_t, SocketState> sockets;
    long timeout_ms = 50;
};

int socket_callback(CURL *, curl_socket_t socket, int what, void *userp, void *) {
    auto *context = static_cast<MultiContext *>(userp);
    if (what == CURL_POLL_REMOVE) {
        context->sockets.erase(socket);
        return 0;
    }

    auto &state = context->sockets[socket];
    if (state.what != what) {
        state.what = what;
        state.watcher.reset();
    }
    return 0;
}

int timer_callback(CURLM *, long timeout_ms, void *userp) {
    auto *context = static_cast<MultiContext *>(userp);
    context->timeout_ms = timeout_ms;
    return 0;
}

int to_uv_events(int what) {
    int events = 0;
    if ((what & CURL_POLL_IN) != 0) {
        events |= static_cast<int>(async_uv::FdEventFlags::readable);
    }
    if ((what & CURL_POLL_OUT) != 0) {
        events |= static_cast<int>(async_uv::FdEventFlags::writable);
    }
    if (events == 0) {
        events = static_cast<int>(async_uv::FdEventFlags::readable) |
                 static_cast<int>(async_uv::FdEventFlags::writable);
    }
    return events;
}

int to_curl_select_mask(const async_uv::FdEvent &event) {
    int mask = 0;
    if (event.readable()) {
        mask |= CURL_CSELECT_IN;
    }
    if (event.writable()) {
        mask |= CURL_CSELECT_OUT;
    }
    if (!event.ok()) {
        mask |= CURL_CSELECT_ERR;
    }
    return mask;
}

Task<void> drive_multi_until_done(CURLM *multi, MultiContext &context, int &running_handles) {
    while (running_handles > 0) {
        if (context.timeout_ms == 0) {
            const auto rc =
                curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
            if (rc != CURLM_OK) {
                throw HttpError("curl_multi_socket_action(timeout) failed",
                                HttpErrorCode::curl_failure,
                                static_cast<int>(rc));
            }
            continue;
        }

        curl_socket_t active_socket = CURL_SOCKET_BAD;
        SocketState *active_state = nullptr;
        for (auto &[socket, state] : context.sockets) {
            if (state.what == CURL_POLL_REMOVE) {
                continue;
            }
            active_socket = socket;
            active_state = &state;
            break;
        }

        const long timeout_ms = context.timeout_ms < 0 ? 50 : std::max<long>(1, context.timeout_ms);

        if (active_state == nullptr || active_socket == CURL_SOCKET_BAD) {
            co_await async_uv::sleep_for(std::chrono::milliseconds(timeout_ms));
            const auto rc =
                curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
            if (rc != CURLM_OK) {
                throw HttpError("curl_multi_socket_action(timeout) failed",
                                HttpErrorCode::curl_failure,
                                static_cast<int>(rc));
            }
            continue;
        }

        const int uv_events = to_uv_events(active_state->what);
        if (!active_state->watcher.has_value()) {
            active_state->watcher.emplace(co_await async_uv::FdWatcher::watch(
                static_cast<uv_os_sock_t>(active_socket), uv_events));
        }

        std::optional<async_uv::FdEvent> event;
        try {
            event = co_await active_state->watcher->next_for(std::chrono::milliseconds(timeout_ms));
        } catch (const async_uv::Error &error) {
            if (error.code() != UV_ETIMEDOUT) {
                throw;
            }
        }
        if (!event.has_value()) {
            const auto rc =
                curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
            if (rc != CURLM_OK) {
                throw HttpError("curl_multi_socket_action(timeout) failed",
                                HttpErrorCode::curl_failure,
                                static_cast<int>(rc));
            }
            continue;
        }

        const int mask = to_curl_select_mask(*event);
        const auto rc = curl_multi_socket_action(multi, active_socket, mask, &running_handles);
        if (rc != CURLM_OK) {
            throw HttpError("curl_multi_socket_action(socket) failed",
                            HttpErrorCode::curl_failure,
                            static_cast<int>(rc));
        }
    }
}

Task<Response> execute_async(const Client::Config &config, Request request) {
    ensure_curl_global_init();

    auto *current_runtime = async_uv::Runtime::current();
    if (current_runtime == nullptr) {
        throw HttpError("http client requires a current runtime", HttpErrorCode::invalid_request);
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

    if (effective_request_format == RequestFormat::multipart && request.multipart_parts.empty()) {
        throw HttpError("request_format is multipart but no multipart part is provided",
                        HttpErrorCode::invalid_request);
    }

    if (effective_request_format != RequestFormat::multipart && !request.multipart_parts.empty()) {
        throw HttpError("multipart payload requires request_format multipart",
                        HttpErrorCode::invalid_request);
    }

    if (const auto content_type = content_type_for_request_format(effective_request_format);
        content_type.has_value()) {
        set_header_if_absent(final_headers, "Content-Type", *content_type);
    }

    if (!request.multipart_parts.empty() && method != "POST" && method != "PUT") {
        throw HttpError("multipart form-data requires POST or PUT", HttpErrorCode::invalid_request);
    }

    Response response;

    CURL *raw_easy = curl_easy_init();
    if (raw_easy == nullptr) {
        throw HttpError("curl_easy_init failed",
                        HttpErrorCode::curl_failure,
                        static_cast<int>(CURLE_FAILED_INIT));
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(raw_easy, &curl_easy_cleanup);

    curl_mime *raw_mime = nullptr;
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(nullptr, &curl_mime_free);

    curl_slist *header_list = nullptr;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_guard(nullptr,
                                                                             &curl_slist_free_all);
    for (const auto &header : final_headers) {
        const std::string line = header.name + ": " + header.value;
        auto *new_list = curl_slist_append(header_list, line.c_str());
        if (new_list == nullptr) {
            throw HttpError("curl_slist_append failed",
                            HttpErrorCode::curl_failure,
                            static_cast<int>(CURLE_OUT_OF_MEMORY));
        }
        header_list = new_list;
    }
    header_guard.reset(header_list);

    const std::string effective_request_url = append_query_items(request.url, request.query_items);
    curl_easy_setopt(easy.get(), CURLOPT_URL, effective_request_url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(
        easy.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connect_timeout.count()));
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, &write_body_callback);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(easy.get(), CURLOPT_HEADERFUNCTION, &write_header_callback);
    curl_easy_setopt(easy.get(), CURLOPT_HEADERDATA, &response.headers);
    const std::string user_agent = request.user_agent.value_or(config.user_agent);
    if (!user_agent.empty()) {
        curl_easy_setopt(easy.get(), CURLOPT_USERAGENT, user_agent.c_str());
    }

    if (method != "GET") {
        curl_easy_setopt(easy.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    if (!request.multipart_parts.empty()) {
        raw_mime = curl_mime_init(easy.get());
        if (raw_mime == nullptr) {
            throw HttpError("curl_mime_init failed",
                            HttpErrorCode::curl_failure,
                            static_cast<int>(CURLE_OUT_OF_MEMORY));
        }
        mime.reset(raw_mime);

        for (const auto &part : request.multipart_parts) {
            curl_mimepart *mime_part = curl_mime_addpart(mime.get());
            if (mime_part == nullptr) {
                throw HttpError("curl_mime_addpart failed",
                                HttpErrorCode::curl_failure,
                                static_cast<int>(CURLE_OUT_OF_MEMORY));
            }

            if (curl_mime_name(mime_part, part.name.c_str()) != CURLE_OK) {
                throw HttpError("curl_mime_name failed", HttpErrorCode::curl_failure);
            }

            if (part.file_path.has_value()) {
                const auto rc = curl_mime_filedata(mime_part, part.file_path->c_str());
                if (rc != CURLE_OK) {
                    throw HttpError("curl_mime_filedata failed",
                                    HttpErrorCode::curl_failure,
                                    static_cast<int>(rc));
                }

                if (part.file_name.has_value()) {
                    const auto name_rc = curl_mime_filename(mime_part, part.file_name->c_str());
                    if (name_rc != CURLE_OK) {
                        throw HttpError("curl_mime_filename failed",
                                        HttpErrorCode::curl_failure,
                                        static_cast<int>(name_rc));
                    }
                }
            } else {
                const auto rc = curl_mime_data(
                    mime_part, part.value.c_str(), static_cast<size_t>(part.value.size()));
                if (rc != CURLE_OK) {
                    throw HttpError(
                        "curl_mime_data failed", HttpErrorCode::curl_failure, static_cast<int>(rc));
                }

                if (part.file_name.has_value()) {
                    const auto name_rc = curl_mime_filename(mime_part, part.file_name->c_str());
                    if (name_rc != CURLE_OK) {
                        throw HttpError("curl_mime_filename failed",
                                        HttpErrorCode::curl_failure,
                                        static_cast<int>(name_rc));
                    }
                }
            }

            if (part.content_type.has_value()) {
                const auto type_rc = curl_mime_type(mime_part, part.content_type->c_str());
                if (type_rc != CURLE_OK) {
                    throw HttpError("curl_mime_type failed",
                                    HttpErrorCode::curl_failure,
                                    static_cast<int>(type_rc));
                }
            }
        }

        curl_easy_setopt(easy.get(), CURLOPT_MIMEPOST, mime.get());
    } else if (!request.body.empty()) {
        curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(
            easy.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }

    if (header_list != nullptr) {
        curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, header_list);
    }

    CURLM *raw_multi = curl_multi_init();
    if (raw_multi == nullptr) {
        throw HttpError("curl_multi_init failed",
                        HttpErrorCode::curl_failure,
                        static_cast<int>(CURLM_INTERNAL_ERROR));
    }
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(raw_multi, &curl_multi_cleanup);

    MultiContext context;
    curl_multi_setopt(multi.get(), CURLMOPT_SOCKETFUNCTION, &socket_callback);
    curl_multi_setopt(multi.get(), CURLMOPT_SOCKETDATA, &context);
    curl_multi_setopt(multi.get(), CURLMOPT_TIMERFUNCTION, &timer_callback);
    curl_multi_setopt(multi.get(), CURLMOPT_TIMERDATA, &context);

    const auto add_rc = curl_multi_add_handle(multi.get(), easy.get());
    if (add_rc != CURLM_OK) {
        throw HttpError(
            "curl_multi_add_handle failed", HttpErrorCode::curl_failure, static_cast<int>(add_rc));
    }

    int running_handles = 0;
    const auto start_rc =
        curl_multi_socket_action(multi.get(), CURL_SOCKET_TIMEOUT, 0, &running_handles);
    if (start_rc != CURLM_OK) {
        curl_multi_remove_handle(multi.get(), easy.get());
        throw HttpError("curl_multi_socket_action(start) failed",
                        HttpErrorCode::curl_failure,
                        static_cast<int>(start_rc));
    }

    co_await drive_multi_until_done(multi.get(), context, running_handles);

    CURLMsg *message = nullptr;
    int remaining = 0;
    CURLcode done_code = CURLE_OK;
    while ((message = curl_multi_info_read(multi.get(), &remaining)) != nullptr) {
        if (message->msg == CURLMSG_DONE && message->easy_handle == easy.get()) {
            done_code = message->data.result;
            break;
        }
    }

    curl_multi_remove_handle(multi.get(), easy.get());

    if (done_code != CURLE_OK) {
        std::ostringstream oss;
        oss << "curl request failed: " << curl_easy_strerror(done_code);
        throw HttpError(oss.str(), HttpErrorCode::curl_failure, static_cast<int>(done_code));
    }

    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &response.status_code);
    char *effective_url = nullptr;
    curl_easy_getinfo(easy.get(), CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url != nullptr) {
        response.effective_url = effective_url;
    }

    if (config.throw_on_http_error && !response.ok()) {
        std::ostringstream oss;
        oss << "http status is not success: " << response.status_code;
        throw HttpError(oss.str(), HttpErrorCode::http_status_failure, 0, response.status_code);
    }

    co_return response;
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
    co_return co_await execute_async(config_, std::move(request));
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

} // namespace async_uv::http
