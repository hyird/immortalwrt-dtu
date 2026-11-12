#include "edge_modbus_gateway.h"

#include <errno.h>
#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

_Static_assert(sizeof(double) == sizeof(uint64_t), "gateway requires 64-bit double");

typedef struct {
    uint8_t platform[16], device[16];
    char element[65];
    bool active;
    uint32_t timeout_sec;
    uint16_t type, quality;
    uint64_t bits, observed_at_ms, received_at_ms;
} edge_gateway_point;

struct edge_modbus_gateway {
    size_t count;
    size_t capacity;
    edge_gateway_point *points;
};

static bool reserve_point(edge_modbus_gateway *gateway) {
    if (gateway->count < gateway->capacity)
        return true;
    const size_t capacity = gateway->capacity + 32 > EDGE_GATEWAY_MAX_POINTS
                                ? EDGE_GATEWAY_MAX_POINTS
                                : gateway->capacity + 32;
    if (capacity <= gateway->count)
        return false;
    void *points = realloc(gateway->points, capacity * sizeof(*gateway->points));
    if (!points)
        return false;
    gateway->points = points;
    gateway->capacity = capacity;
    return true;
}

static bool same_point(const edge_gateway_point *a, const edge_gateway_point *b) {
    return !memcmp(a->platform, b->platform, 16) && !memcmp(a->device, b->device, 16) &&
           !strcmp(a->element, b->element);
}

static void hex_write(FILE *file, const void *bytes, size_t count) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < count; ++i)
        fprintf(file, "%02x", p[i]);
}

static bool hex_read(const char *text, void *bytes, size_t count) {
    unsigned char *p = bytes;
    for (size_t i = 0; i < count; ++i) {
        const char *digits = "0123456789abcdef";
        const char *a = strchr(digits, text[i * 2]);
        const char *b = strchr(digits, text[i * 2 + 1]);
        if (!text[i * 2] || !text[i * 2 + 1] || !a || !b)
            return false;
        p[i] = (unsigned char)(((a - digits) << 4) | (b - digits));
    }
    return true;
}

static bool load_map(edge_modbus_gateway *gateway, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return errno == ENOENT;
    char line[240];
    bool ok = fgets(line, sizeof(line), file) && !strcmp(line, "EDGEMB1\n");
    while (ok && fgets(line, sizeof(line), file)) {
        edge_gateway_point point = {0};
        const size_t size = strlen(line);
        if (gateway->count == EDGE_GATEWAY_MAX_POINTS || size < 69 || size > 195 ||
            line[32] != ' ' || line[65] != ' ' || line[size - 1] != '\n' || (size - 67) % 2) {
            ok = false;
            break;
        }
        const size_t length = (size - 67) / 2;
        ok = length && length <= 64 && hex_read(line, point.platform, 16) &&
             hex_read(line + 33, point.device, 16) && hex_read(line + 66, point.element, length) &&
             !memchr(point.element, 0, length);
        for (size_t i = 0; ok && i < gateway->count; ++i)
            if (same_point(&point, &gateway->points[i]))
                ok = false;
        if (ok)
            ok = reserve_point(gateway);
        if (ok)
            gateway->points[gateway->count++] = point;
    }
    if (ferror(file))
        ok = false;
    fclose(file);
    return ok;
}

static bool save_map(const edge_modbus_gateway *gateway, const char *path) {
    char temporary[512];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
        return false;
    FILE *file = fopen(temporary, "wb");
    if (!file)
        return false;
    fputs("EDGEMB1\n", file);
    for (size_t i = 0; i < gateway->count; ++i) {
        const edge_gateway_point *point = &gateway->points[i];
        hex_write(file, point->platform, 16);
        fputc(' ', file);
        hex_write(file, point->device, 16);
        fputc(' ', file);
        hex_write(file, point->element, strlen(point->element));
        fputc('\n', file);
    }
    bool ok = !ferror(file) && fflush(file) == 0;
#ifdef _WIN32
    if (ok)
        ok = _commit(_fileno(file)) == 0;
#else
    if (ok)
        ok = fsync(fileno(file)) == 0;
#endif
    if (fclose(file))
        ok = false;
#ifdef _WIN32
    if (ok)
        ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok)
        ok = rename(temporary, path) == 0;
    if (ok) {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash) {
            if (slash == parent)
                slash[1] = 0;
            else
                *slash = 0;
        } else
            strcpy(parent, ".");
        const int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ok = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0)
            close(fd);
    }
