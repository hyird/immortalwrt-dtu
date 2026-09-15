#undef NDEBUG
#include <assert.h>
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
static bool sl651_store_report(void *context, const uint8_t platform[16], const iot_edge_v1_TelemetryRecord *record) {
    (void)context; (void)platform;
    assert(record->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651 && record->values_count == 1);
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
    assert(sl651_images && n == 28 && bytes[n-3] == 4 && bytes[16] == 3);
    close(fd); edge_acquisition_destroy(acquisition);
}

int main(void) {
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
    return 0;
}
