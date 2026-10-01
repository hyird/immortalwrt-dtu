#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "edge_sl651.h"
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edge_acquisition.h"

static uint64_t monotonic_ms(void) {
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void set_id(void *field, const uint8_t id[16]) {
    const pb_size_t size = 16U;
    memcpy(field, &size, sizeof(size));
    memcpy((uint8_t *)field + sizeof(size), id, 16U);
}

static void copy_text(char *output, size_t capacity, const char *input) {
    snprintf(output, capacity, "%s", input);
}

static void receive_modbus_request(int fd, uint8_t request[12]);

static bool raw_fixture;

static bool telemetry(void *context, const uint8_t platform_id[16],
                      const iot_edge_v1_TelemetryRecord *record) {
    (void)context;
    (void)platform_id;
    (void)record;
    return true;
}

static bool command(void *context, const uint8_t platform_id[16],
                    const iot_edge_v1_CommandResult *result) {
    (void)context;
    (void)platform_id;
    (void)result;
    return true;
}

static edge_runtime_config make_config(iot_edge_v1_ConfigItem values[3]) {
    const uint8_t endpoint_id[16] = {1U};
    const uint8_t device_id[16] = {2U};
    values[0] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    values[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    set_id(&values[0].item.endpoint.endpoint_id, endpoint_id);
    copy_text(values[0].item.endpoint.name, sizeof(values[0].item.endpoint.name),
              "offline endpoint");
    values[0].item.endpoint.transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    values[0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    copy_text(values[0].item.endpoint.ip, sizeof(values[0].item.endpoint.ip),
              "127.0.0.1");
    values[0].item.endpoint.port = 1U;
    values[0].item.endpoint.enabled = true;

    values[1] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[1].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DEVICE;
    values[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    set_id(&values[1].item.device.device_id, device_id);
    set_id(&values[1].item.device.endpoint_id, endpoint_id);
    copy_text(values[1].item.device.device_code,
              sizeof(values[1].item.device.device_code), "OFFLINE01");
    values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    values[1].item.device.io_interval_ms = 1000U;
    values[1].item.device.report_interval_sec = 5U;
    values[1].item.device.modbus_slave_id = 1U;
    copy_text(values[1].item.device.modbus_mode,
              sizeof(values[1].item.device.modbus_mode), "TCP");
    values[1].item.device.enabled = true;

    values[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_MODBUS_REGISTER;
    values[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    set_id(&values[2].item.modbus_register.device_id, device_id);
    copy_text(values[2].item.modbus_register.element_id,
              sizeof(values[2].item.modbus_register.element_id), "holding-1");
    copy_text(values[2].item.modbus_register.register_type,
              sizeof(values[2].item.modbus_register.register_type),
              "HOLDING_REGISTER");
    copy_text(values[2].item.modbus_register.data_type,
              sizeof(values[2].item.modbus_register.data_type), "UINT16");
    copy_text(values[2].item.modbus_register.byte_order,
              sizeof(values[2].item.modbus_register.byte_order), "BIG_ENDIAN");
    values[2].item.modbus_register.quantity = 1U;
    values[2].item.modbus_register.scale = 1.0;

    return (edge_runtime_config){
        .revision = 1U,
        .items = values,
        .item_count = 3U,
        .endpoint_count = 1U,
        .device_count = 1U,
    };
}

static void wait_for_status(edge_acquisition *acquisition) {
    const int fd = edge_acquisition_event_fd(acquisition);
    assert(fd >= 0);
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    assert(poll(&descriptor, 1U, 3000) == 1);
    edge_acquisition_tick(acquisition, monotonic_ms());
    iot_edge_v1_DeviceStatusReport report =
        iot_edge_v1_DeviceStatusReport_init_zero;
    edge_acquisition_status(acquisition, &report);
    assert(report.devices_count == 1U);
    assert(report.devices[0].device_id.size == 16U);
    assert(report.devices[0].device_id.bytes[0] == 2U);
    assert(strcmp(report.devices[0].state, "reconnecting") == 0);
}

static void make_serial_config(iot_edge_v1_ConfigItem values[3], uint32_t baud_rate) {
    (void)make_config(values);
    iot_edge_v1_EndpointConfig *endpoint = &values[0].item.endpoint;
    endpoint->transport = iot_edge_v1_Transport_TRANSPORT_SERIAL;
    endpoint->mode = iot_edge_v1_LinkMode_LINK_MODE_SERIAL;
    endpoint->has_serial = true;
    copy_text(endpoint->serial.channel, sizeof(endpoint->serial.channel), "/dev/ttyS1");
    endpoint->serial.baud_rate = baud_rate;
    endpoint->serial.data_bits = 8U;
    endpoint->serial.stop_bits = 1U;
    copy_text(endpoint->serial.parity, sizeof(endpoint->serial.parity), "none");
    copy_text(values[1].item.device.modbus_mode,
              sizeof(values[1].item.device.modbus_mode), "RTU");
}

static void verify_shared_resources(void) {
    iot_edge_v1_ConfigItem items[4][3];
    edge_runtime_config configs[4];
    configs[0] = make_config(items[0]);
    configs[1] = make_config(items[1]);
    items[0][0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    items[1][0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    items[0][0].item.endpoint.port = 35000U;
    items[1][0].item.endpoint.port = 35000U;
    copy_text(items[0][0].item.endpoint.ip, sizeof(items[0][0].item.endpoint.ip),
              "0.0.0.0");
    copy_text(items[1][0].item.endpoint.ip, sizeof(items[1][0].item.endpoint.ip),
              "127.0.0.1");
    make_serial_config(items[2], 9600U);
    make_serial_config(items[3], 115200U);
    configs[2] = (edge_runtime_config){
        .revision = 1U, .items = items[2], .item_count = 3U,
        .endpoint_count = 1U, .device_count = 1U};
    configs[3] = (edge_runtime_config){
        .revision = 1U, .items = items[3], .item_count = 3U,
        .endpoint_count = 1U, .device_count = 1U};
    uint8_t platform_ids[4][16] = {{1U}, {2U}, {3U}, {4U}};
    edge_acquisition_source sources[4];
    for (size_t index = 0U; index < 4U; ++index) {
        sources[index] = (edge_acquisition_source){
            .platform_id = platform_ids[index],
            .priority = (uint16_t)(100U - index),
            .bootstrap = index == 0U,
            .config = &configs[index],
        };
    }
    edge_acquisition *acquisition = edge_acquisition_create(telemetry, command, NULL);
    assert(acquisition != NULL);
    char error[256] = {0};
    assert(edge_acquisition_apply_multi(acquisition, sources, 4U, monotonic_ms(),
                                        error, sizeof(error)));
    assert(edge_acquisition_device_count(acquisition) == 4U);
    assert(edge_acquisition_resource_count(acquisition) == 2U);
    edge_acquisition_destroy(acquisition);
}

static bool sl651_allow_report, sl651_allow_command, sl651_expect_failure;
static bool sl651_last_ack_missing;
static unsigned sl651_reports, sl651_results, sl651_images;
static unsigned sl651_raw_frames;
static bool sl651_store_report(void *context, const uint8_t platform[16], const iot_edge_v1_TelemetryRecord *record) {
    (void)context; (void)platform;
    assert(record->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651);
    assert(record->report_id.size == 16 && record->part_count >= (raw_fixture ? 1U : 2U) && record->part_index < record->part_count);
    if (record->raw_payloads_count) {
        assert(record->values_count == 0);
        if (raw_fixture) {
            assert(record->raw_requests_count == record->raw_payloads_count);
            for (pb_size_t i = 0; i < record->raw_requests_count; ++i) {
                const pb_bytes_array_t *request = record->raw_requests[i];
                if (!strcmp(record->function_code, "4C") && i == 0) {
                    assert(request->size == 27 && request->bytes[10] == 0x4C && request->bytes[24] == 5);
                } else assert(request->size == 0);
            }
            if (!strcmp(record->function_code, "36")) ++sl651_images;
            else ++sl651_reports;
        }
        for (pb_size_t index = 0; index < record->raw_payloads_count; ++index) {
            edge_sl651_frame frame;
            assert(edge_sl651_parse(record->raw_payloads[index]->bytes,
                                     record->raw_payloads[index]->size, &frame));
            if (frame.total)
                assert(frame.sequence == index + 1);
            ++sl651_raw_frames;
        }
        return sl651_allow_report;
    }
    assert(record->values_count == 1);
    if (!strcmp(record->function_code, "36")) {
        assert(!record->values[0].has_value && record->values[0].encoded_value);
        assert(!strcmp(record->values[0].encoding, "JPEG"));
        assert(record->values[0].encoded_value->size == 8192 &&
               record->values[0].encoded_value->bytes[0] == 0xFF &&
               record->values[0].encoded_value->bytes[8191] == 0xD9);
        ++sl651_images;
    } else assert(record->values[0].has_value && record->values[0].value.which_value == iot_edge_v1_ScalarValue_double_value_tag);
    ++sl651_reports; return sl651_allow_report;
}
static bool sl651_store_command(void *context, const uint8_t platform[16], const iot_edge_v1_CommandResult *result) {
    (void)context; (void)platform;
    assert(result->state == (sl651_expect_failure
        ? iot_edge_v1_CommandState_COMMAND_STATE_FAILED
        : iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED));
    sl651_last_ack_missing = result->write_ack_missing;
    ++sl651_results; return sl651_allow_command;
}
static void sl651_pump(edge_acquisition *acquisition, unsigned milliseconds) {
    uint64_t until = monotonic_ms() + milliseconds;
    while (monotonic_ms() < until) { edge_acquisition_tick(acquisition, monotonic_ms()); usleep(10000); }
}
static ssize_t sl651_read(edge_acquisition *acquisition, int fd, uint8_t *bytes, size_t capacity) {
    uint64_t until = monotonic_ms() + 6000;
    while (monotonic_ms() < until) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        if (poll(&ready, 1, 10) > 0) return read(fd, bytes, capacity);
    }
    return -1;
}
static void sl651_send_report(int fd, uint8_t function, uint8_t serial, uint8_t ending) {
    uint8_t bytes[] = {0x7E,0x7E,1,0,0,0,0,1,0,0,0,0,10,2,0,0,0x24,1,2,3,4,5,0x12,0x34,3,0,0};
    bytes[10] = function; bytes[15] = serial; bytes[sizeof(bytes) - 3U] = ending;
    uint16_t crc = edge_sl651_crc(bytes, sizeof(bytes) - 2);
    bytes[sizeof(bytes)-2] = (uint8_t)(crc >> 8U); bytes[sizeof(bytes)-1] = (uint8_t)crc;
    assert(write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
}
static void verify_sl651_commit(void) {
    sl651_allow_report = sl651_allow_command = false;
    sl651_reports = sl651_results = sl651_images = sl651_raw_frames = 0;
    int reservation = socket(AF_INET, SOCK_STREAM, 0); assert(reservation >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(reservation, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_size = sizeof(address); assert(getsockname(reservation, (struct sockaddr *)&address, &address_size) == 0);
    iot_edge_v1_ConfigItem values[5]; edge_runtime_config config = make_config(values);
    values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_SL651;
    values[0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    values[0].item.endpoint.port = ntohs(address.sin_port);
    values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_SL651;
    values[1].item.device.sl651_response_mode = 2;
    strcpy(values[1].item.device.device_code, "0000000001"); strcpy(values[1].item.device.timezone, "+08:00");
    values[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_SL651_ELEMENT;
    values[2].which_item = iot_edge_v1_ConfigItem_sl651_element_tag;
    iot_edge_v1_Sl651ElementConfig *element = &values[2].item.sl651_element;
    const uint8_t device_id[16] = {2}; set_id(&element->device_id, device_id);
    strcpy(element->element_id, "water"); strcpy(element->function_code, "32"); strcpy(element->encoding, "BCD");
    element->fixed_position = true; element->byte_offset = 8; element->length = 2; element->digits = 2;
    values[3] = values[2]; strcpy(values[3].item.sl651_element.element_id, "write");
    strcpy(values[3].item.sl651_element.function_code, "4C"); values[3].item.sl651_element.writable = true;
    values[4] = values[2];
    strcpy(values[4].item.sl651_element.element_id, "image");
    strcpy(values[4].item.sl651_element.function_code, "36");
    strcpy(values[4].item.sl651_element.encoding, "JPEG");
    values[4].item.sl651_element.fixed_position = false;
    values[4].item.sl651_element.length = 0;
    values[4].item.sl651_element.guide.size = 2;
    memset(values[4].item.sl651_element.guide.bytes, 0xF3, 2);
    config.item_count = 5;
    edge_acquisition *acquisition = edge_acquisition_create(sl651_store_report, sl651_store_command, NULL); assert(acquisition);
    char error[256] = {0}; assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(), error, sizeof(error)));
    const uint8_t platform[16] = {0};
    assert(edge_acquisition_set_raw_telemetry(acquisition, platform, raw_fixture));
    close(reservation); assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    int fd = -1;
    for (unsigned attempt = 0; attempt < 60; ++attempt) {
        fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) break;
        close(fd); fd = -1; sl651_pump(acquisition, 100);
    }
    assert(fd >= 0); sl651_send_report(fd, 0x32, 1, 3);
    uint64_t until = monotonic_ms() + 5000;
    while (!sl651_reports && monotonic_ms() < until) sl651_pump(acquisition, 20);
    assert(sl651_reports); struct pollfd ready = {.fd = fd, .events = POLLIN};
    assert(poll(&ready, 1, 100) == 0); // IPC delivery alone must never confirm M2.
    sl651_allow_report = true; uint8_t bytes[128]; ssize_t n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(n >= 25 && bytes[n-3] == 4 && bytes[15] == 1);
    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {9}; set_id(&request.command_id, command_id); set_id(&request.device_id, device_id);
    request.values_count = 1; strcpy(request.values[0].element_id, "write"); request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value = iot_edge_v1_ScalarValue_string_value_tag;
    strcpy(request.values[0].expected.value.string_value, "12.34"); request.timeout_ms = 10000;
    request.has_start_before_ms = true; request.start_before_ms = 1;
    const uint8_t platform_id[16] = {0};
    assert(edge_acquisition_command_for_platform_until(acquisition, platform_id,
        &request, monotonic_ms() + 30000U, UINT64_MAX, error, sizeof(error)));
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes)); assert(n == 27 && bytes[n-3] == 5 && bytes[14] == 0 && bytes[15] == 0 && bytes[22] == 0x12);
    sl651_send_report(fd, 0x4C, 2, 3); until = monotonic_ms() + 5000;
    while (!sl651_results && monotonic_ms() < until) sl651_pump(acquisition, 20);
    assert(sl651_results && poll(&ready, 1, 100) == 0);
    sl651_allow_command = true; n = sl651_read(acquisition, fd, bytes, sizeof(bytes)); assert(n >= 25 && bytes[n-3] == 4 && bytes[15] == 2);

    const unsigned prior_results = sl651_results;
    const uint8_t continuous_command_id[16] = {10};
    set_id(&request.command_id, continuous_command_id);
    request.timeout_ms = 100U;
    const uint64_t continuous_start = monotonic_ms();
    assert(edge_acquisition_command_for_platform_until(
        acquisition, platform_id, &request, continuous_start + 30000U,
        continuous_start + 30000U, error, sizeof(error)));
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(n == 27 && bytes[n - 3] == 5 && bytes[10] == 0x4C);
    sl651_send_report(fd, 0x4C, 3, 0x17);
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(n == 25 && bytes[n - 3] == 6 && bytes[10] == 0x4C);
    /* The station repeats the continuation after its first ACK is lost. It gets
     * another ACK, never a resend of the already-started control request. */
    sl651_send_report(fd, 0x4C, 3, 0x17);
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(n == 25 && bytes[n - 3] == 6 && bytes[10] == 0x4C);
    sl651_send_report(fd, 0x4C, 4, 3);
    until = monotonic_ms() + 5000U;
    while (sl651_results == prior_results && monotonic_ms() < until)
        sl651_pump(acquisition, 20U);
    assert(sl651_results == prior_results + 1U);
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(n >= 25 && bytes[n - 3] == 4);

    const unsigned before_timeout = sl651_results;
    const uint8_t timeout_command_id[16] = {11};
    set_id(&request.command_id, timeout_command_id);
    sl651_expect_failure = true;
    assert(edge_acquisition_command_for_platform_until(
        acquisition, platform_id, &request, monotonic_ms() + 30000U,
        monotonic_ms() + 30000U, error, sizeof(error)));
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(n == 27 && bytes[n - 3] == 5 && bytes[10] == 0x4C);
    until = monotonic_ms() + 11000U;
    while (sl651_results == before_timeout && monotonic_ms() < until) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        struct pollfd no_resend = {.fd = fd, .events = POLLIN};
        assert(poll(&no_resend, 1, 0) == 0);
        usleep(10000U);
    }
    assert(sl651_results > before_timeout && sl651_last_ack_missing);
    sl651_expect_failure = false;

    uint8_t image_body[8202] = {0,3,0x24,1,2,3,4,5,0xF3,0xF3,0xFF,0xD8};
    image_body[8200] = 0xFF; image_body[8201] = 0xD9;
    size_t offset = 0;
    for (unsigned sequence = 1; sequence <= 3; ++sequence) {
        size_t length = sizeof(image_body) - offset;
        if (length > 3000) length = 3000;
        uint8_t packet[3020] = {0x7E,0x7E,1,0,0,0,0,1,0,0,0x36};
        packet[11] = (uint8_t)((length + 3) >> 8); packet[12] = (uint8_t)(length + 3);
        packet[13] = 0x16; packet[14] = 0; packet[15] = 0x30; packet[16] = (uint8_t)sequence;
        memcpy(packet + 17, image_body + offset, length);
        packet[17 + length] = sequence == 3 ? 3 : 0x17;
        uint16_t crc = edge_sl651_crc(packet, 18 + length);
        packet[18 + length] = (uint8_t)(crc >> 8); packet[19 + length] = (uint8_t)crc;
        assert(write(fd, packet, length + 20) == (ssize_t)(length + 20));
        offset += length;
    }
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes));
    assert(sl651_images && sl651_raw_frames >= 3 && n == 28 && bytes[n-3] == 4 && bytes[16] == 3);
    close(fd); edge_acquisition_destroy(acquisition);
}

static unsigned response_records, debug_values;
static uint8_t debug_response_ids[16][16];
static unsigned debug_response_count;
static uint8_t expected_responses[2][27];
static size_t expected_response_size;
static bool s7_responses;

static bool store_response_record(void *context, const uint8_t platform_id[16],
                                  const iot_edge_v1_TelemetryRecord *record) {
    (void)context;
    (void)platform_id;
    assert(response_records++ == 0U);
    assert(record->protocol == (s7_responses ? iot_edge_v1_Protocol_PROTOCOL_S7
                                            : iot_edge_v1_Protocol_PROTOCOL_MODBUS));
    assert(record->values_count == (raw_fixture ? 0U : s7_responses ? 2U : 3U));
    if (!raw_fixture) {
    assert(strcmp(record->values[0].element_id, "holding-1") == 0);
    assert(record->values[0].value.value.double_value == 100.0);
    assert(strcmp(record->values[1].element_id, "holding-2") == 0);
    assert(record->values[1].value.value.double_value == 101.0);
    if (!s7_responses) {
        assert(strcmp(record->values[2].element_id, "holding-copy") == 0);
        assert(record->values[2].value.value.double_value == 100.0);
    }
    }
    assert(record->raw_payloads_count == 2 && record->raw_packet_ids_count == 2);
    assert(record->raw_requests_count == (raw_fixture ? 2U : 0U));
    for (unsigned index = 0; index < 2; ++index) {
        assert(record->raw_payloads[index]->size == expected_response_size);
        assert(memcmp(record->raw_payloads[index]->bytes, expected_responses[index], expected_response_size) == 0);
        assert(record->raw_packet_ids[index]->size == 16);
        if (raw_fixture) {
            const pb_bytes_array_t *request = record->raw_requests[index];
            assert(request->size == (s7_responses ? 31U : 12U));
            assert(request->bytes[s7_responses ? 17 : 7] == (s7_responses ? 4U : 3U));
            assert(request->bytes[s7_responses ? 30 : 9] == (index ? s7_responses ? 80U : 100U : 0U));
        }
    }
    assert(memcmp(record->raw_packet_ids[0]->bytes, record->raw_packet_ids[1]->bytes, 16) != 0);
    assert(record->observed_at_ms > 0);
    return true;
}

static void receive_s7_request(int fd, uint8_t request[1024]) {
    assert(recv(fd, request, 4, MSG_WAITALL) == 4);
    const size_t size = (size_t)request[2] * 256U + request[3];
    assert(size >= 4 && size <= 1024);
    assert(recv(fd, request + 4, size - 4, MSG_WAITALL) == (ssize_t)(size - 4));
}

static unsigned debug_rx, debug_tx, debug_success;
static void record_debug(void *context, const uint8_t platform_id[16], const iot_edge_v1_RawPacket *packet) {
    (void)context; (void)platform_id;
    if (!strcmp(packet->status, "success")) ++debug_success;
    assert(packet->debug && packet->packet_id.size == 16 && packet->device_id.size == 16);
    assert(packet->device_id.bytes[0] == 2 && packet->endpoint_id.bytes[0] == 1);
    assert(packet->acquisition_id.size == 16);
    if (packet->has_parsed_value) {
        assert(!strcmp(packet->direction, "RX"));
        assert(response_records == 0);
        assert(packet->parsed_value.has_value);
        assert(packet->parsed_value.value.value.double_value ==
            (!strcmp(packet->parsed_value.element_id, "holding-2") ? 101.0 : 100.0));
        bool matched = false;
        for (unsigned i = 0; i < debug_response_count; ++i)
            if (!memcmp(packet->packet_id.bytes, debug_response_ids[i], 16)) matched = true;
        assert(matched);
        ++debug_values;
        return;
    }
    if (!packet->payload.size) {
        assert(!strcmp(packet->acquisition_state, "running") || !strcmp(packet->acquisition_state, "success") ||
               !strcmp(packet->acquisition_state, "partial") || !strcmp(packet->acquisition_state, "failed"));
        return;
    }
    assert(packet->payload.size <= 4096);
    if (!strcmp(packet->direction, "RX")) {
        debug_rx += packet->payload.size;
        assert(debug_response_count < 16);
        memcpy(debug_response_ids[debug_response_count++], packet->packet_id.bytes, 16);
    }
    else { assert(!strcmp(packet->direction, "TX")); debug_tx += packet->payload.size; }
}
static void verify_complete_acquisition_record(bool s7, bool link_debug, bool device_debug) {
    debug_rx = debug_tx = debug_success = debug_values = debug_response_count = 0;
    response_records = 0;
    s7_responses = s7;
    expected_response_size = s7 ? 27 : 11;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    assert(listen(listener, 1) == 0);
    iot_edge_v1_ConfigItem values[5];
    edge_runtime_config config = make_config(values);
    values[0].item.endpoint.port = ntohs(address.sin_port);
    values[1].item.device.report_interval_sec = 1U;
    values[0].item.endpoint.debug_enabled = link_debug;
    values[1].item.device.debug_enabled = device_debug;
    values[3] = values[2];
    values[3].item.modbus_register.address = 100;
    copy_text(values[3].item.modbus_register.element_id,
              sizeof(values[3].item.modbus_register.element_id), "holding-2");
    config.item_count = 4;
    if (!s7) {
        values[4] = values[2];
        copy_text(values[4].item.modbus_register.element_id,
                  sizeof(values[4].item.modbus_register.element_id), "holding-copy");
        config.item_count = 5;
    }
    if (s7) {
        values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_S7;
        values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_S7;
        values[1].item.device.io_interval_ms = 300000U;
        for (unsigned index = 2; index < 4; ++index) {
            values[index] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
            values[index].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_S7_AREA;
            values[index].which_item = iot_edge_v1_ConfigItem_s7_area_tag;
            iot_edge_v1_S7AreaConfig *point = &values[index].item.s7_area;
            set_id(&point->device_id, values[1].item.device.device_id.bytes);
            copy_text(point->element_id, sizeof(point->element_id),
                      index == 2 ? "holding-1" : "holding-2");
            copy_text(point->area, sizeof(point->area), "DB");
            copy_text(point->data_type, sizeof(point->data_type), "UINT16");
            point->db_number = 1;
            point->start = (index - 2) * 10;
            point->size = 2;
            point->scale = 1.0;
        }
    }
    edge_acquisition *acquisition = edge_acquisition_create(store_response_record, command, NULL);
    edge_acquisition_set_debug_callback(acquisition, record_debug);
    assert(acquisition != NULL);
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(), error, sizeof(error)));
    const uint8_t platform[16] = {0};
    assert(edge_acquisition_set_raw_telemetry(acquisition, platform, raw_fixture));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    struct pollfd connection = {.fd = listener, .events = POLLIN};
    assert(poll(&connection, 1, 3000) == 1);
    int fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    struct timeval timeout = {.tv_sec = 3};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    if (s7) {
        uint8_t request[1024];
        receive_s7_request(fd, request);
        const uint8_t cotp[] = {3,0,0,11,6,0xd0,0,1,0,6,0};
        assert(send(fd, cotp, sizeof(cotp), 0) == (ssize_t)sizeof(cotp));
        receive_s7_request(fd, request);
        uint8_t setup[] = {3,0,0,27,2,0xf0,0x80,0x32,3,0,0,0,0,0,8,0,0,0,0,
                           0xf0,0,0,1,0,1,1,0xe0};
        setup[11] = request[11]; setup[12] = request[12];
        assert(send(fd, setup, sizeof(setup), 0) == (ssize_t)sizeof(setup));
    }
    for (unsigned index = 0; index < 4; ++index) {
        if (s7) {
            uint8_t request[1024];
            receive_s7_request(fd, request);
            assert(request[17] == 4);
            uint8_t response[] = {3,0,0,27,2,0xf0,0x80,0x32,3,0,0,0,0,0,2,0,6,0,0,
                                  4,1,0xff,4,0,16,0,0};
            response[11] = request[11]; response[12] = request[12];
            response[26] = (uint8_t)(100 + index % 2U);
            memcpy(expected_responses[index % 2U], response, sizeof(response));
            assert(send(fd, response, sizeof(response), 0) == (ssize_t)sizeof(response));
            continue;
        }
        uint8_t request[12];
        assert(recv(fd, request, sizeof(request), MSG_WAITALL) == (ssize_t)sizeof(request));
        assert(request[7] == 3);
        assert(request[9] == (index % 2U == 0U ? 0 : 100));
        uint8_t *response = expected_responses[index % 2U];
        memcpy(response, request, 7);
        response[5] = 5;
        response[7] = 3;
        response[8] = 2;
        response[9] = 0;
        response[10] = (uint8_t)(100 + index % 2U);
        assert(send(fd, response, 11, 0) == 11);
    }
    const uint64_t deadline = monotonic_ms() + 3000;
    while (response_records < 1 && monotonic_ms() < deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition), .events = POLLIN};
        (void)poll(&event, 1, 100);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(response_records == 1);
    if (link_debug || device_debug) { assert(debug_rx >= expected_response_size * 2 && debug_tx > 0); assert(debug_success >= 2); assert(debug_values == (raw_fixture ? 0U : s7 ? 2U : 3U)); }
    else { assert(debug_rx == 0 && debug_tx == 0 && debug_values == 0); }
    if (s7) {
        iot_edge_v1_DeviceStatusReport status = iot_edge_v1_DeviceStatusReport_init_zero;
        edge_acquisition_status(acquisition, &status);
        assert(status.devices_count == 1U &&
               strcmp(status.devices[0].state, "connected") == 0 &&
               status.devices[0].client_count == 1U);
    } else {
        uint8_t byte = 0U;
        assert(recv(fd, &byte, 1U, MSG_PEEK | MSG_DONTWAIT) == -1 &&
               (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    edge_acquisition_destroy(acquisition);
    close(fd);
    close(listener);
}

#define TEST_S7_POINT_COUNT 11U

typedef enum {
    TEST_S7_RESPONSE_VALID,
    TEST_S7_RESPONSE_SHORT,
    TEST_S7_RESPONSE_WRONG_TRANSPORT,
    TEST_S7_RESPONSE_UNDECODABLE,
} test_s7_response_case;

static unsigned s7_test_records;
static unsigned s7_invalid_responses;

static int open_s7_listener(struct sockaddr_in *address) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)address, sizeof(*address)) == 0);
    socklen_t length = sizeof(*address);
    assert(getsockname(listener, (struct sockaddr *)address, &length) == 0);
    assert(listen(listener, 4) == 0);
    return listener;
}

