cmake_minimum_required(VERSION 3.20)
project(edgenode_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(EDGENODE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_compile_definitions(PB_ENABLE_MALLOC)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NANOPB_ROOT)
    add_library(edgenode_nanopb STATIC
        "${NANOPB_ROOT}/pb_common.c"
        "${NANOPB_ROOT}/pb_decode.c"
        "${NANOPB_ROOT}/pb_encode.c"
        "${EDGENODE_ROOT}/generated/edge.pb.c")
    target_include_directories(edgenode_nanopb PUBLIC
        "${NANOPB_ROOT}"
        "${EDGENODE_ROOT}/generated")
else()
    find_package(nanopb CONFIG REQUIRED)
    add_library(edgenode_nanopb STATIC
        "${EDGENODE_ROOT}/generated/edge.pb.c")
    target_include_directories(edgenode_nanopb PUBLIC
        "${EDGENODE_ROOT}/generated")
    target_link_libraries(edgenode_nanopb PUBLIC nanopb::protobuf-nanopb-static)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Flexible array members are standard C99, but MSVC reports C4200.
    add_compile_options(/W4 /WX /permissive- /wd4200 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

enable_testing()

option(EDGENODE_GATEWAY_NETWORK_TESTS "验证 Modbus 网关实际 TCP 连接与分包" OFF)

if(MBEDTLS_ROOT)
    add_library(edgenode_gateway_crypto STATIC
        "${MBEDTLS_ROOT}/library/sha256.c" "${MBEDTLS_ROOT}/library/platform_util.c")
    target_include_directories(edgenode_gateway_crypto PUBLIC "${MBEDTLS_ROOT}/include")
else()
    find_path(GATEWAY_MBEDTLS_INCLUDE_DIR mbedtls/sha256.h REQUIRED)
    find_library(GATEWAY_MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)
    add_library(edgenode_gateway_crypto INTERFACE)
    target_include_directories(edgenode_gateway_crypto INTERFACE "${GATEWAY_MBEDTLS_INCLUDE_DIR}")
    target_link_libraries(edgenode_gateway_crypto INTERFACE "${GATEWAY_MBEDCRYPTO_LIBRARY}")
endif()

add_executable(edge_modbus_gateway_test edge_modbus_gateway_test.c
    "${EDGENODE_ROOT}/src/edge_modbus_gateway.c"
    "${EDGENODE_ROOT}/src/edge_runtime_config.c"
    "${EDGENODE_ROOT}/src/edge_industrial.c"
    "${EDGENODE_ROOT}/src/edge_derived.c")
target_include_directories(edge_modbus_gateway_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_modbus_gateway_test PRIVATE edgenode_nanopb edgenode_gateway_crypto)
if(NOT MSVC)
    target_link_libraries(edge_modbus_gateway_test PRIVATE m)
endif()
add_test(NAME edge_modbus_gateway COMMAND edge_modbus_gateway_test)

option(EDGENODE_EXTERNAL_IO_TESTS "验证外部 IO 的 ubus 读取和写入" OFF)
if(EDGENODE_EXTERNAL_IO_TESTS)
    find_path(UBUS_INCLUDE_DIR libubus.h REQUIRED)
    find_library(UBUS_LIBRARY ubus REQUIRED)
    find_library(UBOX_LIBRARY ubox REQUIRED)
    find_program(UBUSD_EXECUTABLE ubusd REQUIRED)
    add_executable(edge_external_io_test edge_external_io_test.c "${EDGENODE_ROOT}/src/edge_external_io.c")
    target_include_directories(edge_external_io_test PRIVATE "${EDGENODE_ROOT}/src" "${UBUS_INCLUDE_DIR}")
    target_link_libraries(edge_external_io_test PRIVATE edgenode_nanopb "${UBUS_LIBRARY}" "${UBOX_LIBRARY}")
    add_test(NAME edge_external_io COMMAND edge_external_io_test "${UBUSD_EXECUTABLE}")
    set_tests_properties(edge_external_io PROPERTIES TIMEOUT 15)
    if(UNIX AND NOT APPLE)
        get_filename_component(UBUS_LIBRARY_DIR "${UBUS_LIBRARY}" DIRECTORY)
        get_filename_component(UBOX_LIBRARY_DIR "${UBOX_LIBRARY}" DIRECTORY)
        set_tests_properties(edge_external_io PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${UBUS_LIBRARY_DIR}:${UBOX_LIBRARY_DIR}:$ENV{LD_LIBRARY_PATH}")
    endif()
endif()

option(EDGENODE_WS_TRANSPORT_TESTS "验证 libwebsockets 原生传输及压缩线帧" OFF)
if(EDGENODE_WS_TRANSPORT_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_path(LWS_INCLUDE_DIR libwebsockets.h REQUIRED)
    find_path(LWS_CONFIG_INCLUDE_DIR lws_config.h REQUIRED)
    find_path(LIBEV_INCLUDE_DIR ev.h REQUIRED)
    find_library(LWS_LIBRARY websockets REQUIRED)
    find_library(LIBEV_LIBRARY ev REQUIRED)
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/sha256.h REQUIRED)
    # Compile the actual caller too, so transport ABI changes cannot leave the
    # firmware application broken while isolated transport fixtures still pass.
    add_library(edge_ws_application_compile OBJECT
        "${EDGENODE_ROOT}/src/edge_ws.c" "${EDGENODE_ROOT}/src/main.c")
    target_include_directories(edge_ws_application_compile PRIVATE
        "${EDGENODE_ROOT}/src" "${LWS_INCLUDE_DIR}" "${LWS_CONFIG_INCLUDE_DIR}"
        "${LIBEV_INCLUDE_DIR}" "${MBEDTLS_INCLUDE_DIR}")
    target_link_libraries(edge_ws_application_compile PRIVATE edgenode_nanopb)
    add_executable(edge_ws_transport_fixture edge_ws_transport_fixture.c
        "${EDGENODE_ROOT}/src/edge_process.c"
        "${EDGENODE_ROOT}/src/edge_ws_transport.c")
    target_include_directories(edge_ws_transport_fixture PRIVATE
        "${EDGENODE_ROOT}/src" "${LWS_INCLUDE_DIR}" "${LWS_CONFIG_INCLUDE_DIR}" "${LIBEV_INCLUDE_DIR}")
    target_link_libraries(edge_ws_transport_fixture PRIVATE "${LWS_LIBRARY}" "${LIBEV_LIBRARY}")
    add_test(NAME edge_ws_transport COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/edge_ws_transport_test.py" "$<TARGET_FILE:edge_ws_transport_fixture>")
    set_tests_properties(edge_ws_transport PROPERTIES TIMEOUT 45)
    add_executable(edge_ws_recovery_fixture edge_ws_recovery_fixture.c
        "${EDGENODE_ROOT}/src/edge_process.c"
        "${EDGENODE_ROOT}/src/edge_ws_transport.c"
        "${EDGENODE_ROOT}/src/edge_outbox.c")
    target_include_directories(edge_ws_recovery_fixture PRIVATE
        "${EDGENODE_ROOT}/src" "${LWS_INCLUDE_DIR}" "${LWS_CONFIG_INCLUDE_DIR}" "${LIBEV_INCLUDE_DIR}")
    target_link_libraries(edge_ws_recovery_fixture PRIVATE "${LWS_LIBRARY}" "${LIBEV_LIBRARY}")
    add_test(NAME edge_ws_recovery COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/edge_ws_recovery_test.py" "$<TARGET_FILE:edge_ws_recovery_fixture>")
    set_tests_properties(edge_ws_recovery PROPERTIES TIMEOUT 40)
    add_executable(edge_ws_isolation_fixture edge_ws_isolation_fixture.c
        "${EDGENODE_ROOT}/src/edge_process.c")
    target_include_directories(edge_ws_isolation_fixture PRIVATE
        "${EDGENODE_ROOT}/src" "${LWS_INCLUDE_DIR}" "${LWS_CONFIG_INCLUDE_DIR}" "${LIBEV_INCLUDE_DIR}")
    target_link_libraries(edge_ws_isolation_fixture PRIVATE "${LWS_LIBRARY}" "${LIBEV_LIBRARY}")
    add_test(NAME edge_ws_isolation COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/edge_ws_isolation_test.py" "$<TARGET_FILE:edge_ws_isolation_fixture>")
    set_tests_properties(edge_ws_isolation PROPERTIES TIMEOUT 60)
    add_executable(edge_identity_test edge_identity_test.c)
    target_include_directories(edge_identity_test PRIVATE
        "${EDGENODE_ROOT}/src" "${LWS_INCLUDE_DIR}" "${LWS_CONFIG_INCLUDE_DIR}" "${LIBEV_INCLUDE_DIR}")
    target_link_libraries(edge_identity_test PRIVATE edgenode_nanopb "${LIBEV_LIBRARY}")
    target_compile_options(edge_identity_test PRIVATE -UNDEBUG)
    add_test(NAME edge_identity COMMAND edge_identity_test)
    set_tests_properties(edge_identity PROPERTIES TIMEOUT 10)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(edge_traffic_test edge_traffic_test.c "${EDGENODE_ROOT}/src/edge_traffic.c")
    target_include_directories(edge_traffic_test PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_traffic_test PRIVATE edgenode_nanopb)
    target_compile_options(edge_traffic_test PRIVATE -UNDEBUG)
    add_test(NAME edge_traffic COMMAND edge_traffic_test)
endif()

add_executable(edge_protocol_test
    edge_protocol_test.c
    "${EDGENODE_ROOT}/src/edge_protocol.c")
target_include_directories(edge_protocol_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_protocol_test PRIVATE edgenode_nanopb)
add_test(NAME edge_protocol COMMAND edge_protocol_test)

add_executable(edge_report_test edge_report_test.c "${EDGENODE_ROOT}/src/edge_report.c")
target_include_directories(edge_report_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_report_test PRIVATE edgenode_nanopb)
add_test(NAME edge_report COMMAND edge_report_test)

add_executable(edge_config_test
    edge_config_test.c
    "${EDGENODE_ROOT}/src/edge_industrial.c"
    "${EDGENODE_ROOT}/src/edge_runtime_config.c"
    "${EDGENODE_ROOT}/src/edge_derived.c")
target_include_directories(edge_config_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_config_test PRIVATE edgenode_nanopb)
add_test(NAME edge_config COMMAND edge_config_test)

add_executable(edge_url_test
    edge_url_test.c
    "${EDGENODE_ROOT}/src/edge_url.c")
target_include_directories(edge_url_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_url COMMAND edge_url_test)

add_executable(edge_runtime_test
    edge_runtime_test.c
    "${EDGENODE_ROOT}/src/edge_device_runtime.c"
    "${EDGENODE_ROOT}/src/edge_modbus.c"
    "${EDGENODE_ROOT}/src/edge_s7.c")
target_include_directories(edge_runtime_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_runtime COMMAND edge_runtime_test)

add_executable(edge_outbox_test
    edge_outbox_test.c
    "${EDGENODE_ROOT}/src/edge_outbox.c")
target_include_directories(edge_outbox_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_outbox COMMAND edge_outbox_test)

add_executable(edge_retry_test
    edge_retry_test.c
    "${EDGENODE_ROOT}/src/edge_retry.c")
target_include_directories(edge_retry_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_retry COMMAND edge_retry_test)

add_executable(edge_migration_test edge_migration_test.c)
target_compile_definitions(edge_migration_test PRIVATE
    EDGENODE_MIGRATION_SCRIPT="${EDGENODE_ROOT}/files/95-edgenode-platform")
add_test(NAME edge_migration COMMAND edge_migration_test)

add_executable(edge_interface_test
    edge_interface_test.c
    "${EDGENODE_ROOT}/src/edge_interface.c")
target_include_directories(edge_interface_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_interface COMMAND edge_interface_test)

add_executable(edge_firmware_policy_test edge_firmware_policy_test.c)
target_include_directories(edge_firmware_policy_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_firmware_policy COMMAND edge_firmware_policy_test)

add_executable(edge_vpn_config_test edge_vpn_config_test.c "${EDGENODE_ROOT}/src/edge_vpn_plan.c")
target_include_directories(edge_vpn_config_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_vpn_config_test PRIVATE edgenode_nanopb)
add_test(NAME edge_vpn_config COMMAND edge_vpn_config_test)
option(EDGENODE_VPN_NAMESPACE_TESTS "验证实际 WireGuard、多平台重叠网段及独立停机，运行需要 root" OFF)
if(EDGENODE_VPN_NAMESPACE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME edge_vpn_namespace COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/edge_vpn_namespace_test.py" "$<TARGET_FILE:edge_vpn_config_test>")
    set_tests_properties(edge_vpn_namespace PROPERTIES TIMEOUT 60)
endif()

add_executable(edge_firmware_stream_test
    edge_firmware_stream_test.c
    "${EDGENODE_ROOT}/src/edge_firmware_stream.c")
target_include_directories(edge_firmware_stream_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_firmware_stream COMMAND edge_firmware_stream_test)

add_executable(edge_status_test
    edge_status_test.c
    "${EDGENODE_ROOT}/src/edge_status.c")
target_include_directories(edge_status_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_status_test PRIVATE edgenode_nanopb)
add_test(NAME edge_status COMMAND edge_status_test)

add_executable(edgenode_stability_x64
    edgenode_stability_test.c
    "${EDGENODE_ROOT}/src/edge_protocol.c"
    "${EDGENODE_ROOT}/src/edge_industrial.c"
    "${EDGENODE_ROOT}/src/edge_runtime_config.c"
    "${EDGENODE_ROOT}/src/edge_derived.c"
    "${EDGENODE_ROOT}/src/edge_device_runtime.c"
    "${EDGENODE_ROOT}/src/edge_modbus.c"
    "${EDGENODE_ROOT}/src/edge_s7.c"
    "${EDGENODE_ROOT}/src/edge_outbox.c"
    "${EDGENODE_ROOT}/src/edge_retry.c")
target_include_directories(edgenode_stability_x64 PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edgenode_stability_x64 PRIVATE edgenode_nanopb)
if(WIN32)
    target_link_libraries(edgenode_stability_x64 PRIVATE psapi)
endif()
set_target_properties(edgenode_stability_x64 PROPERTIES
    OUTPUT_NAME "edgenode-stability-x64")
add_test(NAME edgenode_stability COMMAND edgenode_stability_x64 --iterations 20000)

if(UNIX)
    if(EDGENODE_GATEWAY_NETWORK_TESTS)
        find_path(GATEWAY_LIBEV_INCLUDE_DIR ev.h REQUIRED)
        find_library(GATEWAY_LIBEV_LIBRARY ev REQUIRED)
        find_path(GATEWAY_UCI_INCLUDE_DIR uci.h REQUIRED)
        find_path(GATEWAY_UBUS_INCLUDE_DIR libubus.h REQUIRED)
        find_library(GATEWAY_UCI_LIBRARY uci REQUIRED)
        find_library(GATEWAY_UBUS_LIBRARY ubus REQUIRED)
        find_library(GATEWAY_UBOX_LIBRARY ubox REQUIRED)
        add_executable(edge_modbus_gateway_config_test edge_modbus_gateway_config_test.c
            "${EDGENODE_ROOT}/src/edge_config.c" "${EDGENODE_ROOT}/src/edge_network.c"
            "${EDGENODE_ROOT}/src/edge_interface.c" "${EDGENODE_ROOT}/src/edge_process.c"
            "${EDGENODE_ROOT}/src/edge_protocol.c" "${EDGENODE_ROOT}/src/edge_url.c")
        target_compile_options(edge_modbus_gateway_config_test PRIVATE -UNDEBUG)
        target_link_options(edge_modbus_gateway_config_test PRIVATE -Wl,--wrap=uci_alloc_context)
        target_include_directories(edge_modbus_gateway_config_test PRIVATE
            "${EDGENODE_ROOT}/src" "${GATEWAY_UCI_INCLUDE_DIR}"
            "${GATEWAY_UBUS_INCLUDE_DIR}" "${GATEWAY_LIBEV_INCLUDE_DIR}")
        target_link_libraries(edge_modbus_gateway_config_test PRIVATE edgenode_nanopb
            "${GATEWAY_UCI_LIBRARY}" "${GATEWAY_UBUS_LIBRARY}" "${GATEWAY_UBOX_LIBRARY}" "${GATEWAY_LIBEV_LIBRARY}" m)
        add_test(NAME edge_modbus_gateway_config COMMAND edge_modbus_gateway_config_test)
        add_executable(edge_modbus_gateway_fixture edge_modbus_gateway_fixture.c
            "${EDGENODE_ROOT}/src/edge_modbus_gateway.c"
            "${EDGENODE_ROOT}/src/edge_modbus_gateway_transport.c"
            "${EDGENODE_ROOT}/src/edge_runtime_config.c"
            "${EDGENODE_ROOT}/src/edge_industrial.c"
            "${EDGENODE_ROOT}/src/edge_derived.c")
        target_include_directories(edge_modbus_gateway_fixture PRIVATE "${EDGENODE_ROOT}/src" "${GATEWAY_LIBEV_INCLUDE_DIR}")
        target_link_libraries(edge_modbus_gateway_fixture PRIVATE edgenode_nanopb edgenode_gateway_crypto "${GATEWAY_LIBEV_LIBRARY}" m)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME edge_modbus_gateway_network COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/edge_modbus_gateway_network_test.py" "$<TARGET_FILE:edge_modbus_gateway_fixture>")
        set_tests_properties(edge_modbus_gateway_network PROPERTIES TIMEOUT 15)
    endif()
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/sha256.h PATH_SUFFIXES mbedtls3)
    find_library(MBEDCRYPTO_LIBRARY mbedcrypto PATH_SUFFIXES mbedtls3)
    if(MBEDTLS_INCLUDE_DIR AND MBEDCRYPTO_LIBRARY)
        add_executable(edge_dtu_checkpoint_fixture edge_dtu_checkpoint_fixture.c
            "${EDGENODE_ROOT}/src/edge_spool.c" "${EDGENODE_ROOT}/src/edge_memory.c"
            "${EDGENODE_ROOT}/src/edge_outbox.c" "${EDGENODE_ROOT}/src/edge_protocol.c"
            "${EDGENODE_ROOT}/src/log.c")
        target_include_directories(edge_dtu_checkpoint_fixture PRIVATE "${EDGENODE_ROOT}/src" "${MBEDTLS_INCLUDE_DIR}")
        target_link_libraries(edge_dtu_checkpoint_fixture PRIVATE edgenode_nanopb "${MBEDCRYPTO_LIBRARY}")
        target_compile_definitions(edge_dtu_checkpoint_fixture PRIVATE EDGE_SPOOL_ROOT="spool" EDGE_DTU_CONFIG_ROOT="checkpoints")
        add_executable(edge_spool_io_test edge_spool_io_test.c
            "${EDGENODE_ROOT}/src/edge_spool.c" "${EDGENODE_ROOT}/src/edge_memory.c"
            "${EDGENODE_ROOT}/src/edge_outbox.c" "${EDGENODE_ROOT}/src/edge_protocol.c"
            "${EDGENODE_ROOT}/src/log.c")
        target_include_directories(edge_spool_io_test PRIVATE "${EDGENODE_ROOT}/src" "${MBEDTLS_INCLUDE_DIR}")
        target_link_libraries(edge_spool_io_test PRIVATE edgenode_nanopb "${MBEDCRYPTO_LIBRARY}")
        target_compile_definitions(edge_spool_io_test PRIVATE EDGE_SPOOL_ROOT="spool-io" EDGE_DTU_CONFIG_ROOT="checkpoints-io")
        add_test(NAME edge_spool_io COMMAND edge_spool_io_test)
    endif()
    add_executable(edge_dtu_fixture edge_dtu_fixture.c "${EDGENODE_ROOT}/src/edge_dtu.c")
    target_include_directories(edge_dtu_fixture PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_dtu_fixture PRIVATE edgenode_nanopb)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        if(TARGET edge_dtu_checkpoint_fixture)
            add_test(NAME edge_dtu_checkpoint COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/edge_dtu_checkpoint_test.py" "$<TARGET_FILE:edge_dtu_checkpoint_fixture>")
        endif()
        add_test(NAME edge_dtu COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/edge_dtu_test.py" "$<TARGET_FILE:edge_dtu_fixture>")
        add_test(NAME network_watchdog COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/network_watchdog_test.py")
    endif()
    add_executable(edge_terminal_test
        edge_terminal_test.c
        "${EDGENODE_ROOT}/src/edge_terminal.c")
    target_compile_definitions(edge_terminal_test PRIVATE EDGENODE_TERMINAL_TEST=1)
    target_include_directories(edge_terminal_test PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_terminal_test PRIVATE edgenode_nanopb)
    find_library(UTIL_LIBRARY util)
    if(UTIL_LIBRARY)
        target_link_libraries(edge_terminal_test PRIVATE "${UTIL_LIBRARY}")
    endif()
    add_test(NAME edge_terminal COMMAND edge_terminal_test)

    add_executable(edge_process_test
        edge_process_test.c
        "${EDGENODE_ROOT}/src/edge_process.c")
    target_include_directories(edge_process_test PRIVATE "${EDGENODE_ROOT}/src")
    add_test(NAME edge_process COMMAND edge_process_test)

    add_executable(edge_log_test
        edge_log_test.c
        "${EDGENODE_ROOT}/src/log.c"
        "${EDGENODE_ROOT}/src/edge_protocol.c")
    target_compile_definitions(edge_log_test PRIVATE
        clock_gettime=edge_log_test_clock_gettime
        EDGE_LOG_ROOT=\"/tmp/edgenode-test-local-diagnostics\"
        EDGE_LOG_SPACE_PATH=\"/tmp\")
    target_compile_options(edge_log_test PRIVATE -UNDEBUG)
    target_include_directories(edge_log_test PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_log_test PRIVATE edgenode_nanopb)
    add_test(NAME edge_log COMMAND edge_log_test)

    set(EDGENODE_ACQUISITION_TEST_SOURCES
        edge_external_io_stub.c
        "${EDGENODE_ROOT}/src/edge_sl651.c"
        "${EDGENODE_ROOT}/src/edge_acquisition.c"
        "${EDGENODE_ROOT}/src/edge_dtu.c"
        "${EDGENODE_ROOT}/src/edge_industrial.c"
    "${EDGENODE_ROOT}/src/edge_runtime_config.c"
    "${EDGENODE_ROOT}/src/edge_derived.c"
        "${EDGENODE_ROOT}/src/edge_device_runtime.c"
        "${EDGENODE_ROOT}/src/edge_modbus.c"
        "${EDGENODE_ROOT}/src/edge_s7.c"
        "${EDGENODE_ROOT}/src/edge_protocol.c"
        "${EDGENODE_ROOT}/src/edge_process.c"
        "${EDGENODE_ROOT}/src/log.c")
    add_executable(edge_acquisition_test edge_acquisition_test.c
        ${EDGENODE_ACQUISITION_TEST_SOURCES})
    target_compile_definitions(edge_acquisition_test PRIVATE
        EDGE_LOG_ROOT=\"/tmp/edgenode-test-acquisition\"
        EDGE_LOG_SPACE_PATH=\"/tmp\")
    target_include_directories(edge_acquisition_test PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_acquisition_test PRIVATE edgenode_nanopb m)
    add_test(NAME edge_acquisition COMMAND edge_acquisition_test)
    add_test(NAME edge_acquisition_external_io COMMAND edge_acquisition_test)
    set_tests_properties(edge_acquisition_external_io PROPERTIES ENVIRONMENT "EDGE_TEST_EXTERNAL_IO_ONLY=1" TIMEOUT 20)
    if(EDGENODE_GATEWAY_NETWORK_TESTS)
        find_path(GATEWAY_LWS_INCLUDE_DIR libwebsockets.h REQUIRED)
        find_path(GATEWAY_LWS_CONFIG_INCLUDE_DIR lws_config.h REQUIRED)
        add_executable(edge_modbus_gateway_write_test edge_modbus_gateway_write_test.c
            ${EDGENODE_ACQUISITION_TEST_SOURCES}
            "${EDGENODE_ROOT}/src/edge_modbus_gateway.c"
            "${EDGENODE_ROOT}/src/edge_modbus_gateway_transport.c")
        target_compile_options(edge_modbus_gateway_write_test PRIVATE
            -ffunction-sections -fdata-sections -UNDEBUG)
        target_link_options(edge_modbus_gateway_write_test PRIVATE -Wl,--gc-sections)
        target_compile_definitions(edge_modbus_gateway_write_test PRIVATE
            EDGE_LOG_ROOT=\"gateway-write-log\" EDGE_LOG_SPACE_PATH=\".\")
        target_include_directories(edge_modbus_gateway_write_test PRIVATE
            "${EDGENODE_ROOT}/src" "${GATEWAY_LIBEV_INCLUDE_DIR}"
            "${GATEWAY_LWS_INCLUDE_DIR}" "${GATEWAY_LWS_CONFIG_INCLUDE_DIR}")
        target_link_libraries(edge_modbus_gateway_write_test PRIVATE
            edgenode_nanopb edgenode_gateway_crypto "${GATEWAY_LIBEV_LIBRARY}" m)
        add_test(NAME edge_modbus_gateway_write COMMAND edge_modbus_gateway_write_test)
        set_tests_properties(edge_modbus_gateway_write PROPERTIES TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

add_executable(edge_sl651_test edge_sl651_test.c "${EDGENODE_ROOT}/src/edge_sl651.c")
target_include_directories(edge_sl651_test PRIVATE "${EDGENODE_ROOT}/src")
if(NOT MSVC)
    target_link_libraries(edge_sl651_test PRIVATE m)
endif()
add_test(NAME edge_sl651 COMMAND edge_sl651_test)

add_executable(edge_industrial_test edge_industrial_test.c "${EDGENODE_ROOT}/src/edge_industrial.c")
target_include_directories(edge_industrial_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_industrial_test PRIVATE edgenode_nanopb)
add_test(NAME edge_industrial COMMAND edge_industrial_test)

if(NOT MSVC)
    target_link_libraries(edge_config_test PRIVATE m)
    target_link_libraries(edgenode_stability_x64 PRIVATE m)
endif()

add_executable(edge_derived_test edge_derived_test.c "${EDGENODE_ROOT}/src/edge_derived.c")
target_include_directories(edge_derived_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_derived_test PRIVATE edgenode_nanopb)
if(NOT MSVC)
    target_link_libraries(edge_derived_test PRIVATE m)
endif()
add_test(NAME edge_derived COMMAND edge_derived_test)
