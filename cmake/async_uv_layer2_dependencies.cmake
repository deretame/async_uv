include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(FLUX_LAYER2_PREFER_SOURCE_DEPS
       "Prefer building layer2 dependencies from source (except OpenSSL)"
       ON)
option(FLUX_LAYER2_FETCH_BOOST
       "Fetch Boost headers from source when system Boost is unavailable"
       ON)

if(NOT TARGET Boost::headers AND NOT FLUX_LAYER2_PREFER_SOURCE_DEPS)
    find_package(Boost QUIET)
endif()

if(NOT TARGET Boost::headers)
    if(Boost_FOUND AND Boost_INCLUDE_DIRS)
        add_library(flux_layer2_boost_headers INTERFACE)
        target_include_directories(flux_layer2_boost_headers INTERFACE ${Boost_INCLUDE_DIRS})
        add_library(Boost::headers ALIAS flux_layer2_boost_headers)
    elseif(FLUX_LAYER2_FETCH_BOOST)
        FetchContent_Declare(
            flux_boost_headers
            URL https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        )
        FetchContent_MakeAvailable(flux_boost_headers)

        add_library(flux_layer2_boost_headers INTERFACE)
        target_include_directories(
            flux_layer2_boost_headers INTERFACE ${flux_boost_headers_SOURCE_DIR})
        target_compile_definitions(
            flux_layer2_boost_headers INTERFACE BOOST_ERROR_CODE_HEADER_ONLY=1)
        add_library(Boost::headers ALIAS flux_layer2_boost_headers)
    else()
        message(FATAL_ERROR
            "layer2 requires Boost headers (beast/url/mysql/redis); "
            "install Boost or set FLUX_LAYER2_FETCH_BOOST=ON")
    endif()
endif()

set(FLUX_LAYER2_BOOST_ROOT "")
if(DEFINED flux_boost_headers_SOURCE_DIR AND
   EXISTS "${flux_boost_headers_SOURCE_DIR}/boost/version.hpp")
    set(FLUX_LAYER2_BOOST_ROOT "${flux_boost_headers_SOURCE_DIR}")
elseif(TARGET Boost::headers)
    get_target_property(_flux_layer2_boost_header_dirs Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
    if(_flux_layer2_boost_header_dirs)
        foreach(_flux_layer2_boost_dir IN LISTS _flux_layer2_boost_header_dirs)
            if(EXISTS "${_flux_layer2_boost_dir}/boost/version.hpp")
                set(FLUX_LAYER2_BOOST_ROOT "${_flux_layer2_boost_dir}")
                break()
            endif()
        endforeach()
    endif()
endif()

if(NOT TARGET Boost::url)
    if(FLUX_LAYER2_BOOST_ROOT AND
       EXISTS "${FLUX_LAYER2_BOOST_ROOT}/libs/url/src")
        file(
            GLOB_RECURSE FLUX_LAYER2_BOOST_URL_SOURCES
            CONFIGURE_DEPENDS
            "${FLUX_LAYER2_BOOST_ROOT}/libs/url/src/*.cpp")

        add_library(flux_layer2_boost_url STATIC ${FLUX_LAYER2_BOOST_URL_SOURCES})
        target_include_directories(flux_layer2_boost_url PUBLIC ${FLUX_LAYER2_BOOST_ROOT})
        target_link_libraries(flux_layer2_boost_url PUBLIC Boost::headers)
        target_compile_definitions(
            flux_layer2_boost_url
            PUBLIC BOOST_URL_NO_LIB=1 BOOST_URL_STATIC_LINK=1
            PRIVATE BOOST_URL_SOURCE=1)
        add_library(Boost::url ALIAS flux_layer2_boost_url)
    else()
        find_package(Boost QUIET COMPONENTS url)
        if(NOT TARGET Boost::url)
            message(FATAL_ERROR
                "layer2 requires Boost.URL compiled library; "
                "set FLUX_LAYER2_FETCH_BOOST=ON or install Boost::url")
        endif()
    endif()
endif()

if(NOT TARGET Boost::charconv)
    if(FLUX_LAYER2_BOOST_ROOT AND
       EXISTS "${FLUX_LAYER2_BOOST_ROOT}/libs/charconv/src")
        set(FLUX_LAYER2_BOOST_CHARCONV_SOURCES
            "${FLUX_LAYER2_BOOST_ROOT}/libs/charconv/src/from_chars.cpp"
            "${FLUX_LAYER2_BOOST_ROOT}/libs/charconv/src/to_chars.cpp")

        add_library(flux_layer2_boost_charconv STATIC ${FLUX_LAYER2_BOOST_CHARCONV_SOURCES})
        target_include_directories(flux_layer2_boost_charconv PUBLIC ${FLUX_LAYER2_BOOST_ROOT})
        target_link_libraries(flux_layer2_boost_charconv PUBLIC Boost::headers)
        target_compile_definitions(
            flux_layer2_boost_charconv
            PUBLIC BOOST_CHARCONV_NO_LIB=1
            PRIVATE BOOST_CHARCONV_SOURCE=1)
        add_library(Boost::charconv ALIAS flux_layer2_boost_charconv)
    else()
        find_package(Boost QUIET COMPONENTS charconv)
        if(NOT TARGET Boost::charconv)
            message(FATAL_ERROR
                "layer2 mysql requires Boost.Charconv; "
                "set FLUX_LAYER2_FETCH_BOOST=ON or install Boost::charconv")
        endif()
    endif()
endif()
