#define _GNU_SOURCE

#include "edge_acquisition_internal.h"

static bool worker_sl651_report(edge_acquisition_device *device,
                                const iot_edge_v1_TelemetryRecord *record, uint32_t part) {
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_EVENT_SL651, 0U);
    memcpy(message.platform_id, device->platform_id, 16);
    memcpy(message.device_id, device->config->device_id.bytes, 16);
    message.report_token = device->sl651_token;
    message.report_part = part;
    pb_ostream_t stream =
        pb_ostream_from_buffer(message.payload.telemetry, sizeof(message.payload.telemetry));
    iot_edge_v1_TelemetryRecord stamped = *record;
    if (device->raw_telemetry) {
        stamped.configuration_sha256.size = 32U;
        memcpy(stamped.configuration_sha256.bytes, device->configuration_digest, 32U);
    }
    if (!pb_encode(&stream, iot_edge_v1_TelemetryRecord_fields, &stamped)) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_WORKER_ENCODE, 1U);
        return false;
    }
    message.payload_size = (uint32_t)stream.bytes_written;
    const size_t size = acquisition_message_size(message.payload_size);
    const bool sent = send(device->owner->worker_fd, &message, size,
                           MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)size;
    if (!sent)
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_WORKER_SEND, 1U);
    return sent;
}

static int sl651_timezone(const edge_acquisition_device *device) {
    const char *zone = device->config->timezone;
    unsigned hours = 8, minutes = 0;
    if (strlen(zone) != 6 || (zone[0] != '+' && zone[0] != '-') ||
        sscanf(zone + 1, "%2u:%2u", &hours, &minutes) != 2 || hours > 23 || minutes > 59)
        return 8 * 3600;
    return (zone[0] == '-' ? -1 : 1) * (int)(hours * 3600 + minutes * 60);
}

void sl651_time(edge_acquisition_device *device, uint8_t output[6]) {
    time_t now = (time_t)(current_ms() / 1000 + sl651_timezone(device));
    struct tm local;
    gmtime_r(&now, &local);
    unsigned values[6] = {(unsigned)(local.tm_year + 1900) % 100,
                          (unsigned)local.tm_mon + 1,
                          (unsigned)local.tm_mday,
                          (unsigned)local.tm_hour,
                          (unsigned)local.tm_min,
                          (unsigned)local.tm_sec};
    for (size_t i = 0; i < 6; ++i)
        output[i] = (uint8_t)((values[i] / 10) * 16 + values[i] % 10);
}

static int64_t sl651_observed(edge_acquisition_device *device, const uint8_t *body) {
    unsigned values[6];
    for (size_t i = 0; i < 6; ++i)
        values[i] = (body[i + 2] >> 4U) * 10U + (body[i + 2] & 15U);
    struct tm local = {.tm_year = (int)values[0] + 100,
                       .tm_mon = (int)values[1] - 1,
                       .tm_mday = (int)values[2],
                       .tm_hour = (int)values[3],
                       .tm_min = (int)values[4],
                       .tm_sec = (int)values[5]};
    return ((int64_t)timegm(&local) - sl651_timezone(device)) * 1000;
}