#endif
    if (!ok)
        remove(temporary);
    return ok;
}

static bool add_point(edge_modbus_gateway *gateway, const uint8_t platform[16],
                      const edge_runtime_config *config, const uint8_t device[16],
                      const char *element) {
    const iot_edge_v1_DeviceConfig *owner = edge_runtime_config_device(config, device);
    if (!owner || !owner->enabled)
        return true;
    const iot_edge_v1_EndpointConfig *endpoint =
        edge_runtime_config_endpoint(config, owner->endpoint_id.bytes);
    if (!endpoint || !endpoint->enabled)
        return true;
    edge_gateway_point key = {0};
    memcpy(key.platform, platform, 16);
    memcpy(key.device, device, 16);
    if (!element[0] || strlen(element) > 64)
        return false;
    strcpy(key.element, element);
    size_t slot = 0;
    while (slot < gateway->count && !same_point(&key, &gateway->points[slot]))
        ++slot;
    if (slot == gateway->count) {
        if (!reserve_point(gateway))
            return false;
        gateway->points[gateway->count++] = key;
    }
    gateway->points[slot].active = true;
    gateway->points[slot].timeout_sec = owner->online_timeout_sec ? owner->online_timeout_sec : 900;
    return true;
}

edge_modbus_gateway *edge_modbus_gateway_load(const char *path) {
    edge_modbus_gateway *gateway = calloc(1, sizeof(*gateway));
    if (!gateway)
        return NULL;
    if (!load_map(gateway, path)) {
        edge_modbus_gateway_free(gateway);
        return NULL;
    }
    return gateway;
}

edge_modbus_gateway *edge_modbus_gateway_prepare(const char *path,
                                                 const edge_acquisition_source *sources,
                                                 size_t count) {
    edge_modbus_gateway *gateway = edge_modbus_gateway_load(path);
    if (!gateway)
        return NULL;
    const size_t original_count = gateway->count;
    for (size_t s = 0; s < count; ++s) {
        const edge_runtime_config *config = sources[s].config;
        if (!config)
            continue;
        for (size_t i = 0; i < config->item_count; ++i) {
            const iot_edge_v1_ConfigItem *item = &config->items[i];
            const uint8_t *device = NULL;
            const char *element = NULL;
            switch (item->which_item) {
            case iot_edge_v1_ConfigItem_modbus_register_tag:
                device = item->item.modbus_register.device_id.bytes;
                element = item->item.modbus_register.element_id;
                break;
            case iot_edge_v1_ConfigItem_s7_area_tag:
                device = item->item.s7_area.device_id.bytes;
                element = item->item.s7_area.element_id;
                break;
            case iot_edge_v1_ConfigItem_industrial_point_tag:
                device = item->item.industrial_point.device_id.bytes;
                element = item->item.industrial_point.element_id;
                break;
            case iot_edge_v1_ConfigItem_sl651_element_tag:
                device = item->item.sl651_element.device_id.bytes;
                element = item->item.sl651_element.element_id;
                break;
            case iot_edge_v1_ConfigItem_external_io_tag:
                device = item->item.external_io.device_id.bytes;
                element = item->item.external_io.point_id;
                break;
            default:
                break;
            }
            if (device && !add_point(gateway, sources[s].platform_id, config, device, element))
                goto fail;
        }
    }
    if (gateway->count != original_count && !save_map(gateway, path))
        goto fail;
    return gateway;
fail:
    edge_modbus_gateway_free(gateway);
    return NULL;
}

