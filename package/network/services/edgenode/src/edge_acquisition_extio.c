#define _GNU_SOURCE

#include "edge_acquisition_internal.h"

void edge_acquisition_set_io_callback(edge_acquisition *acquisition,
                                      edge_acquisition_io_callback callback) {
    if (acquisition) acquisition->io_callback = callback;
}

const edge_external_io_snapshot *edge_acquisition_io_capability(const edge_acquisition *acquisition) {
    return acquisition ? &acquisition->io_capability : NULL;
}

const iot_edge_v1_ExternalIoPointConfig *external_io_command_point(
    edge_acquisition_device *device, const iot_edge_v1_CommandRequest *request) {
    if (!device || !device->external_io_enabled || !request || !request->external_io ||
        request->command_id.size != 16U || request->values_count != 1U || !request->values[0].has_expected ||
        request->values[0].expected.which_value != iot_edge_v1_ScalarValue_string_value_tag ||
        (strcmp(request->values[0].expected.value.string_value, "0") && strcmp(request->values[0].expected.value.string_value, "1"))) return NULL;
    for (size_t i = 0; i < device->external_io_count; ++i)
        if (device->external_io[i]->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO &&
            !strcmp(device->external_io[i]->point_id, request->values[0].element_id)) return device->external_io[i];
    return NULL;
}

size_t node_io_platform_index(const edge_acquisition *acquisition, const uint8_t platform_id[16]) {
    for (size_t i = 0; i < acquisition->platform_count; ++i)
        if (!memcmp(acquisition->platform_ids[i], platform_id, 16U)) return i;
    return 4U;
}

static bool node_io_channel_unbound(const edge_acquisition *acquisition, const char *channel) {
    for (size_t i = 0; i < acquisition->device_count; ++i)
        for (size_t j = 0; j < acquisition->devices[i].external_io_count; ++j)
            if (!strcmp(acquisition->devices[i].external_io[j]->channel, channel)) return false;
    return true;
}

bool node_io_request_valid(const edge_acquisition *acquisition, const iot_edge_v1_CommandRequest *request) {
    if (!request->external_io || request->device_id.size || request->command_id.size != 16U || request->values_count != 1U ||
        !request->values[0].has_expected || request->values[0].expected.which_value != iot_edge_v1_ScalarValue_string_value_tag ||
        strcmp(request->values[0].element_id, request->node_io_channel) ||
        (strcmp(request->values[0].expected.value.string_value, "0") && strcmp(request->values[0].expected.value.string_value, "1")) ||
        !node_io_channel_unbound(acquisition, request->node_io_channel)) return false;
    for (size_t i = 0; i < acquisition->io_capability.count; ++i) {
        const edge_external_io_channel *channel = &acquisition->io_capability.channels[i];
        if (!strcmp(channel->channel, request->node_io_channel))
            return channel->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO && channel->available && channel->controllable;
    }
    return false;
}

typedef struct {
    edge_acquisition *acquisition;
    const uint8_t *platform_id;
    uint64_t deadline;
    uint64_t mapping_valid_until;
    uint32_t generation;
    const char *node_channel;
} external_io_write_guard;

static bool external_io_write_permitted(void *context) {
    const external_io_write_guard *guard = context;
    const uint64_t now = monotonic_milliseconds();
    return (!guard->node_channel || node_io_channel_unbound(guard->acquisition, guard->node_channel)) && now && now < guard->deadline && now < guard->mapping_valid_until &&
        command_clock_generation_is_current(guard->acquisition, guard->platform_id, guard->generation);
}