bool sl651_send(void *context, const uint8_t *bytes, size_t size,
                const uint8_t acquisition_id[16], const uint8_t *reply_to_packet_id) {
    edge_acquisition_device *device = context;
    const bool control_request = device->sl651_command_pending &&
        reply_to_packet_id == NULL &&
        memcmp(acquisition_id, device->sl651_command_id, 16U) == 0 &&
        edge_sl651_is_control_request(bytes, size);
    if (control_request && device->runtime.southbound_write_started)
        return false; /* Never retransmit an already-started control request. */

    if (size >= 17 && size <= 4096 && (bytes[11] & 0x80U) && bytes[size - 3] == 0x05) {
        uint8_t *request = malloc(size);
        if (!request) return false;
        memcpy(request, bytes, size);
        free(device->sl651_request);
        device->sl651_request = request;
        device->sl651_request_size = size;
    }
    if (serial_paused(device)) return false;
    memcpy(device->acquisition_id, acquisition_id, 16);
    if (reply_to_packet_id) memcpy(device->received_packet_id, reply_to_packet_id, 16);
    else memset(device->received_packet_id, 0, 16);
    if (!device->sl651_transmitter)
        return true;
    if (device->link->fd < 0)
        return false;
    size_t offset = 0;
    while (offset < size) {
        const bool guarded_first_write = control_request &&
            !device->runtime.southbound_write_started;
        /* The generation can change after this check; never claim atomicity with write(). */
        if (guarded_first_write && !command_first_write_is_valid(device))
            return false;
        const ssize_t n = write(device->link->fd, bytes + offset, size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        if (guarded_first_write) {
            device->runtime.southbound_write_started = true;
            memcpy(device->southbound_command_id, device->sl651_command_id, 16U);
            device->southbound_command_id_valid = true;
        }
        debug_packet(device, "TX", bytes + offset, (size_t)n, true, false);
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
            serial_observe(device->owner, device->endpoint->serial.channel,
                &device->endpoint->serial, "TX", bytes + offset, (size_t)n);
        offset += (size_t)n;
    }
    return true;
}

void sl651_trace(void *context, const uint8_t *bytes, size_t size,
                 uint8_t packet_id[16], uint8_t acquisition_id[16]) {
    edge_acquisition_device *device = context;
    static const uint8_t empty[16] = {0};
    if (!memcmp(acquisition_id, empty, 16)) record_id((uint64_t)current_ms(), acquisition_id);
    memcpy(device->acquisition_id, acquisition_id, 16);
    record_id((uint64_t)current_ms(), packet_id);
    memcpy(device->received_packet_id, packet_id, 16);
    debug_packet_with_id(device, packet_id, "RX", bytes, size, true, false);
}

static bool sl651_value(const iot_edge_v1_Sl651ElementConfig *element, const uint8_t *bytes,
                        size_t size, iot_edge_v1_ScalarValue *value) {
    if (!strcmp(element->encoding, "BCD")) {
        double decoded;
        if (!edge_sl651_bcd(bytes, size, element->digits, &decoded))
            return false;
        scalar_double(value, decoded);
        return true;
    }
    value->kind = iot_edge_v1_ValueKind_VALUE_STRING;
    value->which_value = iot_edge_v1_ScalarValue_string_value_tag;
    char *text = value->value.string_value;
    if (!strcmp(element->encoding, "TIME_YYMMDDHHMMSS")) {
        if (size != 5 && size != 6)
            return false;
        for (size_t i = 0; i < size; ++i)
            if ((bytes[i] & 15) > 9 || (bytes[i] >> 4) > 9)
                return false;
        snprintf(text, sizeof(value->value.string_value), "20%02X-%02X-%02XT%02X:%02X:%02X",
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], size == 6 ? bytes[5] : 0);
        return true;
    }
    if (size * 2 >= sizeof(value->value.string_value)) {
        if (strcmp(element->encoding, "HEX") || size > sizeof(value->value.bytes_value.bytes))
            return false;
        value->kind = iot_edge_v1_ValueKind_VALUE_BYTES;
        value->which_value = iot_edge_v1_ScalarValue_bytes_value_tag;
        value->value.bytes_value.size = (pb_size_t)size;
        memcpy(value->value.bytes_value.bytes, bytes, size);
        return true;
    }
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) {
        text[i * 2] = hex[bytes[i] >> 4];
        text[i * 2 + 1] = hex[bytes[i] & 15];
    }
    text[size * 2] = 0;
    return true;
}

