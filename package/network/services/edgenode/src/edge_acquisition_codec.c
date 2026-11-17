#define _GNU_SOURCE

#include "edge_acquisition_internal.h"

uint8_t modbus_function(const char *type) {
    if (strcmp(type, "COIL") == 0)
        return 1U;
    if (strcmp(type, "DISCRETE_INPUT") == 0)
        return 2U;
    if (strcmp(type, "HOLDING_REGISTER") == 0)
        return 3U;
    if (strcmp(type, "INPUT_REGISTER") == 0)
        return 4U;
    return 0U;
}

static edge_modbus_transport modbus_transport(const edge_acquisition_device *device) {
    return strcmp(device->config->modbus_mode, "RTU") == 0
               ? EDGE_MODBUS_RTU : EDGE_MODBUS_TCP;
}

static edge_s7_area s7_area(const char *area) {
    if (strcmp(area, "DB") == 0 || strcmp(area, "V") == 0)
        return EDGE_S7_AREA_DB;
    if (strcmp(area, "MK") == 0)
        return EDGE_S7_AREA_FLAGS;
    if (strcmp(area, "PE") == 0)
        return EDGE_S7_AREA_INPUTS;
    if (strcmp(area, "PA") == 0)
        return EDGE_S7_AREA_OUTPUTS;
    if (strcmp(area, "CT") == 0)
        return EDGE_S7_AREA_COUNTER;
    return EDGE_S7_AREA_TIMER;
}

static edge_s7_address s7_address(const iot_edge_v1_S7AreaConfig *point) {
    edge_s7_address address = {.area = s7_area(point->area),
                               .db_number = (uint16_t)(strcmp(point->area, "V") == 0
                                                          ? 1U : point->db_number),
                               .start_byte = point->start,
                               .start_bit = (uint8_t)point->start_bit,
                               .size = (uint16_t)point->size,
                               .bit_access = strcmp(point->data_type, "BOOL") == 0};
    return address;
}

void release_response(edge_acquisition_response *response) {
    if (response != NULL && --response->references == 0U) {
        release_response(response->previous);
        free(response);
    }
}

static bool retain_read_response(edge_acquisition_device *device,
                                 const uint8_t *request, size_t request_size,
                                 const uint8_t *bytes, size_t size) {
    if (size == 0U || size > 8192U || request_size > 4096U) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_CAPTURE, 1U);
        return false;
    }
    edge_acquisition_response *response = malloc(sizeof(*response) + size + request_size);
    if (response == NULL) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_CAPTURE, 2U);
        return false;
    }
    response->references = 1U;
    response->previous = NULL;
    response->sequence = ++device->response_sequence;
    response->size = size;
    response->request_size = request_size;
    response->observed_at_ms = device->received_packet_time;
    memcpy(response->packet_id, device->received_packet_id, 16);
    memcpy(response->bytes, bytes, size);
    memcpy(response->bytes + size, request, request_size);
    release_response(device->read_response);
    device->read_response = response;
    return true;
}

static edge_io_result read_industrial_point(edge_acquisition_device *device,
    const iot_edge_v1_IndustrialPointConfig *point, uint8_t *data, size_t capacity, size_t *size) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS &&
        device->industrial_generation != device->link->generation) {
        edge_io_result ready = device_handshake(device);
        if (ready != EDGE_IO_OK) return ready;
    }
    uint8_t request[EDGE_INDUSTRIAL_MAX_FRAME], response[EDGE_INDUSTRIAL_MAX_FRAME];
    size_t wire_size = 0, total = 0;
    for (unsigned sequence = 0; sequence <= 255; ++sequence) {
        size_t request_size = edge_industrial_request(device->config, &device->industrial, point,
            ++device->transaction, (uint8_t)sequence, NULL, 0, request, sizeof(request));
        size_t response_size = 0, count = 0; bool more = false;
        if (!request_size) return EDGE_IO_PROTOCOL_ERROR;
        if (!exchange(device, request, request_size, response, sizeof(response), &response_size)) return EDGE_IO_NO_RESPONSE;
        const bool decoded = edge_industrial_response(
            device->config, request, request_size, response, response_size,
            data + total, capacity - total, &count, &more);
        if (!decoded || response_size > 4096 - wire_size) {
            edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                                 (unsigned)device->config->protocol,
                                 EDGE_LOCAL_FAULT_PROTOCOL_DECODE, decoded ? 2U : 1U);
            debug_request_status(device, "failed", "industrial_read_invalid", true);
            close_fd(&device->link->fd); return EDGE_IO_OFFLINE;
        }
        edge_acquisition_response *previous = sequence ? device->read_response : NULL;
        if (previous) ++previous->references;
        if (!retain_read_response(device, request, request_size, response, response_size)) {
            release_response(previous); return EDGE_IO_PROTOCOL_ERROR;
        }
        device->read_response->previous = previous;
        wire_size += response_size; total += count;
        if (total > edge_industrial_width(point) || (more && total == edge_industrial_width(point))) return EDGE_IO_PROTOCOL_ERROR;
        debug_request_status(device, "success", "", true);
        if (!more) {
            if (total != edge_industrial_width(point)) return EDGE_IO_PROTOCOL_ERROR;
            *size = total; return EDGE_IO_OK;
        }
    }
    return EDGE_IO_PROTOCOL_ERROR;
}

