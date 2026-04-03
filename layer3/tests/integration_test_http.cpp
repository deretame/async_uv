#include "integration_test_common.hpp"

TEST(app_basic_routing) {
    App app;
    app.get("/hello", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        ctx.send("Hello World");
        co_return;
    });
    
    app.post("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(201);
        ctx.json_raw("{\"created\":true}");
        co_return;
    });
}

TEST(router_static_routing) {
    Router router;
    router.get("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    
    router.post("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(201);
        co_return;
    });
    
    auto result = router.match("GET", "/users");
    assert(result.has_value());
    assert(result->params.empty());
    
    result = router.match("POST", "/users");
    assert(result.has_value());
    
    result = router.match("DELETE", "/users");
    assert(!result.has_value());
}

TEST(router_with_params) {
    Router router;
    router.get("/users/{id}", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    
    auto result = router.match("GET", "/users/123");
    assert(result.has_value());
    assert(result->params["id"] == "123");
    
    result = router.match("GET", "/users/abc");
    assert(result.has_value());
    assert(result->params["id"] == "abc");
}

TEST(router_with_wildcard) {
    Router router;
    router.get("/files/{path*}", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    
    auto result = router.match("GET", "/files/a/b/c.txt");
    assert(result.has_value());
    assert(result->params["path"] == "a/b/c.txt");
    
    result = router.match("GET", "/files/single.txt");
    assert(result.has_value());
    assert(result->params["path"] == "single.txt");
}

TEST(context_json_operations) {
    auto ctx = make_context("POST", "/api", R"({"name":"test","value":42})", 
                           {{"Content-Type", "application/json"}});
    
    std::string json_body = ctx.body;
    assert(json_body.find("test") != std::string::npos);
    assert(json_body.find("42") != std::string::npos);
    
    ctx.json_raw(R"({"status":"ok"})");
    assert(ctx.response.status_code == 200);
}

TEST(context_redirect_methods) {
    auto ctx = make_context("GET", "/old");
    
    ctx.redirect("/new");
    assert(ctx.response.status_code == 302);
    
    ctx.redirect_permanent("/permanent");
    assert(ctx.response.status_code == 301);
}

TEST(context_attachment_headers) {
    auto ctx = make_context("GET", "/download");
    
    ctx.attachment("report.pdf");
    bool found = false;
    for (const auto& h : ctx.response.headers) {
        if (h.name == "Content-Disposition") {
            found = true;
            assert(h.value.find("attachment") != std::string::npos);
            assert(h.value.find("report.pdf") != std::string::npos);
        }
    }
    assert(found);
}

TEST(error_codes) {
    Error err = errors::not_found("User not found", "User ID: 123");
    assert(err.code == ErrorCode::NotFound);
    assert(err.message == "User not found");
    assert(err.status_code() == 404);
    assert(err.status_text() == "Not Found");
    
    std::string json = err.to_json();
    assert(json.find("\"code\":404") != std::string::npos);
    assert(json.find("\"message\":\"User not found\"") != std::string::npos);
}

TEST(error_exceptions) {
    bool caught = false;
    try {
        throw FrameworkError(ErrorCode::BadRequest, "Invalid input");
    } catch (const FrameworkError& ex) {
        caught = true;
        assert(ex.error.code == ErrorCode::BadRequest);
        assert(ex.error.status_code() == 400);
    }
    assert(caught);
}

TEST(middleware_error_handler) {
    auto ctx = make_context("GET", "/test");
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::error_handler(ctx, next));
    assert(ctx.response.status_code == 200);
}

TEST(middleware_error_handler_framework_error_shape) {
    auto ctx = make_context("GET", "/limited");
    ctx.set_local<std::string>("request_id", "req-123");

    Next next = []() -> async_uv::Task<void> {
        throw FrameworkError(ErrorCode::TooManyRequests, "Too many requests");
        co_return;
    };
    async_simple::coro::syncAwait(middleware::error_handler(ctx, next));

    assert(ctx.response.status_code == 429);
    assert(ctx.response.body.find("\"status\":429") != std::string::npos);
    assert(ctx.response.body.find("\"code\":\"TOO_MANY_REQUESTS\"") != std::string::npos);
    assert(ctx.response.body.find("\"request_id\":\"req-123\"") != std::string::npos);
}

TEST(form_parser_integration) {
    std::string body = "username=admin&password=secret123";
    auto ctx = make_context("POST", "/login", body, 
                           {{"Content-Type", "application/x-www-form-urlencoded"}});
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::form_parser(ctx, next));
    
    auto username = form_field(ctx, "username");
    assert(username.has_value());
    assert(*username == "admin");
    
    auto password = form_field(ctx, "password");
    assert(password.has_value());
    assert(*password == "secret123");
}

