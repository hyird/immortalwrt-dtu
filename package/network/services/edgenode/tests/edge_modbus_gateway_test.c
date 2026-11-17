#include "edge_modbus_gateway.h"
#include <float.h>
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
        strcpy(items[i].item.modbus_register.data_type, "UINT16");
        items[i].item.modbus_register.scale = 1;
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
    const unsigned addresses[] = {0,1,0,1,2,4,6,8,12,16};
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
        CHECK(point.has_mapping && point.value_address == addresses[i]);
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
        if (i < 2) {
            CHECK(read_native(gateway, 2, addresses[i], 1, response) == 10 && response[9] == 1);
        } else {
            CHECK(read_native(gateway, 3, addresses[i], words[i], response) == 9 + 2 * words[i]);
            CHECK(response[8] == words[i] * 2);
            uint64_t actual = 0;
            for (unsigned w = 0; w < words[i]; ++w) actual = (actual << 16) | word(response + 9 + w * 2);
            CHECK(actual == bits[i]);
        }
        CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, sample.element_id, &point, 1));
        CHECK(point.quality == 1);
        if (i == 8) CHECK(!strcmp(point.value, "18446744073709551615"));
    }
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[8] == 1 && response[9] == 3);
    CHECK(read_native(gateway, 2, 1, 9, response) == 11 && response[9] == 1 && response[10] == 0);
    CHECK(read_native(gateway, 2, 0, 0, response) == 9 && response[8] == 3);
    CHECK(read_native(gateway, 2, 0, 2001, response) == 9 && response[8] == 3);
    CHECK(read_native(gateway, 0x7f, 0, 1, response) == 9 && response[8] == 1);
    /* FC04 is exactly the same compact value-only view as FC03. */
    CHECK(read_native(gateway, 4, 16, 4, response) == 17 && word(response+9) == 0x3ff8);
    CHECK(read_native(gateway, 4, 0, 1, response) == 11 && word(response+9) == 0xff85);
    /* The last value and zero-filled tail share one successful request. */
    CHECK(read_native(gateway, 3, 16, 8, response) == 25 && word(response+9) == 0x3ff8);
    for (unsigned i = 17; i < 25; ++i) CHECK(response[i] == 0);
    CHECK(edge_modbus_gateway_export(gateway, "gateway-native-test.csv"));
    FILE *csv = fopen("gateway-native-test.csv", "rb"); CHECK(csv);
    char text[4096]; size_t length = fread(text, 1, sizeof(text)-1, csv); text[length] = 0;
    CHECK(fclose(csv) == 0);
    CHECK(strstr(text, "register_type,address,words\n") &&
          strstr(text, "\"typed-2\",HOLDING_REGISTER,0,1\n") &&
          strstr(text, "\"typed-1\",DISCRETE_INPUT,1,1\n"));
    remove("gateway-native-test.csv");
    /* Mixing input bits and holding registers is rejected without changes. */
    iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
    order.expected_digest.size = 32; order.addresses.size = 4; order.addresses.bytes[1] = 20;
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(read_native(gateway, 3, 0, 1, response) == 11 && word(response+9) == 0xff85);
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[9] == 3);
    /* Boolean rows reorder within 1X and keep every numeric slot intact. */
    edge_acquisition_sample boolean = {0}; boolean.device_id[0] = 4; boolean.valid = true;
    strcpy(boolean.element_id, "typed-0"); boolean.value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    edge_modbus_gateway_update(gateway, platform, &boolean, 1);
    order.addresses.bytes[1] = 10;
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[9] == 1);
    CHECK(read_native(gateway, 3, 0, 1, response) == 11 && word(response+9) == 0xff85);
    /* Int16, UInt16 and Float32 share 4X, while Bool slots remain fixed. */
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    order.addresses.size = 6;
    order.addresses.bytes[1] = 60; order.addresses.bytes[3] = 20; order.addresses.bytes[5] = 30;
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(read_native(gateway, 3, 0, 2, response) == 13 && word(response+9) == 0x3fc0);
    CHECK(read_native(gateway, 3, 2, 1, response) == 11 && word(response+9) == 0xff85);
    CHECK(read_native(gateway, 3, 7, 1, response) == 11 && word(response+9) == 0xffff);
    CHECK(read_native(gateway, 2, 0, 2, response) == 10 && response[9] == 1);
    /* Native integer conversion must reject overflow and fractional values. */
    edge_acquisition_sample sample = {0}; sample.device_id[0] = 4; sample.valid = true;
    strcpy(sample.element_id, "typed-2"); sample.value.which_value = iot_edge_v1_ScalarValue_signed_value_tag;
    sample.value.value.signed_value = 32768;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_native(gateway, 3, 2, 1, response) == 11 && word(response+9) == 0);
    iot_edge_v1_ModbusGatewayPoint converted = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "typed-2", &converted, 1));
    CHECK(converted.quality == 2 && converted.value_address == 2 && converted.value_words == 1);
    CHECK(read_native(gateway, 4, 2, 1, response) == 11 && word(response+9) == 0);
    sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag; sample.value.value.double_value = 1.25;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_native(gateway, 3, 2, 1, response) == 11 && word(response+9) == 0);
    strcpy(sample.element_id, "typed-6"); sample.value.value.double_value = 1e300;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(read_native(gateway, 3, 0, 2, response) == 13 && word(response+9) == 0 && word(response+11) == 0);
    CHECK(edge_modbus_gateway_describe(gateway, platform, sample.device_id, "typed-6", &converted, 1));
    CHECK(converted.quality == 2 && converted.value_words == 2 && converted.value_address == 0);
    CHECK(read_native(gateway, 4, 0, 2, response) == 13 && word(response+9) == 0 && word(response+11) == 0);
    edge_modbus_gateway_free(gateway);
    gateway = edge_modbus_gateway_prepare(path, &source, 1);
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(gateway && edge_modbus_gateway_describe(gateway, platform, sample.device_id, "typed-0", &point, 1));
    CHECK(point.value_address == 1 && point.value_words == 1 && !strcmp(point.register_type,"DISCRETE_INPUT"));
    edge_modbus_gateway_free(gateway); remove(path);
}