static edge_runtime_config make_s7_config(iot_edge_v1_ConfigItem *items,
                                           uint16_t port, size_t point_count,
                                           bool undecodable_last_point,
                                           uint32_t report_interval_sec) {
    edge_runtime_config config = make_config(items);
    items[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_S7;
    items[0].item.endpoint.port = port;
    items[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_S7;
    items[1].item.device.io_interval_ms = 300000U;
    items[1].item.device.report_interval_sec = report_interval_sec;
    for (size_t index = 0U; index < point_count; ++index) {
        iot_edge_v1_ConfigItem *item = &items[index + 2U];
        *item = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
        item->kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_S7_AREA;
        item->which_item = iot_edge_v1_ConfigItem_s7_area_tag;
        iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        set_id(&point->device_id, items[1].item.device.device_id.bytes);
        snprintf(point->element_id, sizeof(point->element_id), "s7-%02zu", index);
        copy_text(point->area, sizeof(point->area), "DB");
        copy_text(point->data_type, sizeof(point->data_type),
                  undecodable_last_point && index + 1U == point_count
                      ? "FLOAT32" : "UINT16");
        point->db_number = 1U;
        point->start = (uint32_t)(index * 4U);
        point->size = 2U;
        point->scale = 1.0;
        point->decimals = -1;
    }
    config.item_count = (uint32_t)(point_count + 2U);
    return config;
}

static bool count_s7_telemetry(void *context, const uint8_t platform_id[16],
                               const iot_edge_v1_TelemetryRecord *record) {
    (void)context;
    (void)platform_id;
    assert(record->protocol == iot_edge_v1_Protocol_PROTOCOL_S7);
    ++s7_test_records;
    return true;
}

static bool verify_s7_full_telemetry(void *context, const uint8_t platform_id[16],
                                     const iot_edge_v1_TelemetryRecord *record) {
    (void)context;
    (void)platform_id;
    assert(record->protocol == iot_edge_v1_Protocol_PROTOCOL_S7);
    assert(record->values_count == TEST_S7_POINT_COUNT);
    assert(record->raw_payloads_count == TEST_S7_POINT_COUNT);
    assert(record->raw_packet_ids_count == TEST_S7_POINT_COUNT);
    for (size_t index = 0U; index < TEST_S7_POINT_COUNT; ++index) {
        char element_id[32];
        snprintf(element_id, sizeof(element_id), "s7-%02zu", index);
        const iot_edge_v1_TelemetryValue *value = NULL;
        for (pb_size_t candidate = 0U; candidate < record->values_count; ++candidate)
            if (strcmp(record->values[candidate].element_id, element_id) == 0)
                value = &record->values[candidate];
        assert(value != NULL && value->has_value);
        assert(value->value.which_value == iot_edge_v1_ScalarValue_unsigned_value_tag);
        assert(value->value.value.unsigned_value == 100U + index);
        assert(record->raw_payloads[index]->size == 27U);
        assert(record->raw_packet_ids[index]->size == 16U);
        for (size_t previous = 0U; previous < index; ++previous)
            assert(memcmp(record->raw_packet_ids[index]->bytes,
                          record->raw_packet_ids[previous]->bytes, 16U) != 0);
    }
    ++s7_test_records;
    return true;
}

static void s7_set_timeout(int fd) {
    const struct timeval timeout = {.tv_sec = 3};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
}

static void s7_send_bytes(int fd, const uint8_t *bytes, size_t size) {
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t sent = send(fd, bytes + offset, size - offset, 0);
        assert(sent > 0);
        offset += (size_t)sent;
    }
}

static void s7_server_handshake(int fd) {
    uint8_t request[1024];
    receive_s7_request(fd, request);
    assert(request[4] == 0x11U && request[5] == 0xe0U);
    const uint8_t cotp[] = {3,0,0,11,6,0xd0,0,1,0,6,0};
    s7_send_bytes(fd, cotp, sizeof(cotp));
    receive_s7_request(fd, request);
    assert(request[17] == 0xf0U);
    uint8_t setup[] = {3,0,0,27,2,0xf0,0x80,0x32,3,0,0,0,0,0,8,0,0,0,0,
                       0xf0,0,0,1,0,1,1,0xe0};
    setup[11] = request[11];
    setup[12] = request[12];
    s7_send_bytes(fd, setup, sizeof(setup));
}

static void s7_send_read_response(int fd, const uint8_t request[1024],
                                  size_t point_index,
                                  test_s7_response_case response_case) {
    uint8_t response[] = {3,0,0,27,2,0xf0,0x80,0x32,3,0,0,0,0,0,2,0,6,0,0,
                          4,1,0xff,4,0,16,0,0};
    response[11] = request[11];
    response[12] = request[12];
    response[26] = (uint8_t)(100U + point_index);
    if (response_case == TEST_S7_RESPONSE_SHORT) {
        response[3] = 26U;
        response[16] = 5U;
        s7_send_bytes(fd, response, sizeof(response) - 1U);
        ++s7_invalid_responses;
        return;
    }
    if (response_case == TEST_S7_RESPONSE_WRONG_TRANSPORT)
        response[22] = 0x03U;
    s7_send_bytes(fd, response, sizeof(response));
    if (response_case != TEST_S7_RESPONSE_VALID)
        ++s7_invalid_responses;
}

static int accept_s7_with_ticks(int listener, edge_acquisition *acquisition,
                                uint64_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        struct pollfd ready = {.fd = listener, .events = POLLIN};
        const int result = poll(&ready, 1U, 10);
        if (result < 0 && errno == EINTR)
            continue;
        if (result > 0 && (ready.revents & POLLIN) != 0) {
            const int fd = accept(listener, NULL, NULL);
            assert(fd >= 0);
            s7_set_timeout(fd);
            return fd;
        }
    }
    return -1;
}

