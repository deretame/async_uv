cmake_minimum_required(VERSION 3.24)

project(async_uv LANGUAGES C CXX)

if(WIN32)
    message(FATAL_ERROR "async_uv now targets Linux-only builds")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(ASYNC_UV_BUILD_EXAMPLES "Build async_uv examples" OFF)
option(ASYNC_UV_BUILD_TESTS "Build async_uv tests" OFF)
option(ASYNC_UV_USE_MIMALLOC "Link mimalloc into async_uv targets" OFF)
option(ASYNC_UV_ENABLE_LAYER2 "Build layer2 HTTP wrapper on top of async_uv" OFF)
option(ASYNC_UV_ENABLE_LAYER3 "Build layer3 components (requires layer2)" OFF)

if(ASYNC_UV_ENABLE_LAYER3)
    set(ASYNC_UV_ENABLE_LAYER2 ON CACHE BOOL "Build layer2 HTTP wrapper on top of async_uv" FORCE)
endif()

if(ASYNC_UV_ENABLE_LAYER2 OR ASYNC_UV_ENABLE_LAYER3)
    message(FATAL_ERROR
        "layer2/layer3 are intentionally disabled during the layer1 stdexec+asio rewrite")
endif()

add_subdirectory(src)

if(ASYNC_UV_BUILD_EXAMPLES)
    message(FATAL_ERROR "examples are disabled during the layer1 stdexec+asio rewrite")
endif()

if(ASYNC_UV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
