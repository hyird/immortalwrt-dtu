#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge_status.h"

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void set_id(void *field, size_t capacity, uint8_t seed) {
    pb_size_t *size = field;
    uint8_t *bytes = (uint8_t *)field + sizeof(*size);
    *size = 16U;
    for (size_t index = 0U; index < 16U && index < capacity; ++index)
        bytes[index] = (uint8_t)(seed + index);
}

int main(void) {
    edge_platform_config platform = {0};
    for (size_t index = 0U; index < sizeof(platform.id); ++index)
        platform.id[index] = (uint8_t)index;
    snprintf(platform.name, sizeof(platform.name), "production \"one\"");
    platform.priority = 10U;
    platform.outbox_max_bytes = 262144U;

    iot_edge_v1_ConfigItem items[3] = {
        iot_edge_v1_ConfigItem_init_zero,
        iot_edge_v1_ConfigItem_init_zero,
        iot_edge_v1_ConfigItem_init_zero,
    };
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    set_id(&items[0].item.endpoint.endpoint_id,
           sizeof(items[0].item.endpoint.endpoint_id.bytes), 1U);
    snprintf(items[0].item.endpoint.name, sizeof(items[0].item.endpoint.name),
             "RS485");
    items[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    items[0].item.endpoint.transport = iot_edge_v1_Transport_TRANSPORT_SERIAL;
    items[0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_SERIAL;
    items[0].item.endpoint.enabled = true;
    snprintf(items[0].item.endpoint.serial.channel,
             sizeof(items[0].item.endpoint.serial.channel), "/dev/ttyS1");
    items[0].item.endpoint.serial.baud_rate = 9600U;
    items[0].item.endpoint.serial.data_bits = 8U;
    items[0].item.endpoint.serial.stop_bits = 1U;
    snprintf(items[0].item.endpoint.serial.parity,
             sizeof(items[0].item.endpoint.serial.parity), "none");
    items[0].item.endpoint.serial.rs485 = true;

    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    set_id(&items[1].item.device.device_id,
           sizeof(items[1].item.device.device_id.bytes), 33U);
    memcpy(&items[1].item.device.endpoint_id,
           &items[0].item.endpoint.endpoint_id,
           sizeof(items[1].item.device.endpoint_id));
    snprintf(items[1].item.device.device_code,
             sizeof(items[1].item.device.device_code), "gate-1");
    snprintf(items[1].item.device.name, sizeof(items[1].item.device.name), "Gate 1");
    items[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    items[1].item.device.enabled = true;
    items[1].item.device.report_interval_sec = 30U;
    items[1].item.device.modbus_slave_id = 1U;
    snprintf(items[1].item.device.modbus_mode,
             sizeof(items[1].item.device.modbus_mode), "RTU");

    items[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    memcpy(&items[2].item.modbus_register.device_id,
           &items[1].item.device.device_id,
           sizeof(items[2].item.modbus_register.device_id));
    snprintf(items[2].item.modbus_register.element_id,
             sizeof(items[2].item.modbus_register.element_id), "opening");
    snprintf(items[2].item.modbus_register.name,
             sizeof(items[2].item.modbus_register.name), "Opening");
    snprintf(items[2].item.modbus_register.register_type,
             sizeof(items[2].item.modbus_register.register_type),
             "HOLDING_REGISTER");
    snprintf(items[2].item.modbus_register.data_type,
             sizeof(items[2].item.modbus_register.data_type), "UINT16");
    snprintf(items[2].item.modbus_register.byte_order,
             sizeof(items[2].item.modbus_register.byte_order), "BIG_ENDIAN");
    items[2].item.modbus_register.address = 100U;
    items[2].item.modbus_register.quantity = 1U;
    items[2].item.modbus_register.scale = 0.1;

    edge_runtime_config runtime = {
        .revision = 42U,
        .items = items,
        .item_count = 3U,
        .endpoint_count = 1U,
        .device_count = 1U,
    };
    edge_memory_config_set staging = {
        .revision = 43U,
        .item_count = 5U,
        .received_count = 2U,
    };
    edge_memory_outbox outbox = {
        .maximum_bytes = 262144U,
        .bytes = 1024U,
        .count = 3U,
        .in_flight = 1U,
    };
    edge_status_platform status_platform = {
        .config = &platform,
        .runtime_config = &runtime,
        .staging_config = &staging,
        .outbox = &outbox,
        .phase = EDGE_RETRY_CONNECTED,
        .last_heartbeat_ms = 9000U,
        .last_inbound_ms = 9500U,
        .heartbeat_interval_sec = 30U,
        .websocket_open = true,
        .enrolled = true,
    };
    edge_status_snapshot snapshot = {
        .software_version = "0.3.30",
        .now_ms = 10000U,
        .platforms = &status_platform,
        .platform_count = 1U,
    };

    FILE *output = tmpfile();
    require(output != NULL, "cannot create status test file");
    require(edge_status_render(output, &snapshot), "status JSON rendering failed");
    require(fseek(output, 0L, SEEK_SET) == 0, "cannot rewind status JSON");
    char json[8192];
    const size_t size = fread(json, 1U, sizeof(json) - 1U, output);
    fclose(output);
    json[size] = '\0';

    require(strstr(json, "\"state\":\"connected\"") != NULL,
            "connection state is missing");
    require(strstr(json, "\"lastHeartbeatAgeSec\":1") != NULL,
            "heartbeat age is missing");
    require(strstr(json, "\"revision\":42") != NULL,
            "active revision is missing");
    require(strstr(json, "\"reportIntervalSec\":30") != NULL,
            "device report interval is missing");
    require(strstr(json, "\"elementId\":\"opening\"") != NULL,
            "point details are missing");
    require(strstr(json, "production \\\"one\\\"") != NULL,
            "JSON string escaping changed");

    puts("edge status tests passed");
    return EXIT_SUCCESS;
}