void sl651_diagnostic(void *context, edge_sl651_fault_reason reason) {
    edge_acquisition_device *device = context;
    edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                         (unsigned)device->config->protocol,
                         reason == EDGE_SL651_FAULT_FRAME ? EDGE_LOCAL_FAULT_SL651_FRAME :
                         reason == EDGE_SL651_FAULT_OVERFLOW ? EDGE_LOCAL_FAULT_SL651_BUFFER :
                         EDGE_LOCAL_FAULT_SL651_REPORT, (unsigned)reason);
}

bool sl651_report(void *context, uint64_t token, const edge_sl651_frame *frame) {
    edge_acquisition_device *device = context;
    if (device->sl651_token != token) {
        device->sl651_report_encoded = false;
        device->sl651_report_raw = device->raw_telemetry;
    }
    const bool raw_report = device->sl651_report_raw;
    char function[3];
    snprintf(function, sizeof(function), "%02X", frame->function);
    bool response = false;
    for (size_t i = 0; i < device->point_count; ++i) {
        const iot_edge_v1_Sl651ElementConfig *element = &device->points[i].item->item.sl651_element;
        if (!strcmp(function, element->function_code) && element->response_element)
            response = true;
    }
    size_t guide_start = 8;
    for (size_t i = 0; i < device->point_count; ++i) {
        const iot_edge_v1_Sl651ElementConfig *element = &device->points[i].item->item.sl651_element;
        if (!strcmp(function, element->function_code) && element->response_element == response &&
            element->fixed_position && element->length &&
            element->byte_offset <= frame->body_size &&
            element->length <= frame->body_size - element->byte_offset &&
            guide_start < element->byte_offset + element->length)
            guide_start = element->byte_offset + element->length;
    }
    iot_edge_v1_TelemetryValue *values =
        calloc((device->point_count ? device->point_count : 1) + edge_derived_count(device->derived), sizeof(*values));
    if (!values) {
        log_report_result(device, EDGE_LOCAL_REPORT_ALLOCATION, 0U);
        return false;
    }
    size_t count = 0, binary_size = 0;
    if (!raw_report || device->owner->gateway_enabled) {
        for (size_t i = 0; i < device->point_count; ++i) {
            const iot_edge_v1_Sl651ElementConfig *element = &device->points[i].item->item.sl651_element;
            if (strcmp(function, element->function_code) || element->response_element != response)
                continue;
            const uint8_t *bytes;
            size_t size;
            if (!edge_sl651_field(frame->body, frame->body_size, element->fixed_position,
                                  element->fixed_position ? element->byte_offset : guide_start,
                                  element->guide.bytes, element->guide.size, element->length, &bytes,
                                  &size))
                continue;
            iot_edge_v1_TelemetryValue *value = &values[count];
            bool binary =
                !strcmp(element->encoding, "JPEG") ||
                ((!strcmp(element->encoding, "HEX") || !strcmp(element->encoding, "DICT")) && size > 63);
            if (raw_report && binary) continue;
            bool decoded = false;
            if (binary && size <= 8192 && size <= EDGE_SL651_BODY_MAX - binary_size) {
                value->encoded_value = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(size));
                if (value->encoded_value) {
                    value->encoded_value->size = (pb_size_t)size;
                    memcpy(value->encoded_value->bytes, bytes, size);
                    copy_text(value->encoding, sizeof(value->encoding), element->encoding);
                    binary_size += size;
                    decoded = true;
                }
            } else if (!binary)
                decoded = sl651_value(element, bytes, size, &value->value);
            if (!decoded && raw_report) {
                copy_text(value->element_id, sizeof(value->element_id), element->element_id);
                publish_gateway_value(device, value, sl651_observed(device, frame->body));
                free(value->encoded_value);
                memset(value, 0, sizeof(*value));
                continue;
            }
            if (!decoded) {
                for (size_t j = 0; j < count; ++j)
                    free(values[j].encoded_value);
                free(values);
                copy_text(device->last_error, sizeof(device->last_error),
                          "SL651 value exceeds telemetry encoding or is invalid");
                log_report_result(device, EDGE_LOCAL_REPORT_INVALID, 0U);
                return false;
            }
            copy_text(value->element_id, sizeof(value->element_id), element->element_id);
            copy_text(value->name, sizeof(value->name), element->name);
            copy_text(value->unit, sizeof(value->unit), element->unit);
            value->has_value = !binary;
            ++count;
        }
        /* Multipart reports become decodable on the final received frame. */
        const size_t raw_count = edge_sl651_report_frame_count(device->sl651);
        const uint8_t *parsed_packet = raw_count ? edge_sl651_report_packet_id(device->sl651, raw_count - 1) : frame->packet_id;
        if (parsed_packet) for (size_t index = 0; index < count; ++index)
            publish_debug_value(device, parsed_packet, &values[index]);
        for (size_t index = 0; index < count; ++index) values[index].sample_time_ms = sl651_observed(device, frame->body);
        for (size_t index = 0; index < count; ++index)
            publish_gateway_value(device, &values[index], values[index].sample_time_ms);
        const bool derived_enabled = !raw_report || device->owner->gateway_enabled;
        if (derived_enabled)
            (void)update_derived(device, values, count, current_ms());
        if (raw_report) {
            for (size_t index = 0; index < count; ++index) free(values[index].encoded_value);
            memset(values, 0, count * sizeof(*values));
            count = 0;
        }
        if (!raw_report) {
            edge_derived_values(device->derived, values + count);
            count += edge_derived_count(device->derived);
        }
    }
    size_t starts[129] = {0}, parts = 0;
    for (size_t index = 0; index < count;) {
        if (parts == 128) {
            for (size_t j = 0; j < count; ++j)
                free(values[j].encoded_value);
            free(values);
            log_report_result(device, EDGE_LOCAL_REPORT_FRAME_LIMIT, 0U);
            return false;
        }
        starts[parts++] = index;
        size_t bytes = 0, first = index;
        while (index < count && index - first < 8) {
            size_t estimated =
                512 + (values[index].encoded_value ? values[index].encoded_value->size : 0);
            if (bytes + estimated > 12288)
                break;
            bytes += estimated;
            ++index;
        }
        starts[parts] = index;
    }
    const size_t value_parts = parts;
    const size_t frame_count = edge_sl651_report_frame_count(device->sl651);
    pb_bytes_array_t **raw_frames = calloc(frame_count, sizeof(*raw_frames));
    pb_bytes_array_t **raw_ids = calloc(frame_count, sizeof(*raw_ids));
    pb_bytes_array_t **raw_requests = raw_report
        ? calloc(frame_count, sizeof(*raw_requests)) : NULL;
    bool queued = false;
    edge_local_report_result result = frame_count == 0U
        ? EDGE_LOCAL_REPORT_EMPTY : EDGE_LOCAL_REPORT_ALLOCATION;
    size_t raw_starts[257] = {0}, raw_parts = 0;
    if (frame_count == 0 || raw_frames == NULL || raw_ids == NULL ||
        (raw_report && !raw_requests))
        goto report_cleanup;
    size_t raw_bytes = 0;
    for (size_t index = 0; index < frame_count; ++index) {
        const uint8_t *bytes;
        size_t size;
        if (!edge_sl651_report_frame(device->sl651, index, &bytes, &size)) {
            result = EDGE_LOCAL_REPORT_INVALID;
            goto report_cleanup;
        }
        const size_t request_size = raw_report && index == 0 && device->sl651_query_active
            ? device->sl651_request_size : 0;
        if (raw_bytes && raw_bytes + size + request_size + 32 > 12000) {
            raw_starts[++raw_parts] = index;
            raw_bytes = 0;
        }
        if (raw_parts + value_parts >= 256) {
            result = EDGE_LOCAL_REPORT_FRAME_LIMIT;
            goto report_cleanup;
        }
        raw_frames[index] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(size));
        if (raw_frames[index] == NULL)
            goto report_cleanup;
        raw_ids[index] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(16));
        if (raw_ids[index] == NULL) goto report_cleanup;
        raw_ids[index]->size = 16;
        memcpy(raw_ids[index]->bytes, edge_sl651_report_packet_id(device->sl651, index), 16);
        raw_frames[index]->size = (pb_size_t)size;
        memcpy(raw_frames[index]->bytes, bytes, size);
        if (raw_report) {
            raw_requests[index] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(request_size));
            if (!raw_requests[index]) goto report_cleanup;
            raw_requests[index]->size = (pb_size_t)request_size;
            if (request_size) memcpy(raw_requests[index]->bytes, device->sl651_request, request_size);
        }
        raw_bytes += size + request_size + 32;
    }
    raw_starts[++raw_parts] = frame_count;
    parts = value_parts + raw_parts;
    if (device->sl651_token != token) {
        device->sl651_token = token;
        device->sl651_parts = (uint32_t)parts;
        memset(device->sl651_committed, 0, sizeof(device->sl651_committed));
        for (size_t i = 0; i < parts; ++i)
            record_id((uint64_t)current_ms(), device->sl651_record_ids[i]);
    }
    device->sl651_report_encoded = true;
    queued = true;
    result = EDGE_LOCAL_REPORT_QUEUED;
    device->observed_at_ms = sl651_observed(device, frame->body);
    device->last_activity_at_ms = current_ms();
    for (size_t part = 0; part < parts; ++part) {
        if (device->sl651_committed[part])
            continue;
        iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
        edge_protocol_set_bytes(&record.record_id, sizeof(record.record_id.bytes),
                                device->sl651_record_ids[part], 16);
        edge_protocol_set_bytes(&record.report_id, sizeof(record.report_id.bytes),
                                frame->acquisition_id, 16);
        record.part_index = (uint32_t)part;
        record.part_count = (uint32_t)parts;
        edge_protocol_set_bytes(&record.device_id, sizeof(record.device_id.bytes),
                                device->config->device_id.bytes, 16);
        edge_protocol_set_bytes(&record.endpoint_id, sizeof(record.endpoint_id.bytes),
                                device->config->endpoint_id.bytes, 16);
        record.protocol = iot_edge_v1_Protocol_PROTOCOL_SL651;
        record.observed_at_ms = device->observed_at_ms;
        copy_text(record.function_code, sizeof(record.function_code), function);
        copy_text(record.direction, sizeof(record.direction), "UP");
        if (part < value_parts) {
            record.values = values + starts[part];
            record.values_count = (pb_size_t)(starts[part + 1] - starts[part]);
        } else {
            size_t raw_part = part - value_parts;
            record.raw_packet_ids = raw_ids + raw_starts[raw_part];
            record.raw_packet_ids_count = (pb_size_t)(raw_starts[raw_part + 1] - raw_starts[raw_part]);
            record.raw_payloads = raw_frames + raw_starts[raw_part];
            if (raw_report) {
                record.raw_requests = raw_requests + raw_starts[raw_part];
                record.raw_requests_count = (pb_size_t)(raw_starts[raw_part + 1] - raw_starts[raw_part]);
            }
            record.raw_payloads_count = (pb_size_t)(raw_starts[raw_part + 1] - raw_starts[raw_part]);
        }
        if (!worker_sl651_report(device, &record, (uint32_t)part)) {
            queued = false;
            result = EDGE_LOCAL_REPORT_ENQUEUE;
        }
    }