bool external_io_command(edge_acquisition *acquisition, const edge_acquisition_message *message) {
    const iot_edge_v1_CommandRequest *request = &message->payload.command_request;
    edge_acquisition_device *device = request->device_id.size == 16U ? find_device(acquisition, message->platform_id, request->device_id.bytes) : NULL;
    const bool node_command = request->node_io_channel[0] != 0;
    const size_t platform = node_io_platform_index(acquisition, message->platform_id);
    const iot_edge_v1_ExternalIoPointConfig *point = node_command ? NULL : external_io_command_point(device, request);
    if (node_command && platform < 4U && acquisition->node_io[platform].pending &&
        !memcmp(acquisition->node_io[platform].result.command_id.bytes, request->command_id.bytes, 16U)) return true;
    if (acquisition->io_quiesce_requested || (node_command ? platform >= 4U || acquisition->node_io[platform].pending || !node_io_request_valid(acquisition, request) : !point || device->external_io_result)) return false;
    external_io_write_guard guard = { acquisition, message->platform_id, message->command_start_before_monotonic_ms,
                                     message->mapping_valid_until_monotonic_ms, message->command_clock_generation, node_command ? request->node_io_channel : NULL };
    if (!external_io_write_permitted(&guard)) return false;
    iot_edge_v1_CommandResult *result = calloc(1, sizeof(*result));
    if (!result) return false;
    edge_device_runtime *commands = node_command ? &acquisition->node_io[platform].commands : &device->runtime;
    if (!edge_device_runtime_claim_command_id(commands, request->command_id.bytes)) { free(result); return false; }
    result->command_id.size = 16U;
    memcpy(result->command_id.bytes, request->command_id.bytes, 16U);
    if (node_command) copy_text(result->node_io_channel, sizeof(result->node_io_channel), request->node_io_channel);
    else {
        result->device_id.size = 16U;
        memcpy(result->device_id.bytes, request->device_id.bytes, 16U);
    }
    const char *channel = node_command ? request->node_io_channel : point->channel;
    const bool expected = !strcmp(request->values[0].expected.value.string_value, "1");
    bool started = false, actual = false;
    const bool success = edge_external_io_write(NULL, channel, expected, external_io_write_permitted, &guard, &started, &actual);
    result->state = success ? iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED :
        started ? iot_edge_v1_CommandState_COMMAND_STATE_FAILED : iot_edge_v1_CommandState_COMMAND_STATE_REJECTED;
    result->write_ack_missing = started && !success;
    result->completed_at_ms = current_ms();
    copy_text(result->message, sizeof(result->message), success ? "external IO output readback confirmed" :
              started ? "external IO write result is uncertain" : "external IO write was not started");
    if (success) {
        result->actual_values_count = 1U;
        iot_edge_v1_TelemetryValue *value = &result->actual_values[0];
        copy_text(value->element_id, sizeof(value->element_id), node_command ? channel : point->point_id);
        copy_text(value->name, sizeof(value->name), node_command ? channel : point->name);
        value->has_value = true;
        value->value.kind = iot_edge_v1_ValueKind_VALUE_BOOL;
        value->value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
        value->value.value.bool_value = actual;
        if (!node_command)
            (void)update_derived(device, value, 1U, result->completed_at_ms);
    }
    if (node_command) {
        acquisition->node_io[platform].result = *result;
        acquisition->node_io[platform].pending = true;
        acquisition->node_io[platform].retry_ms = 0;
        free(result);
    } else {
        device->external_io_result = result;
        device->external_io_result_retry_ms = 0;
    }
    acquisition->io_next_read_ms = 0;
    return true;
}

bool edge_acquisition_set_external_io(edge_acquisition *acquisition,
                                      const uint8_t platform_id[16], bool enabled) {
    if (!acquisition || !platform_id) return false;
    if (acquisition->worker_pid > 0) {
        edge_acquisition_message message;
        acquisition_init_message(&message, EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_ENABLE, 1U);
        memcpy(message.platform_id, platform_id, 16U);
        message.payload.telemetry[0] = enabled ? 1 : 0;
        const size_t size = acquisition_message_size(1U);
        if (send(acquisition->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)size) return false;
    }
    for (size_t i = 0; i < acquisition->device_count; ++i)
        if (!memcmp(acquisition->devices[i].platform_id, platform_id, 16U))
            acquisition->devices[i].external_io_enabled = enabled;
    return true;
}

static bool io_capability_changed(const edge_external_io_snapshot *left, const edge_external_io_snapshot *right) {
    if (left->supported != right->supported || left->count != right->count) return true;
    for (size_t i = 0; i < left->count; ++i) {
        const edge_external_io_channel *a = &left->channels[i], *b = &right->channels[i];
        if (strcmp(a->channel, b->channel) || a->kind != b->kind || a->available != b->available || a->controllable != b->controllable || (a->available && a->value != b->value)) return true;
    }
    return false;
}

