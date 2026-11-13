#include "edge_modbus_gateway.h"
#include "edge_sha256.h"

#include <errno.h>
#include <float.h>
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
#include <syslog.h>
#include <unistd.h>
#endif

_Static_assert(sizeof(double) == sizeof(uint64_t), "gateway requires 64-bit double");
_Static_assert(sizeof(float) == sizeof(uint32_t), "gateway requires 32-bit float");

/* Keep existing 64-bit type codes; append native-width register encodings. */
enum {
    EDGE_GATEWAY_BOOL = 2, EDGE_GATEWAY_INT64 = 3, EDGE_GATEWAY_UINT64 = 4,
    EDGE_GATEWAY_DOUBLE = 5, EDGE_GATEWAY_INT16 = 6, EDGE_GATEWAY_UINT16 = 7,
    EDGE_GATEWAY_INT32 = 8, EDGE_GATEWAY_UINT32 = 9, EDGE_GATEWAY_FLOAT32 = 10
};

static unsigned native_words(unsigned type) {
    switch (type) {
    case EDGE_GATEWAY_BOOL: case EDGE_GATEWAY_INT16: case EDGE_GATEWAY_UINT16: return 1;
    case EDGE_GATEWAY_INT32: case EDGE_GATEWAY_UINT32: case EDGE_GATEWAY_FLOAT32: return 2;
    case EDGE_GATEWAY_INT64: case EDGE_GATEWAY_UINT64: case EDGE_GATEWAY_DOUBLE: return 4;
    default: return 0;
    }
}

static unsigned configured_type(const iot_edge_v1_ConfigItem *item) {
    const char *type = "";
    double scale = 1;
    switch (item->which_item) {
    case iot_edge_v1_ConfigItem_modbus_register_tag:
        type = item->item.modbus_register.data_type; scale = item->item.modbus_register.scale; break;
    case iot_edge_v1_ConfigItem_s7_area_tag:
        type = item->item.s7_area.data_type; scale = item->item.s7_area.scale; break;
    case iot_edge_v1_ConfigItem_industrial_point_tag:
        type = item->item.industrial_point.data_type; scale = item->item.industrial_point.scale; break;
    case iot_edge_v1_ConfigItem_sl651_element_tag:
        return !strcmp(item->item.sl651_element.encoding, "BCD") ? EDGE_GATEWAY_DOUBLE : 0;
    case iot_edge_v1_ConfigItem_external_io_tag:
        return item->item.external_io.kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI
            ? EDGE_GATEWAY_DOUBLE : EDGE_GATEWAY_BOOL;
    default: return 0;
    }
    if (!strcmp(type, "BOOL")) return EDGE_GATEWAY_BOOL;
    if (!strcmp(type, "INT8")) return EDGE_GATEWAY_INT16;
    if (!strcmp(type, "UINT8") || !strcmp(type, "BYTE")) return EDGE_GATEWAY_UINT16;
    if (!strcmp(type, "INT16")) return scale == 1 ? EDGE_GATEWAY_INT16 : EDGE_GATEWAY_DOUBLE;
    if (!strcmp(type, "UINT16") || !strcmp(type, "WORD")) return scale == 1 ? EDGE_GATEWAY_UINT16 : EDGE_GATEWAY_DOUBLE;
    if (!strcmp(type, "INT32")) return scale == 1 ? EDGE_GATEWAY_INT32 : EDGE_GATEWAY_DOUBLE;
    if (!strcmp(type, "UINT32") || !strcmp(type, "DWORD")) return scale == 1 ? EDGE_GATEWAY_UINT32 : EDGE_GATEWAY_DOUBLE;
    if (!strcmp(type, "INT64")) return EDGE_GATEWAY_INT64;
    if (!strcmp(type, "UINT64")) return EDGE_GATEWAY_UINT64;
    if (!strcmp(type, "FLOAT32") || !strcmp(type, "FLOAT") || !strcmp(type, "REAL")) return EDGE_GATEWAY_FLOAT32;
    if (!strcmp(type, "DOUBLE") || !strcmp(type, "FLOAT64") || !strcmp(type, "LREAL") ||
        !strcmp(type, "BCD") || !strcmp(type, "BCD_SIGNED")) return EDGE_GATEWAY_DOUBLE;
    return 0;
}