static void test_cross_platform_reorder(void) {
    const char *path = "gateway-cross-order-test.map";
    remove(path);
    uint8_t platform[16] = {1}, foreign[16] = {2}, device[16] = {4};
    iot_edge_v1_ConfigItem items[4] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    memcpy(items[1].item.device.device_id.bytes, device, 16);
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    for (unsigned i = 2; i < 4; ++i) {
        items[i].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &items[i].item.modbus_register;
        point->device_id.size = 16; memcpy(point->device_id.bytes, device, 16);
        strcpy(point->element_id, i == 2 ? "short" : "wide");
        strcpy(point->data_type, i == 2 ? "UINT16" : "UINT64"); point->scale = 1;
    }
    edge_runtime_config prefix = {.items = items, .item_count = 3};
    edge_runtime_config full = {.items = items, .item_count = 4};
    edge_acquisition_source sources[] = {{.platform_id = platform, .config = &prefix},
                                        {.platform_id = foreign, .config = &prefix},
                                        {.platform_id = platform, .config = &full}};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, sources, 3); CHECK(gateway);
    edge_acquisition_sample sample = {.valid = true};
    memcpy(sample.device_id, device, 16); strcpy(sample.element_id, "wide");
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = UINT64_C(0x1122334455667788);
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    uint8_t old[32], current[32], response[260];
    CHECK(edge_modbus_gateway_digest(gateway, foreign, old));
    iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
    order.expected_digest.size = 32; order.addresses.size = 4; order.addresses.bytes[1] = 20;
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    CHECK(!edge_modbus_gateway_reorder(gateway, "no-such-directory/map", platform, &order));
    CHECK(edge_modbus_gateway_digest(gateway, foreign, current) && !memcmp(old, current, sizeof(old)));
    CHECK(read_native(gateway, 3, 2, 4, response) == 17 && word(response+9) == 0x1122);
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    CHECK(edge_modbus_gateway_digest(gateway, foreign, current) && memcmp(old, current, sizeof(old)));
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Stale. */
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    order.addresses.bytes[1] = 0;
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Duplicate. */
    order.addresses.bytes[1] = 10;
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Foreign. */
    order.addresses.bytes[1] = 1;
    CHECK(!edge_modbus_gateway_reorder(gateway, path, platform, &order)); /* Not an identity token. */
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "short", &point, 1));
    CHECK(point.address == 10 && point.value_address == 4 && point.value_words == 1);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "short", &point, 1));
    CHECK(point.address == 20 && point.value_address == 5);
    CHECK(read_native(gateway, 3, 0, 4, response) == 17 && word(response+9) == 0x1122 &&
          word(response+15) == 0x7788);
    CHECK(read_native(gateway, 4, 0, 4, response) == 17 && word(response+9) == 0x1122);
    edge_modbus_gateway_free(gateway);
    gateway = edge_modbus_gateway_prepare(path, sources, 3); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "short", &point, 1));
    CHECK(point.address == 10 && point.value_address == 4 && point.quality == 0);
    edge_modbus_gateway_free(gateway); remove(path);
}

static void check_zero_space(edge_modbus_gateway *gateway) {
    uint8_t response[260];
    const unsigned functions[] = {1, 2, 3, 4};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned function = functions[i];
        const bool bits = function == 1 || function == 2;
        CHECK(read_native(gateway, function, 65535, 1, response) == (bits ? 10 : 11));
        CHECK(response[9] == 0 && (bits || response[10] == 0));
        CHECK(read_native(gateway, function, 65535, 2, response) == 9 && response[8] == 2);
        CHECK(read_native(gateway, function, 65411, 125, response) == (bits ? 25 : 259));
        for (unsigned b = 9; b < (bits ? 25U : 259U); ++b) CHECK(response[b] == 0);
        CHECK(read_native(gateway, function, 0, 0, response) == 9 && response[8] == 3);
        const unsigned excess = bits ? 2001 : 126;
        CHECK(read_native(gateway, function, 0, excess, response) == 9 && response[8] == 3);
        uint8_t short_request[11] = {0,1,0,0,0,5,1,0,0,0,1};
        short_request[7] = (uint8_t)function;
        CHECK(edge_modbus_gateway_reply(gateway, short_request, sizeof(short_request), response, 1) == 9 &&
              response[8] == 3);
    }
}