static edge_io_result read_modbus_range(edge_acquisition_device *device,
                                        uint8_t function, uint16_t address,
                                        uint16_t quantity, uint8_t *data,
                                        size_t capacity, size_t *data_size) {
    edge_modbus_request request = {
        .transport = modbus_transport(device),
        .transaction_id = ++device->transaction,
        .unit_id = (uint8_t)device->config->modbus_slave_id,
        .function = function,
        .address = address,
        .quantity = quantity};
    uint8_t output[EDGE_MODBUS_MAX_FRAME];
    uint8_t response[EDGE_MODBUS_MAX_FRAME];
    size_t output_size = 0U;
    size_t response_size = 0U;
    uint8_t exception = 0U;
    if (request.function == 0U ||
        edge_modbus_build_read(&request, output, sizeof(output), &output_size) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return device->link->fd < 0 ? EDGE_IO_OFFLINE : EDGE_IO_NO_RESPONSE;
    const edge_modbus_result parsed = edge_modbus_parse_response(
        &request, response, response_size, NULL, 0U, data, capacity, data_size, &exception);
    debug_request_status(device, parsed == EDGE_MODBUS_OK ? "success" : "failed",
        parsed == EDGE_MODBUS_OK ? "" : "modbus_exception_or_invalid_response", true);
    if (parsed != EDGE_MODBUS_OK) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_PROTOCOL_DECODE,
                             ((unsigned)parsed << 8U) | exception);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    if (!retain_read_response(device, output, output_size, response, response_size))
        return EDGE_IO_PROTOCOL_ERROR;
    return EDGE_IO_OK;
}

static edge_io_result read_modbus_point(edge_acquisition_device *device,
                                        const iot_edge_v1_ModbusRegisterConfig *point,
                                        uint8_t *data, size_t capacity, size_t *data_size) {
    const uint8_t function = modbus_function(point->register_type);
    const edge_io_result result = read_modbus_range(
        device, function, (uint16_t)point->address, (uint16_t)point->quantity,
        data, capacity, data_size);
    if (result == EDGE_IO_OK && (function == 1U || function == 2U) && *data_size != 0U) {
        data[0] = (uint8_t)(data[0] & 1U);
        *data_size = 1U;
    }
    return result;
}

static edge_io_result read_s7_point(edge_acquisition_device *device,
                                    const iot_edge_v1_S7AreaConfig *point,
                                    uint8_t *data, size_t capacity, size_t *data_size) {
    const edge_s7_address address = s7_address(point);
    const uint16_t reference = ++device->s7_reference;
    uint8_t output[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    const size_t output_size = edge_s7_build_read(reference, &address, output, sizeof(output));
    size_t response_size = 0U;
    uint8_t return_code = 0U;
    bool receive_timed_out = false;
    if (output_size == 0U)
        return EDGE_IO_OFFLINE; /* Local request construction failed; nothing was sent. */
    if (!exchange_with_receive_timeout(device, output, output_size, response,
                                       sizeof(response), &response_size,
                                       &receive_timed_out)) {
        /* exchange sets this only after the full read request was written. */
        if (receive_timed_out && device->endpoint->transport ==
                                     iot_edge_v1_Transport_TRANSPORT_ETHERNET)
            return EDGE_IO_NO_RESPONSE;
        return EDGE_IO_OFFLINE;
    }
    const edge_s7_result result = edge_s7_parse_read(response, response_size, reference,
                                                     &address, data, capacity, data_size,
                                                     &return_code);
    debug_request_status(device, result == EDGE_S7_OK ? "success" : "failed",
        result == EDGE_S7_OK ? "" : "s7_exception_or_invalid_response", true);
    if (result != EDGE_S7_OK) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_PROTOCOL_DECODE,
                             ((unsigned)result << 8U) | return_code);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    if (!retain_read_response(device, output, output_size, response, response_size))
        return EDGE_IO_PROTOCOL_ERROR;
    if (address.bit_access && *data_size != 0U) {
        data[0] = data[0] != 0U ? 1U : 0U;
        *data_size = 1U;
    }
    return EDGE_IO_OK;
}

static uint16_t be16(const uint8_t *value) {
    return (uint16_t)(((uint16_t)value[0] << 8U) | value[1]);
}

static uint32_t be32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) | value[3];
}

static uint64_t be64(const uint8_t *value) {
    uint64_t output = 0U;
    for (size_t index = 0U; index < 8U; ++index)
        output = (output << 8U) | value[index];
    return output;
}