static void wait_s7_peer_close(edge_acquisition *acquisition, int fd,
                               uint64_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        const int result = poll(&ready, 1U, 10);
        if (result < 0 && errno == EINTR)
            continue;
        if (result > 0 && (ready.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            uint8_t byte;
            const ssize_t received = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
            if (received == 0)
                return;
            assert(received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        }
    }
    assert(!"S7 client did not close its timed-out TCP session");
}

static bool s7_status_is(edge_acquisition *acquisition, const char *state) {
    iot_edge_v1_DeviceStatusReport report =
        iot_edge_v1_DeviceStatusReport_init_zero;
    edge_acquisition_status(acquisition, &report);
    return report.devices_count == 1U &&
           strcmp(report.devices[0].state, state) == 0;
}

static int accept_s7_retry_session(int listener, edge_acquisition *acquisition,
                                   uint64_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        if (s7_status_is(acquisition, "reconnecting"))
            return -1;
        struct pollfd descriptors[2] = {
            {.fd = listener, .events = POLLIN},
            {.fd = edge_acquisition_event_fd(acquisition), .events = POLLIN},
        };
        const int result = poll(descriptors, 2U, 10);
        if (result < 0 && errno == EINTR)
            continue;
        assert(result >= 0);
        edge_acquisition_tick(acquisition, monotonic_ms());
        if (s7_status_is(acquisition, "reconnecting"))
            return -1;
        if (result > 0 && (descriptors[0].revents & POLLIN) != 0) {
            const int fd = accept(listener, NULL, NULL);
            assert(fd >= 0);
            s7_set_timeout(fd);
            return fd;
        }
    }
    return -1;
}

