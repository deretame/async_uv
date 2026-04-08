include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(ASYNC_UV_LAYER2_ENABLE_WEBSOCKET
       "Enable layer2 websocket wrapper based on libwebsockets"
       ON)
option(ASYNC_UV_LAYER2_ENABLE_WSS
       "Enable TLS support for layer2 websocket wrapper"
       ON)
option(ASYNC_UV_LAYER2_FETCH_LIBWEBSOCKETS
       "Fetch libwebsockets when websocket wrapper is enabled"
       ON)

set(ASYNC_UV_WS_HAS_LIBWEBSOCKETS OFF)

if(NOT ASYNC_UV_LAYER2_ENABLE_WEBSOCKET)
    return()
endif()

if(TARGET async_uv_layer2_lws)
    set(ASYNC_UV_WS_HAS_LIBWEBSOCKETS ON)
    return()
endif()

if(NOT ASYNC_UV_LAYER2_FETCH_LIBWEBSOCKETS)
    message(FATAL_ERROR "websocket wrapper requires libwebsockets; set ASYNC_UV_LAYER2_FETCH_LIBWEBSOCKETS=ON")
endif()

find_package(OpenSSL QUIET)

set(LWS_WITH_NETWORK ON CACHE BOOL "" FORCE)
set(LWS_ROLE_H1 ON CACHE BOOL "" FORCE)
set(LWS_ROLE_WS ON CACHE BOOL "" FORCE)
set(LWS_WITH_HTTP2 OFF CACHE BOOL "" FORCE)
set(LWS_ROLE_H2 OFF CACHE BOOL "" FORCE)
set(LWS_WITH_LIBUV ON CACHE BOOL "" FORCE)
set(LWS_WITH_EVLIB_PLUGINS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
if(ASYNC_UV_LAYER2_ENABLE_WSS)
    if(NOT OpenSSL_FOUND)
        find_package(OpenSSL REQUIRED)
    endif()
    set(LWS_WITH_SSL ON CACHE BOOL "" FORCE)
    set(LWS_WITH_MBEDTLS OFF CACHE BOOL "" FORCE)
else()
    set(LWS_WITH_SSL OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_MBEDTLS OFF CACHE BOOL "" FORCE)
endif()
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_EXTENSIONS ON CACHE BOOL "" FORCE)
set(LWS_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(LWS_WITH_EXPORT_LWSTARGETS OFF CACHE BOOL "" FORCE)
set(DISABLE_WERROR ON CACHE BOOL "" FORCE)
set(LWS_WITH_LHP OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SECURE_STREAMS OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SYS_STATE OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SYS_SMD OFF CACHE BOOL "" FORCE)
set(LWS_WITH_CONMON OFF CACHE BOOL "" FORCE)
set(LWS_WITH_DIR OFF CACHE BOOL "" FORCE)
set(LWS_WITH_LEJP_CONF OFF CACHE BOOL "" FORCE)

if(DEFINED libuv_SOURCE_DIR)
    set(LIBUV_INCLUDE_DIRS "${libuv_SOURCE_DIR}/include" CACHE PATH "" FORCE)
endif()
set(LIBUV_LIBRARIES libuv::libuv CACHE STRING "" FORCE)

FetchContent_Declare(
    async_uv_libwebsockets
    URL https://codeload.github.com/warmcat/libwebsockets/tar.gz/refs/tags/v4.4.0
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

FetchContent_MakeAvailable(async_uv_libwebsockets)

set(_async_uv_lws_vhost_c "${async_uv_libwebsockets_SOURCE_DIR}/lib/core-net/vhost.c")
if(EXISTS "${_async_uv_lws_vhost_c}")
    file(READ "${_async_uv_lws_vhost_c}" _async_uv_lws_vhost_text)
    if(_async_uv_lws_vhost_text MATCHES "vh->protocols\\[n\\]\\.callback\\(\\(struct lws \\*\\)lwsa,")
        string(REPLACE
            "if (vh->protocols[n].callback((struct lws *)lwsa,"
            "if (vh->protocols[n].callback && vh->protocols[n].callback((struct lws *)lwsa,"
            _async_uv_lws_vhost_text
            "${_async_uv_lws_vhost_text}")
        file(WRITE "${_async_uv_lws_vhost_c}" "${_async_uv_lws_vhost_text}")
    endif()
endif()

set(_async_uv_lws_windows_service_c
    "${async_uv_libwebsockets_SOURCE_DIR}/lib/plat/windows/windows-service.c")
if(EXISTS "${_async_uv_lws_windows_service_c}")
    file(READ "${_async_uv_lws_windows_service_c}" _async_uv_lws_windows_service_text_original)
    set(_async_uv_lws_windows_service_text "${_async_uv_lws_windows_service_text_original}")
    string(REPLACE
        "if (timeout_ms64 < 0)\n\t\ttimeout_ms64 = 0;\n\telse\n\t\t/* force a default timeout of 23 days */\n\t\ttimeout_ms64 = 2000000000;"
        "if (timeout_ms64 < 0)\n\t\ttimeout_ms64 = 0;\n\telse if (timeout_ms64 > 2000000000)\n\t\ttimeout_ms64 = 2000000000;"
        _async_uv_lws_windows_service_text
        "${_async_uv_lws_windows_service_text}")
    if(NOT _async_uv_lws_windows_service_text STREQUAL _async_uv_lws_windows_service_text_original)
        file(WRITE "${_async_uv_lws_windows_service_c}" "${_async_uv_lws_windows_service_text}")
    endif()
endif()

if(TARGET websockets)
    if(MSVC)
        target_compile_options(websockets PRIVATE /WX-)
    endif()
    add_library(async_uv_layer2_lws INTERFACE)
    target_link_libraries(async_uv_layer2_lws INTERFACE websockets)
elseif(TARGET websockets_shared)
    if(MSVC)
        target_compile_options(websockets_shared PRIVATE /WX-)
    endif()
    add_library(async_uv_layer2_lws INTERFACE)
    target_link_libraries(async_uv_layer2_lws INTERFACE websockets_shared)
else()
    message(FATAL_ERROR "libwebsockets target not found after FetchContent")
endif()

add_library(flux::layer2_lws ALIAS async_uv_layer2_lws)
set(ASYNC_UV_WS_HAS_LIBWEBSOCKETS ON)
