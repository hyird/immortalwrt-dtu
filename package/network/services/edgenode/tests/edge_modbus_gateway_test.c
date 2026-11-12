#include "edge_modbus_gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "gateway check failed at %d: %s\n", __LINE__, #condition);             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint16_t word(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static size_t read_registers(edge_modbus_gateway *gateway, uint16_t start, uint16_t count,
                             uint8_t response[260], uint64_t now) {
    uint8_t request[12] = {0x12, 0x34, 0, 0, 0, 6, 1, 4, 0, 0, 0, 0};
    request[8] = (uint8_t)(start >> 8);
    request[9] = (uint8_t)start;
    request[10] = (uint8_t)(count >> 8);
    request[11] = (uint8_t)count;
    return edge_modbus_gateway_reply(gateway, request, sizeof(request), response, now);
}

static void test_drag_permutation(void) {
    const char *path = "gateway-order-test.map";
    remove(path);
    uint8_t platform[16] = {1}, foreign[16] = {2};
    iot_edge_v1_ConfigItem items[5] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    items[1].item.device.device_id.bytes[0] = 4;
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    for (size_t i = 2; i < 5; ++i) {
        items[i].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        items[i].item.modbus_register.device_id.size = 16;
        items[i].item.modbus_register.device_id.bytes[0] = 4;
        snprintf(items[i].item.modbus_register.element_id, 65, "point-%u", (unsigned)i);
    }
    edge_runtime_config config = {.items = items, .item_count = 5};
    edge_acquisition_source sources[] = {{.platform_id = platform, .config = &config},
                                        {.platform_id = foreign, .config = &config}};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway);
    edge_acquisition_sample sample = {0};
    sample.device_id[0] = 4;
    strcpy(sample.element_id, "point-4");
    sample.valid = true;
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = 123;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
    order.expected_digest.size = 32;
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    order.addresses.size = 6;
    order.addresses.bytes[1] = 20;
    order.addresses.bytes[5] = 10; /* Move last to first: [20, 0, 10]. */
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    const unsigned expected[] = {10, 20, 0};
    for (unsigned i = 0; i < 3; ++i) {
        char element[65]; snprintf(element, sizeof(element), "point-%u", i + 2);
        CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, element, &point, 1));
        CHECK(point.address == expected[i]);
        CHECK(edge_modbus_gateway_describe(gateway, foreign, sample.device_id, element, &point, 1));
        CHECK(point.address == (i + 3) * 10);
    }
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "point-4", &point, 1));
    CHECK(!strcmp(point.value, "123"));
    edge_modbus_gateway_free(gateway);
    gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway && edge_modbus_gateway_describe(gateway, platform, sample.device_id, "point-4", &point, 1));
    CHECK(point.address == 0);
    edge_modbus_gateway_free(gateway);
    remove(path);
}

static size_t read_native(edge_modbus_gateway *gateway, unsigned function, unsigned address,
                           unsigned count, uint8_t response[260]) {
    uint8_t request[12] = {0, 1, 0, 0, 0, 6, 1, 0};
    request[7] = (uint8_t)function;
    request[8] = (uint8_t)(address >> 8); request[9] = (uint8_t)address;
    request[10] = (uint8_t)(count >> 8); request[11] = (uint8_t)count;
    return edge_modbus_gateway_reply(gateway, request, sizeof(request), response, 1);
}

