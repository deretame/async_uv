if(NOT TARGET flux)
    message(FATAL_ERROR "layer2 requires flux target")
endif()

find_package(OpenSSL REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_sql_dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_redis_dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/async_uv_layer2_ws_dependencies.cmake)

add_library(flux_http
    src/http.cpp
    src/parser.cpp
    src/server.cpp
)

add_library(flux_sql
    src/sql.cpp
)

add_library(flux_redis
    src/redis.cpp
)

add_library(flux_ws
    src/ws.cpp
)

add_library(flux::http ALIAS flux_http)
add_library(flux::sql ALIAS flux_sql)
add_library(flux::redis ALIAS flux_redis)
add_library(flux::ws ALIAS flux_ws)

target_include_directories(flux_http
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(flux_http
    PUBLIC
        flux
        Boost::headers
        Boost::url
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_features(flux_http PUBLIC cxx_std_23)

target_include_directories(flux_sql
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(flux_sql
    PUBLIC
        flux
        Boost::headers
        sqlite3::sqlite3
)

if(FLUX_SQL_HAS_LIBPQ)
    target_link_libraries(flux_sql PUBLIC libpq::libpq)
    target_compile_definitions(flux_sql PUBLIC FLUX_SQL_HAS_LIBPQ=1)
endif()

if(FLUX_SQL_HAS_MYSQL)
    target_link_libraries(flux_sql PUBLIC OpenSSL::SSL OpenSSL::Crypto Boost::charconv)
    target_compile_definitions(flux_sql PRIVATE BOOST_MYSQL_SEPARATE_COMPILATION=1)
    target_compile_definitions(flux_sql PUBLIC FLUX_SQL_HAS_MYSQL=1)
endif()

if(FLUX_SQL_HAS_SQLITE3)
    target_compile_definitions(flux_sql PUBLIC FLUX_SQL_HAS_SQLITE3=1)
endif()

target_compile_features(flux_sql PUBLIC cxx_std_23)

target_include_directories(flux_redis
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(flux_redis
    PUBLIC
        flux
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_definitions(flux_redis PUBLIC FLUX_REDIS_HAS_HIREDIS=0 FLUX_REDIS_HAS_HIREDIS_SSL=0)

target_compile_features(flux_redis PUBLIC cxx_std_23)

target_include_directories(flux_ws
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(flux_ws
    PUBLIC
        flux
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_definitions(flux_ws PUBLIC FLUX_WS_HAS_LIBWEBSOCKETS=0)

target_compile_features(flux_ws PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(flux_http PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(flux_sql PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(flux_redis PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(flux_ws PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FLUX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