static void test_compact_lifecycle(void) {
    const char *path = "gateway-lifecycle-test.map";
    remove(path);
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, NULL, 0);
    CHECK(gateway);
    check_zero_space(gateway);
    uint8_t response[260];
    CHECK(read_native(gateway, 3, 0, 125, response) == 259);
    for (unsigned b = 9; b < 259; ++b) CHECK(response[b] == 0);
    CHECK(read_native(gateway, 2, 0, 2000, response) == 259);
    for (unsigned b = 9; b < 259; ++b) CHECK(response[b] == 0);
    edge_modbus_gateway_free(gateway);

    uint8_t platform[16] = {1}, foreign[16] = {2}, device[16] = {4};
    iot_edge_v1_ConfigItem items[6] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    memcpy(items[1].item.device.device_id.bytes, device, 16);
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    const char *types[] = {"UINT16", "BOOL", "UINT32", "STRING"};
    for (unsigned i = 0; i < 4; ++i) {
        items[i+2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &items[i+2].item.modbus_register;
        point->device_id.size = 16; memcpy(point->device_id.bytes, device, 16);
        snprintf(point->element_id, sizeof(point->element_id), "life-%u", i);
        strcpy(point->data_type, types[i]); point->scale = 1;
    }
    edge_runtime_config config = {.items = items, .item_count = 6};
    edge_acquisition_source sources[] = {{.platform_id = platform, .config = &config},
                                        {.platform_id = foreign, .config = &config}};
    gateway = edge_modbus_gateway_prepare(path, sources, 2);
    CHECK(gateway); check_zero_space(gateway);
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "life-0", &point, 1));
    CHECK(point.address == 40 && point.value_address == 3 && point.has_mapping);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "life-3", &point, 1));
    CHECK(!point.has_mapping && !point.value_words && !point.register_type[0]);
    /* Unknown configuration never acquires a layout from the sample's type. */
    edge_acquisition_sample sample = {.valid = true};
    memcpy(sample.device_id, device, 16); strcpy(sample.element_id, "life-3");
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = 17;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "life-3", &point, 1));
    CHECK(!point.has_mapping && !point.value_words && !point.register_type[0]);
    iot_edge_v1_ModbusGatewayOrder stale = iot_edge_v1_ModbusGatewayOrder_init_zero;
    stale.expected_digest.size = 32;
    CHECK(edge_modbus_gateway_digest(gateway, foreign, stale.expected_digest.bytes));
    stale.addresses.size = 4; stale.addresses.bytes[1] = 60; stale.addresses.bytes[3] = 40;
    uint8_t digest[32]; memcpy(digest, stale.expected_digest.bytes, sizeof(digest));
    edge_modbus_gateway_free(gateway);
    /* A width change in another platform shifts this platform's primary view,
     * without moving its persistent identities. Its old digest must expire. */
    edge_runtime_config first = config;
    iot_edge_v1_ConfigItem changed[6]; memcpy(changed, items, sizeof(changed));
    strcpy(changed[2].item.modbus_register.data_type, "UINT64");
    first.items = changed; sources[0].config = &first;
    gateway = edge_modbus_gateway_prepare(path, sources, 2); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "life-0", &point, 1));
    CHECK(point.address == 40 && point.value_address == 6 && point.value_words == 1);
    uint8_t current[32]; CHECK(edge_modbus_gateway_digest(gateway, foreign, current));
    CHECK(memcmp(digest, current, sizeof(digest)) &&
          !edge_modbus_gateway_reorder(gateway, path, foreign, &stale));
    edge_modbus_gateway_free(gateway);
    /* Disable only the first platform; no holes in either compact space. */
    changed[1].item.device.enabled = false;
    gateway = edge_modbus_gateway_prepare(path, sources, 2); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "life-0", &point, 1));
    CHECK(!point.has_mapping && !point.value_words && !point.register_type[0]);
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "life-0", &point, 1));
    CHECK(point.address == 40 && point.value_address == 0);
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "life-1", &point, 1));
    CHECK(point.value_address == 0 && !strcmp(point.register_type, "DISCRETE_INPUT"));
    strcpy(sample.element_id, "life-0"); sample.value.value.unsigned_value = 0x1234;
    edge_modbus_gateway_update(gateway, foreign, &sample, 1);
    CHECK(read_native(gateway, 3, 0, 5, response) == 19 && word(response+9) == 0x1234);
    for (unsigned b = 11; b < 19; ++b) CHECK(response[b] == 0);
    CHECK(read_native(gateway, 4, 0, 5, response) == 19 && word(response+9) == 0x1234);
    for (unsigned b = 11; b < 19; ++b) CHECK(response[b] == 0);
    edge_modbus_gateway_free(gateway);
    /* Delete the first foreign point: next numeric point compacts to zero. */
    items[2] = items[3]; items[3] = items[4]; items[4] = items[5];
    config.item_count = 5;
    gateway = edge_modbus_gateway_prepare(path, sources, 2); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "life-0", &point, 1));
    CHECK(!point.has_mapping);
    CHECK(edge_modbus_gateway_describe(gateway, foreign, device, "life-2", &point, 1));
    CHECK(point.value_address == 0 && point.value_words == 2);
    edge_modbus_gateway_free(gateway);
    sources[0].config = &config;
    gateway = edge_modbus_gateway_prepare(path, sources, 2); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "life-2", &point, 1));
    CHECK(point.address == 20 && point.value_address == 0);
    edge_modbus_gateway_free(gateway);
    remove(path);
}

static size_t write_adu(uint8_t request[260], unsigned function, unsigned address,
                         unsigned quantity, const uint8_t *data) {
    const unsigned bytes = function == 5 || function == 6 ? 2 :
        function == 15 ? (quantity + 7) / 8 : quantity * 2;
    const size_t size = (function == 5 || function == 6 ? 10 : 13) + bytes;
    memset(request, 0, size);
    request[1] = 12; request[4] = (uint8_t)((size - 6) >> 8);
    request[5] = (uint8_t)(size - 6); request[6] = 1; request[7] = (uint8_t)function;
    request[8] = (uint8_t)(address >> 8); request[9] = (uint8_t)address;
    if (function != 5 && function != 6) {
        request[10] = (uint8_t)(quantity >> 8); request[11] = (uint8_t)quantity;
        request[12] = (uint8_t)bytes;
    }
    if (data) memcpy(request + (function == 5 || function == 6 ? 10 : 13), data, bytes);
    return size;
}

static void check_write_reply(edge_modbus_gateway *gateway, const uint8_t *request,
                               size_t size, unsigned exception) {
    uint8_t response[260];
    const size_t reply_size = edge_modbus_gateway_reply(gateway, request, size, response, 1);
    CHECK(reply_size == (exception ? 9U : 12U));
    if (exception) CHECK(response[7] == (request[7] | 0x80) && response[8] == exception);
    else CHECK(response[7] == request[7] && word(response+4) == 6 &&
               !memcmp(response+8, request+8, 4));
}

