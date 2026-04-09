#include <cassert>
#include <atomic>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <tuple>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <tinyxml2.h>

#include "flux/flux.h"
#include "flux_http/http.h"

namespace {

struct DemoPayload {
    std::string hello;
    std::string lang;
};

struct HttpBinPostJsonEcho {
    DemoPayload json;
};

struct XmlRootInfo {
    std::string root_name;
};

struct QueryToStringValue {
    int id = 0;
    std::string to_string() const {
        return std::string("id-") + std::to_string(id);
    }
};

struct QueryToStringCamelValue {
    std::string mode;
    std::string toString() const {
        return mode;
    }
};

class CountingInterceptor : public flux::http::Interceptor {
public:
    void on_request(flux::http::Request &request) const override {
        request_count.fetch_add(1, std::memory_order_relaxed);
        request.headers.push_back({"X-Interceptor", "on"});
    }

    void on_response(const flux::http::Request &,
                     flux::http::Response &response) const override {
        response_count.fetch_add(1, std::memory_order_relaxed);
        response.headers.push_back({"X-Interceptor-Response", "on"});
    }

    void on_error(const flux::http::Request &,
                  const flux::http::HttpError &) const override {
        error_count.fetch_add(1, std::memory_order_relaxed);
    }

    mutable std::atomic_int request_count{0};
    mutable std::atomic_int response_count{0};
    mutable std::atomic_int error_count{0};
};

bool has_header(const flux::http::Response &response, const std::string &name) {
    for (const auto &header : response.headers) {
        if (header.name == name) {
            return true;
        }
    }
    return false;
}

struct QueryTupleEntry {
    std::string key;
    std::string value;
};

template <std::size_t I>
decltype(auto) get(const QueryTupleEntry &entry) {
    static_assert(I < 2);
    if constexpr (I == 0) {
        return (entry.key);
    } else {
        return (entry.value);
    }
}

template <std::size_t I>
decltype(auto) get(QueryTupleEntry &entry) {
    static_assert(I < 2);
    if constexpr (I == 0) {
        return (entry.key);
    } else {
        return (entry.value);
    }
}

} // namespace

namespace std {

template <>
struct tuple_size<QueryTupleEntry> : std::integral_constant<std::size_t, 2> {};

template <>
struct tuple_element<0, QueryTupleEntry> {
    using type = std::string;
};

template <>
struct tuple_element<1, QueryTupleEntry> {
    using type = std::string;
};

} // namespace std

namespace {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DemoPayload, hello, lang)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(HttpBinPostJsonEcho, json)

class SimpleMapJsonCodec : public flux::http::JsonCodec {
public:
    template <typename T>
    void register_type() {
        serializers_[std::type_index(typeid(T))] = [](const void *value) {
            return nlohmann::json(*static_cast<const T *>(value)).dump();
        };

        deserializers_[std::type_index(typeid(T))] = [](std::string_view text, void *output) {
            *static_cast<T *>(output) = nlohmann::json::parse(text).template get<T>();
        };
    }

    std::string serialize(const void *value, const std::type_info &type) const override {
        const auto it = serializers_.find(std::type_index(type));
        if (it == serializers_.end()) {
            throw std::runtime_error("unsupported json serialize type");
        }
        return it->second(value);
    }

    void
    deserialize(std::string_view text, void *output, const std::type_info &type) const override {
        const auto it = deserializers_.find(std::type_index(type));
        if (it == deserializers_.end()) {
            throw std::runtime_error("unsupported json deserialize type");
        }
        it->second(text, output);
    }

private:
    using SerializeFn = std::function<std::string(const void *)>;
    using DeserializeFn = std::function<void(std::string_view, void *)>;

    std::unordered_map<std::type_index, SerializeFn> serializers_;
    std::unordered_map<std::type_index, DeserializeFn> deserializers_;
};

