#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace async_uv {

enum class TlsBioStatus {
    ok,
    want_read,
    want_write,
    closed,
    error,
};

struct TlsBioResult {
    TlsBioStatus status = TlsBioStatus::ok;
    std::size_t bytes = 0;
    int code = 0;

    bool success() const noexcept {
        return status == TlsBioStatus::ok;
    }
};

class TlsBio {
public:
    virtual ~TlsBio() = default;

    // 握手是否已完成。
    //
    // Whether the handshake has completed.
    virtual bool handshake_finished() const noexcept = 0;

    // 驱动一次 TLS 握手状态机。
    //
    // Drive one step of the TLS handshake state machine.
    virtual TlsBioResult handshake() = 0;

    // 喂入网络密文数据。
    //
    // Feed encrypted bytes received from the network.
    virtual TlsBioResult write_encrypted(std::string_view encrypted) = 0;

    // 取出待发送到网络的密文数据。
    //
    // Pull encrypted bytes to be sent to the network.
    virtual TlsBioResult read_encrypted(std::string &out) = 0;

    // 喂入应用层明文数据。
    //
    // Feed plaintext bytes from the application layer.
    virtual TlsBioResult write_plain(std::string_view plain) = 0;

    // 取出解密后的明文数据。
    //
    // Pull decrypted plaintext bytes for the application layer.
    virtual TlsBioResult read_plain(std::string &out) = 0;
};

} // namespace async_uv
