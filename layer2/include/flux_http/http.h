#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <boost/url/encode.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>

#include "flux/runtime.h"
#include "flux/task.h"

namespace flux::http {

namespace detail {

template <typename T>
concept HasToStringMember = requires(const T &value) {
    { value.to_string() } -> std::convertible_to<std::string>;
};

template <typename T>
concept HasToStringCamelMember = requires(const T &value) {
    { value.toString() } -> std::convertible_to<std::string>;
};

template <typename T>
concept HasStdToString = requires(const T &value) {
    { std::to_string(value) } -> std::convertible_to<std::string>;
};

inline std::string percent_encode_component(std::string_view text) {
    return boost::urls::encode(text, boost::urls::unreserved_chars);
}

template <typename T>
std::string stringify(const T &value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, std::string>) {
        return value;
    } else if constexpr (std::convertible_to<U, std::string_view>) {
        return std::string(std::string_view(value));
    } else if constexpr (HasToStringMember<U>) {
        return std::string(value.to_string());
    } else if constexpr (HasToStringCamelMember<U>) {
        return std::string(value.toString());
    } else if constexpr (HasStdToString<U>) {
        return std::to_string(value);
    } else {
        static_assert(sizeof(U) == 0,
                      "query value must be string-like, support std::to_string, or provide "
                      "to_string/toString member");
    }
}

template <typename T>
concept QueryStringifiable = requires(const T &value) {
    { stringify(value) } -> std::same_as<std::string>;
};

template <typename T>
concept QueryContainer = requires(const T &container) {
    container.begin();
    container.end();
};

template <typename Entry>
concept HasFirstSecond = requires(const Entry &entry) {
    entry.first;
    entry.second;
};

template <typename Entry>
concept HasTupleGet = requires(const Entry &entry) {
    {
        []<std::size_t I>(const Entry & e) -> decltype(auto) {
            using std::get;
            return get<I>(e);
        }.template operator()<0>(entry)
    };
    {
        []<std::size_t I>(const Entry & e) -> decltype(auto) {
            using std::get;
            return get<I>(e);
        }.template operator()<1>(entry)
    };
};

template <typename Entry>
concept QueryEntry = HasFirstSecond<Entry> || HasTupleGet<Entry>;

template <typename Entry>
decltype(auto) entry_key(const Entry &entry) {
    if constexpr (HasFirstSecond<Entry>) {
        return (entry.first);
    } else {
        using std::get;
        return get<0>(entry);
    }
}

template <typename Entry>
decltype(auto) entry_value(const Entry &entry) {
    if constexpr (HasFirstSecond<Entry>) {
        return (entry.second);
    } else {
        using std::get;
        return get<1>(entry);
    }
}

} // namespace detail

struct Header {
    std::string name;
    std::string value;
};

enum class ResponseFormat {
    json,
    form,
    xml,
    text,
};

enum class RequestFormat {
    json,
    form,
    xml,
    text,
    binary,
    multipart,
};

struct RetryPolicy {
    bool enabled = true;
    int max_attempts = 1;
    bool retry_idempotent_only = true;
    std::chrono::milliseconds base_backoff = std::chrono::milliseconds(100);
    std::chrono::milliseconds max_backoff = std::chrono::seconds(2);
    bool retry_on_http_5xx = true;
};

struct ProxyOptions {
    std::string url;
    std::optional<std::string> username;
    std::optional<std::string> password;
};

struct CookieJarOptions {
    bool enabled = false;
    std::optional<std::string> file_path;
};

struct DownloadOptions {
    bool resume = true;
    bool use_temp_file = true;
    bool overwrite = true;
    std::optional<std::filesystem::path> temp_path;
};

struct DownloadResult {
    std::filesystem::path output_path;
    std::uintmax_t size = 0;
    bool resumed = false;
    long status_code = 0;
};