static bool wait_s7_status(edge_acquisition *acquisition, const char *state,
                           uint64_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        if (s7_status_is(acquisition, state))
            return true;
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 10);
    }
    return false;
}

static bool wait_s7_activity_after(edge_acquisition *acquisition,
                                  int64_t previous_activity_ms,
                                  uint64_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        iot_edge_v1_DeviceStatusReport report =
            iot_edge_v1_DeviceStatusReport_init_zero;
        edge_acquisition_status(acquisition, &report);
        if (report.devices_count == 1U &&
            strcmp(report.devices[0].state, "connected") == 0 &&
            report.devices[0].last_activity_at_ms > previous_activity_ms)
            return true;
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 10);
    }
    return false;
}

static void verify_s7_timeout_retry_over_loopback(void) {
    struct sockaddr_in address;
    const int listener = open_s7_listener(&address);
    iot_edge_v1_ConfigItem values[3];
    edge_runtime_config config = make_s7_config(values, ntohs(address.sin_port), 1U,
                                                false, 300U);
    edge_acquisition *acquisition = edge_acquisition_create(count_s7_telemetry,
                                                              command, NULL);
    assert(acquisition != NULL);
    s7_test_records = 0U;
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));

    int fd = accept_s7_with_ticks(listener, acquisition, 3000U);
    assert(fd >= 0);
    s7_server_handshake(fd);
    uint8_t request[1024];
    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_VALID);
    assert(wait_s7_status(acquisition, "connected", 3000U));
    iot_edge_v1_DeviceStatusReport status =
        iot_edge_v1_DeviceStatusReport_init_zero;
    edge_acquisition_status(acquisition, &status);
    const int64_t first_activity_ms = status.devices[0].last_activity_at_ms;

    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    wait_s7_peer_close(acquisition, fd, 3000U);
    close(fd);

    fd = accept_s7_retry_session(listener, acquisition, 3000U);
    assert(fd >= 0);
    s7_server_handshake(fd);
    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_VALID);
    assert(wait_s7_activity_after(acquisition, first_activity_ms, 3000U));
    assert(s7_test_records == 0U);
    edge_acquisition_stop(acquisition);
    close(fd);
    close(listener);
    edge_acquisition_destroy(acquisition);
}

static void verify_s7_invalid_response_retry_over_loopback(void) {
    struct sockaddr_in address;
    const int listener = open_s7_listener(&address);
    iot_edge_v1_ConfigItem values[3];
    edge_runtime_config config = make_s7_config(values, ntohs(address.sin_port), 1U,
                                                false, 1U);
    edge_acquisition *acquisition = edge_acquisition_create(count_s7_telemetry,
                                                              command, NULL);
    assert(acquisition != NULL);
    s7_test_records = 0U;
    s7_invalid_responses = 0U;
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));

    int fd = accept_s7_with_ticks(listener, acquisition, 3000U);
    assert(fd >= 0);
    s7_server_handshake(fd);
    uint8_t request[1024];
    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_VALID);
    assert(wait_s7_status(acquisition, "connected", 3000U));
    iot_edge_v1_DeviceStatusReport status =
        iot_edge_v1_DeviceStatusReport_init_zero;
    edge_acquisition_status(acquisition, &status);
    int64_t last_activity_ms = status.devices[0].last_activity_at_ms;
    s7_test_records = 0U;

    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_VALID);
    assert(wait_s7_activity_after(acquisition, last_activity_ms, 3000U));
    s7_test_records = 0U;

    /* The 1-second ordinary report is due on this scan; fail the read and its retry. */
    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_WRONG_TRANSPORT);
    wait_s7_peer_close(acquisition, fd, 3000U);
    close(fd);

    fd = accept_s7_retry_session(listener, acquisition, 3000U);
    assert(fd >= 0);
    s7_server_handshake(fd);
    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_SHORT);
    wait_s7_peer_close(acquisition, fd, 3000U);
    assert(wait_s7_status(acquisition, "reconnecting", 3000U));
    assert(s7_test_records == 0U);
    assert(s7_invalid_responses == 2U);
    close(fd);

    fd = accept_s7_with_ticks(listener, acquisition, 3000U);
    assert(fd >= 0);
    s7_server_handshake(fd);
    receive_s7_request(fd, request);
    assert(request[17] == 4U);
    s7_send_read_response(fd, request, 0U, TEST_S7_RESPONSE_VALID);
    const uint64_t report_deadline = monotonic_ms() + 3000U;
    while (s7_test_records == 0U && monotonic_ms() < report_deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 10);
    }
    assert(s7_test_records == 1U);
    edge_acquisition_stop(acquisition);
    close(fd);
    close(listener);
    edge_acquisition_destroy(acquisition);
}

static void verify_s7_full_and_invalid_scans(void) {
    for (unsigned scenario = TEST_S7_RESPONSE_VALID;
         scenario <= TEST_S7_RESPONSE_UNDECODABLE; ++scenario) {
        struct sockaddr_in address;
        const int listener = open_s7_listener(&address);
        iot_edge_v1_ConfigItem values[TEST_S7_POINT_COUNT + 2U];
        const bool undecodable = scenario == TEST_S7_RESPONSE_UNDECODABLE;
        edge_runtime_config config = make_s7_config(
            values, ntohs(address.sin_port), TEST_S7_POINT_COUNT, undecodable, 1U);
        s7_test_records = 0U;
        s7_invalid_responses = 0U;
        edge_acquisition *acquisition = edge_acquisition_create(
            scenario == TEST_S7_RESPONSE_VALID ? verify_s7_full_telemetry
                                                : count_s7_telemetry,
            command, NULL);
        assert(acquisition != NULL);
        char error[256] = {0};
        assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                      error, sizeof(error)));
        assert(edge_acquisition_start(acquisition, error, sizeof(error)));

        int fd = -1;
        size_t reads_on_connection = 0U;
        bool failed_scan_closed = false;
        const uint64_t deadline = monotonic_ms() + 5000U;
        while (monotonic_ms() < deadline && s7_test_records == 0U &&
               !failed_scan_closed) {
            edge_acquisition_tick(acquisition, monotonic_ms());
            struct pollfd descriptors[2] = {
                {.fd = listener, .events = POLLIN},
                {.fd = fd, .events = POLLIN | POLLHUP},
            };
            const nfds_t count = fd >= 0 ? 2U : 1U;
            const int ready_count = poll(descriptors, count, 5);
            if (ready_count < 0 && errno == EINTR)
                continue;
            assert(ready_count >= 0);
            if (fd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                uint8_t byte;
                const ssize_t peeked = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
                if (peeked == 0) {
                    if (scenario != TEST_S7_RESPONSE_VALID &&
                        reads_on_connection == TEST_S7_POINT_COUNT)
                        failed_scan_closed = true;
                    close(fd);
                    fd = -1;
                    reads_on_connection = 0U;
                } else if (peeked > 0) {
                    uint8_t request[1024];
                    receive_s7_request(fd, request);
                    assert(request[17] == 4U);
                    const size_t point_index = reads_on_connection % TEST_S7_POINT_COUNT;
                    ++reads_on_connection;
                    test_s7_response_case response_case = TEST_S7_RESPONSE_VALID;
                    if (point_index + 1U == TEST_S7_POINT_COUNT) {
                        response_case = (test_s7_response_case)scenario;
                        if (scenario == TEST_S7_RESPONSE_UNDECODABLE)
                            response_case = TEST_S7_RESPONSE_VALID;
                    }
                    s7_send_read_response(fd, request, point_index, response_case);
                } else {
                    assert(errno == EAGAIN || errno == EWOULDBLOCK);
                }
            }
            if (fd < 0 && (descriptors[0].revents & POLLIN) != 0) {
                fd = accept(listener, NULL, NULL);
                assert(fd >= 0);
                s7_set_timeout(fd);
                s7_server_handshake(fd);
                reads_on_connection = 0U;
            }
        }
        if (scenario == TEST_S7_RESPONSE_VALID) {
            assert(s7_test_records == 1U);
        } else {
            assert(s7_test_records == 0U);
            assert(failed_scan_closed);
            assert(s7_invalid_responses ==
                   (scenario == TEST_S7_RESPONSE_UNDECODABLE ? 0U : 1U));
        }
        edge_acquisition_destroy(acquisition);
        if (fd >= 0)
            close(fd);
        close(listener);
    }
}