static void test_write_plans(void) {
    const char *path = "gateway-write-test.map";
    remove(path);
    uint8_t platform[16] = {1}, device[16] = {4};
    iot_edge_v1_ConfigItem items[11] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    memcpy(items[1].item.device.device_id.bytes, device, 16);
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    const char *types[] = {"BOOL","BOOL","INT16","UINT32","FLOAT32","INT64","UINT64","DOUBLE","FLOAT32"};
    for (unsigned i = 0; i < 9; ++i) {
        items[i+2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &items[i+2].item.modbus_register;
        point->device_id.size = 16; memcpy(point->device_id.bytes, device, 16);
        snprintf(point->element_id, sizeof(point->element_id), "write-%u", i);
        strcpy(point->data_type, types[i]); point->scale = 1; point->decimals = -1;
        point->writable = i != 1 && i != 8;
    }
    edge_runtime_config config = {.items = items, .item_count = 11};
    edge_acquisition_source source = {.platform_id = platform, .config = &config};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    uint8_t request[260], response[260];
    const uint8_t values[][8] = {
        {0xff,0xff}, {0,1,0,2}, {0x3f,0xc0,0,0},
        {0x80,0,0,0,0,0,0,0}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
        {0x3f,0xf8,0,0,0,0,0,0}
    };
    const unsigned addresses[] = {0,1,3,5,9,13};
    const unsigned quantities[] = {1,2,2,4,4,4};
    const char *expected[] = {"-1","65538","1.5","-9223372036854775808",
                              "18446744073709551615","1.5"};
    edge_modbus_gateway_write write;
    for (unsigned i = 0; i < 6; ++i) {
        const size_t size = write_adu(request, i ? 16 : 6, addresses[i], quantities[i], values[i]);
        check_write_reply(gateway, request, size, 0);
        uint32_t cursor = 0;
        CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1);
        CHECK(!strcmp(write.value, expected[i]) && !memcmp(write.platform_id, platform, 16) &&
              !memcmp(write.device_id, device, 16) && !write.external_io);
        CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 0);
        CHECK(read_native(gateway, 3, addresses[i], quantities[i], response) == 9 + 2 * quantities[i]);
        for (unsigned b = 9; b < 9 + 2 * quantities[i]; ++b) CHECK(response[b] == 0);
    }
    /* A full request is validated before any caller may submit a target. */
    const uint8_t batch[] = {0xff,0xff,0,1,0,2,0x3f,0xc0,0,0};
    size_t size = write_adu(request, 16, 0, 5, batch);
    check_write_reply(gateway, request, size, 0);
    uint32_t cursor = 0;
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1);
        CHECK(!strcmp(write.value, expected[i]));
    }
    CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 0);
    request[19] = 0x7f; request[20] = 0xc0; /* Float32 NaN after two valid targets. */
    check_write_reply(gateway, request, size, 3);
    const uint8_t nan[] = {0x7f,0xc0,0,0};
    size = write_adu(request, 16, 3, 2, nan); check_write_reply(gateway, request, size, 3);
    const uint8_t infinity[] = {0x7f,0xf0,0,0,0,0,0,0};
    size = write_adu(request, 16, 13, 4, infinity); check_write_reply(gateway, request, size, 3);
    /* Read-only float data is ignored, even when the attempted bits are NaN. */
    size = write_adu(request, 16, 17, 2, nan); check_write_reply(gateway, request, size, 0);
    cursor = 0; CHECK(!edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write));
    const uint8_t replacement[] = {0xab,0xcd};
    size = write_adu(request, 6, 2, 1, replacement);
    check_write_reply(gateway, request, size, 4); /* Partial write has no valid history. */
    edge_acquisition_sample sample = {.valid = true};
    memcpy(sample.device_id, device, 16); strcpy(sample.element_id, "write-3");
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = 0x11223344;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    check_write_reply(gateway, request, size, 0);
    cursor = 0; CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1);
    char merged[32]; snprintf(merged, sizeof(merged), "%u", 0x1122abcdU);
    CHECK(!strcmp(write.value, merged));
    CHECK(read_native(gateway, 3, 1, 2, response) == 13 &&
          word(response+9) == 0x1122 && word(response+11) == 0x3344);
    sample.valid = false; edge_modbus_gateway_update(gateway, platform, &sample, 2);
    check_write_reply(gateway, request, size, 4);
    const uint8_t on[] = {0xff,0};
    size = write_adu(request, 5, 0, 1, on); check_write_reply(gateway, request, size, 0);
    cursor = 0; CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1 &&
                      !strcmp(write.value, "1"));
    CHECK(read_native(gateway, 1, 0, 2, response) == 10 && response[9] == 0);
    size = write_adu(request, 5, 1, 1, on); check_write_reply(gateway, request, size, 0);
    cursor = 0; CHECK(!edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write));
    const uint8_t packed[] = {1,0};
    size = write_adu(request, 15, 0, 10, packed); check_write_reply(gateway, request, size, 0);
    cursor = 0; CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1 &&
                      !strcmp(write.value, "1"));
    CHECK(!edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write));
    request[12] = 1; check_write_reply(gateway, request, size, 3);
    size = write_adu(request, 5, 0, 1, replacement); check_write_reply(gateway, request, size, 3);
    size = write_adu(request, 5, 65535, 1, on); check_write_reply(gateway, request, size, 0);
    size = write_adu(request, 6, 65535, 1, replacement); check_write_reply(gateway, request, size, 0);
    size = write_adu(request, 15, 65535, 2, NULL); check_write_reply(gateway, request, size, 2);
    size = write_adu(request, 16, 65535, 2, NULL); check_write_reply(gateway, request, size, 2);
    size = write_adu(request, 15, 0, 1968, NULL); check_write_reply(gateway, request, size, 0);
    request[10] = 7; request[11] = 0xb1; check_write_reply(gateway, request, size, 3); /* 1969 bits. */
    size = write_adu(request, 16, 100, 123, NULL); check_write_reply(gateway, request, size, 0);
    request[11] = 124; check_write_reply(gateway, request, size, 3);
    request[10] = request[11] = 0; check_write_reply(gateway, request, size, 3);
    size = write_adu(request, 16, 100, 1, NULL);
    request[12] = 1; check_write_reply(gateway, request, size, 3);
    size = write_adu(request, 6, 100, 1, replacement);
    request[5] = 7; request[12] = 0; check_write_reply(gateway, request, 13, 3);
    edge_modbus_gateway_free(gateway);
    /* External output permission is taken from DO kind, not a blanket gateway
     * flag; external inputs remain acknowledge-and-ignore. */
    items[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    items[2].which_item = iot_edge_v1_ConfigItem_external_io_tag;
    items[2].item.external_io.device_id.size = 16;
    memcpy(items[2].item.external_io.device_id.bytes, device, 16);
    strcpy(items[2].item.external_io.point_id, "digital");
    items[2].item.external_io.kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
    config.item_count = 3;
    gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    size = write_adu(request, 5, 0, 1, on); check_write_reply(gateway, request, size, 0);
    cursor = 0;
    CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1 &&
          write.external_io && !strcmp(write.element_id, "digital") && !strcmp(write.value, "1"));
    edge_modbus_gateway_free(gateway);
    items[2].item.external_io.kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI;
    gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    check_write_reply(gateway, request, size, 0);
    cursor = 0; CHECK(!edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write));
    edge_modbus_gateway_free(gateway); remove(path);
}

