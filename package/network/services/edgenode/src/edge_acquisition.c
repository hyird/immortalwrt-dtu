#define _GNU_SOURCE

#include "edge_acquisition_internal.h"

void edge_acquisition_set_sample_callback(edge_acquisition *acquisition,
                                          edge_acquisition_sample_callback callback) {
    if (acquisition) {
        acquisition->sample_callback = callback;
        acquisition->gateway_enabled = callback != NULL;
    }
}

bool edge_acquisition_set_gateway_enabled(edge_acquisition *acquisition, bool enabled) {
    if (!acquisition || (enabled && !acquisition->sample_callback)) return false;
    if (acquisition->worker_pid > 0) {
        const uint8_t value = enabled ? 1 : 0;
        if (!worker_send(acquisition, EDGE_ACQUISITION_CONTROL_GATEWAY, (const uint8_t[16]){0}, &value, 1)) return false;
    }
    acquisition->gateway_enabled = enabled;
    return true;
}

void publish_gateway_value(edge_acquisition_device *device,
                           const iot_edge_v1_TelemetryValue *value, int64_t observed_at_ms) {
    edge_acquisition *owner = device->owner;
    if (!owner->sample_callback || !owner->gateway_enabled) return;
    edge_acquisition_sample sample = {0};
    memcpy(sample.device_id, device->config->device_id.bytes, 16);
    snprintf(sample.element_id, sizeof(sample.element_id), "%s", value->element_id);
    sample.value = value->value;
    sample.valid = value->has_value && (!value->quality[0] || !strcmp(value->quality, "good"));
    sample.observed_at_ms = observed_at_ms;
    if (owner->worker_child) {
        if (!worker_send(owner, EDGE_ACQUISITION_EVENT_SAMPLE, device->platform_id, &sample, sizeof(sample)))
            syslog(LOG_WARNING, "Modbus gateway sample not delivered: acquisition backpressure");
    } else owner->sample_callback(owner->callback_context, device->platform_id, &sample);
}

bool update_derived(edge_acquisition_device *device,
                    const iot_edge_v1_TelemetryValue *values, size_t count, int64_t now) {
    if (device->raw_telemetry && !device->owner->gateway_enabled) return false;
    const bool changed = edge_derived_update(device->derived, values, count, now);
    device->derived_deadline = edge_derived_deadline(device->derived, now);
    if (changed && device->owner->gateway_enabled) {
        for (size_t i = 0; i < edge_derived_count(device->derived); ++i) {
            const iot_edge_v1_TelemetryValue *value = edge_derived_value(device->derived, i);
            publish_gateway_value(device, value, value->sample_time_ms);
        }
    }
    return changed;
}

/* Upload chunks share one logical report ID; the platform commits only after
 * every chunk is present. Small cycles use one record directly. */
bool publish_acquisition_cycle(edge_acquisition_device *device,
                               const uint8_t platform_id[16],
                               iot_edge_v1_TelemetryRecord *record) {
    if (device->raw_telemetry) {
        record->configuration_sha256.size = 32U;
        memcpy(record->configuration_sha256.bytes, device->configuration_digest, 32U);
    }
    size_t encoded = 0;
    if (!pb_get_encoded_size(&encoded, iot_edge_v1_TelemetryRecord_fields, record))
        return false;
    if (encoded <= 14000)
        return device->owner->telemetry(device->owner->callback_context, platform_id, record);
    /* Preflight the complete report before enqueuing any fragment. Use actual
     * encoded sizes, not fixed point/frame counts that waste an envelope and
     * acknowledgement on each nearly empty fragment. */
    size_t values = 0, raw = 0, parts = 0;
    while (values < record->values_count || raw < record->raw_payloads_count) {
        iot_edge_v1_TelemetryRecord part;
        if (parts == 256U || !edge_protocol_telemetry_slice(record, values, raw, 14000, &part))
            return false;
        values += part.values_count;
        raw += part.raw_payloads_count;
        ++parts;
    }
    if (parts == 0) return false;
    values = raw = 0;
    for (size_t index = 0; index < parts; ++index) {
        iot_edge_v1_TelemetryRecord part;
        if (!edge_protocol_telemetry_slice(record, values, raw, 14000, &part)) return false;
        values += part.values_count;
        raw += part.raw_payloads_count;
        uint8_t id[16]; record_id((uint64_t)current_ms(), id);
        edge_protocol_set_bytes(&part.record_id, sizeof(part.record_id.bytes), id, 16);
        part.part_index = (uint32_t)index;
        part.part_count = (uint32_t)parts;
        if (!pb_get_encoded_size(&encoded, iot_edge_v1_TelemetryRecord_fields, &part) || encoded > 14000 ||
            !device->owner->telemetry(device->owner->callback_context, platform_id, &part)) return false;
    }
    return true;
}

bool publish_derived_expiry(edge_acquisition_device *device) {
    const int64_t now = current_ms();
    if (!device->derived) return true;
    if (device->raw_telemetry) {
        if (device->owner->gateway_enabled && now >= device->derived_deadline)
            (void)update_derived(device, NULL, 0, now);
        return true;
    }
    if (!device->derived_pending && now >= device->derived_deadline) {
        iot_edge_v1_TelemetryRecord *record = calloc(1, sizeof(*record));
        if (!record) return false;
        record->values_count = (pb_size_t)edge_derived_count(device->derived);
        record->values = calloc(record->values_count, sizeof(*record->values));
        if (!record->values) { free(record); return false; }
        const bool changed = update_derived(device, NULL, 0, now);
        if (!changed) { free(record->values); free(record); return true; }
        edge_derived_values(device->derived, record->values);
        record->record_id.size = record->device_id.size = record->endpoint_id.size = 16;
        record_id((uint64_t)now, record->record_id.bytes);
        memcpy(record->device_id.bytes, device->config->device_id.bytes, 16);
        memcpy(record->endpoint_id.bytes, device->config->endpoint_id.bytes, 16);
        record->protocol = device->config->protocol; record->observed_at_ms = now; record->derived_update = true;
        copy_text(record->direction, sizeof(record->direction), "UP");
        copy_text(record->function_code, sizeof(record->function_code), "DERIVED");
        device->derived_pending = record;
    }
    if (!device->derived_pending) return true;
    if (!publish_acquisition_cycle(device, device->platform_id, device->derived_pending)) return false;
    free(device->derived_pending->values);
    free(device->derived_pending);
    device->derived_pending = NULL;
    return true;
}

void log_report_result(edge_acquisition_device *device,
                       edge_local_report_result result, size_t frames) {
    const int64_t now = current_ms();
    if (result == EDGE_LOCAL_REPORT_QUEUED) {
        if (device->last_queue_failure_log_ms != 0 ||
            device->last_report_success_log_ms == 0 ||
            now - device->last_report_success_log_ms >= EDGE_IO_LOG_REPEAT_MS) {
            edge_log_local_report(device->platform_id, device->config->device_id.bytes,
                                  (unsigned)device->config->protocol, result,
                                  (unsigned)device->point_count, (unsigned)frames);
            device->last_report_success_log_ms = now;
        }
        device->last_queue_failure_log_ms = 0;
    } else if (device->last_queue_failure_log_ms == 0 ||
               now - device->last_queue_failure_log_ms >= EDGE_IO_LOG_REPEAT_MS) {
        edge_log_local_report(device->platform_id, device->config->device_id.bytes,
                              (unsigned)device->config->protocol, result,
                              (unsigned)device->point_count, (unsigned)frames);
        device->last_queue_failure_log_ms = now;
    }
}

static bool device_report(void *context, const uint8_t platform_id[16],
                          const uint8_t device_id[16], const edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    bool queued = false;
    edge_local_report_result result = EDGE_LOCAL_REPORT_ALLOCATION;
    (void)sample;
    iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
    const size_t capacity = device->point_count;
    size_t raw_capacity = 0;
    for (size_t index = 0; index < capacity; ++index)
        if (device->points[index].valid)
            for (const edge_acquisition_response *part = device->points[index].response; part; part = part->previous)
                ++raw_capacity;
    if (raw_capacity > 512) {
        log_report_result(device, EDGE_LOCAL_REPORT_FRAME_LIMIT, raw_capacity);
        return false;
    }
    if (raw_capacity == 0) {
        log_report_result(device, EDGE_LOCAL_REPORT_EMPTY, 0);
        return false;
    }
    if (!device->raw_telemetry)
        record.values = calloc(capacity + edge_derived_count(device->derived), sizeof(*record.values));
    record.raw_payloads = calloc(raw_capacity, sizeof(*record.raw_payloads));
    if (device->raw_telemetry)
        record.raw_requests = calloc(raw_capacity, sizeof(*record.raw_requests));
    record.raw_packet_ids = calloc(raw_capacity, sizeof(*record.raw_packet_ids));
    const edge_acquisition_response **responses = calloc(raw_capacity, sizeof(*responses));
    if ((!device->raw_telemetry && !record.values) || !record.raw_payloads || !record.raw_packet_ids || !responses ||
        (device->raw_telemetry && !record.raw_requests)) goto cleanup;
    for (size_t index = 0; index < capacity; ++index) {
        const edge_acquisition_point *point = &device->points[index];
        if (!point->valid || !point->response) continue;
        if (!device->raw_telemetry) record.values[record.values_count++] = point->value;
        for (const edge_acquisition_response *part = point->response; part; part = part->previous) {
            size_t raw = 0;
            while (raw < record.raw_payloads_count && responses[raw] != part) ++raw;
            if (raw < record.raw_payloads_count) continue;
            responses[raw] = part;
            ++record.raw_payloads_count;
        }
    }
    /* Point definitions may be in a different order from the read plan. */
    for (size_t i = 1; i < record.raw_payloads_count; ++i) {
        const edge_acquisition_response *response = responses[i];
        size_t j = i;
        while (j > 0 && responses[j-1]->sequence > response->sequence) {
            responses[j] = responses[j-1]; --j;
        }
        responses[j] = response;
    }
    for (size_t raw = 0; raw < record.raw_payloads_count; ++raw) {
        const edge_acquisition_response *response = responses[raw];
        record.raw_payloads[raw] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(response->size));
        record.raw_packet_ids[raw] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(16));
        if (!record.raw_payloads[raw] || !record.raw_packet_ids[raw]) goto cleanup;
        record.raw_payloads[raw]->size = (pb_size_t)response->size;
        memcpy(record.raw_payloads[raw]->bytes, response->bytes, response->size);
        if (device->raw_telemetry) {
            record.raw_requests[raw] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(response->request_size));
            if (!record.raw_requests[raw]) goto cleanup;
            record.raw_requests[raw]->size = (pb_size_t)response->request_size;
            memcpy(record.raw_requests[raw]->bytes, response->bytes + response->size, response->request_size);
        }
        record.raw_packet_ids[raw]->size = 16;
        memcpy(record.raw_packet_ids[raw]->bytes, response->packet_id, 16);
    }
    record.raw_packet_ids_count = record.raw_payloads_count;
    if (device->raw_telemetry) record.raw_requests_count = record.raw_payloads_count;
    result = EDGE_LOCAL_REPORT_EMPTY;
    if ((!device->raw_telemetry && !record.values_count) || !record.raw_payloads_count) goto cleanup;
    record.observed_at_ms = responses[0]->observed_at_ms;
    device->observed_at_ms = responses[record.raw_payloads_count-1]->observed_at_ms;
    edge_protocol_set_bytes(&record.record_id, sizeof(record.record_id.bytes), device->acquisition_id, 16);
    edge_protocol_set_bytes(&record.device_id, sizeof(record.device_id.bytes), device->config->device_id.bytes, 16);
    edge_protocol_set_bytes(&record.endpoint_id, sizeof(record.endpoint_id.bytes), device->config->endpoint_id.bytes, 16);
    record.protocol = device->config->protocol;
    copy_text(record.function_code, sizeof(record.function_code), "POLL");
    copy_text(record.function_name, sizeof(record.function_name), "定时采集");
    copy_text(record.direction, sizeof(record.direction), "UP");
    iot_edge_v1_DeviceStatusReport status = iot_edge_v1_DeviceStatusReport_init_zero;
    acquisition_status_local(device->owner, platform_id, &status);
    for (pb_size_t i = 0; i < status.devices_count; ++i)
        if (memcmp(status.devices[i].device_id.bytes, device_id, 16) == 0) {
            record.has_device_status = true; record.device_status = status.devices[i]; break;
        }
    if (!device->raw_telemetry) {
        edge_derived_values(device->derived, record.values + record.values_count);
        record.values_count += (pb_size_t)edge_derived_count(device->derived);
    }
    result = EDGE_LOCAL_REPORT_ENQUEUE;
    queued = publish_acquisition_cycle(device, platform_id, &record);
    if (!queued)
        syslog(LOG_ERR, "cannot queue complete acquisition cycle");
    else
        result = EDGE_LOCAL_REPORT_QUEUED;
