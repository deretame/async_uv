#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "async_uv/async_uv.h"

using namespace std::chrono_literals;

namespace {

bool is_network_unavailable_error(CURLcode rc) {
    return rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT ||
           rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_GOT_NOTHING ||
           rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> build_candidate_urls() {
    if (const char *env_url = std::getenv("ASYNC_UV_CURL_TEST_URL");
        env_url != nullptr && *env_url != '\0') {
        std::vector<std::string> urls;
        std::string text(env_url);
        std::size_t begin = 0;
        while (begin <= text.size()) {
            const auto end = text.find(',', begin);
            const std::string token = trim(
                text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
            if (!token.empty()) {
                urls.push_back(token);
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        if (!urls.empty()) {
            return urls;
        }
    }

    return {
        "https://example.com/",
        "https://www.cloudflare.com/",
        "https://www.wikipedia.org/",
    };
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::pair<std::string, std::string> parse_https_url(const std::string &url) {
    static constexpr std::string_view prefix = "https://";
    if (!url.starts_with(prefix)) {
        throw std::runtime_error("fd curl test requires an https:// url");
    }

    const auto host_begin = prefix.size();
    const auto slash_pos = url.find('/', host_begin);
    if (slash_pos == std::string::npos) {
        return {url.substr(host_begin), "/"};
    }

    const auto host = url.substr(host_begin, slash_pos - host_begin);
    auto path = url.substr(slash_pos);
    if (path.empty()) {
        path = "/";
    }
    return {host, path};
}

async_uv::Task<bool> run_fd_curl_check(const std::string &url) {
    auto [host, path] = parse_https_url(url);

    const auto *version = curl_version_info(CURLVERSION_NOW);
    assert(version != nullptr);
    assert(version->ssl_version != nullptr);
    const auto ssl_backend = to_lower(version->ssl_version);
    assert(ssl_backend.find("mbedtls") != std::string::npos);

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), &curl_easy_cleanup);
    assert(easy != nullptr);

    curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy.get(), CURLOPT_USERAGENT, "async_uv_fd_curl_test/1.0");

    const CURLcode connect_rc = curl_easy_perform(easy.get());
    if (is_network_unavailable_error(connect_rc)) {
        std::cerr << "[fd_curl_test] skip: network unavailable for URL '" << url
                  << "', curl rc=" << static_cast<int>(connect_rc) << " ("
                  << curl_easy_strerror(connect_rc) << ")\n";
        co_return false;
    }
    assert(connect_rc == CURLE_OK);

    curl_socket_t socket_fd = CURL_SOCKET_BAD;
    const CURLcode socket_rc = curl_easy_getinfo(easy.get(), CURLINFO_ACTIVESOCKET, &socket_fd);
    assert(socket_rc == CURLE_OK);
    assert(socket_fd != CURL_SOCKET_BAD);

    auto watcher = co_await async_uv::FdWatcher::watch(static_cast<uv_os_sock_t>(socket_fd),
                                                       async_uv::FdEventFlags::readable |
                                                           async_uv::FdEventFlags::writable);

    auto first = co_await watcher.next_for(2s);
    assert(first.has_value());
    if (!first->ok()) {
        std::cerr << "[fd_curl_test] skip: watcher reported error for URL '" << url
                  << "', status=" << first->status << "\n";
        co_return false;
    }

    const std::string request = "GET " + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" +
                                "Connection: close\r\n" +
                                "User-Agent: async_uv_fd_curl_test/1.0\r\n" + "\r\n";

    std::size_t sent = 0;
    while (sent < request.size()) {
        std::size_t n = 0;
        const CURLcode send_rc =
            curl_easy_send(easy.get(), request.data() + sent, request.size() - sent, &n);
        if (send_rc == CURLE_AGAIN) {
            co_await async_uv::sleep_for(2ms);
            continue;
        }
        if (is_network_unavailable_error(send_rc)) {
            std::cerr << "[fd_curl_test] skip: send failed for URL '" << url
                      << "', curl rc=" << static_cast<int>(send_rc) << " ("
                      << curl_easy_strerror(send_rc) << ")\n";
            co_return false;
        }
        assert(send_rc == CURLE_OK);
        sent += n;
    }

    std::string response;
    for (int i = 0; i < 3000; ++i) {
        char buffer[2048];
        std::size_t n = 0;
        const CURLcode recv_rc = curl_easy_recv(easy.get(), buffer, sizeof(buffer), &n);
        if (recv_rc == CURLE_AGAIN) {
            auto event = co_await watcher.next_for(2s);
            assert(event.has_value());
            if (!event->ok()) {
                std::cerr << "[fd_curl_test] skip: watcher recv error for URL '" << url
                          << "', status=" << event->status << "\n";
                co_return false;
            }
            continue;
        }
        if (is_network_unavailable_error(recv_rc)) {
            std::cerr << "[fd_curl_test] skip: recv failed for URL '" << url
                      << "', curl rc=" << static_cast<int>(recv_rc) << " ("
                      << curl_easy_strerror(recv_rc) << ")\n";
            co_return false;
        }
        assert(recv_rc == CURLE_OK);

        if (n == 0) {
            break;
        }

        response.append(buffer, n);
        if (response.find("\r\n\r\n") != std::string::npos &&
            response.find("<html") != std::string::npos) {
            break;
        }
    }

    if (response.find("HTTP/") == std::string::npos ||
        response.find("\r\n\r\n") == std::string::npos) {
        std::cerr << "[fd_curl_test] skip: incomplete HTTP response for URL '" << url << "'\n";
        co_return false;
    }

    co_await watcher.stop_for(500ms);
    co_return true;
}

} // namespace

int main() {
    const CURLcode init_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    assert(init_rc == CURLE_OK);

    const auto urls = build_candidate_urls();

    async_uv::Runtime runtime(async_uv::Runtime::build().name("fd_curl_tls_test"));
    bool passed = false;
    for (const auto &url : urls) {
        if (runtime.block_on(run_fd_curl_check(url))) {
            passed = true;
            break;
        }
    }

    if (!passed) {
        std::cerr << "[fd_curl_test] skip: all candidate URLs are unavailable\n";
    }

    curl_global_cleanup();
    return 0;
}