class SimpleXmlCodec : public flux::http::XmlCodec {
public:
    std::string serialize(const void *value, const std::type_info &type) const override {
        if (type != typeid(DemoPayload)) {
            throw std::runtime_error("unsupported xml serialize type");
        }

        const auto &payload = *static_cast<const DemoPayload *>(value);
        tinyxml2::XMLDocument doc;
        auto *root = doc.NewElement("payload");
        doc.InsertEndChild(root);

        auto *hello = doc.NewElement("hello");
        hello->SetText(payload.hello.c_str());
        root->InsertEndChild(hello);

        auto *lang = doc.NewElement("lang");
        lang->SetText(payload.lang.c_str());
        root->InsertEndChild(lang);

        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        return printer.CStr();
    }

    void
    deserialize(std::string_view text, void *output, const std::type_info &type) const override {
        tinyxml2::XMLDocument doc;
        if (doc.Parse(text.data(), text.size()) != tinyxml2::XML_SUCCESS) {
            throw std::runtime_error("xml parse failed");
        }

        auto *root = doc.RootElement();
        if (root == nullptr) {
            throw std::runtime_error("xml root missing");
        }

        if (type == typeid(XmlRootInfo)) {
            auto &result = *static_cast<XmlRootInfo *>(output);
            result.root_name = root->Name() == nullptr ? std::string() : std::string(root->Name());
            return;
        }

        if (type == typeid(DemoPayload)) {
            auto &result = *static_cast<DemoPayload *>(output);
            auto *hello = root->FirstChildElement("hello");
            auto *lang = root->FirstChildElement("lang");
            result.hello =
                (hello != nullptr && hello->GetText() != nullptr) ? hello->GetText() : "";
            result.lang = (lang != nullptr && lang->GetText() != nullptr) ? lang->GetText() : "";
            return;
        }

        throw std::runtime_error("unsupported xml deserialize type");
    }
};

bool is_network_error(const flux::http::HttpError &error) {
    if (error.code() != flux::http::HttpErrorCode::curl_failure) {
        return false;
    }
    const auto kind = error.transport_kind();
    return kind == flux::http::TransportErrorKind::timeout ||
           kind == flux::http::TransportErrorKind::dns ||
           kind == flux::http::TransportErrorKind::connect ||
           kind == flux::http::TransportErrorKind::tls ||
           kind == flux::http::TransportErrorKind::recv ||
           kind == flux::http::TransportErrorKind::send ||
           kind == flux::http::TransportErrorKind::reset;
}

