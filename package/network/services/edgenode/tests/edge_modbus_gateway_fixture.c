#include "edge_modbus_gateway.h"
#include "edge_derived.h"
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3)
        return 2;
    const char *mode = argc == 3 ? argv[2] : "normal";
    iot_edge_v1_ConfigItem items[9] = {0};
    iot_edge_v1_DerivedPointInput inputs[2] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 1;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    items[1].item.device.device_id.bytes[0] = 2;
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 1;
    items[1].item.device.enabled = strcmp(mode, "disabled") != 0;
    const char *elements[] = {"counter", "flag", "short", "wide", "real"};
    const char *types[] = {"UINT64", "BOOL", "UINT16", "UINT32", "DOUBLE"};
    for (unsigned i = 0; i < 5; ++i) {
        items[i+2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &items[i+2].item.modbus_register;
        point->device_id.size = 16; point->device_id.bytes[0] = 2;
        strcpy(point->element_id, elements[i]); strcpy(point->data_type, types[i]);
        point->scale = 1;
    }
    for (unsigned i = 0; i < 2; ++i) {
        items[7 + i].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
        iot_edge_v1_DerivedPointConfig *rule = &items[7 + i].item.derived_point;
        rule->device_id.size = 16; rule->device_id.bytes[0] = 2;
        strcpy(rule->point_id, i ? "derived-flag" : "derived-number");
        strcpy(rule->name, i ? "派生状态" : "派生数值");
        strcpy(rule->kind, "expression");
        strcpy(rule->expression, i ? "x > 1" : "x * 2");
        rule->boolean_result = i != 0;
        rule->max_age_seconds = 60;
        rule->inputs_count = 1;
        rule->inputs = &inputs[i];
        strcpy(rule->inputs[0].alias, "x");
        strcpy(rule->inputs[0].point_id, "real");
    }
    edge_runtime_config config = {.items = items, .item_count = !strcmp(mode, "deleted") ? 2 : 9};
    uint8_t platform[16] = {3};
    edge_acquisition_source source = {.platform_id = platform, .config = &config};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare("network.map",
        !strcmp(mode, "empty") ? NULL : &source, !strcmp(mode, "empty") ? 0 : 1);
    if (!gateway)
        return 3;
    if (!edge_modbus_gateway_set_unit_id(gateway, !strcmp(mode, "unit247") ? 247U : 0U))
        return 8;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 7;
    const uint64_t received_at = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    for (unsigned i = 0; i < 5; ++i) {
        edge_acquisition_sample sample = {.valid = true, .observed_at_ms = 1760000000123LL};
        sample.device_id[0] = 2; strcpy(sample.element_id, elements[i]);
        if (i == 1) {
            sample.value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
            sample.value.value.bool_value = true;
        } else if (i == 4) {
            sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag;
            sample.value.value.double_value = 1.5;
        } else {
            sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
            sample.value.value.unsigned_value = i == 0 ? UINT64_MAX : i == 2 ? 0x1234 : 0x12345678;
        }
        edge_modbus_gateway_update(gateway, platform, &sample, received_at);
    }
    if (config.item_count > 2) {
        edge_derived *derived = edge_derived_create(&config, items[1].item.device.device_id.bytes);
        if (!derived) return 9;
        iot_edge_v1_TelemetryValue input = iot_edge_v1_TelemetryValue_init_zero;
        strcpy(input.element_id, "real");
        input.has_value = strcmp(mode, "derived-failure") != 0;
        input.value.which_value = iot_edge_v1_ScalarValue_double_value_tag;
        input.value.value.double_value = 1.5;
        (void)edge_derived_update(derived, &input, 1, 1760000000123LL);
        for (size_t i = 0; i < edge_derived_count(derived); ++i) {
            const iot_edge_v1_TelemetryValue *value = edge_derived_value(derived, i);
            edge_acquisition_sample sample = {.valid = value->has_value,
                .observed_at_ms = value->sample_time_ms, .value = value->value};
            sample.device_id[0] = 2;
            strcpy(sample.element_id, value->element_id);
            edge_modbus_gateway_update(gateway, platform, &sample, received_at);
        }
        edge_derived_free(derived);
    }
    if (!strcmp(mode, "sorted") || !strcmp(mode, "save-failure")) {
        iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
        order.expected_digest.size = 32; order.addresses.size = 4;
        order.addresses.bytes[1] = 40; order.addresses.bytes[3] = 20;
        if (!edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes)) return 5;
        const bool sorted = !strcmp(mode, "sorted");
        if (edge_modbus_gateway_reorder(gateway, sorted ? "network.map" : "missing/map", platform, &order) != sorted)
            return 6;
    }
    edge_modbus_gateway_server *server =
        edge_modbus_gateway_listen(EV_DEFAULT, gateway, "lo", (uint16_t)strtoul(argv[1], NULL, 10));
    if (!server) {
        edge_modbus_gateway_free(gateway);
        return 4;
    }
    puts("READY");
    fflush(stdout);
    ev_run(EV_DEFAULT, 0);
    edge_modbus_gateway_close(server);
    edge_modbus_gateway_free(gateway);
    return 0;
}