typedef struct {
    uint8_t platform[16], device[16];
    char element[65];
    bool active, writable, external_io;
    uint32_t timeout_sec;
    uint16_t type, native_quality, native_type, value_address;
    uint64_t bits, native_bits, observed_at_ms, received_at_ms;
} edge_gateway_point;

struct edge_modbus_gateway {
    size_t count;
    size_t capacity;
    edge_gateway_point *points;
    uint16_t *holding, *discrete;
    size_t holding_count, discrete_count;
    uint8_t unit_id;
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

/* Compact views contain only active, configured types, in persistent identity
 * order. The indices cost O(points), never a full 16-bit register image. */
static bool prepare_layout(edge_modbus_gateway *gateway) {
    size_t holding_count = 0, discrete_count = 0;
    uint32_t words = 0;
    for (size_t i = 0; i < gateway->count; ++i) {
        const edge_gateway_point *point = &gateway->points[i];
        if (!point->active || !native_words(point->native_type)) continue;
        if (point->native_type == EDGE_GATEWAY_BOOL)
            ++discrete_count;
        else {
            words += native_words(point->native_type);
            ++holding_count;
        }
    }
    if (words > 65536U || discrete_count > 65536U) return false;
    uint16_t *holding = holding_count ? malloc(holding_count * sizeof(*holding)) : NULL;
    uint16_t *discrete = discrete_count ? malloc(discrete_count * sizeof(*discrete)) : NULL;
    if ((holding_count && !holding) || (discrete_count && !discrete)) {
        free(holding);
        free(discrete);
        return false;
    }
    size_t h = 0, d = 0;
    words = 0;
    for (size_t i = 0; i < gateway->count; ++i) {
        edge_gateway_point *point = &gateway->points[i];
        point->value_address = 0;
        if (!point->active || !native_words(point->native_type)) continue;
        if (point->native_type == EDGE_GATEWAY_BOOL) {
            point->value_address = (uint16_t)d;
            discrete[d++] = (uint16_t)i;
        } else {
            point->value_address = (uint16_t)words;
            words += native_words(point->native_type);
            holding[h++] = (uint16_t)i;
        }
    }
    gateway->holding = holding;
    gateway->discrete = discrete;
    gateway->holding_count = holding_count;
    gateway->discrete_count = discrete_count;
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
        /* rename is the commit boundary: after it succeeds, the live map must
         * also switch even if directory durability reporting fails. */
        if (fd < 0 || fsync(fd) != 0)
            syslog(LOG_ERR, "Modbus map applied but directory sync failed: %s", strerror(errno));
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
                      const char *element, const iot_edge_v1_ConfigItem *item) {
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
    gateway->points[slot].native_type = (uint16_t)configured_type(item);
    gateway->points[slot].external_io = item->which_item == iot_edge_v1_ConfigItem_external_io_tag;
    switch (item->which_item) {
    case iot_edge_v1_ConfigItem_modbus_register_tag:
        gateway->points[slot].writable = item->item.modbus_register.writable; break;
    case iot_edge_v1_ConfigItem_s7_area_tag:
        gateway->points[slot].writable = item->item.s7_area.writable; break;
    case iot_edge_v1_ConfigItem_industrial_point_tag:
        gateway->points[slot].writable = item->item.industrial_point.writable; break;
    case iot_edge_v1_ConfigItem_external_io_tag:
        gateway->points[slot].writable =
            item->item.external_io.kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO; break;
    default:
        gateway->points[slot].writable = false; break;
    }
    gateway->points[slot].value_address = 0;
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
            if (device && !add_point(gateway, sources[s].platform_id, config, device, element, item))
                goto fail;
        }
    }
    if (!prepare_layout(gateway))
        goto fail;
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
        free(gateway->holding);
        free(gateway->discrete);
        free(gateway);
    }
}

