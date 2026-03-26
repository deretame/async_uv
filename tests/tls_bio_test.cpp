#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <string>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>

#include "async_uv/async_uv.h"

namespace {

async_uv::TlsBioResult to_result(int rc) {
    if (rc > 0) {
        return {async_uv::TlsBioStatus::ok, static_cast<std::size_t>(rc), 0};
    }
    if (rc == 0) {
        return {async_uv::TlsBioStatus::ok, 0, 0};
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
        return {async_uv::TlsBioStatus::want_read, 0, rc};
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return {async_uv::TlsBioStatus::want_write, 0, rc};
    }
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF) {
        return {async_uv::TlsBioStatus::closed, 0, rc};
    }
    return {async_uv::TlsBioStatus::error, 0, rc};
}

class MbedTlsBioEngine final : public async_uv::TlsBio {
public:
    explicit MbedTlsBioEngine(mbedtls_ssl_config *config) {
        mbedtls_ssl_init(&ssl_);
        const int setup_rc = mbedtls_ssl_setup(&ssl_, config);
        assert(setup_rc == 0);
        mbedtls_ssl_set_bio(
            &ssl_, this, &MbedTlsBioEngine::send_cb, &MbedTlsBioEngine::recv_cb, nullptr);
    }

    ~MbedTlsBioEngine() override {
        mbedtls_ssl_free(&ssl_);
    }

    bool handshake_finished() const noexcept override {
        return handshake_finished_;
    }

    async_uv::TlsBioResult handshake() override {
        const int rc = mbedtls_ssl_handshake(&ssl_);
        auto result = to_result(rc);
        if (rc == 0) {
            handshake_finished_ = true;
        }
        return result;
    }

    async_uv::TlsBioResult write_encrypted(std::string_view encrypted) override {
        for (unsigned char ch : encrypted) {
            incoming_.push_back(ch);
        }
        return {async_uv::TlsBioStatus::ok, encrypted.size(), 0};
    }

    async_uv::TlsBioResult read_encrypted(std::string &out) override {
        const std::size_t before = out.size();
        out.append(outgoing_);
        const std::size_t appended = out.size() - before;
        outgoing_.clear();
        return {async_uv::TlsBioStatus::ok, appended, 0};
    }

    async_uv::TlsBioResult write_plain(std::string_view plain) override {
        const int rc = mbedtls_ssl_write(
            &ssl_, reinterpret_cast<const unsigned char *>(plain.data()), plain.size());
        return to_result(rc);
    }

    async_uv::TlsBioResult read_plain(std::string &out) override {
        unsigned char buffer[4096];
        const int rc = mbedtls_ssl_read(&ssl_, buffer, sizeof(buffer));
        auto result = to_result(rc);
        if (result.status == async_uv::TlsBioStatus::ok && result.bytes > 0) {
            out.append(reinterpret_cast<const char *>(buffer), result.bytes);
        }
        return result;
    }

private:
    static int send_cb(void *ctx, const unsigned char *buf, size_t len) {
        auto *self = static_cast<MbedTlsBioEngine *>(ctx);
        self->outgoing_.append(reinterpret_cast<const char *>(buf), len);
        return static_cast<int>(len);
    }

    static int recv_cb(void *ctx, unsigned char *buf, size_t len) {
        auto *self = static_cast<MbedTlsBioEngine *>(ctx);
        if (self->incoming_.empty()) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }

        const std::size_t n = std::min(len, self->incoming_.size());
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = self->incoming_.front();
            self->incoming_.pop_front();
        }
        return static_cast<int>(n);
    }

    mbedtls_ssl_context ssl_{};
    bool handshake_finished_ = false;
    std::deque<unsigned char> incoming_;
    std::string outgoing_;
};

void pump(MbedTlsBioEngine &from, MbedTlsBioEngine &to) {
    std::string encrypted;
    const auto pull = from.read_encrypted(encrypted);
    assert(pull.status == async_uv::TlsBioStatus::ok);
    if (!encrypted.empty()) {
        const auto push = to.write_encrypted(encrypted);
        assert(push.status == async_uv::TlsBioStatus::ok);
    }
}

struct MbedTlsFixture {
    MbedTlsFixture() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_ssl_config_init(&client_config);
        mbedtls_ssl_config_init(&server_config);

