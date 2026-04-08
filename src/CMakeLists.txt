include(${CMAKE_CURRENT_LIST_DIR}/../cmake/flux_dependencies.cmake)

add_library(flux
    cancel.cpp
    fd.cpp
    fs.cpp
    runtime.cpp
    tcp.cpp
    timer.cpp
    udp.cpp
    watch.cpp
)

add_library(flux::flux ALIAS flux)

target_include_directories(flux
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(flux
    PUBLIC
        flux_stdexec_headers
        flux_asio_headers
        Threads::Threads
)

if(FLUX_USE_MIMALLOC)
    if(TARGET mimalloc-static)
        target_link_libraries(flux PUBLIC mimalloc-static)
    elseif(TARGET mimalloc)
        target_link_libraries(flux PUBLIC mimalloc)
    else()
        message(FATAL_ERROR "FLUX_USE_MIMALLOC=ON but mimalloc target was not created")
    endif()
endif()

target_compile_features(flux PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(flux PRIVATE -Wall -Wextra -Wpedantic)
endif()