static unsigned industrial_records, industrial_commands;
static bool industrial_telemetry(void *context, const uint8_t platform[16], const iot_edge_v1_TelemetryRecord *record) {
    (void)context; (void)platform;
    assert(record->values_count == (raw_fixture ? 0U : 1U) && record->raw_payloads_count == 1);
    if (raw_fixture) {
        assert(record->raw_requests_count == 1 && record->raw_requests[0]->size > 0);
        ++industrial_records; return true;
    }
    const iot_edge_v1_ScalarValue *value = &record->values[0].value;
    if (record->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645) {
        assert(value->which_value == iot_edge_v1_ScalarValue_decimal_value_tag);
        assert(!strcmp(value->value.decimal_value, "42.00") || !strcmp(value->value.decimal_value, "13.25"));
    } else {
        assert(value->which_value == iot_edge_v1_ScalarValue_unsigned_value_tag);
        assert(value->value.unsigned_value == 42 || value->value.unsigned_value == 13);
    }
    ++industrial_records; return true;
}
static bool industrial_command(void *context, const uint8_t platform[16], const iot_edge_v1_CommandResult *result) {
    (void)context; (void)platform;
    assert(result->state == iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED);
    ++industrial_commands; return true;
}
static void receive_bytes(int fd, uint8_t *data, size_t size) {
    while (size) { ssize_t n = recv(fd, data, size, 0); assert(n > 0); data += n; size -= (size_t)n; }
}
static void verify_industrial_acquisition(iot_edge_v1_Protocol protocol, bool variant) {
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address); assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    assert(listen(listener, 1) == 0);
    iot_edge_v1_ConfigItem items[3]; edge_runtime_config config = make_config(items);
    items[0].item.endpoint.protocol = protocol; items[0].item.endpoint.port = ntohs(address.sin_port);
    iot_edge_v1_DeviceConfig *device = &items[1].item.device;
    device->protocol = protocol; device->has_industrial = true; strcpy(device->device_code, "000000123456");
    device->industrial.mc_four_e = variant; device->industrial.mc_station = 255;
    device->industrial.mc_module_io = 1023; device->industrial.mc_monitoring_timer = 16;
    device->industrial.dlt645_version = variant ? 1997 : 2007; device->industrial.dlt645_wakeup_bytes = 4;
    device->industrial.dlt645_write_password.size = device->industrial.dlt645_operator_code.size = 4;
    items[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    items[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_INDUSTRIAL_POINT;
    items[2].which_item = iot_edge_v1_ConfigItem_industrial_point_tag;
    iot_edge_v1_IndustrialPointConfig *point = &items[2].item.industrial_point;
    set_id(&point->device_id, device->device_id.bytes); strcpy(point->element_id, "value");
    strcpy(point->area, "D"); strcpy(point->data_type, protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? "BCD" : "UINT16");
    strcpy(point->byte_order, protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? "LITTLE_ENDIAN" : "BIG_ENDIAN");
    strcpy(point->identifier, variant ? "9010" : "00000000"); point->length = 4; point->digits = 2;
    point->scale = 1; point->decimals = -1; point->writable = true;
    industrial_records = industrial_commands = 0;
    edge_acquisition *acquisition = edge_acquisition_create(industrial_telemetry, industrial_command, NULL);
    char error[256] = {0}; assert(acquisition);
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(), error, sizeof(error)));
    const uint8_t platform[16] = {0};
    assert(edge_acquisition_set_raw_telemetry(acquisition, platform, raw_fixture));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    struct pollfd ready = {.fd = listener, .events = POLLIN}; assert(poll(&ready, 1, 3000) == 1);
    int fd = accept(listener, NULL, NULL); assert(fd >= 0);
    struct timeval timeout = {.tv_sec = 3}; assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint8_t stored[4] = {0, 0x42, 0, 0};
    if (protocol != iot_edge_v1_Protocol_PROTOCOL_DLT645) { stored[0] = protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? 42 : 0; stored[1] = protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? 0 : 42; }
    const uint8_t platform_id[16] = {0};
    unsigned writes = 0; bool queued = false;
    uint64_t deadline = monotonic_ms() + 7000;
    while (!industrial_commands && monotonic_ms() < deadline) {
        struct pollfd event = {.fd = fd, .events = POLLIN};
        if (poll(&event, 1, 30) == 1) {
            uint8_t q[256], r[256] = {0}; receive_bytes(fd, q, 1);
            while (q[0] == 0xfe) receive_bytes(fd, q, 1);
            size_t h = protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? variant ? 13 : 9 : protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? 8 : 10;
            receive_bytes(fd, q + 1, h - 1);
            size_t n = protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? h + q[h-2] + (size_t)q[h-1]*256 :
                protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? 8U + q[7] : 12U + q[9];
            assert(n <= sizeof(q)); receive_bytes(fd, q + h, n - h);
            size_t response_size;
            if (protocol == iot_edge_v1_Protocol_PROTOCOL_MC) {
                bool write = q[h+3] == 0x14;
                if (write) { memcpy(stored, q+h+12, 2); ++writes; }
                memcpy(r,q,h);r[0]=variant?0xd4:0xd0;r[h-2]=write?2:4;r[h-1]=0;
                response_size=h+2+(write?0:2);if (!write)memcpy(r+h+2,stored,2);
            } else if (protocol == iot_edge_v1_Protocol_PROTOCOL_FINS) {
                memcpy(r,q,16);
                if (q[11] == 0) {response_size=24;r[7]=16;r[11]=1;r[19]=10;r[23]=20;}
                else {
                    assert(q[20]==20 && q[23]==10);bool write=q[27]==2;
                    if(write){memcpy(stored,q+34,2);++writes;}
                    memcpy(r+16,q+16,12);r[16]=0xc0;memcpy(r+19,q+22,3);memcpy(r+22,q+19,3);
                    response_size=write?30:32;r[7]=(uint8_t)(response_size-8);if(!write)memcpy(r+30,stored,2);
                }
            } else {
                size_t id=variant?2:4;bool write=q[8]==(variant?4:0x14);
                if(write){for(size_t i=0;i<4;++i)stored[i]=(uint8_t)(q[10+id+(variant?4:8)+i]-0x33);++writes;}
                memcpy(r,q,8);r[8]=(uint8_t)(q[8]|0x80);r[9]=(uint8_t)(write?0:id+4);
                if(!write){memcpy(r+10,q+10,id);for(size_t i=0;i<4;++i)r[10+id+i]=(uint8_t)(stored[i]+0x33);}
                response_size=12+r[9];for(size_t i=0;i<response_size-2;++i)r[response_size-2]=(uint8_t)(r[response_size-2]+r[i]);r[response_size-1]=0x16;
            }
            assert(send(fd,r,3,0)==3);assert(send(fd,r+3,response_size-3,0)==(ssize_t)response_size-3);
        }
        edge_acquisition_tick(acquisition,monotonic_ms());
        if (industrial_records && !queued) {
            iot_edge_v1_CommandRequest request=iot_edge_v1_CommandRequest_init_zero;uint8_t command_id[16]={4};
            set_id(&request.command_id,command_id);set_id(&request.device_id,device->device_id.bytes);
            request.values_count=1;strcpy(request.values[0].element_id,"value");request.values[0].has_expected=true;
            request.values[0].expected.kind=iot_edge_v1_ValueKind_VALUE_STRING;
            request.values[0].expected.which_value=iot_edge_v1_ScalarValue_string_value_tag;
            request.has_start_before_ms = true; request.start_before_ms = 1;
            strcpy(request.values[0].expected.value.string_value,protocol==iot_edge_v1_Protocol_PROTOCOL_DLT645?"1000000.00":"65536");
            assert(!edge_acquisition_command_for_platform_until(acquisition, platform_id, &request, monotonic_ms()+60000U, UINT64_MAX, error, sizeof(error)));
            strcpy(request.values[0].expected.value.string_value,"-18446744073709551615");
            assert(!edge_acquisition_command_for_platform_until(acquisition, platform_id, &request, monotonic_ms()+60000U, UINT64_MAX, error, sizeof(error)));
            strcpy(request.values[0].expected.value.string_value,protocol==iot_edge_v1_Protocol_PROTOCOL_DLT645?"13.25":"13");
            assert(edge_acquisition_command_for_platform_until(acquisition, platform_id, &request, monotonic_ms()+60000U, UINT64_MAX, error, sizeof(error)));queued=true;
        }
    }
    assert(industrial_records && industrial_commands==1 && writes==1);
    close(fd);edge_acquisition_destroy(acquisition);close(listener);
}

static unsigned deadline_ipc_results;
static iot_edge_v1_CommandState deadline_ipc_state;
static bool deadline_ipc_ack_missing;
static bool deadline_ipc_command(void *context, const uint8_t platform[16],
                                const iot_edge_v1_CommandResult *result) {
    (void)context;
    (void)platform;
    ++deadline_ipc_results;
    deadline_ipc_state = result->state;
    deadline_ipc_ack_missing = result->write_ack_missing;
    return true;
}