flux::Task<void> run_http_checks(std::string ok_url,
                                     std::string fail_url,
                                     std::string json_url,
                                     std::string json_get_url,
                                     std::string xml_get_url,
                                     std::string query_url,
                                     std::string download_url) {
    auto *runtime = co_await flux::get_current_runtime();

    auto codec = std::make_shared<SimpleMapJsonCodec>();
    auto xml_codec = std::make_shared<SimpleXmlCodec>();
    auto interceptor = std::make_shared<CountingInterceptor>();
    std::atomic_size_t streamed_bytes = 0;

    flux::http::CookieJarOptions cookie_jar;
    cookie_jar.enabled = true;
    cookie_jar.file_path =
        (std::filesystem::temp_directory_path() / "flux_http_cookie.jar").string();

    flux::http::RetryPolicy retry;
    retry.enabled = true;
    retry.max_attempts = 2;
    retry.base_backoff = std::chrono::milliseconds(20);
    retry.max_backoff = std::chrono::milliseconds(40);
    codec->register_type<std::map<std::string, std::string>>();
    codec->register_type<nlohmann::json>();
    codec->register_type<DemoPayload>();
    codec->register_type<HttpBinPostJsonEcho>();

    auto client = flux::http::Client::build()
                      .runtime(*runtime)
                      .json_codec(codec)
                      .xml_codec(xml_codec)
                      .interceptor(interceptor)
                      .retry(retry)
                      .cookie_jar(cookie_jar)
                      .on_response_chunk([&](std::string_view chunk) {
                          streamed_bytes.fetch_add(chunk.size(), std::memory_order_relaxed);
                      })
                      .default_header("Accept", "*/*")
                      .timeout(std::chrono::seconds(15))
                      .connect_timeout(std::chrono::seconds(8))
                      .throw_on_http_error(true)
                      .build();

    auto response = co_await client.get(std::move(ok_url)).follow_redirects(true).send().raw();

    assert(response.ok());
    assert(response.status_code >= 200);
    assert(response.status_code < 300);
    assert(!response.effective_url.empty());
    assert(has_header(response, "X-Interceptor-Response"));
    assert(streamed_bytes.load(std::memory_order_relaxed) > 0);

    std::atomic_size_t stream_only_bytes = 0;
    auto stream_only_body =
        co_await client.get(response.effective_url)
            .stream_response(
                [&](std::string_view chunk) {
                    stream_only_bytes.fetch_add(chunk.size(), std::memory_order_relaxed);
                },
                false)
            .response_format(flux::http::ResponseFormat::text)
            .send()
            .text();
    assert(stream_only_bytes.load(std::memory_order_relaxed) > 0);
    assert(stream_only_body.empty());

    auto ua_response = co_await client.get(response.effective_url)
                           .user_agent("flux_http_test/ua")
                           .send()
                           .raw();
    assert(ua_response.ok());

    bool got_http_error = false;
    try {
        (void)co_await client.get(std::move(fail_url)).send().raw();
    } catch (const flux::http::HttpError &error) {
        if (error.code() == flux::http::HttpErrorCode::http_status_failure) {
            got_http_error = true;
        } else {
            throw;
        }
    }

    assert(got_http_error);

    {
        bool saw_transport_error = false;
        try {
            (void)co_await client.get("http://nonexistent.async-uv.invalid/")
                .retry(flux::http::RetryPolicy{
                    .enabled = true,
                    .max_attempts = 2,
                    .retry_idempotent_only = true,
                    .base_backoff = std::chrono::milliseconds(5),
                    .max_backoff = std::chrono::milliseconds(10),
                    .retry_on_http_5xx = false,
                })
                .send()
                .raw();
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::curl_failure) {
                saw_transport_error =
                    error.transport_kind() != flux::http::TransportErrorKind::none;
            } else {
                throw;
            }
        }
        assert(saw_transport_error);
    }

    flux::OnceCell<flux::http::Client> cell;
    std::atomic_int init_count = 0;

    auto fetch_once = [&](std::string url) -> flux::Task<long> {
        const auto &shared_client = cell.get_or_init([&]() {
            init_count.fetch_add(1, std::memory_order_relaxed);
            return flux::http::Client::build()
                .runtime(*runtime)
                .default_header("Accept", "*/*")
                .timeout(std::chrono::seconds(15))
                .connect_timeout(std::chrono::seconds(8))
                .throw_on_http_error(true)
                .build();
        });

        auto shared_response =
            co_await shared_client.get(std::move(url)).follow_redirects(true).send().raw();
        assert(shared_response.ok());
        co_return shared_response.status_code;
    };

    auto [s1, s2, s3] = co_await flux::with_task_scope(
        [&](flux::TaskScope &scope) -> flux::Task<std::tuple<long, long, long>> {
            co_return co_await scope.all(
                fetch_once(ok_url), fetch_once(ok_url), fetch_once(ok_url));
        });

    assert(s1 >= 200 && s1 < 300);
    assert(s2 >= 200 && s2 < 300);
    assert(s3 >= 200 && s3 < 300);
    assert(init_count.load(std::memory_order_relaxed) == 1);

    bool saw_mixed_status_error = false;
    try {
        co_await flux::with_task_scope([&](flux::TaskScope &scope) -> flux::Task<void> {
            auto [m1, m2, m3] =
                co_await scope.all(fetch_once(ok_url), fetch_once(fail_url), fetch_once(ok_url));
            (void)m1;
            (void)m2;
            (void)m3;
            co_return;
        });
    } catch (const flux::http::HttpError &error) {
        if (error.code() == flux::http::HttpErrorCode::http_status_failure) {
            saw_mixed_status_error = true;
        } else {
            throw;
        }
    }

    assert(saw_mixed_status_error);
    assert(init_count.load(std::memory_order_relaxed) == 1);
    assert(interceptor->request_count.load(std::memory_order_relaxed) > 0);
    assert(interceptor->response_count.load(std::memory_order_relaxed) > 0);
    assert(interceptor->error_count.load(std::memory_order_relaxed) > 0);

    {
        bool saw_invalid_request = false;
        try {
            (void)co_await client.post(ok_url)
                .request_format(flux::http::RequestFormat::multipart)
                .send()
                .raw();
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::invalid_request) {
                saw_invalid_request = true;
            } else {
                throw;
            }
        }
        assert(saw_invalid_request);
    }

    {
        bool saw_invalid_request = false;
        try {
            (void)co_await client.post(ok_url)
                .multipart({flux::http::field("k", "v")})
                .request_format(flux::http::RequestFormat::json)
                .send()
                .raw();
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::invalid_request) {
                saw_invalid_request = true;
            } else {
                throw;
            }
        }
        assert(saw_invalid_request);
    }

    {
        DemoPayload request_data{"world", "cpp"};
        auto post_echo = co_await client.post(std::move(json_url))
                             .json(request_data)
                             .request_format("json")
                             .response_format(flux::http::ResponseFormat::json)
                             .send()
                             .json<HttpBinPostJsonEcho>();
        assert(post_echo.json.hello == "world");
        assert(post_echo.json.lang == "cpp");

        auto decoded =
            client.parse_json<std::map<std::string, std::string>>("{\"a\":\"1\",\"b\":\"2\"}");
        assert(decoded.size() == 2);
        assert(decoded["a"] == "1");
        assert(decoded["b"] == "2");

        auto decoded_json = client.parse_json<nlohmann::json>("{\"hello\":\"world\"}");
        assert(decoded_json["hello"].get<std::string>() == "world");

        auto decoded_struct = client.parse_json<DemoPayload>("{\"hello\":\"x\",\"lang\":\"y\"}");
        assert(decoded_struct.hello == "x");
        assert(decoded_struct.lang == "y");

        try {
            auto direct_json = co_await client.get(std::move(json_get_url))
                                   .response_format("json")
                                   .send()
                                   .json<nlohmann::json>();
            assert(direct_json.is_object());
            assert(!direct_json.empty());
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::curl_failure &&
                is_network_error(error)) {
                std::cerr << "[flux_http_test] skip: json GET demo unavailable, transport_code="
                          << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
            }
        }

        {
            const auto download_output =
                std::filesystem::temp_directory_path() / "flux_http_download.bin";
            const auto download_temp =
                std::filesystem::temp_directory_path() / "flux_http_download.bin.part";

            {
                std::ofstream pre(download_temp, std::ios::binary | std::ios::trunc);
                auto pre_resp =
                    co_await client.get(download_url)
                        .header("Range", "bytes=0-255")
                        .stream_response(
                            [&](std::string_view chunk) {
                                pre.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                            },
                            false)
                        .response_format(flux::http::ResponseFormat::text)
                        .send()
                        .raw();
                (void)pre_resp;
                pre.flush();
            }

            flux::http::DownloadOptions download_options;
            download_options.resume = true;
            download_options.use_temp_file = true;
            download_options.overwrite = true;
            download_options.temp_path = download_temp;

            auto result =
                co_await client.download_to_file(download_url, download_output, download_options);
            assert(result.output_path == download_output);
            assert(result.status_code == 200 || result.status_code == 206);
            assert(result.size > 0);
            assert(std::filesystem::exists(download_output));

            std::error_code remove_ec;
            std::filesystem::remove(download_output, remove_ec);
            std::filesystem::remove(download_temp, remove_ec);
        }

        try {
            auto cookie_set = co_await client.get("http://httpbin.org/cookies/set?session=flux")
                                  .follow_redirects(true)
                                  .response_format(flux::http::ResponseFormat::json)
                                  .send()
                                  .json<nlohmann::json>();
            (void)cookie_set;

            auto cookie_get = co_await client.get("http://httpbin.org/cookies")
                                  .response_format(flux::http::ResponseFormat::json)
                                  .send()
                                  .json<nlohmann::json>();
            if (cookie_get.contains("cookies") && cookie_get["cookies"].contains("session")) {
                assert(cookie_get["cookies"]["session"].get<std::string>() == "flux");
            }
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::curl_failure &&
                is_network_error(error)) {
                std::cerr
                    << "[flux_http_test] skip: cookie jar demo unavailable, transport_code="
                    << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
            }
        }

        {
            flux::http::RetryPolicy no_retry;
            no_retry.enabled = false;

            flux::http::ProxyOptions bad_proxy;
            bad_proxy.url = "http://127.0.0.1:1";

            try {
                (void)co_await client.get(ok_url)
                    .proxy(bad_proxy)
                    .retry(no_retry)
                    .connect_timeout(std::chrono::milliseconds(300))
                    .timeout(std::chrono::milliseconds(1000))
                    .send()
                    .raw();
            } catch (const flux::http::HttpError &error) {
                if (error.code() == flux::http::HttpErrorCode::curl_failure) {
                    assert(error.transport_kind() != flux::http::TransportErrorKind::none);
                } else {
                    throw;
                }
            }
        }

        try {
            std::unordered_map<std::string, int> numeric_map{{"page", 2}, {"size", 10}};
            std::map<std::string, QueryToStringCamelValue> mode_map{{"mode", {"full"}}};
            std::vector<std::pair<std::string, int>> pair_vec{{"v1", 11}, {"v2", 22}};
            std::array<QueryTupleEntry, 2> tuple_entries{{
                QueryTupleEntry{"tuple_a", "A"},
                QueryTupleEntry{"tuple_b", "B"},
            }};

            auto query_json = co_await client.get(std::move(query_url))
                                  .query({{"lang", "cpp"}, {"sort", "desc"}})
                                  .query("token", QueryToStringValue{7})
                                  .query(numeric_map)
                                  .query(mode_map)
                                  .query(pair_vec)
                                  .query(tuple_entries)
                                  .response_format(flux::http::ResponseFormat::json)
                                  .send()
                                  .json<nlohmann::json>();

            assert(query_json.contains("args"));
            assert(query_json["args"]["lang"].get<std::string>() == "cpp");
            assert(query_json["args"]["sort"].get<std::string>() == "desc");
            assert(query_json["args"]["token"].get<std::string>() == "id-7");
            assert(query_json["args"]["page"].get<std::string>() == "2");
            assert(query_json["args"]["size"].get<std::string>() == "10");
            assert(query_json["args"]["mode"].get<std::string>() == "full");
            assert(query_json["args"]["v1"].get<std::string>() == "11");
            assert(query_json["args"]["v2"].get<std::string>() == "22");
            assert(query_json["args"]["tuple_a"].get<std::string>() == "A");
            assert(query_json["args"]["tuple_b"].get<std::string>() == "B");
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::curl_failure &&
                is_network_error(error)) {
                std::cerr << "[flux_http_test] skip: query demo unavailable, transport_code="
                          << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
            }
        }

        const auto tmp_file =
            std::filesystem::temp_directory_path() / "flux_http_form_upload.txt";
        {
            std::ofstream out(tmp_file, std::ios::binary);
            out << "hello-form-data";
        }

        auto form_data_json = co_await client.post(json_url)
                                  .multipart({
                                      flux::http::field("kind", "demo"),
                                      flux::http::field("lang", QueryToStringCamelValue{"cpp"}),
                                      flux::http::field("seq", 9),
                                      flux::http::file("file", tmp_file.string())
                                          .filename("demo.txt")
                                          .content_type("text/plain"),
                                  })
                                  .request_format(flux::http::RequestFormat::multipart)
                                  .response_format(flux::http::ResponseFormat::json)
                                  .send()
                                  .json<nlohmann::json>();

        assert(form_data_json.contains("form"));
        assert(form_data_json["form"]["kind"].get<std::string>() == "demo");
        assert(form_data_json["form"]["lang"].get<std::string>() == "cpp");
        assert(form_data_json.contains("files"));
        assert(form_data_json["files"]["file"].get<std::string>() == "hello-form-data");

        std::string png_bytes("\x89PNG\r\n\x1A\n", 8);
        auto binary_json = co_await client.post(json_url)
                               .form_binary("image", png_bytes, "demo.png", "image/png")
                               .response_format(flux::http::ResponseFormat::json)
                               .send()
                               .json<nlohmann::json>();
        assert(binary_json.contains("files"));
        assert(binary_json["files"].contains("image"));

        std::unordered_map<std::string, int> post_form{{"x", 10}, {"y", 20}};
        auto urlencoded_json = co_await client.post(json_url)
                                   .urlencoded(post_form)
                                   .request_format(flux::http::RequestFormat::form)
                                   .response_format(flux::http::ResponseFormat::json)
                                   .send()
                                   .json<nlohmann::json>();
        assert(urlencoded_json.contains("form"));
        assert(urlencoded_json["form"]["x"].get<std::string>() == "10");
        assert(urlencoded_json["form"]["y"].get<std::string>() == "20");

        std::error_code remove_ec;
        std::filesystem::remove(tmp_file, remove_ec);

        auto xml_post_json = co_await client.post(json_url)
                                 .xml(DemoPayload{"x", "y"})
                                 .response_format(flux::http::ResponseFormat::json)
                                 .send()
                                 .json<nlohmann::json>();
        assert(xml_post_json.contains("data"));

        auto xml_from_data =
            client.parse_xml<DemoPayload>(xml_post_json["data"].get<std::string>());
        assert(xml_from_data.hello == "x");
        assert(xml_from_data.lang == "y");

        try {
            auto xml_root = co_await client.get(std::move(xml_get_url))
                                .response_format(flux::http::ResponseFormat::xml)
                                .send()
                                .xml<XmlRootInfo>();
            assert(!xml_root.root_name.empty());
        } catch (const flux::http::HttpError &error) {
            if (error.code() == flux::http::HttpErrorCode::curl_failure &&
                is_network_error(error)) {
                std::cerr << "[flux_http_test] skip: xml GET demo unavailable, transport_code="
                          << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
            }
        }
    }
}

} // namespace