report_cleanup:
    if (raw_requests) { for (size_t i = 0; i < frame_count; ++i) free(raw_requests[i]); free(raw_requests); }
    if (raw_ids != NULL) { for (size_t i = 0; i < frame_count; ++i) free(raw_ids[i]); free(raw_ids); }
    if (raw_frames != NULL) {
        for (size_t index = 0; index < frame_count; ++index)
            free(raw_frames[index]);
        free(raw_frames);
    }
    for (size_t j = 0; j < count; ++j)
        free(values[j].encoded_value);
    free(values);
    if (queued) {
        memcpy(device->acquisition_id, frame->acquisition_id, 16);
        finish_debug_acquisition(device, EDGE_IO_OK);
    }
    log_report_result(device, result, frame_count);
    return queued;
}

void sl651_command_result(void *context, const uint8_t id[16], bool success,
                          const char *reason) {
    edge_acquisition_device *device = context;
    device->sl651_result = (iot_edge_v1_CommandResult)iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&device->sl651_result.command_id,
                            sizeof(device->sl651_result.command_id.bytes), id, 16);
    edge_protocol_set_bytes(&device->sl651_result.device_id,
                            sizeof(device->sl651_result.device_id.bytes),
                            device->config->device_id.bytes, 16);
    device->sl651_result.state = success ? iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED
                                         : iot_edge_v1_CommandState_COMMAND_STATE_FAILED;
    device->runtime.write_ack_received = success;
    device->sl651_result.write_ack_missing =
        device->runtime.southbound_write_started && !device->runtime.write_ack_received;
    device->sl651_result.completed_at_ms = current_ms();
    copy_text(device->sl651_result.message, sizeof(device->sl651_result.message), reason);
    if (!success)
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_COMMAND,
                             (device->runtime.southbound_write_started ? 2U : 0U) |
                             (device->runtime.write_ack_received ? 1U : 0U));
    if (!success)
        for (size_t i = 0; i < device->owner->device_count; ++i) {
            edge_acquisition_device *other = &device->owner->devices[i];
            if (other->sl651 && other->link == device->link &&
                !memcmp(other->sl651_station, device->sl651_station, 5))
                edge_sl651_quarantine(other->sl651);
        }
    device->sl651_result_pending = true;
    (void)device->owner->command(device->owner->callback_context, device->platform_id,
                                 &device->sl651_result);
}