static void test_scaled_write_plans(void) {
    static const struct {
        const char *type;
        double scale, engineering;
        int32_t decimals;
        const char *raw;
        bool scalar_only;
    } cases[] = {
        {"UINT16",0.1,20,-1,"200",false},
        {"UINT16",0.1,12.5,-1,"125",false},
        {"UINT16",0.1,6553.5,-1,"65535",false},
        {"UINT16",0.1,6553.6,-1,NULL,false},
        {"UINT16",0.1,-0.1,-1,NULL,false},
        {"UINT16",0.1,12.55,-1,NULL,false},
        {"UINT16",0,0,-1,NULL,false},
        {"UINT16",0.1,0.3,-1,"3",false},
        {"UINT16",0.1,12.5,0,NULL,false},
        {"UINT16",-0.1,-12.5,-1,"125",false},
        {"INT16",0.1,-3276.8,-1,"-32768",false},
        {"INT16",0.1,3276.8,-1,NULL,false},
        {"UINT32",0.1,429496729.5,-1,"4294967295",false},
        {"UINT32",0.1,429496729.6,-1,NULL,false},
        {"INT32",0.1,-214748364.8,-1,"-2147483648",false},
        {"INT32",0.1,214748364.8,-1,NULL,false},
        {"FLOAT32",0.1,12.5,-1,"125",false},
        {"FLOAT32",1e38,FLT_MIN,-1,NULL,false},
        {"FLOAT32",1e-38,FLT_MAX,-1,NULL,false},
        {"FLOAT32",0,0,-1,NULL,false},
        {"FLOAT32",1,12.5,0,NULL,false},
        {"DOUBLE",0.1,12.5,-1,"125",false},
        {"DOUBLE",0,0,-1,NULL,false},
        {"DOUBLE",0.1,12.55,1,NULL,false},
        {"DOUBLE",0.1,12.5,1,"125",false},
        {"DOUBLE",0.1,DBL_MAX,-1,NULL,false},
        {"DOUBLE",1,DBL_MIN / 2,-1,NULL,false},
        /* Decoding deliberately leaves 8/64-bit integer scale unused. */
        {"INT8",0.1,127,-1,"127",true},
        {"INT8",0.1,128,-1,NULL,true},
        {"UINT8",0.1,255,-1,"255",true},
        {"UINT8",0.1,256,-1,NULL,true},
        {"INT64",0.1,7,-1,"7",false},
        {"UINT64",0.1,7,-1,"7",false},
        {"WORD",0.1,12.5,-1,"125",true},
        {"DWORD",0.1,12.5,-1,"125",true},
        {"REAL",0.1,12.5,-1,"125",true},
        {"LREAL",0.1,12.5,-1,"125",true},
    };
    const char *path = "gateway-scale-test.map";
    remove(path);
    uint8_t platform[16] = {1}, device[16] = {4};
    iot_edge_v1_ConfigItem items[3] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    memcpy(items[1].item.device.device_id.bytes, device, 16);
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    edge_runtime_config config = {.items = items, .item_count = 3};
    edge_acquisition_source source = {.platform_id = platform, .config = &config};
    for (unsigned protocol = 0; protocol < 3; ++protocol) {
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            if (protocol == 2 && cases[c].scalar_only) continue;
            items[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
            if (protocol == 0) {
                items[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
                iot_edge_v1_ModbusRegisterConfig *p = &items[2].item.modbus_register;
                p->device_id.size = 16; memcpy(p->device_id.bytes, device, 16);
                strcpy(p->element_id, "scaled"); strcpy(p->data_type, cases[c].type);
                p->writable = true; p->scale = cases[c].scale; p->decimals = cases[c].decimals;
            } else if (protocol == 1) {
                items[2].which_item = iot_edge_v1_ConfigItem_s7_area_tag;
                iot_edge_v1_S7AreaConfig *p = &items[2].item.s7_area;
                p->device_id.size = 16; memcpy(p->device_id.bytes, device, 16);
                strcpy(p->element_id, "scaled"); strcpy(p->data_type, cases[c].type);
                p->writable = true; p->scale = cases[c].scale; p->decimals = cases[c].decimals;
            } else {
                items[2].which_item = iot_edge_v1_ConfigItem_industrial_point_tag;
                iot_edge_v1_IndustrialPointConfig *p = &items[2].item.industrial_point;
                p->device_id.size = 16; memcpy(p->device_id.bytes, device, 16);
                strcpy(p->element_id, "scaled"); strcpy(p->data_type, cases[c].type);
                p->writable = true; p->scale = cases[c].scale; p->decimals = cases[c].decimals;
            }
            edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, &source, 1);
            CHECK(gateway);
            iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
            CHECK(edge_modbus_gateway_describe(gateway, platform, device, "scaled", &point, 1));
            /* No config storage may be consulted after prepare. */
            memset(&items[2], 0, sizeof(items[2]));
            uint64_t bits;
            if (point.value_type == 5) memcpy(&bits, &cases[c].engineering, sizeof(bits));
            else if (point.value_type == 10) {
                const float number = (float)cases[c].engineering;
                uint32_t words; memcpy(&words, &number, sizeof(words)); bits = words;
            } else if (cases[c].engineering < 0)
                bits = (uint64_t)(int64_t)cases[c].engineering;
            else bits = (uint64_t)cases[c].engineering;
            uint8_t data[8], request[260];
            for (unsigned b = 0; b < point.value_words * 2; ++b)
                data[b] = (uint8_t)(bits >> ((point.value_words * 2 - 1 - b) * 8));
            size_t size = write_adu(request, 16, 0, point.value_words, data);
            check_write_reply(gateway, request, size, cases[c].raw ? 0 : 3);
            if (cases[c].raw) {
                uint32_t cursor = 0;
                edge_modbus_gateway_write write;
                CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1);
                CHECK(!strcmp(write.value, cases[c].raw));
                if (c == 0) {
                    edge_acquisition_sample sample = {.valid = true};
                    memcpy(sample.device_id, device, 16); strcpy(sample.element_id, "scaled");
                    sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag;
                    sample.value.value.double_value = 12.5;
                    edge_modbus_gateway_update(gateway, platform, &sample, 1);
                    size = write_adu(request, 6, 0, 1, data);
                    check_write_reply(gateway, request, size, 0);
                    cursor = 0;
                    CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1 &&
                          !strcmp(write.value, "200"));
                }
            }
            edge_modbus_gateway_free(gateway);
        }
    }
    /* Meter BCD already accepts engineering fixed-point text: don't divide by
     * scalar scale a second time, and validate its own width/precision. */
    static const struct { const char *type; double number; const char *text; } meter[] = {
        {"BCD",12.34,"12.34"}, {"BCD",100,NULL}, {"BCD",12.345,NULL},
        {"BCD",-1,NULL}, {"BCD_SIGNED",-12.34,"-12.34"}, {"BCD_SIGNED",80,NULL}
    };
    for (size_t c = 0; c < sizeof(meter) / sizeof(meter[0]); ++c) {
        items[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
        items[2].which_item = iot_edge_v1_ConfigItem_industrial_point_tag;
        iot_edge_v1_IndustrialPointConfig *p = &items[2].item.industrial_point;
        p->device_id.size = 16; memcpy(p->device_id.bytes, device, 16);
        strcpy(p->element_id, "scaled"); strcpy(p->data_type, meter[c].type);
        p->writable = true; p->scale = 0.1; p->length = 2; p->digits = 2;
        edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, &source, 1);
        CHECK(gateway);
        uint64_t bits; memcpy(&bits, &meter[c].number, sizeof(bits));
        uint8_t data[8], request[260];
        for (unsigned b = 0; b < 8; ++b) data[b] = (uint8_t)(bits >> ((7 - b) * 8));
        const size_t size = write_adu(request, 16, 0, 4, data);
        check_write_reply(gateway, request, size, meter[c].text ? 0 : 3);
        if (meter[c].text) {
            uint32_t cursor = 0;
            edge_modbus_gateway_write write;
            CHECK(edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write) == 1 &&
                  !strcmp(write.value, meter[c].text));
        }
        edge_modbus_gateway_free(gateway);
    }
    remove(path);
}

