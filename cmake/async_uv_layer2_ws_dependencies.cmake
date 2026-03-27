include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

option(ASYNC_UV_LAYER2_FETCH_IXWEBSOCKET "Fetch IXWebSocket from source for WebSocket layer" ON)

set(ASYNC_UV_WS_HAS_IXWEBSOCKET OFF)

if(ASYNC_UV_LAYER2_FETCH_IXWEBSOCKET)
    set(USE_TLS ON CACHE BOOL "" FORCE)
    set(USE_MBED_TLS OFF CACHE BOOL "" FORCE)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(USE_WS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        ixwebsocket
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG v11.4.5
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
    )

    FetchContent_MakeAvailable(ixwebsocket)

    if(TARGET ixwebsocket::ixwebsocket)
        set(ASYNC_UV_WS_HAS_IXWEBSOCKET ON)
    elseif(TARGET ixwebsocket)
        add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
        set(ASYNC_UV_WS_HAS_IXWEBSOCKET ON)
    endif()

    if(NOT ASYNC_UV_WS_HAS_IXWEBSOCKET)
        message(WARNING "Failed to build IXWebSocket source dependency; ws layer will be disabled")
    endif()
endif()
