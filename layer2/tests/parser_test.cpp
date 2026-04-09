#include <cassert>
#include <filesystem>
#include <string>

#include "flux/flux.h"
#include "flux_http/parser.h"

namespace {

flux::Task<void> run_parser_checks() {
    using flux::http::HttpParser;
    using flux::http::ParseError;
    using flux::http::ParseErrorKind;
    using flux::http::ParseMode;

    {
        HttpParser parser(ParseMode::response);
        co_await parser.feed(
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhe");
        assert(!(co_await parser.has_message()));

        co_await parser.feed("llo");
        assert(co_await parser.has_message());

        auto message = co_await parser.next_message();
        assert(message.has_value());
        assert(!message->is_request());
        assert(message->status_code() == 200);
        assert(message->reason() == "OK");
        assert(message->http_major() == 1);
        assert(message->http_minor() == 1);
        assert(message->body() == "hello");
        assert(message->header("Content-Type").value_or("") == "text/plain");
        assert(message->header_values("Content-Type").size() == 1);
        assert(!(co_await parser.has_message()));
    }

    {
        flux::http::ParserOptions options;
        options.max_feed_chunk_size = 7;
        options.yield_every_chunks = 1;
        HttpParser parser(ParseMode::response, options);
        co_await parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
        auto message = co_await parser.next_message();
        assert(message.has_value());
        assert(message->body() == "hello");
    }

    {
        HttpParser parser(ParseMode::response);
        co_await parser.feed("HTTP/1.1 200 OK\r\n"
                             "Set-Cookie: a=1\r\n"
                             "Set-Cookie: b=2\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "Trailer: Expires\r\n"
                             "\r\n"
                             "5\r\nhello\r\n"
                             "0\r\n"
                             "Expires: soon\r\n"
                             "Set-Cookie: c=3\r\n"
                             "\r\n");
        auto message = co_await parser.next_message();
        assert(message.has_value());
        assert(message->body() == "hello");
        const auto header_values = message->header_values("Set-Cookie");
        assert(header_values.size() == 2);
        assert(header_values[0] == "a=1");
        assert(header_values[1] == "b=2");
        assert(message->trailer("Expires").value_or("") == "soon");
        const auto trailer_values = message->trailer_values("Set-Cookie");
        assert(trailer_values.size() == 1);
        assert(trailer_values[0] == "c=3");
    }

    {
        auto message = co_await flux::http::parse_first_message(
            "GET /demo?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n", ParseMode::request);
        assert(message.has_value());
        assert(message->is_request());
        assert(message->method() == "GET");
        assert(message->url() == "/demo?q=1");
        assert(message->header("Host").value_or("") == "example.com");
    }

    {
        auto messages = co_await flux::http::parse_all_messages(
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK",
            ParseMode::response);
        assert(messages.size() == 2);
        assert(messages[0].status_code() == 204);
        assert(messages[1].status_code() == 200);
        assert(messages[1].body() == "OK");
    }

    {
        const auto temp_dir = std::filesystem::path(flux::path::join(
            std::filesystem::temp_directory_path().string(), "flux_parser_tmp"));
        std::filesystem::create_directories(temp_dir);

        flux::http::ParserOptions options;
        options.max_body_in_memory = 16;
        options.temp_directory = temp_dir;

        HttpParser parser(ParseMode::response, options);
        const std::string big_body(128, 'x');
        const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 128\r\n\r\n" + big_body;
        co_await parser.feed(raw);

        auto message = co_await parser.next_message();
        assert(message.has_value());
        assert(message->body_in_file());
        assert(message->body().empty());
        assert(message->body_size() == 128);
        assert(message->body_file_path().has_value());
        assert(std::filesystem::exists(*message->body_file_path()));

        const auto moved =
            std::filesystem::path(flux::path::join(temp_dir.string(), "moved_body.bin"));
        assert(co_await message->move_body_file_to(moved, true));
        assert(message->body_file_path().has_value());
        assert(*message->body_file_path() == moved);
        assert(std::filesystem::exists(moved));

        std::error_code ec;
        std::filesystem::remove(moved, ec);
    }

    {
        const auto temp_dir = std::filesystem::path(flux::path::join(
            std::filesystem::temp_directory_path().string(), "flux_parser_tmp_dispose"));
        std::filesystem::create_directories(temp_dir);

        flux::http::ParserOptions options;
        options.max_body_in_memory = 16;
        options.temp_directory = temp_dir;

        HttpParser parser(ParseMode::response, options);
        const std::string big_body(64, 'z');
        const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n\r\n" + big_body;
        co_await parser.feed(raw);

        auto message = co_await parser.next_message();
        assert(message.has_value());
        assert(message->body_in_file());
        const auto path = message->body_file_path();
        assert(path.has_value());
        assert(std::filesystem::exists(*path));

        assert(co_await message->dispose_body_file());
        assert(!message->body_in_file());
        assert(!message->body_file_path().has_value());
        assert(!std::filesystem::exists(*path));
    }

    {
        bool threw = false;
        try {
            HttpParser parser(ParseMode::response);
            co_await parser.feed("NOT A VALID HTTP MESSAGE");
        } catch (const ParseError &error) {
            threw = true;
            assert(error.kind() == ParseErrorKind::protocol);
        }
        assert(threw);
    }

    {
        bool threw = false;
        try {
            flux::http::ParserOptions options;
            options.max_header_count = 1;
            HttpParser parser(ParseMode::response, options);
            co_await parser.feed("HTTP/1.1 200 OK\r\nA: 1\r\nB: 2\r\n\r\n");
        } catch (const ParseError &error) {
            threw = true;
            assert(error.kind() == ParseErrorKind::header_count_exceeded);
        }
        assert(threw);
    }

    {
        bool threw = false;
        try {
            flux::http::ParserOptions options;
            options.max_header_line_size = 8;
            HttpParser parser(ParseMode::response, options);
            co_await parser.feed("HTTP/1.1 200 OK\r\nLong: 123456\r\n\r\n");
        } catch (const ParseError &error) {
            threw = true;
            assert(error.kind() == ParseErrorKind::header_line_too_long);
        }
        assert(threw);
    }

    {
        bool threw = false;
        try {
            flux::http::ParserOptions options;
            options.max_start_line_size = 12;
            HttpParser parser(ParseMode::request, options);
            co_await parser.feed("GET /very-long-path HTTP/1.1\r\nHost: example.com\r\n\r\n");
        } catch (const ParseError &error) {
            threw = true;
            assert(error.kind() == ParseErrorKind::start_line_too_long);
        }
        assert(threw);
    }
}

} // namespace

int main() {
    flux::Runtime runtime(flux::Runtime::build().name("flux_http_parser_test"));
    runtime.block_on(run_parser_checks());
    return 0;
}
