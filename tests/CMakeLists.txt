if(NOT TARGET async_uv)
    message(FATAL_ERROR "tests require the async_uv target")
endif()

add_executable(async_uv_smoke_test smoke_test.cpp)
target_link_libraries(async_uv_smoke_test PRIVATE async_uv)

add_executable(async_uv_scope_test scope_test.cpp)
target_link_libraries(async_uv_scope_test PRIVATE async_uv)

if(BUILD_TESTING)
    add_test(NAME async_uv_smoke_test COMMAND async_uv_smoke_test)
    add_test(NAME async_uv_scope_test COMMAND async_uv_scope_test)
endif()