static void order_bytes(uint8_t *output, const uint8_t *input, size_t size,
                        const char *order) {
    memcpy(output, input, size);
    if (strcmp(order, "LITTLE_ENDIAN") == 0) {
        for (size_t left = 0U, right = size == 0U ? 0U : size - 1U; left < right;
             ++left, --right) {
            const uint8_t temporary = output[left];
            output[left] = output[right];
            output[right] = temporary;
        }
    } else if (strcmp(order, "BIG_ENDIAN_BYTE_SWAP") == 0) {
        for (size_t index = 0U; index + 1U < size; index += 2U) {
            const uint8_t temporary = output[index];
            output[index] = output[index + 1U];
            output[index + 1U] = temporary;
        }
    } else if (strcmp(order, "LITTLE_ENDIAN_BYTE_SWAP") == 0) {
        const size_t words = size / 2U;
        for (size_t left = 0U; left < words / 2U; ++left) {
            const size_t right = words - 1U - left;
            uint8_t temporary[2] = {output[left * 2U], output[left * 2U + 1U]};
            output[left * 2U] = output[right * 2U];
            output[left * 2U + 1U] = output[right * 2U + 1U];
            output[right * 2U] = temporary[0];
            output[right * 2U + 1U] = temporary[1];
        }
    }
}

static double rounded(double value, double scale, int32_t decimals) {
    value *= scale;
    if (decimals >= 0 && decimals <= 9) {
        double factor = 1.0;
        for (int32_t index = 0; index < decimals; ++index)
            factor *= 10.0;
        value = round(value * factor) / factor;
    }
    return value;
}

static bool decode_scalar(const iot_edge_v1_ConfigItem *item, const uint8_t *raw,
                          size_t size, iot_edge_v1_ScalarValue *value) {
    const char *type = NULL;
    double scale = 1.0;
    int32_t decimals = -1;
    uint8_t ordered[EDGE_DEVICE_VALUE_MAX];
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *point = &item->item.modbus_register;
        type = point->data_type;
        scale = point->scale;
        decimals = point->decimals;
        order_bytes(ordered, raw, size, point->byte_order);
        raw = ordered;
    } else if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag) {
        const iot_edge_v1_IndustrialPointConfig *point = &item->item.industrial_point;
        type = point->data_type;
        if (!strcmp(type, "BCD") || !strcmp(type, "BCD_SIGNED") || !strcmp(type, "HEX")) {
            *value = (iot_edge_v1_ScalarValue)iot_edge_v1_ScalarValue_init_zero;
            if (!strcmp(type, "HEX")) {
                if (size != point->length || size > sizeof(value->value.bytes_value.bytes)) return false;
                value->kind = iot_edge_v1_ValueKind_VALUE_BYTES;
                value->which_value = iot_edge_v1_ScalarValue_bytes_value_tag;
                value->value.bytes_value.size = (pb_size_t)size;
                memcpy(value->value.bytes_value.bytes, raw, size);
                return true;
            }
            value->kind = iot_edge_v1_ValueKind_VALUE_DECIMAL;
            value->which_value = iot_edge_v1_ScalarValue_decimal_value_tag;
            return edge_meter_decode(point, raw, size, value->value.decimal_value, sizeof(value->value.decimal_value));
        }
        scale = point->scale; decimals = point->decimals;
        order_bytes(ordered, raw, size, point->byte_order); raw = ordered;
    } else {
        const iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        type = point->data_type;
        scale = point->scale;
        decimals = point->decimals;
    }
    *value = (iot_edge_v1_ScalarValue)iot_edge_v1_ScalarValue_init_zero;
    if (strcmp(type, "BOOL") == 0 && size >= 1U) {
        scalar_bool(value, raw[0] != 0U);
        return true;
    }
    if ((strcmp(type, "UINT8") == 0 || strcmp(type, "BYTE") == 0) && size >= 1U) {
        scalar_unsigned(value, raw[0]);
        return true;
    }
    if (strcmp(type, "INT8") == 0 && size >= 1U) {
        scalar_signed(value, (int8_t)raw[0]);
        return true;
    }
    uint64_t numeric = size >= 8U ? be64(raw) : size >= 4U ? be32(raw)
                               : size >= 2U ? be16(raw) : 0U;
    if (strcmp(type, "INT16") == 0 && size >= 2U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((int16_t)numeric, scale, decimals));
        else
            scalar_signed(value, (int16_t)numeric);
        return true;
    }
    if ((strcmp(type, "UINT16") == 0 || strcmp(type, "WORD") == 0) && size >= 2U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((uint16_t)numeric, scale, decimals));
        else
            scalar_unsigned(value, (uint16_t)numeric);
        return true;
    }
    if (strcmp(type, "INT32") == 0 && size >= 4U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((int32_t)numeric, scale, decimals));
        else
            scalar_signed(value, (int32_t)numeric);
        return true;
    }
    if ((strcmp(type, "UINT32") == 0 || strcmp(type, "DWORD") == 0) && size >= 4U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((uint32_t)numeric, scale, decimals));
        else
            scalar_unsigned(value, (uint32_t)numeric);
        return true;
    }
    if (strcmp(type, "INT64") == 0 && size >= 8U) {
        scalar_signed(value, (int64_t)numeric);
        return true;
    }
    if (strcmp(type, "UINT64") == 0 && size >= 8U) {
        scalar_unsigned(value, numeric);
        return true;
    }
    if ((strcmp(type, "FLOAT") == 0 || strcmp(type, "FLOAT32") == 0 ||
         strcmp(type, "REAL") == 0) && size >= 4U) {
        const uint32_t bits = (uint32_t)numeric;
        float parsed;
        memcpy(&parsed, &bits, sizeof(parsed));
        scalar_double(value, rounded(parsed, scale, decimals));
        return isfinite(value->value.double_value);
    }
    if ((strcmp(type, "DOUBLE") == 0 || strcmp(type, "LREAL") == 0) && size >= 8U) {
        double parsed;
        memcpy(&parsed, &numeric, sizeof(parsed));
        scalar_double(value, rounded(parsed, scale, decimals));
        return isfinite(value->value.double_value);
    }
    if (strcmp(type, "STRING") == 0 && size != 0U) {
        value->kind = iot_edge_v1_ValueKind_VALUE_STRING;
        value->which_value = iot_edge_v1_ScalarValue_string_value_tag;
        const size_t copy = size < sizeof(value->value.string_value) - 1U
                                ? size : sizeof(value->value.string_value) - 1U;
        memcpy(value->value.string_value, raw, copy);
        value->value.string_value[copy] = '\0';
        return true;
    }
    return false;
}