        static constexpr char kPersonalization[] = "async_uv_tls_bio_test";
        const int seed_rc =
            mbedtls_ctr_drbg_seed(&drbg,
                                  mbedtls_entropy_func,
                                  &entropy,
                                  reinterpret_cast<const unsigned char *>(kPersonalization),
                                  std::strlen(kPersonalization));
        assert(seed_rc == 0);

        const int client_rc = mbedtls_ssl_config_defaults(&client_config,
                                                          MBEDTLS_SSL_IS_CLIENT,
                                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                                          MBEDTLS_SSL_PRESET_DEFAULT);
        assert(client_rc == 0);

        const int server_rc = mbedtls_ssl_config_defaults(&server_config,
                                                          MBEDTLS_SSL_IS_SERVER,
                                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                                          MBEDTLS_SSL_PRESET_DEFAULT);
        assert(server_rc == 0);

        mbedtls_ssl_conf_rng(&client_config, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_rng(&server_config, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_authmode(&client_config, MBEDTLS_SSL_VERIFY_NONE);

        static const unsigned char psk[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        static const unsigned char identity[] = "async_uv_client";

        const int client_psk_rc =
            mbedtls_ssl_conf_psk(&client_config, psk, sizeof(psk), identity, sizeof(identity) - 1);
        assert(client_psk_rc == 0);

        const int server_psk_rc =
            mbedtls_ssl_conf_psk(&server_config, psk, sizeof(psk), identity, sizeof(identity) - 1);
        assert(server_psk_rc == 0);
    }

    ~MbedTlsFixture() {
        mbedtls_ssl_config_free(&client_config);
        mbedtls_ssl_config_free(&server_config);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_ssl_config client_config{};
    mbedtls_ssl_config server_config{};
};

} // namespace

int main() {
    MbedTlsFixture fixture;

    MbedTlsBioEngine client(&fixture.client_config);
    MbedTlsBioEngine server(&fixture.server_config);

    for (int i = 0; i < 10000 && (!client.handshake_finished() || !server.handshake_finished());
         ++i) {
        if (!client.handshake_finished()) {
            const auto result = client.handshake();
            assert(result.status == async_uv::TlsBioStatus::ok ||
                   result.status == async_uv::TlsBioStatus::want_read ||
                   result.status == async_uv::TlsBioStatus::want_write);
        }
        pump(client, server);

        if (!server.handshake_finished()) {
            const auto result = server.handshake();
            assert(result.status == async_uv::TlsBioStatus::ok ||
                   result.status == async_uv::TlsBioStatus::want_read ||
                   result.status == async_uv::TlsBioStatus::want_write);
        }
        pump(server, client);
    }

    assert(client.handshake_finished());
    assert(server.handshake_finished());

    {
        const auto written = client.write_plain("ping");
        assert(written.status == async_uv::TlsBioStatus::ok);
        assert(written.bytes > 0);
        pump(client, server);

        std::string plain;
        for (int i = 0; i < 1000 && plain.empty(); ++i) {
            const auto read = server.read_plain(plain);
            if (read.status == async_uv::TlsBioStatus::ok && read.bytes > 0) {
                break;
            }
            assert(read.status == async_uv::TlsBioStatus::want_read ||
                   read.status == async_uv::TlsBioStatus::want_write ||
                   read.status == async_uv::TlsBioStatus::ok);
            pump(client, server);
            pump(server, client);
        }
        assert(plain == "ping");
    }

    {
        const auto written = server.write_plain("pong");
        assert(written.status == async_uv::TlsBioStatus::ok);
        assert(written.bytes > 0);
        pump(server, client);

        std::string plain;
        for (int i = 0; i < 1000 && plain.empty(); ++i) {
            const auto read = client.read_plain(plain);
            if (read.status == async_uv::TlsBioStatus::ok && read.bytes > 0) {
                break;
            }
            assert(read.status == async_uv::TlsBioStatus::want_read ||
                   read.status == async_uv::TlsBioStatus::want_write ||
                   read.status == async_uv::TlsBioStatus::ok);
            pump(client, server);
            pump(server, client);
        }
        assert(plain == "pong");
    }

    return 0;
}