cleanup:
    for (size_t index = 0; index < raw_capacity; ++index) {
        if (record.raw_payloads) free(record.raw_payloads[index]);
        if (record.raw_requests) free(record.raw_requests[index]);
        if (record.raw_packet_ids) free(record.raw_packet_ids[index]);
    }
    free(responses); free(record.raw_payloads); free(record.raw_requests); free(record.raw_packet_ids); free(record.values);
    log_report_result(device, result,
                      record.raw_payloads_count ? record.raw_payloads_count : raw_capacity);
    return queued;
}

static iot_edge_v1_CommandState command_state(edge_command_result result) {
    switch (result) {
    case EDGE_COMMAND_SUCCEEDED:
        return iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
    case EDGE_COMMAND_READBACK_MISMATCH:
        return iot_edge_v1_CommandState_COMMAND_STATE_READBACK_MISMATCH;
    case EDGE_COMMAND_DEVICE_OFFLINE:
        return iot_edge_v1_CommandState_COMMAND_STATE_DEVICE_OFFLINE;
    case EDGE_COMMAND_TIMED_OUT:
        return iot_edge_v1_CommandState_COMMAND_STATE_TIMED_OUT;
    case EDGE_COMMAND_REJECTED_START_EXPIRED:
        return iot_edge_v1_CommandState_COMMAND_STATE_REJECTED;
    default:
        return iot_edge_v1_CommandState_COMMAND_STATE_FAILED;
    }
}

static void device_command_complete(void *context, const uint8_t platform_id[16],
                                    const uint8_t device_id[16],
                                    const uint8_t command_id[16],
                                    edge_command_result result,
                                    const edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    (void)device_id;
    iot_edge_v1_CommandResult output = iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&output.command_id, sizeof(output.command_id.bytes),
                            command_id, 16U);
    edge_protocol_set_bytes(&output.device_id, sizeof(output.device_id.bytes),
                            device->config->device_id.bytes,
                            device->config->device_id.size);
    output.state = command_state(result);
    output.completed_at_ms = current_ms();
    copy_text(output.message, sizeof(output.message),
              result == EDGE_COMMAND_SUCCEEDED ? "write and readback succeeded"
              : result == EDGE_COMMAND_READBACK_MISMATCH ? "write readback mismatch"
              : result == EDGE_COMMAND_DEVICE_OFFLINE ? "device offline"
              : result == EDGE_COMMAND_TIMED_OUT ? "device response timed out"
              : result == EDGE_COMMAND_REJECTED_START_EXPIRED
                    ? "command start deadline expired before southbound write"
                    : "device command failed");
    output.write_ack_missing = device->runtime.southbound_write_started &&
                               !device->runtime.write_ack_received;
    if (result != EDGE_COMMAND_SUCCEEDED)
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_COMMAND,
                             ((unsigned)result << 2U) |
                             (device->runtime.southbound_write_started ? 2U : 0U) |
                             (device->runtime.write_ack_received ? 1U : 0U));
    if (device->runtime.write_ack_received &&
        result != EDGE_COMMAND_SUCCEEDED && result != EDGE_COMMAND_READBACK_MISMATCH)
        copy_text(output.message, sizeof(output.message),
                  "write acknowledged; readback failed or unavailable");
    const edge_write_command *command = &device->runtime.writes[device->runtime.write_head];
    edge_acquisition_point *point = find_point(device, command->element_id);
    if (point != NULL && actual != NULL && actual->size != 0U) {
        fill_point_value(device, point, actual->bytes, actual->size, device->read_response);
        if (point->valid) {
            output.actual_values[0] = point->value;
            output.actual_values_count = 1U;
        }
    }
    (void)device->owner->command(device->owner->callback_context,
                                 platform_id, &output);
}

static uint64_t device_monotonic_ms(void *context) {
    (void)context;
    return monotonic_milliseconds();
}

static const edge_device_driver kDriver = {
    .connect = device_connect,
    .handshake = device_handshake,
    .read = device_read,
    .write_readback = device_write_readback,
    .monotonic_ms = device_monotonic_ms,
    .disconnect = device_disconnect,
    .report = device_report,
    .command_complete = device_command_complete};

static void free_devices(edge_acquisition_device *devices, size_t count) {
    if (devices == NULL)
        return;
    for (size_t index = 0U; index < count; ++index) {
        if (devices[index].sl651)
            edge_sl651_destroy(devices[index].sl651);
        else
            edge_device_runtime_close(&devices[index].runtime);
        for (size_t point = 0U; devices[index].points != NULL &&
                                point < devices[index].point_count; ++point)
            release_response(devices[index].points[point].response);
        release_response(devices[index].read_response);
        free(devices[index].sl651_request);
        free(devices[index].debug_request);
        free(devices[index].external_io_pending);
        free(devices[index].external_io_result);
        if (devices[index].derived_pending) { free(devices[index].derived_pending->values); free(devices[index].derived_pending); }
        edge_derived_free(devices[index].derived);
        free(devices[index].points);
        free(devices[index].modbus_points);
        free(devices[index].modbus_groups);
    }
    free(devices);
}

static void free_links(edge_acquisition_link *links, size_t count) {
    if (links == NULL)
        return;
    for (size_t index = 0U; index < count; ++index) {
        close_fd(&links[index].fd);
        close_fd(&links[index].listen_fd);
    }
    free(links);
}

void edge_acquisition_set_debug_callback(edge_acquisition *acquisition, edge_acquisition_debug_callback callback) {
    if (acquisition) acquisition->debug = callback;
}

void edge_acquisition_set_dtu_callback(edge_acquisition *acquisition, edge_acquisition_dtu_callback callback) {
    if (acquisition) acquisition->dtu_callback = callback;
}

edge_acquisition *edge_acquisition_create(
    edge_acquisition_telemetry_callback telemetry,
    edge_acquisition_command_callback command, void *callback_context) {
    if (telemetry == NULL || command == NULL)
        return NULL;
    edge_acquisition *value = calloc(1U, sizeof(*value));
    if (value == NULL)
        return NULL;
    value->command_clock_generations = mmap(
        NULL, 4U * sizeof(*value->command_clock_generations),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (value->command_clock_generations == MAP_FAILED) {
        free(value);
        return NULL;
    }
    for (size_t index = 0U; index < 4U; ++index) {
        atomic_init(&value->command_clock_generations[index].generation, 1U);
        if (!atomic_is_lock_free(&value->command_clock_generations[index].generation)) {
            edge_log_write("error", "acquisition",
                           "shared 32-bit command generations are not lock-free",
                           "acquisition worker disabled");
            edge_log_local_fault(NULL, NULL, 0U,
                                 EDGE_LOCAL_FAULT_CONFIG, 12U);
            (void)munmap(value->command_clock_generations,
                         4U * sizeof(*value->command_clock_generations));
            free(value);
            return NULL;
        }
        value->cached_status[index] =
            (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    }
    value->telemetry = telemetry;
    value->command = command;
    value->callback_context = callback_context;
    value->worker_fd = -1;
    return value;
}

static bool point_for_device(const iot_edge_v1_ConfigItem *item,
                             const iot_edge_v1_DeviceConfig *device) {
    if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651)
        return item->which_item == iot_edge_v1_ConfigItem_sl651_element_tag &&
               same_id(&item->item.sl651_element.device_id, device->device_id.bytes);
    if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS)
        return item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag &&
               same_id(&item->item.modbus_register.device_id, device->device_id.bytes);
    if (edge_industrial_protocol(device->protocol))
        return item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag &&
               same_id(&item->item.industrial_point.device_id, device->device_id.bytes);
    return item->which_item == iot_edge_v1_ConfigItem_s7_area_tag &&
           same_id(&item->item.s7_area.device_id, device->device_id.bytes);
}

