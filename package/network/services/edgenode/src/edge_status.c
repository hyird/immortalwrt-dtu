#include "edge_status.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define edge_unlink _unlink
#else
#include <unistd.h>
#define edge_unlink unlink
#endif

static int make_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static bool json_string(FILE *output, const char *value) {
    if (fputc('"', output) == EOF)
        return false;
    const unsigned char *cursor =
        (const unsigned char *)(value != NULL ? value : "");
    while (*cursor != '\0') {
        const unsigned char ch = *cursor++;
        if (ch == '"' || ch == '\\') {
            if (fputc('\\', output) == EOF || fputc(ch, output) == EOF)
                return false;
        } else if (ch == '\b' || ch == '\f' || ch == '\n' || ch == '\r' ||
                   ch == '\t') {
            const char escaped = ch == '\b'   ? 'b'
                                 : ch == '\f' ? 'f'
                                 : ch == '\n' ? 'n'
                                 : ch == '\r' ? 'r'
                                               : 't';
            if (fputc('\\', output) == EOF || fputc(escaped, output) == EOF)
                return false;
        } else if (ch < 32U || ch == 127U) {
            if (fprintf(output, "\\u%04x", ch) < 0)
                return false;
        } else if (fputc(ch, output) == EOF) {
            return false;
        }
    }
    return fputc('"', output) != EOF;
}

static bool json_key(FILE *output, const char *key) {
    return json_string(output, key) && fputc(':', output) != EOF;
}

static bool json_bool(FILE *output, bool value) {
    return fputs(value ? "true" : "false", output) >= 0;
}

static void format_uuid(const uint8_t value[16], char output[37]) {
    static const char digits[] = "0123456789abcdef";
    size_t written = 0U;
    for (size_t index = 0U; index < 16U; ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U)
            output[written++] = '-';
        output[written++] = digits[value[index] >> 4U];
        output[written++] = digits[value[index] & 0x0fU];
    }
    output[written] = '\0';
}

static bool json_uuid(FILE *output, const uint8_t *value, size_t size) {
    if (value == NULL || size != 16U)
        return fputs("null", output) >= 0;
    char uuid[37];
    format_uuid(value, uuid);
    return json_string(output, uuid);
}

static const char *phase_name(edge_retry_phase phase) {
    switch (phase) {
    case EDGE_RETRY_READY:
        return "ready";
    case EDGE_RETRY_WAITING:
        return "waiting";
    case EDGE_RETRY_CONNECTING:
        return "connecting";
    case EDGE_RETRY_AWAITING_APPLICATION:
        return "handshake";
    case EDGE_RETRY_CONNECTED:
        return "connected";
    default:
        return "unknown";
    }
}

static const char *protocol_name(iot_edge_v1_Protocol value) {
    switch (value) {
    case iot_edge_v1_Protocol_PROTOCOL_MODBUS:
        return "Modbus";
    case iot_edge_v1_Protocol_PROTOCOL_S7:
        return "S7";
    case iot_edge_v1_Protocol_PROTOCOL_SL651:
        return "SL651";
    default:
        return "Unknown";
    }
}

static const char *transport_name(iot_edge_v1_Transport value) {
    switch (value) {
    case iot_edge_v1_Transport_TRANSPORT_SERIAL:
        return "serial";
    case iot_edge_v1_Transport_TRANSPORT_ETHERNET:
        return "ethernet";
    default:
        return "unknown";
    }
}

static const char *mode_name(iot_edge_v1_LinkMode value) {
    switch (value) {
    case iot_edge_v1_LinkMode_LINK_MODE_SERIAL:
        return "serial";
    case iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT:
        return "tcp-client";
    case iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER:
        return "tcp-server";
    default:
        return "unknown";
    }
}

static bool write_string_field(FILE *output, const char *name,
                               const char *value, bool comma) {
    return (!comma || fputc(',', output) != EOF) && json_key(output, name) &&
           json_string(output, value);
}

static bool write_uint_field(FILE *output, const char *name,
                             uint64_t value, bool comma) {
    return (!comma || fputc(',', output) != EOF) && json_key(output, name) &&
           fprintf(output, "%" PRIu64, value) >= 0;
}