static void test_unit_id(void) {
    const char *path = "gateway-unit-id.map";
    remove(path);
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, NULL, 0);
    CHECK(gateway);
    uint8_t request[12] = {0, 42, 0, 0, 0, 6, 1, 3, 0, 0, 0, 1}, response[260];
    CHECK(edge_modbus_gateway_reply(gateway, request, sizeof(request), response, 0) == 11);
    CHECK(edge_modbus_gateway_set_unit_id(gateway, 247));
    CHECK(!edge_modbus_gateway_set_unit_id(gateway, 248));
    CHECK(!edge_modbus_gateway_set_unit_id(gateway, 256));
    CHECK(!edge_modbus_gateway_set_unit_id(gateway, UINT32_MAX));
    for (unsigned function = 1; function <= 16; ++function) {
        request[7] = (uint8_t)function;
        request[6] = 1;
        CHECK(!edge_modbus_gateway_reply(gateway, request, sizeof(request), response, 0));
        request[6] = 247;
        CHECK(edge_modbus_gateway_reply(gateway, request, sizeof(request), response, 0) > 0);
        CHECK(response[6] == 247 && response[1] == 42);
    }
    uint32_t cursor = 0;
    edge_modbus_gateway_write write;
    request[6] = 1; request[7] = 6;
    CHECK(edge_modbus_gateway_next_write(gateway, request, sizeof(request), &cursor, &write) == -4);
    CHECK(edge_modbus_gateway_set_unit_id(gateway, 0));
    request[7] = 3;
    CHECK(edge_modbus_gateway_reply(gateway, request, sizeof(request), response, 0) == 11);
    edge_modbus_gateway_free(gateway);
    remove(path);
}

