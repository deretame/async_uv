#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <string>
#include <map>
#include <thread>
#include <vector>

#include <async_uv/task.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_uv_layer3/app.hpp>
#include <async_uv_layer3/router.hpp>
#include <async_uv_layer3/context.hpp>
#include <async_uv_layer3/types.hpp>
#include <async_uv_layer3/middleware.hpp>
#include <async_uv_layer3/route_group.hpp>
#include <async_uv_layer3/form_parser.hpp>
#include <async_uv_layer3/cors.hpp>
#include <async_uv_layer3/rate_limit.hpp>
#include <async_uv_layer3/error.hpp>
#include <async_uv_layer3/di.hpp>
#include <async_uv_http/server.h>

using namespace async_uv::layer3;
using namespace async_uv::http;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { std::cout << "Running " #name "... "; test_##name(); std::cout << "OK\n"; } while(0)

std::optional<std::string> response_header(const Context& ctx, const std::string& name) {
    for (const auto& h : ctx.response.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return std::nullopt;
}

Context make_context(const std::string& method, const std::string& path, 
                     const std::string& body = "", 
                     const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    ServerRequest req;
    req.method = method;
    req.raw_target = path;
    req.target = parse_request_target(path);
    req.body = body;
    for (const auto& [name, value] : headers) {
        req.headers.push_back({name, value});
    }
    return Context(std::move(req));
}

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

namespace {

struct DiConfig {
    int timeout_ms = 0;
};

struct DiLogger {
    int level = 0;
};

struct DiRepo {
    std::shared_ptr<DiLogger> logger;
    DiConfig config;
};

struct DiService {
    std::shared_ptr<DiLogger> logger;
    DiRepo repo;
};

struct DiController {
    std::shared_ptr<DiLogger> logger;

    async_uv::Task<void> profile(Context& ctx) {
        if (logger) {
            ++logger->level;
        }
        ctx.status(200);
        ctx.send("profile-ok");
        co_return;
    }

    async_uv::Task<void> login(Context& ctx) const {
        ctx.status(200);
        ctx.send("login-ok");
        co_return;
    }
};

struct DiNamedController {
    std::string name;

    async_uv::Task<void> who(Context& ctx) const {
        ctx.status(200);
        ctx.send(name);
        co_return;
    }
};

struct DiMountedController {
    std::shared_ptr<DiLogger> logger;
    static constexpr std::string_view prefix = "/users";

    static void map(di::ControllerBinder<DiMountedController>& r) {
        r.post("/login", &DiMountedController::login);
        r.router("/v1", [](di::ControllerBinder<DiMountedController>& v1) {
            v1.get("/profile", &DiMountedController::profile);
        });
    }

    async_uv::Task<void> login(Context& ctx) const {
        ctx.status(200);
        ctx.send("mounted-login");
        co_return;
    }

    async_uv::Task<void> profile(Context& ctx) {
        if (logger) {
            ++logger->level;
        }
        ctx.status(200);
        ctx.send("mounted-profile");
        co_return;
    }
};

struct DiLegacyMappedController {
    static void map(di::RouteBinder& routes) {
        routes.get<DiLegacyMappedController>("/ping", &DiLegacyMappedController::ping);
    }

    async_uv::Task<void> ping(Context& ctx) const {
        ctx.status(200);
        ctx.send("legacy-ok");
        co_return;
    }
};

struct DiTenantController {
    std::string tenant;
    static constexpr std::string_view prefix = "/tenant";

    static void map(di::ControllerBinder<DiTenantController>& r) {
        r.get("/who", &DiTenantController::who);
    }

    async_uv::Task<void> who(Context& ctx) const {
        ctx.status(200);
        ctx.send(tenant);
        co_return;
    }
};

struct DiA;
struct DiB;

struct DiA {
    std::shared_ptr<DiB> b;
};

struct DiB {
    std::shared_ptr<DiA> a;
};

} // namespace

TEST(di_container_reflect_injection) {
    di::Container container;
    container.singleton<DiLogger>([] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 7;
        return logger;
    });
    container.singleton<DiConfig>([] { return DiConfig{.timeout_ms = 2500}; });

    auto service = container.resolve_shared<DiService>();

    assert(service->logger);
    assert(service->repo.logger);
    assert(service->logger.get() == service->repo.logger.get());
    assert(service->logger->level == 7);
    assert(service->repo.config.timeout_ms == 2500);
}

TEST(di_container_transient_lifetime) {
    di::Container container;
    int seq = 0;

    container.transient<DiConfig>([&]() { return DiConfig{.timeout_ms = ++seq}; });

    auto first = container.resolve_shared<DiConfig>();
    auto second = container.resolve_shared<DiConfig>();

    assert(first->timeout_ms == 1);
    assert(second->timeout_ms == 2);
    assert(first.get() != second.get());
}

TEST(di_container_named_singleton_management) {
    di::Container container;
    container.singleton<DiLogger>("auth", [] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 21;
        return logger;
    });
    container.singleton<DiLogger>("admin", [] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 42;
        return logger;
    });

    assert(container.has_binding<DiLogger>("auth"));
    assert(container.has_binding<DiLogger>("admin"));

    auto auth_1 = container.resolve_shared<DiLogger>("auth");
    auto auth_2 = container.resolve_shared<DiLogger>("auth");
    auto admin = container.resolve_shared<DiLogger>("admin");

    assert(auth_1.get() == auth_2.get());
    assert(auth_1.get() != admin.get());
    assert(auth_1->level == 21);
    assert(admin->level == 42);
}