/* Share the decoded value with debug before history policy or outbox work. */
void publish_debug_value(edge_acquisition_device *device, const uint8_t packet_id[16],
                         const iot_edge_v1_TelemetryValue *value) {
    if (!automatic_debug_enabled(device) ||
        (!device->endpoint->debug_enabled && !device->config->debug_enabled) || !device->owner->debug)
        return;
    iot_edge_v1_RawPacket packet = iot_edge_v1_RawPacket_init_zero;
    packet.debug = true;
    packet.packet_id.size = packet.endpoint_id.size = packet.device_id.size = packet.acquisition_id.size = 16;
    memcpy(packet.packet_id.bytes, packet_id, 16);
    memcpy(packet.endpoint_id.bytes, device->endpoint->endpoint_id.bytes, 16);
    memcpy(packet.device_id.bytes, device->config->device_id.bytes, 16);
    memcpy(packet.acquisition_id.bytes, device->acquisition_id, 16);
    copy_text(packet.direction, sizeof(packet.direction), "RX");
    copy_text(packet.status, sizeof(packet.status), "received");
    packet.observed_at_ms = current_ms();
    packet.has_parsed_value = true;
    packet.parsed_value = *value;
    device->owner->debug(device->owner->callback_context, device->platform_id, &packet);
}

void fill_point_value(edge_acquisition_device *device, edge_acquisition_point *point, const uint8_t *raw, size_t size,
                      edge_acquisition_response *response) {
    if (response != NULL)
        ++response->references;
    release_response(point->response);
    point->response = response;
    if (!device->owner->gateway_enabled && device->raw_telemetry && device->acquisition_active &&
        device->runtime.write_count == 0) {
        point->valid = response != NULL;
        return;
    }
    point->value = (iot_edge_v1_TelemetryValue)iot_edge_v1_TelemetryValue_init_zero;
    if (point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *config = &point->item->item.modbus_register;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    } else if (point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag) {
        const iot_edge_v1_IndustrialPointConfig *config = &point->item->item.industrial_point;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    } else {
        const iot_edge_v1_S7AreaConfig *config = &point->item->item.s7_area;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    }
    point->value.has_value = decode_scalar(point->item, raw, size, &point->value.value);
    /* Raw telemetry validity depends on the exchange, not the optional local decoder. */
    point->valid = device->raw_telemetry && device->acquisition_active && device->runtime.write_count == 0
        ? response != NULL : point->value.has_value;
    publish_gateway_value(device, &point->value, response ? response->observed_at_ms : current_ms());
    if (point->value.has_value && response) publish_debug_value(device, response->packet_id, &point->value);
}