static void test_derived_lifecycle(void) {
    const char *path = "gateway-derived-test.map";
    remove(path);
    uint8_t platform[16] = {1}, device[16] = {2};
    iot_edge_v1_ConfigItem items[6] = {0};
    iot_edge_v1_DerivedPointInput inputs[2] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    memcpy(items[1].item.device.device_id.bytes, device, 16);
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    items[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    items[2].item.modbus_register.device_id.size = 16;
    memcpy(items[2].item.modbus_register.device_id.bytes, device, 16);
    strcpy(items[2].item.modbus_register.element_id, "physical");
    strcpy(items[2].item.modbus_register.data_type, "UINT16");
    items[2].item.modbus_register.scale = 1;
    items[2].item.modbus_register.writable = true;
    for (unsigned i = 3; i < 5; ++i) {
        items[i].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
        iot_edge_v1_DerivedPointConfig *rule = &items[i].item.derived_point;
        rule->device_id.size = 16;
        memcpy(rule->device_id.bytes, device, 16);
        strcpy(rule->point_id, i == 3 ? "derived-number" : "derived-bool");
        strcpy(rule->name, i == 3 ? "派生电压" : "派生状态");
        strcpy(rule->unit, i == 3 ? "V" : "");
        strcpy(rule->kind, "expression");
        strcpy(rule->expression, i == 3 ? "x * 2" : "x > 0");
        rule->boolean_result = i == 4;
        rule->max_age_seconds = 60;
        rule->inputs_count = 1;
        rule->inputs = &inputs[i - 3];
        strcpy(rule->inputs[0].alias, "x");
        strcpy(rule->inputs[0].point_id, "physical");
    }
    items[5].which_item = iot_edge_v1_ConfigItem_external_io_tag;
    items[5].item.external_io.device_id.size = 16;
    memcpy(items[5].item.external_io.device_id.bytes, device, 16);
    strcpy(items[5].item.external_io.point_id, "analog");
    items[5].item.external_io.kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI;
    edge_runtime_config config = {.items = items, .item_count = 6};
    edge_acquisition_source source = {.platform_id = platform, .config = &config};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, &source, 1);
    CHECK(gateway);
    iot_edge_v1_ModbusGatewayRequest request = {.limit = 8};
    iot_edge_v1_ModbusGatewayResult result = iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1));
    CHECK(result.points_count == 4 && result.points[0].value_address == 0 &&
          result.points[1].has_mapping && result.points[1].value_address == 1 &&
          result.points[1].value_words == 4 && result.points[1].value_type == 5 &&
          !result.points[1].writable && result.points[1].quality == 0 &&
          !strcmp(result.points[1].name, "派生电压") && !strcmp(result.points[1].unit, "V") &&
          result.points[2].has_mapping && result.points[2].value_address == 0 &&
          result.points[2].value_words == 1 && result.points[2].value_type == 2 &&
          !strcmp(result.points[2].register_type, "DISCRETE_INPUT") &&
          result.points[3].value_address == 5 && result.points[3].value_words == 4);
    uint8_t response[260];
    CHECK(read_native(gateway, 3, 0, 9, response) == 27);
    for (unsigned i = 9; i < 27; ++i) CHECK(response[i] == 0);
    edge_acquisition_sample sample = {.valid = true, .observed_at_ms = 1000};
    memcpy(sample.device_id, device, 16);
    strcpy(sample.element_id, "derived-number");
    sample.value.which_value = iot_edge_v1_ScalarValue_double_value_tag;
    sample.value.value.double_value = 12.5;
    edge_modbus_gateway_update(gateway, platform, &sample, 1);
    iot_edge_v1_ModbusGatewayOrder order = iot_edge_v1_ModbusGatewayOrder_init_zero;
    order.expected_digest.size = 32;
    order.addresses.size = 4;
    order.addresses.bytes[1] = 10;
    CHECK(edge_modbus_gateway_digest(gateway, platform, order.expected_digest.bytes));
    CHECK(edge_modbus_gateway_reorder(gateway, path, platform, &order));
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "derived-number", &point, 1) &&
          point.value_address == 0 && !strcmp(point.value, "12.5"));
    edge_modbus_gateway_update(gateway, (const uint8_t[16]){9}, NULL, 2);
    CHECK(read_native(gateway, 3, 0, 4, response) == 17 && word(response + 9) == 0x4029);
    edge_modbus_gateway_update(gateway, platform, NULL, 2);
    CHECK(read_native(gateway, 3, 0, 4, response) == 17);
    for (unsigned i = 9; i < 17; ++i) CHECK(response[i] == 0);
    edge_modbus_gateway_free(gateway);
    strcpy(items[3].item.derived_point.name, "新名称");
    strcpy(items[3].item.derived_point.unit, "mV");
    gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1));
    CHECK(result.points[1].value_address == 0 && result.points[1].quality == 0 &&
          !result.points[1].value[0] && !strcmp(result.points[1].name, "新名称") &&
          !strcmp(result.points[1].unit, "mV"));
    CHECK(read_native(gateway, 3, 0, 4, response) == 17 && !word(response + 9));
    edge_modbus_gateway_free(gateway);
    items[1].item.device.enabled = false;
    gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1));
    CHECK(!result.points[1].has_mapping && !strcmp(result.points[1].state, "disabled") &&
          !result.points[2].has_mapping);
    edge_modbus_gateway_free(gateway);
    items[1].item.device.enabled = true;
    items[3].which_item = items[4].which_item = 0;
    gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "analog", &point, 1) &&
          point.value_address == 1);
    CHECK(read_native(gateway, 2, 0, 8, response) == 10 && !response[9]);
    edge_modbus_gateway_free(gateway);
    items[3].which_item = items[4].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
    gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "derived-number", &point, 1) &&
          point.address == 0 && point.value_address == 0 && point.quality == 0);
    edge_modbus_gateway_free(gateway);
    /* Every typed derived output reaches the real register encoder, not display-only formatting. */
    const struct {
        const char *type, *text;
        unsigned words, value_type, scalar_tag;
        uint64_t bits;
    } typed[] = {
        {"INT8", "-128", 1, 6, iot_edge_v1_ScalarValue_signed_value_tag, UINT64_C(0xff80)},
        {"UINT8", "255", 1, 7, iot_edge_v1_ScalarValue_unsigned_value_tag, UINT64_C(255)},
        {"INT16", "-2", 1, 6, iot_edge_v1_ScalarValue_signed_value_tag, UINT64_C(0xfffe)},
        {"UINT16", "65535", 1, 7, iot_edge_v1_ScalarValue_unsigned_value_tag, UINT64_C(0xffff)},
        {"INT32", "-2147483648", 2, 8, iot_edge_v1_ScalarValue_signed_value_tag, UINT64_C(0x80000000)},
        {"UINT32", "4294967295", 2, 9, iot_edge_v1_ScalarValue_unsigned_value_tag, UINT64_C(0xffffffff)},
        {"INT64", "-9223372036854775808", 4, 3, iot_edge_v1_ScalarValue_signed_value_tag, UINT64_C(0x8000000000000000)},
        {"UINT64", "18446744073709549568", 4, 4, iot_edge_v1_ScalarValue_unsigned_value_tag, UINT64_C(0xfffffffffffff800)},
        {"FLOAT32", "16777216", 2, 10, iot_edge_v1_ScalarValue_double_value_tag, UINT64_C(0x4b800000)},
        {"FLOAT64", "12.5", 4, 5, iot_edge_v1_ScalarValue_double_value_tag, UINT64_C(0x4029000000000000)}
    };
    for (unsigned i = 0; i < sizeof(typed) / sizeof(typed[0]); ++i) {
        strcpy(items[3].item.derived_point.data_type, typed[i].type);
        gateway = edge_modbus_gateway_prepare(path, &source, 1); CHECK(gateway);
        result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
        CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1));
        CHECK(!strcmp(result.points[1].source_type, typed[i].type) &&
              result.points[1].value_type == typed[i].value_type &&
              result.points[1].value_words == typed[i].words && !result.points[1].writable);
        sample.valid = true;
        sample.value.which_value = typed[i].scalar_tag;
        if (typed[i].scalar_tag == iot_edge_v1_ScalarValue_signed_value_tag)
            sample.value.value.signed_value = strtoll(typed[i].text, NULL, 10);
        else if (typed[i].scalar_tag == iot_edge_v1_ScalarValue_unsigned_value_tag)
            sample.value.value.unsigned_value = strtoull(typed[i].text, NULL, 10);
        else sample.value.value.double_value = strtod(typed[i].text, NULL);
        edge_modbus_gateway_update(gateway, platform, &sample, 1);
        CHECK(edge_modbus_gateway_describe(gateway, platform, device, "derived-number", &point, 1) &&
              point.quality == 1 && !strcmp(point.value, typed[i].text));
        CHECK(read_native(gateway, 3, point.value_address, typed[i].words, response) == 9 + typed[i].words * 2);
        for (unsigned j = 0; j < typed[i].words; ++j)
            CHECK(word(response + 9 + j * 2) == (uint16_t)(typed[i].bits >> ((typed[i].words - j - 1) * 16)));
        uint8_t write_request[260], zeros[8] = {0};
        const size_t write_size = write_adu(write_request, 16, point.value_address, typed[i].words, zeros);
        check_write_reply(gateway, write_request, write_size, 0);
        CHECK(edge_modbus_gateway_describe(gateway, platform, device, "derived-number", &point, 1) &&
              point.quality == 1 && !strcmp(point.value, typed[i].text));
        sample.valid = false;
        edge_modbus_gateway_update(gateway, platform, &sample, 2);
        CHECK(edge_modbus_gateway_describe(gateway, platform, device, "derived-number", &point, 2) &&
              point.quality == 2 && !point.value[0]);
        CHECK(read_native(gateway, 3, point.value_address, typed[i].words, response) == 9 + typed[i].words * 2);
        for (unsigned j = 0; j < typed[i].words * 2; ++j) CHECK(response[9 + j] == 0);
        edge_modbus_gateway_free(gateway);
    }
    remove(path);
}