TEST(cors_basic) {
    middleware::CorsOptions options;
    options.allow_origins = {"*"};
    options.allow_methods = {"GET", "POST", "PUT", "DELETE"};
    options.allow_headers = {"Content-Type", "Authorization"};
    
    auto ctx = make_context("GET", "/api", "", {{"Origin", "http://example.com"}});
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    auto cors_mw = middleware::cors(options);
    async_simple::coro::syncAwait(cors_mw(ctx, next));
    
    bool found_aco = false;
    for (const auto& h : ctx.response.headers) {
        if (h.name == "Access-Control-Allow-Origin") {
            found_aco = true;
            assert(h.value == "*");
        }
    }
    assert(found_aco);
}

TEST(cors_credentials_with_wildcard_origin) {
    middleware::CorsOptions options;
    options.allow_origins = {"*"};
    options.allow_credentials = true;
    auto ctx = make_context("GET", "/api", "", {{"Origin", "http://example.com"}});

    Next next = []() -> async_uv::Task<void> { co_return; };
    auto cors_mw = middleware::cors(options);
    async_simple::coro::syncAwait(cors_mw(ctx, next));

    assert(response_header(ctx, "Access-Control-Allow-Origin").value_or("") == "http://example.com");
    assert(response_header(ctx, "Access-Control-Allow-Credentials").value_or("") == "true");
    assert(response_header(ctx, "Vary").value_or("").find("Origin") != std::string::npos);
}

TEST(cors_preflight) {
    middleware::CorsOptions options;
    options.allow_origins = {"http://localhost:3000"};
    options.allow_methods = {"GET", "POST"};
    
    auto ctx = make_context("OPTIONS", "/api", "", 
                           {{"Origin", "http://localhost:3000"},
                            {"Access-Control-Request-Method", "POST"}});
    
    Next next = []() -> async_uv::Task<void> { co_return; };
    auto cors_mw = middleware::cors(options);
    async_simple::coro::syncAwait(cors_mw(ctx, next));
    
    assert(ctx.response.status_code == 204);
    assert(response_header(ctx, "Vary").value_or("").find("Access-Control-Request-Method") != std::string::npos);
}

TEST(route_group_compile_path) {
    App app;
    RouteGroup api("/api");
    api.use([](Context& ctx, Next next) -> async_uv::Task<void> {
        ctx.set("X-Group", "1");
        co_await next();
    });
    api.get("/users", [](Context& ctx) -> async_uv::Task<void> {
        ctx.status(200);
        co_return;
    });
    api.apply_to(app);
}

TEST(app_router_dsl_nested) {
    App app;
    app.use([](Context& ctx, Next next) -> async_uv::Task<void> {
        ctx.set_local<std::string>("global_mw", "1");
        co_await next();
    });

    app.router("/api", [](RouteGroup& r) {
        r.post("/login", [](Context& ctx) -> async_uv::Task<void> {
            ctx.status(200);
            co_return;
        });

        r.router("", [](RouteGroup& protected_routes) {
            protected_routes.use([](Context& ctx, Next next) -> async_uv::Task<void> {
                ctx.set_local<std::string>("auth_mw", "1");
                co_await next();
            });

            protected_routes.get("/profile", [](Context& ctx) -> async_uv::Task<void> {
                auto auth = ctx.local<std::string>("auth_mw");
                if (!auth || *auth != "1") {
                    throw FrameworkError(ErrorCode::Unauthorized, "missing auth middleware");
                }
                ctx.status(200);
                co_return;
            });

            protected_routes.router("/admin", [](RouteGroup& admin) {
                admin.get("/users", [](Context& ctx) -> async_uv::Task<void> {
                    ctx.status(200);
                    co_return;
                });
            });
        });
    });

    const auto routes = app.routes();
    auto has_route = [&](std::string_view method, std::string_view pattern) {
        for (const auto& route : routes) {
            if (route.method == method && route.pattern == pattern) {
                return true;
            }
        }
        return false;
    };

    assert(has_route("POST", "/api/login"));
    assert(has_route("GET", "/api/profile"));
    assert(has_route("GET", "/api/admin/users"));
}