static void test_native_types(void) {
    const char *path = "gateway-native-test.map";
    remove(path);
    const char *types[] = {"BOOL", "BOOL", "INT16", "UINT16", "INT32", "UINT32", "FLOAT32", "INT64", "UINT64", "DOUBLE"};
    const unsigned words[] = {1,1,1,1,2,2,2,4,4,4};
    const unsigned codes[] = {2,2,6,7,8,9,10,3,4,5};
    const uint64_t bits[] = {1,1,0xff85,0xffff,0xfffe1dc0,UINT32_MAX,0x3fc00000,UINT64_C(0x8000000000000000),UINT64_MAX,UINT64_C(0x3ff8000000000000)};
    uint8_t platform[16] = {1};
    iot_edge_v1_ConfigItem items[12] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    items[1].item.device.device_id.bytes[0] = 4;
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    for (unsigned i = 0; i < 10; ++i) {
        items[i+2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &items[i+2].item.modbus_register;
        point->device_id.size = 16; point->device_id.bytes[0] = 4;
        snprintf(point->element_id, sizeof(point->element_id), "typed-%u", i);
        snprintf(point->data_type, sizeof(point->data_type), "%s", types[i]);
        point->scale = 1; point->decimals = -1;
    }
    edge_runtime_config config = {.items = items, .item_count = 12};
    edge_acquisition_source source = {.platform_id = platform, .config = &config};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, &source, 1);
    CHECK(gateway);
    uint8_t response[260];
    for (unsigned i = 0; i < 10; ++i) {
        edge_acquisition_sample sample = {0}; sample.device_id[0] = 4;
        snprintf(sample.element_id, sizeof(sample.element_id), "typed-%u", i);
        iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
        CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, sample.element_id, &point, 1));
        CHECK(point.value_type == codes[i] && point.value_words == words[i] && point.quality == 0);
        CHECK(point.value_address == (i < 2 ? i : i * 10 + 2));
        CHECK(!strcmp(point.register_type, i < 2 ? "DISCRETE_INPUT" : "HOLDING_REGISTER"));
        sample.valid = true;
        if (i < 2) { sample.value.which_value = iot_edge_v1_ScalarValue_bool_value_tag; sample.value.value.bool_value = true; }
        else if (i == 2 || i == 4 || i == 7) {
            sample.value.which_value = iot_edge_v1_ScalarValue_signed_value_tag;
            sample.value.value.signed_value = i == 2 ? -123 : i == 4 ? -123456 : INT64_MIN;
        } else if (i == 3 || i == 5 || i == 8) {
            sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
            sample.value.value.unsigned_value = i == 3 ? UINT16_MAX : i == 5 ? UINT32_MAX : UINT64_MAX;
        } else { sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag; sample.value.value.double_value = 1.5; }
        edge_modbus_gateway_update(gateway, platform, &sample, 1);
        CHECK(read_native(gateway, 3, i * 10 + 2, words[i], response) == 9 + 2 * words[i]);
        CHECK(response[8] == words[i] * 2);
        uint64_t actual = 0;
        for (unsigned w = 0; w < words[i]; ++w) actual = (actual << 16) | word(response + 9 + w * 2);
        CHECK(actual == bits[i]);
        if (words[i] < 4) {
            CHECK(read_native(gateway, 3, i * 10 + 2 + words[i], 4 - words[i], response) == 9 + 2 * (4 - words[i]));
            for (unsigned b = 9; b < 9 + 2 * (4 - words[i]); ++b) CHECK(response[b] == 0);
        }
        CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, sample.element_id, &point, 1));
        CHECK(point.quality == 1);
        if (i == 8) CHECK(!strcmp(point.value, "18446744073709551615"));
    }
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[8] == 1 && response[9] == 3);
    CHECK(read_native(gateway, 2, 2, 1, response) == 9 && response[8] == 2);
    CHECK(read_native(gateway, 2, 0, 0, response) == 9 && response[8] == 3);
    CHECK(read_native(gateway, 2, 0, 2001, response) == 9 && response[8] == 3);
    CHECK(read_native(gateway, 5, 0, 1, response) == 9 && response[8] == 1);
    /* The input-register view remains the existing four-word 64-bit format. */
    CHECK(read_native(gateway, 4, 62, 4, response) == 17 && word(response+9) == 0x3ff8);
    CHECK(read_native(gateway, 4, 22, 4, response) == 17 && word(response+9) == 0xffff && word(response+15) == 0xff85);
    /* Mixing input bits and holding registers is rejected without changes. */
    iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
    order.expected_digest.size = 32; order.addresses.size = 4; order.addresses.bytes[1] = 20;
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(read_native(gateway, 3, 22, 1, response) == 11 && word(response+9) == 0xff85);
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[9] == 3);
    /* Boolean rows reorder within 1X and keep every numeric slot intact. */
    edge_acquisition_sample boolean = {0}; boolean.device_id[0] = 4; boolean.valid = true;
    strcpy(boolean.element_id, "typed-0"); boolean.value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    edge_modbus_gateway_update(gateway, platform, &boolean, 1);
    order.addresses.bytes[1] = 10;
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[9] == 1);
    CHECK(read_native(gateway, 3, 22, 1, response) == 11 && word(response+9) == 0xff85);
    /* Int16, UInt16 and Float32 share 4X, while Bool slots remain fixed. */
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    order.addresses.size = 6;
    order.addresses.bytes[1] = 60; order.addresses.bytes[3] = 20; order.addresses.bytes[5] = 30;
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(read_native(gateway, 3, 22, 2, response) == 13 && word(response+9) == 0x3fc0);
    CHECK(read_native(gateway, 3, 32, 1, response) == 11 && word(response+9) == 0xff85);
    CHECK(read_native(gateway, 3, 62, 1, response) == 11 && word(response+9) == 0xffff);
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[9] == 1);
    /* Native integer conversion must reject overflow and fractional values. */
    edge_acquisition_sample sample = {0}; sample.device_id[0] = 4; sample.valid = true;
    strcpy(sample.element_id, "typed-2"); sample.value.which_value = iot_edge_v1_ScalarValue_signed_value_tag;
    sample.value.value.signed_value = 32768;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_native(gateway, 3, 31, 1, response) == 11 && word(response+9) == 2);
    CHECK(read_native(gateway, 4, 31, 1, response) == 11 && word(response+9) == 1);
    sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag; sample.value.value.double_value = 1.25;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_native(gateway, 3, 31, 1, response) == 11 && word(response+9) == 2);
    strcpy(sample.element_id, "typed-6"); sample.value.value.double_value = 1e300;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_native(gateway, 3, 21, 1, response) == 11 && word(response+9) == 2);
    CHECK(read_native(gateway, 4, 21, 1, response) == 11 && word(response+9) == 1);
    edge_modbus_gateway_free(gateway);
    gateway = edge_modbus_gateway_prepare(path, &source, 1);
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(gateway && edge_modbus_gateway_describe(gateway, platform, sample.device_id, "typed-0", &point, 1));
    CHECK(point.value_address == 1 && point.value_words == 1 && !strcmp(point.register_type,"DISCRETE_INPUT"));
    edge_modbus_gateway_free(gateway); remove(path);
}