void sl651_poll(edge_acquisition_device *device, uint64_t now) {
    uint8_t time[6];
    sl651_time(device, time);
    edge_sl651_tick(device->sl651, now, time);
    if (device->sl651_result_pending)
        (void)device->owner->command(device->owner->callback_context, device->platform_id,
                                     &device->sl651_result);
    /* A shared physical stream is read once and broadcast to its configured station sessions. */
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (other == device)
            break;
        if (other->sl651 && other->link == device->link)
            return;
    }
    for (size_t i = 0; i < device->owner->device_count; ++i)
        if (device->owner->devices[i].link == device->link && device->owner->devices[i].sl651 &&
            !edge_sl651_ready(device->owner->devices[i].sl651))
            return;
    if (device_connect(device) != EDGE_IO_OK)
        return;
    struct pollfd descriptor = {.fd = device->link->fd, .events = POLLIN};
    if (poll(&descriptor, 1, 0) <= 0)
        return;
    uint8_t bytes[4096];
    ssize_t n = read(device->link->fd, bytes, sizeof(bytes));
    if (n > 0 && device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        serial_observe(device->owner, device->endpoint->serial.channel,
            &device->endpoint->serial, "RX", bytes, (size_t)n);
    if (n <= 0) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_NETWORK, 4U);
        close_fd(&device->link->fd);
        for (size_t i = 0; i < device->owner->device_count; ++i)
            if (device->owner->devices[i].sl651 && device->owner->devices[i].link == device->link)
                edge_sl651_reset(device->owner->devices[i].sl651);
        return;
    }
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (!other->sl651 || other->link != device->link)
            continue;
        bool first = true;
        for (size_t j = 0; j < i; ++j) {
            edge_acquisition_device *previous = &device->owner->devices[j];
            if (previous->link == other->link && !memcmp(previous->platform_id, other->platform_id, 16)) first = false;
        }
        bool framed = (size_t)n >= 17 && bytes[0] == 0x7e && bytes[1] == 0x7e &&
            ((((size_t)bytes[11] & 15U) << 8U) + bytes[12] + 17U == (size_t)n);
        bool identified = false;
        if (framed) for (size_t j = 0; j < device->owner->device_count; ++j) {
            const edge_acquisition_device *candidate = &device->owner->devices[j];
            if (candidate->sl651 && candidate->link == other->link &&
                !memcmp(candidate->platform_id, other->platform_id, 16) &&
                !memcmp(candidate->sl651_station, bytes + 3, 5)) identified = true;
        }
        if (first && other->endpoint->debug_enabled && !identified)
            debug_packet(other, "RX", bytes, (size_t)n, false, false);
        sl651_time(other, time);
        edge_sl651_receive(other->sl651, bytes, (size_t)n, now, time);
    }
}