static bool write_bool_field(FILE *output, const char *name,
                             bool value, bool comma) {
    return (!comma || fputc(',', output) != EOF) && json_key(output, name) &&
           json_bool(output, value);
}

static bool write_uuid_field(FILE *output, const char *name,
                             const void *field, bool comma) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return (!comma || fputc(',', output) != EOF) && json_key(output, name) &&
           json_uuid(output, (const uint8_t *)field + sizeof(size), size);
}

static bool write_endpoint(FILE *output,
                           const iot_edge_v1_EndpointConfig *endpoint) {
    if (fputc('{', output) == EOF ||
        !write_uuid_field(output, "id", &endpoint->endpoint_id, false) ||
        !write_string_field(output, "name", endpoint->name, true) ||
        !write_string_field(output, "protocol", protocol_name(endpoint->protocol), true) ||
        !write_string_field(output, "transport", transport_name(endpoint->transport), true) ||
        !write_string_field(output, "mode", mode_name(endpoint->mode), true) ||
        !write_bool_field(output, "enabled", endpoint->enabled, true) ||
        !write_string_field(output, "interface", endpoint->interface_name, true))
        return false;
    if (endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL) {
        if (!write_string_field(output, "channel", endpoint->serial.channel, true) ||
            !write_uint_field(output, "baudRate", endpoint->serial.baud_rate, true) ||
            !write_uint_field(output, "dataBits", endpoint->serial.data_bits, true) ||
            !write_uint_field(output, "stopBits", endpoint->serial.stop_bits, true) ||
            !write_string_field(output, "parity", endpoint->serial.parity, true) ||
            !write_bool_field(output, "rs485", endpoint->serial.rs485, true))
            return false;
    } else if (!write_string_field(output, "ip", endpoint->ip, true) ||
               !write_uint_field(output, "port", endpoint->port, true)) {
        return false;
    }
    return fputc('}', output) != EOF;
}

static bool write_device(FILE *output,
                         const iot_edge_v1_DeviceConfig *device) {
    if (fputc('{', output) == EOF ||
        !write_uuid_field(output, "id", &device->device_id, false) ||
        !write_uuid_field(output, "endpointId", &device->endpoint_id, true) ||
        !write_string_field(output, "code", device->device_code, true) ||
        !write_string_field(output, "name", device->name, true) ||
        !write_string_field(output, "protocol", protocol_name(device->protocol), true) ||
        !write_bool_field(output, "enabled", device->enabled, true) ||
        !write_uint_field(output, "ioIntervalMs",
                          device->io_interval_ms == 0U ? 1000U : device->io_interval_ms,
                          true) ||
        !write_uint_field(output, "reportIntervalSec", device->report_interval_sec, true) ||
        !write_uint_field(output, "onlineTimeoutSec", device->online_timeout_sec, true) ||
        !write_uint_field(output, "fastReadDurationSec",
                          device->command_fast_read_duration_sec, true) ||
        !write_uint_field(output, "fastReadIntervalSec",
                          device->command_fast_read_interval_sec, true))
        return false;
    if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS) {
        if (!write_uint_field(output, "slaveId", device->modbus_slave_id, true) ||
            !write_string_field(output, "modbusMode", device->modbus_mode, true) ||
            !write_uint_field(output, "mergeGap", device->modbus_merge_gap, true) ||
            !write_uint_field(output, "maxQuantity", device->modbus_max_quantity, true))
            return false;
    } else if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_S7) {
        if (!write_string_field(output, "connectionMode", device->s7_connection_mode, true) ||
            !write_string_field(output, "connectionType", device->s7_connection_type, true) ||
            !write_uint_field(output, "rack", device->s7_rack, true) ||
            !write_uint_field(output, "slot", device->s7_slot, true) ||
            !write_string_field(output, "localTsap", device->s7_local_tsap, true) ||
            !write_string_field(output, "remoteTsap", device->s7_remote_tsap, true))
            return false;
    }
    return fputc('}', output) != EOF;
}

