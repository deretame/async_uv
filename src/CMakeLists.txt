include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_dependencies.cmake)

add_library(async_uv
    cancel.cpp
    runtime.cpp
    scope.cpp
    tls.cpp
    fd.cpp
    fs.cpp
    tcp.cpp
    timer.cpp
    udp.cpp
    watch.cpp
)

add_library(async_uv::async_uv ALIAS async_uv)

target_include_directories(async_uv
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(async_uv
    PUBLIC
        async_simple_headers
        libuv::libuv
)

if(NOT BUILD_SHARED_LIBS AND NOT DEFINED OPENSSL_USE_STATIC_LIBS)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(_async_uv_reset_openssl_use_static_libs TRUE)
endif()

find_package(OpenSSL REQUIRED)

if(_async_uv_reset_openssl_use_static_libs)
    unset(OPENSSL_USE_STATIC_LIBS)
    unset(_async_uv_reset_openssl_use_static_libs)
endif()

target_link_libraries(async_uv PUBLIC OpenSSL::SSL OpenSSL::Crypto)

if(ASYNC_UV_USE_MIMALLOC)
    if(TARGET mimalloc-static)
        target_link_libraries(async_uv PUBLIC mimalloc-static)
    elseif(TARGET mimalloc)
        target_link_libraries(async_uv PUBLIC mimalloc)
    else()
        message(FATAL_ERROR "ASYNC_UV_USE_MIMALLOC=ON but mimalloc target was not created")
    endif()
endif()

target_compile_features(async_uv PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(async_uv PRIVATE -Wall -Wextra -Wpedantic)
endif()
