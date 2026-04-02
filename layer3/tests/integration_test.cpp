#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <string>
#include <stdexcept>
#include <map>
#include <thread>
#include <vector>

#include <async_uv/async_uv.h>
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
#include <async_uv_layer3/orm.hpp>
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

struct OrmUser {
    static constexpr std::string_view table_name = "orm_users";

    orm::AutoId<std::int64_t> id = 0;
    std::string name;
    orm::Unique<std::string> email;
    std::string role;
    std::string status;
    int age = 0;
    std::int64_t create_time = 0;

    const std::string& role_ref() const {
        return role;
    }
};

struct OrmAuditLog {
    struct EpochMillisTimePointConverter {
        static async_uv::sql::SqlParam to_sql_param(const std::chrono::system_clock::time_point& value) {
            const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                value.time_since_epoch());
            return async_uv::sql::SqlParam(static_cast<std::int64_t>(millis.count()));
        }

        static void from_sql_text(
            std::string_view text,
            std::chrono::system_clock::time_point& out) {
            const auto millis = std::stoll(std::string(text));
            out = std::chrono::system_clock::time_point(std::chrono::milliseconds(millis));
        }

        static std::string_view sql_type() {
            return "INTEGER";
        }
    };

    struct EpochMillisDurationConverter {
        static async_uv::sql::SqlParam to_sql_param(const std::chrono::milliseconds& value) {
            return async_uv::sql::SqlParam(static_cast<std::int64_t>(value.count()));
        }

        static void from_sql_text(std::string_view text, std::chrono::milliseconds& out) {
            out = std::chrono::milliseconds(std::stoll(std::string(text)));
        }

        static std::string_view sql_type() {
            return "INTEGER";
        }
    };

    static constexpr std::string_view table_name = "orm_audit_logs";

    orm::AutoId<std::int64_t> id = 0;
    std::string action;
    orm::As<std::chrono::system_clock::time_point, EpochMillisTimePointConverter> created_at{};
    orm::As<std::chrono::milliseconds, EpochMillisDurationConverter> ttl{};
};

struct OrmAuthor {
    static constexpr std::string_view table_name = "orm_authors";

    orm::AutoId<std::int64_t> id = 0;
    std::string name;
};

struct OrmPost {
    static constexpr std::string_view table_name = "orm_posts";

    orm::AutoId<std::int64_t> id = 0;
    std::int64_t author_id = 0;
    std::string title;
};

struct OrmTag {
    static constexpr std::string_view table_name = "orm_tags";

    orm::AutoId<std::int64_t> id = 0;
    orm::Unique<std::string> name;
};

struct OrmPostTag {
    static constexpr std::string_view table_name = "orm_post_tags";

    orm::AutoId<std::int64_t> id = 0;
    std::int64_t post_id = 0;
    std::int64_t tag_id = 0;
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

TEST(orm_sqlite_chain_query_and_schema) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();

        co_await conn.open(options);

        orm::Mapper<OrmUser> mapper(conn);
        co_await mapper.sync_schema();

        OrmUser u1;
        u1.name = "zh_admin_ok";
        u1.email = "admin1@example.com";
        u1.role = "ADMIN";
        u1.status = "ACTIVE";
        u1.age = 30;
        u1.create_time = 100;

        OrmUser u2;
        u2.name = "zh_admin_deleted";
        u2.email = "admin2@example.com";
        u2.role = "ADMIN";
        u2.status = "DELETED";
        u2.age = 32;
        u2.create_time = 200;

        OrmUser u3;
        u3.name = "zh_user_ok";
        u3.email = "user1@example.com";
        u3.role = "USER";
        u3.status = "ACTIVE";
        u3.age = 26;
        u3.create_time = 300;

        const auto id1 = co_await mapper.insert(u1);
        const auto id2 = co_await mapper.insert(u2);
        const auto id3 = co_await mapper.insert(u3);
        assert(id1 > 0);
        assert(id2 > id1);
        assert(id3 > id2);

