#include "async_uv_layer3/app.hpp"
#include "async_uv_layer3/middleware.hpp"
#include "async_uv_layer3/stream.hpp"

#include <format>

namespace async_uv::layer3 {

App::App() = default;
App::~App() = default;

App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;

App& App::use(Middleware middleware) {
    middlewares_.push_back(std::move(middleware));
    return *this;
}

App& App::get(std::string_view pattern, Handler handler) {
    router_.get(pattern, std::move(handler));
    return *this;
}

App& App::post(std::string_view pattern, Handler handler) {
    router_.post(pattern, std::move(handler));
    return *this;
}

App& App::put(std::string_view pattern, Handler handler) {
    router_.put(pattern, std::move(handler));
    return *this;
}

App& App::del(std::string_view pattern, Handler handler) {
    router_.del(pattern, std::move(handler));
    return *this;
}

App& App::patch(std::string_view pattern, Handler handler) {
    router_.patch(pattern, std::move(handler));
    return *this;
}

App& App::route(std::string_view prefix, Router sub_router) {
    router_.route(prefix, std::move(sub_router));
    return *this;
}

App& App::with_limits(http::ServerLimits limits) {
    limits_ = limits;
    return *this;
}

App& App::with_policy(http::ServerConnectionPolicy policy) {
    policy_ = policy;
    return *this;
}

Task<void> App::run_middleware_chain(Context& ctx, size_t index) {
    if (index < middlewares_.size()) {
        co_await middlewares_[index](ctx, [this, &ctx, index]() -> Task<void> {
            co_await run_middleware_chain(ctx, index + 1);
        });
    } else {
        auto result = router_.match(ctx.method, ctx.target.path);
        if (result) {
            for (const auto& [key, value] : result->params) {
                ctx.params[key] = value;
            }
            co_await result->handler(ctx);
        } else {
            ctx.status(404);
            ctx.json_raw("{\"error\":\"Not Found\"}");
        }
    }
}

Task<App::Response> App::handle_request(http::ServerRequest request) {
    Context ctx(std::move(request));
    co_await run_middleware_chain(ctx, 0);
    
    if (ctx.is_streaming()) {
        co_return StreamingResponse{std::move(ctx.response), std::move(ctx.stream_handler)};
    } else {
        co_return NormalResponse{std::move(ctx.response)};
    }
}

Task<void> App::send_response(TcpClient& client, Response&& resp) {
    using namespace http;
    
    if (std::holds_alternative<NormalResponse>(resp)) {
        auto& normal = std::get<NormalResponse>(resp);
        co_await write_response_to_socket(client, normal.response, policy_);
    } else {
        auto& streaming = std::get<StreamingResponse>(resp);
        
        auto status_text = [](int code) -> std::string {
            switch (code) {
                case 200: return "OK";
                case 201: return "Created";
                case 204: return "No Content";
                case 301: return "Moved Permanently";
                case 302: return "Found";
                case 307: return "Temporary Redirect";
                case 308: return "Permanent Redirect";
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 405: return "Method Not Allowed";
                case 409: return "Conflict";
                case 413: return "Payload Too Large";
                case 415: return "Unsupported Media Type";
                case 422: return "Unprocessable Entity";
                case 429: return "Too Many Requests";
                case 500: return "Internal Server Error";
                case 501: return "Not Implemented";
                case 503: return "Service Unavailable";
                default: return "Unknown";
            }
        };
        
        std::string status_line = std::format("HTTP/1.1 {} {}\r\n", 
            streaming.response.status_code, 
            streaming.response.reason.empty() ? status_text(streaming.response.status_code) : streaming.response.reason);
        
        std::string headers;
        headers += status_line;
        
        headers += "Transfer-Encoding: chunked\r\n";
        
        if (streaming.response.keep_alive) {
            headers += "Connection: keep-alive\r\n";
        } else {
            headers += "Connection: close\r\n";
        }
        
        for (const auto& h : streaming.response.headers) {
            headers += h.name + ": " + h.value + "\r\n";
        }
        headers += "\r\n";
        
        co_await client.write_all_for(headers, policy_.write_timeout);
        
        detail::SocketStreamWriter writer(std::move(client), policy_.write_timeout);
        co_await streaming.handler(writer);
    }
}

Task<void> App::listen(uint16_t port, std::string_view host) {
    auto listener = co_await TcpListener::bind(std::string(host), static_cast<int>(port));
    if (!listener.valid()) {
        throw std::runtime_error("Failed to listen on " + std::string(host) + ":" + std::to_string(port));
    }

    http::HttpParser parser(http::ParseMode::request, {});

    while (true) {
        auto client = co_await listener.accept();
        if (!client.valid()) {
            continue;
        }

        http::ServerConnectionState conn_state;
        
        while (true) {
            auto request = co_await http::read_request_from_socket(client, parser, limits_, {});
            if (!request) {
                break;
            }

            auto response = co_await handle_request(std::move(*request));
            co_await send_response(client, std::move(response));

            if (std::holds_alternative<NormalResponse>(response)) {
                auto& normal = std::get<NormalResponse>(response);
                if (!http::should_keep_alive(*request, normal.response, policy_, conn_state)) {
                    break;
                }
            }
            
            conn_state.handled_requests++;
        }
    }
}

App app() {
    return App();
}

} // namespace async_uv::layer3