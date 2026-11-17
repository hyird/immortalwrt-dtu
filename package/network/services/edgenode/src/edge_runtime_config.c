#include "edge_runtime_config.h"
#include "edge_industrial.h"
#include "edge_dtu.h"
#include "edge_derived.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>

#include "edge_util.h"

#define set_error edge_set_error

static bool same_id(const void *field, const uint8_t id[16]) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return size == 16U && memcmp((const uint8_t *)field + sizeof(size), id, 16U) == 0;
}

static const uint8_t *id_bytes(const void *field) {
    return (const uint8_t *)field + sizeof(pb_size_t);
}

void edge_runtime_config_free(edge_runtime_config *config) {
    if (config == NULL)
        return;
    for (uint32_t i = 0; i < config->item_count; ++i) pb_release(iot_edge_v1_ConfigItem_fields, &config->items[i]);
    free(config->items);
    memset(config, 0, sizeof(*config));
}

const iot_edge_v1_EndpointConfig *
edge_runtime_config_endpoint(const edge_runtime_config *config, const uint8_t id[16]) {
    if (config == NULL || id == NULL)
        return NULL;
    for (uint32_t index = 0; index < config->item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &config->items[index];
        if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag &&
            same_id(&item->item.endpoint.endpoint_id, id))
            return &item->item.endpoint;
    }
    return NULL;
}

const iot_edge_v1_DeviceConfig *
edge_runtime_config_device(const edge_runtime_config *config, const uint8_t id[16]) {
    if (config == NULL || id == NULL)
        return NULL;
    for (uint32_t index = 0; index < config->item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &config->items[index];
        if (item->which_item == iot_edge_v1_ConfigItem_device_tag &&
            same_id(&item->item.device.device_id, id))
            return &item->item.device;
    }
    return NULL;
}