static bool source_precedes(const edge_acquisition_source *left,
                            const edge_acquisition_source *right) {
    if (left->priority != right->priority)
        return left->priority < right->priority;
    if (left->bootstrap != right->bootstrap)
        return left->bootstrap;
    return memcmp(left->platform_id, right->platform_id, 16U) < 0;
}

static bool shared_resource(const iot_edge_v1_EndpointConfig *left,
                            const iot_edge_v1_EndpointConfig *right) {
    if (left->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL &&
        right->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        return strcmp(left->serial.channel, right->serial.channel) == 0;
    return left->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
           right->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
           left->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
           right->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
           left->port == right->port;
}

static edge_acquisition_link *assign_link(
    edge_acquisition_link *links, size_t *link_count,
    const iot_edge_v1_EndpointConfig *endpoint,
    const edge_acquisition_source *source) {
    const bool shareable = endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL ||
                           endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    if (shareable) {
        for (size_t index = 0U; index < *link_count; ++index) {
            edge_acquisition_link *link = &links[index];
            if (!shared_resource(link->endpoint, endpoint))
                continue;
            edge_acquisition_source owner = {
                .platform_id = link->owner_platform_id,
                .priority = link->owner_priority,
                .bootstrap = link->owner_bootstrap,
            };
            if (source_precedes(source, &owner)) {
                link->endpoint = endpoint;
                memcpy(link->owner_platform_id, source->platform_id, 16U);
                link->owner_priority = source->priority;
                link->owner_bootstrap = source->bootstrap;
            }
            return link;
        }
    }
    edge_acquisition_link *link = &links[(*link_count)++];
    memset(link, 0, sizeof(*link));
    link->endpoint = endpoint;
    memcpy(link->owner_platform_id, source->platform_id, 16U);
    link->owner_priority = source->priority;
    link->owner_bootstrap = source->bootstrap;
    link->fd = -1;
    link->listen_fd = -1;
    return link;
}

bool edge_acquisition_apply_multi(edge_acquisition *acquisition,
                                  const edge_acquisition_source *sources,
                                  size_t source_count, uint64_t now_ms,
                                  char *error, size_t error_size) {
    if (acquisition == NULL || sources == NULL || source_count == 0U ||
        source_count > 4U || acquisition->worker_pid > 0) {
        set_error(error, error_size, "invalid acquisition configuration");
        return false;
    }
    size_t enabled = 0U;
    for (size_t source_index = 0U; source_index < source_count; ++source_index) {
        if (sources[source_index].platform_id == NULL ||
            sources[source_index].config == NULL) {
            set_error(error, error_size, "invalid platform acquisition source");
            return false;
        }
        const edge_runtime_config *config = sources[source_index].config;
        for (uint32_t index = 0U; index < config->item_count; ++index)
            if (config->items[index].which_item == iot_edge_v1_ConfigItem_device_tag &&
                config->items[index].item.device.enabled)
                ++enabled;
    }
    edge_dtu_channel dtu[EDGE_DTU_MAX_CHANNELS] = {0};
    size_t dtu_count = 0;
    for (size_t s = 0; s < source_count; ++s) {
        const edge_runtime_config *config = sources[s].config;
        for (uint32_t i = 0; i < config->item_count; ++i) {
            const iot_edge_v1_ConfigItem *item = &config->items[i];
            if (item->which_item != iot_edge_v1_ConfigItem_dtu_tag || !item->item.dtu.enabled) continue;
            const iot_edge_v1_DtuConfig *channel = &item->item.dtu;
            if (dtu_count == EDGE_DTU_MAX_CHANNELS) {
                set_error(error, error_size, "at most 8 enabled DTU channels are supported"); return false;
            }
            if (channel->south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL &&
                strcmp(channel->serial.channel, acquisition->serial_path)) {
                set_error(error, error_size, "DTU serial port is not advertised"); return false;
            }
            for (size_t p = 0; p < source_count; ++p) {
                const edge_runtime_config *other = sources[p].config;
                for (uint32_t j = 0; j < other->item_count; ++j) {
                    const iot_edge_v1_ConfigItem *entry = &other->items[j];
                    if (entry->which_item != iot_edge_v1_ConfigItem_device_tag || !entry->item.device.enabled) continue;
                    const iot_edge_v1_EndpointConfig *endpoint = edge_runtime_config_endpoint(other, entry->item.device.endpoint_id.bytes);
                    if (!endpoint || !endpoint->enabled) continue;
                    if ((channel->south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL &&
                         endpoint->mode == channel->south_mode && !strcmp(endpoint->serial.channel, channel->serial.channel)) ||
                        (channel->south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
                         endpoint->mode == channel->south_mode && endpoint->port == channel->south_port)) {
                        set_error(error, error_size, "DTU south resource is already used by acquisition"); return false;
                    }
                }
            }
            for (size_t j = 0; j < dtu_count; ++j) {
                const iot_edge_v1_DtuConfig *other = &dtu[j].config;
                if ((!memcmp(dtu[j].platform_id, sources[s].platform_id, 16) &&
                     !memcmp(other->channel_id.bytes, channel->channel_id.bytes, 16)) ||
                    (other->south_mode == channel->south_mode &&
                     ((channel->south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL && !strcmp(other->serial.channel, channel->serial.channel)) ||
                      (channel->south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER && other->south_port == channel->south_port)))) {
                    set_error(error, error_size, "duplicate DTU channel or exclusive south resource"); return false;
                }
            }
            dtu[dtu_count].config = *channel;
            memcpy(dtu[dtu_count++].platform_id, sources[s].platform_id, 16);
        }
    }
    edge_acquisition_device *devices = enabled != 0U ? calloc(enabled, sizeof(*devices)) : NULL;
    edge_acquisition_link *links = enabled != 0U ? calloc(enabled, sizeof(*links)) : NULL;
    if (enabled != 0U && (devices == NULL || links == NULL)) {
        free(devices);
        free(links);
        set_error(error, error_size, "acquisition memory allocation failed");
        return false;
    }
    size_t order[4] = {0U, 1U, 2U, 3U};
    for (size_t index = 1U; index < source_count; ++index) {
        const size_t value = order[index];
        size_t position = index;
        while (position != 0U &&
               source_precedes(&sources[value], &sources[order[position - 1U]])) {
            order[position] = order[position - 1U];
            --position;
        }
        order[position] = value;
    }
    size_t output = 0U;
    size_t link_count = 0U;
    for (size_t source_position = 0U; source_position < source_count; ++source_position) {
        const edge_acquisition_source *source = &sources[order[source_position]];
        const edge_runtime_config *config = source->config;
        for (uint32_t index = 0U; index < config->item_count; ++index) {
            const iot_edge_v1_ConfigItem *item = &config->items[index];
            if (item->which_item != iot_edge_v1_ConfigItem_device_tag ||
                !item->item.device.enabled)
                continue;
            const iot_edge_v1_DeviceConfig *device = &item->item.device;
            const iot_edge_v1_EndpointConfig *endpoint =
                edge_runtime_config_endpoint(config, device->endpoint_id.bytes);
            if (endpoint == NULL || !endpoint->enabled ||
                (device->protocol != iot_edge_v1_Protocol_PROTOCOL_MODBUS &&
                 device->protocol != iot_edge_v1_Protocol_PROTOCOL_S7 &&
                 device->protocol != iot_edge_v1_Protocol_PROTOCOL_SL651 &&
                 device->protocol != iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO && !edge_industrial_protocol(device->protocol))) {
                free_devices(devices, output);
                free_links(links, link_count);
                set_error(error, error_size,
                          "endpoint protocol is not supported");
                return false;
            }
            if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 &&
                (endpoint->transport != iot_edge_v1_Transport_TRANSPORT_ETHERNET ||
                 endpoint->mode != iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT)) {
                free_devices(devices, output);
                free_links(links, link_count);
                set_error(error, error_size, "S7 requires TCP Client endpoint");
                return false;
            }
            edge_acquisition_device *runtime = &devices[output];
            runtime->owner = acquisition;
            runtime->endpoint = endpoint;
            runtime->config = device;
            runtime->raw_telemetry = source->raw_telemetry;
            runtime->external_io_enabled = source->external_io;
            memcpy(runtime->configuration_digest, config->digest, sizeof(runtime->configuration_digest));
            runtime->external_io_next_ms = now_ms;
            for (uint32_t io_index = 0; io_index < config->item_count; ++io_index) {
                const iot_edge_v1_ConfigItem *entry = &config->items[io_index];
                if (entry->which_item != iot_edge_v1_ConfigItem_external_io_tag ||
                    memcmp(entry->item.external_io.device_id.bytes, device->device_id.bytes, 16U)) continue;
                const iot_edge_v1_ExternalIoPointConfig *point = &entry->item.external_io;
                if (runtime->external_io_count == 15U ||
                    (runtime->external_io_count && memcmp(runtime->external_io[0]->model_id.bytes, point->model_id.bytes, 16U))) {
                    free_devices(devices, output + 1U); free_links(links, link_count);
                    set_error(error, error_size, "invalid external IO model or point count"); return false;
                }
                if (point->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO) {
                    for (size_t previous = 0; previous < output; ++previous)
                        for (size_t p = 0; p < devices[previous].external_io_count; ++p)
                            if (devices[previous].external_io[p]->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO &&
                                !strcmp(devices[previous].external_io[p]->channel, point->channel)) {
                                free_devices(devices, output + 1U); free_links(links, link_count);
                                set_error(error, error_size, "external IO output belongs to another device or platform"); return false;
                            }
                }
                runtime->external_io[runtime->external_io_count++] = point;
            }
            runtime->derived = edge_derived_create(config, device->device_id.bytes);
            runtime->derived_deadline = INT64_MAX;
            for (uint32_t i = 0; i < config->item_count; ++i) {
                const iot_edge_v1_ConfigItem *derived_item = &config->items[i];
                if (derived_item->which_item == iot_edge_v1_ConfigItem_derived_point_tag && !memcmp(derived_item->item.derived_point.device_id.bytes, device->device_id.bytes, 16) && !runtime->derived) {
                    free_devices(devices, output + 1U); free_links(links, link_count);
                    set_error(error, error_size, "cannot allocate derived point state"); return false;
                }
            }
            runtime->industrial = device->industrial;
            memcpy(runtime->platform_id, source->platform_id, 16U);
            runtime->link = assign_link(links, &link_count, endpoint, source);
            for (uint32_t point = 0U; point < config->item_count; ++point)
                if (point_for_device(&config->items[point], device))
                    ++runtime->point_count;
            if (runtime->point_count != 0U) {
                runtime->points = calloc(runtime->point_count,
                                         sizeof(*runtime->points));
                if (runtime->points == NULL) {
                    free_devices(devices, output + 1U);
                    free_links(links, link_count);
                    set_error(error, error_size, "point memory allocation failed");
                    return false;
                }
                size_t point_output = 0U;
                for (uint32_t point = 0U; point < config->item_count; ++point)
                    if (point_for_device(&config->items[point], device))
                        runtime->points[point_output++].item = &config->items[point];
                if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS) {
                    runtime->modbus_points = calloc(runtime->point_count,
                                                    sizeof(*runtime->modbus_points));
                    runtime->modbus_groups = calloc(runtime->point_count,
                                                    sizeof(*runtime->modbus_groups));
                    if (runtime->modbus_points == NULL || runtime->modbus_groups == NULL) {
                        free_devices(devices, output + 1U);
                        free_links(links, link_count);
                        set_error(error, error_size, "Modbus plan memory allocation failed");
                        return false;
                    }
                    runtime->modbus_point_count = runtime->point_count;
                    for (size_t point = 0U; point < runtime->point_count; ++point) {
                        const iot_edge_v1_ModbusRegisterConfig *register_config =
                            &runtime->points[point].item->item.modbus_register;
                        runtime->modbus_points[point] = (edge_modbus_read_point){
                            .point_index = point,
                            .function = modbus_function(register_config->register_type),
                            .address = (uint16_t)register_config->address,
                            .quantity = (uint16_t)register_config->quantity,
                        };
                    }
                    if (!edge_modbus_plan_reads(
                            runtime->modbus_points, runtime->modbus_point_count,
                            device->modbus_merge_gap, device->modbus_max_quantity,
                            runtime->modbus_groups, runtime->point_count,
                            &runtime->modbus_group_count)) {
                        free_devices(devices, output + 1U);
                        free_links(links, link_count);
                        set_error(error, error_size, "invalid Modbus read plan");
                        return false;
                    }
                }
            }
            if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651) {
                uint8_t station[5];
                const char *code = device->device_code;
                size_t size = strlen(code);
                if (!size || size > 10) {
                    free_devices(devices, output + 1);
                    free_links(links, link_count);
                    set_error(error, error_size, "SL651 requires a BCD station address");
                    return false;
                }
                char padded[11] = "0000000000";
                memcpy(padded + 10 - size, code, size);
                for (size_t i = 0; i < 10; ++i)
                    if (padded[i] < '0' || padded[i] > '9') {
                        free_devices(devices, output + 1);
                        free_links(links, link_count);
                        set_error(error, error_size, "SL651 station address must be decimal");
                        return false;
                    }
                for (size_t i = 0; i < 5; ++i)
                    station[i] = (uint8_t)((padded[i * 2] - '0') * 16 + padded[i * 2 + 1] - '0');
                edge_sl651_callbacks callbacks = {sl651_send, sl651_report,
                                                  sl651_command_result, sl651_trace,
                                                  sl651_diagnostic};
                runtime->sl651 =
                    edge_sl651_create(device->sl651_response_mode, station, callbacks, runtime);
                if (!runtime->sl651) {
                    free_devices(devices, output + 1);
                    free_links(links, link_count);
                    set_error(error, error_size, "SL651 session allocation failed");
                    return false;
                }
                memcpy(runtime->sl651_station, station, 5);
                runtime->sl651_transmitter = true;
                for (size_t i = 0; i < output; ++i)
                    if (devices[i].link == runtime->link) {
                        if (!devices[i].sl651 ||
                            (endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL &&
                             memcmp(&endpoint->serial, &devices[i].endpoint->serial,
                                    sizeof(endpoint->serial)))) {
                            free_devices(devices, output + 1);
                            free_links(links, link_count);
                            set_error(
                                error, error_size,
                                "SL651 passive stream requires matching exclusive serial settings");
                            return false;
                        }
                        if (!memcmp(devices[i].sl651_station, runtime->sl651_station, 5)) {
                            unsigned previous_mode = devices[i].config->sl651_response_mode;
                            unsigned current_mode = device->sl651_response_mode;
                            if ((previous_mode ? previous_mode : 1) !=
                                (current_mode ? current_mode : 1)) {
                                free_devices(devices, output + 1);
                                free_links(links, link_count);
                                set_error(error, error_size,
                                          "shared SL651 station response modes conflict");
                                return false;
                            }
                            runtime->sl651_transmitter = false;
                        }
                    }
                ++output;
                continue;
            }
            for (size_t i = 0; i < output; ++i)
                if (devices[i].link == runtime->link && devices[i].sl651) {
                    free_devices(devices, output + 1);
                    free_links(links, link_count);
                    set_error(error, error_size,
                              "SL651 cannot share a physical stream with a polling protocol");
                    return false;
                }
            if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO) {
                /* Cached configurations load before platform negotiation. IO
                 * remains gated by external_io_enabled until the HelloAck. */
                if (endpoint->transport != iot_edge_v1_Transport_TRANSPORT_EXTERNAL_IO ||
                    !runtime->external_io_count || runtime->point_count) {
                    free_devices(devices, output + 1U); free_links(links, link_count);
                    set_error(error, error_size, "invalid standalone external IO device");
                    return false;
                }
                ++output;
                continue;
            }
            const edge_device_protocol protocol =
                device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS
                    ? EDGE_DEVICE_MODBUS : device->protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? EDGE_DEVICE_MC :
                    device->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? EDGE_DEVICE_FINS :
                    device->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? EDGE_DEVICE_DLT645 : EDGE_DEVICE_S7;
            if (!edge_device_runtime_init(&runtime->runtime, protocol,
                                          source->platform_id,
                                          device->device_id.bytes,
                                          device->io_interval_ms,
                                          device->report_interval_sec, now_ms,
                                          &kDriver, runtime)) {
                free_devices(devices, output + 1U);
                free_links(links, link_count);
                set_error(error, error_size,
                          "device runtime initialization failed");
                return false;
            }
            /* Retain the S7 TCP Client session across configured read periods. */
            runtime->runtime.s7_tcp_client =
                protocol == EDGE_DEVICE_S7 &&
                runtime->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
                runtime->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
            ++output;
        }
    }
    free_devices(acquisition->devices, acquisition->device_count);
    free_links(acquisition->links, acquisition->link_count);
    memcpy(acquisition->dtu, dtu, sizeof(dtu));
    acquisition->dtu_count = dtu_count;
    acquisition->devices = devices;
    acquisition->device_count = output;
    acquisition->links = links;
    acquisition->link_count = link_count;
    acquisition->platform_count = source_count;
    acquisition->io_enabled = false;
    for (size_t i = 0; i < source_count; ++i) acquisition->io_enabled = acquisition->io_enabled || sources[i].external_io;
    for (size_t index = 0U; index < source_count; ++index) {
        memcpy(acquisition->platform_ids[index], sources[index].platform_id, 16U);
        acquisition->cached_status[index] =
            (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    }
    acquisition->worker_required = dtu_count != 0U || output != 0U || acquisition->io_enabled || acquisition->serial_path[0] != '\0' || access("/etc/init.d/dtu-io", X_OK) == 0;
    char detail[96];
    snprintf(detail, sizeof(detail), "platforms=%zu devices=%zu resources=%zu",
             source_count, output, link_count);
    edge_log_write("info", "config", "acquisition config applied", detail);
    for (size_t index = 0U; index < output; ++index)
        edge_log_local_config(devices[index].platform_id,
                              devices[index].config->device_id.bytes,
                              (unsigned)devices[index].config->protocol,
                              (unsigned)devices[index].endpoint->transport,
                              devices[index].runtime.io_interval_ms,
                              devices[index].config->report_interval_sec);
    return true;
}