bool edge_modbus_gateway_digest(const edge_modbus_gateway *gateway,
                                const uint8_t platform[16], uint8_t digest[32]) {
    if (!gateway) return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = edge_sha256_starts(&context, 0) == 0;
    for (size_t i = 0; ok && i < gateway->count; ++i) {
        const edge_gateway_point *point = &gateway->points[i];
        if (memcmp(point->platform, platform, 16)) continue;
        const uint8_t metadata[] = {
            (uint8_t)(i >> 8), (uint8_t)i, point->active ? 1 : 0,
            (uint8_t)point->native_type, (uint8_t)(point->value_address >> 8),
            (uint8_t)point->value_address,
            point->active ? (uint8_t)native_words(point->native_type) : 0,
            point->writable ? 1 : 0
        };
        ok = edge_sha256_update(&context, metadata, sizeof(metadata)) == 0 &&
             edge_sha256_update(&context, point->device, 16) == 0 &&
             edge_sha256_update(&context, (const unsigned char *)point->element,
                                strlen(point->element) + 1) == 0;
    }
    if (ok) ok = edge_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    return ok;
}

bool edge_modbus_gateway_reorder(edge_modbus_gateway *gateway, const char *path,
                                 const uint8_t platform[16],
                                 const iot_edge_v1_ModbusGatewayOrder *order) {
    uint8_t digest[32];
    if (!gateway || order->expected_digest.size != sizeof(digest) ||
        order->addresses.size < 4 || order->addresses.size % 2 ||
        order->addresses.size > EDGE_GATEWAY_MAX_POINTS * 2 ||
        !edge_modbus_gateway_digest(gateway, platform, digest) ||
        memcmp(digest, order->expected_digest.bytes, sizeof(digest)))
        return false;
    const size_t count = order->addresses.size / 2;
    /* One bit per reserved slot; validate the entire permutation before I/O. */
    uint8_t selected[(EDGE_GATEWAY_MAX_POINTS + 7) / 8] = {0};
    bool discrete = false;
    for (size_t i = 0; i < count; ++i) {
        const unsigned address = ((unsigned)order->addresses.bytes[i * 2] << 8) |
                                  order->addresses.bytes[i * 2 + 1];
        const size_t slot = address / EDGE_GATEWAY_SLOT_WORDS;
        if (address % EDGE_GATEWAY_SLOT_WORDS || slot >= gateway->count ||
            !gateway->points[slot].active || !native_words(gateway->points[slot].native_type) ||
            memcmp(gateway->points[slot].platform, platform, 16) ||
            (selected[slot / 8] & (1U << (slot % 8))))
            return false;
        const bool point_discrete = gateway->points[slot].native_type == EDGE_GATEWAY_BOOL;
        if (i && discrete != point_discrete) return false;
        discrete = point_discrete;
        selected[slot / 8] |= (uint8_t)(1U << (slot % 8));
    }
    edge_modbus_gateway candidate = *gateway;
    candidate.points = malloc(gateway->count * sizeof(*candidate.points));
    if (!candidate.points) return false;
    memcpy(candidate.points, gateway->points, gateway->count * sizeof(*candidate.points));
    candidate.holding = NULL;
    candidate.discrete = NULL;
    size_t next = 0;
    bool changed = false;
    for (size_t slot = 0; slot < gateway->count; ++slot) {
        if (!(selected[slot / 8] & (1U << (slot % 8)))) continue;
        const size_t source = (((unsigned)order->addresses.bytes[next * 2] << 8) |
                              order->addresses.bytes[next * 2 + 1]) / EDGE_GATEWAY_SLOT_WORDS;
        candidate.points[slot] = gateway->points[source];
        changed = changed || slot != source;
        ++next;
    }
    const bool saved = prepare_layout(&candidate) && (!changed || save_map(&candidate, path));
    if (saved && changed) {
        free(gateway->points);
        free(gateway->holding);
        free(gateway->discrete);
        gateway->points = candidate.points;
        gateway->capacity = gateway->count;
        gateway->holding = candidate.holding;
        gateway->discrete = candidate.discrete;
        gateway->holding_count = candidate.holding_count;
        gateway->discrete_count = candidate.discrete_count;
    } else {
        free(candidate.points);
        free(candidate.holding);
        free(candidate.discrete);
    }
    return saved;
}