        auto users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .eq(&OrmUser::role, "ADMIN")
                .ne(&OrmUser::status, "DELETED")
                .like(&OrmUser::name, "zh")
                .gt(&OrmUser::age, 18)
                .orderByDesc(&OrmUser::create_time)
                .last("LIMIT 10"));

        assert(users.size() == 1);
        assert(users[0].name == "zh_admin_ok");
        assert(users[0].id.get() == static_cast<std::int64_t>(id1));

        auto by_id = co_await mapper.selectById(static_cast<std::int64_t>(id1));
        assert(by_id.has_value());
        assert(by_id->email.get() == "admin1@example.com");

        by_id->status = "UPDATED";
        const auto updated = co_await mapper.updateById(*by_id);
        assert(updated == 1);

        auto after_update = co_await mapper.selectById(static_cast<std::int64_t>(id1));
        assert(after_update.has_value());
        assert(after_update->status == "UPDATED");

        OrmUser dup;
        dup.name = "dup";
        dup.email = "admin1@example.com";
        dup.role = "ADMIN";
        dup.status = "ACTIVE";
        dup.age = 21;
        dup.create_time = 400;

        bool duplicate_blocked = false;
        try {
            (void)co_await mapper.insert(dup);
        } catch (const async_uv::sql::SqlError&) {
            duplicate_blocked = true;
        }
        assert(duplicate_blocked);

        OrmUser patch = *after_update;
        patch.age = 41;
        const auto upserted = co_await mapper.upsert(patch);
        assert(upserted == 1);

        auto after_upsert = co_await mapper.selectById(static_cast<std::int64_t>(id1));
        assert(after_upsert.has_value());
        assert(after_upsert->age == 41);

        const auto removed = co_await mapper.removeById(static_cast<std::int64_t>(id2));
        assert(removed == 1);
        auto deleted = co_await mapper.selectById(static_cast<std::int64_t>(id2));
        assert(!deleted.has_value());

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_test"));
    runtime.block_on(run());
}

TEST(orm_sqlite_custom_chrono_converter) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmAuditLog> mapper(conn);
        co_await mapper.sync_schema();

        auto pragma = co_await conn.query("PRAGMA table_info(\"orm_audit_logs\")");
        bool created_at_integer = false;
        bool ttl_integer = false;
        for (const auto& row : pragma.rows) {
            if (row.values.size() < 3 || !row.values[1] || !row.values[2]) {
                continue;
            }
            if (*row.values[1] == "created_at" && *row.values[2] == "INTEGER") {
                created_at_integer = true;
            }
            if (*row.values[1] == "ttl" && *row.values[2] == "INTEGER") {
                ttl_integer = true;
            }
        }
        assert(created_at_integer);
        assert(ttl_integer);

        const auto expected_ms = std::chrono::milliseconds(1710000000123LL);

        OrmAuditLog log;
        log.action = "BOOT";
        log.created_at = std::chrono::system_clock::time_point(expected_ms);
        log.ttl = std::chrono::minutes(5);
        const auto id = co_await mapper.insert(log);
        assert(id > 0);

        auto loaded = co_await mapper.selectById(static_cast<std::int64_t>(id));
        assert(loaded.has_value());
        assert(loaded->action == "BOOT");
        assert(std::chrono::duration_cast<std::chrono::milliseconds>(
            loaded->created_at.get().time_since_epoch()) == expected_ms);
        assert(loaded->ttl.get() == std::chrono::minutes(5));

        orm::As<std::chrono::system_clock::time_point, OrmAuditLog::EpochMillisTimePointConverter> threshold{
            std::chrono::system_clock::time_point(expected_ms)};
        auto recent = co_await mapper.selectList(
            orm::Query<OrmAuditLog>()
                .ge(&OrmAuditLog::created_at, threshold)
                .eq(&OrmAuditLog::action, "BOOT")
                .limit(1));
        assert(recent.size() == 1);
        assert(recent.front().action == "BOOT");

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_chrono_test"));
    runtime.block_on(run());
}