static bool write_point(FILE *output, const iot_edge_v1_ConfigItem *item) {
    if (fputc('{', output) == EOF)
        return false;
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *point = &item->item.modbus_register;
        if (!write_string_field(output, "kind", "modbus", false) ||
            !write_uuid_field(output, "deviceId", &point->device_id, true) ||
            !write_string_field(output, "elementId", point->element_id, true) ||
            !write_string_field(output, "name", point->name, true) ||
            !write_string_field(output, "unit", point->unit, true) ||
            !write_string_field(output, "registerType", point->register_type, true) ||
            !write_string_field(output, "dataType", point->data_type, true) ||
            !write_string_field(output, "byteOrder", point->byte_order, true) ||
            !write_uint_field(output, "address", point->address, true) ||
            !write_uint_field(output, "quantity", point->quantity, true) ||
            fprintf(output, ",\"scale\":%.17g,\"decimals\":%" PRId32,
                    point->scale, point->decimals) < 0 ||
            !write_bool_field(output, "writable", point->writable, true))
            return false;
    } else if (item->which_item == iot_edge_v1_ConfigItem_s7_area_tag) {
        const iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        if (!write_string_field(output, "kind", "s7", false) ||
            !write_uuid_field(output, "deviceId", &point->device_id, true) ||
            !write_string_field(output, "elementId", point->element_id, true) ||
            !write_string_field(output, "name", point->name, true) ||
            !write_string_field(output, "unit", point->unit, true) ||
            !write_string_field(output, "area", point->area, true) ||
            !write_uint_field(output, "dbNumber", point->db_number, true) ||
            !write_uint_field(output, "start", point->start, true) ||
            !write_uint_field(output, "startBit", point->start_bit, true) ||
            !write_uint_field(output, "size", point->size, true) ||
            !write_string_field(output, "dataType", point->data_type, true) ||
            fprintf(output, ",\"scale\":%.17g,\"decimals\":%" PRId32,
                    point->scale, point->decimals) < 0 ||
            !write_bool_field(output, "writable", point->writable, true))
            return false;
    } else {
        if (!write_string_field(output, "kind", "sl651", false))
            return false;
    }
    return fputc('}', output) != EOF;
}

static bool write_config(FILE *output, const edge_runtime_config *config) {
    if (fputc('{', output) == EOF ||
        !write_uint_field(output, "revision", config != NULL ? config->revision : 0U,
                          false) ||
        !write_uint_field(output, "itemCount", config != NULL ? config->item_count : 0U,
                          true) ||
        !write_uint_field(output, "endpointCount",
                          config != NULL ? config->endpoint_count : 0U, true) ||
        !write_uint_field(output, "deviceCount",
                          config != NULL ? config->device_count : 0U, true) ||
        fputs(",\"endpoints\":[", output) < 0)
        return false;
    bool comma = false;
    if (config != NULL) {
        for (uint32_t index = 0U; index < config->item_count; ++index) {
            const iot_edge_v1_ConfigItem *item = &config->items[index];
            if (item->which_item != iot_edge_v1_ConfigItem_endpoint_tag)
                continue;
            if ((comma && fputc(',', output) == EOF) ||
                !write_endpoint(output, &item->item.endpoint))
                return false;
            comma = true;
        }
    }
    if (fputs("],\"devices\":[", output) < 0)
        return false;
    comma = false;
    if (config != NULL) {
        for (uint32_t index = 0U; index < config->item_count; ++index) {
            const iot_edge_v1_ConfigItem *item = &config->items[index];
            if (item->which_item != iot_edge_v1_ConfigItem_device_tag)
                continue;
            if ((comma && fputc(',', output) == EOF) ||
                !write_device(output, &item->item.device))
                return false;
            comma = true;
        }
    }
    if (fputs("],\"points\":[", output) < 0)
        return false;
    comma = false;
    if (config != NULL) {
        for (uint32_t index = 0U; index < config->item_count; ++index) {
            const iot_edge_v1_ConfigItem *item = &config->items[index];
            if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag ||
                item->which_item == iot_edge_v1_ConfigItem_device_tag)
                continue;
            if ((comma && fputc(',', output) == EOF) || !write_point(output, item))
                return false;
            comma = true;
        }
    }
    return fputs("]}", output) >= 0;
}

static uint64_t age_seconds(uint64_t now_ms, uint64_t event_ms) {
    return event_ms == 0U || now_ms < event_ms ? 0U : (now_ms - event_ms) / 1000U;
}

