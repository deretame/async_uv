include(FetchContent)
include(ExternalProject)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(ASYNC_UV_LAYER2_FETCH_LIBPQ "Fetch libpq from source for SQL layer" ON)
option(ASYNC_UV_LAYER2_FETCH_MYSQL "Fetch MySQL client from source for SQL layer" ON)
option(ASYNC_UV_LAYER2_FETCH_SQLITE3 "Fetch SQLite3 from source for SQL layer" ON)

set(ASYNC_UV_SQL_HAS_LIBPQ OFF)
set(ASYNC_UV_SQL_HAS_MYSQL OFF)
set(ASYNC_UV_SQL_HAS_SQLITE3 OFF)

if(ASYNC_UV_LAYER2_FETCH_LIBPQ)
    FetchContent_Declare(
        libpq
        URL https://gitlab.com/sabelka/libpq-standalone/-/archive/REL_17_7/libpq-standalone-REL_17_7.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(libpq)

    if(TARGET libpq AND NOT TARGET libpq::libpq)
        add_library(libpq::libpq ALIAS libpq)
    elseif(TARGET pq AND NOT TARGET libpq::libpq)
        add_library(libpq::libpq ALIAS pq)
    endif()

    if(TARGET libpq::libpq)
        set(ASYNC_UV_SQL_HAS_LIBPQ ON)
    else()
        message(WARNING "Failed to build libpq source dependency; postgres driver will be disabled")
    endif()
endif()

if(ASYNC_UV_LAYER2_FETCH_MYSQL)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        message(WARNING "Official mysql client source build requires OpenSSL development package; mysql driver will be disabled")
    else()
    set(_mysql_source_url "https://dev.mysql.com/get/Downloads/MySQL-8.4/mysql-8.4.0.tar.gz")
    set(_mysql_ep_prefix "${CMAKE_BINARY_DIR}/_deps/mysql_server_client")
    set(_mysql_ep_install "${_mysql_ep_prefix}/install")

    FetchContent_Declare(
        mysql_server_headers
        URL ${_mysql_source_url}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_GetProperties(mysql_server_headers)
    if(NOT mysql_server_headers_POPULATED)
        FetchContent_Populate(mysql_server_headers)
    endif()

    ExternalProject_Add(mysql_server_client_ep
        SOURCE_DIR ${mysql_server_headers_SOURCE_DIR}
        DOWNLOAD_COMMAND ""
        PREFIX ${_mysql_ep_prefix}
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${_mysql_ep_install}
            -DWITHOUT_SERVER=ON
            -DWITH_UNIT_TESTS=OFF
            -DWITH_SSL=yes
            -DDOWNLOAD_BOOST=ON
            -DWITH_BOOST=${_mysql_ep_prefix}/boost
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libmysql
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
            <BINARY_DIR>/lib/libmysqlclient.so
    )

    ExternalProject_Get_Property(mysql_server_client_ep SOURCE_DIR BINARY_DIR)
    file(MAKE_DIRECTORY "${BINARY_DIR}/include")
    file(MAKE_DIRECTORY "${BINARY_DIR}/include/mysql")

    add_library(async_uv_mysqlclient_external SHARED IMPORTED GLOBAL)
    set_target_properties(async_uv_mysqlclient_external PROPERTIES
        IMPORTED_LOCATION "${BINARY_DIR}/lib/libmysqlclient.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SOURCE_DIR}/include;${SOURCE_DIR}/libmysql;${BINARY_DIR}/include;${BINARY_DIR}/include/mysql"
    )
    add_dependencies(async_uv_mysqlclient_external mysql_server_client_ep)

    if(NOT TARGET mysql::mysql)
        add_library(mysql::mysql ALIAS async_uv_mysqlclient_external)
    endif()

    set(ASYNC_UV_SQL_HAS_MYSQL ON)
    endif()
endif()

if(ASYNC_UV_LAYER2_FETCH_SQLITE3)
    FetchContent_Declare(
        sqlite3_source
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(sqlite3_source)

    if(EXISTS "${sqlite3_source_SOURCE_DIR}/sqlite3.c" AND EXISTS "${sqlite3_source_SOURCE_DIR}/sqlite3.h")
        add_library(async_uv_sqlite3 STATIC
            ${sqlite3_source_SOURCE_DIR}/sqlite3.c
        )
        target_include_directories(async_uv_sqlite3 PUBLIC
            ${sqlite3_source_SOURCE_DIR}
        )
        add_library(sqlite3::sqlite3 ALIAS async_uv_sqlite3)
        set(ASYNC_UV_SQL_HAS_SQLITE3 ON)
    endif()
endif()

if(NOT ASYNC_UV_SQL_HAS_SQLITE3)
    message(FATAL_ERROR "sqlite3 source dependency is required for layer2 sql")
endif()
