#include "integration_test_common.hpp"

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