static bool valid_endpoint(const iot_edge_v1_EndpointConfig *value) {
    if (value->endpoint_id.size != 16U || value->protocol ==
                                                    iot_edge_v1_Protocol_PROTOCOL_UNSPECIFIED)
        return false;
    if (value->protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO ||
        value->transport == iot_edge_v1_Transport_TRANSPORT_EXTERNAL_IO)
        return value->protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO &&
               value->transport == iot_edge_v1_Transport_TRANSPORT_EXTERNAL_IO &&
               value->mode == iot_edge_v1_LinkMode_LINK_MODE_UNSPECIFIED &&
               !value->has_serial && !value->interface_name[0] && !value->ip[0] && !value->port;
    if (value->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        return value->mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL && value->has_serial &&
               value->serial.channel[0] != '\0' && value->serial.baud_rate >= 300U &&
               value->serial.data_bits >= 5U && value->serial.data_bits <= 8U &&
               value->serial.stop_bits >= 1U && value->serial.stop_bits <= 2U;
    if (value->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET)
        return (value->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT ||
                value->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER) &&
               value->ip[0] != '\0' && value->port != 0U && value->port <= 65535U;
    return false;
}

static bool valid_dtu(const iot_edge_v1_DtuConfig *value) {
    if (value->channel_id.size != 16U || !value->name[0] || !value->north_host[0] ||
        !value->north_port || value->north_port > 65535U ||
        value->max_clients < 1U || value->max_clients > EDGE_DTU_MAX_CLIENTS ||
        value->queue_bytes < 4096U || value->queue_bytes > 65536U ||
        value->serial_frame_ms > 1000U || value->heartbeat_interval_sec > 86400U ||
        (value->heartbeat_interval_sec && !value->heartbeat.size)) return false;
    if (value->south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL) {
        const iot_edge_v1_SerialSettings *s = &value->serial;
        const bool baud = edge_serial_baud_speed(s->baud_rate) != 0;
        return value->has_serial && !strncmp(s->channel, "/dev/", 5) && baud &&
            s->data_bits >= 5U && s->data_bits <= 8U && s->stop_bits >= 1U && s->stop_bits <= 2U &&
            (!strcmp(s->parity, "none") || !strcmp(s->parity, "even") || !strcmp(s->parity, "odd")) &&
            s->rs485_rts_delay_before_us <= 1000000U && s->rs485_rts_delay_after_us <= 1000000U;
    }
    return (value->south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT ||
            value->south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER) &&
        value->south_host[0] && value->south_port && value->south_port <= 65535U;
}

static bool valid_device(const edge_runtime_config *config,
                         const iot_edge_v1_DeviceConfig *value) {
    if (value->device_id.size != 16U || value->endpoint_id.size != 16U ||
        value->device_code[0] == '\0' || value->report_interval_sec == 0U ||
        value->command_fast_read_duration_sec > 3600U ||
        value->command_fast_read_interval_sec > 3600U ||
        (value->command_fast_read_duration_sec != 0U &&
         value->command_fast_read_interval_sec == 0U) ||
        (value->io_interval_ms != 0U &&
         (value->io_interval_ms < 1000U || value->io_interval_ms > 3600000U)) ||
        (value->protocol != iot_edge_v1_Protocol_PROTOCOL_MODBUS &&
         value->protocol != iot_edge_v1_Protocol_PROTOCOL_S7 &&
         value->protocol != iot_edge_v1_Protocol_PROTOCOL_SL651 &&
         value->protocol != iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO && !edge_industrial_protocol(value->protocol)) || value->sl651_response_mode > 4)
        return false;
    const iot_edge_v1_EndpointConfig *endpoint =
        edge_runtime_config_endpoint(config, value->endpoint_id.bytes);
    if (edge_industrial_protocol(value->protocol) &&
        (!edge_industrial_valid_device(value) || endpoint == NULL ||
         (value->protocol != iot_edge_v1_Protocol_PROTOCOL_DLT645 &&
          endpoint->transport != iot_edge_v1_Transport_TRANSPORT_ETHERNET))) return false;
    return endpoint != NULL && endpoint->protocol == value->protocol;
}

static bool valid_sl651_function(const char *code) {
    if (strlen(code) != 2) return false;
    for (size_t i = 0; i < 2; ++i)
        if (!((code[i] >= '0' && code[i] <= '9') || (code[i] >= 'A' && code[i] <= 'F')))
            return false;
    return true;
}

static bool valid_sl651_element(const iot_edge_v1_Sl651ElementConfig *point) {
    if (!point->element_id[0] || !valid_sl651_function(point->function_code) ||
        point->digits > 7 || point->length > 65536 ||
        (strcmp(point->encoding, "BCD") && strcmp(point->encoding, "HEX") && strcmp(point->encoding, "DICT") &&
         strcmp(point->encoding, "JPEG") && strcmp(point->encoding, "TIME_YYMMDDHHMMSS")))
        return false;
    if (point->fixed_position)
        return point->length && point->byte_offset <= 65536U - point->length &&
               (!point->writable || point->byte_offset >= 8);
    size_t n = point->guide.size;
    const uint8_t *guide = point->guide.bytes;
    if ((n != 2 && n != 3) || (guide[0] == 0xFF ? n != 3 : n != 2)) return false;
    if (guide[0] >= 0xF0 && guide[0] != 0xFF) {
        if (guide[1] != guide[0]) return false;
        if (guide[0] == 0xF0) return point->length == 5;
        if (guide[0] == 0xF1) return point->length == 6;
        if (guide[0] == 0xF2 || guide[0] == 0xF3) return true;
        return point->length > 0;
    }
    return point->length && point->length == (uint32_t)(guide[n - 1] >> 3U) &&
           point->digits == (guide[n - 1] & 7U);
}

static bool valid_point(const edge_runtime_config *config,
                        const iot_edge_v1_ConfigItem *item) {
    const void *device_id = NULL;
    iot_edge_v1_Protocol expected = iot_edge_v1_Protocol_PROTOCOL_UNSPECIFIED;
    switch (item->which_item) {
    case iot_edge_v1_ConfigItem_modbus_register_tag: {
        const iot_edge_v1_ModbusRegisterConfig *point = &item->item.modbus_register;
        const bool bit = strcmp(point->register_type, "COIL") == 0 ||
                         strcmp(point->register_type, "DISCRETE_INPUT") == 0;
        const bool word = strcmp(point->register_type, "HOLDING_REGISTER") == 0 ||
                          strcmp(point->register_type, "INPUT_REGISTER") == 0;
        const bool order = strcmp(point->byte_order, "BIG_ENDIAN") == 0 ||
                           strcmp(point->byte_order, "LITTLE_ENDIAN") == 0 ||
                           strcmp(point->byte_order, "BIG_ENDIAN_BYTE_SWAP") == 0 ||
                           strcmp(point->byte_order, "LITTLE_ENDIAN_BYTE_SWAP") == 0;
        if (point->element_id[0] == '\0' || (!bit && !word) || !order ||
            point->address > 65535U || point->quantity == 0U ||
            (bit && point->quantity > 2000U) || (word && point->quantity > 125U) ||
            (bit && strcmp(point->data_type, "BOOL") != 0) ||
            (point->writable && strcmp(point->register_type, "DISCRETE_INPUT") == 0) ||
            (point->writable && strcmp(point->register_type, "INPUT_REGISTER") == 0))
            return false;
        device_id = &item->item.modbus_register.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
        break;
    }
    case iot_edge_v1_ConfigItem_industrial_point_tag: {
        const iot_edge_v1_IndustrialPointConfig *point = &item->item.industrial_point;
        if (point->device_id.size != 16) return false;
        const iot_edge_v1_DeviceConfig *device = edge_runtime_config_device(config, point->device_id.bytes);
        return device != NULL && edge_industrial_protocol(device->protocol) &&
               edge_industrial_valid_point(device, point);
    }
    case iot_edge_v1_ConfigItem_s7_area_tag: {
        const iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        if (point->element_id[0] == '\0' || point->size == 0U || point->size > 512U ||
            point->start > 0x1fffffU || point->start_bit > 7U)
            return false;
        device_id = &item->item.s7_area.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_S7;
        break;
    }
    case iot_edge_v1_ConfigItem_sl651_function_tag:
        if (!valid_sl651_function(item->item.sl651_function.function_code)) return false;
        device_id = &item->item.sl651_function.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_SL651;
        break;
    case iot_edge_v1_ConfigItem_sl651_element_tag:
        if (!valid_sl651_element(&item->item.sl651_element)) return false;
        device_id = &item->item.sl651_element.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_SL651;
        break;
    case iot_edge_v1_ConfigItem_sl651_dictionary_tag:
        device_id = &item->item.sl651_dictionary.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_SL651;
        break;
    default:
        return false;
    }
    pb_size_t size = 0U;
    memcpy(&size, device_id, sizeof(size));
    if (size != 16U)
        return false;
    const iot_edge_v1_DeviceConfig *device =
        edge_runtime_config_device(config, id_bytes(device_id));
    return device != NULL && device->protocol == expected;
}

static bool valid_external_io(const edge_runtime_config *config, uint32_t index) {
    const iot_edge_v1_ConfigItem *item = &config->items[index];
    const iot_edge_v1_ExternalIoPointConfig *point = &item->item.external_io;
    if (item->kind != iot_edge_v1_ConfigItemKind_CONFIG_ITEM_EXTERNAL_IO ||
        point->device_id.size != 16U || point->model_id.size != 16U ||
        !edge_runtime_config_device(config, point->device_id.bytes) ||
        !point->name[0] || strlen(point->point_id) != 36U)
        return false;
    for (size_t i = 0; i < 36U; ++i) {
        const char ch = point->point_id[i];
        if (i == 8U || i == 13U || i == 18U || i == 23U) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    if (point->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DI) {
        if (strlen(point->channel) != 3U || strncmp(point->channel, "di", 2U) ||
            point->channel[2] < '0' || point->channel[2] > '3' || point->unit[0]) return false;
    } else if (point->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO) {
        if (strlen(point->channel) != 3U || strncmp(point->channel, "do", 2U) ||
            point->channel[2] < '0' || point->channel[2] > '6' || point->unit[0]) return false;
    } else if (point->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI) {
        if (strlen(point->channel) != 3U || strncmp(point->channel, "AI", 2U) ||
            point->channel[2] < '0' || point->channel[2] > '3' ||
            strcmp(point->unit, "mA")) return false;
    } else {
        return false;
    }
    for (uint32_t other_index = 0U; other_index < index; ++other_index) {
        const iot_edge_v1_ConfigItem *other = &config->items[other_index];
        if (other->which_item != iot_edge_v1_ConfigItem_external_io_tag) continue;
        const iot_edge_v1_ExternalIoPointConfig *previous = &other->item.external_io;
        if (same_id(&previous->device_id, point->device_id.bytes) &&
            (!strcmp(previous->point_id, point->point_id) || !strcmp(previous->channel, point->channel))) return false;
        if (point->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO && !strcmp(previous->channel, point->channel)) return false;
    }
    return true;
}

bool edge_runtime_config_load(edge_runtime_config *output,
                              const edge_memory_config_set *snapshot,
                              char *error, size_t error_size) {
    if (output == NULL || snapshot == NULL || snapshot->revision == 0U ||
        snapshot->received_count != snapshot->item_count) {
        set_error(error, error_size, "configuration snapshot is incomplete");
        return false;
    }
    edge_runtime_config candidate = {0};
    candidate.revision = snapshot->revision;
    memcpy(candidate.digest, snapshot->digest, sizeof(candidate.digest));
    candidate.item_count = snapshot->item_count;
    if (candidate.item_count != 0U) {
        candidate.items = calloc(candidate.item_count, sizeof(*candidate.items));
        if (candidate.items == NULL) {
            set_error(error, error_size, "configuration memory allocation failed");
            return false;
        }
    }
    for (uint32_t index = 0; index < candidate.item_count; ++index) {
        const edge_memory_config_item *source = &snapshot->items[index];
        pb_istream_t stream = pb_istream_from_buffer(source->payload, source->payload_size);
        candidate.items[index] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
        if (!source->present ||
            !pb_decode(&stream, iot_edge_v1_ConfigItem_fields, &candidate.items[index])) {
            edge_runtime_config_free(&candidate);
            set_error(error, error_size, "configuration item decode failed");
            return false;
        }
        const iot_edge_v1_ConfigItem *item = &candidate.items[index];
        if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag)
            ++candidate.endpoint_count;
        else if (item->which_item == iot_edge_v1_ConfigItem_device_tag)
            ++candidate.device_count;
    }
    for (uint32_t index = 0; index < candidate.item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &candidate.items[index];
        bool valid = true;
        if (item->revision != candidate.revision || item->index != index)
            valid = false;
        else if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag)
            valid = valid_endpoint(&item->item.endpoint);
        else if (item->which_item == iot_edge_v1_ConfigItem_device_tag)
            valid = valid_device(&candidate, &item->item.device);
        else if (item->which_item == iot_edge_v1_ConfigItem_derived_point_tag)
            valid = item->kind == iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DERIVED_POINT && edge_derived_validate(&item->item.derived_point) && edge_runtime_config_device(&candidate, item->item.derived_point.device_id.bytes);
        else if (item->which_item == iot_edge_v1_ConfigItem_dtu_tag)
            valid = item->kind == iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DTU && valid_dtu(&item->item.dtu);
        else if (item->which_item == iot_edge_v1_ConfigItem_external_io_tag)
            valid = valid_external_io(&candidate, index);
        else
            valid = valid_point(&candidate, item);
        if (!valid) {
            edge_runtime_config_free(&candidate);
            set_error(error, error_size, "configuration reference or value is invalid");
            return false;
        }
    }
    if (!edge_derived_validate_config(&candidate)) {
        edge_runtime_config_free(&candidate);
        set_error(error, error_size, "derived point dependency is invalid or unordered");
        return false;
    }
    edge_runtime_config_free(output);
    *output = candidate;
    return true;
}
