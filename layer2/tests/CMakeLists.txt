include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

FetchContent_Declare(
    tinyxml2
    URL https://codeload.github.com/leethomason/tinyxml2/tar.gz/refs/tags/10.0.0
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(tinyxml2)

add_executable(async_uv_http_test http_test.cpp)
target_link_libraries(async_uv_http_test PRIVATE async_uv_http)
target_link_libraries(async_uv_http_test PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(async_uv_http_test PRIVATE tinyxml2::tinyxml2)

add_executable(async_uv_http_parser_test parser_test.cpp)
target_link_libraries(async_uv_http_parser_test PRIVATE async_uv_http)

add_executable(async_uv_http_server_test http_server_test.cpp)
target_link_libraries(async_uv_http_server_test PRIVATE async_uv_http)

add_executable(async_uv_sql_test sql_test.cpp)
target_link_libraries(async_uv_sql_test PRIVATE async_uv_sql)

add_executable(async_uv_redis_test redis_test.cpp)
target_link_libraries(async_uv_redis_test PRIVATE async_uv_redis)

add_executable(async_uv_ws_test ws_test.cpp)
target_link_libraries(async_uv_ws_test PRIVATE async_uv_ws)

if(BUILD_TESTING)
    add_test(NAME async_uv_http_test COMMAND async_uv_http_test)
    add_test(NAME async_uv_http_parser_test COMMAND async_uv_http_parser_test)
    add_test(NAME async_uv_http_server_test COMMAND async_uv_http_server_test)
    add_test(NAME async_uv_sql_test COMMAND async_uv_sql_test)
    add_test(NAME async_uv_redis_test COMMAND async_uv_redis_test)
    add_test(NAME async_uv_ws_test COMMAND async_uv_ws_test)
endif()