bool edge_modbus_gateway_export(const edge_modbus_gateway *gateway, const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    fputs("platform_id,device_id,element_id,register_type,address,words\n", file);
    for (size_t i = 0; i < gateway->count; ++i) {
        const edge_gateway_point *point = &gateway->points[i];
        if (!point->active || !native_words(point->native_type))
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
        fprintf(file, "\",%s,%u,%u\n",
                point->native_type == EDGE_GATEWAY_BOOL ? "DISCRETE_INPUT" : "HOLDING_REGISTER",
                point->value_address, native_words(point->native_type));
    }
    const bool ok = !ferror(file);
    return fclose(file) == 0 && ok;
}

static bool native_value(edge_gateway_point *point) {
    point->native_bits = 0;
    const unsigned type = point->native_type;
    if (type == EDGE_GATEWAY_BOOL) {
        if (point->type != iot_edge_v1_ScalarValue_bool_value_tag) return false;
        point->native_bits = point->bits ? 1 : 0;
        return true;
    }
    int64_t signed_value = 0;
    double number = 0;
    if (point->type == iot_edge_v1_ScalarValue_signed_value_tag) {
        memcpy(&signed_value, &point->bits, sizeof(signed_value));
        number = (double)signed_value;
    } else if (point->type == iot_edge_v1_ScalarValue_unsigned_value_tag)
        number = (double)point->bits;
    else if (point->type == iot_edge_v1_ScalarValue_double_value_tag)
        memcpy(&number, &point->bits, sizeof(number));
    else return false;
    if (!isfinite(number)) return false;
    if (type == EDGE_GATEWAY_FLOAT32) {
        if (fabs(number) > FLT_MAX) return false;
        const float value = (float)number;
        if (!isfinite(value)) return false;
        uint32_t bits; memcpy(&bits, &value, sizeof(bits)); point->native_bits = bits;
        return true;
    }
    if (type == EDGE_GATEWAY_DOUBLE) {
        memcpy(&point->native_bits, &number, sizeof(number));
        return true;
    }
    const bool signed_type = type == EDGE_GATEWAY_INT16 || type == EDGE_GATEWAY_INT32 || type == EDGE_GATEWAY_INT64;
    const unsigned width = native_words(type) * 16;
    if (!width || width > 64) return false;
    if (signed_type) {
        const int64_t maximum = width == 64 ? INT64_MAX : (INT64_C(1) << (width - 1)) - 1;
        const int64_t minimum = width == 64 ? INT64_MIN : -(INT64_C(1) << (width - 1));
        int64_t value;
        if (point->type == iot_edge_v1_ScalarValue_signed_value_tag) {
            if (signed_value < minimum || signed_value > maximum) return false;
            value = signed_value;
        } else if (point->type == iot_edge_v1_ScalarValue_unsigned_value_tag) {
            if (point->bits > (uint64_t)maximum) return false;
            value = (int64_t)point->bits;
        } else {
            if (trunc(number) != number || number < -(ldexp(1.0, (int)width - 1)) ||
                number >= ldexp(1.0, (int)width - 1)) return false;
            value = (int64_t)number;
        }
        point->native_bits = (uint64_t)value;
    } else {
        const uint64_t maximum = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
        if (point->type == iot_edge_v1_ScalarValue_unsigned_value_tag) {
            if (point->bits > maximum) return false;
            point->native_bits = point->bits;
        } else if (point->type == iot_edge_v1_ScalarValue_signed_value_tag) {
            if (signed_value < 0 || (uint64_t)signed_value > maximum) return false;
            point->native_bits = (uint64_t)signed_value;
        } else {
            if (trunc(number) != number || number < 0 || number >= ldexp(1.0, (int)width)) return false;
            point->native_bits = (uint64_t)number;
        }
    }
    if (width < 64) point->native_bits &= (UINT64_C(1) << width) - 1;
    return true;
}

