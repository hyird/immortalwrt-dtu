#include "edge_modbus_gateway.h"
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    iot_edge_v1_ConfigItem items[3] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 1;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    items[1].item.device.device_id.bytes[0] = 2;
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 1;
    items[1].item.device.enabled = true;
    items[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    items[2].item.modbus_register.device_id.size = 16;
    items[2].item.modbus_register.device_id.bytes[0] = 2;
    strcpy(items[2].item.modbus_register.element_id, "counter");
    edge_runtime_config config = {.items = items, .item_count = 3};
    uint8_t platform[16] = {3};
    edge_acquisition_source source = {.platform_id = platform, .config = &config};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare("network.map", &source, 1);
    if (!gateway)
        return 3;
    edge_acquisition_sample sample = {.valid = true, .observed_at_ms = 1760000000123LL};
    sample.device_id[0] = 2;
    strcpy(sample.element_id, "counter");
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = UINT64_MAX;
    edge_modbus_gateway_update(gateway, platform, &sample, 0);
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