void edge_modbus_gateway_free(edge_modbus_gateway *gateway) {
    if (gateway) {
        free(gateway->points);
        free(gateway);
    }
}

bool edge_modbus_gateway_export(const edge_modbus_gateway *gateway, const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    fputs("platform_id,device_id,element_id,address,words\n", file);
    for (size_t i = 0; i < gateway->count; ++i) {
        const edge_gateway_point *point = &gateway->points[i];
        if (!point->active)
            continue;
        hex_write(file, point->platform, 16);
        fputc(',', file);
        hex_write(file, point->device, 16);
        fputs(",\"", file);
        for (const char *p = point->element; *p; ++p) {
            if (*p == '"')
                fputc('"', file);
            fputc(*p, file);
        }
        fprintf(file, "\",%u,%u\n", (unsigned)(i * EDGE_GATEWAY_SLOT_WORDS),
                EDGE_GATEWAY_SLOT_WORDS);
    }
    const bool ok = !ferror(file);
    return fclose(file) == 0 && ok;
}

void edge_modbus_gateway_update(edge_modbus_gateway *gateway, const uint8_t platform[16],
                                const edge_acquisition_sample *sample, uint64_t now_ms) {
    if (!gateway)
        return;
    if (!sample) {
        for (size_t i = 0; i < gateway->count; ++i)
            if (!memcmp(gateway->points[i].platform, platform, 16))
                gateway->points[i].quality = 2;
        return;
    }
    for (size_t i = 0; i < gateway->count; ++i) {
        edge_gateway_point *point = &gateway->points[i];
        if (!point->active || memcmp(point->platform, platform, 16) ||
            memcmp(point->device, sample->device_id, 16) ||
            strcmp(point->element, sample->element_id))
            continue;
        point->quality = 2;
        point->type = 0;
        point->bits = 0;
        point->received_at_ms = now_ms;
        point->observed_at_ms = sample->observed_at_ms > 0 ? (uint64_t)sample->observed_at_ms : 0;
        if (!sample->valid)
            return;
        const iot_edge_v1_ScalarValue *value = &sample->value;
        point->type = value->which_value;
        switch (value->which_value) {
        case iot_edge_v1_ScalarValue_bool_value_tag:
            point->bits = value->value.bool_value ? 1 : 0;
            break;
        case iot_edge_v1_ScalarValue_signed_value_tag:
            point->bits = (uint64_t)value->value.signed_value;
            break;
        case iot_edge_v1_ScalarValue_unsigned_value_tag:
            point->bits = value->value.unsigned_value;
            break;
        case iot_edge_v1_ScalarValue_double_value_tag:
            if (!isfinite(value->value.double_value))
                return;
            memcpy(&point->bits, &value->value.double_value, sizeof(point->bits));
            break;
        case iot_edge_v1_ScalarValue_decimal_value_tag: {
            point->type = iot_edge_v1_ScalarValue_double_value_tag;
            char *end = NULL;
            errno = 0;
            const double number = strtod(value->value.decimal_value, &end);
            if (errno || end == value->value.decimal_value || *end || !isfinite(number))
                return;
            memcpy(&point->bits, &number, sizeof(point->bits));
            break;
        }
        default:
            point->type = 0;
            return;
        }
        point->quality = 1;
        return;
    }
}

static uint16_t point_quality(const edge_gateway_point *point, uint64_t now_ms) {
    return point->quality == 1 && now_ms >= point->received_at_ms &&
                   now_ms - point->received_at_ms >= (uint64_t)point->timeout_sec * 1000
               ? 3
               : point->quality;
}

