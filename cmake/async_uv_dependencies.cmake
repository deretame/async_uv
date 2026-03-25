include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

if(NOT TARGET async_simple_headers)
    FetchContent_Declare(
        async_simple_src
        GIT_REPOSITORY https://github.com/alibaba/async_simple.git
        GIT_TAG be69039baea62ef376a396aa8224d1ab353685f8
        GIT_SHALLOW FALSE
        UPDATE_DISCONNECTED TRUE
    )

    FetchContent_GetProperties(async_simple_src)
    if(NOT async_simple_src_POPULATED)
        FetchContent_Populate(async_simple_src)
    endif()

    add_library(async_simple_headers INTERFACE)
    target_include_directories(async_simple_headers SYSTEM INTERFACE ${async_simple_src_SOURCE_DIR})
endif()

if(NOT TARGET libuv::libuv)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        libuv
        GIT_REPOSITORY https://github.com/libuv/libuv.git
        GIT_TAG v1.x
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
    )

    FetchContent_MakeAvailable(libuv)
endif()

if(ASYNC_UV_USE_MIMALLOC)
    if(NOT TARGET mimalloc-static AND NOT TARGET mimalloc)
        message(STATUS "Fetching mimalloc")
        set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
        set(MI_OVERRIDE ON CACHE BOOL "" FORCE)

        FetchContent_Declare(
            mimalloc
            GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
            GIT_TAG v3.1.5
            GIT_SHALLOW TRUE
            UPDATE_DISCONNECTED TRUE
        )

        FetchContent_MakeAvailable(mimalloc)
    endif()
endif()