struct Request {
    struct MultipartPart {
        std::string name;
        std::string value;
        std::optional<std::string> file_path;
        std::optional<std::string> file_name;
        std::optional<std::string> content_type;
    };

    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> query_items;
    std::vector<Header> headers;
    std::string body;
    std::vector<MultipartPart> multipart_parts;
    std::optional<std::string> user_agent;
    std::optional<RequestFormat> request_format;
    std::optional<ResponseFormat> response_format;
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<bool> follow_redirects;
    std::optional<RetryPolicy> retry;
    std::optional<ProxyOptions> proxy;
    std::optional<bool> aggregate_response_body;
    std::function<void(std::string_view)> on_response_chunk;
};

struct MultipartItem {
    Request::MultipartPart part;
};

class MultipartFileBuilder {
public:
    MultipartFileBuilder(std::string name, std::string file_path) {
        part_.name = std::move(name);
        part_.file_path = std::move(file_path);
    }

    MultipartFileBuilder &filename(std::string value) {
        part_.file_name = std::move(value);
        return *this;
    }

    MultipartFileBuilder &content_type(std::string value) {
        part_.content_type = std::move(value);
        return *this;
    }

    operator MultipartItem() const & {
        return MultipartItem{part_};
    }

    operator MultipartItem() && {
        return MultipartItem{std::move(part_)};
    }

private:
    Request::MultipartPart part_;
};

template <detail::QueryStringifiable K, detail::QueryStringifiable V>
MultipartItem field(K &&name, V &&value) {
    Request::MultipartPart part;
    part.name = detail::stringify(std::forward<K>(name));
    part.value = detail::stringify(std::forward<V>(value));
    return MultipartItem{std::move(part)};
}

template <detail::QueryStringifiable K, detail::QueryStringifiable P>
MultipartFileBuilder file(K &&name, P &&path) {
    return MultipartFileBuilder(detail::stringify(std::forward<K>(name)),
                                detail::stringify(std::forward<P>(path)));
}

struct Response {
    long status_code = 0;
    std::string effective_url;
    std::vector<Header> headers;
    std::string body;

    bool ok() const noexcept {
        return status_code >= 200 && status_code < 300;
    }
};

enum class HttpErrorCode {
    invalid_request,
    curl_failure,
    http_status_failure,
};

enum class TransportErrorKind {
    none,
    timeout,
    dns,
    tls,
    connect,
    send,
    recv,
    reset,
    unknown,
};

class HttpError : public std::runtime_error {
public:
    HttpError(std::string message,
              HttpErrorCode code,
              int transport_code = 0,
              long status_code = 0,
              TransportErrorKind transport_kind = TransportErrorKind::none)
        : std::runtime_error(std::move(message)), code_(code), transport_code_(transport_code),
          status_code_(status_code), transport_kind_(transport_kind) {}

    HttpErrorCode code() const noexcept {
        return code_;
    }

    int transport_code() const noexcept {
        return transport_code_;
    }

    long status_code() const noexcept {
        return status_code_;
    }

    TransportErrorKind transport_kind() const noexcept {
        return transport_kind_;
    }

private:
    HttpErrorCode code_ = HttpErrorCode::invalid_request;
    int transport_code_ = 0;
    long status_code_ = 0;
    TransportErrorKind transport_kind_ = TransportErrorKind::none;
};

class Interceptor {
public:
    virtual ~Interceptor() = default;

    virtual void on_request(Request &request) const {
        (void)request;
    }

    virtual void on_response(const Request &request, Response &response) const {
        (void)request;
        (void)response;
    }

    virtual void on_error(const Request &request, const HttpError &error) const {
        (void)request;
        (void)error;
    }
};

class JsonCodec {
public:
    virtual ~JsonCodec() = default;

    virtual std::string serialize(const void *value, const std::type_info &type) const = 0;
    virtual void
    deserialize(std::string_view text, void *output, const std::type_info &type) const = 0;
};

class XmlCodec {
public:
    virtual ~XmlCodec() = default;