TEST(di_container_keyed_inject_and_fallback) {
    di::Container container;
    container.singleton<DiLogger>([] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 5;
        return logger;
    });
    container.singleton<DiLogger>("auth", [] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 99;
        return logger;
    });
    container.singleton<DiConfig>([] { return DiConfig{.timeout_ms = 1200}; });
    container.singleton<DiConfig>("auth", [] { return DiConfig{.timeout_ms = 3200}; });

    DiRepo auth_repo;
    container.inject(auth_repo, "auth");
    assert(auth_repo.logger);
    assert(auth_repo.logger->level == 99);
    assert(auth_repo.config.timeout_ms == 3200);

    DiRepo fallback_repo;
    container.inject(fallback_repo, "unknown-key");
    assert(fallback_repo.logger);
    assert(fallback_repo.logger->level == 5);
    assert(fallback_repo.config.timeout_ms == 1200);
}

TEST(di_container_resolve_ref_for_transient_throws) {
    di::Container container;
    container.transient<DiConfig>("transient_only", [] { return DiConfig{.timeout_ms = 1}; });

    bool caught = false;
    try {
        (void)container.resolve<DiConfig>("transient_only");
    } catch (const di::Error&) {
        caught = true;
    }
    assert(caught);
}

TEST(di_container_cycle_detection) {
    di::Container container;

    bool caught = false;
    try {
        (void)container.resolve_shared<DiA>();
    } catch (const di::Error&) {
        caught = true;
    }

    assert(caught);
}

TEST(di_global_and_tls_container_accessors) {
    auto& global_a = di::global_di();
    auto& global_b = di::global_di();
    assert(&global_a == &global_b);

    auto& tls_a = di::thread_local_di();
    auto& tls_b = di::thread_local_di();
    assert(&tls_a == &tls_b);
}

TEST(di_route_binder_controller_methods) {
    di::Container container;
    container.singleton<DiLogger>([] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 10;
        return logger;
    });

    RouteGroup api("/api");
    auto routes = di::bind_routes(api, container);
    routes.post<DiController>("/login", &DiController::login);
    routes.get<DiController>("/profile", &DiController::profile);

    Router router = api.to_router();
    auto login = router.match("POST", "/api/login");
    auto profile = router.match("GET", "/api/profile");
    assert(login.has_value());
    assert(profile.has_value());

    auto login_ctx = make_context("POST", "/api/login");
    async_simple::coro::syncAwait(login->handler(login_ctx));
    assert(login_ctx.response.status_code == 200);
    assert(login_ctx.response.body == "login-ok");

    auto profile_ctx = make_context("GET", "/api/profile");
    async_simple::coro::syncAwait(profile->handler(profile_ctx));
    assert(profile_ctx.response.status_code == 200);
    assert(profile_ctx.response.body == "profile-ok");

    auto logger = container.resolve_shared<DiLogger>();
    assert(logger->level == 11);
}