static bool sl651_encode(const iot_edge_v1_Sl651ElementConfig *element, const char *text,
                         uint8_t *out, size_t cap) {
    size_t length = element->length;
    if (!length || length > cap)
        return false;
    memset(out, 0, length);
    if (!strcmp(element->encoding, "BCD")) {
        if (element->digits > 7 || length > 31)
            return false;
        char *end;
        errno = 0;
        double number = strtod(text, &end);
        if (end == text || *end || errno || !isfinite(number))
            return false;
        bool negative = number < 0;
        size_t sign = negative ? 1 : 0;
        if (length <= sign)
            return false;
        char decimal[96];
        int count = snprintf(decimal, sizeof(decimal), "%.*f", (int)element->digits, fabs(number));
        if (count <= 0 || (size_t)count >= sizeof(decimal))
            return false;
        size_t digits = 0;
        for (int i = 0; i < count; ++i)
            if (decimal[i] != '.')
                decimal[digits++] = decimal[i];
        if (digits > (length - sign) * 2)
            return false;
        size_t padding = (length - sign) * 2 - digits;
        for (size_t i = 0; i < digits; ++i) {
            if (decimal[i] < '0' || decimal[i] > '9')
                return false;
            size_t position = padding + i;
            out[sign + position / 2] |= (uint8_t)((decimal[i] - '0') << (position % 2 ? 0 : 4));
        }
        if (negative)
            out[0] = 0xFF;
        return true;
    }
    size_t digits = strlen(text);
    if (!digits || digits > length * 2)
        return false;
    for (size_t i = 0; i < digits; ++i) {
        char ch = text[i];
        unsigned digit = ch >= '0' && ch <= '9'   ? (unsigned)(ch - '0')
                         : ch >= 'A' && ch <= 'F' ? (unsigned)(ch - 'A' + 10)
                         : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10)
                                                  : 16;
        if (digit > 15)
            return false;
        size_t position = length * 2 - digits + i;
        out[position / 2] |= (uint8_t)(digit << (position % 2 ? 0 : 4));
    }
    return true;
}