TEST(rate_limit_blocks_after_threshold) {
    auto limiter = middleware::create_rate_limiter({.max_requests = 1, .window = std::chrono::milliseconds(60000)});
    auto ctx = make_context("GET", "/limited");

    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(middleware::rate_limit(ctx, limiter, next));

    bool blocked = false;
    try {
        async_simple::coro::syncAwait(middleware::rate_limit(ctx, limiter, next));
    } catch (const FrameworkError& ex) {
        blocked = true;
        assert(ex.error.code == ErrorCode::TooManyRequests);
    }
    assert(blocked);
}

TEST(context_send_file_and_download) {
    const auto tmp = std::filesystem::path("/tmp/async_uv_layer3_send_file.txt");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "hello-file";
    }

    auto ctx = make_context("GET", "/file");
    ctx.send_file(tmp);
    assert(ctx.response.body == "hello-file");
    assert(response_header(ctx, "Content-Type").value_or("") == "text/plain");

    auto dl_ctx = make_context("GET", "/download");
    dl_ctx.download(tmp, "report.txt");
    assert(dl_ctx.response.body == "hello-file");
    assert(response_header(dl_ctx, "Content-Disposition").value_or("").find("report.txt") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST(middleware_security_headers) {
    auto ctx = make_context("GET", "/safe");
    auto mw = middleware::security_headers();
    Next next = []() -> async_uv::Task<void> { co_return; };
    async_simple::coro::syncAwait(mw(ctx, next));

    assert(response_header(ctx, "X-Content-Type-Options").value_or("") == "nosniff");
    assert(response_header(ctx, "X-Frame-Options").value_or("") == "DENY");
    assert(response_header(ctx, "Content-Security-Policy").value_or("").find("default-src") != std::string::npos);
}

TEST(middleware_etag) {
    auto ctx = make_context("GET", "/etag");
    auto mw = middleware::etag();
    Next next = [&ctx]() -> async_uv::Task<void> {
        ctx.send("etag-body");
        co_return;
    };
    async_simple::coro::syncAwait(mw(ctx, next));
    auto tag = response_header(ctx, "ETag");
    assert(tag.has_value());

    auto ctx2 = make_context("GET", "/etag", "", {{"If-None-Match", *tag}});
    Next next2 = [&ctx2]() -> async_uv::Task<void> {
        ctx2.send("etag-body");
        co_return;
    };
    async_simple::coro::syncAwait(mw(ctx2, next2));
    assert(ctx2.response.status_code == 304);
    assert(ctx2.response.body.empty());
}

TEST(middleware_compress) {
    middleware::CompressOptions options;
    options.min_size = 32;
    options.level = 6;
    options.types = {"text/*"};

    auto ctx = make_context("GET", "/compress", "", {{"Accept-Encoding", "gzip, deflate"}});
    auto mw = middleware::compress(options);
    Next next = [&ctx]() -> async_uv::Task<void> {
        ctx.content_type("text/plain");
        ctx.send(std::string(4096, 'a'));
        co_return;
    };
    async_simple::coro::syncAwait(mw(ctx, next));

#if defined(ASYNC_UV_LAYER3_HAS_ZLIB) && ASYNC_UV_LAYER3_HAS_ZLIB
    assert(response_header(ctx, "Content-Encoding").value_or("") == "gzip");
    assert(response_header(ctx, "Vary").value_or("").find("Accept-Encoding") != std::string::npos);
    assert(ctx.response.body.size() < 4096);
#else
    assert(!response_header(ctx, "Content-Encoding").has_value());
#endif
}