TEST(di_route_binder_with_key_selects_named_controller) {
    di::Container container;
    container.singleton<DiNamedController>([] {
        auto c = std::make_shared<DiNamedController>();
        c->name = "default-controller";
        return c;
    });
    container.singleton<DiNamedController>("auth", [] {
        auto c = std::make_shared<DiNamedController>();
        c->name = "auth-controller";
        return c;
    });
    container.singleton<DiNamedController>("admin", [] {
        auto c = std::make_shared<DiNamedController>();
        c->name = "admin-controller";
        return c;
    });

    RouteGroup api("/api");
    auto routes = di::bind_routes(api, container);
    routes.with_key("auth").get<DiNamedController>("/auth", &DiNamedController::who);
    routes.with_key("admin").get<DiNamedController>("/ops", &DiNamedController::who);
    routes.with_key("missing").get<DiNamedController>("/fallback", &DiNamedController::who);

    Router router = api.to_router();
    auto auth = router.match("GET", "/api/auth");
    auto admin = router.match("GET", "/api/ops");
    auto fallback = router.match("GET", "/api/fallback");
    assert(auth.has_value());
    assert(admin.has_value());
    assert(fallback.has_value());

    auto auth_ctx = make_context("GET", "/api/auth");
    async_simple::coro::syncAwait(auth->handler(auth_ctx));
    assert(auth_ctx.response.body == "auth-controller");

    auto admin_ctx = make_context("GET", "/api/ops");
    async_simple::coro::syncAwait(admin->handler(admin_ctx));
    assert(admin_ctx.response.body == "admin-controller");

    auto fallback_ctx = make_context("GET", "/api/fallback");
    async_simple::coro::syncAwait(fallback->handler(fallback_ctx));
    assert(fallback_ctx.response.body == "default-controller");
}

TEST(di_container_concurrent_singleton_stress) {
    di::Container container;
    container.singleton<DiLogger>("stress", [] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 1;
        return logger;
    });
    container.singleton<DiLogger>([] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 2;
        return logger;
    });

    constexpr int kThreads = 8;
    constexpr int kIterations = 2000;

    std::atomic<DiLogger*> stress_ptr{nullptr};
    std::atomic<DiLogger*> default_ptr{nullptr};
    std::atomic<bool> failed{false};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            try {
                for (int i = 0; i < kIterations; ++i) {
                    DiLogger* p1 = container.resolve_shared<DiLogger>("stress").get();
                    DiLogger* expected1 = stress_ptr.load(std::memory_order_acquire);
                    if (!expected1) {
                        stress_ptr.compare_exchange_strong(expected1, p1, std::memory_order_acq_rel);
                    } else if (expected1 != p1) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }

                    DiLogger* p2 = container.resolve_shared<DiLogger>().get();
                    DiLogger* expected2 = default_ptr.load(std::memory_order_acquire);
                    if (!expected2) {
                        default_ptr.compare_exchange_strong(expected2, p2, std::memory_order_acq_rel);
                    } else if (expected2 != p2) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_release);
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    assert(!failed.load(std::memory_order_acquire));
    assert(stress_ptr.load(std::memory_order_acquire) != nullptr);
    assert(default_ptr.load(std::memory_order_acquire) != nullptr);
    assert(stress_ptr.load(std::memory_order_acquire) != default_ptr.load(std::memory_order_acquire));
}