static void verify_stale_deadline_over_ipc(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_size = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    assert(listen(listener, 1) == 0);

    iot_edge_v1_ConfigItem items[3];
    edge_runtime_config config = make_config(items);
    items[0].item.endpoint.port = ntohs(address.sin_port);
    items[2].item.modbus_register.writable = true;
    deadline_ipc_results = 0U;
    char error[256] = {0};
    edge_acquisition *acquisition = edge_acquisition_create(telemetry,
                                                              deadline_ipc_command, NULL);
    assert(acquisition != NULL);
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    struct pollfd ready = {.fd = listener, .events = POLLIN};
    assert(poll(&ready, 1, 3000) == 1);
    int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    struct timeval timeout = {.tv_sec = 3};
    assert(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint8_t request_frame[12];
    receive_modbus_request(peer, request_frame);
    assert(request_frame[7] == 3U);

    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {0x71U}, device_id[16] = {2U};
    set_id(&request.command_id, command_id);
    set_id(&request.device_id, device_id);
    request.values_count = 1U;
    copy_text(request.values[0].element_id, sizeof(request.values[0].element_id),
              "holding-1");
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value =
        iot_edge_v1_ScalarValue_string_value_tag;
    copy_text(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), "43");
    request.timeout_ms = 5000U;
    char missing_error[128] = {0};
    assert(!edge_acquisition_command_for_platform_until(
        acquisition, (const uint8_t[16]){0}, &request,
        monotonic_ms() + 100U, UINT64_MAX, missing_error, sizeof(missing_error)));
    request.has_start_before_ms = true;
    request.start_before_ms = 1700000005000LL;
    assert(edge_acquisition_command_for_platform_until(
        acquisition, (const uint8_t[16]){0}, &request,
        monotonic_ms() + 100U, UINT64_MAX, error, sizeof(error)));
    usleep(200000);

    const uint64_t result_deadline = monotonic_ms() + 3000U;
    while (deadline_ipc_results == 0U && monotonic_ms() < result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(deadline_ipc_results == 1U &&
           deadline_ipc_state == iot_edge_v1_CommandState_COMMAND_STATE_REJECTED &&
           !deadline_ipc_ack_missing);
    ready = (struct pollfd){.fd = peer, .events = POLLIN};
    if (poll(&ready, 1, 200) == 1) {
        receive_modbus_request(peer, request_frame);
        assert(request_frame[7] == 3U);
    }
    close(peer);
    edge_acquisition_destroy(acquisition);
    close(listener);
}

static void verify_pty_partial_write_is_not_replayed(void) {
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char path[97];
    copy_text(path, sizeof(path), ptsname(master));
    iot_edge_v1_ConfigItem items[3];
    edge_runtime_config config = make_config(items);
    make_serial_config(items, 9600U);
    copy_text(items[0].item.endpoint.serial.channel,
              sizeof(items[0].item.endpoint.serial.channel), path);
    items[2].item.modbus_register.writable = true;
    deadline_ipc_results = 0U;
    char error[256] = {0};
    edge_acquisition *acquisition = edge_acquisition_create(telemetry,
                                                              deadline_ipc_command, NULL);
    assert(acquisition != NULL);
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));

    struct pollfd ready = {.fd = master, .events = POLLIN};
    assert(poll(&ready, 1, 3000) == 1);
    uint8_t frame[4096];
    const ssize_t initial_size = read(master, frame, sizeof(frame));
    assert(initial_size > 0 && frame[1] == 3U);
    int fill_fd = open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    assert(fill_fd >= 0);
    const uint8_t filler[4096] = {0x55U};
    size_t queued = 0U;
    bool full = false;
    while (queued < 1024U * 1024U) {
        const ssize_t count = write(fill_fd, filler, sizeof(filler));
        if (count > 0) {
            queued += (size_t)count;
            continue;
        }
        assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        full = true;
        break;
    }
    assert(full && queued > sizeof(filler));
    const uint8_t byte = 0x55U;
    while (queued < 1024U * 1024U) {
        const ssize_t count = write(fill_fd, &byte, sizeof(byte));
        if (count == 1) {
            ++queued;
            continue;
        }
        assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }

    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {0x72U}, device_id[16] = {2U};
    set_id(&request.command_id, command_id);
    set_id(&request.device_id, device_id);
    request.values_count = 1U;
    copy_text(request.values[0].element_id, sizeof(request.values[0].element_id),
              "holding-1");
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value =
        iot_edge_v1_ScalarValue_string_value_tag;
    copy_text(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), "44");
    request.timeout_ms = 5000U;
    request.has_start_before_ms = true;
    request.start_before_ms = 1700000006000LL;
    const uint64_t start_before_monotonic_ms = monotonic_ms() + 2500U;
    assert(edge_acquisition_command_for_platform_until(
        acquisition, (const uint8_t[16]){0}, &request,
        start_before_monotonic_ms, UINT64_MAX, error, sizeof(error)));

    const uint64_t result_deadline = monotonic_ms() + 5000U;
    while (deadline_ipc_results == 0U && monotonic_ms() < result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(deadline_ipc_results == 1U &&
           monotonic_ms() >= start_before_monotonic_ms);
    size_t transmitted = 0U;
    for (;;) {
        const ssize_t count = read(master, frame, sizeof(frame));
        if (count > 0) {
            transmitted += (size_t)count;
            continue;
        }
        assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    assert(transmitted >= queued);
    const size_t command_bytes = transmitted - queued;
    if (deadline_ipc_state == iot_edge_v1_CommandState_COMMAND_STATE_REJECTED) {
        assert(!deadline_ipc_ack_missing && command_bytes == 0U);
    } else {
        assert(deadline_ipc_state == iot_edge_v1_CommandState_COMMAND_STATE_TIMED_OUT &&
               deadline_ipc_ack_missing && command_bytes > 0U && command_bytes <= 8U);
    }
    close(fill_fd);
    edge_acquisition_destroy(acquisition);
    close(master);
}

static unsigned priority_telemetry_records, priority_command_results;
static bool priority_telemetry(void *context, const uint8_t platform[16],
                               const iot_edge_v1_TelemetryRecord *record) {
    (void)context; (void)platform;
    assert(record->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS);
    assert(record->values_count == 1U);
    assert(record->values[0].has_value);
    assert(record->values[0].value.which_value == iot_edge_v1_ScalarValue_double_value_tag);
    ++priority_telemetry_records;
    return true;
}
static bool priority_command(void *context, const uint8_t platform[16],
                             const iot_edge_v1_CommandResult *result) {
    (void)context; (void)platform;
    assert(result->state >= iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED);
    ++priority_command_results;
    return true;
}
static void receive_modbus_request(int fd, uint8_t request[12]) {
    assert(recv(fd, request, 7, MSG_WAITALL) == 7);
    const size_t length = (size_t)request[4] * 256U + request[5];
    assert(length == 6U);
    assert(recv(fd, request + 7, 5, MSG_WAITALL) == 5);
}
static void send_modbus_read_response(int fd, const uint8_t request[12], uint8_t value) {
    uint8_t response[11];
    memcpy(response, request, 7);
    response[4] = 0; response[5] = 5;
    response[7] = 3; response[8] = 2;
    response[9] = 0; response[10] = value;
    assert(send(fd, response, sizeof(response), 0) == (ssize_t)sizeof(response));
}
static unsigned acknowledged_readback_results;
static bool acknowledged_readback_ack_missing;
static iot_edge_v1_CommandState acknowledged_readback_state;
static char acknowledged_readback_message[257];
static bool acknowledged_readback_command(void *context, const uint8_t platform[16],
                                           const iot_edge_v1_CommandResult *result) {
    (void)context;
    (void)platform;
    ++acknowledged_readback_results;
    acknowledged_readback_ack_missing = result->write_ack_missing;
    acknowledged_readback_state = result->state;
    copy_text(acknowledged_readback_message, sizeof(acknowledged_readback_message),
              result->message);
    return true;
}

static void verify_acknowledged_write_with_offline_readback(void) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_size = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    assert(listen(listener, 1) == 0);

    iot_edge_v1_ConfigItem items[3];
    edge_runtime_config config = make_config(items);
    items[0].item.endpoint.port = ntohs(address.sin_port);
    items[1].item.device.io_interval_ms = 300000U;
    items[1].item.device.report_interval_sec = 300U;
    items[2].item.modbus_register.writable = true;
    acknowledged_readback_results = 0U;
    acknowledged_readback_message[0] = '\0';
    char error[256] = {0};
    edge_acquisition *acquisition = edge_acquisition_create(
        telemetry, acknowledged_readback_command, NULL);
    assert(acquisition != NULL);
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    struct pollfd ready = {.fd = listener, .events = POLLIN};
    assert(poll(&ready, 1, 3000) == 1);
    const int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    struct timeval timeout = {.tv_sec = 3};
    assert(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint8_t request_frame[12];
    receive_modbus_request(peer, request_frame);
    assert(request_frame[7] == 3U);
    send_modbus_read_response(peer, request_frame, 42U);

    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {0x73U}, device_id[16] = {2U};
    set_id(&request.command_id, command_id);
    set_id(&request.device_id, device_id);
    request.values_count = 1U;
    copy_text(request.values[0].element_id, sizeof(request.values[0].element_id),
              "holding-1");
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value = iot_edge_v1_ScalarValue_string_value_tag;
    copy_text(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), "43");
    request.timeout_ms = 5000U;
    request.has_start_before_ms = true;
    request.start_before_ms = 1700000005000LL;
    assert(edge_acquisition_command_for_platform_until(
        acquisition, (const uint8_t[16]){0}, &request,
        monotonic_ms() + 60000U, UINT64_MAX, error, sizeof(error)));
    receive_modbus_request(peer, request_frame);
    assert(request_frame[7] == 6U);
    assert(send(peer, request_frame, sizeof(request_frame), 0) ==
           (ssize_t)sizeof(request_frame));
    receive_modbus_request(peer, request_frame);
    assert(request_frame[7] == 3U);
    close(peer);

    const uint64_t result_deadline = monotonic_ms() + 3000U;
    while (acknowledged_readback_results == 0U && monotonic_ms() < result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(acknowledged_readback_results == 1U);
    assert(!acknowledged_readback_ack_missing);
    assert(acknowledged_readback_state ==
           iot_edge_v1_CommandState_COMMAND_STATE_DEVICE_OFFLINE ||
           acknowledged_readback_state ==
               iot_edge_v1_CommandState_COMMAND_STATE_TIMED_OUT);
    assert(strstr(acknowledged_readback_message,
                  "write acknowledged; readback failed or unavailable") != NULL);
    edge_acquisition_destroy(acquisition);
    close(listener);
}

static void verify_write_priority_across_devices(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_size = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    assert(listen(listener, 1) == 0);
    int listener_b = socket(AF_INET, SOCK_STREAM, 0); assert(listener_b >= 0);
    struct sockaddr_in address_b = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener_b, (struct sockaddr *)&address_b, sizeof(address_b)) == 0);
    socklen_t address_b_size = sizeof(address_b);
    assert(getsockname(listener_b, (struct sockaddr *)&address_b, &address_b_size) == 0);
    assert(listen(listener_b, 1) == 0);

    iot_edge_v1_ConfigItem items[6];
    edge_runtime_config config = make_config(items);
    items[0].item.endpoint.port = ntohs(address.sin_port);
    items[1].item.device.io_interval_ms = 1000U;
    items[1].item.device.report_interval_sec = 1U;
    items[2].item.modbus_register.address = 10U;
    items[2].item.modbus_register.writable = true;
    items[3] = items[0];
    const uint8_t endpoint_b_id[16] = {4U}, device_b_id[16] = {3U};
    set_id(&items[3].item.endpoint.endpoint_id, endpoint_b_id);
    items[3].item.endpoint.port = ntohs(address_b.sin_port);
    items[4] = items[1];
    set_id(&items[4].item.device.device_id, device_b_id);
    set_id(&items[4].item.device.endpoint_id, endpoint_b_id);
    copy_text(items[4].item.device.device_code, sizeof(items[4].item.device.device_code), "OFFLINE02");
    items[4].item.device.modbus_slave_id = 2U;
    items[5] = items[2];
    set_id(&items[5].item.modbus_register.device_id, device_b_id);
    copy_text(items[5].item.modbus_register.element_id,
              sizeof(items[5].item.modbus_register.element_id), "holding-b");
    items[5].item.modbus_register.address = 20U;
    config.item_count = 6U;
    config.endpoint_count = 2U;
    config.device_count = 2U;

    priority_telemetry_records = priority_command_results = 0U;
    edge_acquisition *acquisition = edge_acquisition_create(priority_telemetry, priority_command, NULL);
    assert(acquisition != NULL);
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(), error, sizeof(error)));
    assert(edge_acquisition_device_count(acquisition) == 2U);
    iot_edge_v1_CommandRequest command_request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {7U}, device_a_id[16] = {2U};
    set_id(&command_request.command_id, command_id);
    set_id(&command_request.device_id, device_a_id);
    command_request.values_count = 1U;
    copy_text(command_request.values[0].element_id, sizeof(command_request.values[0].element_id), "holding-1");
    command_request.values[0].has_expected = true;
    command_request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    command_request.values[0].expected.which_value = iot_edge_v1_ScalarValue_string_value_tag;
    copy_text(command_request.values[0].expected.value.string_value,
              sizeof(command_request.values[0].expected.value.string_value), "42");
    command_request.timeout_ms = 10000U;
    command_request.has_start_before_ms = true;
    command_request.start_before_ms = 1;
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    assert(edge_acquisition_command_for_platform_until(
        acquisition, (const uint8_t[16]){0}, &command_request,
        monotonic_ms() + 60000U, UINT64_MAX, error, sizeof(error)));

    struct pollfd ready = {.fd = listener, .events = POLLIN};
    assert(poll(&ready, 1, 3000) == 1);
    int fd = accept(listener, NULL, NULL); assert(fd >= 0);
    ready.fd = listener_b;
    assert(poll(&ready, 1, 3000) == 1);
    int fd_b = accept(listener_b, NULL, NULL); assert(fd_b >= 0);
    struct timeval timeout = {.tv_sec = 5};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint8_t request[12];
    receive_modbus_request(fd, request);
    assert(request[6] == 1U && request[7] == 6U && request[8] == 0U && request[9] == 10U);
    assert(request[10] == 0U && request[11] == 42U);
    assert(send(fd, request, sizeof(request), 0) == (ssize_t)sizeof(request));

    /* The queued write must precede both devices' reads in this worker pass. */
    receive_modbus_request(fd, request);
    assert(request[6] == 1U && request[7] == 3U && request[9] == 10U);
    send_modbus_read_response(fd, request, 10U);
    receive_modbus_request(fd_b, request);
    assert(request[6] == 2U && request[7] == 3U && request[9] == 20U);
    send_modbus_read_response(fd_b, request, 20U);

    /* A later pass still polls A then B, and never replays the completed write. */
    bool saw_next_a = false, saw_next_b = false;
    const uint64_t deadline = monotonic_ms() + 5000U;
    while ((!saw_next_a || !saw_next_b) && monotonic_ms() < deadline) {
        if (!saw_next_a) {
            receive_modbus_request(fd, request);
            assert(request[6] == 1U && request[7] == 3U && request[9] == 10U);
            saw_next_a = true;
            send_modbus_read_response(fd, request, 11U);
        } else {
            receive_modbus_request(fd_b, request);
            assert(request[6] == 2U && request[7] == 3U && request[9] == 20U);
            saw_next_b = true;
            send_modbus_read_response(fd_b, request, 21U);
        }
    }
    assert(saw_next_a && saw_next_b);
    const uint64_t result_deadline = monotonic_ms() + 3000U;
    while (priority_command_results == 0U && monotonic_ms() < result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition), .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(priority_command_results == 1U);
    edge_acquisition_tick(acquisition, monotonic_ms());
    assert(priority_command_results == 1U);
    close(fd); close(fd_b); close(listener); close(listener_b); edge_acquisition_destroy(acquisition);
}

