if(NOT TARGET flux)
    message(FATAL_ERROR "tests require the flux target")
endif()

add_executable(flux_smoke_test smoke_test.cpp)
target_link_libraries(flux_smoke_test PRIVATE flux)

add_executable(flux_scope_test scope_test.cpp)
target_link_libraries(flux_scope_test PRIVATE flux)

add_executable(flux_fs_watch_test fs_watch_test.cpp)
target_link_libraries(flux_fs_watch_test PRIVATE flux)

add_executable(flux_trace_test tls_bio_test.cpp)
target_link_libraries(flux_trace_test PRIVATE flux)

add_executable(flux_constraints_test constraints_test.cpp)
target_link_libraries(flux_constraints_test PRIVATE flux)

add_executable(flux_timer_test timer_test.cpp)
target_link_libraries(flux_timer_test PRIVATE flux)

add_executable(flux_message_bus_test message_bus_test.cpp)
target_link_libraries(flux_message_bus_test PRIVATE flux)

add_executable(flux_fd_redis_test fd_redis_test.cpp)
target_link_libraries(flux_fd_redis_test PRIVATE flux)

find_library(FLUX_CURL_LIBRARY
    NAMES
        curl
        libcurl
        libcurl.so.4
        curl.so.4
)
if(FLUX_CURL_LIBRARY)
    add_executable(flux_fd_curl_test fd_curl_test.cpp)
    target_link_libraries(flux_fd_curl_test PRIVATE flux ${FLUX_CURL_LIBRARY})
else()
    message(WARNING "libcurl library not found: flux_fd_curl_test will be skipped")
endif()

if(BUILD_TESTING)
    add_test(NAME flux_smoke_test COMMAND flux_smoke_test)
    add_test(NAME flux_scope_test COMMAND flux_scope_test)
    add_test(NAME flux_fs_watch_test COMMAND flux_fs_watch_test)
    add_test(NAME flux_trace_test COMMAND flux_trace_test)
    add_test(NAME flux_constraints_test COMMAND flux_constraints_test)
    add_test(NAME flux_timer_test COMMAND flux_timer_test)
    add_test(NAME flux_message_bus_test COMMAND flux_message_bus_test)
    add_test(NAME flux_fd_redis_test COMMAND flux_fd_redis_test)
    if(TARGET flux_fd_curl_test)
        add_test(NAME flux_fd_curl_test COMMAND flux_fd_curl_test)
    endif()
endif()