static bool read_external_io(edge_acquisition *acquisition, uint64_t now, edge_external_io_snapshot *snapshot) {
    const bool connected = edge_external_io_read(NULL, snapshot);
    if (!connected) {
        *snapshot = acquisition->io_capability;
        for (size_t i = 0; i < snapshot->count; ++i) {
            snapshot->channels[i].available = false;
            snapshot->channels[i].controllable = false;
        }
        acquisition->io_reconnect_ms = now + 5000U;
    }
    acquisition->io_connected = connected;
    acquisition->io_next_read_ms = now + 1000U;
    if (!acquisition->io_capability_seen || io_capability_changed(&acquisition->io_capability, snapshot)) {
        acquisition->io_capability = *snapshot;
        acquisition->io_capability_seen = true;
        if (acquisition->platform_count)
            (void)worker_send(acquisition, EDGE_ACQUISITION_EVENT_IO_CAPABILITY,
                              acquisition->platform_ids[0], snapshot, sizeof(*snapshot));
    }
    return connected;
}

static bool prepare_external_io(edge_acquisition_device *device, const edge_external_io_snapshot *snapshot, uint64_t now) {
    iot_edge_v1_TelemetryValue values[15] = {0};
    bool any_available = false;
    for (size_t i = 0; i < device->external_io_count; ++i) {
        const iot_edge_v1_ExternalIoPointConfig *point = device->external_io[i];
        iot_edge_v1_TelemetryValue *value = &values[i];
        copy_text(value->element_id, sizeof(value->element_id), point->point_id);
        copy_text(value->name, sizeof(value->name), point->name);
        copy_text(value->unit, sizeof(value->unit), point->unit);
        value->hidden = point->hidden;
        for (size_t j = 0; j < snapshot->count; ++j) {
            const edge_external_io_channel *channel = &snapshot->channels[j];
            if (strcmp(channel->channel, point->channel) || channel->kind != point->kind || !channel->available) continue;
            value->has_value = true;
            if (point->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI) {
                value->value.kind = iot_edge_v1_ValueKind_VALUE_DOUBLE;
                value->value.which_value = iot_edge_v1_ScalarValue_double_value_tag;
                value->value.value.double_value = (double)channel->value / 1000.0;
            } else {
                value->value.kind = iot_edge_v1_ValueKind_VALUE_BOOL;
                value->value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
                value->value.value.bool_value = channel->value != 0;
            }
            any_available = true;
            break;
        }
    }
    for (size_t i = 0; i < device->external_io_count; ++i)
        publish_gateway_value(device, &values[i], current_ms());
    (void)update_derived(device, values, device->external_io_count, current_ms());
    const uint64_t interval = device->config->io_interval_ms ? device->config->io_interval_ms : (uint64_t)device->config->report_interval_sec * 1000U;
    device->external_io_next_ms = now + interval;
    if (!any_available && device->external_io_unavailable) return true;
    device->external_io_unavailable = !any_available;
    iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
    record.record_id.size = record.device_id.size = record.endpoint_id.size = record.model_id.size = 16U;
    record_id((uint64_t)current_ms(), record.record_id.bytes);
    memcpy(record.device_id.bytes, device->config->device_id.bytes, 16U);
    memcpy(record.endpoint_id.bytes, device->config->endpoint_id.bytes, 16U);
    memcpy(record.model_id.bytes, device->external_io[0]->model_id.bytes, 16U);
    record.protocol = iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO;
    record.configuration_sha256.size = 32U;
    memcpy(record.configuration_sha256.bytes, device->configuration_digest, 32U);
    record.observed_at_ms = current_ms();
    record.values = values;
    record.values_count = (pb_size_t)device->external_io_count;
    copy_text(record.direction, sizeof(record.direction), "UP");
    size_t size = 0;
    if (!pb_get_encoded_size(&size, iot_edge_v1_TelemetryRecord_fields, &record) || size > 14000U) return false;
    uint8_t *wire = malloc(size);
    if (!wire) return false;
    pb_ostream_t stream = pb_ostream_from_buffer(wire, size);
    if (!pb_encode(&stream, iot_edge_v1_TelemetryRecord_fields, &record)) { free(wire); return false; }
    device->external_io_pending = wire;
    device->external_io_pending_size = stream.bytes_written;
    memcpy(device->external_io_pending_id, record.record_id.bytes, 16U);
    device->external_io_retry_ms = now;
    return true;
}

