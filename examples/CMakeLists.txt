if(NOT TARGET async_uv)
    message(FATAL_ERROR "examples require the async_uv target")
endif()

add_executable(async_uv_demo demo.cpp)
target_link_libraries(async_uv_demo PRIVATE async_uv)