void edge_modbus_gateway_update(edge_modbus_gateway *gateway, const uint8_t platform[16],
                                const edge_acquisition_sample *sample, uint64_t now_ms) {
    if (!gateway)
        return;
    if (!sample) {
        for (size_t i = 0; i < gateway->count; ++i)
            if (!memcmp(gateway->points[i].platform, platform, 16)) {
                gateway->points[i].native_quality = 2;
            }
        return;
    }
    for (size_t i = 0; i < gateway->count; ++i) {
        edge_gateway_point *point = &gateway->points[i];
        if (!point->active || memcmp(point->platform, platform, 16) ||
            memcmp(point->device, sample->device_id, 16) ||
            strcmp(point->element, sample->element_id))
            continue;
        point->native_quality = 2;
        point->type = 0;
        point->bits = 0;
        point->native_bits = 0;
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
        if (!native_value(point)) return;
        point->native_quality = 1;
        return;
    }
}

static uint16_t point_quality(const edge_gateway_point *point, uint64_t now_ms) {
    const uint16_t quality = point->native_quality;
    return quality == 1 && now_ms >= point->received_at_ms &&
                   now_ms - point->received_at_ms >= (uint64_t)point->timeout_sec * 1000
               ? 3
               : quality;
}

static void point_layout(iot_edge_v1_ModbusGatewayPoint *output, const edge_gateway_point *point) {
    output->value_type = point->native_type;
    output->has_mapping = point->active && native_words(point->native_type);
    snprintf(output->register_type, sizeof(output->register_type), "%s",
             !output->has_mapping ? "" :
             point->native_type == EDGE_GATEWAY_BOOL ? "DISCRETE_INPUT" : "HOLDING_REGISTER");
    output->value_address = output->has_mapping ? point->value_address : 0;
    output->value_words = output->has_mapping ? native_words(point->native_type) : 0;
}

static bool format_native_value(unsigned type, uint64_t bits, char *output, size_t size) {
    switch (type) {
    case EDGE_GATEWAY_BOOL:
        snprintf(output, size, "%s", bits ? "true" : "false"); break;
    case EDGE_GATEWAY_INT16: case EDGE_GATEWAY_INT32: case EDGE_GATEWAY_INT64: {
        int64_t value;
        if (type == EDGE_GATEWAY_INT16) {
            const uint16_t word = (uint16_t)bits;
            int16_t number; memcpy(&number, &word, sizeof(number)); value = number;
        } else if (type == EDGE_GATEWAY_INT32) {
            const uint32_t words = (uint32_t)bits;
            int32_t number; memcpy(&number, &words, sizeof(number)); value = number;
        } else memcpy(&value, &bits, sizeof(value));
        snprintf(output, size, "%" PRId64, value); break;
    }
    case EDGE_GATEWAY_UINT16: case EDGE_GATEWAY_UINT32: case EDGE_GATEWAY_UINT64:
        snprintf(output, size, "%" PRIu64, bits); break;
    case EDGE_GATEWAY_FLOAT32: {
        const uint32_t words = (uint32_t)bits;
        float value; memcpy(&value, &words, sizeof(value));
        if (!isfinite(value)) return false;
        snprintf(output, size, "%.9g", (double)value); break;
    }
    case EDGE_GATEWAY_DOUBLE: {
        double value; memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value)) return false;
        snprintf(output, size, "%.17g", value); break;
    }
    default: return false;
    }
    return true;
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
        output->address = (uint32_t)(i * EDGE_GATEWAY_SLOT_WORDS);
        output->words = EDGE_GATEWAY_SLOT_WORDS;
        point_layout(output, point);
        output->quality = point_quality(point, now_ms);
        output->observed_at_ms = (int64_t)point->observed_at_ms;
        snprintf(output->state, sizeof(output->state), "%s", point->active ? "active" : "disabled");
        if (output->quality != 1 && output->quality != 3)
            return true;
        (void)format_native_value(point->native_type, point->native_bits,
                                  output->value, sizeof(output->value));
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
    if (type[0] && !configured_type(item))
        gateway_text(point->state, sizeof(point->state), "unsupported");
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
            if (unsupported || disabled) {
                point.has_mapping = false;
                point.value_address = point.value_words = 0;
                point.register_type[0] = 0;
            }
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

