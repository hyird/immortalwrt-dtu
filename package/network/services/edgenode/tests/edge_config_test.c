#include "edge_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_encode.h>

#include "edge_runtime_config.h"

static void require_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge config test failed: %s\n", message);
        exit(1);
    }
}

static void copy_text(char *output, size_t capacity, const char *input) {
    snprintf(output, capacity, "%s", input);
}

static size_t encode_item(const iot_edge_v1_ConfigItem *item, uint8_t *output,
                          size_t capacity) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    require_true(pb_encode(&stream, iot_edge_v1_ConfigItem_fields, item),
                 "cannot encode config item");
    return stream.bytes_written;
}

static void set_id(void *field, size_t capacity, const uint8_t id[16]) {
    const pb_size_t size = 16U;
    memcpy(field, &size, sizeof(size));
    require_true(capacity >= 16U, "invalid bytes capacity");
    memcpy((uint8_t *)field + sizeof(size), id, 16U);
}

static void test_valid_snapshot(void) {
    const uint8_t id[16] = {1U, 2U, 3U};
    iot_edge_v1_ConfigItem values[3] = {
        iot_edge_v1_ConfigItem_init_zero,
        iot_edge_v1_ConfigItem_init_zero,
        iot_edge_v1_ConfigItem_init_zero};

    values[0].revision = 7U;
    values[0].index = 0U;
    values[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    values[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    set_id(&values[0].item.endpoint.endpoint_id,
           sizeof(values[0].item.endpoint.endpoint_id.bytes), id);
    copy_text(values[0].item.endpoint.name, sizeof(values[0].item.endpoint.name),
              "PLC endpoint");
    values[0].item.endpoint.transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    values[0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    copy_text(values[0].item.endpoint.ip, sizeof(values[0].item.endpoint.ip), "192.168.1.8");
    values[0].item.endpoint.port = 502U;
    values[0].item.endpoint.enabled = true;

    values[1].revision = 7U;
    values[1].index = 1U;
    values[1].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DEVICE;
    values[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    set_id(&values[1].item.device.device_id,
           sizeof(values[1].item.device.device_id.bytes), id);
    set_id(&values[1].item.device.endpoint_id,
           sizeof(values[1].item.device.endpoint_id.bytes), id);
    copy_text(values[1].item.device.device_code, sizeof(values[1].item.device.device_code),
              "PLC01");
    values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    values[1].item.device.io_interval_ms = 1000U;
    values[1].item.device.report_interval_sec = 5U;
    values[1].item.device.command_fast_read_duration_sec = 60U;
    values[1].item.device.command_fast_read_interval_sec = 5U;
    values[1].item.device.modbus_slave_id = 1U;
    copy_text(values[1].item.device.modbus_mode, sizeof(values[1].item.device.modbus_mode),
              "TCP");
    values[1].item.device.enabled = true;

    values[2].revision = 7U;
    values[2].index = 2U;
    values[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_MODBUS_REGISTER;
    values[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    set_id(&values[2].item.modbus_register.device_id,
           sizeof(values[2].item.modbus_register.device_id.bytes), id);
    copy_text(values[2].item.modbus_register.element_id,
              sizeof(values[2].item.modbus_register.element_id),
              "00000000-0000-7000-8000-000000000001");
    copy_text(values[2].item.modbus_register.name,
              sizeof(values[2].item.modbus_register.name), "Pressure");
    copy_text(values[2].item.modbus_register.register_type,
              sizeof(values[2].item.modbus_register.register_type), "HOLDING_REGISTER");
    copy_text(values[2].item.modbus_register.data_type,
              sizeof(values[2].item.modbus_register.data_type), "FLOAT32");
    copy_text(values[2].item.modbus_register.byte_order,
              sizeof(values[2].item.modbus_register.byte_order),
              "LITTLE_ENDIAN_BYTE_SWAP");
    values[2].item.modbus_register.quantity = 2U;
    values[2].item.modbus_register.scale = 1.0;
    values[2].item.modbus_register.decimals = 2;
    values[2].item.modbus_register.writable = true;

    uint8_t payloads[3][EDGENODE_MAX_WS_MESSAGE];
    edge_memory_config_item items[3] = {0};
    for (size_t index = 0U; index < 3U; ++index) {
        items[index].payload = payloads[index];
        items[index].payload_size = encode_item(&values[index], payloads[index],
                                                sizeof(payloads[index]));
        items[index].present = true;
    }
    edge_memory_config_set snapshot = {
        .revision = 7U, .item_count = 3U, .received_count = 3U, .items = items};
    edge_runtime_config runtime = {0};
    char error[256] = {0};
    require_true(edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), error);
    require_true(runtime.endpoint_count == 1U && runtime.device_count == 1U,
                 "snapshot counts are wrong");
    require_true(strcmp(runtime.items[2].item.modbus_register.byte_order,
                        "LITTLE_ENDIAN_BYTE_SWAP") == 0,
                 "nanopb byte order was truncated");
    edge_runtime_config_free(&runtime);

    const uint32_t intervals[] = {0U, 1000U, 5000U, 30000U, 3600000U, 500U, 3600001U};
    for (size_t i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i) {
        values[1].item.device.io_interval_ms = intervals[i];
        items[1].payload_size = encode_item(&values[1], payloads[1], sizeof(payloads[1]));
        bool loaded = edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error));
        require_true(loaded == (i < 5U), "configured acquisition interval validation mismatch");
        if (loaded) edge_runtime_config_free(&runtime);
    }
    values[1].item.device.io_interval_ms = 0U;

    values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_SL651;
    values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_SL651;
    strcpy(values[1].item.device.device_code, "0000000001");
    values[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_SL651_ELEMENT;
    values[2].which_item = iot_edge_v1_ConfigItem_sl651_element_tag;
    memset(&values[2].item, 0, sizeof(values[2].item));
    iot_edge_v1_Sl651ElementConfig *point = &values[2].item.sl651_element;
    set_id(&point->device_id, sizeof(point->device_id.bytes), id);
    strcpy(point->function_code, "E1"); strcpy(point->element_id, "custom");
    strcpy(point->encoding, "BCD");
    point->length = 2; point->digits = 2;
    point->guide.size = 2; point->guide.bytes[0] = 0xAB; point->guide.bytes[1] = 0x12;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        if (scenario == 1) { point->fixed_position = true; point->byte_offset = 8; point->guide.size = 0; }
        if (scenario == 2) point->byte_offset = 65535;
        if (scenario == 3) { point->byte_offset = 0; point->writable = true; }
        if (scenario == 4) { point->byte_offset = 8; point->digits = 8; }
        if (scenario == 5) { point->digits = 2; values[1].item.device.sl651_response_mode = 5; }
        for (size_t index = 0; index < 3; ++index)
            items[index].payload_size = encode_item(&values[index], payloads[index], sizeof(payloads[index]));
        bool loaded = edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error));
        require_true(loaded == (scenario < 2), "SL651 positioning/mode validation mismatch");
        if (loaded) {
            require_true(runtime.items[1].item.device.sl651_response_mode == 0,
                         "legacy absent mode must retain zero/M1 default");
            require_true(runtime.items[2].item.sl651_element.fixed_position == (scenario == 1),
                         "positioning did not survive protobuf snapshot");
            edge_runtime_config_free(&runtime);
        }
    }
}

static void test_unknown_item_rejected(void) {
    iot_edge_v1_ConfigItem value = iot_edge_v1_ConfigItem_init_zero;
    value.revision = 9U;
    value.index = 0U;
    uint8_t payload[EDGENODE_MAX_WS_MESSAGE];
    edge_memory_config_item item = {.payload = payload,
                                    .payload_size = encode_item(&value, payload, sizeof(payload)),
                                    .present = true};
    edge_memory_config_set snapshot = {
        .revision = 9U, .item_count = 1U, .received_count = 1U, .items = &item};
    edge_runtime_config runtime = {0};
    char error[128] = {0};
    require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)),
                 "unknown config item was accepted");
}

