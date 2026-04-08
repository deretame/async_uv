#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>

#include "flux/flux.h"

namespace {

namespace asio = exec::asio::asio_impl;

using namespace std::chrono_literals;

flux::FdStream::ConstBuffer as_const_buffer(std::string_view data) {
    return flux::FdStream::ConstBuffer{
        reinterpret_cast<const std::byte *>(data.data()),
        data.size()};
}

enum class RespType {
    simple_string,
    bulk_string,
};

struct RespValue {
    RespType type = RespType::simple_string;
    std::optional<std::string> value;
};

std::string make_resp_command(std::initializer_list<std::string_view> parts) {
    std::string payload;
    payload.reserve(128);
    payload += "*";
    payload += std::to_string(parts.size());
    payload += "\r\n";
    for (std::string_view part : parts) {
        payload += "$";
        payload += std::to_string(part.size());
        payload += "\r\n";
        payload.append(part.data(), part.size());
        payload += "\r\n";
    }
    return payload;
}

flux::Task<flux::FdStream> connect_fd_stream(std::string host, int port) {
    auto ex = co_await flux::get_current_loop();
    asio::ip::tcp::resolver resolver(ex);
    auto endpoints =
        co_await resolver.async_resolve(std::move(host), std::to_string(port), exec::asio::use_sender);

    asio::ip::tcp::socket socket(ex);
    co_await asio::async_connect(socket, endpoints, exec::asio::use_sender);

    auto stream = co_await flux::FdStream::attach(socket.native_handle());
    std::error_code ec;
    socket.close(ec);
    co_return stream;
}

flux::Task<std::string> read_line(flux::FdStream &stream, std::string &buffer) {
    while (true) {
        const auto pos = buffer.find("\r\n");
        if (pos != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            co_return line;
        }

        std::array<std::byte, 4096> chunk{};
        auto n = co_await stream.read_some_sender(std::span<std::byte>(chunk));
        if (n == 0) {
            throw std::runtime_error("redis connection closed while reading line");
        }
        buffer.append(reinterpret_cast<const char *>(chunk.data()), n);
    }
}

flux::Task<void>
read_exact_bytes(flux::FdStream &stream, std::string &buffer, std::size_t bytes) {
    while (buffer.size() < bytes) {
        std::array<std::byte, 4096> chunk{};
        auto n = co_await stream.read_some_sender(std::span<std::byte>(chunk));
        if (n == 0) {
            throw std::runtime_error("redis connection closed while reading payload");
        }
        buffer.append(reinterpret_cast<const char *>(chunk.data()), n);
    }
}

flux::Task<RespValue> read_resp(flux::FdStream &stream, std::string &buffer) {
    const std::string line = co_await read_line(stream, buffer);
    if (line.empty()) {
        throw std::runtime_error("invalid empty RESP line");
    }

    const char prefix = line.front();
    const std::string payload = line.substr(1);
    if (prefix == '+') {
        co_return RespValue{RespType::simple_string, payload};
    }

    if (prefix == '-') {
        throw std::runtime_error("redis error reply: " + payload);
    }

    if (prefix == '$') {
        const long long length = std::stoll(payload);
        if (length < -1) {
            throw std::runtime_error("invalid RESP bulk length");
        }
        if (length == -1) {
            co_return RespValue{RespType::bulk_string, std::nullopt};
        }

        const std::size_t bulk_len = static_cast<std::size_t>(length);
        co_await read_exact_bytes(stream, buffer, bulk_len + 2);
        if (buffer.size() < bulk_len + 2 || buffer[bulk_len] != '\r' || buffer[bulk_len + 1] != '\n') {
            throw std::runtime_error("invalid RESP bulk payload terminator");
        }

        std::string value = buffer.substr(0, bulk_len);
        buffer.erase(0, bulk_len + 2);
        co_return RespValue{RespType::bulk_string, std::move(value)};
    }

    throw std::runtime_error("unsupported RESP prefix");
}

bool looks_like_redis_unavailable(std::string message) {
    std::transform(message.begin(), message.end(), message.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return message.find("connection refused") != std::string::npos ||
           message.find("connection reset") != std::string::npos ||
           message.find("host unreachable") != std::string::npos ||
           message.find("network unreachable") != std::string::npos ||
           message.find("timed out") != std::string::npos;
}

bool is_redis_unavailable_code(const std::error_code &ec) {
    return ec == std::errc::connection_refused || ec == std::errc::connection_reset ||
           ec == std::errc::host_unreachable || ec == std::errc::network_unreachable ||
           ec == std::errc::timed_out;
}

flux::Task<void> run_fd_redis_test() {
    auto stream = co_await connect_fd_stream("127.0.0.1", 6379);
    assert(stream.valid());

    std::string read_buffer;

    {
        const std::string ping = make_resp_command({"PING"});
        auto ping_flow = stream.write_all_sender(as_const_buffer(ping))
                       | stdexec::then([expected = ping.size()](std::size_t sent) {
                             assert(sent == expected);
                         })
                       | stdexec::let_value([&] {
                             return read_resp(stream, read_buffer);
                         });
        auto reply = co_await std::move(ping_flow);
        assert(reply.type == RespType::simple_string);
        assert(reply.value.has_value());
        assert(*reply.value == "PONG");
    }

    const std::string key = "flux:fd:redis:test";
    const std::string value = "fd_sender_ok";

    {
        const std::string set = make_resp_command({"SET", key, value});
        auto set_flow = stream.write_all_sender(as_const_buffer(set))
                      | stdexec::then([expected = set.size()](std::size_t sent) {
                            assert(sent == expected);
                        })
                      | stdexec::let_value([&] {
                            return read_resp(stream, read_buffer);
                        });
        auto reply = co_await std::move(set_flow);
        assert(reply.type == RespType::simple_string);
        assert(reply.value.has_value());
        assert(*reply.value == "OK");
    }

    {
        const std::string get = make_resp_command({"GET", key});
        auto get_flow = stream.write_all_sender(as_const_buffer(get))
                      | stdexec::then([expected = get.size()](std::size_t sent) {
                            assert(sent == expected);
                        })
                      | stdexec::let_value([&] {
                            return read_resp(stream, read_buffer);
                        });
        auto reply = co_await std::move(get_flow);
        assert(reply.type == RespType::bulk_string);
        assert(reply.value.has_value());
        assert(*reply.value == value);
    }

    co_await stream.close();
}

} // namespace

int main() {
    try {
        flux::Runtime runtime(flux::Runtime::build().io_threads(2).blocking_threads(2));
        runtime.block_on(run_fd_redis_test());
        return 0;
    } catch (const std::system_error &error) {
        if (is_redis_unavailable_code(error.code()) || looks_like_redis_unavailable(error.what())) {
            std::cerr << "[skip] redis unavailable at 127.0.0.1:6379: " << error.what() << "\n";
            return 0;
        }
        std::cerr << "fd redis test failed: " << error.what() << "\n";
        return 1;
    } catch (const std::exception &error) {
        if (looks_like_redis_unavailable(error.what())) {
            std::cerr << "[skip] redis unavailable at 127.0.0.1:6379: " << error.what() << "\n";
            return 0;
        }
        std::cerr << "fd redis test failed: " << error.what() << "\n";
        return 1;
    }
}