int main(void) {
    test_derived_lifecycle();
    test_unit_id();
    test_write_plans();
    test_scaled_write_plans();
    test_cross_platform_reorder();
    test_compact_lifecycle();
    test_native_types();
    test_drag_permutation();
    const char *path = "gateway-management-test.map";
    remove(path);
    uint8_t platform[16] = {1}, foreign[16] = {2}, device[16] = {4};
    iot_edge_v1_ConfigItem items[5] = {0};
    items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    items[0].item.endpoint.endpoint_id.size = 16;
    items[0].item.endpoint.endpoint_id.bytes[0] = 3;
    items[0].item.endpoint.enabled = true;
    items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    items[1].item.device.device_id.size = 16;
    memcpy(items[1].item.device.device_id.bytes, device, 16);
    items[1].item.device.endpoint_id.size = 16;
    items[1].item.device.endpoint_id.bytes[0] = 3;
    items[1].item.device.enabled = true;
    items[1].item.device.online_timeout_sec = 5;
    items[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(items[1].item.device.name, "pump");
    for (unsigned i = 2; i < 4; ++i) {
        items[i].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &items[i].item.modbus_register;
        point->device_id.size = 16; memcpy(point->device_id.bytes, device, 16);
        strcpy(point->element_id, i == 2 ? "temperature" : "counter");
        strcpy(point->data_type, i == 2 ? "DOUBLE" : "UINT64");
        strcpy(point->byte_order, "ABCD"); point->scale = 1;
    }
    items[4].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
    items[4].item.derived_point.device_id.size = 16;
    memcpy(items[4].item.derived_point.device_id.bytes, device, 16);
    strcpy(items[4].item.derived_point.point_id, "derived");
    edge_runtime_config config = {.items = items, .item_count = 5};
    edge_acquisition_source sources[] = {{.platform_id = platform, .config = &config},
                                        {.platform_id = foreign, .config = &config}};
    edge_modbus_gateway *gateway = edge_modbus_gateway_prepare(path, sources, 2); CHECK(gateway);
    edge_acquisition_sample sample = {.valid = true, .observed_at_ms = 1760000000123LL};
    memcpy(sample.device_id, device, 16); strcpy(sample.element_id, "counter");
    sample.value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    sample.value.value.unsigned_value = UINT64_MAX;
    edge_modbus_gateway_update(gateway, platform, &sample, 1000);
    iot_edge_v1_ModbusGatewayRequest request = iot_edge_v1_ModbusGatewayRequest_init_zero;
    request.include_points = true; request.limit = 1; request.offset = 1;
    iot_edge_v1_ModbusGatewayResult result = iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1000));
    CHECK(result.total == 3 && result.points_count == 1 && result.offset == 1);
    CHECK(result.points[0].address == 10 && result.points[0].value_address == 4 &&
          result.points[0].value_words == 4 && result.points[0].has_mapping &&
          !strcmp(result.points[0].value, "18446744073709551615") &&
          !strcmp(result.points[0].device_name, "pump") &&
          !strcmp(result.points[0].source_protocol, "Modbus") && !result.points[0].byte_order[0]);
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, foreign, &config, true, true, &request, &result, 1000));
    CHECK(result.points[0].address == 30 && result.points[0].value_address == 12 &&
          !result.points[0].value[0]);
    request.limit = 8; request.offset = 0;
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, false, false, &request, &result, 1000));
    CHECK(result.total == 3 && result.points_count == 3 &&
          !strcmp(result.points[0].state, "gateway_disabled") &&
          !result.points[2].has_mapping && !strcmp(result.points[2].state, "unsupported"));
    items[1].item.device.enabled = false;
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1000));
    CHECK(!result.points[0].has_mapping && !result.points[0].value_words &&
          !result.points[0].register_type[0] && !strcmp(result.points[0].state, "disabled"));
    items[1].item.device.enabled = true;
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, false, &request, &result, 1000));
    CHECK(!strcmp(result.points[0].state, "listener_error"));
    request.offset = 3;
    result = (iot_edge_v1_ModbusGatewayResult)iot_edge_v1_ModbusGatewayResult_init_zero;
    CHECK(edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1000));
    CHECK(result.total == 3 && !result.points_count);
    request.offset = 0;
    request.limit = 9;
    CHECK(!edge_modbus_gateway_points(gateway, platform, &config, true, true, &request, &result, 1000));
    iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "counter", &point, 6000) &&
          point.quality == 3 && point.observed_at_ms == sample.observed_at_ms);
    uint8_t response[260], same[260];
    CHECK(read_native(gateway, 3, 4, 4, response) == 17);
    CHECK(read_native(gateway, 4, 4, 4, same) == 17 && !memcmp(response+8, same+8, 9));
    edge_modbus_gateway_update(gateway, platform, NULL, 6001);
    CHECK(edge_modbus_gateway_describe(gateway, platform, device, "counter", &point, 6001) &&
          point.quality == 2 && point.value_address == 4 && point.value_words == 4);
    uint8_t invalid[12] = {0,1,0,0,0,6,1,0x7f,0,0,0,1};
    CHECK(edge_modbus_gateway_reply(gateway, invalid, sizeof(invalid), response, 0) == 9 && response[8] == 1);
    invalid[2] = 1;
    CHECK(!edge_modbus_gateway_reply(gateway, invalid, sizeof(invalid), response, 0));
    invalid[2] = 0; invalid[6] = 0;
    CHECK(!edge_modbus_gateway_reply(gateway, invalid, sizeof(invalid), response, 0));
    FILE *file = fopen(path, "ab"); CHECK(file);
    CHECK(fputs("corrupt\n", file) >= 0 && fclose(file) == 0);
    CHECK(!edge_modbus_gateway_prepare(path, sources, 2));
    CHECK(!edge_modbus_gateway_prepare("no-such-directory/map", sources, 2));
    CHECK(read_native(gateway, 3, 4, 4, response) == 17 && word(response+9) == 0xffff);
    edge_modbus_gateway_free(gateway); remove(path);
    puts("Modbus gateway checks passed");
    return 0;
}
