include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(FLUX_LAYER2_FETCH_LIBPQ "Fetch libpq from source for SQL layer" ON)
option(FLUX_LAYER2_FETCH_SQLITE3 "Fetch SQLite3 from source for SQL layer" ON)
option(FLUX_LAYER2_ENABLE_POSTGRES "Enable PostgreSQL SQL driver" OFF)
option(FLUX_LAYER2_ENABLE_MYSQL "Enable MySQL SQL driver" ON)

set(FLUX_SQL_HAS_LIBPQ OFF)
set(FLUX_SQL_HAS_MYSQL OFF)
set(FLUX_SQL_HAS_SQLITE3 OFF)

if(FLUX_LAYER2_ENABLE_POSTGRES AND FLUX_LAYER2_FETCH_LIBPQ)
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
        set(FLUX_SQL_HAS_LIBPQ ON)
    else()
        message(WARNING "Failed to build libpq source dependency; postgres driver will be disabled")
    endif()
endif()

# MySQL backend uses Boost.MySQL (header/source implementation in layer2/src/sql.cpp).
if(FLUX_LAYER2_ENABLE_MYSQL)
    set(FLUX_SQL_HAS_MYSQL ON)
endif()

if(FLUX_LAYER2_FETCH_SQLITE3)
    FetchContent_Declare(
        sqlite3_source
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )

    FetchContent_MakeAvailable(sqlite3_source)

    if(EXISTS "${sqlite3_source_SOURCE_DIR}/sqlite3.c" AND EXISTS "${sqlite3_source_SOURCE_DIR}/sqlite3.h")
        add_library(flux_sqlite3 STATIC
            ${sqlite3_source_SOURCE_DIR}/sqlite3.c
        )
        target_include_directories(flux_sqlite3 PUBLIC
            ${sqlite3_source_SOURCE_DIR}
        )
        add_library(sqlite3::sqlite3 ALIAS flux_sqlite3)
        set(FLUX_SQL_HAS_SQLITE3 ON)
    endif()
endif()

if(NOT FLUX_SQL_HAS_SQLITE3)
    message(FATAL_ERROR "sqlite3 source dependency is required for layer2 sql")
endif()
