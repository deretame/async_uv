#include <cassert>
#include <iostream>
#include <string>

#include <async_uv/task.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_uv_layer3/multipart_parser.hpp>
#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/types.hpp>
#include <async_uv_http/server.h>

using namespace async_uv::layer3;
using namespace async_uv::http;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { std::cout << "Running " #name "... "; test_##name(); std::cout << "OK\n"; } while(0)

TEST(multipart_parse_basic) {
    std::string body = 
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1\r\n"
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"field2\"\r\n"
        "\r\n"
        "value2\r\n"
        "--boundary--";
    
    auto result = parse_multipart(body, "boundary");
    assert(result.has_value());
    assert(result->fields.size() == 2);
    assert(result->get("field1") == "value1");
    assert(result->get("field2") == "value2");
}

TEST(multipart_parse_file) {
    std::string body =
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World\r\n"
        "--boundary--";
    
    auto result = parse_multipart(body, "boundary");
    assert(result.has_value());
    
    auto file = result->get_file("file");
    assert(file.has_value());
    assert(file->filename == "test.txt");
    assert(file->content_type == "text/plain");
    assert(file->data == "Hello World");
    assert(file->is_file());
}

TEST(multipart_parse_multiple_files) {
    std::string body =
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"files\"; filename=\"file1.txt\"\r\n"
        "\r\n"
        "content1\r\n"
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"files\"; filename=\"file2.txt\"\r\n"
        "\r\n"
        "content2\r\n"
        "--boundary--";
    
    auto result = parse_multipart(body, "boundary");
    assert(result.has_value());
    
    auto all_files = result->get_all("files");
    assert(all_files != nullptr);
    assert(all_files->size() == 2);
    assert((*all_files)[0].filename == "file1.txt");
    assert((*all_files)[1].filename == "file2.txt");
}

TEST(multipart_parse_mixed) {
    std::string body =
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "alice\r\n"
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"avatar.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n"
        "BINARY_DATA\r\n"
        "--boundary--";
    
    auto result = parse_multipart(body, "boundary");
    assert(result.has_value());
    
    auto username = result->get("username");
    assert(username.has_value());
    assert(*username == "alice");
    
    auto avatar = result->get_file("avatar");
    assert(avatar.has_value());
    assert(avatar->filename == "avatar.jpg");
    assert(avatar->content_type == "image/jpeg");
}

TEST(multipart_parse_quoted_boundary) {
    std::string body =
        "--abc123\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--abc123--";
    
    auto result = parse_multipart(body, "abc123");
    assert(result.has_value());
    assert(result->get("field") == "value");
}

TEST(multipart_parse_empty) {
    std::string body = "--boundary--";
    auto result = parse_multipart(body, "boundary");
    assert(result.has_value());
    assert(result->fields.empty());
}

TEST(multipart_content_disposition_parsing) {
    std::string body =
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"field\"; filename=\"test.txt\"\r\n"
        "\r\n"
        "data\r\n"
        "--boundary--";
    
    auto result = parse_multipart(body, "boundary");
    assert(result.has_value());
    
    auto file = result->get_file("field");
    assert(file.has_value());
    assert(file->name == "field");
    assert(file->filename == "test.txt");
}

TEST(multipart_middleware_integration) {
    std::string body =
        "--boundary\r\n"
        "Content-Disposition: form-data; name=\"test\"\r\n"
        "\r\n"
        "value\r\n"
        "--boundary--";
    
    ServerRequest req;
    req.body = body;
    req.headers.push_back({"Content-Type", "multipart/form-data; boundary=boundary"});
    
    Context ctx(std::move(req));
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::multipart_parser(ctx, next));
    
    auto multipart = get_multipart(ctx);
    assert(multipart.has_value());
    assert(multipart->get("test") == "value");
}

TEST(multipart_large_file_handling) {
    std::string boundary = "boundary123";
    std::string filename = "large_file.bin";
    std::string content(1024, 'X');
    
    std::string body =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        + content + "\r\n"
        "--" + boundary + "--";
    
    auto result = parse_multipart(body, boundary);
    assert(result.has_value());
    
    auto file = result->get_file("file");
    assert(file.has_value());
    assert(file->filename == filename);
    assert(file->content_type == "application/octet-stream");
    assert(file->data.size() == content.size());
    assert(file->data == content);
}

int main() {
    std::cout << "=== Multipart Parser Tests ===\n";
    
    RUN_TEST(multipart_parse_basic);
    RUN_TEST(multipart_parse_file);
    RUN_TEST(multipart_parse_multiple_files);
    RUN_TEST(multipart_parse_mixed);
    RUN_TEST(multipart_parse_quoted_boundary);
    RUN_TEST(multipart_parse_empty);
    RUN_TEST(multipart_content_disposition_parsing);
    RUN_TEST(multipart_middleware_integration);
    RUN_TEST(multipart_large_file_handling);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}