bool edge_acquisition_apply(edge_acquisition *acquisition,
                            const edge_runtime_config *config,
                            uint64_t now_ms, char *error, size_t error_size) {
    const uint8_t platform_id[16] = {0};
    const edge_acquisition_source source = {
        .platform_id = platform_id,
        .priority = 0U,
        .bootstrap = true,
        .config = config,
    };
    return edge_acquisition_apply_multi(acquisition, &source, 1U, now_ms,
                                        error, error_size);
}

void acquisition_status_local(edge_acquisition *acquisition,
                              const uint8_t platform_id[16],
                              iot_edge_v1_DeviceStatusReport *report) {
    if (acquisition == NULL || platform_id == NULL || report == NULL)
        return;
    *report = (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    const size_t capacity = sizeof(report->devices) / sizeof(report->devices[0]);
    for (size_t index = 0U; index < acquisition->device_count &&
                           report->devices_count < capacity; ++index) {
        edge_acquisition_device *device = &acquisition->devices[index];
        if (memcmp(device->platform_id, platform_id, 16U) != 0)
            continue;
        iot_edge_v1_DeviceStatus *status = &report->devices[report->devices_count++];
        edge_protocol_set_bytes(&status->device_id, sizeof(status->device_id.bytes),
                                 device->config->device_id.bytes, 16U);
        const bool connected = device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO
            ? acquisition->io_connected : device->link->fd >= 0;
        if (connected) {
            copy_text(status->state, sizeof(status->state), "connected");
            status->client_count =
                connected && device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET ? 1U : 0U;
        } else if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
                   device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT) {
            copy_text(status->state, sizeof(status->state), "reconnecting");
        } else {
            copy_text(status->state, sizeof(status->state), "disconnected");
        }
        if (device->has_last_io_result && device->last_io_result != EDGE_IO_OK)
            copy_text(status->reason, sizeof(status->reason),
                      device->last_error[0] != '\0' ? device->last_error
                                                     : io_result_name(device->last_io_result));
        else
            copy_text(status->reason, sizeof(status->reason), "");
        status->last_activity_at_ms = device->last_activity_at_ms;
    }
}

edge_acquisition_device *find_device(edge_acquisition *acquisition,
                                     const uint8_t platform_id[16],
                                     const uint8_t id[16]) {
    for (size_t index = 0U; index < acquisition->device_count; ++index)
        if (memcmp(acquisition->devices[index].platform_id, platform_id, 16U) == 0 &&
            same_id(&acquisition->devices[index].config->device_id, id))
            return &acquisition->devices[index];
    return NULL;
}

