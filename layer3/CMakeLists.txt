if(NOT TARGET async_uv_http)
    message(FATAL_ERROR "layer3 requires async_uv_http target from layer2")
endif()

add_library(async_uv_layer3 INTERFACE)
add_library(async_uv::layer3 ALIAS async_uv_layer3)

target_link_libraries(async_uv_layer3 INTERFACE async_uv_http)
