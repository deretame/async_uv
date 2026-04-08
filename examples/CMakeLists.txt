if(NOT TARGET flux)
    message(FATAL_ERROR "examples require the flux target")
endif()

add_executable(flux_demo demo.cpp)
target_link_libraries(flux_demo PRIVATE flux)