TEST(orm_sqlite_relations_one_many_many) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmAuthor> author_mapper(conn);
        orm::Mapper<OrmPost> post_mapper(conn);
        orm::Mapper<OrmTag> tag_mapper(conn);
        orm::Mapper<OrmPostTag> post_tag_mapper(conn);

        co_await author_mapper.sync_schema();
        co_await post_mapper.sync_schema();
        co_await tag_mapper.sync_schema();
        co_await post_tag_mapper.sync_schema();

        OrmAuthor alice;
        alice.name = "alice";
        const auto alice_id = static_cast<std::int64_t>(co_await author_mapper.insert(alice));

        OrmAuthor bob;
        bob.name = "bob";
        const auto bob_id = static_cast<std::int64_t>(co_await author_mapper.insert(bob));

        OrmPost p1;
        p1.author_id = alice_id;
        p1.title = "http-intro";
        const auto p1_id = static_cast<std::int64_t>(co_await post_mapper.insert(p1));

        OrmPost p2;
        p2.author_id = alice_id;
        p2.title = "orm-tips";
        const auto p2_id = static_cast<std::int64_t>(co_await post_mapper.insert(p2));

        OrmPost p3;
        p3.author_id = bob_id;
        p3.title = "router-notes";
        const auto p3_id = static_cast<std::int64_t>(co_await post_mapper.insert(p3));

        OrmPost orphan;
        orphan.author_id = 999999;
        orphan.title = "orphan-post";
        const auto orphan_id = static_cast<std::int64_t>(co_await post_mapper.insert(orphan));

        OrmTag t1;
        t1.name = "cpp";
        const auto t1_id = static_cast<std::int64_t>(co_await tag_mapper.insert(t1));

        OrmTag t2;
        t2.name = "database";
        const auto t2_id = static_cast<std::int64_t>(co_await tag_mapper.insert(t2));

        OrmTag t3;
        t3.name = "network";
        const auto t3_id = static_cast<std::int64_t>(co_await tag_mapper.insert(t3));

        OrmPostTag j1;
        j1.post_id = p1_id;
        j1.tag_id = t1_id;
        (void)co_await post_tag_mapper.insert(j1);

        OrmPostTag j2;
        j2.post_id = p1_id;
        j2.tag_id = t2_id;
        (void)co_await post_tag_mapper.insert(j2);

        OrmPostTag j3;
        j3.post_id = p2_id;
        j3.tag_id = t2_id;
        (void)co_await post_tag_mapper.insert(j3);

        OrmPostTag j4;
        j4.post_id = p3_id;
        j4.tag_id = t3_id;
        (void)co_await post_tag_mapper.insert(j4);

        auto authors = co_await author_mapper.selectList(
            orm::Query<OrmAuthor>().orderByAsc(&OrmAuthor::id));
        assert(authors.size() == 2);

        auto posts_by_author = co_await author_mapper.load_one_to_many(
            authors,
            &OrmAuthor::id,
            post_mapper,
            &OrmPost::author_id,
            orm::Query<OrmPost>().orderByAsc(&OrmPost::id));
        assert(posts_by_author.size() == 2);
        assert(posts_by_author[alice_id].size() == 2);
        assert(posts_by_author[alice_id][0].title == "http-intro");
        assert(posts_by_author[alice_id][1].title == "orm-tips");
        assert(posts_by_author[bob_id].size() == 1);
        assert(posts_by_author[bob_id][0].title == "router-notes");

        auto alice_posts = co_await author_mapper.loadOneToMany(
            authors[0],
            &OrmAuthor::id,
            post_mapper,
            &OrmPost::author_id,
            orm::Query<OrmPost>().orderByAsc(&OrmPost::id));
        assert(alice_posts.size() == 2);

        auto posts = co_await post_mapper.selectList(
            orm::Query<OrmPost>().orderByAsc(&OrmPost::id));
        assert(posts.size() == 4);

        auto author_by_foreign_key = co_await post_mapper.load_many_to_one(
            posts,
            &OrmPost::author_id,
            author_mapper,
            &OrmAuthor::id);
        assert(author_by_foreign_key.size() == 2);
        assert(author_by_foreign_key[alice_id].name == "alice");
        assert(author_by_foreign_key[bob_id].name == "bob");
        assert(author_by_foreign_key.find(999999) == author_by_foreign_key.end());

        auto one_parent = co_await post_mapper.loadManyToOne(
            posts[0],
            &OrmPost::author_id,
            author_mapper,
            &OrmAuthor::id);
        assert(one_parent.has_value());
        assert(one_parent->name == "alice");

        std::optional<OrmPost> orphan_post;
        for (const auto& post : posts) {
            if (post.id.get() == orphan_id) {
                orphan_post = post;
                break;
            }
        }
        assert(orphan_post.has_value());

        auto orphan_parent = co_await post_mapper.loadManyToOne(
            *orphan_post,
            &OrmPost::author_id,
            author_mapper,
            &OrmAuthor::id);
        assert(!orphan_parent.has_value());

        auto tags_by_post = co_await post_mapper.load_many_to_many(
            posts,
            &OrmPost::id,
            post_tag_mapper,
            &OrmPostTag::post_id,
            &OrmPostTag::tag_id,
            tag_mapper,
            &OrmTag::id,
            orm::Query<OrmTag>().orderByAsc(&OrmTag::id));
        assert(tags_by_post.size() == 4);
        assert(tags_by_post[p1_id].size() == 2);
        assert(tags_by_post[p1_id][0].name.get() == "cpp");
        assert(tags_by_post[p1_id][1].name.get() == "database");
        assert(tags_by_post[p2_id].size() == 1);
        assert(tags_by_post[p2_id][0].name.get() == "database");
        assert(tags_by_post[p3_id].size() == 1);
        assert(tags_by_post[p3_id][0].name.get() == "network");
        assert(tags_by_post[orphan_id].empty());

        auto post_two_tags = co_await post_mapper.loadManyToMany(
            posts[1],
            &OrmPost::id,
            post_tag_mapper,
            &OrmPostTag::post_id,
            &OrmPostTag::tag_id,
            tag_mapper,
            &OrmTag::id,
            orm::Query<OrmTag>().orderByAsc(&OrmTag::id));
        assert(post_two_tags.size() == 1);
        assert(post_two_tags[0].name.get() == "database");

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_relation_test"));
    runtime.block_on(run());
}

