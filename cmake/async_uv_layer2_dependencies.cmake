include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(ASYNC_UV_LAYER2_FETCH_CURL "Fetch curl from source when system curl is unavailable" ON)
option(ASYNC_UV_LAYER2_FETCH_LLHTTP "Fetch llhttp from source when unavailable" ON)
option(ASYNC_UV_LAYER2_FETCH_BOOST "Fetch boost from source when unavailable" ON)

find_package(OpenSSL QUIET)
find_package(CURL QUIET)

if(NOT TARGET CURL::libcurl)
    if(NOT ASYNC_UV_LAYER2_FETCH_CURL)
        message(FATAL_ERROR "layer2 requires CURL::libcurl. Install system libcurl or set ASYNC_UV_LAYER2_FETCH_CURL=ON")
    endif()

    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)

    set(ASYNC_UV_LAYER2_USE_MBEDTLS OFF)

    if(OpenSSL_FOUND)
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    else()
        message(WARNING "OpenSSL not found, fallback to mbedTLS backend for curl")
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
        set(ASYNC_UV_LAYER2_USE_MBEDTLS ON)

        if(NOT TARGET mbedtls)
            set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
            set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
            set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
            set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
            set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)

            FetchContent_Declare(
                mbedtls
                URL https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/v3.6.3
                DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            )

            FetchContent_MakeAvailable(mbedtls)
        endif()

        set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
        set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
        set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
        set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(
        curl
        URL https://codeload.github.com/curl/curl/tar.gz/refs/tags/curl-8_13_0
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(curl)

    if(TARGET libcurl AND NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl ALIAS libcurl)
    endif()
endif()

if(NOT TARGET llhttp::llhttp)
    if(NOT ASYNC_UV_LAYER2_FETCH_LLHTTP)
        message(FATAL_ERROR "layer2 requires llhttp. Install llhttp or set ASYNC_UV_LAYER2_FETCH_LLHTTP=ON")
    endif()

    FetchContent_Declare(
        llhttp
        URL https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.2.1
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(llhttp)

    if(TARGET llhttp_static AND NOT TARGET llhttp::llhttp)
        add_library(llhttp::llhttp ALIAS llhttp_static)
    elseif(TARGET llhttp AND NOT TARGET llhttp::llhttp)
        add_library(llhttp::llhttp ALIAS llhttp)
    elseif(TARGET llhttp_shared AND NOT TARGET llhttp::llhttp)
        add_library(llhttp::llhttp ALIAS llhttp_shared)
    endif()

    if(NOT TARGET llhttp::llhttp)
        if(EXISTS "${llhttp_SOURCE_DIR}/src/api.c" AND EXISTS "${llhttp_SOURCE_DIR}/include/llhttp.h")
            add_library(async_uv_llhttp_fallback STATIC
                ${llhttp_SOURCE_DIR}/src/api.c
                ${llhttp_SOURCE_DIR}/src/http.c
                ${llhttp_SOURCE_DIR}/src/llhttp.c
            )
            target_include_directories(async_uv_llhttp_fallback PUBLIC
                ${llhttp_SOURCE_DIR}/include
            )
            add_library(llhttp::llhttp ALIAS async_uv_llhttp_fallback)
        endif()
    endif()

    if(NOT TARGET llhttp::llhttp)
        message(FATAL_ERROR "Failed to provide llhttp::llhttp target")
    endif()
endif()

option(ASYNC_UV_LAYER2_FETCH_ADA "Fetch ada-url from source when unavailable" ON)

if(NOT TARGET ada::ada)
    if(NOT ASYNC_UV_LAYER2_FETCH_ADA)
        message(FATAL_ERROR "layer2 requires ada::ada. Install ada-url or set ASYNC_UV_LAYER2_FETCH_ADA=ON")
    endif()

    FetchContent_Declare(
        ada_url
        URL https://codeload.github.com/ada-url/ada/tar.gz/refs/tags/v2.9.1
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(ada_url)

    if(NOT TARGET ada::ada)
        if(EXISTS "${ada_url_SOURCE_DIR}/include")
            add_library(ada::ada INTERFACE IMPORTED GLOBAL)
            target_include_directories(ada::ada INTERFACE "${ada_url_SOURCE_DIR}/include")
        else()
            message(FATAL_ERROR "Failed to provide ada::ada target")
        endif()
    endif()
endif()
