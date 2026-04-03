#include "integration_test_common.hpp"

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

TEST(orm_transaction_shared_context_across_mappers) {
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

        bool outer_failed = false;
        try {
            co_await author_mapper.tx([&](auto&) -> async_uv::Task<void> {
                OrmAuthor a1;
                a1.name = "outer_author_1";
                (void)co_await author_mapper.insert(a1);

                bool inner_failed = false;
                try {
                    co_await post_mapper.tx([&](auto&) -> async_uv::Task<void> {
                        OrmPost p;
                        p.author_id = 1;
                        p.title = "inner_post_should_rollback";
                        (void)co_await post_mapper.insert(p);
                        throw std::runtime_error("inner required fail");
                        co_return;
                    });
                } catch (const std::runtime_error&) {
                    inner_failed = true;
                }
                assert(inner_failed);

                OrmAuthor a2;
                a2.name = "outer_author_2";
                (void)co_await author_mapper.insert(a2);
                co_return;
            });
        } catch (const orm::Error&) {
            outer_failed = true;
        }
        assert(outer_failed);

        const auto author_count = co_await author_mapper.count(orm::Query<OrmAuthor>());
        const auto post_count = co_await post_mapper.count(orm::Query<OrmPost>());
        assert(author_count == 0);
        assert(post_count == 0);

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_cross_mapper_tx_test"));
    runtime.block_on(run());
}