static void test_external_io_with_protocol(bool standalone) {
    const uint8_t device_id[16] = {1U};
    const uint8_t model_id[16] = {2U};
    iot_edge_v1_ConfigItem values[6] = {0};
    for (size_t i = 0; i < 6U; ++i) { values[i].revision = 8U; values[i].index = (uint32_t)i; }
    values[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    values[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    iot_edge_v1_EndpointConfig *endpoint = &values[0].item.endpoint;
    set_id(&endpoint->endpoint_id, sizeof(endpoint->endpoint_id.bytes), device_id);
    endpoint->transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    endpoint->mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    endpoint->protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(endpoint->ip, "127.0.0.1"); endpoint->port = 502; endpoint->enabled = true;
    values[1].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DEVICE;
    values[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    iot_edge_v1_DeviceConfig *device = &values[1].item.device;
    set_id(&device->device_id, sizeof(device->device_id.bytes), device_id);
    set_id(&device->endpoint_id, sizeof(device->endpoint_id.bytes), device_id);
    strcpy(device->device_code, "device"); device->protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    device->report_interval_sec = 5U; device->enabled = true;
    for (size_t i = 2; i < 4U; ++i) {
        values[i].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_EXTERNAL_IO;
        values[i].which_item = iot_edge_v1_ConfigItem_external_io_tag;
        iot_edge_v1_ExternalIoPointConfig *io = &values[i].item.external_io;
        set_id(&io->device_id, sizeof(io->device_id.bytes), device_id);
        set_id(&io->model_id, sizeof(io->model_id.bytes), model_id);
        snprintf(io->point_id, sizeof(io->point_id), "00000000-0000-4000-8000-%012u", (unsigned)i);
        strcpy(io->name, "IO"); io->kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
        snprintf(io->channel, sizeof(io->channel), "do%u", (unsigned)(i - 2));
    }
    if (standalone) {
        endpoint->protocol = device->protocol = iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO;
        endpoint->transport = iot_edge_v1_Transport_TRANSPORT_EXTERNAL_IO;
        endpoint->mode = iot_edge_v1_LinkMode_LINK_MODE_UNSPECIFIED;
        endpoint->ip[0] = '\0'; endpoint->port = 0;
    }
    uint8_t payloads[6][EDGENODE_MAX_WS_MESSAGE];
    edge_memory_config_item items[6] = {0};
    for (size_t i = 0; i < 4U; ++i) {
        items[i].payload = payloads[i]; items[i].present = true;
        items[i].payload_size = encode_item(&values[i], payloads[i], sizeof(payloads[i]));
    }
    edge_memory_config_set snapshot = {.revision = 8U, .item_count = 4U, .received_count = 4U, .items = items};
    edge_runtime_config runtime = {0}; char error[128] = {0};
    require_true(edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), error);
    edge_runtime_config_free(&runtime);

    iot_edge_v1_ExternalIoPointConfig *analog = &values[2].item.external_io;
    analog->kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI;
    strcpy(analog->channel, "AI0");
    strcpy(analog->unit, "mA");
    items[2].payload_size = encode_item(&values[2], payloads[2], sizeof(payloads[2]));
    require_true(edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), "mA AI configuration rejected");
    edge_runtime_config_free(&runtime);
    strcpy(analog->unit, "uA");
    items[2].payload_size = encode_item(&values[2], payloads[2], sizeof(payloads[2]));
    require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), "old AI wire unit accepted");
    analog->kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
    strcpy(analog->channel, "do0");
    analog->unit[0] = '\0';
    items[2].payload_size = encode_item(&values[2], payloads[2], sizeof(payloads[2]));

    iot_edge_v1_DerivedPointInput inputs[2] = {iot_edge_v1_DerivedPointInput_init_zero,
                                              iot_edge_v1_DerivedPointInput_init_zero};
    strcpy(inputs[0].alias, "s");
    strcpy(inputs[0].point_id, values[2].item.external_io.point_id);
    strcpy(inputs[1].alias, "x");
    strcpy(inputs[1].point_id, "derived_first");
    for (size_t i = 4; i < 6U; ++i) {
        values[i].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DERIVED_POINT;
        values[i].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
        iot_edge_v1_DerivedPointConfig *rule = &values[i].item.derived_point;
        set_id(&rule->device_id, sizeof(rule->device_id.bytes), device_id);
        strcpy(rule->point_id, i == 4 ? "derived_first" : "derived_second");
        strcpy(rule->name, i == 4 ? "first" : "second");
        strcpy(rule->kind, "expression");
        strcpy(rule->expression, i == 4 ? "if(s, 1, 0)" : "x * 2");
        rule->inputs = &inputs[i - 4];
        rule->inputs_count = 1;
        rule->max_age_seconds = 30;
        items[i].payload = payloads[i];
        items[i].present = true;
        items[i].payload_size = encode_item(&values[i], payloads[i], sizeof(payloads[i]));
    }
    snapshot.item_count = snapshot.received_count = 6U;
    require_true(edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)),
                 "encoded IO-derived chain was rejected");
    edge_runtime_config_free(&runtime);
    iot_edge_v1_DerivedPointConfig *typed = &values[4].item.derived_point;
    strcpy(typed->data_type, "INT8");
    strcpy(typed->rounding, "truncate");
    items[4].payload_size = encode_item(&values[4], payloads[4], sizeof(payloads[4]));
    require_true(edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)),
                 "typed IO-derived chain rejected");
    require_true(!strcmp(runtime.items[4].item.derived_point.data_type, "INT8") &&
                 !strcmp(runtime.items[4].item.derived_point.rounding, "truncate"),
                 "typed fields lost through snapshot");
    iot_edge_v1_ConfigItem *active = runtime.items;
    const struct { const char *type, *rounding; bool boolean_result; } invalid[] = {
        {"DOUBLE", "", false}, {"INT8", "floor", false},
        {"FLOAT32", "round", false}, {"BOOL", "", false}, {"UINT8", "", true}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(typed->data_type, invalid[i].type);
        strcpy(typed->rounding, invalid[i].rounding);
        typed->boolean_result = invalid[i].boolean_result;
        items[4].payload_size = encode_item(&values[4], payloads[4], sizeof(payloads[4]));
        require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)),
                     "invalid typed output configuration accepted");
        require_true(runtime.items == active &&
                     !strcmp(runtime.items[4].item.derived_point.data_type, "INT8") &&
                     !strcmp(runtime.items[4].item.derived_point.rounding, "truncate"),
                     "rejected typed configuration damaged active state");
    }
    edge_runtime_config_free(&runtime);
    typed->data_type[0] = typed->rounding[0] = 0;
    typed->boolean_result = false;
    strcpy(inputs[0].point_id, "missing_io");
    items[4].payload_size = encode_item(&values[4], payloads[4], sizeof(payloads[4]));
    require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)),
                 "missing IO dependency was accepted");
    snapshot.item_count = snapshot.received_count = 4U;
    if (standalone) {
        endpoint->transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
        items[0].payload_size = encode_item(&values[0], payloads[0], sizeof(payloads[0]));
        require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), "IO with Ethernet accepted");
        endpoint->transport = iot_edge_v1_Transport_TRANSPORT_EXTERNAL_IO;
        strcpy(endpoint->interface_name, "br-lan");
        items[0].payload_size = encode_item(&values[0], payloads[0], sizeof(payloads[0]));
        require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), "IO with interface accepted");
        endpoint->interface_name[0] = '\0';
        items[0].payload_size = encode_item(&values[0], payloads[0], sizeof(payloads[0]));
    }
    strcpy(values[3].item.external_io.channel, "do0");
    items[3].payload_size = encode_item(&values[3], payloads[3], sizeof(payloads[3]));
    require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), "duplicate output channel accepted");
    strcpy(values[3].item.external_io.channel, "do7");
    items[3].payload_size = encode_item(&values[3], payloads[3], sizeof(payloads[3]));
    require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), "unknown output channel accepted");
}

int main(void) {
    test_valid_snapshot();
    test_unknown_item_rejected();
    test_external_io_with_protocol(false);
    test_external_io_with_protocol(true);
    puts("edge config tests passed");
    return 0;
}
