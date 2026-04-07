if(NOT TARGET async_uv)
    message(FATAL_ERROR "tests require the async_uv target")
endif()

add_executable(async_uv_smoke_test smoke_test.cpp)
target_link_libraries(async_uv_smoke_test PRIVATE async_uv)

add_executable(async_uv_scope_test scope_test.cpp)
target_link_libraries(async_uv_scope_test PRIVATE async_uv)

add_executable(async_uv_fs_watch_test fs_watch_test.cpp)
target_link_libraries(async_uv_fs_watch_test PRIVATE async_uv)

add_executable(async_uv_trace_test tls_bio_test.cpp)
target_link_libraries(async_uv_trace_test PRIVATE async_uv)

add_executable(async_uv_constraints_test constraints_test.cpp)
target_link_libraries(async_uv_constraints_test PRIVATE async_uv)

add_executable(async_uv_timer_test timer_test.cpp)
target_link_libraries(async_uv_timer_test PRIVATE async_uv)

add_executable(async_uv_message_bus_test message_bus_test.cpp)
target_link_libraries(async_uv_message_bus_test PRIVATE async_uv)

add_executable(async_uv_fd_redis_test fd_redis_test.cpp)
target_link_libraries(async_uv_fd_redis_test PRIVATE async_uv)

find_library(ASYNC_UV_CURL_LIBRARY
    NAMES
        curl
        libcurl
        libcurl.so.4
        curl.so.4
)
if(ASYNC_UV_CURL_LIBRARY)
    add_executable(async_uv_fd_curl_test fd_curl_test.cpp)
    target_link_libraries(async_uv_fd_curl_test PRIVATE async_uv ${ASYNC_UV_CURL_LIBRARY})
else()
    message(WARNING "libcurl library not found: async_uv_fd_curl_test will be skipped")
endif()

if(BUILD_TESTING)
    add_test(NAME async_uv_smoke_test COMMAND async_uv_smoke_test)
    add_test(NAME async_uv_scope_test COMMAND async_uv_scope_test)
    add_test(NAME async_uv_fs_watch_test COMMAND async_uv_fs_watch_test)
    add_test(NAME async_uv_trace_test COMMAND async_uv_trace_test)
    add_test(NAME async_uv_constraints_test COMMAND async_uv_constraints_test)
    add_test(NAME async_uv_timer_test COMMAND async_uv_timer_test)
    add_test(NAME async_uv_message_bus_test COMMAND async_uv_message_bus_test)
    add_test(NAME async_uv_fd_redis_test COMMAND async_uv_fd_redis_test)
    if(TARGET async_uv_fd_curl_test)
        add_test(NAME async_uv_fd_curl_test COMMAND async_uv_fd_curl_test)
    endif()
endif()