bool sl651_query_request(edge_acquisition_device *device,
                         const iot_edge_v1_CommandRequest *request,
                         uint64_t start_before_monotonic_ms,
                         uint64_t mapping_valid_until_monotonic_ms,
                         uint32_t command_clock_generation,
                         char *error, size_t error_size) {
    if (device->sl651_command_pending || device->sl651_query_active) {
        set_error(error, error_size, "SL651 station already has a pending command");
        return false;
    }
    if (request->command_id.size != 16 || !request->values_count || request->values_count > 8) {
        set_error(error, error_size, "SL651 command requires configured values");
        return false;
    }
    uint8_t body[4087] = {0};
    bool occupied[4087] = {0};
    size_t size = 0;
    const iot_edge_v1_Sl651ElementConfig *elements[8] = {0};
    const char *function = NULL;
    for (pb_size_t i = 0; i < request->values_count; ++i) {
        const iot_edge_v1_CommandValue *value = &request->values[i];
        if (!value->has_expected ||
            value->expected.which_value != iot_edge_v1_ScalarValue_string_value_tag)
            return false;
        for (size_t j = 0; j < device->point_count; ++j) {
            const iot_edge_v1_Sl651ElementConfig *element =
                &device->points[j].item->item.sl651_element;
            if (!strcmp(element->element_id, value->element_id) && element->writable &&
                !element->response_element) {
                if (elements[i]) {
                    set_error(error, error_size, "ambiguous SL651 command element");
                    return false;
                }
                elements[i] = element;
            }
        }
        if (!elements[i] || (function && strcmp(function, elements[i]->function_code))) {
            set_error(error, error_size, "SL651 command elements must share a writable function");
            return false;
        }
        function = elements[i]->function_code;
    }
    for (unsigned pass = 0; pass < 2; ++pass)
        for (pb_size_t i = 0; i < request->values_count; ++i) {
            const iot_edge_v1_Sl651ElementConfig *element = elements[i];
            if (element->fixed_position != (pass == 0))
                continue;
            size_t offset = size;
            if (element->fixed_position) {
                if (element->byte_offset < 8) {
                    set_error(error, error_size, "SL651 fixed command overlaps serial/time");
                    return false;
                }
                offset = element->byte_offset - 8;
            } else {
                if (element->guide.size > sizeof(body) - size)
                    return false;
                memcpy(body + size, element->guide.bytes, element->guide.size);
                offset += element->guide.size;
            }
            if (offset > sizeof(body) || element->length > sizeof(body) - offset ||
                !sl651_encode(element, request->values[i].expected.value.string_value,
                              body + offset, sizeof(body) - offset)) {
                set_error(error, error_size, "invalid SL651 command value or length");
                return false;
            }
            for (size_t j = offset; j < offset + element->length; ++j) {
                if (occupied[j]) {
                    set_error(error, error_size, "overlapping SL651 command elements");
                    return false;
                }
                occupied[j] = true;
            }
            if (size < offset + element->length)
                size = offset + element->length;
        }
    char *end;
    unsigned long code = strtoul(function, &end, 16);
    if (strlen(function) != 2 || *end || code > 255)
        return false;
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (other->link == device->link &&
            !memcmp(other->sl651_station, device->sl651_station, 5) && other->sl651_query_active) {
            set_error(error, error_size, "SL651 physical station command is busy");
            return false;
        }
    }
    bool previous = device->sl651_transmitter;
    device->sl651_transmitter = true;
    uint8_t time[6];
    sl651_time(device, time);
    device->command_start_before_monotonic_ms = start_before_monotonic_ms;
    device->mapping_valid_until_monotonic_ms = mapping_valid_until_monotonic_ms;
    device->command_clock_generation = command_clock_generation;
    device->command_start_expired = false;
    device->runtime.southbound_write_started = false;
    device->runtime.write_ack_received = false;
    device->sl651_command_pending = true;
    memcpy(device->sl651_command_id, request->command_id.bytes, 16U);
    if (monotonic_milliseconds() == 0U ||
        monotonic_milliseconds() >= start_before_monotonic_ms ||
        !edge_sl651_query(device->sl651, request->command_id.bytes, (uint8_t)code, body, size,
                          monotonic_milliseconds(), request->timeout_ms, time)) {
        device->sl651_transmitter = previous;
        if (device->runtime.southbound_write_started) {
            edge_sl651_quarantine(device->sl651);
            set_error(error, error_size,
                      "SL651 write may have started; result is uncertain and will not be resent");
        } else {
            device->sl651_command_pending = false;
            set_error(error, error_size,
                      device->command_start_expired ||
                              monotonic_milliseconds() >= start_before_monotonic_ms
                          ? "command start deadline expired before southbound write"
                          : "SL651 command requires an online M2/M3/M4 station without a pending or "
                            "timed-out query");
        }
        return false;
    }
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (other != device && other->link == device->link &&
            !memcmp(other->sl651_station, device->sl651_station, 5))
            other->sl651_transmitter = false;
    }
    device->sl651_query_active = true;
    return true;
}
