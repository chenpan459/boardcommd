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
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_library(boardcomm_log STATIC
    src/log/log.c
)
set_target_properties(boardcomm_log PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(boardcomm_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/api)

add_library(bc_api SHARED
    api/bc_client.c
    api/bc_file.c
    src/shm/bc_shm.c
)

target_include_directories(bc_api
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/api
    PRIVATE ${BOARDCOMM_SRC_DIR}/daemon
)

add_library(bc_transport SHARED
    src/transport/udp/udp_transport.c
    src/transport/udp/udp_kcp_transport.c
    src/transport/udp/kcp/ikcp.c
    src/transport/tcp/tcp_transport.c
    src/transport/uart/uart_transport.c
)
target_include_directories(bc_transport
    PUBLIC
        ${BOARDCOMM_SRC_DIR}/transport
        ${BOARDCOMM_SRC_DIR}/transport/udp
        ${BOARDCOMM_SRC_DIR}/transport/udp/kcp
        ${BOARDCOMM_SRC_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/api
)
target_link_libraries(bc_transport PRIVATE boardcomm_log)

add_executable(boardcommd
    src/daemon/main.c
    src/daemon/reactor.c
    src/daemon/client_manager.c
    src/daemon/message_bus.c
    src/daemon/router.c
    src/daemon/protocol.c
    src/daemon/transport_manager.c
    src/daemon/config.c
    src/daemon/topic_match.c
    src/daemon/stats.c
    src/daemon/fragment.c
    src/shm/bc_shm.c
)
target_link_libraries(boardcommd PRIVATE boardcomm_log bc_transport dl rt)

add_executable(test_topic_match
    tests/test_topic_match.c
    src/daemon/topic_match.c
)
target_include_directories(test_topic_match PRIVATE ${BOARDCOMM_SRC_DIR}/daemon)

add_executable(boardcomm_pub examples/pub.c)
target_link_libraries(boardcomm_pub bc_api boardcomm_log)

add_executable(boardcomm_sub examples/sub.c)
target_link_libraries(boardcomm_sub bc_api boardcomm_log)

add_executable(boardcomm_perf_pub examples/perf_pub.c)
target_link_libraries(boardcomm_perf_pub bc_api boardcomm_log)

add_executable(boardcomm_perf_sub examples/perf_sub.c)
target_link_libraries(boardcomm_perf_sub bc_api boardcomm_log)

add_executable(boardcomm_stability_pub examples/stability_pub.c)
target_link_libraries(boardcomm_stability_pub bc_api boardcomm_log)

add_executable(boardcomm_file_send examples/file_send.c)
target_link_libraries(boardcomm_file_send bc_api boardcomm_log)

add_executable(boardcomm_file_recv examples/file_recv.c)
target_link_libraries(boardcomm_file_recv bc_api boardcomm_log)