    virtual std::string serialize(const void *value, const std::type_info &type) const = 0;
    virtual void
    deserialize(std::string_view text, void *output, const std::type_info &type) const = 0;
};

class Client {
public:
    struct Config {
        Runtime *runtime = nullptr;
        std::vector<Header> default_headers;
        std::string user_agent = "flux_http/0.4";
        std::chrono::milliseconds timeout = std::chrono::seconds(30);
        std::chrono::milliseconds connect_timeout = std::chrono::seconds(10);
        bool follow_redirects = true;
        bool throw_on_http_error = true;
        std::shared_ptr<const JsonCodec> json_codec;
        std::shared_ptr<const XmlCodec> xml_codec;
        std::vector<std::shared_ptr<const Interceptor>> interceptors;
        RetryPolicy retry;
        std::optional<ProxyOptions> proxy;
        CookieJarOptions cookie_jar;
        bool aggregate_response_body = true;
        std::function<void(std::string_view)> on_response_chunk;
    };

    class RequestBuilder {
    public:
        RequestBuilder &header(std::string name, std::string value);
        RequestBuilder &user_agent(std::string value);
        RequestBuilder &body(std::string value);

        template <detail::QueryStringifiable K, detail::QueryStringifiable V>
        RequestBuilder &query(K &&key, V &&value) {
            request_.query_items.emplace_back(detail::stringify(std::forward<K>(key)),
                                              detail::stringify(std::forward<V>(value)));
            return *this;
        }

        template <detail::QueryContainer Container>
            requires(
                detail::QueryEntry<
                    std::remove_cvref_t<decltype(*std::declval<const Container &>().begin())>> &&
                detail::QueryStringifiable<std::remove_cvref_t<
                    decltype(detail::entry_key(*std::declval<const Container &>().begin()))>> &&
                detail::QueryStringifiable<std::remove_cvref_t<
                    decltype(detail::entry_value(*std::declval<const Container &>().begin()))>>)
        RequestBuilder &query(const Container &container) {
            for (const auto &entry : container) {
                query(detail::entry_key(entry), detail::entry_value(entry));
            }
            return *this;
        }

        template <detail::QueryStringifiable K, detail::QueryStringifiable V>
        RequestBuilder &query(std::initializer_list<std::pair<K, V>> items) {
            for (const auto &[k, v] : items) {
                query(k, v);
            }
            return *this;
        }

        RequestBuilder &query(std::initializer_list<std::pair<const char *, const char *>> items) {
            for (const auto &[k, v] : items) {
                query(k, v);
            }
            return *this;
        }

        template <typename T>
        RequestBuilder &json(const T &value) {
            if (client_ == nullptr) {
                throw HttpError("request builder has no bound client",
                                HttpErrorCode::invalid_request);
            }
            return json(client_->serialize_json(value));
        }

        RequestBuilder &json(std::string value);
        RequestBuilder &xml(std::string value);

        template <typename T>
        RequestBuilder &xml(const T &value) {
            if (client_ == nullptr) {
                throw HttpError("request builder has no bound client",
                                HttpErrorCode::invalid_request);
            }
            return xml(client_->serialize_xml(value));
        }
        // 兼容直观写法：单个表单字段。
        // Compatibility helper: add one multipart text field.
        RequestBuilder &form_data(std::string name, std::string value);

        template <detail::QueryStringifiable K, detail::QueryStringifiable V>
        RequestBuilder &form_data(K &&name, V &&value) {
            return form_data(detail::stringify(std::forward<K>(name)),
                             detail::stringify(std::forward<V>(value)));
        }

        // 兼容直观写法：单个文件字段。
        // Compatibility helper: add one multipart file field.
        RequestBuilder &form_file(std::string name,
                                  std::string file_path,
                                  std::optional<std::string> file_name = std::nullopt,
                                  std::optional<std::string> content_type = std::nullopt);

        RequestBuilder &form_binary(std::string name,
                                    std::string data,
                                    std::optional<std::string> file_name = std::nullopt,
                                    std::optional<std::string> content_type = std::nullopt);
        RequestBuilder &multipart(std::initializer_list<MultipartItem> items);