TEST(orm_sqlite_join_query) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmAuthor> author_mapper(conn);
        orm::Mapper<OrmPost> post_mapper(conn);
        co_await author_mapper.sync_schema();
        co_await post_mapper.sync_schema();

        OrmAuthor alice;
        alice.name = "alice";
        const auto alice_id = static_cast<std::int64_t>(co_await author_mapper.insert(alice));

        OrmAuthor bob;
        bob.name = "bob";
        const auto bob_id = static_cast<std::int64_t>(co_await author_mapper.insert(bob));

        OrmPost p1;
        p1.author_id = alice_id;
        p1.title = "alice-post-1";
        const auto p1_id = static_cast<std::int64_t>(co_await post_mapper.insert(p1));

        OrmPost p2;
        p2.author_id = alice_id;
        p2.title = "alice-post-2";
        (void)co_await post_mapper.insert(p2);

        OrmPost p3;
        p3.author_id = bob_id;
        p3.title = "bob-post-1";
        (void)co_await post_mapper.insert(p3);

        OrmPost orphan;
        orphan.author_id = 9999;
        orphan.title = "orphan-post";
        const auto orphan_id = static_cast<std::int64_t>(co_await post_mapper.insert(orphan));

        auto posts_of_alice = co_await post_mapper.selectList(
            orm::Query<OrmPost>()
                .join<OrmAuthor>(&OrmPost::author_id, &OrmAuthor::id, "a")
                .eq<OrmAuthor>(&OrmAuthor::name, "alice")
                .orderByAsc(&OrmPost::id));
        assert(posts_of_alice.size() == 2);
        assert(posts_of_alice[0].id.get() == p1_id);
        assert(posts_of_alice[0].title == "alice-post-1");
        assert(posts_of_alice[1].title == "alice-post-2");

        auto posts_of_bob = co_await post_mapper.selectList(
            orm::Query<OrmPost>()
                .join<OrmAuthor>(&OrmPost::author_id, &OrmAuthor::id)
                .eq<OrmAuthor>(&OrmAuthor::name, "bob")
                .orderByAsc(&OrmPost::id));
        assert(posts_of_bob.size() == 1);
        assert(posts_of_bob[0].title == "bob-post-1");

        auto orphan_posts = co_await post_mapper.selectList(
            orm::Query<OrmPost>()
                .leftJoin<OrmAuthor>(&OrmPost::author_id, &OrmAuthor::id)
                .isNull<OrmAuthor>(&OrmAuthor::id)
                .orderByAsc(&OrmPost::id));
        assert(orphan_posts.size() == 1);
        assert(orphan_posts.front().id.get() == orphan_id);
        assert(orphan_posts.front().title == "orphan-post");

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_join_test"));
    runtime.block_on(run());
}