static bool write_platform(FILE *output, const edge_status_snapshot *snapshot,
                           const edge_status_platform *platform) {
    const edge_platform_config *config = platform->config;
    const edge_memory_outbox *outbox = platform->outbox;
    const edge_memory_config_set *staging = platform->staging_config;
    if (config == NULL || fputc('{', output) == EOF ||
        !json_key(output, "id") || !json_uuid(output, config->id, 16U) ||
        !write_string_field(output, "name", config->name, true) ||
        !write_uint_field(output, "priority", config->priority, true) ||
        !write_string_field(output, "state", phase_name(platform->phase), true) ||
        !write_bool_field(output, "websocketOpen", platform->websocket_open, true) ||
        !write_bool_field(output, "enrolled", platform->enrolled, true) ||
        !write_uint_field(output, "heartbeatIntervalSec",
                          platform->heartbeat_interval_sec, true) ||
        !write_bool_field(output, "hasHeartbeat",
                          platform->last_heartbeat_ms != 0U, true) ||
        !write_uint_field(output, "lastHeartbeatAgeSec",
                          age_seconds(snapshot->now_ms, platform->last_heartbeat_ms), true) ||
        !write_bool_field(output, "hasInbound",
                          platform->last_inbound_ms != 0U, true) ||
        !write_uint_field(output, "lastInboundAgeSec",
                          age_seconds(snapshot->now_ms, platform->last_inbound_ms), true) ||
        !write_uint_field(output, "stagingRevision",
                          staging != NULL ? staging->revision : 0U, true) ||
        !write_uint_field(output, "stagingReceived",
                          staging != NULL ? staging->received_count : 0U, true) ||
        !write_uint_field(output, "stagingItems",
                          staging != NULL ? staging->item_count : 0U, true) ||
        fputs(",\"outbox\":{", output) < 0 ||
        !write_uint_field(output, "records", outbox != NULL ? outbox->count : 0U,
                          false) ||
        !write_uint_field(output, "bytes", outbox != NULL ? outbox->bytes : 0U, true) ||
        !write_uint_field(output, "inFlight", outbox != NULL ? outbox->in_flight : 0U,
                          true) ||
        !write_uint_field(output, "maximumBytes",
                          outbox != NULL ? outbox->maximum_bytes : config->outbox_max_bytes,
                          true) ||
        fputs("},\"config\":", output) < 0 ||
        !write_config(output, platform->runtime_config))
        return false;
    return fputc('}', output) != EOF;
}

bool edge_status_render(FILE *output, const edge_status_snapshot *snapshot) {
    if (output == NULL || snapshot == NULL ||
        (snapshot->platform_count != 0U && snapshot->platforms == NULL) ||
        snapshot->platform_count > EDGE_MAX_PLATFORMS)
        return false;
    if (fputc('{', output) == EOF ||
        !write_string_field(output, "softwareVersion", snapshot->software_version, false) ||
        !write_uint_field(output, "generatedAtMonotonicMs", snapshot->now_ms, true) ||
        fputs(",\"platforms\":[", output) < 0)
        return false;
    for (size_t index = 0U; index < snapshot->platform_count; ++index) {
        if ((index != 0U && fputc(',', output) == EOF) ||
            !write_platform(output, snapshot, &snapshot->platforms[index]))
            return false;
    }
    return fputs("]}\n", output) >= 0 && !ferror(output);
}

bool edge_status_write(const edge_status_snapshot *snapshot) {
    if (make_directory("/tmp/edgenode") != 0 && errno != EEXIST)
        return false;
    const char temporary[] = EDGE_STATUS_PATH ".new";
    FILE *output = fopen(temporary, "w");
    if (output == NULL)
        return false;
    const bool rendered = edge_status_render(output, snapshot);
    const bool flushed = rendered && fflush(output) == 0;
    const bool closed = fclose(output) == 0;
    const bool renamed = flushed && closed && rename(temporary, EDGE_STATUS_PATH) == 0;
    if (!renamed)
        edge_unlink(temporary);
    return renamed;
}

void edge_status_remove(void) {
    edge_unlink(EDGE_STATUS_PATH ".new");
    edge_unlink(EDGE_STATUS_PATH);
}
