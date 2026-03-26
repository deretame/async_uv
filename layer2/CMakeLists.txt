if(NOT TARGET async_uv)
    message(FATAL_ERROR "layer2 requires async_uv target")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_dependencies.cmake)

add_library(async_uv_http
    src/http.cpp
)

add_library(async_uv::http ALIAS async_uv_http)

target_include_directories(async_uv_http
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(async_uv_http
    PUBLIC
        async_uv
        CURL::libcurl
)

target_compile_features(async_uv_http PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(async_uv_http PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASYNC_UV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