        template <detail::QueryContainer Container>
            requires(
                detail::QueryEntry<
                    std::remove_cvref_t<decltype(*std::declval<const Container &>().begin())>> &&
                detail::QueryStringifiable<std::remove_cvref_t<
                    decltype(detail::entry_key(*std::declval<const Container &>().begin()))>> &&
                detail::QueryStringifiable<std::remove_cvref_t<
                    decltype(detail::entry_value(*std::declval<const Container &>().begin()))>>)
        RequestBuilder &form(const Container &container) {
            return form_map_like(container);
        }

        RequestBuilder &form_urlencoded(std::string value);
        RequestBuilder &urlencoded(std::string value);

        template <detail::QueryContainer Container>
            requires(
                detail::QueryEntry<
                    std::remove_cvref_t<decltype(*std::declval<const Container &>().begin())>> &&
                detail::QueryStringifiable<std::remove_cvref_t<
                    decltype(detail::entry_key(*std::declval<const Container &>().begin()))>> &&
                detail::QueryStringifiable<std::remove_cvref_t<
                    decltype(detail::entry_value(*std::declval<const Container &>().begin()))>>)
        RequestBuilder &urlencoded(const Container &container) {
            return form(container);
        }
        RequestBuilder &response_format(ResponseFormat format);
        RequestBuilder &response_format(std::string_view format);
        RequestBuilder &request_format(RequestFormat format);
        RequestBuilder &request_format(std::string_view format);
        RequestBuilder &retry(RetryPolicy value);
        RequestBuilder &proxy(ProxyOptions value);
        RequestBuilder &stream_response(std::function<void(std::string_view)> on_chunk,
                                        bool aggregate = false);
        RequestBuilder &timeout(std::chrono::milliseconds value);
        RequestBuilder &connect_timeout(std::chrono::milliseconds value);
        RequestBuilder &follow_redirects(bool value);

        class SentResponse {
        public:
            SentResponse(const Client *client, Task<Response> response_task, ResponseFormat format)
                : client_(client), response_task_(std::move(response_task)), format_(format) {}

            Task<Response> raw() {
                co_return co_await std::move(response_task_);
            }

            template <typename T>
            Task<T> json() {
                if (client_ == nullptr) {
                    throw HttpError("request builder has no bound client",
                                    HttpErrorCode::invalid_request);
                }
                auto response = co_await raw();
                co_return client_->parse_json<T>(response);
            }

            template <typename T>
            Task<T> xml() {
                if (client_ == nullptr) {
                    throw HttpError("request builder has no bound client",
                                    HttpErrorCode::invalid_request);
                }
                auto response = co_await raw();
                co_return client_->parse_xml<T>(response.body);
            }

            Task<std::map<std::string, std::string>> form();
            Task<std::string> xml();
            Task<std::string> text();

            ResponseFormat format() const noexcept {
                return format_;
            }

        private:
            const Client *client_ = nullptr;
            Task<Response> response_task_;
            ResponseFormat format_ = ResponseFormat::json;
        };

        SentResponse send();

    private:
        friend class Client;
        RequestBuilder(const Client *client, Request request)
            : client_(client), request_(std::move(request)) {}

        template <typename MapLike>
        RequestBuilder &form_map_like(const MapLike &map);

        const Client *client_ = nullptr;
        Request request_;
    };

    class Builder {
    public:
        Builder &runtime(Runtime &runtime) {
            config_.runtime = &runtime;
            return *this;
        }

        Builder &default_header(std::string name, std::string value) {
            config_.default_headers.push_back(Header{std::move(name), std::move(value)});
            return *this;
        }

        Builder &user_agent(std::string value) {
            config_.user_agent = std::move(value);
            return *this;
        }

        Builder &timeout(std::chrono::milliseconds value) {
            config_.timeout = value;
            return *this;
        }

