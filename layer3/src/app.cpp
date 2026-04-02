#include "async_uv_layer3/app.hpp"
#include "async_uv_layer3/middleware.hpp"
#include "async_uv_layer3/route_group.hpp"
#include "async_uv_layer3/stream.hpp"

#include <exception>
#include <format>

namespace {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string join_methods(const std::vector<std::string>& methods) {
    if (methods.empty()) {
        return "";
    }
    std::string out = methods.front();
    for (size_t i = 1; i < methods.size(); ++i) {
        out += ", ";
        out += methods[i];
    }
    return out;
}

std::optional<std::string> local_string(const async_uv::layer3::Context& ctx, std::string_view key) {
    if (auto value = ctx.local<std::string>(key)) {
        return *value;
    }
    return std::nullopt;
}

std::string build_error_payload(
    int status,
    std::string_view error,
    std::string_view code,
    const async_uv::layer3::Context& ctx) {
    std::string payload = "{";
    payload += "\"status\":" + std::to_string(status);
    payload += ",\"error\":\"" + json_escape(error) + "\"";
    payload += ",\"code\":\"" + json_escape(code) + "\"";
    payload += ",\"biz_code\":null";
    payload += ",\"path\":\"" + json_escape(ctx.target.path) + "\"";
    payload += ",\"method\":\"" + json_escape(ctx.method) + "\"";

    if (auto request_id = local_string(ctx, "request_id")) {
        payload += ",\"request_id\":\"" + json_escape(*request_id) + "\"";
    } else {
        payload += ",\"request_id\":null";
    }

    if (auto trace_id = local_string(ctx, "trace_id")) {
        payload += ",\"trace_id\":\"" + json_escape(*trace_id) + "\"";
    } else {
        payload += ",\"trace_id\":null";
    }

    payload += "}";
    return payload;
}

} // namespace

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

App& App::all(std::string_view pattern, Handler handler) {
    router_.all(pattern, std::move(handler));
    return *this;
}

App& App::get(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
    router_.get(pattern, std::move(handler), std::move(meta));
    return *this;
}

App& App::post(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
    router_.post(pattern, std::move(handler), std::move(meta));
    return *this;
}

App& App::put(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
    router_.put(pattern, std::move(handler), std::move(meta));
    return *this;
}

App& App::del(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
    router_.del(pattern, std::move(handler), std::move(meta));
    return *this;
}

App& App::patch(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
    router_.patch(pattern, std::move(handler), std::move(meta));
    return *this;
}

App& App::all(std::string_view pattern, Handler handler, Router::RouteMeta meta) {
    router_.all(pattern, std::move(handler), std::move(meta));
    return *this;
}

App& App::route(std::string_view prefix, Router sub_router) {
    router_.route(prefix, std::move(sub_router));
    return *this;
}

App& App::router(std::string_view prefix, std::function<void(RouteGroup&)> builder) {
    RouteGroup scoped{std::string(prefix)};
    builder(scoped);
    scoped.apply_to(*this);
    return *this;
}

std::vector<Router::RouteInfo> App::routes() const {
    return router_.routes();
}

App& App::with_limits(http::ServerLimits limits) {
    limits_ = limits;
    return *this;
}

App& App::with_policy(http::ServerConnectionPolicy policy) {
    policy_ = policy;
    return *this;
}

App& App::with_lifecycle(Lifecycle lifecycle) {
    lifecycle_ = std::move(lifecycle);
    return *this;
}

Task<void> App::run_middleware_chain(Context& ctx, size_t index) {
    try {
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
                const auto allow_methods = router_.allowed_methods(ctx.target.path);
                if (!allow_methods.empty()) {
                    ctx.set("Allow", join_methods(allow_methods));
                    if (ctx.method == "OPTIONS") {
                        ctx.send_status(204);
                        co_return;
                    }

                    ctx.status(405);
                    ctx.json_raw(build_error_payload(405, "Method Not Allowed", "METHOD_NOT_ALLOWED", ctx));
                } else {
                    ctx.status(404);
                    ctx.json_raw(build_error_payload(404, "Not Found", "ROUTE_NOT_FOUND", ctx));
                }
            }
        }
    } catch (...) {
        if (lifecycle_ && lifecycle_->on_error) {
            try {
                lifecycle_->on_error(ctx);
            } catch (...) {
            }
        }
        throw;
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
        
        detail::SocketStreamWriter writer(client, policy_.write_timeout);
        co_await streaming.handler(writer);
        co_await writer.close();
    }
}

Task<void> App::listen(uint16_t port, std::string_view host) {
    auto listener = co_await TcpListener::bind(std::string(host), static_cast<int>(port));
    if (!listener.valid()) {
        throw std::runtime_error("Failed to listen on " + std::string(host) + ":" + std::to_string(port));
    }

    if (lifecycle_ && lifecycle_->on_start) {
        co_await lifecycle_->on_start();
    }

    std::exception_ptr listen_error;
    try {
        while (true) {
            auto client = co_await listener.accept();
            if (!client.valid()) {
                continue;
            }

            http::HttpParser parser(http::ParseMode::request, {});
            http::ServerConnectionState conn_state;
            
            while (true) {
                bool send_internal_error = false;
                try {
                    auto request = co_await http::read_request_from_socket(client, parser, limits_, policy_);
                    if (!request) {
                        break;
                    }

                    http::ServerRequest keep_alive_request;
                    keep_alive_request.http_major = request->http_major;
                    keep_alive_request.http_minor = request->http_minor;
                    if (auto conn = request->header("Connection")) {
                        keep_alive_request.headers.push_back({"Connection", *conn});
                    }

                    auto response = co_await handle_request(std::move(*request));
                    bool keep_alive = false;
                    if (std::holds_alternative<NormalResponse>(response)) {
                        auto& normal = std::get<NormalResponse>(response);
                        keep_alive = http::should_keep_alive(keep_alive_request, normal.response, policy_, conn_state);
                        normal.response.keep_alive = keep_alive;
                    } else {
                        auto& streaming = std::get<StreamingResponse>(response);
                        keep_alive = http::should_keep_alive(keep_alive_request, streaming.response, policy_, conn_state);
                        streaming.response.keep_alive = keep_alive;
                    }

                    co_await send_response(client, std::move(response));

                    conn_state.handled_requests++;
                    if (!keep_alive) {
                        break;
                    }
                } catch (...) {
                    send_internal_error = true;
                }

                if (send_internal_error) {
                    http::ServerResponse error_response;
                    error_response.status_code = 500;
                    error_response.body = "{\"error\":\"Internal Server Error\"}";
                    error_response.headers.push_back({"Content-Type", "application/json"});
                    error_response.keep_alive = false;
                    try {
                        co_await http::write_response_to_socket(client, error_response, policy_);
                    } catch (...) {
                    }
                    break;
                }
            }
        }
    } catch (...) {
        listen_error = std::current_exception();
    }

    if (listen_error) {
        if (lifecycle_ && lifecycle_->on_stop) {
            try {
                co_await lifecycle_->on_stop();
            } catch (...) {
            }
        }
        std::rethrow_exception(listen_error);
    }
}

App app() {
    return App();
}

} // namespace async_uv::layer3