TEST(orm_transaction_requires_new_and_options) {
    auto run = []() -> async_uv::Task<void> {
        const auto db_path = std::filesystem::path("/tmp/async_uv_layer3_orm_requires_new.sqlite");
        std::error_code rm_ec;
        std::filesystem::remove(db_path, rm_ec);

        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(db_path.string())
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmUser> mapper(conn);
        co_await mapper.sync_schema();

        bool outer_failed = false;
        try {
            co_await mapper.tx([&](auto& outer) -> async_uv::Task<void> {
                co_await outer.tx(
                    {.propagation = orm::TxPropagation::requires_new},
                    [&](auto& isolated) -> async_uv::Task<void> {
                        OrmUser u2;
                        u2.name = "requires_new_should_commit";
                        u2.email = "requires_new_should_commit@example.com";
                        u2.role = "ADMIN";
                        u2.status = "ACTIVE";
                        u2.age = 30;
                        u2.create_time = 2;
                        (void)co_await isolated.insert(u2);
                        co_return;
                    });

                // sqlite 下外层已写入时会持有写锁，内层 requires_new 写入可能被锁阻塞；
                // 先做 requires_new，再做外层写入，更稳定地验证“内层提交不受外层回滚影响”。
                OrmUser u1;
                u1.name = "outer_should_rollback";
                u1.email = "outer_should_rollback@example.com";
                u1.role = "USER";
                u1.status = "ACTIVE";
                u1.age = 20;
                u1.create_time = 1;
                (void)co_await outer.insert(u1);

                throw std::runtime_error("rollback outer");
                co_return;
            });
        } catch (const std::runtime_error&) {
            outer_failed = true;
        }
        assert(outer_failed);

        auto all_users = co_await mapper.selectList(orm::Query<OrmUser>().orderByAsc(&OrmUser::id));
        assert(all_users.size() == 1);
        assert(all_users.front().name == "requires_new_should_commit");

        bool read_only_blocked_write = false;
        co_await mapper.tx(
            {.propagation = orm::TxPropagation::requires_new,
             .isolation = orm::TxIsolation::read_uncommitted,
             .read_only = true},
            [&](auto& read_only_tx) -> async_uv::Task<void> {
                const auto visible = co_await read_only_tx.count(orm::Query<OrmUser>());
                assert(visible == 1);

                OrmUser write_attempt;
                write_attempt.name = "should_fail";
                write_attempt.email = "should_fail@example.com";
                write_attempt.role = "USER";
                write_attempt.status = "ACTIVE";
                write_attempt.age = 10;
                write_attempt.create_time = 3;

                try {
                    (void)co_await read_only_tx.insert(write_attempt);
                } catch (const async_uv::sql::SqlError&) {
                    read_only_blocked_write = true;
                }
                co_return;
            });
        assert(read_only_blocked_write);

        bool timeout_raised = false;
        try {
            co_await mapper.tx(
                {.propagation = orm::TxPropagation::requires_new, .timeout_ms = 5},
                [&](auto&) -> async_uv::Task<void> {
                    co_await async_uv::sleep_for(std::chrono::milliseconds(30));
                    co_return;
                });
        } catch (const orm::Error& ex) {
            timeout_raised = std::string(ex.what()).find("timeout") != std::string::npos;
        }
        assert(timeout_raised);

        const auto final_count = co_await mapper.count(orm::Query<OrmUser>());
        assert(final_count == 1);

        co_await conn.close();

        std::error_code cleanup_ec;
        std::filesystem::remove(db_path, cleanup_ec);
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_requires_new_tx_test"));
    runtime.block_on(run());
}

TEST(orm_sqlite_json_raw_sql_in_chain) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmJsonDoc> mapper(conn);
        co_await mapper.sync_schema();

        OrmJsonDoc d1;
        d1.biz_key = "boot_prod";
        d1.payload = R"({"level":"INFO","count":1,"tags":["boot","api"],"meta":{"env":"prod"}})";
        const auto id1 = static_cast<std::int64_t>(co_await mapper.insert(d1));

        OrmJsonDoc d2;
        d2.biz_key = "alert_prod";
        d2.payload = R"({"level":"ERROR","count":3,"tags":["alert"],"meta":{"env":"prod"}})";
        const auto id2 = static_cast<std::int64_t>(co_await mapper.insert(d2));

        OrmJsonDoc d3;
        d3.biz_key = "boot_dev";
        d3.payload = R"({"level":"INFO","count":2,"tags":["boot","debug"],"meta":{"env":"dev"}})";
        const auto id3 = static_cast<std::int64_t>(co_await mapper.insert(d3));

        // 1) 链式查询 + 原生 SQL: 使用 json_extract 按 JSON 字段过滤
        auto prod_info_docs = co_await mapper.query()
                                 .whereRaw(
                                     "json_extract(\"payload\", '$.level') = ? "
                                     "AND json_extract(\"payload\", '$.meta.env') = ?",
                                     std::vector<async_uv::sql::SqlParam>{
                                         async_uv::sql::SqlParam("INFO"),
                                         async_uv::sql::SqlParam("prod")})
                                 .orderByAsc(&OrmJsonDoc::id)
                                 .list();
        assert(prod_info_docs.size() == 1);
        assert(prod_info_docs.front().id.get() == id1);

        // 2) 链式查询 + 原生 SQL: 使用 json_each 检查数组里是否包含某标签
        auto has_boot_tag = co_await mapper.query()
                                .whereRaw(
                                    "EXISTS ("
                                    "SELECT 1 FROM json_each(\"payload\", '$.tags') AS t "
                                    "WHERE t.value = ?)",
                                    std::vector<async_uv::sql::SqlParam>{
                                        async_uv::sql::SqlParam("boot")})
                                .orderByAsc(&OrmJsonDoc::id)
                                .list();
        assert(has_boot_tag.size() == 2);
        assert(has_boot_tag[0].id.get() == id1);
        assert(has_boot_tag[1].id.get() == id3);

        // 3) 链式更新 + 原生 SQL: setSql 直接调用 json_set 做原地更新
        const auto bumped = co_await mapper.update()
                                .setSql(
                                    &OrmJsonDoc::payload,
                                    "json_set("
                                    "\"payload\", '$.count', "
                                    "COALESCE(CAST(json_extract(\"payload\", '$.count') AS INTEGER), 0) + ?)",
                                    {async_uv::sql::SqlParam(10)})
                                .whereRaw("json_extract(\"payload\", '$.level') = ?",
                                          std::vector<async_uv::sql::SqlParam>{
                                              async_uv::sql::SqlParam("INFO")})
                                .execute();
        assert(bumped == 2);

        auto count_ge_11 = co_await mapper.query()
                               .whereRaw(
                                   "CAST(json_extract(\"payload\", '$.count') AS INTEGER) >= ?",
                                   std::vector<async_uv::sql::SqlParam>{
                                       async_uv::sql::SqlParam(11)})
                               .orderByAsc(&OrmJsonDoc::id)
                               .list();
        assert(count_ge_11.size() == 2);
        assert(count_ge_11[0].id.get() == id1);
        assert(count_ge_11[1].id.get() == id3);

        auto untouched_error = co_await mapper.query()
                                  .whereRaw(
                                      "json_extract(\"payload\", '$.level') = ? "
                                      "AND CAST(json_extract(\"payload\", '$.count') AS INTEGER) = ?",
                                      std::vector<async_uv::sql::SqlParam>{
                                          async_uv::sql::SqlParam("ERROR"),
                                          async_uv::sql::SqlParam(3)})
                                  .list();
        assert(untouched_error.size() == 1);
        assert(untouched_error.front().id.get() == id2);

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_json_raw_sql_test"));
    runtime.block_on(run());
}