TEST(orm_sqlite_advanced_chain_methods) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmUser> mapper(conn);
        co_await mapper.sync_schema();

        OrmUser u1;
        u1.name = "alice_admin";
        u1.email = "alice@example.com";
        u1.role = "ADMIN";
        u1.status = "ACTIVE";
        u1.age = 30;
        u1.create_time = 100;
        const auto id1 = static_cast<std::int64_t>(co_await mapper.insert(u1));

        OrmUser u2;
        u2.name = "bob_admin";
        u2.email = "bob@example.com";
        u2.role = "ADMIN";
        u2.status = "BANNED";
        u2.age = 22;
        u2.create_time = 200;
        const auto id2 = static_cast<std::int64_t>(co_await mapper.insert(u2));

        OrmUser u3;
        u3.name = "carol_admin";
        u3.email = "carol@example.com";
        u3.role = "ADMIN";
        u3.status = "ACTIVE";
        u3.age = 19;
        u3.create_time = 300;
        (void)co_await mapper.insert(u3);

        OrmUser u4;
        u4.name = "dave_user";
        u4.email = "dave@example.com";
        u4.role = "USER";
        u4.status = "DELETED";
        u4.age = 40;
        u4.create_time = 400;
        const auto id4 = static_cast<std::int64_t>(co_await mapper.insert(u4));

        auto between_users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .between(&OrmUser::age, 20, 35)
                .orderByAsc(&OrmUser::id));
        assert(between_users.size() == 2);
        assert(between_users[0].id.get() == id1);
        assert(between_users[1].id.get() == id2);

        auto not_like_users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .notLike(&OrmUser::name, "admin")
                .orderByAsc(&OrmUser::id));
        assert(not_like_users.size() == 1);
        assert(not_like_users[0].id.get() == id4);

        auto like_left_users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .likeLeft(&OrmUser::name, "admin")
                .orderByAsc(&OrmUser::id));
        assert(like_left_users.size() == 3);

        auto like_right_users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .likeRight(&OrmUser::name, "dave")
                .orderByAsc(&OrmUser::id));
        assert(like_right_users.size() == 1);
        assert(like_right_users[0].id.get() == id4);

        auto not_in_users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .notIn(&OrmUser::role, {"ADMIN"})
                .orderByAsc(&OrmUser::id));
        assert(not_in_users.size() == 1);
        assert(not_in_users[0].id.get() == id4);

        auto in_sql_users = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .inSql(&OrmUser::id, "SELECT id FROM orm_users WHERE role = 'ADMIN' AND age >= 30")
                .orderByAsc(&OrmUser::id));
        assert(in_sql_users.size() == 1);
        assert(in_sql_users[0].id.get() == id1);

        auto grouped = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .groupBy(&OrmUser::role)
                .having("COUNT(*) >= ?", {async_uv::sql::SqlParam(2)}));
        assert(grouped.size() == 1);
        assert(grouped[0].role == "ADMIN");

        auto logic_and_or = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .eq(&OrmUser::role, "ADMIN")
                .and_([](auto& q) {
                    q.ge(&OrmUser::age, 30).or_().eq(&OrmUser::status, "BANNED");
                })
                .orderByAsc(&OrmUser::id));
        assert(logic_and_or.size() == 2);
        assert(logic_and_or[0].id.get() == id1);
        assert(logic_and_or[1].id.get() == id2);

        auto logic_nested = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .eq(&OrmUser::role, "USER")
                .or_()
                .nested([](auto& q) {
                    q.eq(&OrmUser::role, "ADMIN").eq(&OrmUser::status, "BANNED");
                })
                .orderByAsc(&OrmUser::id));
        assert(logic_nested.size() == 2);
        assert(logic_nested[0].id.get() == id2);
        assert(logic_nested[1].id.get() == id4);

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_advanced_chain_test"));
    runtime.block_on(run());
}