static bool acquisition_command_local(edge_acquisition *acquisition,
                                      const uint8_t platform_id[16],
                                      const iot_edge_v1_CommandRequest *request,
                                      uint64_t start_before_monotonic_ms,
                                      uint64_t mapping_valid_until_monotonic_ms,
                                      uint32_t command_clock_generation,
                                      char *error, size_t error_size) {
    if (request != NULL && request->command_id.size == 16U &&
        request->device_id.size == 16U) {
        edge_acquisition_device *previous =
            find_device(acquisition, platform_id, request->device_id.bytes);
        if (previous != NULL && edge_device_runtime_command_id_seen(
                                    &previous->runtime, request->command_id.bytes))
            return true;
    }
    const uint64_t now = monotonic_milliseconds();
    if (request == NULL || !request->has_start_before_ms ||
        start_before_monotonic_ms == 0U || mapping_valid_until_monotonic_ms == 0U ||
        command_clock_generation == 0U || now == 0U ||
        now >= start_before_monotonic_ms || now >= mapping_valid_until_monotonic_ms ||
        !command_clock_generation_is_current(acquisition, platform_id,
                                            command_clock_generation)) {
        set_error(error, error_size,
                  "command start deadline or database-time mapping expired or was invalidated");
        return false;
    }
    if (request && request->device_id.size == 16) {
        edge_acquisition_device *station =
            find_device(acquisition, platform_id, request->device_id.bytes);
        if (station && serial_paused(station)) {
            set_error(error, error_size, "serial port is paused for manual debugging");
            return false;
        }
        if (station && station->sl651) {
            if (request->command_id.size != 16U) {
                set_error(error, error_size, "invalid SL651 command id");
                return false;
            }
            if (edge_device_runtime_claim_command_id(&station->runtime,
                                                      request->command_id.bytes))
                return sl651_query_request(station, request, start_before_monotonic_ms,
                                           mapping_valid_until_monotonic_ms,
                                           command_clock_generation, error, error_size);
            return true;
        }
    }
    edge_acquisition_device *device = NULL;
    edge_write_command command;
    if (!build_write_command(acquisition, platform_id, request,
                             start_before_monotonic_ms, mapping_valid_until_monotonic_ms,
                             command_clock_generation, &device, &command,
                             error, error_size))
        return false;
    if (!edge_device_runtime_enqueue_write(&device->runtime, &command)) {
        set_error(error, error_size, "device write queue is full");
        return false;
    }
    return true;
}

bool worker_send(edge_acquisition *acquisition, uint32_t type,
                 const uint8_t platform_id[16],
                 const void *payload, uint32_t payload_size) {
    if (acquisition == NULL || acquisition->worker_fd < 0 || platform_id == NULL ||
        payload == NULL ||
        payload_size > sizeof(((edge_acquisition_message *)0)->payload))
        return false;
    edge_acquisition_message message;
    acquisition_init_message(&message, type, payload_size);
    memcpy(message.platform_id, platform_id, sizeof(message.platform_id));
    memcpy(&message.payload, payload, payload_size);
    const size_t size = acquisition_message_size(payload_size);
    for (;;) {
        const ssize_t sent = send(acquisition->worker_fd, &message, size, MSG_NOSIGNAL);
        if (sent == (ssize_t)size)
            return true;
        if (sent < 0 && errno == EINTR)
            continue;
        return false;
    }
}

static void worker_debug(void *context, const uint8_t platform_id[16], const iot_edge_v1_RawPacket *packet) {
    edge_acquisition *acquisition = context;
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_EVENT_DEBUG, 0U);
    memcpy(message.platform_id, platform_id, 16);
    pb_ostream_t stream = pb_ostream_from_buffer(message.payload.telemetry, sizeof(message.payload.telemetry));
    if (!pb_encode(&stream, iot_edge_v1_RawPacket_fields, packet)) {
        syslog(LOG_WARNING, "debug packet dropped: worker encoding limit");
        edge_log_local_fault(platform_id, NULL, 0U,
                             EDGE_LOCAL_FAULT_WORKER_ENCODE, 2U);
        return;
    }
    message.payload_size = (uint32_t)stream.bytes_written;
    const size_t size = acquisition_message_size(message.payload_size);
    if (send(acquisition->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)size) {
        syslog(LOG_WARNING, "debug packet dropped: acquisition worker backpressure");
        edge_log_local_fault(platform_id, NULL, 0U,
                             EDGE_LOCAL_FAULT_WORKER_SEND, 3U);
    }
}

static bool worker_telemetry(void *context,
                             const uint8_t platform_id[16],
                             const iot_edge_v1_TelemetryRecord *record) {
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE];
    pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
    iot_edge_v1_TelemetryRecord stamped = *record;
    edge_acquisition_device *device = record->device_id.size == 16U
        ? find_device(context, platform_id, record->device_id.bytes) : NULL;
    if (device && device->raw_telemetry) {
        stamped.configuration_sha256.size = 32U;
        memcpy(stamped.configuration_sha256.bytes, device->configuration_digest, 32U);
    }
    if (!pb_encode(&stream, iot_edge_v1_TelemetryRecord_fields, &stamped)) {
        syslog(LOG_ERR, "cannot encode complete telemetry record: %s", PB_GET_ERROR(&stream));
        edge_log_local_fault(platform_id,
                             record->device_id.size == 16U ? record->device_id.bytes : NULL,
                             (unsigned)record->protocol, EDGE_LOCAL_FAULT_WORKER_ENCODE, 1U);
        return false;
    }
    // Pointers are process-local. Transfer serialized values, never the C struct.
    const bool sent = worker_send(context, EDGE_ACQUISITION_EVENT_TELEMETRY,
                                  platform_id, wire, (uint32_t)stream.bytes_written);
    if (!sent)
        edge_log_local_fault(platform_id,
                             record->device_id.size == 16U ? record->device_id.bytes : NULL,
                             (unsigned)record->protocol, EDGE_LOCAL_FAULT_WORKER_SEND, 1U);
    return sent;
}

static bool worker_command_result(void *context,
                                  const uint8_t platform_id[16],
                                  const iot_edge_v1_CommandResult *result) {
    const bool sent = worker_send(context, EDGE_ACQUISITION_EVENT_COMMAND_RESULT,
                                  platform_id, result, sizeof(*result));
    if (!sent) {
        edge_acquisition *acquisition = context;
        const uint8_t *device_id = result->device_id.size == 16U
            ? result->device_id.bytes : NULL;
        edge_acquisition_device *device = device_id != NULL
            ? find_device(acquisition, platform_id, device_id) : NULL;
        edge_log_local_fault(platform_id, device_id,
                             device != NULL ? (unsigned)device->config->protocol : 0U,
                             EDGE_LOCAL_FAULT_WORKER_SEND, 2U);
    }
    return sent;
}

static void worker_send_command_failure(edge_acquisition *acquisition,
                                        const uint8_t platform_id[16],
                                        const iot_edge_v1_CommandRequest *request,
                                        const char *error) {
    iot_edge_v1_CommandResult result = iot_edge_v1_CommandResult_init_zero;
    if (request->command_id.size == 16U)
        edge_protocol_set_bytes(&result.command_id, sizeof(result.command_id.bytes),
                                request->command_id.bytes, 16U);
    if (request->device_id.size == 16U)
        edge_protocol_set_bytes(&result.device_id, sizeof(result.device_id.bytes),
                                request->device_id.bytes, 16U);
    edge_acquisition_device *device = request->device_id.size == 16U
        ? find_device(acquisition, platform_id, request->device_id.bytes) : NULL;
    const bool write_started = device != NULL && request->command_id.size == 16U &&
        device->runtime.southbound_write_started && device->southbound_command_id_valid &&
        memcmp(device->southbound_command_id, request->command_id.bytes, 16U) == 0;
    copy_text(result.node_io_channel, sizeof(result.node_io_channel), request->node_io_channel);
    result.state = write_started ? iot_edge_v1_CommandState_COMMAND_STATE_FAILED
                                 : iot_edge_v1_CommandState_COMMAND_STATE_REJECTED;
    result.write_ack_missing = write_started && !device->runtime.write_ack_received;
    result.completed_at_ms = current_ms();
    copy_text(result.message, sizeof(result.message), error);
    edge_log_local_fault(platform_id,
                         request->device_id.size == 16U ? request->device_id.bytes : NULL,
                         device != NULL ? (unsigned)device->config->protocol : 0U,
                         EDGE_LOCAL_FAULT_COMMAND,
                         write_started ? 2U : 0U);
    (void)worker_command_result(acquisition, platform_id, &result);
}

static void set_device_telemetry_format(edge_acquisition_device *device, bool raw) {
    if (device->raw_telemetry == raw) return;
    device->raw_telemetry = raw;
    for (size_t i = 0; i < device->point_count; ++i) device->points[i].valid = false;
    if (raw) {
        device->derived_deadline = device->owner->gateway_enabled
            ? edge_derived_deadline(device->derived, current_ms()) : INT64_MAX;
        if (device->derived_pending) {
            free(device->derived_pending->values);
            free(device->derived_pending);
            device->derived_pending = NULL;
        }
    }
}