TEST(orm_count_batch_and_raw_sql_policy) {
    auto run = []() -> async_uv::Task<void> {
        async_uv::sql::Connection conn;
        auto options = async_uv::sql::ConnectionOptions::builder()
                           .driver(async_uv::sql::Driver::sqlite)
                           .file(":memory:")
                           .build();
        co_await conn.open(options);

        orm::Mapper<OrmUser> mapper(conn);
        orm::Service<OrmUser> service(mapper);
        co_await mapper.sync_schema();

        orm::List<OrmUser> seed;
        for (int i = 0; i < 5; ++i) {
            OrmUser u;
            u.name = "batch_user_" + std::to_string(i);
            u.email = "batch_user_" + std::to_string(i) + "@example.com";
            u.role = (i % 2 == 0) ? "ADMIN" : "USER";
            u.status = (i == 4) ? "INACTIVE" : "ACTIVE";
            u.age = 20 + i;
            u.create_time = 100 + i;
            seed.push_back(std::move(u));
        }

        const auto inserted = co_await mapper.batchInsert(seed);
        assert(inserted == seed.size());

        orm::Query<OrmUser> window_q;
        window_q.eq(&OrmUser::status, "ACTIVE")
            .orderByDesc(&OrmUser::id)
            .limit(2)
            .offset(1);

        const auto filtered_count = co_await mapper.count(window_q);
        const auto filtered_count_alias = co_await mapper.countFiltered(window_q);
        const auto window_count = co_await mapper.countWindow(window_q);
        assert(filtered_count == 4);
        assert(filtered_count_alias == 4);
        assert(window_count == 2);

        const auto chain_filtered = co_await service.query()
                                        .eq(&OrmUser::status, "ACTIVE")
                                        .orderByDesc(&OrmUser::id)
                                        .limit(2)
                                        .offset(1)
                                        .count();
        const auto chain_window = co_await service.query()
                                      .eq(&OrmUser::status, "ACTIVE")
                                      .orderByDesc(&OrmUser::id)
                                      .limit(2)
                                      .offset(1)
                                      .countWindow();
        assert(chain_filtered == 4);
        assert(chain_window == 2);

        auto before_upsert = co_await mapper.selectOne(
            orm::Query<OrmUser>().eq(&OrmUser::email, "batch_user_1@example.com"));
        assert(before_upsert.has_value());

        OrmUser up_existing;
        up_existing.id = before_upsert->id.get();
        up_existing.name = "batch_user_1_renamed";
        up_existing.email = "batch_user_1@example.com";
        up_existing.role = "ADMIN";
        up_existing.status = "LOCKED";
        up_existing.age = 77;
        up_existing.create_time = 201;

        OrmUser up_new;
        up_new.name = "batch_user_new";
        up_new.email = "batch_user_new@example.com";
        up_new.role = "USER";
        up_new.status = "ACTIVE";
        up_new.age = 26;
        up_new.create_time = 202;

        const auto upsert_affected = co_await mapper.batchUpsert(
            orm::List<OrmUser>{up_existing, up_new});
        assert(upsert_affected >= 1);

        auto updated_existing = co_await mapper.selectOne(
            orm::Query<OrmUser>().eq(&OrmUser::email, "batch_user_1@example.com"));
        assert(updated_existing.has_value());
        assert(updated_existing->status == "LOCKED");
        assert(updated_existing->age == 77);

        auto update_targets = co_await mapper.selectList(
            orm::Query<OrmUser>()
                .eq(&OrmUser::status, "ACTIVE")
                .orderByAsc(&OrmUser::id)
                .limit(2));
        assert(update_targets.size() == 2);
        for (auto& u : update_targets) {
            u.status = "BATCHED";
        }
        const auto updated_by_id = co_await mapper.batchUpdateById(update_targets);
        assert(updated_by_id == update_targets.size());

        const auto batched_count = co_await mapper.count(
            orm::Query<OrmUser>().eq(&OrmUser::status, "BATCHED"));
        assert(batched_count == 2);

        const auto deleted = co_await mapper.deleteByQuery(
            orm::Query<OrmUser>().eq(&OrmUser::status, "BATCHED"));
        assert(deleted == 2);

        bool blocked_full_delete = false;
        try {
            (void)co_await mapper.deleteByQuery(orm::Query<OrmUser>{});
        } catch (const orm::Error&) {
            blocked_full_delete = true;
        }
        assert(blocked_full_delete);

        int audit_calls = 0;
        bool saw_marked = false;
        orm::set_raw_sql_audit_hook([&](std::string_view api, std::string_view sql, bool unsafe_marked) {
            ++audit_calls;
            assert(!api.empty());
            assert(!sql.empty());
            if (unsafe_marked) {
                saw_marked = true;
            }
        });
        orm::set_raw_sql_policy(orm::RawSqlPolicy::require_unsafe_marker);

        bool blocked_unmarked_raw = false;
        try {
            (void)co_await mapper.query().whereRaw("1 = 1").list();
        } catch (const orm::Error& ex) {
            blocked_unmarked_raw = std::string(ex.what()).find("unsafe marker") != std::string::npos;
        }
        assert(blocked_unmarked_raw);

        const auto raw_where_count = co_await mapper.query()
                                     .whereRaw(orm::unsafe_sql("1 = 1"))
                                     .count();
        assert(raw_where_count >= 1);

        const auto raw_in_sql_count = co_await mapper.query()
                                      .inSql(&OrmUser::id, orm::unsafe_sql("SELECT id FROM orm_users"))
                                      .count();
        assert(raw_in_sql_count >= 1);

        const auto raw_set_sql = co_await mapper.update()
                                   .setSql(&OrmUser::age, orm::unsafe_sql("\"age\" + ?"),
                                           {async_uv::sql::SqlParam(1)})
                                   .whereRaw(orm::unsafe_sql("1 = 1"))
                                   .execute();
        assert(raw_set_sql >= 1);
        assert(audit_calls >= 3);
        assert(saw_marked);

        orm::set_raw_sql_policy(orm::RawSqlPolicy::permissive);
        orm::clear_raw_sql_audit_hook();

        co_await conn.close();
        co_return;
    };

    async_uv::Runtime runtime(async_uv::Runtime::build().name("layer3_orm_count_batch_raw_policy_test"));
    runtime.block_on(run());
}
