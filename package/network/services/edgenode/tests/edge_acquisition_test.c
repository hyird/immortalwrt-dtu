#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
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

static bool sl651_allow_report, sl651_allow_command;
static unsigned sl651_reports, sl651_results, sl651_images;
static unsigned sl651_raw_frames;
static bool sl651_store_report(void *context, const uint8_t platform[16], const iot_edge_v1_TelemetryRecord *record) {
    (void)context; (void)platform;
    assert(record->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651);
    assert(record->report_id.size == 16 && record->part_count >= 2 && record->part_index < record->part_count);
    if (record->raw_payloads_count) {
        assert(record->values_count == 0);
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
    assert(result->state == iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED);
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
static void sl651_send_report(int fd, uint8_t function, uint8_t serial) {
    uint8_t bytes[] = {0x7E,0x7E,1,0,0,0,0,1,0,0,0,0,10,2,0,0,0x24,1,2,3,4,5,0x12,0x34,3,0,0};
    bytes[10] = function; bytes[15] = serial;
    uint16_t crc = edge_sl651_crc(bytes, sizeof(bytes) - 2);
    bytes[sizeof(bytes)-2] = (uint8_t)(crc >> 8U); bytes[sizeof(bytes)-1] = (uint8_t)crc;
    assert(write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
}
static void verify_sl651_commit(void) {
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
    close(reservation); assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    int fd = -1;
    for (unsigned attempt = 0; attempt < 60; ++attempt) {
        fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) break;
        close(fd); fd = -1; sl651_pump(acquisition, 100);
    }
    assert(fd >= 0); sl651_send_report(fd, 0x32, 1);
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
    assert(edge_acquisition_command(acquisition, &request, error, sizeof(error)));
    n = sl651_read(acquisition, fd, bytes, sizeof(bytes)); assert(n == 27 && bytes[n-3] == 5 && bytes[14] == 0 && bytes[15] == 0 && bytes[22] == 0x12);
    sl651_send_report(fd, 0x4C, 2); until = monotonic_ms() + 5000;
    while (!sl651_results && monotonic_ms() < until) sl651_pump(acquisition, 20);
    assert(sl651_results && poll(&ready, 1, 100) == 0);
    sl651_allow_command = true; n = sl651_read(acquisition, fd, bytes, sizeof(bytes)); assert(n >= 25 && bytes[n-3] == 4 && bytes[15] == 2);
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
    assert(record->values_count == (s7_responses ? 2U : 3U));
    assert(strcmp(record->values[0].element_id, "holding-1") == 0);
    assert(record->values[0].value.value.double_value == 100.0);
    assert(strcmp(record->values[1].element_id, "holding-2") == 0);
    assert(record->values[1].value.value.double_value == 101.0);
    if (!s7_responses) {
        assert(strcmp(record->values[2].element_id, "holding-copy") == 0);
        assert(record->values[2].value.value.double_value == 100.0);
    }
    assert(record->raw_payloads_count == 2 && record->raw_packet_ids_count == 2);
    for (unsigned index = 0; index < 2; ++index) {
        assert(record->raw_payloads[index]->size == expected_response_size);
        assert(memcmp(record->raw_payloads[index]->bytes, expected_responses[index], expected_response_size) == 0);
        assert(record->raw_packet_ids[index]->size == 16);
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
        values[1].item.device.report_interval_sec = 300U;
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
    for (unsigned index = 0; index < 2; ++index) {
        if (s7) {
            uint8_t request[1024];
            receive_s7_request(fd, request);
            assert(request[17] == 4);
            uint8_t response[] = {3,0,0,27,2,0xf0,0x80,0x32,3,0,0,0,0,0,2,0,6,0,0,
                                  4,1,0xff,4,0,16,0,0};
            response[11] = request[11]; response[12] = request[12];
            response[26] = (uint8_t)(100 + index);
            memcpy(expected_responses[index], response, sizeof(response));
            assert(send(fd, response, sizeof(response), 0) == (ssize_t)sizeof(response));
            continue;
        }
        uint8_t request[12];
        assert(recv(fd, request, sizeof(request), MSG_WAITALL) == (ssize_t)sizeof(request));
        assert(request[7] == 3);
        assert(request[9] == (index == 0 ? 0 : 100));
        uint8_t *response = expected_responses[index];
        memcpy(response, request, 7);
        response[5] = 5;
        response[7] = 3;
        response[8] = 2;
        response[9] = 0;
        response[10] = (uint8_t)(100 + index);
        assert(send(fd, response, 11, 0) == 11);
    }
    const uint64_t deadline = monotonic_ms() + 3000;
    while (response_records < 1 && monotonic_ms() < deadline) {
        struct pollfd event = {.fd = edge_acquisition_event_fd(acquisition), .events = POLLIN};
        (void)poll(&event, 1, 100);
        edge_acquisition_tick(acquisition, monotonic_ms());
    }
    assert(response_records == 1);
    if (link_debug || device_debug) { assert(debug_rx >= expected_response_size * 2 && debug_tx > 0); assert(debug_success >= 2); assert(debug_values == (s7 ? 2U : 3U)); }
    else { assert(debug_rx == 0 && debug_tx == 0 && debug_values == 0); }
    if (s7) {
        /* The S7 TCP Client reuses its session for a silent 1-second scan. */
        const unsigned background_debug_rx = debug_rx;
        const unsigned background_debug_tx = debug_tx;
        const unsigned background_debug_success = debug_success;
        const unsigned background_debug_values = debug_values;
        struct pollfd scan = {.fd = fd, .events = POLLIN | POLLHUP};
        assert(poll(&scan, 1U, 3000) == 1 && (scan.revents & POLLIN) != 0);
        for (unsigned index = 0; index < 2; ++index) {
            uint8_t request[1024];
            receive_s7_request(fd, request);
            assert(request[17] == 4);
            uint8_t response[] = {3,0,0,27,2,0xf0,0x80,0x32,3,0,0,0,0,0,2,0,6,0,0,
                                  4,1,0xff,4,0,16,0,0};
            response[11] = request[11]; response[12] = request[12];
            response[26] = (uint8_t)(110 + index);
            assert(send(fd, response, sizeof(response), 0) == (ssize_t)sizeof(response));
        }
        const uint64_t scan_deadline = monotonic_ms() + 1000U;
        while (monotonic_ms() < scan_deadline) {
            edge_acquisition_tick(acquisition, monotonic_ms());
            usleep(10000);
        }
        assert(response_records == 1U && debug_rx == background_debug_rx &&
               debug_tx == background_debug_tx && debug_success == background_debug_success &&
               debug_values == background_debug_values);
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

static unsigned industrial_records, industrial_commands;
static bool industrial_telemetry(void *context, const uint8_t platform[16], const iot_edge_v1_TelemetryRecord *record) {
    (void)context; (void)platform;
    assert(record->values_count == 1 && record->raw_payloads_count == 1);
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
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    struct pollfd ready = {.fd = listener, .events = POLLIN}; assert(poll(&ready, 1, 3000) == 1);
    int fd = accept(listener, NULL, NULL); assert(fd >= 0);
    struct timeval timeout = {.tv_sec = 3}; assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint8_t stored[4] = {0, 0x42, 0, 0};
    if (protocol != iot_edge_v1_Protocol_PROTOCOL_DLT645) { stored[0] = protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? 42 : 0; stored[1] = protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? 0 : 42; }
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
            strcpy(request.values[0].expected.value.string_value,protocol==iot_edge_v1_Protocol_PROTOCOL_DLT645?"1000000.00":"65536");
            assert(!edge_acquisition_command(acquisition,&request,error,sizeof(error)));
            strcpy(request.values[0].expected.value.string_value,"-18446744073709551615");
            assert(!edge_acquisition_command(acquisition,&request,error,sizeof(error)));
            strcpy(request.values[0].expected.value.string_value,protocol==iot_edge_v1_Protocol_PROTOCOL_DLT645?"13.25":"13");
            assert(edge_acquisition_command(acquisition,&request,error,sizeof(error)));queued=true;
        }
    }
    assert(industrial_records && industrial_commands==1 && writes==1);
    close(fd);edge_acquisition_destroy(acquisition);close(listener);
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

int main(void) {
    verify_serial_debug();
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
    return 0;
}
