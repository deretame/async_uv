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

if(BUILD_TESTING)
    add_test(NAME async_uv_http_test COMMAND async_uv_http_test)
endif()