TEST(orm_service_chain_query_update_count) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmUser> mapper(conn);
        co_await mapper.sync_schema();
        orm::Service<OrmUser> service(mapper);

        OrmUser u1;
        u1.name = "service_a";
        u1.email = "service_a@example.com";
        u1.role = "ADMIN";
        u1.status = "ACTIVE";
        u1.age = 21;
        u1.create_time = 10;
        (void)co_await mapper.insert(u1);

        OrmUser u2;
        u2.name = "service_b";
        u2.email = "service_b@example.com";
        u2.role = "USER";
        u2.status = "ACTIVE";
        u2.age = 18;
        u2.create_time = 20;
        (void)co_await mapper.insert(u2);

        OrmUser u3;
        u3.name = "service_c";
        u3.email = "service_c@example.com";
        u3.role = "ADMIN";
        u3.status = "INACTIVE";
        u3.age = 42;
        u3.create_time = 30;
        (void)co_await mapper.insert(u3);

        auto admins = co_await service.query()
                          .eq(&OrmUser::role, "ADMIN")
                          .orderByAsc(&OrmUser::id)
                          .list();
        assert(admins.size() == 2);
        assert(admins[0].name == "service_a");
        assert(admins[1].name == "service_c");

        const auto active_count = co_await service.query()
                                      .eq(&OrmUser::status, "ACTIVE")
                                      .count();
        assert(active_count == 2);

        const auto updated = co_await service.update()
                                 .set(&OrmUser::status, "LOCKED")
                                 .eq(&OrmUser::role, "ADMIN")
                                 .execute();
        assert(updated == 2);

        auto locked_admins = co_await service.query()
                                 .eq(&OrmUser::role, "ADMIN")
                                 .eq(&OrmUser::status, "LOCKED")
                                 .list();
        assert(locked_admins.size() == 2);

        auto still_active_user = co_await service.query()
                                     .eq(&OrmUser::role, "USER")
                                     .eq(&OrmUser::status, "ACTIVE")
                                     .one();
        assert(still_active_user.has_value());
        assert(still_active_user->name == "service_b");

        const auto sql_updated = co_await service.update()
                                    .setSql(&OrmUser::age, "\"age\" + ?", {async_uv::sql::SqlParam(5)})
                                    .eq(&OrmUser::name, "service_b")
                                    .execute();
        assert(sql_updated == 1);

        auto age_bumped = co_await service.query()
                              .eq(&OrmUser::name, "service_b")
                              .one();
        assert(age_bumped.has_value());
        assert(age_bumped->age == 23);

        const auto absent_updated = co_await service.update()
                                       .setIfAbsent(&OrmUser::status, "SHOULD_NOT_APPLY")
                                       .set(&OrmUser::status, "FINAL")
                                       .setIfAbsent(&OrmUser::status, "IGNORED")
                                       .eq(&OrmUser::name, "service_c")
                                       .execute();
        assert(absent_updated == 1);

        auto final_status = co_await service.query()
                                .eq(&OrmUser::name, "service_c")
                                .one();
        assert(final_status.has_value());
        assert(final_status->status == "FINAL");

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_service_chain_test"));
    runtime.block_on(run());
}