static edge_io_result read_acquisition(void *context, edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    if (!prepare_serial_task(device)) {
        log_io_result(device, EDGE_IO_OFFLINE, "read");
        return EDGE_IO_OFFLINE;
    }
    bool any = false;
    for (size_t index = 0U; index < device->point_count; ++index)
        device->points[index].valid = false;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS) {
        for (size_t group_index = 0U; group_index < device->modbus_group_count;
             ++group_index) {
            const edge_modbus_read_group *group = &device->modbus_groups[group_index];
            uint8_t grouped[EDGE_MODBUS_MAX_FRAME];
            size_t grouped_size = 0U;
            const edge_io_result result = read_modbus_range(
                device, group->function, group->address, group->quantity,
                grouped, sizeof(grouped), &grouped_size);
            if (result != EDGE_IO_OK) {
                log_io_result(device, result, "read");
                if (result == EDGE_IO_NO_RESPONSE &&
                    device->endpoint->transport ==
                        iot_edge_v1_Transport_TRANSPORT_SERIAL) {
                    /* A timed-out RS485 transaction can leave the mt76x8 UART
                     * driver unable to drain its transmit state. Close it now
                     * so the runtime reconnects before the next scan cycle
                     * instead of carrying poisoned state forward. */
                    close_fd(&device->link->fd);
                    return EDGE_IO_OFFLINE;
                }
                return result;
            }
            for (size_t point_index = 0U; point_index < device->modbus_point_count;
                 ++point_index) {
                const edge_modbus_read_point *planned = &device->modbus_points[point_index];
                if (planned->function != group->function ||
                    planned->address < group->address ||
                    (uint32_t)planned->address + planned->quantity >
                        (uint32_t)group->address + group->quantity ||
                    device->points[planned->point_index].valid)
                    continue;
                uint8_t raw[EDGE_DEVICE_VALUE_MAX];
                size_t raw_size = 0U;
                if (!edge_modbus_extract_point(group, planned, grouped, grouped_size,
                                                raw, sizeof(raw), &raw_size)) {
                    edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                                         (unsigned)device->config->protocol,
                                         EDGE_LOCAL_FAULT_PROTOCOL_DECODE, 3U);
                    log_io_result(device, EDGE_IO_PROTOCOL_ERROR, "read");
                    return EDGE_IO_PROTOCOL_ERROR;
                }
                edge_acquisition_point *point = &device->points[planned->point_index];
                fill_point_value(device, point, raw, raw_size, device->read_response);
                if (!point->valid) {
                    log_incomplete_sample(device);
                    log_io_result(device, EDGE_IO_PROTOCOL_ERROR, "read");
                    return EDGE_IO_PROTOCOL_ERROR;
                }
                any = any || point->valid;
            }
        }
        for (size_t index = 0U; index < device->point_count; ++index) {
            if (!device->points[index].valid) {
                log_incomplete_sample(device);
                log_io_result(device, EDGE_IO_PROTOCOL_ERROR, "read");
                return EDGE_IO_PROTOCOL_ERROR;
            }
        }
        log_incomplete_sample(device);
        log_io_result(device, EDGE_IO_OK, "read");
        sample->bytes[0] = any || device->point_count == 0U ? 1U : 0U;
        sample->size = 1U;
        return EDGE_IO_OK;
    }
    for (size_t index = 0U; index < device->point_count; ++index) {
        edge_acquisition_point *point = &device->points[index];
        uint8_t raw[EDGE_DEVICE_VALUE_MAX];
        size_t raw_size = 0U;
        edge_io_result result = point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
            ? read_industrial_point(device, &point->item->item.industrial_point, raw, sizeof(raw), &raw_size)
            : read_s7_point(device, &point->item->item.s7_area, raw, sizeof(raw), &raw_size);
        if (result != EDGE_IO_OK) {
            log_io_result(device, result, "read");
            return result;
        }
        fill_point_value(device, point, raw, raw_size, device->read_response);
        if (!point->valid) {
            log_incomplete_sample(device);
            log_io_result(device, EDGE_IO_PROTOCOL_ERROR, "read");
            return EDGE_IO_PROTOCOL_ERROR;
        }
        any = any || point->valid;
    }
    log_incomplete_sample(device);
    log_io_result(device, EDGE_IO_OK, "read");
    sample->bytes[0] = any || device->point_count == 0U ? 1U : 0U;
    sample->size = 1U;
    return EDGE_IO_OK;
}

edge_acquisition_point *find_point(edge_acquisition_device *device,
                                   const char *element_id) {
    for (size_t index = 0U; index < device->point_count; ++index) {
        const iot_edge_v1_ConfigItem *item = device->points[index].item;
        const char *current = item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
                                  ? item->item.modbus_register.element_id
                                  : item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                                      ? item->item.industrial_point.element_id : item->item.s7_area.element_id;
        if (strcmp(current, element_id) == 0)
            return &device->points[index];
    }
    return NULL;
}

static bool parse_signed(const char *text, int64_t *output) {
    char *end = NULL;
    errno = 0;
    const long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (int64_t)value;
    return true;
}

static bool parse_unsigned(const char *text, uint64_t *output) {
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (uint64_t)value;
    return true;
}