static iot_edge_v1_SerialDebugEvent serial_events[512];
static size_t serial_event_count;
static void serial_event(void *context, const uint8_t platform[16], const iot_edge_v1_SerialDebugEvent *event) {
    (void)context; (void)platform;
    assert(serial_event_count < 512);
    serial_events[serial_event_count++] = *event;
}
static const iot_edge_v1_SerialDebugEvent *await_serial_event(edge_acquisition *acquisition,
    uint8_t session, uint64_t request, const char *kind, size_t start) {
    const uint64_t deadline = monotonic_ms() + 5000;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        for (size_t i = start; i < serial_event_count; ++i)
            if (serial_events[i].session_id.bytes[0] == session &&
                serial_events[i].request_sequence == request && !strcmp(serial_events[i].kind, kind))
                return &serial_events[i];
        usleep(10000);
    }
    fprintf(stderr, "missing serial event: session=%u request=%llu kind=%s\n", session,
            (unsigned long long)request, kind);
    abort();
}
static iot_edge_v1_SerialDebugRequest serial_request(uint8_t id, uint64_t sequence,
    const char *action, const char *path) {
    iot_edge_v1_SerialDebugRequest request = iot_edge_v1_SerialDebugRequest_init_zero;
    const uint8_t session[16] = {id};
    set_id(&request.session_id, session);
    request.request_sequence = sequence;
    copy_text(request.action, sizeof(request.action), action);
    request.has_settings = true;
    copy_text(request.settings.channel, sizeof(request.settings.channel), path);
    request.settings.baud_rate = 9600; request.settings.data_bits = 8; request.settings.stop_bits = 1;
    strcpy(request.settings.parity, "none");
    return request;
}
static pid_t acquisition_child_pid(void) {
    char path[128];
    const int length = snprintf(path, sizeof(path), "/proc/self/task/%ld/children",
                                (long)getpid());
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *children = fopen(path, "r");
    assert(children != NULL);
    long pid = -1;
    (void)fscanf(children, "%ld", &pid);
    fclose(children);
    return pid > 0 ? (pid_t)pid : -1;
}

static pid_t wait_for_restarted_worker(edge_acquisition *acquisition,
                                       pid_t previous_pid, uint64_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        const pid_t current_pid = acquisition_child_pid();
        if (current_pid > 0 && current_pid != previous_pid)
            return current_pid;
        usleep(10000U);
    }
    return -1;
}

static void verify_restarted_worker_does_not_replay_command(int master,
                                                             edge_acquisition *acquisition) {
    uint8_t bytes[256];
    size_t size = 0U;
    const uint64_t deadline = monotonic_ms() + 1400U;
    while (monotonic_ms() < deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        struct pollfd ready = {.fd = master, .events = POLLIN};
        const int result = poll(&ready, 1U, 20);
        if (result < 0 && errno == EINTR)
            continue;
        assert(result >= 0);
        if (result == 0 || (ready.revents & POLLIN) == 0)
            continue;
        assert(size < sizeof(bytes));
        const ssize_t count = read(master, bytes + size, sizeof(bytes) - size);
        if (count > 0) {
            size += (size_t)count;
        } else {
            assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO));
        }
    }
    assert(size >= 8U && size % 8U == 0U);
    for (size_t offset = 0U; offset < size; offset += 8U)
        assert(bytes[offset] == 1U && bytes[offset + 1U] == 3U);
}

static void verify_worker_kill_and_old_command_not_replayed(void) {
    serial_event_count = 0U;
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char path[97];
    copy_text(path, sizeof(path), ptsname(master));
    iot_edge_v1_ConfigItem items[3];
    edge_runtime_config config = make_config(items);
    make_serial_config(items, 9600U);
    copy_text(items[0].item.endpoint.serial.channel,
              sizeof(items[0].item.endpoint.serial.channel), path);
    items[2].item.modbus_register.writable = true;
    const uint8_t platform[16] = {0U};
    char error[256] = {0};
    edge_acquisition *acquisition = edge_acquisition_create(telemetry, command, NULL);
    assert(acquisition != NULL);
    edge_acquisition_enable_serial_debug(acquisition, path, false, serial_event);
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));

    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {0x76U}, device_id[16] = {2U};
    set_id(&request.command_id, command_id);
    set_id(&request.device_id, device_id);
    request.values_count = 1U;
    copy_text(request.values[0].element_id, sizeof(request.values[0].element_id),
              "holding-1");
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value =
        iot_edge_v1_ScalarValue_string_value_tag;
    copy_text(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), "46");
    request.timeout_ms = 10000U;
    request.has_start_before_ms = true;
    request.start_before_ms = 1700000008000LL;
    const uint64_t accepted_at = monotonic_ms();
    assert(edge_acquisition_command_for_platform_until(
        acquisition, platform, &request, accepted_at + 30000U,
        accepted_at + 30000U, error, sizeof(error)));

    iot_edge_v1_SerialDebugRequest serial = serial_request(0x76U, 1U, "open", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &serial));
    (void)await_serial_event(acquisition, 0x76U, 1U, "state", 0U);
    serial = serial_request(0x76U, 2U, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &serial));
    assert(await_serial_event(acquisition, 0x76U, 2U, "state", 0U)->manual);
    usleep(50000U);
    uint8_t discarded[256];
    while (read(master, discarded, sizeof(discarded)) > 0) {
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    pid_t child = acquisition_child_pid();
    assert(child > 0);
    const uint64_t invalidation_started = monotonic_ms();
    edge_acquisition_invalidate_command_clock(acquisition, platform);
    assert(monotonic_ms() - invalidation_started < 250U);

    for (unsigned restart = 0U; restart < 2U; ++restart) {
        assert(kill(child, SIGKILL) == 0);
        const pid_t replacement = wait_for_restarted_worker(acquisition, child, 8000U);
        assert(replacement > 0);
        verify_restarted_worker_does_not_replay_command(master, acquisition);
        child = replacement;
    }
    edge_acquisition_destroy(acquisition);
    close(master);
}

