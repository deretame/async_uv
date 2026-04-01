if(NOT TARGET async_uv_http)
    message(FATAL_ERROR "layer3 requires async_uv_http target from layer2")
endif()

option(ASYNC_UV_LAYER3_FETCH_REFLECT_CPP "Fetch reflect-cpp from source when unavailable" ON)

if(NOT TARGET reflectcpp::reflectcpp)
    if(NOT ASYNC_UV_LAYER3_FETCH_REFLECT_CPP)
        message(FATAL_ERROR "layer3 requires reflect-cpp. Install it or set ASYNC_UV_LAYER3_FETCH_REFLECT_CPP=ON")
    endif()

    FetchContent_Declare(
        reflect_cpp
        URL https://codeload.github.com/getml/reflect-cpp/tar.gz/refs/tags/v0.24.0
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(reflect_cpp)

    if(NOT TARGET reflectcpp::reflectcpp)
        message(FATAL_ERROR "Failed to provide reflectcpp::reflectcpp target")
    endif()
endif()

add_library(async_uv_layer3
    src/router.cpp
    src/app.cpp
)

add_library(async_uv::layer3 ALIAS async_uv_layer3)

target_include_directories(async_uv_layer3
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(async_uv_layer3
    PUBLIC
        async_uv_http
        reflectcpp::reflectcpp
)

target_compile_features(async_uv_layer3 PUBLIC cxx_std_23)

# Tests will be added later
# if(ASYNC_UV_BUILD_TESTS)
#     add_executable(layer3_router_test tests/router_test.cpp)
#     target_link_libraries(layer3_router_test PRIVATE async_uv_layer3)
# endif()