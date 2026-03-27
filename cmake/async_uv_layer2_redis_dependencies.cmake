include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(ASYNC_UV_LAYER2_FETCH_REDIS "Fetch hiredis from source for Redis layer" ON)

set(ASYNC_UV_REDIS_HAS_HIREDIS OFF)
set(ASYNC_UV_REDIS_HAS_HIREDIS_SSL OFF)

if(ASYNC_UV_LAYER2_FETCH_REDIS)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        set(ENABLE_SSL ON CACHE BOOL "" FORCE)
    else()
        message(WARNING "OpenSSL not found; hiredis TLS support will be disabled")
        set(ENABLE_SSL OFF CACHE BOOL "" FORCE)
    endif()

    FetchContent_Declare(
        hiredis
        URL https://github.com/redis/hiredis/archive/refs/tags/v1.2.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(hiredis)

    if(TARGET hiredis AND NOT TARGET hiredis::hiredis)
        add_library(hiredis::hiredis ALIAS hiredis)
    endif()

    if(TARGET hiredis::hiredis)
        set(ASYNC_UV_REDIS_HAS_HIREDIS ON)
        if(TARGET hiredis::hiredis_ssl)
            set(ASYNC_UV_REDIS_HAS_HIREDIS_SSL ON)
        endif()
    else()
        message(WARNING "Failed to build hiredis source dependency; redis layer will be disabled")
    endif()
endif()
