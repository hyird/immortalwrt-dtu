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
    require(strstr(json, "\"ioIntervalMs\":30000") != NULL,
            "status must show the configured acquisition interval instead of one second");
    require(strstr(json, "\"elementId\":\"opening\"") != NULL,
            "point details are missing");
    require(strstr(json, "production \\\"one\\\"") != NULL,
            "JSON string escaping changed");

    items[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO;
    items[0].item.endpoint.transport = iot_edge_v1_Transport_TRANSPORT_EXTERNAL_IO;
    items[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO;
    memset(&items[2], 0, sizeof(items[2]));
    items[2].which_item = iot_edge_v1_ConfigItem_external_io_tag;
    iot_edge_v1_ExternalIoPointConfig *io = &items[2].item.external_io;
    memcpy(&io->device_id, &items[1].item.device.device_id, sizeof(io->device_id));
    set_id(&io->model_id, sizeof(io->model_id.bytes), 65U);
    strcpy(io->point_id, "local-point");
    strcpy(io->name, "Local \"IO\"");
    const iot_edge_v1_ExternalIoKind kinds[] = {
        iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI,
        iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO,
        iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI,
    };
    const char *channels[] = { "di0", "do0", "ai0" };
    for (size_t index = 0; index < 3; ++index) {
        io->kind = kinds[index];
        strcpy(io->channel, channels[index]);
        strcpy(io->unit, index == 2 ? "mA" : "");
        io->hidden = index == 2;
        output = tmpfile();
        require(output != NULL, "cannot create external IO status file");
        require(edge_status_render(output, &snapshot), "external IO rendering failed");
        rewind(output);
        json[fread(json, 1U, sizeof(json) - 1U, output)] = '\0';
        fclose(output);
        require(strstr(json, "\"protocol\":\"External IO\"") != NULL, "external IO protocol missing");
        require(strstr(json, "\"transport\":\"external-io\"") != NULL, "external IO transport missing");
        require(strstr(json, "\"ip\":") == NULL && strstr(json, "\"port\":") == NULL,
                "external IO must not advertise network parameters");
        require(strstr(json, "\"kind\":\"external-io\"") != NULL &&
                strstr(json, "\"kind\":\"sl651\"") == NULL, "external IO misclassified");
        require(strstr(json, "\"elementId\":\"local-point\"") != NULL &&
                strstr(json, channels[index]) != NULL, "external IO identity missing");
        require(strstr(json, index == 1 ? "\"writable\":true" : "\"writable\":false") != NULL,
                "external IO permissions incorrect");
        require(strstr(json, index == 2 ? "\"dataType\":\"DOUBLE\"" : "\"dataType\":\"BOOL\"") != NULL,
                "external IO value type incorrect");
        if (index == 2)
            require(strstr(json, "\"unit\":\"mA\"") != NULL && strstr(json, "\"hidden\":true") != NULL,
                    "analog IO metadata missing");
    }

    iot_edge_v1_ConfigItem extra_items[8] = {0};
    extra_items[0] = items[0];
    extra_items[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_MC;
    extra_items[1] = items[1];
    extra_items[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_DLT645;

    extra_items[2].which_item = iot_edge_v1_ConfigItem_industrial_point_tag;
    memcpy(&extra_items[2].item.industrial_point.device_id,
           &extra_items[1].item.device.device_id,
           sizeof(extra_items[2].item.industrial_point.device_id));
    strcpy(extra_items[2].item.industrial_point.element_id, "energy-total");
    strcpy(extra_items[2].item.industrial_point.name, "Total Energy");
    strcpy(extra_items[2].item.industrial_point.area, "DATA");
    strcpy(extra_items[2].item.industrial_point.identifier, "00010000");
    extra_items[2].item.industrial_point.address = 100U;
    strcpy(extra_items[2].item.industrial_point.data_type, "UINT32");
    extra_items[2].item.industrial_point.scale = 0.01;
    extra_items[2].item.industrial_point.decimals = 2U;
    strcpy(extra_items[2].item.industrial_point.unit, "kWh");

    extra_items[3].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
    memcpy(&extra_items[3].item.derived_point.device_id,
           &extra_items[1].item.device.device_id,
           sizeof(extra_items[3].item.derived_point.device_id));
    strcpy(extra_items[3].item.derived_point.point_id, "energy-delta");
    strcpy(extra_items[3].item.derived_point.name, "Energy Delta");
    strcpy(extra_items[3].item.derived_point.kind, "DELTA");
    strcpy(extra_items[3].item.derived_point.expression, "a - b");

    extra_items[4].which_item = iot_edge_v1_ConfigItem_sl651_element_tag;
    memcpy(&extra_items[4].item.sl651_element.device_id,
           &extra_items[1].item.device.device_id,
           sizeof(extra_items[4].item.sl651_element.device_id));
    strcpy(extra_items[4].item.sl651_element.element_id, "water-level");
    strcpy(extra_items[4].item.sl651_element.name, "Water Level");
    strcpy(extra_items[4].item.sl651_element.function_code, "32");
    strcpy(extra_items[4].item.sl651_element.encoding, "BCD");
    extra_items[4].item.sl651_element.length = 4U;
    extra_items[4].item.sl651_element.digits = 2U;
    extra_items[4].item.sl651_element.writable = true;

    extra_items[5].which_item = iot_edge_v1_ConfigItem_dtu_tag;
    extra_items[6].which_item = iot_edge_v1_ConfigItem_sl651_function_tag;
    extra_items[7].which_item = iot_edge_v1_ConfigItem_sl651_dictionary_tag;

    runtime.items = extra_items;
    runtime.item_count = 8U;
    output = tmpfile();
    require(output != NULL, "cannot create extended status file");
    require(edge_status_render(output, &snapshot), "extended status rendering failed");
    rewind(output);
    json[fread(json, 1U, sizeof(json) - 1U, output)] = '\0';
    fclose(output);
    require(strstr(json, "\"protocol\":\"MC\"") != NULL, "MC protocol missing");
    require(strstr(json, "\"protocol\":\"DLT645\"") != NULL, "DLT645 protocol missing");
    require(strstr(json, "\"kind\":\"industrial\"") != NULL &&
            strstr(json, "\"identifier\":\"00010000\"") != NULL,
            "industrial point missing");
    require(strstr(json, "\"kind\":\"derived\"") != NULL &&
            strstr(json, "\"derivedKind\":\"DELTA\"") != NULL,
            "derived point missing");
    require(strstr(json, "\"kind\":\"sl651\"") != NULL &&
            strstr(json, "\"functionCode\":\"32\"") != NULL,
            "sl651 element point missing");
    require(strstr(json, "\"kind\":\"\",") == NULL,
            "non-point config items must not be serialized into points");

    puts("edge status tests passed");
    return EXIT_SUCCESS;
}
