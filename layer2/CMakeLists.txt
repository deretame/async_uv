if(NOT TARGET async_uv)
    message(FATAL_ERROR "layer2 requires async_uv target")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_sql_dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_redis_dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_ws_dependencies.cmake)

add_library(async_uv_http
    src/http.cpp
    src/parser.cpp
    src/server.cpp
)

add_library(async_uv_sql
    src/sql.cpp
)

add_library(async_uv_redis
    src/redis.cpp
)

add_library(async_uv_ws
    src/ws.cpp
)

add_library(async_uv::http ALIAS async_uv_http)
add_library(async_uv::sql ALIAS async_uv_sql)
add_library(async_uv::redis ALIAS async_uv_redis)
add_library(async_uv::ws ALIAS async_uv_ws)

target_include_directories(async_uv_http
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(async_uv_http
    PUBLIC
        async_uv
        CURL::libcurl
        llhttp::llhttp
        ada::ada
)

target_compile_features(async_uv_http PUBLIC cxx_std_23)

target_include_directories(async_uv_sql
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(async_uv_sql
    PUBLIC
        async_uv
        sqlite3::sqlite3
)

if(ASYNC_UV_SQL_HAS_LIBPQ)
    target_link_libraries(async_uv_sql PUBLIC libpq::libpq)
    target_compile_definitions(async_uv_sql PUBLIC ASYNC_UV_SQL_HAS_LIBPQ=1)
endif()

if(ASYNC_UV_SQL_HAS_MYSQL)
    target_link_libraries(async_uv_sql PUBLIC mysql::mysql)
    target_compile_definitions(async_uv_sql PUBLIC ASYNC_UV_SQL_HAS_MYSQL=1)
endif()

if(ASYNC_UV_SQL_HAS_SQLITE3)
    target_compile_definitions(async_uv_sql PUBLIC ASYNC_UV_SQL_HAS_SQLITE3=1)
endif()

target_compile_features(async_uv_sql PUBLIC cxx_std_23)

target_include_directories(async_uv_redis
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(async_uv_redis
    PUBLIC
        async_uv
)

if(ASYNC_UV_REDIS_HAS_HIREDIS)
    target_link_libraries(async_uv_redis PUBLIC hiredis::hiredis)
    target_compile_definitions(async_uv_redis PUBLIC ASYNC_UV_REDIS_HAS_HIREDIS=1)
endif()

if(ASYNC_UV_REDIS_HAS_HIREDIS_SSL)
    target_link_libraries(async_uv_redis PUBLIC hiredis::hiredis_ssl)
    target_compile_definitions(async_uv_redis PUBLIC ASYNC_UV_REDIS_HAS_HIREDIS_SSL=1)
endif()

target_compile_features(async_uv_redis PUBLIC cxx_std_23)

target_include_directories(async_uv_ws
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(async_uv_ws
    PUBLIC
        async_uv
)

if(ASYNC_UV_WS_HAS_LIBWEBSOCKETS)
    target_link_libraries(async_uv_ws PUBLIC async_uv::layer2_lws)
    target_compile_definitions(async_uv_ws PUBLIC ASYNC_UV_WS_HAS_LIBWEBSOCKETS=1)
endif()

target_compile_features(async_uv_ws PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(async_uv_http PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(async_uv_sql PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(async_uv_redis PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(async_uv_ws PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASYNC_UV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
