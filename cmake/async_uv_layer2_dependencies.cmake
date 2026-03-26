include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(ASYNC_UV_LAYER2_FETCH_CURL "Fetch curl from source when system curl is unavailable" ON)

find_package(OpenSSL QUIET)
find_package(CURL QUIET)

if(TARGET CURL::libcurl)
    return()
endif()

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
    if(WIN32)
        message(WARNING "OpenSSL not found on Windows, fallback to Schannel backend for curl")
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    else()
        message(WARNING "OpenSSL not found on non-Windows, fallback to mbedTLS backend for curl")
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