static bool parse_double_value(const char *text, double *output) {
    char *end = NULL;
    errno = 0;
    const double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value))
        return false;
    *output = value;
    return true;
}

static void put_be(uint8_t *output, uint64_t value, size_t size) {
    for (size_t index = 0U; index < size; ++index)
        output[index] = (uint8_t)(value >> ((size - 1U - index) * 8U));
}

static bool encode_scalar(const iot_edge_v1_ConfigItem *item, const char *text,
                          uint8_t *output, size_t capacity, size_t *size) {
    const char *type;
    const char *order = "BIG_ENDIAN";
    size_t width = 0U;
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        type = item->item.modbus_register.data_type;
        order = item->item.modbus_register.byte_order;
        width = (size_t)item->item.modbus_register.quantity * 2U;
        if (strcmp(type, "BOOL") == 0)
            width = 1U;
    } else if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag) {
        const iot_edge_v1_IndustrialPointConfig *point = &item->item.industrial_point;
        type = point->data_type; order = point->byte_order; width = edge_industrial_width(point);
        if (!strcmp(type, "BCD") || !strcmp(type, "BCD_SIGNED") || !strcmp(type, "HEX"))
            return edge_meter_encode(point, text, output, capacity, size);
    } else {
        type = item->item.s7_area.data_type;
        width = item->item.s7_area.size;
        if (strcmp(type, "BOOL") == 0)
            width = 1U;
    }
    if (width == 0U || width > capacity)
        return false;
    uint8_t canonical[EDGE_DEVICE_VALUE_MAX] = {0};
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0U;
    double decimal = 0.0;
    if (strcmp(type, "BOOL") == 0) {
        if (strcmp(text, "0") != 0 && strcmp(text, "1") != 0)
            return false;
        canonical[0] = text[0] == '1' ? 1U : 0U;
    } else if (strcmp(type, "INT8") == 0 || strcmp(type, "INT16") == 0 ||
               strcmp(type, "INT32") == 0 || strcmp(type, "INT64") == 0) {
        if (!parse_signed(text, &signed_value))
            return false;
        if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag && width < 8U &&
            (signed_value < -(INT64_C(1) << (width * 8U - 1U)) ||
             signed_value > (INT64_C(1) << (width * 8U - 1U)) - 1))
            return false;
        put_be(canonical, (uint64_t)signed_value, width);
    } else if (strcmp(type, "UINT8") == 0 || strcmp(type, "BYTE") == 0 ||
               strcmp(type, "UINT16") == 0 || strcmp(type, "WORD") == 0 ||
               strcmp(type, "UINT32") == 0 || strcmp(type, "DWORD") == 0 ||
               strcmp(type, "UINT64") == 0) {
        if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag &&
            (text[0] < '0' || text[0] > '9'))
            return false;
        if (!parse_unsigned(text, &unsigned_value))
            return false;
        if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag && width < 8U &&
            unsigned_value >= (UINT64_C(1) << (width * 8U)))
            return false;
        put_be(canonical, unsigned_value, width);
    } else if (strcmp(type, "FLOAT") == 0 || strcmp(type, "FLOAT32") == 0 ||
               strcmp(type, "REAL") == 0) {
        if (width != 4U || !parse_double_value(text, &decimal))
            return false;
        const float narrowed = (float)decimal;
        if (!isfinite(narrowed))
            return false;
        uint32_t bits;
        memcpy(&bits, &narrowed, sizeof(bits));
        put_be(canonical, bits, width);
    } else if (strcmp(type, "DOUBLE") == 0 || strcmp(type, "LREAL") == 0) {
        if (width != 8U || !parse_double_value(text, &decimal))
            return false;
        uint64_t bits;
        memcpy(&bits, &decimal, sizeof(bits));
        put_be(canonical, bits, width);
    } else if (strcmp(type, "STRING") == 0) {
        const size_t length = strlen(text);
        if (length > width)
            return false;
        memcpy(canonical, text, length);
    } else {
        return false;
    }
    order_bytes(output, canonical, width, order);
    *size = width;
    return true;
}

