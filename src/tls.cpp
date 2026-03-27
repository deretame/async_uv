#include "async_uv/tls.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace async_uv {
namespace {

TlsBioResult map_ssl_result(SSL *ssl, int rc) {
    if (rc > 0) {
        return {TlsBioStatus::ok, static_cast<std::size_t>(rc), 0};
    }

    const int error = SSL_get_error(ssl, rc);
    switch (error) {
        case SSL_ERROR_WANT_READ:
            return {TlsBioStatus::want_read, 0, error};
        case SSL_ERROR_WANT_WRITE:
            return {TlsBioStatus::want_write, 0, error};
        case SSL_ERROR_ZERO_RETURN:
            return {TlsBioStatus::closed, 0, error};
        default:
            return {TlsBioStatus::error, 0, error};
    }
}

void ensure_openssl_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        OPENSSL_init_ssl(0, nullptr);
        SSL_load_error_strings();
    });
}

class OpenSslTlsBio final : public TlsBio {
public:
    explicit OpenSslTlsBio(const OpenSslTlsOptions &options)
        : options_(options),
          ctx_(SSL_CTX_new(options.role == TlsRole::server ? TLS_server_method()
                                                           : TLS_client_method())) {
        if (ctx_ == nullptr) {
            throw std::runtime_error("openssl SSL_CTX_new failed");
        }

        SSL_CTX_set_verify(ctx_, options.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

        if (!options.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(ctx_, options.ca_file.c_str(), nullptr) != 1) {
                throw std::runtime_error("openssl load CA file failed");
            }
        }
        if (!options.cert_file.empty()) {
            if (SSL_CTX_use_certificate_file(ctx_, options.cert_file.c_str(), SSL_FILETYPE_PEM) !=
                1) {
                throw std::runtime_error("openssl load cert file failed");
            }
        }
        if (!options.key_file.empty()) {
            if (SSL_CTX_use_PrivateKey_file(ctx_, options.key_file.c_str(), SSL_FILETYPE_PEM) !=
                1) {
                throw std::runtime_error("openssl load key file failed");
            }
        }

        ssl_ = SSL_new(ctx_);
        if (ssl_ == nullptr) {
            throw std::runtime_error("openssl SSL_new failed");
        }

        BIO *rbio = BIO_new(BIO_s_mem());
        BIO *wbio = BIO_new(BIO_s_mem());
        if (rbio == nullptr || wbio == nullptr) {
            if (rbio != nullptr) {
                BIO_free(rbio);
            }
            if (wbio != nullptr) {
                BIO_free(wbio);
            }
            throw std::runtime_error("openssl BIO_new failed");
        }

        SSL_set_bio(ssl_, rbio, wbio);
        if (options.role == TlsRole::server) {
            SSL_set_accept_state(ssl_);
        } else {
            SSL_set_connect_state(ssl_);
            if (!options.server_name.empty()) {
                SSL_set_tlsext_host_name(ssl_, options.server_name.c_str());
            }
        }
    }

    ~OpenSslTlsBio() override {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    bool handshake_finished() const noexcept override {
        return handshake_done_;
    }

    TlsBioResult handshake() override {
        const int rc = SSL_do_handshake(ssl_);
        auto result = map_ssl_result(ssl_, rc);
        if (result.status == TlsBioStatus::ok) {
            handshake_done_ = true;
        }
        return result;
    }

    TlsBioResult write_encrypted(std::string_view encrypted) override {
        if (encrypted.empty()) {
            return {TlsBioStatus::ok, 0, 0};
        }
        BIO *rbio = SSL_get_rbio(ssl_);
        if (rbio == nullptr) {
            return {TlsBioStatus::error, 0, -1};
        }
        const int rc = BIO_write(rbio, encrypted.data(), static_cast<int>(encrypted.size()));
        if (rc <= 0) {
            return {TlsBioStatus::error, 0, rc};
        }
        return {TlsBioStatus::ok, static_cast<std::size_t>(rc), 0};
    }

    TlsBioResult read_encrypted(std::string &out) override {
        BIO *wbio = SSL_get_wbio(ssl_);
        if (wbio == nullptr) {
            return {TlsBioStatus::error, 0, -1};
        }

        std::array<char, 16 * 1024> buffer{};
        std::size_t total = 0;
        while (true) {
            const int rc = BIO_read(wbio, buffer.data(), static_cast<int>(buffer.size()));
            if (rc <= 0) {
                break;
            }
            out.append(buffer.data(), static_cast<std::size_t>(rc));
            total += static_cast<std::size_t>(rc);
        }
        return {TlsBioStatus::ok, total, 0};
    }

    TlsBioResult write_plain(std::string_view plain) override {
        if (plain.empty()) {
            return {TlsBioStatus::ok, 0, 0};
        }
        const int rc = SSL_write(ssl_, plain.data(), static_cast<int>(plain.size()));
        return map_ssl_result(ssl_, rc);
    }

    TlsBioResult read_plain(std::string &out) override {
        std::array<char, 16 * 1024> buffer{};
        const int rc = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
        auto result = map_ssl_result(ssl_, rc);
        if (result.status == TlsBioStatus::ok && result.bytes > 0) {
            out.append(buffer.data(), result.bytes);
        }
        return result;
    }

private:
    OpenSslTlsOptions options_{};
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    bool handshake_done_ = false;
};

} // namespace

std::unique_ptr<TlsBio> make_openssl_tls_bio(const OpenSslTlsOptions &options) {
    ensure_openssl_init();
    return std::make_unique<OpenSslTlsBio>(options);
}

} // namespace async_uv
