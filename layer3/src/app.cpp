#include "async_uv_layer3/app.hpp"
#include "async_uv_layer3/middleware.hpp"

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

Task<http::ServerResponse> App::handle_request(http::ServerRequest request) {
    Context ctx(std::move(request));
    co_await run_middleware_chain(ctx, 0);
    co_return std::move(ctx.response);
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
            co_await http::write_response_to_socket(client, response, policy_);

            if (!http::should_keep_alive(*request, response, policy_, conn_state)) {
                break;
            }
            
            conn_state.handled_requests++;
        }
    }
}

App app() {
    return App();
}

} // namespace async_uv::layer3