#pragma once

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

inline std::optional<std::string> response_header(const Context& ctx, const std::string& name) {
    for (const auto& h : ctx.response.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return std::nullopt;
}

inline Context make_context(const std::string& method, const std::string& path,
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

struct OrmJsonDoc {
    static constexpr std::string_view table_name = "orm_json_docs";

    orm::AutoId<std::int64_t> id = 0;
    std::string biz_key;
    std::string payload;
};

} // namespace