static bool worker_receive_control(edge_acquisition *acquisition, bool *stop) {
    edge_acquisition_message message;
    const ssize_t size = recv(acquisition->worker_fd, &message, sizeof(message),
                              MSG_DONTWAIT);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return true;
    if (size < 0 && errno == EINTR)
        return true;
    if (size <= 0)
        return false;
    if ((size_t)size < offsetof(edge_acquisition_message, payload) ||
        message.magic != EDGE_ACQUISITION_MAGIC ||
        message.payload_size > sizeof(message.payload) ||
        acquisition_message_size(message.payload_size) != (size_t)size)
        return false;
    if (message.type == EDGE_ACQUISITION_CONTROL_IO_QUIESCE && message.payload_size == 0U) {
        acquisition->io_quiesce_requested = true;
        acquisition->io_quiesce_sent = false;
        acquisition->io_quiesce_token = message.report_token;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_GATEWAY && message.payload_size == 1U) {
        acquisition->gateway_enabled = message.payload.telemetry[0] != 0;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_IO_RESUME && message.payload_size == 0U) {
        acquisition->io_quiesce_requested = false;
        acquisition->io_quiesce_sent = false;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_ENABLE && message.payload_size == 1) {
        for (size_t i = 0; i < acquisition->device_count; ++i)
            if (!memcmp(acquisition->devices[i].platform_id, message.platform_id, 16U))
                acquisition->devices[i].external_io_enabled = message.payload.telemetry[0] != 0;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_COMMIT && message.payload_size == 16U) {
        edge_acquisition_device *device = find_device(acquisition, message.platform_id, message.device_id);
        if (device && device->external_io_pending && !memcmp(device->external_io_pending_id, message.payload.telemetry, 16U)) {
            free(device->external_io_pending);
            device->external_io_pending = NULL;
            device->external_io_pending_size = 0;
        }
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_COMMAND_COMMIT && message.payload_size == 16U) {
        edge_acquisition_device *device = find_device(acquisition, message.platform_id, message.device_id);
        if (device && device->external_io_result && !memcmp(device->external_io_result->command_id.bytes, message.payload.telemetry, 16U)) {
            free(device->external_io_result);
            device->external_io_result = NULL;
        }
        const size_t platform = node_io_platform_index(acquisition, message.platform_id);
        if (platform < 4U && acquisition->node_io[platform].pending && !memcmp(acquisition->node_io[platform].result.command_id.bytes, message.payload.telemetry, 16U)) acquisition->node_io[platform].pending = false;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_RAW_TELEMETRY && message.payload_size == 1) {
        for (size_t i = 0; i < acquisition->device_count; ++i)
            if (!memcmp(acquisition->devices[i].platform_id, message.platform_id, 16))
                set_device_telemetry_format(&acquisition->devices[i], message.payload.telemetry[0] != 0);
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_SL651_COMMIT ||
        message.type == EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT) {
        edge_acquisition_device *device =
            find_device(acquisition, message.platform_id, message.device_id);
        if (!device || !device->sl651)
            return true;
        uint8_t time[6];
        sl651_time(device, time);
        if (message.type == EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT) {
            if (message.payload_size != 16 || !device->sl651_result_pending ||
                memcmp(message.payload.telemetry, device->sl651_result.command_id.bytes, 16))
                return true;
            device->sl651_result_pending = false;
            device->sl651_query_active = false;
            device->sl651_command_pending = false;
            edge_sl651_command_committed(device->sl651, message.payload.telemetry,
                                         monotonic_milliseconds(), time);
            return true;
        }
        if (message.report_token != device->sl651_token ||
            message.report_part >= device->sl651_parts)
            return true;
        device->sl651_committed[message.report_part] = true;
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *other = &acquisition->devices[i];
            if (!other->sl651 || other->link != device->link ||
                memcmp(other->sl651_station, device->sl651_station, 5))
                continue;
            if (!other->sl651_report_encoded)
                return true;
            for (uint32_t part = 0; part < other->sl651_parts; ++part)
                if (!other->sl651_committed[part])
                    return true;
        }
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *other = &acquisition->devices[i];
            if (!other->sl651 || other->link != device->link ||
                memcmp(other->sl651_station, device->sl651_station, 5))
                continue;
            sl651_time(other, time);
            edge_sl651_commit(other->sl651, other->sl651_token, monotonic_milliseconds(), time);
        }
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_STOP) {
        *stop = true;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_SERIAL &&
        message.payload_size == sizeof(message.payload.serial_request)) {
        serial_control(acquisition, message.platform_id, &message.payload.serial_request);
        return true;
    }
    if (message.type != EDGE_ACQUISITION_CONTROL_COMMAND ||
        message.payload_size != sizeof(message.payload.command_request))
        return false;
    char error[256] = {0};
    if (message.payload.command_request.external_io) {
        if (!external_io_command(acquisition, &message))
            worker_send_command_failure(acquisition, message.platform_id, &message.payload.command_request, "external IO command rejected");
        return true;
    }
    if (!acquisition_command_local(acquisition, message.platform_id,
                                   &message.payload.command_request,
                                   message.command_start_before_monotonic_ms,
                                   message.mapping_valid_until_monotonic_ms,
                                   message.command_clock_generation,
                                   error, sizeof(error)))
        worker_send_command_failure(acquisition, message.platform_id,
                                    &message.payload.command_request,
                                    error[0] != '\0' ? error : "device command rejected");
    return true;
}

typedef struct { edge_acquisition *acquisition; const uint8_t *platform_id; } dtu_report_context;

bool edge_acquisition_set_raw_telemetry(edge_acquisition *acquisition,
                                        const uint8_t platform_id[16], bool enabled) {
    if (!acquisition || !platform_id) return false;
    if (acquisition->worker_pid > 0) {
        edge_acquisition_message message;
        acquisition_init_message(&message, EDGE_ACQUISITION_CONTROL_RAW_TELEMETRY, 1U);
        memcpy(message.platform_id, platform_id, 16);
        message.payload.telemetry[0] = enabled ? 1 : 0;
        const size_t size = acquisition_message_size(1U);
        if (send(acquisition->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)size)
            return false;
    }
    for (size_t i = 0; i < acquisition->device_count; ++i)
        if (!memcmp(acquisition->devices[i].platform_id, platform_id, 16))
            set_device_telemetry_format(&acquisition->devices[i], enabled);
    return true;
}

static bool worker_dtu_status(void *context, const iot_edge_v1_DtuStatus *status) {
    const dtu_report_context *report = context;
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_EVENT_DTU, sizeof(*status));
    memcpy(message.platform_id, report->platform_id, 16);
    message.payload.dtu_status = *status;
    const size_t size = acquisition_message_size(sizeof(*status));
    const bool sent = send(report->acquisition->worker_fd, &message, size,
                           MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)size;
    if (!sent)
        edge_log_local_fault(report->platform_id, NULL, 0U,
                             EDGE_LOCAL_FAULT_DTU, 2U);
    return sent;
}