bool edge_modbus_gateway_describe(const edge_modbus_gateway *gateway, const uint8_t platform[16],
                                  const uint8_t device[16], const char *element,
                                  iot_edge_v1_ModbusGatewayPoint *output, uint64_t now_ms) {
    if (!gateway)
        return false;
    for (size_t i = 0; i < gateway->count; ++i) {
        const edge_gateway_point *point = &gateway->points[i];
        if (memcmp(point->platform, platform, 16) || memcmp(point->device, device, 16) ||
            strcmp(point->element, element))
            continue;
        output->has_mapping = true;
        output->address = (uint32_t)(i * EDGE_GATEWAY_SLOT_WORDS);
        output->words = EDGE_GATEWAY_SLOT_WORDS;
        output->value_type = point->type;
        output->quality = point_quality(point, now_ms);
        output->observed_at_ms = (int64_t)point->observed_at_ms;
        snprintf(output->state, sizeof(output->state), "%s", point->active ? "active" : "disabled");
        if (output->quality != 1 && output->quality != 3)
            return true;
        switch (point->type) {
        case iot_edge_v1_ScalarValue_bool_value_tag:
            snprintf(output->value, sizeof(output->value), "%s", point->bits ? "true" : "false");
            break;
        case iot_edge_v1_ScalarValue_signed_value_tag: {
            int64_t value;
            memcpy(&value, &point->bits, sizeof(value));
            snprintf(output->value, sizeof(output->value), "%" PRId64, value);
            break;
        }
        case iot_edge_v1_ScalarValue_unsigned_value_tag:
            snprintf(output->value, sizeof(output->value), "%" PRIu64, point->bits);
            break;
        case iot_edge_v1_ScalarValue_double_value_tag: {
            double value;
            memcpy(&value, &point->bits, sizeof(value));
            snprintf(output->value, sizeof(output->value), "%.17g", value);
            break;
        }
        default:
            break;
        }
        return true;
    }
    return false;
}

static void gateway_text(char *output, size_t size, const char *input) {
    if (!size)
        return;
    size_t length = strlen(input);
    if (length >= size)
        length = size - 1;
    memcpy(output, input, length);
    output[length] = 0;
}

static const char *gateway_protocol_name(iot_edge_v1_Protocol protocol) {
    switch (protocol) {
    case iot_edge_v1_Protocol_PROTOCOL_MODBUS:
        return "Modbus";
    case iot_edge_v1_Protocol_PROTOCOL_S7:
        return "S7";
    case iot_edge_v1_Protocol_PROTOCOL_MC:
        return "MC";
    case iot_edge_v1_Protocol_PROTOCOL_FINS:
        return "FINS";
    case iot_edge_v1_Protocol_PROTOCOL_DLT645:
        return "DLT645";
    case iot_edge_v1_Protocol_PROTOCOL_SL651:
        return "SL651";
    case iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO:
        return "ExternalIO";
    default:
        return "unknown";
    }
}

static void gateway_point_definition(iot_edge_v1_ModbusGatewayPoint *point, const char *element,
                                     const char *name, const char *unit, const char *type) {
    gateway_text(point->element_id, sizeof(point->element_id), element);
    gateway_text(point->name, sizeof(point->name), name);
    gateway_text(point->unit, sizeof(point->unit), unit);
    gateway_text(point->source_type, sizeof(point->source_type), type);
}

