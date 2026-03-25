cmake_minimum_required(VERSION 3.24)

project(async_uv LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(ASYNC_UV_BUILD_EXAMPLES "Build async_uv examples" ${PROJECT_IS_TOP_LEVEL})
option(ASYNC_UV_BUILD_TESTS "Build async_uv tests" ${PROJECT_IS_TOP_LEVEL})
option(ASYNC_UV_USE_MIMALLOC "Link mimalloc into async_uv targets" OFF)

add_subdirectory(src)

if(ASYNC_UV_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ASYNC_UV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