void external_io_tick(edge_acquisition *acquisition, uint64_t now) {
    bool due = false;
    for (size_t i = 0; i < acquisition->device_count; ++i) {
        edge_acquisition_device *device = &acquisition->devices[i];
        if (!acquisition->io_quiesce_requested && device->external_io_enabled && device->external_io_count && !device->external_io_pending && now >= device->external_io_next_ms) due = true;
    }
    if (due || (!acquisition->io_quiesce_requested && now >= acquisition->io_next_read_ms && (acquisition->io_connected || now >= acquisition->io_reconnect_ms) && (acquisition->io_enabled || access("/etc/init.d/dtu-io", X_OK) == 0))) {
        edge_external_io_snapshot snapshot = {0};
        (void)read_external_io(acquisition, now, &snapshot);
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *device = &acquisition->devices[i];
            if (!acquisition->io_quiesce_requested && device->external_io_enabled && device->external_io_count && !device->external_io_pending && now >= device->external_io_next_ms &&
                !prepare_external_io(device, &snapshot, now)) {
                edge_log_local_fault(device->platform_id, device->config->device_id.bytes, 0U, EDGE_LOCAL_FAULT_WORKER_ENCODE, 3U);
            }
        }
    }
    for (size_t i = 0; i < acquisition->device_count; ++i) {
        edge_acquisition_device *device = &acquisition->devices[i];
        if (device->external_io_result && now >= device->external_io_result_retry_ms) {
            (void)worker_send(acquisition, EDGE_ACQUISITION_EVENT_EXTERNAL_IO_COMMAND, device->platform_id,
                              device->external_io_result, sizeof(*device->external_io_result));
            device->external_io_result_retry_ms = now + 1000U;
        }
        if (!device->external_io_enabled || !device->external_io_pending || now < device->external_io_retry_ms) continue;
        (void)worker_send(acquisition, EDGE_ACQUISITION_EVENT_EXTERNAL_IO, device->platform_id,
                          device->external_io_pending, (uint32_t)device->external_io_pending_size);
        device->external_io_retry_ms = now + 1000U;
    }
    for (size_t i = 0; i < acquisition->platform_count; ++i) {
        if (acquisition->node_io[i].pending && now >= acquisition->node_io[i].retry_ms) {
            (void)worker_send(acquisition, EDGE_ACQUISITION_EVENT_EXTERNAL_IO_COMMAND, acquisition->platform_ids[i], &acquisition->node_io[i].result, sizeof(acquisition->node_io[i].result));
            acquisition->node_io[i].retry_ms = now + 1000U;
        }
    }
    if (acquisition->io_quiesce_requested && !acquisition->io_quiesce_sent) {
        bool pending = false;
        for (size_t i = 0; i < acquisition->platform_count; ++i) pending = pending || acquisition->node_io[i].pending;
        for (size_t i = 0; i < acquisition->device_count; ++i)
            pending = pending || acquisition->devices[i].external_io_pending || acquisition->devices[i].external_io_result;
        if (!pending && acquisition->platform_count) {
            edge_acquisition_message ready;
            acquisition_init_message(&ready, EDGE_ACQUISITION_EVENT_IO_QUIESCED, 1U);
            ready.report_token = acquisition->io_quiesce_token;
            memcpy(ready.platform_id, acquisition->platform_ids[0], 16U);
            ready.payload.telemetry[0] = 1;
            const size_t ready_size = acquisition_message_size(1U);
            acquisition->io_quiesce_sent = send(acquisition->worker_fd, &ready, ready_size, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)ready_size;
        }
    }
}

bool edge_acquisition_prepare_io_swap(edge_acquisition *acquisition) {
    if (!acquisition || acquisition->worker_pid <= 0 || acquisition->worker_fd < 0) return acquisition != NULL;
    ++acquisition->io_quiesce_token;
    if (!acquisition->io_quiesce_token) ++acquisition->io_quiesce_token;
    acquisition->io_quiesced_ack = false;
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_CONTROL_IO_QUIESCE, 0U);
    message.report_token = acquisition->io_quiesce_token;
    if (send(acquisition->worker_fd, &message, acquisition_message_size(0U), MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)acquisition_message_size(0U)) return false;
    /* Allow the current bounded I/O exchange and one outbox retry to finish. */
    const uint64_t deadline = monotonic_milliseconds() + EDGE_IO_TIMEOUT_MS + 1200U;
    while (monotonic_milliseconds() < deadline) {
        struct pollfd event = {.fd = acquisition->worker_fd, .events = POLLIN};
        (void)poll(&event, 1U, 10);
        drain_worker(acquisition, monotonic_milliseconds());
        if (acquisition->io_quiesced_ack) return true;
        if (acquisition->worker_fd < 0) return false;
    }
    message.type = EDGE_ACQUISITION_CONTROL_IO_RESUME;
    (void)send(acquisition->worker_fd, &message, acquisition_message_size(0U), MSG_DONTWAIT | MSG_NOSIGNAL);
    return false;
}