static size_t reply_discrete(const edge_modbus_gateway *gateway, const uint8_t *request,
                             size_t size, uint8_t response[260]) {
    const unsigned start = size == 12 ? read_word(request + 8) : 0;
    const unsigned quantity = size == 12 ? read_word(request + 10) : 0;
    unsigned exception = size != 12 || !quantity || quantity > 2000 ? 3 : 0;
    if (!exception && (uint32_t)start + quantity > 65536U) exception = 2;
    if (exception) {
        write_word(response + 4, 3); response[7] |= 0x80;
        response[8] = (uint8_t)exception; return 9;
    }
    const unsigned bytes = (quantity + 7) / 8;
    write_word(response + 4, (uint16_t)(3 + bytes));
    response[8] = (uint8_t)bytes;
    memset(response + 9, 0, bytes);
    const uint32_t end = (uint32_t)start + quantity;
    for (uint32_t address = start; address < end && address < gateway->discrete_count; ++address)
        if (gateway->points[gateway->discrete[address]].native_bits)
            response[9 + (address - start) / 8] |= (uint8_t)(1U << ((address - start) % 8));
    return 9 + bytes;
}

/* Locate the first point span intersecting an address. */
static size_t holding_from(const edge_modbus_gateway *gateway, uint32_t start) {
    size_t low = 0, high = gateway->holding_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const edge_gateway_point *point = &gateway->points[gateway->holding[middle]];
        if ((uint32_t)point->value_address + native_words(point->native_type) <= start)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static void reply_holding(const edge_modbus_gateway *gateway, uint32_t start,
                           uint32_t quantity, uint8_t *output) {
    const uint32_t end = start + quantity;
    for (size_t i = holding_from(gateway, start); i < gateway->holding_count; ++i) {
        const edge_gateway_point *point = &gateway->points[gateway->holding[i]];
        const unsigned words = native_words(point->native_type);
        if (point->value_address >= end) break;
        const uint32_t first = start > point->value_address ? start : point->value_address;
        for (uint32_t address = first; address < end &&
             address < (uint32_t)point->value_address + words; ++address) {
            const unsigned offset = address - point->value_address;
            write_word(output + (address - start) * 2,
                       (uint16_t)(point->native_bits >> ((words - 1 - offset) * 16)));
        }
    }
}

static bool write_function(unsigned function) {
    return function == 5 || function == 6 || function == 15 || function == 16;
}

/* Full PDU validation precedes enumeration, including ignored addresses. */
static int write_bounds(const uint8_t *request, size_t size, uint32_t *start,
                         uint32_t *quantity) {
    if (size < 12) return -3;
    *start = read_word(request + 8);
    const unsigned function = request[7];
    if (function == 5 || function == 6) {
        if (size != 12) return -3;
        if (function == 5 && read_word(request + 10) != 0 &&
            read_word(request + 10) != 0xff00) return -3;
        *quantity = 1;
    } else if (function == 15 || function == 16) {
        *quantity = read_word(request + 10);
        if (size < 13 || !*quantity || *quantity > (function == 15 ? 1968U : 123U)) return -3;
        const unsigned bytes = function == 15 ? (*quantity + 7) / 8 : *quantity * 2;
        if (request[12] != bytes || size != 13 + bytes) return -3;
    } else return -1;
    return *start + *quantity <= 65536U ? 0 : -2;
}

static void write_identity(const edge_gateway_point *point, edge_modbus_gateway_write *write) {
    memcpy(write->platform_id, point->platform, 16);
    memcpy(write->device_id, point->device, 16);
    strcpy(write->element_id, point->element);
    write->external_io = point->external_io;
}

int edge_modbus_gateway_next_write(const edge_modbus_gateway *gateway,
    const uint8_t *request, size_t size, uint32_t *cursor,
    edge_modbus_gateway_write *write) {
    uint32_t start, quantity;
    if (!gateway || !request || !cursor || !write) return -4;
    if (size < 8U || request[6] != (gateway->unit_id ? gateway->unit_id : 1U)) return -4;
    const int error = write_bounds(request, size, &start, &quantity);
    if (error) return error;
    if (*cursor >= quantity) return 0;
    const uint32_t end = start + quantity;
    const unsigned function = request[7];
    if (function == 5 || function == 15) {
        while (*cursor < quantity && start + *cursor < gateway->discrete_count) {
            const unsigned offset = (*cursor)++;
            const edge_gateway_point *point = &gateway->points[gateway->discrete[start + offset]];
            if (!point->writable) continue;
            const bool value = function == 5 ? read_word(request + 10) != 0
                : (request[13 + offset / 8] & (1U << (offset % 8))) != 0;
            write_identity(point, write);
            write->value[0] = value ? '1' : '0';
            write->value[1] = '\0';
            return 1;
        }
    } else {
        for (size_t i = holding_from(gateway, start + *cursor); i < gateway->holding_count; ++i) {
            const edge_gateway_point *point = &gateway->points[gateway->holding[i]];
            const unsigned words = native_words(point->native_type);
            const uint32_t point_end = (uint32_t)point->value_address + words;
            if (point->value_address >= end) break;
            *cursor = (point_end < end ? point_end : end) - start;
            if (!point->writable) continue;
            const uint32_t first = start > point->value_address ? start : point->value_address;
            const uint32_t last = end < point_end ? end : point_end;
            const bool full = first == point->value_address && last == point_end;
            if (!full && point->native_quality != 1) return -4;
            uint64_t bits = full ? 0 : point->native_bits;
            for (uint32_t address = first; address < last; ++address) {
                const unsigned shift = (words - 1 - (address - point->value_address)) * 16;
                const uint16_t value = read_word(request + (function == 6 ? 10 : 13 + (address - start) * 2));
                bits = (bits & ~(UINT64_C(0xffff) << shift)) | ((uint64_t)value << shift);
            }
            write_identity(point, write);
            if (!format_native_value(point->native_type, bits, write->value, sizeof(write->value))) return -3;
            return 1;
        }
    }
    *cursor = quantity;
    return 0;
}

static size_t reply_write(const edge_modbus_gateway *gateway, const uint8_t *request,
                           size_t size, uint8_t response[260]) {
    uint32_t start, quantity, cursor = 0;
    int result = write_bounds(request, size, &start, &quantity);
    edge_modbus_gateway_write write;
    if (!result)
        while ((result = edge_modbus_gateway_next_write(gateway, request, size, &cursor, &write)) > 0) {}
    if (result < 0) {
        write_word(response + 4, 3);
        response[7] |= 0x80;
        response[8] = (uint8_t)-result;
        return 9;
    }
    write_word(response + 4, 6);
    memcpy(response + 8, request + 8, 4);
    return 12;
}

bool edge_modbus_gateway_set_unit_id(edge_modbus_gateway *gateway, uint32_t unit_id) {
    if (!gateway || unit_id > 247U) return false;
    gateway->unit_id = unit_id ? (uint8_t)unit_id : 1U;
    return true;
}

size_t edge_modbus_gateway_reply(const edge_modbus_gateway *gateway, const uint8_t *request,
                                 size_t size, uint8_t response[260], uint64_t now_ms) {
    (void)now_ms;
    if (!gateway || size < 8 || size > 260 || read_word(request + 2) ||
        read_word(request + 4) != size - 6 ||
        request[6] != (gateway->unit_id ? gateway->unit_id : 1U))
        return 0;
    memcpy(response, request, 8);
    if (write_function(request[7])) return reply_write(gateway, request, size, response);
    if (request[7] == 1 || request[7] == 2) return reply_discrete(gateway, request, size, response);
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
        else if ((uint32_t)start + quantity > 65536U)
            exception = 2;
    }
    if (exception) {
        write_word(response + 4, 3);
        response[7] |= 0x80;
        response[8] = exception;
        return 9;
    }
    write_word(response + 4, (uint16_t)(3 + 2 * quantity));
    response[8] = (uint8_t)(2 * quantity);
    memset(response + 9, 0, quantity * 2);
    reply_holding(gateway, start, quantity, response + 9);
    return 9 + 2 * quantity;
}
