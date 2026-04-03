#include "integration_test_common.hpp"
#include "integration_test_decl.hpp"

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
    RUN_TEST(orm_transaction_shared_context_across_mappers);
    RUN_TEST(orm_transaction_requires_new_and_options);
    RUN_TEST(orm_sqlite_json_raw_sql_in_chain);
    RUN_TEST(orm_count_batch_and_raw_sql_policy);
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
