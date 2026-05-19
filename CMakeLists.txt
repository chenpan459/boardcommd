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

set(BOARDCOMM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${BOARDCOMM_SRC_DIR}/daemon
    ${BOARDCOMM_SRC_DIR}/transport
    ${BOARDCOMM_SRC_DIR}/log
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_library(boardcomm_log STATIC
    src/log/log.c
)

add_library(boardcomm SHARED
    api/boardcomm_client.c
)

target_include_directories(boardcomm PUBLIC api)
target_link_libraries(boardcomm PRIVATE boardcomm_log)

add_executable(boardcommd
    src/daemon/main.c
    src/daemon/reactor.c
    src/daemon/client_manager.c
    src/daemon/message_bus.c
    src/daemon/router.c
    src/daemon/protocol.c
    src/daemon/transport_manager.c
    src/daemon/config.c
    src/transport/udp_transport.c
    src/transport/tcp_transport.c
    src/transport/uart_transport.c
)
target_link_libraries(boardcommd PRIVATE boardcomm_log)

add_executable(boardcomm_pub examples/pub.c)
target_link_libraries(boardcomm_pub boardcomm boardcomm_log)

add_executable(boardcomm_sub examples/sub.c)
target_link_libraries(boardcomm_sub boardcomm boardcomm_log)

add_executable(boardcomm_perf_pub examples/perf_pub.c)
target_link_libraries(boardcomm_perf_pub boardcomm boardcomm_log)

add_executable(boardcomm_perf_sub examples/perf_sub.c)
target_link_libraries(boardcomm_perf_sub boardcomm boardcomm_log)

add_executable(boardcomm_stability_pub examples/stability_pub.c)
target_link_libraries(boardcomm_stability_pub boardcomm boardcomm_log)