int main() {
    const char *ok_env = std::getenv("FLUX_HTTP_TEST_URL");
    const std::string ok_url =
        (ok_env != nullptr && *ok_env != '\0') ? std::string(ok_env) : "http://example.com/";

    const char *fail_env = std::getenv("FLUX_HTTP_TEST_FAIL_URL");
    const std::string fail_url = (fail_env != nullptr && *fail_env != '\0')
                                     ? std::string(fail_env)
                                     : "http://httpstat.us/404";

    const char *json_env = std::getenv("FLUX_HTTP_TEST_JSON_URL");
    const std::string json_url = (json_env != nullptr && *json_env != '\0')
                                     ? std::string(json_env)
                                     : "http://httpbin.org/post";

    const char *json_get_env = std::getenv("FLUX_HTTP_TEST_JSON_GET_URL");
    const std::string json_get_url = (json_get_env != nullptr && *json_get_env != '\0')
                                         ? std::string(json_get_env)
                                         : "http://httpbin.org/json";

    const char *xml_get_env = std::getenv("FLUX_HTTP_TEST_XML_GET_URL");
    const std::string xml_get_url = (xml_get_env != nullptr && *xml_get_env != '\0')
                                        ? std::string(xml_get_env)
                                        : "http://httpbin.org/xml";

    const char *query_env = std::getenv("FLUX_HTTP_TEST_QUERY_URL");
    const std::string query_url = (query_env != nullptr && *query_env != '\0')
                                      ? std::string(query_env)
                                      : "http://httpbin.org/get";

    const char *download_env = std::getenv("FLUX_HTTP_TEST_DOWNLOAD_URL");
    const std::string download_url = (download_env != nullptr && *download_env != '\0')
                                         ? std::string(download_env)
                                         : "http://httpbin.org/range/4096";

    flux::Runtime runtime(flux::Runtime::build().name("flux_http_test"));
    try {
        runtime.block_on(run_http_checks(
            ok_url, fail_url, json_url, json_get_url, xml_get_url, query_url, download_url));
    } catch (const flux::http::HttpError &error) {
        if (error.code() == flux::http::HttpErrorCode::curl_failure &&
            is_network_error(error)) {
            std::cerr << "[flux_http_test] skip: network unavailable, transport_code="
                      << error.transport_code() << " message=" << error.what() << '\n';
            return 0;
        }
        throw;
    }

    return 0;
}