static bool gateway_point_metadata(const edge_runtime_config *config,
                                   const iot_edge_v1_ConfigItem *item,
                                   iot_edge_v1_ModbusGatewayPoint *point) {
    const uint8_t *device = NULL;
    switch (item->which_item) {
    case iot_edge_v1_ConfigItem_modbus_register_tag: {
        const iot_edge_v1_ModbusRegisterConfig *value = &item->item.modbus_register;
        device = value->device_id.bytes;
        gateway_point_definition(point, value->element_id, value->name, value->unit,
                                 value->data_type);
        gateway_text(point->byte_order, sizeof(point->byte_order), value->byte_order);
        snprintf(point->source_address, sizeof(point->source_address), "%s:%u",
                 value->register_type, value->address);
        point->writable = value->writable;
        break;
    }
    case iot_edge_v1_ConfigItem_s7_area_tag: {
        const iot_edge_v1_S7AreaConfig *value = &item->item.s7_area;
        device = value->device_id.bytes;
        gateway_point_definition(point, value->element_id, value->name, value->unit,
                                 value->data_type);
        snprintf(point->source_address, sizeof(point->source_address), "%s DB%u %u.%u", value->area,
                 value->db_number, value->start, value->start_bit);
        point->writable = value->writable;
        break;
    }
    case iot_edge_v1_ConfigItem_industrial_point_tag: {
        const iot_edge_v1_IndustrialPointConfig *value = &item->item.industrial_point;
        device = value->device_id.bytes;
        gateway_point_definition(point, value->element_id, value->name, value->unit,
                                 value->data_type);
        gateway_text(point->byte_order, sizeof(point->byte_order), value->byte_order);
        if (value->identifier[0])
            gateway_text(point->source_address, sizeof(point->source_address), value->identifier);
        else
            snprintf(point->source_address, sizeof(point->source_address), "%s:%u.%u", value->area,
                     value->address, value->bit);
        point->writable = value->writable;
        break;
    }
    case iot_edge_v1_ConfigItem_sl651_element_tag: {
        const iot_edge_v1_Sl651ElementConfig *value = &item->item.sl651_element;
        device = value->device_id.bytes;
        gateway_point_definition(point, value->element_id, value->name, value->unit,
                                 value->encoding);
        snprintf(point->source_address, sizeof(point->source_address), "FC%s %s:%u",
                 value->function_code, value->fixed_position ? "offset" : "guide-length",
                 value->fixed_position ? value->byte_offset : value->guide.size);
        point->writable = value->writable;
        break;
    }
    case iot_edge_v1_ConfigItem_external_io_tag: {
        const iot_edge_v1_ExternalIoPointConfig *value = &item->item.external_io;
        device = value->device_id.bytes;
        gateway_point_definition(
            point, value->point_id, value->name, value->unit,
            value->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI ? "FLOAT64" : "BOOL");
        gateway_text(point->source_address, sizeof(point->source_address), value->channel);
        point->writable = value->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
        break;
    }
    case iot_edge_v1_ConfigItem_derived_point_tag: {
        const iot_edge_v1_DerivedPointConfig *value = &item->item.derived_point;
        device = value->device_id.bytes;
        gateway_point_definition(point, value->point_id, value->name, value->unit,
                                 value->boolean_result ? "BOOL" : "FLOAT64");
        gateway_text(point->source_address, sizeof(point->source_address), value->kind);
        gateway_text(point->state, sizeof(point->state), "unsupported");
        break;
    }
    default:
        return false;
    }
    const char *type = point->source_type;
    if (!strcmp(type, "STRING") || !strcmp(type, "BYTES") || !strcmp(type, "JPEG") ||
        !strcmp(type, "HEX") || !strcmp(type, "DICT") || !strcmp(type, "ASCII"))
        gateway_text(point->state, sizeof(point->state), "unsupported");
    const iot_edge_v1_DeviceConfig *owner = edge_runtime_config_device(config, device);
    if (!owner)
        return false;
    point->device_id.size = 16;
    memcpy(point->device_id.bytes, device, 16);
    gateway_text(point->device_name, sizeof(point->device_name), owner->name);
    gateway_text(point->source_protocol, sizeof(point->source_protocol),
                 gateway_protocol_name(owner->protocol));
    if (!point->state[0]) {
        const iot_edge_v1_EndpointConfig *endpoint =
            edge_runtime_config_endpoint(config, owner->endpoint_id.bytes);
        gateway_text(point->state, sizeof(point->state),
                     owner->enabled && endpoint && endpoint->enabled ? "unmapped" : "disabled");
    }
    return true;
}