int main(void) {
    test_native_types();
    test_drag_permutation();
    const char *path = "gateway-test.map";
    remove(path);
    remove("gateway-test.csv");
    uint8_t platform[16] = {1}, other_platform[16] = {2};
    iot_edge_v1_ConfigItem items[5] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    items[1].item.device.device_id.bytes[0] = 4;
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    items[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(items[1].item.device.name, "pump");
    items[1].item.device.online_timeout_sec = 5;
    for (size_t i = 2; i < 4; ++i) {
        items[i].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        items[i].item.modbus_register.device_id.size = 16;
        items[i].item.modbus_register.device_id.bytes[0] = 4;
        strcpy(items[i].item.modbus_register.element_id, i == 2 ? "temperature" : "counter");
    }
    edge_runtime_config config = {.items = items, .item_count = 4};
    edge_acquisition_source sources[2] = {{.platform_id = platform, .config = &config},
                                          {.platform_id = other_platform, .config = &config}};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway);
    uint8_t response[260];
    CHECK(read_registers(gateway, 0, 10, response, 1000) == 29);
    CHECK(word(response) == 0x1234 && word(response + 4) == 23);
    CHECK(word(response + 11) == 0); /* No invented valid zero before sampling. */
    edge_acquisition_sample sample = {0};
    sample.device_id[0] = 4;
    strcpy(sample.element_id, "counter");
    sample.valid = true;
    sample.observed_at_ms = 1760000000123LL;
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = UINT64_MAX;
    edge_modbus_gateway_update(gateway, platform, &sample, 1000);
    CHECK(read_registers(gateway, 10, 10, response, 5999) == 29);
    CHECK(word(response + 9) == 4 && word(response + 11) == 1);
    for (size_t i = 0; i < 4; ++i)
        CHECK(word(response + 13 + i * 2) == 0xffff);
    uint64_t timestamp = 0;
    for (size_t i = 0; i < 4; ++i)
        timestamp = (timestamp << 16) | word(response + 21 + i * 2);
    CHECK(timestamp == (uint64_t)sample.observed_at_ms);
    CHECK(read_registers(gateway, 30, 10, response, 1000) == 29 && word(response + 11) == 0);
    iot_edge_v1_ModbusGatewayPoint described = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "counter", &described,
                                       1000));
    CHECK(described.has_mapping && described.address == 10 &&
          !strcmp(described.value, "18446744073709551615"));
    /* Confirmed drag order moves samples with their identities and preserves
     * foreign-platform slots. Stale, duplicate and invalid writes fail closed. */
    iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
    order.expected_digest.size = 32;
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    order.addresses.size = 4;
    order.addresses.bytes[1] = 10;
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "counter", &described, 1000));
    CHECK(described.address == 0 && !strcmp(described.value, "18446744073709551615"));
    CHECK(read_registers(gateway, 2, 4, response, 1000) == 17 && word(response + 9) == 0xffff);
    CHECK(read_registers(gateway, 30, 10, response, 1000) == 29 && word(response + 11) == 0);
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Stale digest. */
    edge_modbus_gateway *reloaded = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(reloaded && edge_modbus_gateway_describe(reloaded, platform, sample.device_id, "counter", &described, 0));
    CHECK(described.address == 0 && described.quality == 0);
    edge_modbus_gateway_free(reloaded);
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    order.addresses.bytes[3] = 10;
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Duplicate. */
    order.addresses.bytes[3] = 20;
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Another platform. */
    order.addresses.bytes[3] = 1;
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Misaligned. */
    order.addresses.bytes[3] = 0;
    CHECK(!edge_modbus_gateway_reorder(gateway, "no-such-directory/map", platform, &order));
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "counter", &described, 1000) && described.address == 0);
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Restore. */
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "counter", &described, 1000) && described.address == 10);
    iot_edge_v1_ModbusGatewayRequest page_request = iot_edge_v1_ModbusGatewayRequest_init_zero;
    page_request.include_points = true;
    page_request.limit = 1;
    page_request.offset = 1;
    iot_edge_v1_ModbusGatewayResult page_result = iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &page_request,
                                     &page_result, 1000));
    CHECK(page_result.total == 2 && page_result.points_count == 1 && page_result.offset == 1);
    CHECK(page_result.points[0].address == 10 &&
          !strcmp(page_result.points[0].element_id, "counter"));
    CHECK(!strcmp(page_result.points[0].device_name, "pump") &&
          !strcmp(page_result.points[0].source_protocol, "Modbus"));
    CHECK(!strcmp(page_result.points[0].value, "18446744073709551615"));
    page_result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, other_platform, &config, true, true, &page_request,
                                     &page_result, 1000));
    CHECK(page_result.points[0].address == 30 && !page_result.points[0].value[0]);
    items[4].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
    items[4].item.derived_point.device_id.size = 16;
    items[4].item.derived_point.device_id.bytes[0] = 4;
    strcpy(items[4].item.derived_point.point_id, "derived");
    strcpy(items[4].item.derived_point.name, "derived flow");
    config.item_count = 5;
    page_request.limit = 8;
    page_request.offset = 0;
    page_result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &page_request,
                                     &page_result, 1000));
    CHECK(page_result.total == 3 && page_result.points_count == 3 &&
          !page_result.points[2].has_mapping);
    CHECK(!strcmp(page_result.points[2].state, "unsupported"));
    page_request.offset = 3;
    page_result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &page_request,
                                     &page_result, 1000));
    CHECK(page_result.total == 3 && !page_result.points_count);
    page_request.offset = 0;
    items[1].item.device.enabled = false;
    page_result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &page_request,
                                     &page_result, 1000));
    CHECK(page_result.points[0].has_mapping && !strcmp(page_result.points[0].state, "disabled"));
    items[1].item.device.enabled = true;
    page_result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, false, false, &page_request,
                                     &page_result, 1000));
    CHECK(!strcmp(page_result.points[0].state, "gateway_disabled"));
    page_result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, false, &page_request,
                                     &page_result, 1000));
    CHECK(!strcmp(page_result.points[0].state, "listener_error"));
    page_request.limit = 9;
    CHECK(!edge_modbus_gateway_points(gateway, platform, &config, true, true, &page_request,
                                      &page_result, 1000));
    config.item_count = 4;

    CHECK(read_registers(gateway, 11, 1, response, 6000) == 11 && word(response + 9) == 3);
    sample.value.which_value = iot_edge_v1_ScalarValue_signed_value_tag;
    sample.value.value.signed_value = INT64_MIN;
    edge_modbus_gateway_update(gateway, platform, &sample, 7000);
    CHECK(read_registers(gateway, 12, 4, response, 7000) == 17 && word(response + 9) == 0x8000);
    sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag;
    sample.value.value.double_value = 1.0;
    edge_modbus_gateway_update(gateway, platform, &sample, 7000);
    CHECK(read_registers(gateway, 12, 4, response, 7000) == 17 && word(response + 9) == 0x3ff0);
    sample.value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    sample.value.value.bool_value = true;
    edge_modbus_gateway_update(gateway, platform, &sample, 7000);
    CHECK(read_registers(gateway, 15, 1, response, 7000) == 11 && word(response + 9) == 1);
    sample.value.which_value = iot_edge_v1_ScalarValue_decimal_value_tag;
    strcpy(sample.value.value.decimal_value, "12.5");
    edge_modbus_gateway_update(gateway, platform, &sample, 7000);
    CHECK(read_registers(gateway, 10, 4, response, 7000) == 17 && word(response + 9) == 5 &&
          word(response + 13) == 0x4029);
    strcpy(sample.value.value.decimal_value, "invalid");
    edge_modbus_gateway_update(gateway, platform, &sample, 7000);
    CHECK(read_registers(gateway, 11, 1, response, 7000) == 11 && word(response + 9) == 2);
    CHECK(read_registers(gateway, 65535, 2, response, 1000) == 9 && response[8] == 2);
    CHECK(read_registers(gateway, 0, 0, response, 1000) == 9 && response[8] == 3);
    CHECK(read_registers(gateway, 0, 126, response, 1000) == 9 && response[8] == 3);
    uint8_t request[12] = {0, 1, 0, 0, 0, 6, 1, 6};
    CHECK(edge_modbus_gateway_reply(gateway, request, 12, response, 0) == 9 &&
          response[7] == 0x86 && response[8] == 1);
    request[7] = 3;
    request[11] = 1;
    CHECK(edge_modbus_gateway_reply(gateway, request, 12, response, 0) == 11 && response[7] == 3);
    request[2] = 1;
    CHECK(!edge_modbus_gateway_reply(gateway, request, 12, response, 0));
    request[2] = 0;
    request[6] = 0;
    CHECK(!edge_modbus_gateway_reply(gateway, request, 12, response, 0));
    CHECK(edge_modbus_gateway_export(gateway, "gateway-test.csv"));
    edge_modbus_gateway_free(gateway);
    /* Reorder config and platforms: reservations survive process restart. */
    iot_edge_v1_ConfigItem temporary = items[2];
    items[2] = items[3];
    items[3] = temporary;
    edge_acquisition_source source = sources[0];
    sources[0] = sources[1];
    sources[1] = source;
    gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway);
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = 42;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_registers(gateway, 15, 1, response, 1) == 11 && word(response + 9) == 42);
    edge_modbus_gateway_update(gateway, platform, NULL, 2);
    CHECK(read_registers(gateway, 11, 1, response, 2) == 11 && word(response + 9) == 2);
    edge_modbus_gateway_free(gateway);
    config.item_count = 3; /* Delete temperature, keeping counter at address 10. */
    gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway && read_registers(gateway, 0, 1, response, 0) == 9 && response[8] == 2);
    CHECK(read_registers(gateway, 10, 1, response, 0) == 11);
    edge_modbus_gateway_free(gateway);
    strcpy(items[2].item.modbus_register.element_id, "new-point");
    gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway && read_registers(gateway, 40, 10, response, 0) == 29);
    CHECK(read_registers(gateway, 10, 1, response, 0) == 9 && response[8] == 2);
    FILE *file = fopen(path, "ab");
    CHECK(file);
    fputs("corrupt\n", file);
    fclose(file);
    CHECK(!edge_modbus_gateway_prepare(path, sources, 2));
    CHECK(!edge_modbus_gateway_prepare("no-such-directory/map", sources, 2));
    CHECK(read_registers(gateway, 40, 10, response, 0) == 29);
    edge_modbus_gateway_free(gateway);
    remove(path);
    remove("gateway-test.csv");
    puts("Modbus gateway checks passed");
    return 0;
}