static edge_io_result write_modbus(edge_acquisition_device *device,
                                   const iot_edge_v1_ModbusRegisterConfig *point,
                                   const edge_write_command *command,
                                   edge_device_sample *actual) {
    const uint8_t read_function = modbus_function(point->register_type);
    edge_modbus_request request = {
        .transport = modbus_transport(device),
        .transaction_id = ++device->transaction,
        .unit_id = (uint8_t)device->config->modbus_slave_id,
        .function = read_function == 1U ? 5U : point->quantity == 1U ? 6U : 16U,
        .address = (uint16_t)point->address,
        .quantity = (uint16_t)point->quantity};
    uint8_t output[EDGE_MODBUS_MAX_FRAME];
    uint8_t response[EDGE_MODBUS_MAX_FRAME];
    size_t output_size = 0U;
    size_t response_size = 0U;
    size_t ignored = 0U;
    uint8_t exception = 0U;
    if (edge_modbus_build_write(&request, command->value, command->value_size,
                                output, sizeof(output), &output_size) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!write_command_exchange(device, output, output_size, response,
                                sizeof(response), &response_size))
        return failed_command_exchange(device);
    if (edge_modbus_parse_response(&request, response, response_size, command->value,
                                   command->value_size, NULL, 0U, &ignored,
                                   &exception) != EDGE_MODBUS_OK) {
        debug_request_status(device, "failed", "modbus_write_exception", true);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    device->runtime.write_ack_received = true;
    debug_request_status(device, "success", "", true);
    return read_modbus_point(device, point, actual->bytes, sizeof(actual->bytes),
                             &actual->size);
}

static edge_io_result write_s7(edge_acquisition_device *device,
                               const iot_edge_v1_S7AreaConfig *point,
                               const edge_write_command *command,
                               edge_device_sample *actual) {
    const edge_s7_address address = s7_address(point);
    const uint16_t reference = ++device->s7_reference;
    uint8_t output[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    const size_t output_size = edge_s7_build_write(reference, &address, command->value,
                                                   command->value_size, output,
                                                   sizeof(output));
    size_t response_size = 0U;
    uint8_t return_code = 0U;
    if (output_size == 0U)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!write_command_exchange(device, output, output_size, response,
                                sizeof(response), &response_size))
        return failed_command_exchange(device);
    if (edge_s7_parse_write(response, response_size, reference, &return_code) != EDGE_S7_OK) {
        debug_request_status(device, "failed", "s7_write_exception", true);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    device->runtime.write_ack_received = true;
    debug_request_status(device, "success", "", true);
    return read_s7_point(device, point, actual->bytes, sizeof(actual->bytes), &actual->size);
}

static edge_io_result write_industrial(edge_acquisition_device *device,
    const iot_edge_v1_IndustrialPointConfig *point, const edge_write_command *command, edge_device_sample *actual) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS &&
        device->industrial_generation != device->link->generation) {
        edge_io_result ready = device_handshake(device);
        if (ready != EDGE_IO_OK) return ready;
    }
    uint8_t request[EDGE_INDUSTRIAL_MAX_FRAME], response[EDGE_INDUSTRIAL_MAX_FRAME];
    size_t n = edge_industrial_request(device->config, &device->industrial, point, ++device->transaction,
        0, command->value, command->value_size, request, sizeof(request));
    size_t received = 0, ignored = 0; bool more = false;
    if (!n) return EDGE_IO_PROTOCOL_ERROR;
    if (!write_command_exchange(device, request, n, response, sizeof(response),
                                &received))
        return failed_command_exchange(device);
    if (!edge_industrial_response(device->config, request, n, response, received, NULL, 0, &ignored, &more)) {
        debug_request_status(device, "failed", "industrial_write_invalid", true);
        close_fd(&device->link->fd); return EDGE_IO_OFFLINE;
    }
    device->runtime.write_ack_received = true;
    debug_request_status(device, "success", "", true);
    return read_industrial_point(device, point, actual->bytes, sizeof(actual->bytes), &actual->size);
}

static edge_io_result write_acquisition(void *context,
                                        const edge_write_command *command,
                                        edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    if (!prepare_serial_task(device)) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_NETWORK, 8U);
        return EDGE_IO_OFFLINE;
    }
    edge_acquisition_point *point = find_point(device, command->element_id);
    if (point == NULL) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_PROTOCOL_DECODE, 7U);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    const edge_io_result result = point->item->which_item ==
                                          iot_edge_v1_ConfigItem_modbus_register_tag
                                      ? write_modbus(device,
                                            &point->item->item.modbus_register,
                                            command, actual)
                                      : point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                                          ? write_industrial(device, &point->item->item.industrial_point, command, actual)
                                      : write_s7(device, &point->item->item.s7_area,
                                            command, actual);
    if (result == EDGE_IO_OK)
        fill_point_value(device, point, actual->bytes, actual->size, device->read_response);
    return result;
}

static void update_derived_samples(edge_acquisition_device *device) {
    if (!device->derived || (device->raw_telemetry && !device->owner->gateway_enabled)) return;
    iot_edge_v1_TelemetryValue *values = calloc(device->point_count ? device->point_count : 1, sizeof(*values));
    if (!values) {
        syslog(LOG_ERR, "cannot allocate derived input snapshot");
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_CAPTURE, 3U);
        return;
    }
    size_t count = 0;
    for (size_t i = 0; i < device->point_count; ++i) if (device->points[i].valid) values[count++] = device->points[i].value;
    const int64_t now = current_ms();
    (void)update_derived(device, values, count, now);
    free(values);
}