bool edge_modbus_gateway_points(const edge_modbus_gateway *map, const uint8_t platform[16],
                                const edge_runtime_config *config, bool enabled, bool listening,
                                const iot_edge_v1_ModbusGatewayRequest *request,
                                iot_edge_v1_ModbusGatewayResult *result, uint64_t now_ms) {
    if (!request->limit || request->limit > 8) {
        gateway_text(result->message, sizeof(result->message), "映射表每页必须为 1 - 8 个点位");
        return false;
    }
    result->offset = request->offset;
    for (uint32_t i = 0; i < config->item_count; ++i) {
        iot_edge_v1_ModbusGatewayPoint point = iot_edge_v1_ModbusGatewayPoint_init_zero;
        if (!gateway_point_metadata(config, &config->items[i], &point))
            continue;
        const uint32_t index = result->total++;
        if (index < request->offset || result->points_count >= request->limit)
            continue;
        if (config->items[i].which_item != iot_edge_v1_ConfigItem_derived_point_tag) {
            const bool unsupported = !strcmp(point.state, "unsupported");
            const bool disabled = !strcmp(point.state, "disabled");
            (void)edge_modbus_gateway_describe(map, platform, point.device_id.bytes,
                                               point.element_id, &point, now_ms);
            if (unsupported)
                gateway_text(point.state, sizeof(point.state), "unsupported");
            else if (disabled)
                gateway_text(point.state, sizeof(point.state), "disabled");
            else if (!enabled)
                gateway_text(point.state, sizeof(point.state), "gateway_disabled");
            else if (!listening)
                gateway_text(point.state, sizeof(point.state), "listener_error");
        }
        result->points[result->points_count++] = point;
    }
    return true;
}

static uint16_t read_word(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void write_word(uint8_t *p, uint16_t word) {
    p[0] = (uint8_t)(word >> 8);
    p[1] = (uint8_t)word;
}

size_t edge_modbus_gateway_reply(const edge_modbus_gateway *gateway, const uint8_t *request,
                                 size_t size, uint8_t response[260], uint64_t now_ms) {
    if (!gateway || size < 8 || size > 260 || read_word(request + 2) ||
        read_word(request + 4) != size - 6 || request[6] != 1)
        return 0;
    memcpy(response, request, 8);
    uint8_t exception = 0;
    uint16_t start = 0, quantity = 0;
    if (request[7] != 3 && request[7] != 4)
        exception = 1;
    else if (size != 12)
        exception = 3;
    else {
        start = read_word(request + 8);
        quantity = read_word(request + 10);
        if (!quantity || quantity > 125)
            exception = 3;
        else if ((uint32_t)start + quantity > gateway->count * EDGE_GATEWAY_SLOT_WORDS)
            exception = 2;
        else
            for (uint32_t word = start; word < (uint32_t)start + quantity; ++word)
                if (!gateway->points[word / EDGE_GATEWAY_SLOT_WORDS].active) {
                    exception = 2;
                    break;
                }
    }
    if (exception) {
        write_word(response + 4, 3);
        response[7] |= 0x80;
        response[8] = exception;
        return 9;
    }
    write_word(response + 4, (uint16_t)(3 + 2 * quantity));
    response[8] = (uint8_t)(2 * quantity);
    for (uint32_t i = 0; i < quantity; ++i) {
        const uint32_t word = start + i;
        const edge_gateway_point *point = &gateway->points[word / EDGE_GATEWAY_SLOT_WORDS];
        const uint32_t offset = word % EDGE_GATEWAY_SLOT_WORDS;
        uint16_t value;
        if (offset == 0)
            value = point->type;
        else if (offset == 1)
            value = point_quality(point, now_ms);
        else if (offset < 6)
            value = (uint16_t)(point->bits >> ((5 - offset) * 16));
        else
            value = (uint16_t)(point->observed_at_ms >> ((9 - offset) * 16));
        write_word(response + 9 + i * 2, value);
    }
    return 9 + 2 * quantity;
}