TEST(orm_service_nested_transactions) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmUser> mapper(conn);
        co_await mapper.sync_schema();
        orm::Service<OrmUser> service(mapper);

        auto count_all = [&]() -> async_uv::Task<std::uint64_t> {
            co_return co_await service.query().count();
        };

        co_await service.tx([&](auto& tx) -> async_uv::Task<void> {
            OrmUser u;
            u.name = "tx_commit";
            u.email = "tx_commit@example.com";
            u.role = "ADMIN";
            u.status = "ACTIVE";
            u.age = 30;
            u.create_time = 1;
            (void)co_await tx.insert(u);
            co_return;
        });
        assert(co_await count_all() == 1);

        bool rolled_back = false;
        try {
            co_await service.tx([&](auto& tx) -> async_uv::Task<void> {
                OrmUser u;
                u.name = "tx_rollback";
                u.email = "tx_rollback@example.com";
                u.role = "ADMIN";
                u.status = "ACTIVE";
                u.age = 31;
                u.create_time = 2;
                (void)co_await tx.insert(u);
                throw std::runtime_error("boom");
                co_return;
            });
        } catch (const std::runtime_error&) {
            rolled_back = true;
        }
        assert(rolled_back);
        assert(co_await count_all() == 1);

        co_await service.tx([&](auto& outer) -> async_uv::Task<void> {
            OrmUser u1;
            u1.name = "outer_keep_1";
            u1.email = "outer_keep_1@example.com";
            u1.role = "USER";
            u1.status = "ACTIVE";
            u1.age = 20;
            u1.create_time = 3;
            (void)co_await outer.insert(u1);

            bool inner_failed = false;
            try {
                co_await outer.tx(
                    {.propagation = orm::TxPropagation::nested},
                    [&](auto& nested) -> async_uv::Task<void> {
                        OrmUser in;
                        in.name = "inner_drop";
                        in.email = "inner_drop@example.com";
                        in.role = "USER";
                        in.status = "ACTIVE";
                        in.age = 21;
                        in.create_time = 4;
                        (void)co_await nested.insert(in);
                        throw std::runtime_error("inner fail");
                        co_return;
                    });
            } catch (const std::runtime_error&) {
                inner_failed = true;
            }
            assert(inner_failed);

            OrmUser u2;
            u2.name = "outer_keep_2";
            u2.email = "outer_keep_2@example.com";
            u2.role = "USER";
            u2.status = "ACTIVE";
            u2.age = 22;
            u2.create_time = 5;
            (void)co_await outer.insert(u2);

            co_return;
        });
        assert(co_await count_all() == 3);

        bool required_failed = false;
        try {
            co_await service.tx([&](auto& outer) -> async_uv::Task<void> {
                OrmUser u;
                u.name = "required_should_rollback";
                u.email = "required_should_rollback@example.com";
                u.role = "ADMIN";
                u.status = "ACTIVE";
                u.age = 40;
                u.create_time = 6;
                (void)co_await outer.insert(u);

                bool inner_failed = false;
                try {
                    co_await outer.tx([&](auto&) -> async_uv::Task<void> {
                        throw std::runtime_error("required fail");
                        co_return;
                    });
                } catch (const std::runtime_error&) {
                    inner_failed = true;
                }
                assert(inner_failed);
                co_return;
            });
        } catch (const orm::Error&) {
            required_failed = true;
        }
        assert(required_failed);
        assert(co_await count_all() == 3);

        bool manual_rollback_only = false;
        try {
            co_await service.tx([&](auto& tx) -> async_uv::Task<void> {
                OrmUser u;
                u.name = "manual_rollback";
                u.email = "manual_rollback@example.com";
                u.role = "ADMIN";
                u.status = "ACTIVE";
                u.age = 50;
                u.create_time = 7;
                (void)co_await tx.insert(u);
                tx.set_rollback_only();
                co_return;
            });
        } catch (const orm::Error&) {
            manual_rollback_only = true;
        }
        assert(manual_rollback_only);
        assert(co_await count_all() == 3);

        co_await service.tx(
            {.propagation = orm::TxPropagation::nested},
            [&](auto& tx) -> async_uv::Task<void> {
                OrmUser u;
                u.name = "nested_without_outer";
                u.email = "nested_without_outer@example.com";
                u.role = "ADMIN";
                u.status = "ACTIVE";
                u.age = 35;
                u.create_time = 8;
                (void)co_await tx.insert(u);
                co_return;
            });
        assert(co_await count_all() == 4);

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_tx_nested_test"));
    runtime.block_on(run());
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
    RUN_TEST(orm_sqlite_chain_query_and_schema);
    RUN_TEST(orm_sqlite_custom_chrono_converter);
    RUN_TEST(orm_sqlite_relations_one_many_many);
    RUN_TEST(orm_sqlite_join_query);
    RUN_TEST(orm_sqlite_advanced_chain_methods);
    RUN_TEST(orm_service_chain_query_update_count);
    RUN_TEST(orm_service_nested_transactions);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
