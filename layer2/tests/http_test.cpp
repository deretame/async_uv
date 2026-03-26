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

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <tinyxml2.h>

#include "async_uv/async_uv.h"
#include "async_uv_http/http.h"

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

class SimpleMapJsonCodec : public async_uv::http::JsonCodec {
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

class SimpleXmlCodec : public async_uv::http::XmlCodec {
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

bool is_network_error(int transport_code) {
    const auto curl_code = static_cast<CURLcode>(transport_code);
    return curl_code == CURLE_COULDNT_RESOLVE_HOST || curl_code == CURLE_COULDNT_CONNECT ||
           curl_code == CURLE_OPERATION_TIMEDOUT || curl_code == CURLE_SSL_CONNECT_ERROR ||
           curl_code == CURLE_RECV_ERROR || curl_code == CURLE_SEND_ERROR ||
           curl_code == CURLE_GOT_NOTHING;
}

async_uv::Task<void> run_http_checks(std::string ok_url,
                                     std::string fail_url,
                                     std::string json_url,
                                     std::string json_get_url,
                                     std::string xml_get_url,
                                     std::string query_url) {
    auto *runtime = co_await async_uv::get_current_runtime();

    auto codec = std::make_shared<SimpleMapJsonCodec>();
    auto xml_codec = std::make_shared<SimpleXmlCodec>();
    codec->register_type<std::map<std::string, std::string>>();
    codec->register_type<nlohmann::json>();
    codec->register_type<DemoPayload>();
    codec->register_type<HttpBinPostJsonEcho>();

    auto client = async_uv::http::Client::build()
                      .runtime(*runtime)
                      .json_codec(codec)
                      .xml_codec(xml_codec)
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

    auto ua_response = co_await client.get(response.effective_url)
                           .user_agent("async_uv_http_test/ua")
                           .send()
                           .raw();
    assert(ua_response.ok());

    bool got_http_error = false;
    try {
        (void)co_await client.get(std::move(fail_url)).send().raw();
    } catch (const async_uv::http::HttpError &error) {
        if (error.code() == async_uv::http::HttpErrorCode::http_status_failure) {
            got_http_error = true;
        } else {
            throw;
        }
    }

    assert(got_http_error);

    async_uv::OnceCell<async_uv::http::Client> cell;
    std::atomic_int init_count = 0;

    auto fetch_once = [&](std::string url) -> async_uv::Task<long> {
        const auto &shared_client = cell.get_or_init([&]() {
            init_count.fetch_add(1, std::memory_order_relaxed);
            return async_uv::http::Client::build()
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

    auto [s1, s2, s3] = co_await async_uv::with_task_scope(
        [&](async_uv::TaskScope &scope) -> async_uv::Task<std::tuple<long, long, long>> {
            co_return co_await scope.all(
                fetch_once(ok_url), fetch_once(ok_url), fetch_once(ok_url));
        });

    assert(s1 >= 200 && s1 < 300);
    assert(s2 >= 200 && s2 < 300);
    assert(s3 >= 200 && s3 < 300);
    assert(init_count.load(std::memory_order_relaxed) == 1);

    bool saw_mixed_status_error = false;
    try {
        co_await async_uv::with_task_scope([&](async_uv::TaskScope &scope) -> async_uv::Task<void> {
            auto [m1, m2, m3] =
                co_await scope.all(fetch_once(ok_url), fetch_once(fail_url), fetch_once(ok_url));
            (void)m1;
            (void)m2;
            (void)m3;
            co_return;
        });
    } catch (const async_uv::http::HttpError &error) {
        if (error.code() == async_uv::http::HttpErrorCode::http_status_failure) {
            saw_mixed_status_error = true;
        } else {
            throw;
        }
    }

    assert(saw_mixed_status_error);
    assert(init_count.load(std::memory_order_relaxed) == 1);

    {
        bool saw_invalid_request = false;
        try {
            (void)co_await client.post(ok_url)
                .request_format(async_uv::http::RequestFormat::multipart)
                .send()
                .raw();
        } catch (const async_uv::http::HttpError &error) {
            if (error.code() == async_uv::http::HttpErrorCode::invalid_request) {
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
                .multipart({async_uv::http::field("k", "v")})
                .request_format(async_uv::http::RequestFormat::json)
                .send()
                .raw();
        } catch (const async_uv::http::HttpError &error) {
            if (error.code() == async_uv::http::HttpErrorCode::invalid_request) {
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
                             .response_format(async_uv::http::ResponseFormat::json)
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
        } catch (const async_uv::http::HttpError &error) {
            if (error.code() == async_uv::http::HttpErrorCode::curl_failure &&
                is_network_error(error.transport_code())) {
                std::cerr << "[async_uv_http_test] skip: json GET demo unavailable, transport_code="
                          << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
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
                                  .response_format(async_uv::http::ResponseFormat::json)
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
        } catch (const async_uv::http::HttpError &error) {
            if (error.code() == async_uv::http::HttpErrorCode::curl_failure &&
                is_network_error(error.transport_code())) {
                std::cerr << "[async_uv_http_test] skip: query demo unavailable, transport_code="
                          << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
            }
        }

        const auto tmp_file =
            std::filesystem::temp_directory_path() / "async_uv_http_form_upload.txt";
        {
            std::ofstream out(tmp_file, std::ios::binary);
            out << "hello-form-data";
        }

        auto form_data_json = co_await client.post(json_url)
                                  .multipart({
                                      async_uv::http::field("kind", "demo"),
                                      async_uv::http::field("lang", QueryToStringCamelValue{"cpp"}),
                                      async_uv::http::field("seq", 9),
                                      async_uv::http::file("file", tmp_file.string())
                                          .filename("demo.txt")
                                          .content_type("text/plain"),
                                  })
                                  .request_format(async_uv::http::RequestFormat::multipart)
                                  .response_format(async_uv::http::ResponseFormat::json)
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
                               .response_format(async_uv::http::ResponseFormat::json)
                               .send()
                               .json<nlohmann::json>();
        assert(binary_json.contains("files"));
        assert(binary_json["files"].contains("image"));

        std::unordered_map<std::string, int> post_form{{"x", 10}, {"y", 20}};
        auto urlencoded_json = co_await client.post(json_url)
                                   .urlencoded(post_form)
                                   .request_format(async_uv::http::RequestFormat::form)
                                   .response_format(async_uv::http::ResponseFormat::json)
                                   .send()
                                   .json<nlohmann::json>();
        assert(urlencoded_json.contains("form"));
        assert(urlencoded_json["form"]["x"].get<std::string>() == "10");
        assert(urlencoded_json["form"]["y"].get<std::string>() == "20");

        std::error_code remove_ec;
        std::filesystem::remove(tmp_file, remove_ec);

        auto xml_post_json = co_await client.post(json_url)
                                 .xml(DemoPayload{"x", "y"})
                                 .response_format(async_uv::http::ResponseFormat::json)
                                 .send()
                                 .json<nlohmann::json>();
        assert(xml_post_json.contains("data"));

        auto xml_from_data =
            client.parse_xml<DemoPayload>(xml_post_json["data"].get<std::string>());
        assert(xml_from_data.hello == "x");
        assert(xml_from_data.lang == "y");

        try {
            auto xml_root = co_await client.get(std::move(xml_get_url))
                                .response_format(async_uv::http::ResponseFormat::xml)
                                .send()
                                .xml<XmlRootInfo>();
            assert(!xml_root.root_name.empty());
        } catch (const async_uv::http::HttpError &error) {
            if (error.code() == async_uv::http::HttpErrorCode::curl_failure &&
                is_network_error(error.transport_code())) {
                std::cerr << "[async_uv_http_test] skip: xml GET demo unavailable, transport_code="
                          << error.transport_code() << " message=" << error.what() << '\n';
            } else {
                throw;
            }
        }
    }
}

} // namespace

int main() {
    const char *ok_env = std::getenv("ASYNC_UV_HTTP_TEST_URL");
    const std::string ok_url =
        (ok_env != nullptr && *ok_env != '\0') ? std::string(ok_env) : "http://example.com/";

    const char *fail_env = std::getenv("ASYNC_UV_HTTP_TEST_FAIL_URL");
    const std::string fail_url = (fail_env != nullptr && *fail_env != '\0')
                                     ? std::string(fail_env)
                                     : "http://httpstat.us/404";

    const char *json_env = std::getenv("ASYNC_UV_HTTP_TEST_JSON_URL");
    const std::string json_url = (json_env != nullptr && *json_env != '\0')
                                     ? std::string(json_env)
                                     : "http://httpbin.org/post";

    const char *json_get_env = std::getenv("ASYNC_UV_HTTP_TEST_JSON_GET_URL");
    const std::string json_get_url = (json_get_env != nullptr && *json_get_env != '\0')
                                         ? std::string(json_get_env)
                                         : "http://httpbin.org/json";

    const char *xml_get_env = std::getenv("ASYNC_UV_HTTP_TEST_XML_GET_URL");
    const std::string xml_get_url = (xml_get_env != nullptr && *xml_get_env != '\0')
                                        ? std::string(xml_get_env)
                                        : "http://httpbin.org/xml";

    const char *query_env = std::getenv("ASYNC_UV_HTTP_TEST_QUERY_URL");
    const std::string query_url = (query_env != nullptr && *query_env != '\0')
                                      ? std::string(query_env)
                                      : "http://httpbin.org/get";

    async_uv::Runtime runtime(async_uv::Runtime::build().name("async_uv_http_test"));
    try {
        runtime.block_on(
            run_http_checks(ok_url, fail_url, json_url, json_get_url, xml_get_url, query_url));
    } catch (const async_uv::http::HttpError &error) {
        if (error.code() == async_uv::http::HttpErrorCode::curl_failure &&
            is_network_error(error.transport_code())) {
            std::cerr << "[async_uv_http_test] skip: network unavailable, transport_code="
                      << error.transport_code() << " message=" << error.what() << '\n';
            return 0;
        }
        throw;
    }

    return 0;
}
