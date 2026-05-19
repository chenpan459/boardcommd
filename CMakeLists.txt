cmake_minimum_required(VERSION 3.16)

project(boardcommd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(BOARDCOMM_VERSION "" CACHE STRING "boardcomm build version")
if(NOT BOARDCOMM_VERSION)
    message(FATAL_ERROR "BOARDCOMM_VERSION is required. Use ./build.sh <version> or -DBOARDCOMM_VERSION=<version>.")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boardcomm_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/boardcomm_version.h
    @ONLY
)

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_library(boardcomm SHARED
    sdk/boardcomm_client.c
)

target_include_directories(boardcomm PUBLIC include)

add_executable(boardcommd
    daemon/main.c
    daemon/reactor.c
    daemon/client_manager.c
    daemon/message_bus.c
    daemon/router.c
    daemon/protocol.c
    daemon/transport_manager.c
    daemon/config.c
    transport/udp_transport.c
    transport/tcp_transport.c
    transport/uart_transport.c
)

add_executable(boardcomm_pub examples/pub.c)
target_link_libraries(boardcomm_pub boardcomm)

add_executable(boardcomm_sub examples/sub.c)
target_link_libraries(boardcomm_sub boardcomm)