static void dtu_supervise(edge_acquisition *acquisition, uint64_t now) {
    for (size_t i = 0; i < acquisition->dtu_count; ++i) {
        edge_dtu_channel *channel = &acquisition->dtu[i];
        if (channel->pid > 0) {
            pid_t result = waitpid(channel->pid, NULL, WNOHANG);
            if (result == channel->pid || (result < 0 && errno == ECHILD)) {
                edge_log_local_fault(channel->platform_id, NULL, 0U,
                                     EDGE_LOCAL_FAULT_DTU, 3U);
                channel->pid = 0;
                channel->retry_at = now + 1000;
            }
        }
        if (channel->pid || now < channel->retry_at) continue;
        pid_t parent = getpid();
        channel->pid = fork();
        if (channel->pid == 0) {
            (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(1);
            edge_process_close_inherited_fds(acquisition->worker_fd);
            dtu_report_context report = {acquisition, channel->platform_id};
            edge_dtu_run(&channel->config, worker_dtu_status, &report);
            _exit(1);
        }
        if (channel->pid < 0) {
            edge_log_local_fault(channel->platform_id, NULL, 0U,
                                 EDGE_LOCAL_FAULT_DTU, 4U);
            channel->pid = 0;
            channel->retry_at = now + 1000;
        }
    }
}

static size_t count_pending_writes(const edge_acquisition *acquisition) {
    size_t total = 0U;
    for (size_t i = 0U; i < acquisition->device_count; ++i) {
        const edge_acquisition_device *device = &acquisition->devices[i];
        if (!device->sl651)
            total += device->runtime.write_count;
    }
    return total;
}

static void acquisition_worker(edge_acquisition *acquisition, int worker_fd) {
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _exit(1);
    edge_process_close_inherited_fds(worker_fd);
    acquisition->worker_child = true;
    acquisition->worker_pid = 0;
    acquisition->worker_fd = worker_fd;
    acquisition->debug = worker_debug;
    acquisition->telemetry = worker_telemetry;
    acquisition->command = worker_command_result;
    acquisition->callback_context = acquisition;
    int link_fd = open_link_monitor();
    if (link_fd < 0) {
        edge_log_write("warn", "network", "cannot subscribe to link events",
                       "device reconnect will rely on transport errors");
        edge_log_local_fault(NULL, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK, 16U);
    }

    bool stop = false;
    bool status_sent[4] = {false};
    uint64_t last_status_ms[4] = {0U};
    uint64_t next_tick = monotonic_milliseconds();
    while (!stop) {
        const uint64_t current = monotonic_milliseconds();
        int timeout = current >= next_tick ? 0 : (int)(next_tick - current);
        if (timeout > 20)
            timeout = 20;
        struct pollfd descriptors[2] = {
            {.fd = worker_fd, .events = POLLIN},
            {.fd = link_fd, .events = POLLIN},
        };
        const nfds_t descriptor_count = link_fd >= 0 ? 2U : 1U;
        const int ready = poll(descriptors, descriptor_count, timeout);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready > 0 && link_fd >= 0 &&
            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close_fd(&link_fd);
            edge_log_write("warn", "network", "link event monitor stopped",
                           "device reconnect will rely on transport errors");
            edge_log_local_fault(NULL, NULL, 0U,
                                 EDGE_LOCAL_FAULT_NETWORK, 17U);
        } else if (ready > 0 && link_fd >= 0 &&
                   (descriptors[1].revents & POLLIN) != 0) {
            drain_link_events(acquisition, link_fd);
        }
        if (ready > 0 &&
            (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            break;
        if (ready > 0 && (descriptors[0].revents & POLLIN) != 0 &&
            !worker_receive_control(acquisition, &stop))
            goto done;
        if (stop)
            break;
        const uint64_t schedule_ms = monotonic_milliseconds();
        dtu_supervise(acquisition, schedule_ms);
        serial_tick(acquisition, schedule_ms);
        external_io_tick(acquisition, schedule_ms);
        if (schedule_ms < next_tick)
            continue;
        size_t write_budget = 4U;
        size_t written_indices[4] = {0U};
        size_t written_count = 0U;
        size_t pending_writes = count_pending_writes(acquisition);
        for (size_t index = 0U; index < acquisition->device_count && !stop; ++index) {
            /* Commands are accepted only between synchronous protocol exchanges: an
             * already-sent request is allowed to finish or time out before any write. */
            bool control_received = false;
            for (size_t received = 0U; received < 16U && !stop; ++received) {
                struct pollfd pending = {.fd = worker_fd, .events = POLLIN};
                if (poll(&pending, 1U, 0) <= 0)
                    break;
                if (!worker_receive_control(acquisition, &stop))
                    goto done;
                control_received = true;
            }
            if (stop)
                break;
            if (control_received)
                pending_writes = count_pending_writes(acquisition);

            size_t write_index = acquisition->device_count;
            if (pending_writes != 0U) {
                /* A manual serial session suppresses device I/O, not command expiry. */
                for (size_t candidate = 0U; candidate < acquisition->device_count; ++candidate) {
                    edge_acquisition_device *device = &acquisition->devices[candidate];
                    if (!device->sl651 && device->runtime.write_count != 0U &&
                        serial_paused(device)) {
                        const edge_write_command *command =
                            &device->runtime.writes[device->runtime.write_head];
                        if (!command_clock_generation_is_current(
                                acquisition, device->platform_id,
                                command->command_clock_generation))
                            edge_device_runtime_reject_write(&device->runtime);
                        else
                            edge_device_runtime_expire_write(&device->runtime,
                                                             monotonic_milliseconds());
                    }
                }
                /* apply_multi keeps devices in platform priority order. Select queued
                 * writes in that order, independent of the next background-read index. */
                if (write_budget != 0U) {
                    for (size_t candidate = 0U; candidate < acquisition->device_count; ++candidate) {
                        edge_acquisition_device *device = &acquisition->devices[candidate];
                        if (!device->sl651 && device->runtime.write_count != 0U &&
                            !serial_paused(device)) {
                            write_index = candidate;
                            break;
                        }
                    }
                }
                if (write_index < acquisition->device_count) {
                    edge_acquisition_device *write_device = &acquisition->devices[write_index];
                    if (publish_derived_expiry(write_device)) {
                        edge_device_runtime_tick(&write_device->runtime, schedule_ms, current_ms());
                        --write_budget;
                        written_indices[written_count++] = write_index;
                    }
                }
                pending_writes = count_pending_writes(acquisition);
            }

            bool wrote_current_device = false;
            for (size_t written = 0U; written < written_count; ++written)
                wrote_current_device = wrote_current_device || written_indices[written] == index;
            edge_acquisition_device *device = &acquisition->devices[index];
            if (!publish_derived_expiry(device)) continue;
            if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO) continue;
            if (serial_paused(device)) {
                /* Manual control owns the port; leave other physical links running. */
            } else if (device->sl651) {
                sl651_poll(device, schedule_ms);
            } else if (device->runtime.write_count == 0U && !wrote_current_device) {
                edge_device_runtime_tick(&device->runtime, schedule_ms, current_ms());
            }
        }
        const uint64_t status_now = monotonic_milliseconds();
        for (size_t index = 0U; index < acquisition->platform_count; ++index) {
            iot_edge_v1_DeviceStatusReport status;
            acquisition_status_local(acquisition, acquisition->platform_ids[index],
                                     &status);
            if (!status_sent[index] ||
                memcmp(&status, &acquisition->cached_status[index], sizeof(status)) != 0 ||
                status_now - last_status_ms[index] >= 1000U) {
                if (!worker_send(acquisition, EDGE_ACQUISITION_EVENT_STATUS,
                                 acquisition->platform_ids[index], &status,
                                 sizeof(status)))
                    goto done;
                acquisition->cached_status[index] = status;
                status_sent[index] = true;
                last_status_ms[index] = status_now;
            }
        }
        next_tick = monotonic_milliseconds() + EDGE_ACQUISITION_TICK_MS;
    }
done:
    for (size_t i = 0; i < acquisition->dtu_count; ++i)
        if (acquisition->dtu[i].pid > 0) {
            kill(acquisition->dtu[i].pid, SIGKILL);
            while (waitpid(acquisition->dtu[i].pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    close_fd(&link_fd);
    close(worker_fd);
    _exit(0);
}

static bool start_worker(edge_acquisition *acquisition,
                         char *error, size_t error_size) {
    if (!acquisition->worker_required)
        return true;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        set_error(error, error_size, "cannot create acquisition worker channel");
        return false;
    }
    for (size_t index = 0U; index < acquisition->platform_count; ++index)
        acquisition_status_local(acquisition, acquisition->platform_ids[index],
                                 &acquisition->cached_status[index]);
    const pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        set_error(error, error_size, "cannot start acquisition worker");
        return false;
    }
    if (child == 0) {
        close(sockets[0]);
        acquisition_worker(acquisition, sockets[1]);
    }
    close(sockets[1]);
    const int flags = fcntl(sockets[0], F_GETFL, 0);
    if (flags < 0 || fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(sockets[0]);
        (void)kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
        set_error(error, error_size, "cannot configure acquisition worker channel");
        return false;
    }
    acquisition->worker_pid = child;
    acquisition->worker_fd = sockets[0];
    acquisition->worker_last_event_ms = monotonic_milliseconds();
    acquisition->worker_restart_at_ms = 0U;
    return true;
}

bool edge_acquisition_start(edge_acquisition *acquisition,
                            char *error, size_t error_size) {
    if (acquisition == NULL || acquisition->worker_pid > 0 || acquisition->worker_child) {
        set_error(error, error_size, "invalid acquisition worker state");
        return false;
    }
    return start_worker(acquisition, error, error_size);
}

int edge_acquisition_event_fd(const edge_acquisition *acquisition) {
    return acquisition != NULL && !acquisition->worker_child
               ? acquisition->worker_fd
               : -1;
}

static void worker_reaped(edge_acquisition *acquisition, uint64_t now_ms) {
    edge_log_local_fault(NULL, NULL, 0U, EDGE_LOCAL_FAULT_NETWORK, 5U);
    if (acquisition->worker_fd >= 0)
        close(acquisition->worker_fd);
    acquisition->worker_fd = -1;
    acquisition->worker_pid = 0;
    acquisition->worker_restart_at_ms = now_ms + EDGE_ACQUISITION_RESTART_MS;
    if (acquisition->sample_callback)
        for (size_t i = 0; i < acquisition->platform_count; ++i)
            acquisition->sample_callback(acquisition->callback_context, acquisition->platform_ids[i], NULL);
    for (size_t platform = 0U; platform < acquisition->platform_count; ++platform)
        for (pb_size_t index = 0U;
             index < acquisition->cached_status[platform].devices_count; ++index) {
            copy_text(acquisition->cached_status[platform].devices[index].state,
                      sizeof(acquisition->cached_status[platform].devices[index].state),
                      "worker-restarting");
            copy_text(acquisition->cached_status[platform].devices[index].reason,
                      sizeof(acquisition->cached_status[platform].devices[index].reason),
                      "acquisition worker exited");
        }
}

void drain_worker(edge_acquisition *acquisition, uint64_t now_ms) {
    if (acquisition->worker_pid <= 0 || acquisition->worker_fd < 0)
        return;
    for (;;) {
        edge_acquisition_message message;
        const ssize_t size = recv(acquisition->worker_fd, &message, sizeof(message), 0);
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (size <= 0) {
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(acquisition->worker_pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
                worker_reaped(acquisition, now_ms);
            break;
        }
        if ((size_t)size < offsetof(edge_acquisition_message, payload) ||
            message.magic != EDGE_ACQUISITION_MAGIC ||
            message.payload_size > sizeof(message.payload) ||
            acquisition_message_size(message.payload_size) != (size_t)size)
            continue;
        acquisition->worker_last_event_ms = now_ms;
        if (message.type == EDGE_ACQUISITION_EVENT_SAMPLE && message.payload_size == sizeof(message.payload.sample)) {
            if (acquisition->sample_callback)
                acquisition->sample_callback(acquisition->callback_context, message.platform_id, &message.payload.sample);
        } else if (message.type == EDGE_ACQUISITION_EVENT_IO_QUIESCED && message.payload_size == 1U && message.report_token == acquisition->io_quiesce_token) {
            acquisition->io_quiesced_ack = true;
        } else if (message.type == EDGE_ACQUISITION_EVENT_IO_CAPABILITY &&
            message.payload_size == sizeof(message.payload.io_capability)) {
            acquisition->io_capability = message.payload.io_capability;
            acquisition->io_capability_seen = true;
            if (acquisition->io_callback)
                acquisition->io_callback(acquisition->callback_context, &acquisition->io_capability);
        } else if (message.type == EDGE_ACQUISITION_EVENT_DTU &&
            message.payload_size == sizeof(message.payload.dtu_status)) {
            if (acquisition->dtu_callback)
                acquisition->dtu_callback(acquisition->callback_context, message.platform_id, &message.payload.dtu_status);
        } else if (message.type == EDGE_ACQUISITION_EVENT_SERIAL &&
            message.payload_size == sizeof(message.payload.serial_event)) {
            if (acquisition->serial_callback)
                acquisition->serial_callback(acquisition->callback_context, message.platform_id,
                    &message.payload.serial_event);
        } else if (message.type == EDGE_ACQUISITION_EVENT_DEBUG) {
            iot_edge_v1_RawPacket packet = iot_edge_v1_RawPacket_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(message.payload.telemetry, message.payload_size);
            if (pb_decode(&stream, iot_edge_v1_RawPacket_fields, &packet) && acquisition->debug)
                acquisition->debug(acquisition->callback_context, message.platform_id, &packet);
            pb_release(iot_edge_v1_RawPacket_fields, &packet);
        } else if (message.type == EDGE_ACQUISITION_EVENT_EXTERNAL_IO ||
            message.type == EDGE_ACQUISITION_EVENT_TELEMETRY ||
            message.type == EDGE_ACQUISITION_EVENT_SL651) {
            iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(message.payload.telemetry,
                                                        message.payload_size);
            if (pb_decode(&stream, iot_edge_v1_TelemetryRecord_fields, &record)) {
                const bool stored = acquisition->telemetry(acquisition->callback_context,
                                                           message.platform_id, &record);
                if (stored && message.type == EDGE_ACQUISITION_EVENT_EXTERNAL_IO &&
                    record.record_id.size == 16U && record.device_id.size == 16U) {
                    memcpy(message.device_id, record.device_id.bytes, 16U);
                    memcpy(message.payload.telemetry, record.record_id.bytes, 16U);
                    message.type = EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_COMMIT;
                    message.payload_size = 16U;
                    (void)send(acquisition->worker_fd, &message, acquisition_message_size(16U), MSG_DONTWAIT | MSG_NOSIGNAL);
                }
                if (stored && message.type == EDGE_ACQUISITION_EVENT_SL651) {
                    message.type = EDGE_ACQUISITION_CONTROL_SL651_COMMIT;
                    message.payload_size = 0;
                    (void)send(acquisition->worker_fd, &message, acquisition_message_size(0),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
                }
            } else {
                syslog(LOG_ERR, "cannot decode telemetry from acquisition worker");
                edge_log_local_fault(message.platform_id, NULL, 0U,
                                     EDGE_LOCAL_FAULT_PARENT_DECODE, 1U);
            }
            pb_release(iot_edge_v1_TelemetryRecord_fields, &record);
        } else if ((message.type == EDGE_ACQUISITION_EVENT_COMMAND_RESULT || message.type == EDGE_ACQUISITION_EVENT_EXTERNAL_IO_COMMAND) &&
                   message.payload_size == sizeof(message.payload.command_result)) {
            const iot_edge_v1_CommandResult *result = &message.payload.command_result;
            const bool stored =
                acquisition->command(acquisition->callback_context, message.platform_id, result);
            edge_acquisition_device *device =
                result->device_id.size == 16
                    ? find_device(acquisition, message.platform_id, result->device_id.bytes)
                    : NULL;
            if (stored && message.type == EDGE_ACQUISITION_EVENT_EXTERNAL_IO_COMMAND && (device || result->node_io_channel[0]) && result->command_id.size == 16U) {
                uint8_t id[16];
                memcpy(id, result->command_id.bytes, 16U);
                memcpy(message.device_id, result->device_id.bytes, 16U);
                memcpy(message.payload.telemetry, id, 16U);
                message.type = EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_COMMAND_COMMIT;
                message.payload_size = 16U;
                (void)send(acquisition->worker_fd, &message, acquisition_message_size(16U), MSG_DONTWAIT | MSG_NOSIGNAL);
            }
            if (stored && message.type == EDGE_ACQUISITION_EVENT_COMMAND_RESULT && device && device->sl651 && result->command_id.size == 16) {
                uint8_t id[16];
                memcpy(id, result->command_id.bytes, 16);
                memcpy(message.device_id, result->device_id.bytes, 16);
                memcpy(message.payload.telemetry, id, 16);
                message.type = EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT;
                message.payload_size = 16;
                (void)send(acquisition->worker_fd, &message, acquisition_message_size(16),
                           MSG_DONTWAIT | MSG_NOSIGNAL);
            }
        } else if (message.type == EDGE_ACQUISITION_EVENT_STATUS &&
                   message.payload_size == sizeof(message.payload.status)) {
            for (size_t index = 0U; index < acquisition->platform_count; ++index)
                if (memcmp(acquisition->platform_ids[index], message.platform_id,
                           16U) == 0) {
                    acquisition->cached_status[index] = message.payload.status;
                    break;
                }
        }
    }
}

void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms) {
    if (acquisition == NULL || acquisition->worker_child)
        return;
    drain_worker(acquisition, now_ms);
    if (acquisition->worker_pid > 0 && acquisition->worker_fd >= 0 &&
        now_ms - acquisition->worker_last_event_ms >= EDGE_ACQUISITION_WATCHDOG_MS) {
        edge_log_write("error", "acquisition", "acquisition worker stalled",
                       "watchdog=120s action=restart");
        edge_log_local_fault(NULL, NULL, 0U, EDGE_LOCAL_FAULT_NETWORK, 6U);
        (void)kill(acquisition->worker_pid, SIGKILL);
        close(acquisition->worker_fd);
        acquisition->worker_fd = -1;
    }
    if (acquisition->worker_pid > 0 && acquisition->worker_fd < 0) {
        int status = 0;
        const pid_t waited = waitpid(acquisition->worker_pid, &status, WNOHANG);
        if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
            worker_reaped(acquisition, now_ms);
    }
    if (acquisition->worker_required && acquisition->worker_pid == 0 &&
        now_ms >= acquisition->worker_restart_at_ms) {
        char error[128];
        if (!start_worker(acquisition, error, sizeof(error)))
            acquisition->worker_restart_at_ms = now_ms + EDGE_ACQUISITION_RESTART_MS;
    }
}

void edge_acquisition_status(edge_acquisition *acquisition,
                             iot_edge_v1_DeviceStatusReport *report) {
    const uint8_t platform_id[16] = {0};
    edge_acquisition_status_for_platform(acquisition, platform_id, report);
}

void edge_acquisition_status_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    iot_edge_v1_DeviceStatusReport *report) {
    if (acquisition == NULL || platform_id == NULL || report == NULL)
        return;
    *report = (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    for (size_t index = 0U; index < acquisition->platform_count; ++index)
        if (memcmp(acquisition->platform_ids[index], platform_id, 16U) == 0) {
            *report = acquisition->cached_status[index];
            return;
        }
}

bool edge_acquisition_command_for_platform_until(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_CommandRequest *request,
    uint64_t start_before_monotonic_ms, uint64_t mapping_valid_until_monotonic_ms,
    char *error, size_t error_size) {
    const uint64_t now = monotonic_milliseconds();
    if (request == NULL || !request->has_start_before_ms ||
        start_before_monotonic_ms == 0U || mapping_valid_until_monotonic_ms == 0U ||
        now == 0U || now >= start_before_monotonic_ms ||
        now >= mapping_valid_until_monotonic_ms) {
        set_error(error, error_size,
                  "command start deadline or database-time mapping is missing, invalid, or expired");
        return false;
    }
    if (acquisition == NULL || acquisition->worker_pid <= 0 ||
        acquisition->worker_fd < 0 || platform_id == NULL) {
        set_error(error, error_size, "acquisition worker is unavailable");
        return false;
    }
    const uint32_t command_clock_generation =
        command_clock_current_generation(acquisition, platform_id);
    if (command_clock_generation == 0U) {
        set_error(error, error_size, "platform database-time mapping is unavailable");
        return false;
    }
    edge_acquisition_device *device = request->device_id.size == 16
        ? find_device(acquisition, platform_id, request->device_id.bytes) : NULL;
    if (request->external_io) {
        if (request->node_io_channel[0] ? !node_io_request_valid(acquisition, request) : !external_io_command_point(device, request)) {
            set_error(error, error_size, "external IO point is unavailable or not writable");
            return false;
        }
    } else if (!device || !device->sl651) {
        edge_write_command command;
        if (!build_write_command(acquisition, platform_id, request,
                                 start_before_monotonic_ms,
                                 mapping_valid_until_monotonic_ms,
                                 command_clock_generation, &device, &command,
                                 error, error_size))
            return false;
    } else if (request->command_id.size != 16 || !request->values_count ||
               request->values_count > 8) {
        set_error(error, error_size, "SL651 command requires configured values");
        return false;
    }
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_CONTROL_COMMAND,
                             sizeof(message.payload.command_request));
    message.command_start_before_monotonic_ms = start_before_monotonic_ms;
    message.mapping_valid_until_monotonic_ms = mapping_valid_until_monotonic_ms;
    message.command_clock_generation = command_clock_generation;
    memcpy(message.platform_id, platform_id, 16U);
    message.payload.command_request = *request;
    const size_t size = acquisition_message_size(message.payload_size);
    const ssize_t sent = send(acquisition->worker_fd, &message, size,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == (ssize_t)size)
        return true;
    set_error(error, error_size,
              sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
                  ? "acquisition worker command queue is full"
                  : "cannot send command to acquisition worker");
    return false;
}

bool edge_acquisition_command_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_CommandRequest *request,
    char *error, size_t error_size) {
    return edge_acquisition_command_for_platform_until(
        acquisition, platform_id, request, 0U, 0U, error, error_size);
}

bool edge_acquisition_command(edge_acquisition *acquisition,
                              const iot_edge_v1_CommandRequest *request,
                              char *error, size_t error_size) {
    const uint8_t platform_id[16] = {0};
    return edge_acquisition_command_for_platform(acquisition, platform_id, request,
                                                 error, error_size);
}

void edge_acquisition_invalidate_command_clock(
    edge_acquisition *acquisition, const uint8_t platform_id[16]) {
    const size_t index = command_clock_platform_index(acquisition, platform_id);
    if (index == SIZE_MAX || acquisition->command_clock_generations == NULL)
        return;
    /* Zero is reserved: wrapping UINT32_MAX disables this mapping permanently. */
    _Atomic uint32_t *generation =
        &acquisition->command_clock_generations[index].generation;
    if (atomic_load_explicit(generation, memory_order_seq_cst) != 0U)
        (void)atomic_fetch_add_explicit(generation, 1U, memory_order_seq_cst);
}

void edge_acquisition_stop(edge_acquisition *acquisition) {
    if (acquisition == NULL || acquisition->worker_child)
        return;
    if (acquisition->worker_pid <= 0)
        return;
    if (acquisition->worker_fd >= 0) {
        edge_acquisition_message message;
        acquisition_init_message(&message, EDGE_ACQUISITION_CONTROL_STOP, 0U);
        (void)send(acquisition->worker_fd, &message,
                   acquisition_message_size(0U), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
        pid_t waited;
        do {
            waited = waitpid(acquisition->worker_pid, NULL, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
            goto stopped;
        usleep(5000U);
    }
    (void)kill(acquisition->worker_pid, SIGTERM);
    for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
        const pid_t waited = waitpid(acquisition->worker_pid, NULL, WNOHANG);
        if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
            goto stopped;
        usleep(5000U);
    }
    (void)kill(acquisition->worker_pid, SIGKILL);
    while (waitpid(acquisition->worker_pid, NULL, 0) < 0 && errno == EINTR) {
    }
stopped:
    if (acquisition->worker_fd >= 0)
        close(acquisition->worker_fd);
    acquisition->worker_fd = -1;
    acquisition->worker_pid = 0;
}

void edge_acquisition_destroy(edge_acquisition *acquisition) {
    if (acquisition == NULL)
        return;
    edge_acquisition_stop(acquisition);
    free_devices(acquisition->devices, acquisition->device_count);
    free_links(acquisition->links, acquisition->link_count);
    if (acquisition->command_clock_generations != NULL)
        (void)munmap(acquisition->command_clock_generations,
                     4U * sizeof(*acquisition->command_clock_generations));
    free(acquisition);
}

size_t edge_acquisition_device_count(const edge_acquisition *acquisition) {
    return acquisition != NULL ? acquisition->device_count : 0U;
}

size_t edge_acquisition_resource_count(const edge_acquisition *acquisition) {
    return acquisition != NULL ? acquisition->link_count : 0U;
}