TEST(di_route_binder_nested_router) {
    di::Container container;
    App app;

    di::bind_routes(app, container).router("/api", [](di::RouteBinder& r) {
        r.post<DiController>("/login", &DiController::login);
        r.router("/v1", [](di::RouteBinder& v1) {
            v1.get<DiController>("/profile", &DiController::profile);
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
    assert(has_route("GET", "/api/v1/profile"));
}

TEST(di_mount_controller_with_static_prefix_and_typed_map) {
    di::Container container;
    container.singleton<DiLogger>([] {
        auto logger = std::make_shared<DiLogger>();
        logger->level = 3;
        return logger;
    });

    RouteGroup api("/api");
    di::mount_controller<DiMountedController>(api, container);

    Router router = api.to_router();
    auto login = router.match("POST", "/api/users/login");
    auto profile = router.match("GET", "/api/users/v1/profile");
    assert(login.has_value());
    assert(profile.has_value());

    auto login_ctx = make_context("POST", "/api/users/login");
    async_simple::coro::syncAwait(login->handler(login_ctx));
    assert(login_ctx.response.status_code == 200);
    assert(login_ctx.response.body == "mounted-login");

    auto profile_ctx = make_context("GET", "/api/users/v1/profile");
    async_simple::coro::syncAwait(profile->handler(profile_ctx));
    assert(profile_ctx.response.status_code == 200);
    assert(profile_ctx.response.body == "mounted-profile");

    auto logger = container.resolve_shared<DiLogger>();
    assert(logger->level == 4);
}

TEST(di_mount_controller_prefix_override_and_key_fallback) {
    di::Container container;
    container.singleton<DiTenantController>([] {
        auto controller = std::make_shared<DiTenantController>();
        controller->tenant = "default-tenant";
        return controller;
    });
    container.singleton<DiTenantController>("auth", [] {
        auto controller = std::make_shared<DiTenantController>();
        controller->tenant = "auth-tenant";
        return controller;
    });

    RouteGroup api("/api");
    di::mount_controller<DiTenantController>(
        api, container, {.prefix = "/secure", .key = "auth"});
    di::mount_controller<DiTenantController>(
        api, container, {.prefix = "/fallback", .key = "missing"});

    Router router = api.to_router();
    auto auth = router.match("GET", "/api/secure/who");
    auto fallback = router.match("GET", "/api/fallback/who");
    assert(auth.has_value());
    assert(fallback.has_value());

    auto auth_ctx = make_context("GET", "/api/secure/who");
    async_simple::coro::syncAwait(auth->handler(auth_ctx));
    assert(auth_ctx.response.body == "auth-tenant");

    auto fallback_ctx = make_context("GET", "/api/fallback/who");
    async_simple::coro::syncAwait(fallback->handler(fallback_ctx));
    assert(fallback_ctx.response.body == "default-tenant");
}

TEST(di_mount_controller_app_overload_and_legacy_map) {
    di::Container container;
    App app;
    di::mount_controller<DiLegacyMappedController>(
        app, container, {.prefix = "api/legacy"});

    const auto routes = app.routes();
    auto has_route = [&](std::string_view method, std::string_view pattern) {
        for (const auto& route : routes) {
            if (route.method == method && route.pattern == pattern) {
                return true;
            }
        }
        return false;
    };

    assert(has_route("GET", "/api/legacy/ping"));
}

int main() {
    std::cout << "=== Layer 3 Integration Tests ===\n";
    
    RUN_TEST(app_basic_routing);
    RUN_TEST(router_static_routing);
    RUN_TEST(router_with_params);
    RUN_TEST(router_with_wildcard);
    RUN_TEST(context_json_operations);
    RUN_TEST(context_redirect_methods);
    RUN_TEST(context_attachment_headers);
    RUN_TEST(error_codes);
    RUN_TEST(error_exceptions);
    RUN_TEST(middleware_error_handler);
    RUN_TEST(middleware_error_handler_framework_error_shape);
    RUN_TEST(form_parser_integration);
    RUN_TEST(cors_basic);
    RUN_TEST(cors_credentials_with_wildcard_origin);
    RUN_TEST(cors_preflight);
    RUN_TEST(route_group_compile_path);
    RUN_TEST(app_router_dsl_nested);
    RUN_TEST(rate_limit_blocks_after_threshold);
    RUN_TEST(context_send_file_and_download);
    RUN_TEST(middleware_security_headers);
    RUN_TEST(middleware_etag);
    RUN_TEST(middleware_compress);
    RUN_TEST(di_container_reflect_injection);
    RUN_TEST(di_container_transient_lifetime);
    RUN_TEST(di_container_named_singleton_management);
    RUN_TEST(di_container_keyed_inject_and_fallback);
    RUN_TEST(di_container_resolve_ref_for_transient_throws);
    RUN_TEST(di_container_cycle_detection);
    RUN_TEST(di_global_and_tls_container_accessors);
    RUN_TEST(di_route_binder_controller_methods);
    RUN_TEST(di_route_binder_with_key_selects_named_controller);
    RUN_TEST(di_container_concurrent_singleton_stress);
    RUN_TEST(di_route_binder_nested_router);
    RUN_TEST(di_mount_controller_with_static_prefix_and_typed_map);
    RUN_TEST(di_mount_controller_prefix_override_and_key_fallback);
    RUN_TEST(di_mount_controller_app_overload_and_legacy_map);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
