#include "edge_modbus_gateway.h"
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3)
        return 2;
    const char *mode = argc == 3 ? argv[2] : "normal";
    iot_edge_v1_ConfigItem items[7] = {0};
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
    edge_runtime_config config = {.items = items, .item_count = !strcmp(mode, "deleted") ? 2 : 7};
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