edge_io_result device_read(void *context, edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    record_id((uint64_t)current_ms(), device->acquisition_id);
    device->acquisition_active = true;
    debug_acquisition_state(device, "running");
    for (size_t index = 0; index < device->point_count; ++index) device->points[index].valid = false;
    const edge_io_result result = read_acquisition(context, sample);
    if (result != EDGE_IO_OK) {
        for (size_t index = 0; index < device->point_count; ++index)
            device->points[index].valid = false;
    } else {
        update_derived_samples(device);
    }
    for (size_t index = 0; device->owner->gateway_enabled && index < device->point_count; ++index) {
        edge_acquisition_point *point = &device->points[index];
        if (point->valid) continue;
        iot_edge_v1_TelemetryValue invalid = iot_edge_v1_TelemetryValue_init_zero;
        const char *element = point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
            ? point->item->item.modbus_register.element_id
            : point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                ? point->item->item.industrial_point.element_id : point->item->item.s7_area.element_id;
        copy_text(invalid.element_id, sizeof(invalid.element_id), element);
        publish_gateway_value(device, &invalid, current_ms());
    }
    finish_debug_acquisition(device, result);
    device->acquisition_active = false;
    return result;
}

edge_io_result device_write_readback(void *context, const edge_write_command *command,
                                     edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    memcpy(device->acquisition_id, command->command_id, 16);
    device->acquisition_active = true;
    device->command_start_before_monotonic_ms = command->start_before_monotonic_ms;
    device->mapping_valid_until_monotonic_ms = command->mapping_valid_until_monotonic_ms;
    device->command_clock_generation = command->command_clock_generation;
    device->command_start_expired = false;
    device->command_start_check = false;
    debug_acquisition_state(device, "running");
    for (size_t index = 0; index < device->point_count; ++index) device->points[index].valid = false;
    const edge_io_result result = write_acquisition(context, command, actual);
    update_derived_samples(device);
    finish_debug_acquisition(device, result);
    device->acquisition_active = false;
    return result;
}

static bool writable_point(const edge_acquisition_point *point) {
    return point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
               ? point->item->item.modbus_register.writable
               : point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                   ? point->item->item.industrial_point.writable : point->item->item.s7_area.writable;
}

bool build_write_command(edge_acquisition *acquisition,
                         const uint8_t platform_id[16],
                         const iot_edge_v1_CommandRequest *request,
                         uint64_t start_before_monotonic_ms,
                         uint64_t mapping_valid_until_monotonic_ms,
                         uint32_t command_clock_generation,
                         edge_acquisition_device **output_device,
                         edge_write_command *output_command,
                         char *error, size_t error_size) {
    if (acquisition == NULL || platform_id == NULL || request == NULL ||
        request->command_id.size != 16U ||
        request->device_id.size != 16U || request->values_count != 1U) {
        set_error(error, error_size, "edge command requires one device and one value");
        return false;
    }
    edge_acquisition_device *device =
        find_device(acquisition, platform_id, request->device_id.bytes);
    if (device == NULL) {
        set_error(error, error_size, "edge device is not active in this platform config");
        return false;
    }
    const iot_edge_v1_CommandValue *value = &request->values[0];
    edge_acquisition_point *point = find_point(device, value->element_id);
    if (point == NULL || !writable_point(point)) {
        set_error(error, error_size, "command element is missing or not writable");
        return false;
    }
    if (!value->has_expected ||
        value->expected.which_value != iot_edge_v1_ScalarValue_string_value_tag) {
        set_error(error, error_size, "command value must be a string scalar");
        return false;
    }
    edge_write_command command = {0};
    memcpy(command.command_id, request->command_id.bytes, 16U);
    copy_text(command.element_id, sizeof(command.element_id), value->element_id);
    command.fast_read_duration_sec = request->fast_read_duration_sec != 0U
                                         ? request->fast_read_duration_sec
                                         : device->config->command_fast_read_duration_sec;
    command.fast_read_interval_sec = request->fast_read_interval_sec != 0U
                                         ? request->fast_read_interval_sec
                                         : device->config->command_fast_read_interval_sec;
    command.start_before_monotonic_ms = start_before_monotonic_ms;
    command.mapping_valid_until_monotonic_ms = mapping_valid_until_monotonic_ms;
    command.command_clock_generation = command_clock_generation;
    if (command.fast_read_duration_sec > 3600U ||
        command.fast_read_interval_sec > 3600U) {
        set_error(error, error_size, "command fast-read policy is invalid");
        return false;
    }
    if (command.fast_read_duration_sec != 0U && command.fast_read_interval_sec == 0U)
        command.fast_read_interval_sec = 1U;
    if (!encode_scalar(point->item, value->expected.value.string_value,
                       command.value, sizeof(command.value), &command.value_size)) {
        set_error(error, error_size, "command value cannot be encoded for the configured type");
        return false;
    }
    *output_device = device;
    *output_command = command;
    return true;
}
