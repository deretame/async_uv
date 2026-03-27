if(NOT TARGET async_uv)
    message(FATAL_ERROR "tests require the async_uv target")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_test_dependencies.cmake)

get_target_property(ASYNC_UV_INCLUDE_DIRS async_uv INCLUDE_DIRECTORIES)

function(async_uv_expect_compile_fail result_var source_file failure_message)
    try_compile(
        ${result_var}
        "${CMAKE_CURRENT_BINARY_DIR}/${result_var}"
        "${source_file}"
        CMAKE_FLAGS
            "-DCMAKE_CXX_STANDARD=23"
            "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
            "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
            "-DINCLUDE_DIRECTORIES=${ASYNC_UV_INCLUDE_DIRS}")

    if(${result_var})
        message(FATAL_ERROR "${failure_message}")
    endif()
endfunction()

async_uv_expect_compile_fail(
    ASYNC_UV_MAILBOX_INT_REJECTED
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/mailbox_copyable_fail.cpp"
    "Mailbox<int> should be rejected by MoveOnlyMessage constraints")

async_uv_expect_compile_fail(
    ASYNC_UV_MAILBOX_STRING_REJECTED
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/mailbox_string_fail.cpp"
    "Mailbox<std::string> should be rejected by MoveOnlyMessage constraints")

async_uv_expect_compile_fail(
    ASYNC_UV_MAILBOX_SHARED_PTR_REJECTED
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/mailbox_shared_ptr_fail.cpp"
    "Mailbox<std::shared_ptr<int>> should be rejected by MoveOnlyMessage constraints")

add_executable(async_uv_smoke_test smoke_test.cpp)
target_link_libraries(async_uv_smoke_test PRIVATE async_uv)

add_executable(async_uv_scope_test scope_test.cpp)
target_link_libraries(async_uv_scope_test PRIVATE async_uv)

add_executable(async_uv_constraints_test constraints_test.cpp)
target_link_libraries(async_uv_constraints_test PRIVATE async_uv)

add_executable(async_uv_tls_bio_test tls_bio_test.cpp)
target_link_libraries(async_uv_tls_bio_test
    PRIVATE
        async_uv
        mbedtls
        mbedx509
        mbedcrypto)

if(TARGET CURL::libcurl)
    add_executable(async_uv_fd_curl_test fd_curl_test.cpp)
    target_link_libraries(async_uv_fd_curl_test PRIVATE async_uv CURL::libcurl)
endif()

if(BUILD_TESTING)
    add_test(NAME async_uv_smoke_test COMMAND async_uv_smoke_test)
    add_test(NAME async_uv_scope_test COMMAND async_uv_scope_test)
    add_test(NAME async_uv_constraints_test COMMAND async_uv_constraints_test)
    add_test(NAME async_uv_tls_bio_test COMMAND async_uv_tls_bio_test)
    if(TARGET CURL::libcurl)
        add_test(NAME async_uv_fd_curl_test COMMAND async_uv_fd_curl_test)
    endif()
endif()