        Builder &connect_timeout(std::chrono::milliseconds value) {
            config_.connect_timeout = value;
            return *this;
        }

        Builder &follow_redirects(bool value) {
            config_.follow_redirects = value;
            return *this;
        }

        Builder &throw_on_http_error(bool value) {
            config_.throw_on_http_error = value;
            return *this;
        }

        Builder &json_codec(std::shared_ptr<const JsonCodec> codec) {
            config_.json_codec = std::move(codec);
            return *this;
        }

        Builder &xml_codec(std::shared_ptr<const XmlCodec> codec) {
            config_.xml_codec = std::move(codec);
            return *this;
        }

        Builder &interceptor(std::shared_ptr<const Interceptor> value) {
            config_.interceptors.push_back(std::move(value));
            return *this;
        }

        Builder &interceptors(std::vector<std::shared_ptr<const Interceptor>> values) {
            config_.interceptors = std::move(values);
            return *this;
        }

        Builder &retry(RetryPolicy value) {
            config_.retry = std::move(value);
            return *this;
        }

        Builder &proxy(ProxyOptions value) {
            config_.proxy = std::move(value);
            return *this;
        }

        Builder &cookie_jar(CookieJarOptions value) {
            config_.cookie_jar = std::move(value);
            return *this;
        }

        Builder &aggregate_response_body(bool value) {
            config_.aggregate_response_body = value;
            return *this;
        }

        Builder &on_response_chunk(std::function<void(std::string_view)> value) {
            config_.on_response_chunk = std::move(value);
            return *this;
        }

        Client build();

    private:
        Config config_;
    };

    static Builder build() {
        return Builder{};
    }

    Task<Response> execute(Request request) const;
    Task<DownloadResult> download_to_file(std::string url,
                                          std::filesystem::path output,
                                          DownloadOptions options = {}) const;

    template <typename T>
    T parse_json(std::string_view text) const {
        if (!config_.json_codec) {
            throw HttpError("json codec is not configured", HttpErrorCode::invalid_request);
        }
        T value{};
        config_.json_codec->deserialize(text, &value, typeid(T));
        return value;
    }

    template <typename T>
    T parse_json(const Response &response) const {
        return parse_json<T>(response.body);
    }

    template <typename T>
    T parse_xml(std::string_view text) const {
        if (!config_.xml_codec) {
            throw HttpError("xml codec is not configured", HttpErrorCode::invalid_request);
        }
        T value{};
        config_.xml_codec->deserialize(text, &value, typeid(T));
        return value;
    }

    template <typename T>
    T parse_xml(const Response &response) const {
        return parse_xml<T>(response.body);
    }

    RequestBuilder get(std::string url) const;
    RequestBuilder post(std::string url) const;
    RequestBuilder put(std::string url) const;
    RequestBuilder del(std::string url) const;

private:
    explicit Client(Config config) : config_(std::move(config)) {}

    template <typename T>
    std::string serialize_json(const T &value) const {
        if (!config_.json_codec) {
            throw HttpError("json codec is not configured", HttpErrorCode::invalid_request);
        }
        return config_.json_codec->serialize(&value, typeid(T));
    }

    template <typename T>
    std::string serialize_xml(const T &value) const {
        if (!config_.xml_codec) {
            throw HttpError("xml codec is not configured", HttpErrorCode::invalid_request);
        }
        return config_.xml_codec->serialize(&value, typeid(T));
    }

    Config config_;
};

template <typename MapLike>
Client::RequestBuilder &Client::RequestBuilder::form_map_like(const MapLike &map) {
    std::string text;
    bool first = true;
    for (const auto &item : map) {
        if (!first) {
            text += "&";
        }
        first = false;
        text += detail::percent_encode_component(detail::stringify(detail::entry_key(item)));
        text += "=";
        text += detail::percent_encode_component(detail::stringify(detail::entry_value(item)));
    }
    return form_urlencoded(std::move(text));
}

} // namespace flux::http