static void verify_serial_debug(void) {
    serial_event_count = 0;
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char path[97];
    copy_text(path, sizeof(path), ptsname(master));
    const uint8_t platform[16] = {0}, other_platform[16] = {9};
    iot_edge_v1_ConfigItem values[3];
    edge_runtime_config config = make_config(values);
    make_serial_config(values, 9600);
    copy_text(values[0].item.endpoint.serial.channel, sizeof(values[0].item.endpoint.serial.channel), path);
    edge_acquisition *acquisition = edge_acquisition_create(telemetry, command, NULL);
    edge_acquisition_enable_serial_debug(acquisition, path, false, serial_event);
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(), error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    iot_edge_v1_SerialDebugRequest request = serial_request(1, 1, "open", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    assert(!await_serial_event(acquisition, 1, 1, "state", 0)->manual);
    const iot_edge_v1_SerialDebugEvent *automatic = await_serial_event(acquisition, 1, 0, "data", 0);
    assert(!strcmp(automatic->direction, "TX") && automatic->data.size > 0);
    request = serial_request(1, 2, "write", path);
    request.data.size = 1; request.data.bytes[0] = 0xff;
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 1, 2, "error", 0);
    request = serial_request(1, 3, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    assert(await_serial_event(acquisition, 1, 3, "state", 0)->manual);
    uint8_t bytes[128];
    while (read(master, bytes, sizeof(bytes)) > 0) {}
    const uint8_t raw[] = {0x00, 0xff, 0x0a, 0x80};
    request = serial_request(1, 4, "write", path);
    request.data.size = sizeof(raw); memcpy(request.data.bytes, raw, sizeof(raw));
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 1, 4, "sent", 0);
    assert(read(master, bytes, sizeof(bytes)) == (ssize_t)sizeof(raw) && !memcmp(bytes, raw, sizeof(raw)));
    // Duplicate delivery must never send the manual bytes twice.
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    usleep(200000);
    edge_acquisition_tick(acquisition, monotonic_ms());
    assert(read(master, bytes, sizeof(bytes)) < 0 && errno == EAGAIN);
    size_t start = serial_event_count;
    assert(write(master, raw, sizeof(raw)) == (ssize_t)sizeof(raw));
    const iot_edge_v1_SerialDebugEvent *received = await_serial_event(acquisition, 1, 0, "data", start);
    assert(!strcmp(received->direction, "RX") && received->data.size == sizeof(raw) &&
           !memcmp(received->data.bytes, raw, sizeof(raw)));
    request = serial_request(2, 1, "open", path);
    assert(edge_acquisition_serial_request(acquisition, other_platform, &request));
    (void)await_serial_event(acquisition, 2, 1, "state", 0);
    request = serial_request(2, 2, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, other_platform, &request));
    (void)await_serial_event(acquisition, 2, 2, "error", 0);
    request = serial_request(1, 5, "close", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 1, 5, "closed", 0);
    request = serial_request(2, 3, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, other_platform, &request));
    assert(await_serial_event(acquisition, 2, 3, "state", 0)->manual);
    request = serial_request(3, 1, "open", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 3, 1, "state", 0);
    request = serial_request(1, UINT64_MAX, "close", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    request = serial_request(3, 2, "keepalive", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 3, 2, "state", 0);
    start = serial_event_count;
    request = serial_request(2, 4, "monitor", path);
    assert(edge_acquisition_serial_request(acquisition, other_platform, &request));
    assert(!await_serial_event(acquisition, 2, 4, "state", start)->manual);
    automatic = await_serial_event(acquisition, 3, 0, "data", start);
    assert(!strcmp(automatic->direction, "TX"));
    // A worker restart releases manual ownership and invalidates the old identity.
    request = serial_request(2, 5, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, other_platform, &request));
    (void)await_serial_event(acquisition, 2, 5, "state", 0);
    edge_acquisition_stop(acquisition);
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    request = serial_request(2, 6, "keepalive", path);
    assert(edge_acquisition_serial_request(acquisition, other_platform, &request));
    (void)await_serial_event(acquisition, 2, 6, "closed", 0);
    // Losing both browser and platform cleanup still releases the physical port after its lease.
    request = serial_request(4, 1, "open", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 4, 1, "state", 0);
    request = serial_request(4, 2, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 4, 2, "state", 0);
    const uint64_t lease_deadline = monotonic_ms() + 65000;
    bool expired = false;
    start = serial_event_count;
    while (!expired && monotonic_ms() < lease_deadline) {
        edge_acquisition_tick(acquisition, monotonic_ms());
        for (size_t i = start; i < serial_event_count; ++i)
            if (serial_events[i].session_id.bytes[0] == 4 && !strcmp(serial_events[i].kind, "closed"))
                expired = !strcmp(serial_events[i].message, "serial debug lease expired");
        usleep(20000);
    }
    assert(expired);
    request = serial_request(5, 1, "open", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &request));
    (void)await_serial_event(acquisition, 5, 1, "state", start);
    automatic = await_serial_event(acquisition, 5, 0, "data", start);
    assert(!strcmp(automatic->direction, "TX"));
    edge_acquisition_destroy(acquisition);
    close(master);
}

static void verify_command_expiry_while_serial_is_paused(void) {
    serial_event_count = 0U;
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char path[97];
    copy_text(path, sizeof(path), ptsname(master));
    iot_edge_v1_ConfigItem items[3];
    edge_runtime_config config = make_config(items);
    make_serial_config(items, 9600U);
    copy_text(items[0].item.endpoint.serial.channel,
              sizeof(items[0].item.endpoint.serial.channel), path);
    items[2].item.modbus_register.writable = true;
    deadline_ipc_results = 0U;
    const uint8_t platform[16] = {0U};
    char error[256] = {0};
    edge_acquisition *acquisition = edge_acquisition_create(telemetry,
                                                              deadline_ipc_command, NULL);
    assert(acquisition != NULL);
    edge_acquisition_enable_serial_debug(acquisition, path, false, serial_event);
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    iot_edge_v1_SerialDebugRequest serial = serial_request(0x73U, 1U, "open", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &serial));
    assert(!await_serial_event(acquisition, 0x73U, 1U, "state", 0U)->manual);
    serial = serial_request(0x73U, 2U, "manual", path);
    assert(edge_acquisition_serial_request(acquisition, platform, &serial));
    assert(await_serial_event(acquisition, 0x73U, 2U, "state", 0U)->manual);
    uint8_t received[256];
    while (read(master, received, sizeof(received)) > 0) {
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    const uint8_t command_id[16] = {0x73U}, device_id[16] = {2U};
    set_id(&request.command_id, command_id);
    set_id(&request.device_id, device_id);
    request.values_count = 1U;
    copy_text(request.values[0].element_id, sizeof(request.values[0].element_id),
              "holding-1");
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value =
        iot_edge_v1_ScalarValue_string_value_tag;
    copy_text(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), "45");
    request.timeout_ms = 5000U;
    request.has_start_before_ms = true;
    request.start_before_ms = 1700000007000LL;
    const uint64_t start_before_monotonic_ms = monotonic_ms() + 650U;
    assert(edge_acquisition_command_for_platform_until(
        acquisition, platform, &request, start_before_monotonic_ms,
        UINT64_MAX, error, sizeof(error)));

    const uint64_t result_deadline = monotonic_ms() + 3000U;
    while (deadline_ipc_results == 0U && monotonic_ms() < result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(deadline_ipc_results == 1U &&
           deadline_ipc_state == iot_edge_v1_CommandState_COMMAND_STATE_REJECTED &&
           !deadline_ipc_ack_missing);
    assert(read(master, received, sizeof(received)) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));

    deadline_ipc_results = 0U;
    const uint8_t age_limited_id[16] = {0x74U};
    set_id(&request.command_id, age_limited_id);
    const uint64_t age_test_now = monotonic_ms();
    assert(edge_acquisition_command_for_platform_until(
        acquisition, platform, &request, age_test_now + 10000U,
        age_test_now + 350U, error, sizeof(error)));
    const uint64_t age_result_deadline = monotonic_ms() + 3000U;
    while (deadline_ipc_results == 0U && monotonic_ms() < age_result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(deadline_ipc_results == 1U &&
           deadline_ipc_state == iot_edge_v1_CommandState_COMMAND_STATE_REJECTED &&
           !deadline_ipc_ack_missing);
    assert(read(master, received, sizeof(received)) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));

    deadline_ipc_results = 0U;
    const uint8_t revoked_id[16] = {0x75U};
    set_id(&request.command_id, revoked_id);
    const uint64_t revoke_test_now = monotonic_ms();
    assert(edge_acquisition_command_for_platform_until(
        acquisition, platform, &request, revoke_test_now + 10000U,
        revoke_test_now + 10000U, error, sizeof(error)));
    edge_acquisition_invalidate_command_clock(acquisition, platform);
    const uint64_t revoked_result_deadline = monotonic_ms() + 3000U;
    while (deadline_ipc_results == 0U && monotonic_ms() < revoked_result_deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition),
                               .events = POLLIN};
        (void)poll(&event, 1U, 20);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(deadline_ipc_results == 1U &&
           deadline_ipc_state == iot_edge_v1_CommandState_COMMAND_STATE_REJECTED &&
           !deadline_ipc_ack_missing);
    assert(read(master, received, sizeof(received)) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    edge_acquisition_destroy(acquisition);
    close(master);
}

int main(void) {
    const char *local_log = "/tmp/edgenode-test-acquisition/logs/acquisition.log";
    (void)unlink(local_log);
    for (unsigned index = 1U; index < 4U; ++index) {
        char older[128];
        snprintf(older, sizeof(older), "%s.%u", local_log, index);
        (void)unlink(older);
    }
    verify_s7_timeout_retry_over_loopback();
    verify_s7_invalid_response_retry_over_loopback();
    verify_s7_full_and_invalid_scans();
    FILE *local = fopen(local_log, "r");
    assert(local != NULL);
    bool saw_config = false, saw_read_failure = false, saw_report = false;
    char diagnostic[240];
    while (fgets(diagnostic, sizeof(diagnostic), local) != NULL) {
        saw_config |= strstr(diagnostic, "\tconfig-applied\t") != NULL;
        saw_read_failure |= strstr(diagnostic, "\tio-read\t3\t") != NULL;
        saw_report |= strstr(diagnostic, "\treport-queued\t0\t") != NULL;
    }
    assert(fclose(local) == 0);
    assert(saw_config && saw_read_failure && saw_report);
    verify_serial_debug();
    verify_worker_kill_and_old_command_not_replayed();
    iot_edge_v1_ConfigItem values[3];
    edge_runtime_config config = make_config(values);
    edge_acquisition *acquisition = edge_acquisition_create(telemetry, command, NULL);
    assert(acquisition != NULL);
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    wait_for_status(acquisition);
    edge_acquisition_stop(acquisition);
    assert(edge_acquisition_event_fd(acquisition) == -1);
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    wait_for_status(acquisition);
    edge_acquisition_destroy(acquisition);
    verify_shared_resources();
    verify_stale_deadline_over_ipc();
    verify_acknowledged_write_with_offline_readback();
    verify_pty_partial_write_is_not_replayed();
    verify_command_expiry_while_serial_is_paused();
    verify_write_priority_across_devices();
    verify_sl651_commit();
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_MC, false);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_MC, true);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_FINS, false);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_DLT645, false);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_DLT645, true);
    for (unsigned flags = 0; flags < 4; ++flags) {
        verify_complete_acquisition_record(false, (flags & 1) != 0, (flags & 2) != 0);
        verify_complete_acquisition_record(true, (flags & 1) != 0, (flags & 2) != 0);
    }
    raw_fixture = true;
    verify_sl651_commit();
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_MC, false);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_MC, true);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_FINS, false);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_DLT645, false);
    verify_industrial_acquisition(iot_edge_v1_Protocol_PROTOCOL_DLT645, true);
    for (unsigned flags = 0; flags < 4; ++flags) {
        verify_complete_acquisition_record(false, (flags & 1) != 0, (flags & 2) != 0);
        verify_complete_acquisition_record(true, (flags & 1) != 0, (flags & 2) != 0);
    }
    bool configured[7] = {false};
    for (unsigned file = 0U; file < 4U; ++file) {
        char path[128];
        if (file == 0U)
            snprintf(path, sizeof(path), "%s", local_log);
        else
            snprintf(path, sizeof(path), "%s.%u", local_log, file);
        FILE *input = fopen(path, "r");
        if (input == NULL) continue;
        while (fgets(diagnostic, sizeof(diagnostic), input) != NULL)
            for (unsigned protocol = 1U; protocol <= 6U; ++protocol) {
                char event[32];
                snprintf(event, sizeof(event), "\t%u\tconfig-applied\t", protocol);
                configured[protocol] |= strstr(diagnostic, event) != NULL;
            }
        assert(fclose(input) == 0);
    }
    for (unsigned protocol = 1U; protocol <= 6U; ++protocol)
        assert(configured[protocol]);
    return 0;
}
