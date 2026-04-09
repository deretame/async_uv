cmake_minimum_required(VERSION 3.24)

project(flux LANGUAGES C CXX)

if(WIN32)
    message(FATAL_ERROR "flux now targets Linux-only builds")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(FLUX_BUILD_EXAMPLES "Build flux examples" OFF)
option(FLUX_BUILD_TESTS "Build flux tests" OFF)
option(FLUX_USE_MIMALLOC "Link mimalloc into flux targets" OFF)
option(FLUX_ENABLE_LAYER2 "Build layer2 HTTP wrapper on top of flux" OFF)
option(FLUX_ENABLE_LAYER3 "Build layer3 components (requires layer2)" OFF)

if(FLUX_ENABLE_LAYER3)
    set(FLUX_ENABLE_LAYER2 ON CACHE BOOL "Build layer2 HTTP wrapper on top of flux" FORCE)
endif()
if(FLUX_ENABLE_LAYER3)
    message(FATAL_ERROR "layer3 migration to flux is not implemented yet")
endif()

add_subdirectory(src)

if(FLUX_ENABLE_LAYER2)
    add_subdirectory(layer2)
endif()

if(FLUX_BUILD_EXAMPLES)
    message(FATAL_ERROR "examples are disabled during the layer1 stdexec+asio rewrite")
endif()

if(FLUX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
