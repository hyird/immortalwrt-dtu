#include "edge_modbus_gateway.h"
#include "edge_protocol.h"
#include "edge_ws.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <pb_encode.h>

#include "edge_capability.h"
#include "edge_firmware.h"
#include "edge_modem.h"
#include "edge_network.h"
#include "edge_process.h"
#include "edge_sha256.h"
#include "edge_status.h"
#include "edge_terminal.h"
#include "edge_url.h"
#include "edge_version.h"
#include "edge_vpn.h"
#include "log.h"

#define EDGE_OUTBOX_WINDOW 16U
#define EDGE_CONNECT_TIMEOUT_SEC 30U
#define EDGE_APPLICATION_HANDSHAKE_TIMEOUT_MS 30000U
#define EDGE_OUTBOX_ACK_TIMEOUT_MS 60000U
#define EDGE_LIVENESS_CHECK_INTERVAL_SEC 1.0
#define EDGE_TERMINAL_OUTPUT_POLL_INTERVAL 0.01
#define EDGE_TERMINAL_ACK_TIMEOUT_MS 15000U

static void send_pending_modem_result(edge_ws_session *session);
static bool send_capability_report(edge_ws_session *session, bool force);
static bool send_io_status(edge_ws_session *session);
static void gateway_commands_stopped(edge_ws_app *app);

static void invalidate_command_clock(edge_ws_session *session) {
    const bool was_valid = session->command_clock.valid;
    edge_command_clock_invalidate(&session->command_clock);
    if (was_valid)
        edge_acquisition_invalidate_command_clock(session->app->acquisition,
                                                 session->config->id);
}

static void reset_terminal_flow(edge_ws_session *session) {
    memset(session->terminal_output, 0, sizeof(session->terminal_output));
    session->terminal_output_size = 0U;
    session->terminal_output_sequence = 0U;
    session->terminal_output_acked_sequence = 0U;
    session->terminal_output_deadline_ms = 0U;
    session->terminal_input_ack_sequence = 0U;
    session->terminal_output_pending = false;
    session->terminal_output_sent = false;
    session->terminal_input_ack_pending = false;
}

static void traffic_ready(struct ev_loop *loop, struct ev_io *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_app *app = (edge_ws_app *)((uint8_t *)watcher - offsetof(edge_ws_app, traffic_io));
    edge_traffic_drain(&app->traffic);
}

static edge_ws_session *session_from_reconnect(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reconnect_timer));
}

static edge_ws_session *session_from_liveness(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, liveness_timer));
}

static edge_ws_session *session_from_status_report(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, status_report_timer));
}

static edge_ws_session *session_from_firmware(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, firmware_timer));
}

static edge_ws_session *session_from_modem(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, modem_timer));
}

static edge_ws_session *session_from_modem_io(struct ev_io *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, modem_io));
}

static edge_ws_session *session_from_modem_child(struct ev_child *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, modem_child));
}

static edge_ws_session *session_from_network(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, network_timer));
}

static edge_ws_session *session_from_reload(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reload_timer));
}

static edge_ws_session *session_from_terminal(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, terminal_timer));
}

static edge_ws_app *app_from_acquisition(struct ev_timer *timer) {
    return (edge_ws_app *)((uint8_t *)timer - offsetof(edge_ws_app, acquisition_timer));
}

static edge_ws_app *app_from_status(struct ev_timer *timer) {
    return (edge_ws_app *)((uint8_t *)timer - offsetof(edge_ws_app, status_timer));
}

static edge_ws_app *app_from_acquisition_io(struct ev_io *watcher) {
    return (edge_ws_app *)((uint8_t *)watcher - offsetof(edge_ws_app, acquisition_io));
}

static edge_ws_session *session_for_platform(edge_ws_app *app,
                                             const uint8_t platform_id[16]) {
    if (app == NULL || platform_id == NULL)
        return NULL;
    for (size_t index = 0U; index < app->config->platform_count; ++index)
        if (memcmp(app->sessions[index].config->id, platform_id, 16U) == 0)
            return &app->sessions[index];
    return NULL;
}

static const char *command_state_name(iot_edge_v1_CommandState state) {
    switch (state) {
    case iot_edge_v1_CommandState_COMMAND_STATE_ACCEPTED:
        return "accepted";
    case iot_edge_v1_CommandState_COMMAND_STATE_RUNNING:
        return "running";
    case iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED:
        return "succeeded";
    case iot_edge_v1_CommandState_COMMAND_STATE_READBACK_MISMATCH:
        return "readback-mismatch";
    case iot_edge_v1_CommandState_COMMAND_STATE_DEVICE_OFFLINE:
        return "device-offline";
    case iot_edge_v1_CommandState_COMMAND_STATE_TIMED_OUT:
        return "timed-out";
    case iot_edge_v1_CommandState_COMMAND_STATE_REJECTED:
        return "rejected";
    case iot_edge_v1_CommandState_COMMAND_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static const char *modem_state_name(iot_edge_v1_ModemControlState state) {
    switch (state) {
    case iot_edge_v1_ModemControlState_MODEM_CONTROL_ACCEPTED:
        return "accepted";
    case iot_edge_v1_ModemControlState_MODEM_CONTROL_RUNNING:
        return "running";
    case iot_edge_v1_ModemControlState_MODEM_CONTROL_SUCCEEDED:
        return "succeeded";
    case iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static const char *firmware_state_name(iot_edge_v1_FirmwareUpdateState state) {
    switch (state) {
    case iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_ACCEPTED:
        return "accepted";
    case iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_DOWNLOADING:
        return "receiving";
    case iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_VERIFYING:
        return "verifying";
    case iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FLASHING:
        return "flashing";
    case iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void log_uuid_task(const edge_ws_session *session, const char *type,
                          const uint8_t id[16], const char *state,
                          const char *summary) {
    char uuid[37];
    edge_config_format_uuid(id, uuid);
    char detail[257];
    snprintf(detail, sizeof(detail),
             "platform=%s id=%s state=%s %s",
             session->config->name, uuid, state != NULL ? state : "unknown",
             summary != NULL ? summary : "");
    edge_log_write(
        state != NULL &&
                (strcmp(state, "failed") == 0 || strcmp(state, "rejected") == 0 ||
                 strcmp(state, "timed-out") == 0 ||
                 strcmp(state, "readback-mismatch") == 0)
            ? "warn"
            : "info",
        "task", type, detail);
}

static void sync_acquisition_io(edge_ws_app *app) {
    const int fd = edge_acquisition_event_fd(app->acquisition);
    if (ev_is_active(&app->acquisition_io) && app->acquisition_io.fd == fd)
        return;
    ev_io_stop(app->loop, &app->acquisition_io);
    if (fd >= 0) {
        ev_io_set(&app->acquisition_io, fd, EV_READ);
        ev_io_start(app->loop, &app->acquisition_io);
    }
}

static int64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static bool random_bytes(uint8_t *output, size_t size) {
    FILE *input = fopen("/dev/urandom", "rb");
    if (input == NULL)
        return false;
    const bool ok = fread(output, 1U, size, input) == size;
    fclose(input);
    return ok;
}

static void safe_copy(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static bool init_envelope(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    uint8_t random[10];
    if (!random_bytes(random, sizeof(random))) {
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_CONFIG, 8U);
        return false;
    }
    const bool initialized = edge_protocol_init_envelope(
        envelope, session->config->id,
        session->enrolled ? session->node_id : NULL,
        session->session_epoch, ++session->sequence, now_ms(), random);
    if (!initialized)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_CONFIG, 10U);
    return initialized;
}

static bool send_envelope(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    size_t wire_size = 0U;
    const char *error = NULL;
    if (!session->websocket_open ||
        !edge_protocol_encode(envelope, session->app->wire, sizeof(session->app->wire),
                              &wire_size, &error)) {
        syslog(LOG_ERR, "cannot encode edge envelope: %s", error != NULL ? error : "closed");
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK,
                             session->websocket_open ? 12U : 11U);
        return false;
    }
    const bool sent = edge_ws_transport_send(&session->transport, session->app->wire, wire_size);
    if (!sent)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK, 13U);
    /* Telemetry may update the same device state as a standalone snapshot. */
    if (sent && envelope->which_payload == iot_edge_v1_Envelope_telemetry_batch_tag)
        edge_report_free(&session->device_snapshot);
    return sent;
}

static bool send_report(edge_ws_session *session, iot_edge_v1_Envelope *envelope,
                         edge_report_snapshot *snapshot, bool force) {
    edge_report_snapshot candidate = {0};
    const edge_report_decision decision = edge_report_prepare(snapshot, envelope, force, &candidate);
    if (decision == EDGE_REPORT_UNCHANGED) return true;
    if (decision == EDGE_REPORT_ERROR) {
        /* Allocation/encoding failure and event-bearing reports must not hide changes. */
        edge_report_free(snapshot);
        return send_envelope(session, envelope);
    }
    const bool sent = send_envelope(session, envelope);
    if (sent) {
        edge_report_free(snapshot);
        *snapshot = candidate;
    } else {
        edge_report_free(&candidate);
    }
    return sent;
}

static void clear_report_snapshots(edge_ws_session *session) {
    edge_report_free(&session->capability_snapshot);
    edge_report_free(&session->device_snapshot);
    for (size_t i = 0; i < 8; ++i) edge_report_free(&session->dtu_snapshots[i]);
    session->capability_config_retry_done = false;
}

static void send_dtu_status(edge_ws_session *session) {
    if (!session->websocket_open || !session->enrolled) return;
    for (size_t i = 0; i < session->dtu_status_count; ++i) {
        if (!session->dtu_status_dirty[i]) continue;
        if (edge_ws_transport_queued_bytes(&session->transport) >= 65536) break;
        iot_edge_v1_Envelope *envelope = &session->app->envelope;
        if (!init_envelope(session, envelope)) return;
        envelope->which_payload = iot_edge_v1_Envelope_dtu_status_tag;
        envelope->payload.dtu_status = session->dtu_status[i];
        if (send_report(session, envelope, &session->dtu_snapshots[i], false))
            session->dtu_status_dirty[i] = false;
    }
}

static void acquisition_dtu(void *context, const uint8_t platform_id[16],
                            const iot_edge_v1_DtuStatus *status) {
    if (status->error[0] != '\0')
        edge_log_local_fault(platform_id, NULL, 0U,
                             EDGE_LOCAL_FAULT_DTU, 1U);
    edge_ws_app *app = context;
    edge_ws_session *session = session_for_platform(app, platform_id);
    if (!session) return;
    size_t index = 0;
    while (index < session->dtu_status_count &&
           memcmp(session->dtu_status[index].channel_id.bytes, status->channel_id.bytes, 16)) ++index;
    if (index >= 8) return;
    if (index == session->dtu_status_count) ++session->dtu_status_count;
    session->dtu_status[index] = *status;
    session->dtu_status_dirty[index] = true;
    send_dtu_status(session);
}

static void acquisition_io_capability(void *context, const edge_external_io_snapshot *snapshot) {
    (void)snapshot;
    edge_ws_app *app = context;
    for (size_t i = 0; i < app->config->platform_count; ++i) {
        edge_ws_session *session = &app->sessions[i];
        if (session->websocket_open && session->enrolled) {
            (void)send_capability_report(session, false);
            (void)send_io_status(session);
        }
    }
}

static void acquisition_serial(void *context, const uint8_t platform_id[16],
                               const iot_edge_v1_SerialDebugEvent *event) {
    edge_ws_app *app = context;
    edge_ws_session *session = session_for_platform(app, platform_id);
    if (!session || !session->websocket_open || !session->enrolled ||
        !session->serial_debug_active || event->session_id.size != 16 ||
        memcmp(session->serial_debug_id, event->session_id.bytes, 16)) return;
    session->serial_debug_sequence = event->sequence;
    if (!strcmp(event->kind, "closed")) session->serial_debug_active = false;
    // Sequence gaps identify discarded monitoring samples without blocking acquisition.
    if (edge_ws_transport_queued_bytes(&session->transport) >= 65536) return;
    iot_edge_v1_Envelope *envelope = &app->envelope;
    if (!init_envelope(session, envelope)) return;
    envelope->which_payload = iot_edge_v1_Envelope_serial_debug_event_tag;
    envelope->payload.serial_debug_event = *event;
    (void)send_envelope(session, envelope);
}

static void close_serial_debug(edge_ws_session *session, const char *reason) {
    if (!session->serial_debug_active) return;
    iot_edge_v1_SerialDebugRequest request = iot_edge_v1_SerialDebugRequest_init_zero;
    edge_protocol_set_bytes(&request.session_id, 16, session->serial_debug_id, 16);
    request.request_sequence = UINT64_MAX;
    safe_copy(request.action, sizeof(request.action), "close");
    (void)edge_acquisition_serial_request(session->app->acquisition, session->config->id, &request);
    iot_edge_v1_SerialDebugEvent event = iot_edge_v1_SerialDebugEvent_init_zero;
    edge_protocol_set_bytes(&event.session_id, 16, session->serial_debug_id, 16);
    event.sequence = ++session->serial_debug_sequence;
    event.occurred_at_ms = now_ms();
    safe_copy(event.kind, sizeof(event.kind), "closed");
    safe_copy(event.message, sizeof(event.message), reason);
    acquisition_serial(session->app, session->config->id, &event);
    session->serial_debug_active = false;
}

#ifndef EDGE_GATEWAY_MAP_PATH
#define EDGE_GATEWAY_MAP_PATH "/etc/edgenode-modbus.map"
#endif

static void export_gateway(edge_ws_app *app) {
    if (!app->gateway) {
        remove("/tmp/edgenode-modbus.csv");
        return;
    }
    if (app->gateway && !edge_modbus_gateway_export(app->gateway, "/tmp/edgenode-modbus.csv")) {
        remove("/tmp/edgenode-modbus.csv");
        syslog(LOG_ERR, "cannot export Modbus gateway register map");
    }
}

static void acquisition_sample(void *context, const uint8_t platform[16],
    const edge_acquisition_sample *sample) {
    edge_ws_app *app = context;
    edge_modbus_gateway_update(app->gateway, platform, sample, monotonic_ms());
    if (sample == NULL)
        gateway_commands_stopped(app);
}

static edge_modbus_gateway *prepare_gateway(const edge_acquisition_source *sources, size_t count, uint16_t port) {
    for (size_t s = 0; s < count; ++s) {
        const edge_runtime_config *config = sources[s].config;
        for (size_t i = 0; i < config->item_count; ++i) {
            const iot_edge_v1_ConfigItem *item = &config->items[i];
            if ((item->which_item == iot_edge_v1_ConfigItem_endpoint_tag &&
                 item->item.endpoint.enabled && item->item.endpoint.mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
                 item->item.endpoint.port == port) ||
                (item->which_item == iot_edge_v1_ConfigItem_dtu_tag && item->item.dtu.enabled &&
                 item->item.dtu.south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
                 item->item.dtu.south_port == port)) return NULL;
        }
    }
    return edge_modbus_gateway_prepare(EDGE_GATEWAY_MAP_PATH, sources, count);
}

static void acquisition_debug(void *context, const uint8_t platform_id[16], const iot_edge_v1_RawPacket *packet) {
    edge_ws_app *app = context;
    edge_ws_session *session = session_for_platform(app, platform_id);
    if (!session || !session->websocket_open || !session->enrolled) return;
    iot_edge_v1_Envelope *envelope = &app->envelope;
    if (!init_envelope(session, envelope)) return;
    envelope->which_payload = iot_edge_v1_Envelope_raw_packet_tag;
    envelope->payload.raw_packet = *packet;
    (void)send_envelope(session, envelope);
}

static bool acquisition_telemetry(void *context,
                                  const uint8_t platform_id[16],
                                  const iot_edge_v1_TelemetryRecord *record) {
    edge_ws_app *app = context;
    edge_ws_session *session = session_for_platform(app, platform_id);
    const uint8_t *device_id = record != NULL && record->device_id.size == 16U
        ? record->device_id.bytes : NULL;
    const unsigned protocol = record != NULL ? (unsigned)record->protocol : 0U;
    if (session == NULL || record == NULL || record->record_id.size != 16U) {
        edge_log_local_fault(platform_id, device_id, protocol,
                             EDGE_LOCAL_FAULT_PARENT_OUTBOX, 1U);
        return false;
    }
    uint8_t record_id[16];
    memcpy(record_id, record->record_id.bytes, sizeof(record_id));
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope)) {
        edge_log_local_fault(platform_id, device_id, protocol,
                             EDGE_LOCAL_FAULT_PARENT_OUTBOX, 2U);
        return false;
    }
    envelope->which_payload = iot_edge_v1_Envelope_telemetry_batch_tag;
    envelope->payload.telemetry_batch.records_count = 1U;
    envelope->payload.telemetry_batch.records[0] = *record;
    const bool queued = edge_ws_app_enqueue(session->app, session->config->id, record_id, envelope);
    // The callback borrows values from the acquisition IPC decoder.
    envelope->payload.telemetry_batch.records[0].values = NULL;
    envelope->payload.telemetry_batch.records[0].values_count = 0U;
    envelope->payload.telemetry_batch.records[0].raw_payloads = NULL;
    envelope->payload.telemetry_batch.records[0].raw_payloads_count = 0U;
    edge_log_local_fault(platform_id, device_id, protocol,
                         EDGE_LOCAL_FAULT_PARENT_OUTBOX, queued ? 0U : 3U);
    return queued;
}

static uint8_t gateway_write(void *context, const edge_modbus_gateway_write *write,
                              uint8_t command_id[16], uint64_t deadline_ms) {
    edge_ws_app *app = context;
    const uint64_t now = monotonic_ms();
    if (now == 0U || now >= deadline_ms)
        return 4U;
    edge_ws_gateway_command *pending = NULL;
    for (size_t i = 0U; i < 8U; ++i) {
        /* A rebound listener can still have an old real command outstanding.
         * Do not merge another partial write with its pre-readback cache. */
        if (app->gateway_commands[i].active)
            return 6U;
        if (!pending)
            pending = &app->gateway_commands[i];
    }
    if (!random_bytes(command_id, 16U))
        return 4U;
    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    request.command_id.size = request.device_id.size = 16U;
    memcpy(request.command_id.bytes, command_id, 16U);
    memcpy(request.device_id.bytes, write->device_id, 16U);
    request.values_count = 1U;
    safe_copy(request.values[0].element_id, sizeof(request.values[0].element_id), write->element_id);
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value = iot_edge_v1_ScalarValue_string_value_tag;
    safe_copy(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), write->value);
    request.external_io = write->external_io;
    const uint64_t remaining = deadline_ms - now;
    request.timeout_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    request.readback_count = 1U;
    request.has_start_before_ms = true;
    /* This is a trusted local monotonic deadline, not a remote DB timestamp.
     * The public acquisition API still stamps/checks the current generation. */
    request.start_before_ms = 1;
    pending->active = true;
    pending->external_io = write->external_io;
    memcpy(pending->command_id, command_id, 16U);
    memcpy(pending->platform_id, write->platform_id, 16U);
    memcpy(pending->device_id, write->device_id, 16U);
    char error[256] = {0};
    if (!edge_acquisition_command_for_platform_until(app->acquisition, write->platform_id,
            &request, deadline_ms, deadline_ms, error, sizeof(error))) {
        pending->active = false;
        return 6U;
    }
    return 0U;
}

static bool consume_gateway_result(edge_ws_app *app, const uint8_t platform_id[16],
                                     const iot_edge_v1_CommandResult *result) {
    if (!result || result->command_id.size != 16U || result->device_id.size != 16U)
        return false;
    for (size_t i = 0U; i < 8U; ++i) {
        edge_ws_gateway_command *pending = &app->gateway_commands[i];
        if (!pending->active || memcmp(pending->command_id, result->command_id.bytes, 16U) ||
            memcmp(pending->platform_id, platform_id, 16U) ||
            memcmp(pending->device_id, result->device_id.bytes, 16U))
            continue;
        const bool success = result->state == iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
        const bool external_io = pending->external_io;
        pending->active = false; /* Release before completion can submit the next point. */
        if (success && external_io) {
            /* Protocol reads already publish samples while parsing. External IO
             * reports the real output readback here, ahead of its ordinary period. */
            for (pb_size_t value = 0U; value < result->actual_values_count; ++value) {
                const iot_edge_v1_TelemetryValue *actual = &result->actual_values[value];
                edge_acquisition_sample sample = {.valid = actual->has_value,
                    .observed_at_ms = result->completed_at_ms, .value = actual->value};
                memcpy(sample.device_id, result->device_id.bytes, 16U);
                safe_copy(sample.element_id, sizeof(sample.element_id), actual->element_id);
                edge_modbus_gateway_update(app->gateway, platform_id, &sample, monotonic_ms());
            }
        }
        /* A replaced/destroyed server has no matching random command ID. */
        edge_modbus_gateway_write_result(app->gateway_server, result->command_id.bytes, success);
        return true;
    }
    return false;
}

static void gateway_commands_stopped(edge_ws_app *app) {
    /* Only after the old worker is definitely stopped and its IPC is closed:
     * no further result can arrive, so these IDs may now be retired. */
    for (size_t i = 0U; i < 8U; ++i) {
        edge_ws_gateway_command *pending = &app->gateway_commands[i];
        if (!pending->active)
            continue;
        pending->active = false;
        edge_modbus_gateway_write_result(app->gateway_server, pending->command_id, false);
    }
}

static bool acquisition_command_result(void *context,
                                       const uint8_t platform_id[16],
                                       const iot_edge_v1_CommandResult *result) {
    edge_ws_app *app = context;
    if (consume_gateway_result(app, platform_id, result))
        return true;
    edge_ws_session *session = session_for_platform(app, platform_id);
    const uint8_t *device_id = result != NULL && result->device_id.size == 16U
        ? result->device_id.bytes : NULL;
    if (session == NULL || result == NULL || result->command_id.size != 16U) {
        edge_log_local_fault(platform_id, device_id, 0U,
                             EDGE_LOCAL_FAULT_PARENT_OUTBOX, 4U);
        return false;
    }
    uint8_t command_id[16];
    memcpy(command_id, result->command_id.bytes, sizeof(command_id));
    char summary[160];
    snprintf(summary, sizeof(summary), "actualValues=%u message=%.120s",
             (unsigned)result->actual_values_count, result->message);
    log_uuid_task(session, "device-command", command_id,
                  command_state_name(result->state), summary);
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope)) {
        edge_log_local_fault(platform_id, device_id, 0U,
                             EDGE_LOCAL_FAULT_PARENT_OUTBOX, 5U);
        return false;
    }
    envelope->which_payload = iot_edge_v1_Envelope_command_result_tag;
    envelope->payload.command_result = *result;
    const bool queued = edge_ws_app_enqueue(session->app, session->config->id,
                                            command_id, envelope);
    if (!queued)
        edge_log_local_fault(platform_id, device_id, 0U,
                             EDGE_LOCAL_FAULT_PARENT_OUTBOX, 6U);
    return queued;
}

static void send_outbox_window(edge_ws_session *session) {
    if (!session->enrolled || !session->websocket_open)
        return;
    while (session->spool.outbox.in_flight < EDGE_OUTBOX_WINDOW) {
        /* Do not start ACK deadlines for an unbounded local socket backlog. */
        if (edge_ws_transport_queued_bytes(&session->transport) >= 65536) return;
        const edge_memory_message *message =
            edge_spool_outbox_next(&session->spool, monotonic_ms());
        if (message == NULL)
            return;
        uint8_t message_id[16];
        memcpy(message_id, message->message_id, sizeof(message_id));
        iot_edge_v1_Envelope *envelope = &session->app->envelope;
        const char *error = NULL;
        if (!edge_protocol_decode(message->payload, message->payload_size, envelope,
                                  &error)) {
            syslog(LOG_ERR, "cannot decode tmpfs outbox envelope: %s",
                   error != NULL ? error : "unknown error");
            edge_log_local_fault(session->config->id, NULL, 0U,
                                 EDGE_LOCAL_FAULT_OUTBOX_CORRUPT, 1U);
            (void)edge_spool_outbox_ack(&session->spool, message_id);
            continue;
        }
        edge_protocol_set_bytes(&envelope->node_id, sizeof(envelope->node_id.bytes),
                                session->node_id, sizeof(session->node_id));
        envelope->session_epoch = session->session_epoch;
        envelope->sequence = ++session->sequence;
        const iot_edge_v1_TelemetryRecord *record =
            envelope->which_payload == iot_edge_v1_Envelope_telemetry_batch_tag &&
            envelope->payload.telemetry_batch.records_count != 0U
                ? &envelope->payload.telemetry_batch.records[0] : NULL;
        const uint8_t *device_id = record != NULL && record->device_id.size == 16U
            ? record->device_id.bytes : NULL;
        const unsigned protocol = record != NULL ? (unsigned)record->protocol : 0U;
        const bool sent = send_envelope(session, envelope);
        if (!sent)
            edge_log_local_fault(session->config->id, device_id, protocol,
                                 EDGE_LOCAL_FAULT_OUTBOX_SEND, 1U);
        edge_protocol_release(envelope);
        if (!sent) {
            (void)edge_spool_outbox_retry(&session->spool, message_id);
            return;
        }
    }
}

static bool send_hello(edge_ws_session *session) {
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_hello_tag;
    iot_edge_v1_Hello *hello = &envelope->payload.hello;
    safe_copy(hello->imei, sizeof(hello->imei), session->app->config->imei);
    safe_copy(hello->model, sizeof(hello->model), session->app->config->model);
    safe_copy(hello->software_version, sizeof(hello->software_version), EDGE_SOFTWARE_VERSION);
    gethostname(hello->hostname, sizeof(hello->hostname) - 1U);
    struct utsname system;
    if (uname(&system) == 0) {
        safe_copy(hello->architecture, sizeof(hello->architecture), system.machine);
        safe_copy(hello->openwrt_release, sizeof(hello->openwrt_release), system.release);
    }
    hello->last_applied_config_version = session->active_revision;
    hello->supported_protocol_versions_count = 1U;
    hello->supported_protocol_versions[0] = EDGENODE_PROTOCOL_VERSION;
    hello->supports_tcp = true;
    hello->supports_serial = true;
    hello->supports_network_config = true;
    hello->supports_terminal = edge_capability_has_terminal();
    hello->supports_firmware_update = true;
    hello->supports_firmware_stream = true;
    hello->supports_sparse_heartbeat = true;
    hello->supports_status_reporting = true;
    hello->supports_command_start_before = true;
    hello->supports_device_config = true;
    hello->supports_raw_telemetry = true;
    hello->supports_external_io = access("/etc/init.d/dtu-io", X_OK) == 0;
    hello->supports_external_io_milliamps = hello->supports_external_io;
    hello->supports_modbus_gateway = true;
    hello->supports_modbus_gateway_order = true;
    hello->supports_modbus_gateway_config = true;
    hello->supports_node_io = hello->supports_external_io;
    hello->network_config_version = 3U;
    hello->supports_logs = true;
    safe_copy(hello->log_level, sizeof(hello->log_level), edge_log_level());
    hello->supports_modem_control =
        session->config->bootstrap && session->app->config->modem_at_port[0] != '\0';
    edge_modem_info modem;
    bool modem_available = false;
    hello->signal_csq = 99U;
    hello->signal_rssi_dbm = -1;
    hello->mobile_registration_status = -1;
    if (edge_modem_read_status(session->app->config->modem_status_path,
                               &modem, &modem_available)) {
        hello->modem_available = modem_available;
        safe_copy(hello->iccid, sizeof(hello->iccid), modem.iccid);
        hello->signal_csq = (uint32_t)modem.csq;
        hello->signal_rssi_dbm = modem.rssi_dbm;
        hello->signal_percent = modem.signal_percent;
        hello->mobile_registered = modem.registered;
        hello->mobile_registration_status = modem.registration_status;
        hello->sim_state = modem.sim_state;
        safe_copy(hello->apn, sizeof(hello->apn), modem.apn);
        safe_copy(hello->mobile_operator, sizeof(hello->mobile_operator),
                  modem.mobile_operator);
        hello->mobile_connected = modem.connected;
        safe_copy(hello->mobile_ipv4, sizeof(hello->mobile_ipv4), modem.mobile_ipv4);
    }
    session->hello_sent_monotonic_ms = monotonic_ms();
    const bool sent = send_envelope(session, envelope);
    if (!sent || session->hello_sent_monotonic_ms == 0U) {
        session->hello_sent_monotonic_ms = 0U;
        invalidate_command_clock(session);
    }
    return sent;
}

static bool send_io_status(edge_ws_session *session) {
    if (!session->node_io || !session->websocket_open || !session->enrolled) return false;
    const edge_external_io_snapshot *io = edge_acquisition_io_capability(session->app->acquisition);
    if (!io || !io->supported) return false;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session,output)) return false;
    output->which_payload = iot_edge_v1_Envelope_external_io_status_tag;
    iot_edge_v1_ExternalIoStatus *status = &output->payload.external_io_status;
    status->observed_at_ms = now_ms();
    status->channels_count = (pb_size_t)io->count;
    for (size_t i=0; i<io->count; ++i) {
        iot_edge_v1_ExternalIoChannel *channel = &status->channels[i];
        safe_copy(channel->channel,sizeof(channel->channel),io->channels[i].channel);
        channel->kind=io->channels[i].kind;
        channel->available=io->channels[i].available;
        channel->controllable=io->channels[i].controllable;
        if (channel->available && channel->kind == iot_edge_v1_ExternalIoKind_EXTERNAL_IO_AI) {
            channel->which_value = iot_edge_v1_ExternalIoChannel_milliamps_tag;
            channel->value.milliamps = (double)io->channels[i].value / 1000.0;
        } else if (channel->available) {
            channel->which_value = iot_edge_v1_ExternalIoChannel_digital_value_tag;
            channel->value.digital_value = io->channels[i].value != 0;
        }
    }
    const bool sent=send_envelope(session,output);
    if (!sent) edge_ws_transport_close(&session->transport,1011,"IO status enqueue failed");
    return sent;
}

static bool send_capability_report(edge_ws_session *session, bool force) {
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_capability_report_tag;
    iot_edge_v1_CapabilityReport *report = &envelope->payload.capability_report;
    safe_copy(report->network_stack, sizeof(report->network_stack), "netifd");
    report->ttyd_available = edge_capability_has_terminal();
    report->supports_dtu = true;
    report->supports_derived_points = true;
    const edge_external_io_snapshot *io = edge_acquisition_io_capability(session->app->acquisition);
    if (session->external_io && io && io->supported) {
        report->supports_external_io = true;
        report->external_io_channels_count = (pb_size_t)io->count;
        for (size_t i = 0; i < io->count; ++i) {
            iot_edge_v1_ExternalIoChannel *channel = &report->external_io_channels[i];
            safe_copy(channel->channel, sizeof(channel->channel), io->channels[i].channel);
            channel->kind = io->channels[i].kind;
            channel->available = io->channels[i].available;
            channel->controllable = io->channels[i].controllable;
        }
    }
    report->supports_serial_debug = session->app->config->serial_port[0] != '\0';
    static const iot_edge_v1_Protocol protocols[] = {
        iot_edge_v1_Protocol_PROTOCOL_MODBUS, iot_edge_v1_Protocol_PROTOCOL_S7,
        iot_edge_v1_Protocol_PROTOCOL_SL651, iot_edge_v1_Protocol_PROTOCOL_MC,
        iot_edge_v1_Protocol_PROTOCOL_FINS, iot_edge_v1_Protocol_PROTOCOL_DLT645,
        iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO
    };
    _Static_assert(sizeof(protocols) <= sizeof(report->supported_protocols),
                   "capability protocol array is too small");
    report->supported_protocols_count = sizeof(protocols) / sizeof(protocols[0]);
    memcpy(report->supported_protocols, protocols, sizeof(protocols));
    (void)edge_capability_collect_network(report, session->app->config->wan_interface);
    report->has_vpn = edge_vpn_collect_capability(&session->vpn, &report->vpn);
    if (session->app->config->serial_port[0] != '\0') {
        report->serial_ports_count = 1U;
        iot_edge_v1_SerialCapability *serial = &report->serial_ports[0];
        safe_copy(serial->path, sizeof(serial->path), session->app->config->serial_port);
        safe_copy(serial->display_name, sizeof(serial->display_name),
                  session->app->config->serial_port);
        serial->available = access(session->app->config->serial_port, R_OK | W_OK) == 0;
        serial->rs485 = session->app->config->serial_rs485;
    }
    return send_report(session, envelope, &session->capability_snapshot, force);
}

static void handle_log_request(edge_ws_session *session,
                               const iot_edge_v1_Envelope *input) {
    iot_edge_v1_LogRequest request = input->payload.log_request;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    if (input->message_id.size == 16U)
        edge_protocol_set_bytes(&output->causation_id,
                                sizeof(output->causation_id.bytes),
                                input->message_id.bytes, 16U);
    output->which_payload = iot_edge_v1_Envelope_log_result_tag;
    edge_log_query(&request, &output->payload.log_result);
    if (!send_envelope(session, output))
        edge_log_write("warn", "ws", "log result send failed", "");
}

static void handle_log_level_request(edge_ws_session *session,
                                     const iot_edge_v1_Envelope *input) {
    const iot_edge_v1_LogLevelRequest request = input->payload.log_level_request;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    edge_log_write("info", "log", "log level request received", request.level);
    if (input->message_id.size == 16U)
        edge_protocol_set_bytes(&output->causation_id,
                                sizeof(output->causation_id.bytes),
                                input->message_id.bytes, 16U);
    output->which_payload = iot_edge_v1_Envelope_log_level_result_tag;
    iot_edge_v1_LogLevelResult *result = &output->payload.log_level_result;
    if (request.request_id.size == 16U)
        edge_protocol_set_bytes(&result->request_id,
                                sizeof(result->request_id.bytes),
                                request.request_id.bytes, 16U);
    result->success = edge_log_set_level(request.level);
    safe_copy(result->level, sizeof(result->level), edge_log_level());
    safe_copy(result->message, sizeof(result->message),
              result->success ? "ok" : "invalid log level");
    if (result->success)
        edge_log_write("info", "log", "log level changed", result->level);
    else
        edge_log_write("warn", "log", "invalid log level", request.level);
    if (!send_envelope(session, output))
        edge_log_write("warn", "ws", "log level result send failed", "");
}

static bool send_device_status(edge_ws_session *session, bool force) {
    if (session == NULL || !session->websocket_open || !session->enrolled ||
        session->runtime_config.endpoint_count == 0U)
        return true;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_device_status_report_tag;
    edge_acquisition_status_for_platform(session->app->acquisition,
                                         session->config->id,
                                         &output->payload.device_status_report);
    if (output->payload.device_status_report.devices_count == 0U)
        return true;
    return send_report(session, output, &session->device_snapshot, force);
}

static void arm_reconnect_timer(edge_ws_session *session) {
    const uint32_t delay_ms = edge_retry_delay_ms(&session->retry, monotonic_ms());
    if (delay_ms == UINT32_MAX)
        return;
    ev_timer_stop(session->app->loop, &session->reconnect_timer);
    ev_timer_set(&session->reconnect_timer,
                 delay_ms == 0U ? 0.001 : (ev_tstamp)delay_ms / 1000.0, 0.0);
    ev_timer_start(session->app->loop, &session->reconnect_timer);
}

static void schedule_reconnect(edge_ws_session *session) {
    edge_vpn_shutdown(&session->vpn);
    clear_report_snapshots(session);
    close_serial_debug(session, "platform connection closed");
    edge_spool_outbox_reset(&session->spool);
    session->websocket_open = false;
    session->enrolled = false;
    session->session_epoch = 0U;
    session->last_report_ms = 0U;
    session->client_active = false;
    session->network_probe_nonce = 0U;
    session->hello_sent_monotonic_ms = 0U;
    invalidate_command_clock(session);
    ev_timer_stop(session->app->loop, &session->liveness_timer);
    ev_timer_stop(session->app->loop, &session->status_report_timer);
    ev_timer_stop(session->app->loop, &session->network_timer);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    if (session->terminal_open) {
        edge_terminal_close(session->terminal_id);
        session->terminal_open = false;
    }
    reset_terminal_flow(session);
    edge_retry_failed(&session->retry, monotonic_ms());
    arm_reconnect_timer(session);
}

static void websocket_open(void *user) {
    edge_ws_session *session = user;
    edge_traffic_register(&session->app->traffic,
        (size_t)(session - session->app->sessions),
        edge_ws_transport_socket_fd(&session->transport));
    session->websocket_open = true;
    session->hello_sent_monotonic_ms = 0U;
    invalidate_command_clock(session);
    edge_command_clock_reset(&session->command_clock);
    edge_retry_transport_connected(&session->retry, monotonic_ms(),
                                   EDGE_APPLICATION_HANDSHAKE_TIMEOUT_MS);
    ev_timer_stop(session->app->loop, &session->reconnect_timer);
    ev_timer_stop(session->app->loop, &session->liveness_timer);
    ev_timer_set(&session->liveness_timer, EDGE_LIVENESS_CHECK_INTERVAL_SEC,
                 EDGE_LIVENESS_CHECK_INTERVAL_SEC);
    ev_timer_start(session->app->loop, &session->liveness_timer);
    if (!send_hello(session)) {
        edge_ws_transport_close(&session->transport, 1011, "hello failed");
        return;
    }
    session->report_interval_sec = session->app->config->heartbeat_interval_sec;
    syslog(LOG_INFO, "platform %s WebSocket connected", session->config->name);
    char detail[96];
    snprintf(detail, sizeof(detail), "platform=%s", session->config->name);
    edge_log_write("info", "ws", "platform connected", detail);
}

static void websocket_close(void *user, int code, const char *reason) {
    edge_ws_session *session = user;
    syslog(LOG_WARNING, "platform %s WebSocket closed %d: %s", session->config->name,
           code, reason != NULL ? reason : "");
    char detail[128];
    snprintf(detail, sizeof(detail), "platform=%s code=%d reason=%s",
             session->config->name, code, reason != NULL ? reason : "");
    edge_log_write("warn", "ws", "platform websocket closed", detail);
    if (code != 1000)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK,
                             code >= 0 ? (unsigned)code : 0U);
    schedule_reconnect(session);
}

static bool valid_origin(const edge_ws_session *session,
                         const iot_edge_v1_Envelope *envelope) {
    if (envelope->platform_id.size != 16U ||
        memcmp(envelope->platform_id.bytes, session->config->id, 16U) != 0)
        return false;
    if (session->enrolled &&
        (envelope->node_id.size != 16U ||
         memcmp(envelope->node_id.bytes, session->node_id, 16U) != 0 ||
         envelope->session_epoch != session->session_epoch))
        return false;
    return true;
}

static bool encode_config_item(const iot_edge_v1_ConfigItem *item, uint8_t *output,
                               size_t capacity, size_t *output_size) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, iot_edge_v1_ConfigItem_fields, item))
        return false;
    *output_size = stream.bytes_written;
    return true;
}

static bool verify_config_item(iot_edge_v1_ConfigItem *item, uint8_t *encoded,
                               size_t capacity, size_t *encoded_size) {
    if (item->sha256.size != 32U)
        return false;
    uint8_t expected[32];
    memcpy(expected, item->sha256.bytes, sizeof(expected));
    item->sha256.size = 0U;
    size_t canonical_size = 0U;
    if (!encode_config_item(item, encoded, capacity, &canonical_size)) {
        memcpy(item->sha256.bytes, expected, sizeof(expected));
        item->sha256.size = sizeof(expected);
        return false;
    }
    uint8_t actual[32];
    if (edge_sha256(encoded, canonical_size, actual, 0) != 0 ||
        memcmp(expected, actual, sizeof(actual)) != 0) {
        memcpy(item->sha256.bytes, expected, sizeof(expected));
        item->sha256.size = sizeof(expected);
        return false;
    }
    memcpy(item->sha256.bytes, expected, sizeof(expected));
    item->sha256.size = sizeof(expected);
    return encode_config_item(item, encoded, capacity, encoded_size);
}

static void send_config_result(edge_ws_session *session, uint64_t revision,
                               const uint8_t digest[32], bool applied,
                               const char *code, const char *message) {
    uint8_t digest_copy[32];
    memcpy(digest_copy, digest, sizeof(digest_copy));
    iot_edge_v1_Envelope *out = &session->app->envelope;
    if (!init_envelope(session, out))
        return;
    if (applied) {
        out->which_payload = iot_edge_v1_Envelope_config_applied_tag;
        out->payload.config_applied.revision = revision;
        edge_protocol_set_bytes(&out->payload.config_applied.sha256,
                                sizeof(out->payload.config_applied.sha256.bytes), digest_copy, 32U);
        out->payload.config_applied.endpoint_count = session->runtime_config.endpoint_count;
        out->payload.config_applied.device_count = session->runtime_config.device_count;
    } else {
        out->which_payload = iot_edge_v1_Envelope_config_rejected_tag;
        out->payload.config_rejected.revision = revision;
        safe_copy(out->payload.config_rejected.code,
                  sizeof(out->payload.config_rejected.code), code);
        safe_copy(out->payload.config_rejected.message,
                  sizeof(out->payload.config_rejected.message), message);
    }
    send_envelope(session, out);
    char detail[257];
    snprintf(detail, sizeof(detail),
             "platform=%s revision=%" PRIu64 " state=%s endpoints=%u devices=%u message=%s",
             session->config->name, revision, applied ? "applied" : "rejected",
             session->runtime_config.endpoint_count,
             session->runtime_config.device_count,
             applied ? "configuration active" : (message != NULL ? message : ""));
    edge_log_write(applied ? "info" : "warn", "task",
                   "acquisition-config", detail);
}

static size_t build_acquisition_sources(
    edge_ws_app *app, const edge_ws_session *replacement_session,
    const edge_runtime_config *replacement_config,
    edge_acquisition_source sources[EDGE_MAX_PLATFORMS]) {
    const size_t count = app->config->platform_count;
    for (size_t index = 0U; index < count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        sources[index].platform_id = session->config->id;
        sources[index].priority = session->config->priority;
        sources[index].bootstrap = session->config->bootstrap;
        sources[index].raw_telemetry = session->raw_telemetry;
        sources[index].external_io = session->external_io;
        sources[index].config = session == replacement_session
                                    ? replacement_config
                                    : &session->runtime_config;
    }
    return count;
}

static void gateway_network_changed(void *context, bool available) {
    edge_ws_app *app = context;
    app->gateway_network_available = available;
    char device[33] = {0};
    const bool resolved = available && edge_network_gateway_resolve(
        app->config->modbus_gateway_interface, true, device,
        app->gateway_error, sizeof(app->gateway_error));
    if (!available)
        safe_copy(app->gateway_error, sizeof(app->gateway_error), "network event subscription unavailable; restart required");
    if (resolved && app->config->modbus_gateway &&
        edge_modbus_gateway_listening(app->gateway_server) && !strcmp(device, app->gateway_device)) {
        app->gateway_error[0] = '\0';
        return;
    }
    edge_modbus_gateway_close(app->gateway_server);
    app->gateway_server = NULL;
    app->gateway_device[0] = '\0';
    if (!resolved) return;
    app->gateway_error[0] = '\0';
    if (!app->config->modbus_gateway) return;
    if (!app->gateway) {
        edge_acquisition_source sources[EDGE_MAX_PLATFORMS];
        const size_t count = build_acquisition_sources(app, NULL, NULL, sources);
        app->gateway = prepare_gateway(sources, count, app->config->modbus_gateway_port);
        if (!app->gateway) {
            safe_copy(app->gateway_error, sizeof(app->gateway_error), "gateway map unavailable");
            return;
        }
    }
    if (!edge_modbus_gateway_set_unit_id(app->gateway, app->config->modbus_gateway_unit_id)) {
        safe_copy(app->gateway_error, sizeof(app->gateway_error), "gateway Unit ID must be between 1 and 247");
        return;
    }
    app->gateway_server = edge_modbus_gateway_listen(app->loop, app->gateway, device,
        app->config->modbus_gateway_port);
    if (!app->gateway_server) {
        safe_copy(app->gateway_error, sizeof(app->gateway_error), "cannot bind logical interface L3 device");
        return;
    }
    safe_copy(app->gateway_device, sizeof(app->gateway_device), device);
    app->gateway_error[0] = '\0';
    edge_modbus_gateway_set_writer(app->gateway_server, gateway_write, app);
}

static bool apply_gateway_settings(edge_ws_app *app, const iot_edge_v1_ModbusGatewaySettings *settings,
    char *error, size_t error_size) {
    if (!settings->port || settings->port > 65535U || settings->unit_id > 247U) {
        snprintf(error, error_size, "监听端口或从站 ID 无效（ID 必须为 1..247）"); return false;
    }
    char device[33] = {0};
    if (!edge_network_gateway_resolve(settings->interface_name, settings->enabled,
                                      device, error, error_size)) return false;
    if (settings->enabled && !app->gateway_network_available) {
        snprintf(error, error_size, "网络事件监听不可用，请重启服务"); return false;
    }
    edge_app_config candidate = app->config_storage;
    candidate.modbus_gateway = settings->enabled;
    candidate.modbus_gateway_port = (uint16_t)settings->port;
    candidate.modbus_gateway_unit_id = settings->unit_id ? (uint8_t)settings->unit_id : 1U;
    safe_copy(candidate.modbus_gateway_interface, sizeof(candidate.modbus_gateway_interface), settings->interface_name);
    const bool reuse = settings->enabled && edge_modbus_gateway_listening(app->gateway_server) &&
        app->config->modbus_gateway_port == settings->port && !strcmp(app->gateway_device, device);
    edge_modbus_gateway *map = NULL;
    edge_modbus_gateway_server *server = NULL;
    if (settings->enabled) {
        edge_acquisition_source sources[EDGE_MAX_PLATFORMS];
        const size_t count = build_acquisition_sources(app, NULL, NULL, sources);
        map = prepare_gateway(sources, count, candidate.modbus_gateway_port);
        if (!map) { snprintf(error, error_size, "地址映射不可用、容量已满或监听端口冲突"); return false; }
        if (!edge_modbus_gateway_set_unit_id(map, candidate.modbus_gateway_unit_id)) {
            edge_modbus_gateway_free(map); snprintf(error, error_size, "从站 ID 无效"); return false;
        }
        server = reuse ? app->gateway_server : edge_modbus_gateway_listen(app->loop, map,
            device, candidate.modbus_gateway_port);
        if (!server) { edge_modbus_gateway_free(map); snprintf(error, error_size, "无法绑定监听接口或端口"); return false; }
        edge_modbus_gateway_set_writer(server, gateway_write, app);
    }
    if (!edge_acquisition_set_gateway_enabled(app->acquisition, settings->enabled)) {
        if (!reuse) edge_modbus_gateway_close(server);
        edge_modbus_gateway_free(map);
        snprintf(error, error_size, "采集进程忙，请稍后重试"); return false;
    }
    if (!edge_config_save_modbus_gateway(&candidate)) {
        if (!edge_acquisition_set_gateway_enabled(app->acquisition, app->config->modbus_gateway))
            syslog(LOG_ERR, "cannot restore gateway sampling mode after UCI failure");
        if (!reuse) edge_modbus_gateway_close(server);
        edge_modbus_gateway_free(map);
        snprintf(error, error_size, "无法持久保存网关配置"); return false;
    }
    if (reuse) edge_modbus_gateway_replace(server, map);
    else edge_modbus_gateway_close(app->gateway_server);
    edge_modbus_gateway_free(app->gateway);
    app->gateway = map; app->gateway_server = server;
    app->config_storage = candidate;
    safe_copy(app->gateway_device, sizeof(app->gateway_device), settings->enabled ? device : "");
    app->gateway_error[0] = '\0';
    export_gateway(app);
    return true;
}

static bool gateway_points(edge_ws_session *session, const iot_edge_v1_ModbusGatewayRequest *request,
    iot_edge_v1_ModbusGatewayResult *result) {
    edge_modbus_gateway *loaded = NULL;
    const edge_modbus_gateway *map = session->app->gateway;
    if (!map) {
        loaded = edge_modbus_gateway_load(EDGE_GATEWAY_MAP_PATH);
        map = loaded;
        if (!map) { safe_copy(result->message, sizeof(result->message), "无法读取寄存器地址预留表"); return false; }
    }
    if (!edge_modbus_gateway_digest(map, session->config->id, result->mapping_digest.bytes)) {
        edge_modbus_gateway_free(loaded);
        safe_copy(result->message, sizeof(result->message), "无法计算映射表版本");
        return false;
    }
    result->mapping_digest.size = 32;
    const bool success = edge_modbus_gateway_points(map, session->config->id, &session->runtime_config,
        session->app->config->modbus_gateway, edge_modbus_gateway_listening(session->app->gateway_server),
        request, result, monotonic_ms());
    edge_modbus_gateway_free(loaded);
    return success;
}

static bool apply_gateway_order(edge_ws_session *session,
    const iot_edge_v1_ModbusGatewayRequest *request, char *error, size_t error_size) {
    edge_ws_app *app = session->app;
    if (request->has_settings || request->include_points || !app->config->modbus_gateway ||
        !edge_modbus_gateway_listening(app->gateway_server)) {
        snprintf(error, error_size, "请启用网关并单独提交地址排序");
        return false;
    }
    if (!edge_modbus_gateway_reorder(app->gateway, EDGE_GATEWAY_MAP_PATH, session->config->id,
                                    &request->order)) {
        snprintf(error, error_size, "映射已变化、排序无效或保存失败；请重新读取后排序");
        return false;
    }
    edge_modbus_gateway_replace(app->gateway_server, app->gateway);
    export_gateway(app);
    return true;
}

static void handle_gateway_request(edge_ws_session *session, const iot_edge_v1_Envelope *input) {
    const iot_edge_v1_ModbusGatewayRequest request = input->payload.modbus_gateway_request;
    if (request.request_id.size != 16U) return;
    char error[257] = {0};
    const bool success = request.has_order
        ? apply_gateway_order(session, &request, error, sizeof(error))
        : !request.has_settings || apply_gateway_settings(session->app, &request.settings, error, sizeof(error));
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output)) return;
    output->which_payload = iot_edge_v1_Envelope_modbus_gateway_result_tag;
    iot_edge_v1_ModbusGatewayResult *result = &output->payload.modbus_gateway_result;
    edge_protocol_set_bytes(&result->request_id, sizeof(result->request_id.bytes), request.request_id.bytes, 16U);
    result->success = success;
    result->order_applied = success && request.has_order;
    result->order_supported = true;
    safe_copy(result->message, sizeof(result->message), success ? "ok" : error);
    result->has_settings = true;
    result->settings.enabled = session->app->config->modbus_gateway;
    result->settings.port = session->app->config->modbus_gateway_port;
    result->settings.unit_id = session->app->config->modbus_gateway_unit_id;
    safe_copy(result->settings.interface_name, sizeof(result->settings.interface_name),
        session->app->config->modbus_gateway_interface);
    if (success && !request.has_settings && session->app->gateway_error[0])
        safe_copy(result->message, sizeof(result->message), session->app->gateway_error);
    if (result->success && request.include_points)
        result->success = gateway_points(session, &request, result);
    if (!send_envelope(session, output)) syslog(LOG_WARNING, "Modbus gateway configuration reply was not sent");
}

static void handle_config(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    const edge_config_replay replay = edge_protocol_config_replay(envelope,
        session->spool.active_config.revision, session->spool.active_config.digest);
    if (replay == EDGE_CONFIG_REPLAY_IGNORE) return;
    if (replay == EDGE_CONFIG_REPLAY_ACK || replay == EDGE_CONFIG_REPLAY_CONFLICT) {
        send_config_result(session, session->spool.active_config.revision,
            session->spool.active_config.digest, replay == EDGE_CONFIG_REPLAY_ACK,
            "config_replay_conflict", "same revision has a different digest");
        return;
    }
    if (envelope->which_payload == iot_edge_v1_Envelope_config_begin_tag) {
        iot_edge_v1_ConfigBegin *begin = &envelope->payload.config_begin;
        if (begin->sha256.size != 32U || begin->revision <= session->active_revision ||
            !edge_spool_config_begin(&session->spool, begin->revision,
                                     begin->item_count, begin->sha256.bytes))
            send_config_result(session, begin->revision, begin->sha256.bytes, false,
                               "config_begin_invalid", "configuration begin was rejected");
        return;
    }
    if (envelope->which_payload == iot_edge_v1_Envelope_config_item_tag) {
        iot_edge_v1_ConfigItem *item = &envelope->payload.config_item;
        uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
        size_t encoded_size = 0U;
        if ((item->which_item == iot_edge_v1_ConfigItem_external_io_tag && !session->external_io) ||
            !verify_config_item(item, encoded, sizeof(encoded), &encoded_size) ||
            !edge_spool_config_put(&session->spool, item->revision, item->index,
                                   item->sha256.bytes, encoded, encoded_size))
            send_config_result(session, item->revision, item->sha256.bytes, false,
                               "config_item_invalid", "configuration item was rejected");
        return;
    }
    const iot_edge_v1_ConfigCommit committed = envelope->payload.config_commit;
    const iot_edge_v1_ConfigCommit *commit = &committed;
    edge_runtime_config candidate = {0};
    edge_acquisition *candidate_acquisition = NULL;
    edge_modbus_gateway *candidate_gateway = NULL;
    char apply_error[256] = "configuration digest or item count failed";
    if (commit->sha256.size != 32U ||
        !edge_runtime_config_load(&candidate, &session->spool.staging_config,
                                  apply_error, sizeof(apply_error))) {
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", apply_error);
        return;
    }
    edge_acquisition_source sources[EDGE_MAX_PLATFORMS];
    const size_t source_count = build_acquisition_sources(
        session->app, session, &candidate, sources);
    candidate_acquisition = edge_acquisition_create(acquisition_telemetry,
                                                     acquisition_command_result,
                                                     session->app);
    edge_acquisition_set_sample_callback(candidate_acquisition,
        acquisition_sample);
    (void)edge_acquisition_set_gateway_enabled(candidate_acquisition, session->app->config->modbus_gateway);
    edge_acquisition_set_debug_callback(candidate_acquisition, acquisition_debug);
    edge_acquisition_set_dtu_callback(candidate_acquisition, acquisition_dtu);
    edge_acquisition_set_io_callback(candidate_acquisition, acquisition_io_capability);
    edge_acquisition_enable_serial_debug(candidate_acquisition, session->app->config->serial_port,
        session->app->config->serial_rs485, acquisition_serial);
    if (candidate_acquisition == NULL ||
        !edge_acquisition_apply_multi(candidate_acquisition, sources, source_count,
                                      monotonic_ms(), apply_error,
                                      sizeof(apply_error))) {
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_runtime_invalid", apply_error);
        return;
    }
    if (session->app->config->modbus_gateway) {
        candidate_gateway = prepare_gateway(sources, source_count, session->app->config->modbus_gateway_port);
        if (!candidate_gateway) {
            edge_acquisition_destroy(candidate_acquisition);
            edge_runtime_config_free(&candidate);
            send_config_result(session, commit->revision, commit->sha256.bytes, false,
                "modbus_gateway_invalid", "gateway address map unavailable, full, or listener port conflicts");
            return;
        }
    }
    if (!edge_acquisition_prepare_io_swap(session->app->acquisition)) {
        edge_modbus_gateway_free(candidate_gateway);
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "io_outbox_busy", "external IO samples or results have not been queued");
        return;
    }
    ev_io_stop(session->app->loop, &session->app->acquisition_io);
    for (size_t i = 0; i < session->app->config->platform_count; ++i)
        close_serial_debug(&session->app->sessions[i], "device configuration changed; reopen serial debug");
    edge_modbus_gateway_replace(session->app->gateway_server, session->app->gateway);
    edge_acquisition_stop(session->app->acquisition);
    gateway_commands_stopped(session->app);
    if (!edge_acquisition_start(candidate_acquisition,
                                apply_error, sizeof(apply_error))) {
        char restart_error[128] = {0};
        if (!edge_acquisition_start(session->app->acquisition, restart_error,
                                    sizeof(restart_error)))
            edge_log_write("error", "acquisition",
                           "previous acquisition worker could not restart",
                           restart_error);
        sync_acquisition_io(session->app);
        edge_modbus_gateway_free(candidate_gateway);
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_runtime_invalid", apply_error);
        return;
    }
    if (!edge_spool_config_commit(&session->spool, commit->revision,
                                  commit->sha256.bytes)) {
        edge_modbus_gateway_free(candidate_gateway);
        edge_acquisition_destroy(candidate_acquisition);
        char restart_error[128] = {0};
        if (!edge_acquisition_start(session->app->acquisition, restart_error,
                                    sizeof(restart_error)))
            edge_log_write("error", "acquisition",
                           "previous acquisition worker could not restart",
                           restart_error);
        sync_acquisition_io(session->app);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", "configuration tmpfs commit failed");
        return;
    }
    edge_acquisition_destroy(session->app->acquisition);
    edge_runtime_config_free(&session->runtime_config);
    session->app->acquisition = candidate_acquisition;
    if (candidate_gateway)
        (void)edge_modbus_gateway_set_unit_id(candidate_gateway, session->app->config->modbus_gateway_unit_id);
    edge_modbus_gateway_replace(session->app->gateway_server, candidate_gateway);
    edge_modbus_gateway_free(session->app->gateway);
    session->app->gateway = candidate_gateway;
    export_gateway(session->app);
    session->runtime_config = candidate;
    session->dtu_status_count = 0;
    for (size_t i = 0; i < 8; ++i) edge_report_free(&session->dtu_snapshots[i]);
    session->active_revision = commit->revision;
    sync_acquisition_io(session->app);
    send_config_result(session, commit->revision, commit->sha256.bytes, true, NULL, NULL);
    /* Keep one initial-config retry for older gateways/session promotion.
     * Later configuration changes only send changed capabilities. */
    if (send_capability_report(session, !session->capability_config_retry_done))
        session->capability_config_retry_done = true;
    else
        edge_log_write("warn", "ws", "capability report after config sync failed", "");
    for (size_t index = 0U; index < session->app->config->platform_count; ++index)
        send_device_status(&session->app->sessions[index], false);
}

static void handle_device_command(edge_ws_session *session,
                                  const iot_edge_v1_CommandRequest *request) {
    uint8_t command_id[16] = {0};
    uint8_t device_id[16] = {0};
    if (request->command_id.size == 16U)
        memcpy(command_id, request->command_id.bytes, sizeof(command_id));
    if (request->device_id.size == 16U)
        memcpy(device_id, request->device_id.bytes, sizeof(device_id));
    char error[257] = {0};
    uint64_t start_before_monotonic_ms = 0U;
    uint64_t mapping_valid_until_monotonic_ms = 0U;
    if (request->node_io_channel[0] && !session->node_io)
        safe_copy(error,sizeof(error),"node IO control was not negotiated");
    if (!request->has_start_before_ms ||
        !edge_command_clock_deadline(&session->command_clock,
                                     request->start_before_ms, monotonic_ms(),
                                     &start_before_monotonic_ms,
                                     &mapping_valid_until_monotonic_ms)) {
        safe_copy(error, sizeof(error),
                  "trusted database-time mapping is missing, stale, or deadline expired");
    }
    if (error[0] == '\0' &&
        edge_acquisition_command_for_platform_until(
            session->app->acquisition, session->config->id, request,
            start_before_monotonic_ms, mapping_valid_until_monotonic_ms,
            error, sizeof(error))) {
        char device_uuid[37];
        edge_config_format_uuid(device_id, device_uuid);
        char summary[160];
        snprintf(summary, sizeof(summary),
                 "device=%s values=%u timeoutMs=%u readback=%u fastRead=%u/%u",
                 device_uuid, (unsigned)request->values_count,
                 request->timeout_ms, request->readback_count,
                 request->fast_read_duration_sec,
                 request->fast_read_interval_sec);
        log_uuid_task(session, "device-command", command_id, "accepted", summary);
        return;
    }
    iot_edge_v1_CommandResult result = iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&result.command_id, sizeof(result.command_id.bytes),
                            command_id, sizeof(command_id));
    if (request->node_io_channel[0]) safe_copy(result.node_io_channel,sizeof(result.node_io_channel),request->node_io_channel);
    else edge_protocol_set_bytes(&result.device_id,sizeof(result.device_id.bytes),device_id,sizeof(device_id));
    result.state = iot_edge_v1_CommandState_COMMAND_STATE_REJECTED;
    result.completed_at_ms = now_ms();
    safe_copy(result.message, sizeof(result.message),
              error[0] != '\0' ? error : "edge command rejected");
    (void)acquisition_command_result(session->app, session->config->id, &result);
}

static void send_pong(edge_ws_session *session, const iot_edge_v1_Envelope *input) {
    const uint64_t nonce = input->payload.ping.nonce;
    uint8_t message_id[16];
    memcpy(message_id, input->message_id.bytes, sizeof(message_id));
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_pong_tag;
    output->payload.pong.nonce = nonce;
    edge_protocol_set_bytes(&output->causation_id, sizeof(output->causation_id.bytes),
                            message_id, sizeof(message_id));
    send_envelope(session, output);
}

static void send_firmware_result(edge_ws_session *session, const uint8_t request_id[16],
                                  iot_edge_v1_FirmwareUpdateState state,
                                  const char *message, uint64_t downloaded_bytes,
                                  uint64_t total_bytes, uint32_t progress_percent) {
    if (state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_FIRMWARE, 1U);
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output)) {
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_FIRMWARE, 2U);
        return;
    }
    output->which_payload = iot_edge_v1_Envelope_firmware_update_result_tag;
    edge_protocol_set_bytes(&output->payload.firmware_update_result.request_id,
                            sizeof(output->payload.firmware_update_result.request_id.bytes),
                            request_id, 16U);
    output->payload.firmware_update_result.state = state;
    safe_copy(output->payload.firmware_update_result.message,
              sizeof(output->payload.firmware_update_result.message), message);
    output->payload.firmware_update_result.downloaded_bytes = downloaded_bytes;
    output->payload.firmware_update_result.total_bytes = total_bytes;
    output->payload.firmware_update_result.progress_percent = progress_percent;
    if (!send_envelope(session, output))
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_FIRMWARE, 3U);
}

static bool send_firmware_chunk_request(edge_ws_session *session, bool force) {
    iot_edge_v1_FirmwareChunkRequest request =
        iot_edge_v1_FirmwareChunkRequest_init_zero;
    if (!edge_firmware_chunk_request(session->config->id, &request, force))
        return false;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_firmware_chunk_request_tag;
    output->payload.firmware_chunk_request = request;
    return send_envelope(session, output);
}

static void send_network_result(edge_ws_session *session,
                                const uint8_t request_id[16], bool success,
                                bool rolled_back, const char *message) {
    if (!success || rolled_back)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK_CONFIG,
                             rolled_back ? 2U : 1U);
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_network_config_result_tag;
    edge_protocol_set_bytes(&output->payload.network_config_result.request_id,
                            sizeof(output->payload.network_config_result.request_id.bytes),
                            request_id, 16U);
    output->payload.network_config_result.success = success;
    output->payload.network_config_result.rolled_back = rolled_back;
    safe_copy(output->payload.network_config_result.message,
              sizeof(output->payload.network_config_result.message), message);
    send_envelope(session, output);
}

static void send_vpn_result(edge_ws_session *session,
                            const uint8_t request_id[16], uint64_t config_version,
                            bool applied, const char *error_message) {
    if (!applied)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_VPN,
                             error_message != NULL && strstr(error_message, "stale") != NULL
                                 ? 1U : 2U);
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_vpn_config_result_tag;
    iot_edge_v1_VpnConfigResult *result = &output->payload.vpn_config_result;
    edge_protocol_set_bytes(&result->request_id, sizeof(result->request_id.bytes),
                            request_id, 16U);
    result->config_version = config_version;
    result->applied = applied;
    if (!applied) {
        safe_copy(result->error_code, sizeof(result->error_code),
                  error_message != NULL && strstr(error_message, "stale") != NULL
                      ? "stale_config"
                      : "apply_failed");
        safe_copy(result->error_message, sizeof(result->error_message),
                  error_message != NULL ? error_message : "VPN configuration failed");
    }
    if (!send_envelope(session, output))
        edge_log_write("warn", "vpn", "VPN configuration result send failed", "");
}

static void handle_vpn_config(edge_ws_session *session,
                              const iot_edge_v1_VpnConfigRequest *request) {
    char error[257] = {0};
    uint8_t request_id[16] = {0};
    uint64_t config_version = 0U;
    if (request != NULL) {
        config_version = request->config_version;
        if (request->request_id.size == sizeof(request_id))
            memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    }
    const bool applied = edge_vpn_apply(&session->vpn, request, error, sizeof(error));
    send_vpn_result(session, request_id, config_version, applied,
                    applied ? "" : error);
    char detail[320];
    snprintf(detail, sizeof(detail), "version=%" PRIu64 " applied=%u message=%s",
             config_version, applied ? 1U : 0U,
             applied ? "" : error);
    edge_log_write(applied ? "info" : "warn", "vpn", "VPN configuration", detail);
}

static void broadcast_network_result(edge_ws_app *app,
                                     const uint8_t request_id[16], bool success,
                                     bool rolled_back, const char *message) {
    for (size_t index = 0U; index < app->config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        if (session->websocket_open && session->enrolled)
            send_network_result(session, request_id, success, rolled_back, message);
    }
}

static void broadcast_network_state(edge_ws_app *app) {
    for (size_t index = 0U; index < app->config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        if (session->websocket_open && session->enrolled)
            send_capability_report(session, false);
    }
}

static void confirm_network_for_session(edge_ws_session *session) {
    uint8_t request_id[16];
    if (!edge_network_confirm(session->config->id, request_id))
        return;
    broadcast_network_result(session->app, request_id, true, false,
                             "network configuration confirmed");
    broadcast_network_state(session->app);
}

static void report_network_rollback(edge_ws_app *app) {
    uint8_t owner_platform_id[16];
    uint8_t request_id[16];
    if (!edge_network_take_rollback(owner_platform_id, request_id))
        return;
    (void)owner_platform_id;
    broadcast_network_result(
        app, request_id, false, true,
        "network configuration rolled back after reconnect timeout");
    broadcast_network_state(app);
}

static bool send_modem_result(edge_ws_session *session, const uint8_t request_id[16],
                              iot_edge_v1_ModemControlAction action,
                              iot_edge_v1_ModemControlState state,
                              const char *message, const char *apn) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_modem_control_result_tag;
    edge_protocol_set_bytes(&output->payload.modem_control_result.request_id,
                            sizeof(output->payload.modem_control_result.request_id.bytes),
                            request_id, 16U);
    output->payload.modem_control_result.action = action;
    output->payload.modem_control_result.state = state;
    safe_copy(output->payload.modem_control_result.message,
              sizeof(output->payload.modem_control_result.message), message);
    safe_copy(output->payload.modem_control_result.apn,
              sizeof(output->payload.modem_control_result.apn), apn);
    const bool sent = send_envelope(session, output);
    if (state == iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_MODEM, (unsigned)action);
    if (!sent)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_MODEM, 100U);
    char summary[160];
    snprintf(summary, sizeof(summary), "action=%d message=%s",
             (int)action, message != NULL ? message : "");
    log_uuid_task(session, "modem-control", request_id,
                  modem_state_name(state), summary);
    return sent;
}

typedef struct {
    uint8_t request_id[16];
    iot_edge_v1_ModemControlAction action;
    bool success;
    char message[257];
    char apn[101];
} edge_modem_worker_result;

static void modem_worker(edge_ws_session *session, int worker_fd,
                         const iot_edge_v1_ModemControlRequest *request) {
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _exit(EXIT_FAILURE);
    edge_process_close_inherited_fds(worker_fd);
    edge_modem_worker_result result = {0};
    memcpy(result.request_id, request->request_id.bytes, sizeof(result.request_id));
    result.action = request->action;
    safe_copy(result.apn, sizeof(result.apn), request->apn);
    result.success = edge_modem_control(session->app->config->modem_at_port, request,
                                        result.message, sizeof(result.message));
    for (;;) {
        const ssize_t sent = send(worker_fd, &result, sizeof(result), MSG_NOSIGNAL);
        if (sent == (ssize_t)sizeof(result))
            break;
        if (sent < 0 && errno == EINTR)
            continue;
        break;
    }
    close(worker_fd);
    _exit(result.success ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void handle_network_config(edge_ws_session *session,
                                  const iot_edge_v1_NetworkConfigRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    bool success = false;
    if (edge_firmware_active())
        safe_copy(message, sizeof(message),
                  "firmware update is in progress");
    else
        success = edge_network_prepare(request, session->app->config->wan_interface,
                                       message, sizeof(message));
    /* UCI is already committed: stop serving a removed/reassigned identity
     * before the asynchronous network reload begins. */
    if (success) gateway_network_changed(session->app, session->app->gateway_network_available);
    if (success &&
        !edge_network_activate(request->rollback_timeout_sec,
                               session->config->id, request_id)) {
        success = false;
        safe_copy(message, sizeof(message),
                  "could not activate network rollback watchdog");
    }
    if (success)
        safe_copy(message, sizeof(message),
                  "network configuration saved locally and is being applied");
    if (success)
        broadcast_network_result(session->app, request_id, true, false, message);
    else
        send_network_result(session, request_id, false, false, message);
    log_uuid_task(session, "network-config", request_id,
                  success ? "accepted" : "rejected", message);
    if (success) {
        ev_timer_stop(session->app->loop, &session->network_timer);
        ev_timer_set(&session->network_timer, 2.0, 0.0);
        ev_timer_start(session->app->loop, &session->network_timer);
    }
}

static bool send_status_report(edge_ws_session *session) {
    const uint64_t probe_sent_ms = monotonic_ms();
    const bool mapping_was_valid = session->command_clock.valid;
    uint64_t clock_nonce = 0U;
    if (probe_sent_ms == 0U ||
        !edge_command_clock_begin_heartbeat(&session->command_clock,
                                            probe_sent_ms, &clock_nonce)) {
        invalidate_command_clock(session);
        return false;
    }
    if (mapping_was_valid && !session->command_clock.valid)
        edge_acquisition_invalidate_command_clock(session->app->acquisition,
                                                 session->config->id);
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope)) {
        invalidate_command_clock(session);
        return false;
    }
    envelope->which_payload = iot_edge_v1_Envelope_heartbeat_tag;
    iot_edge_v1_Heartbeat *heartbeat = &envelope->payload.heartbeat;
    edge_traffic_sample(&session->app->traffic,
        (size_t)(session - session->app->sessions), monotonic_ms(), &heartbeat->tcp_traffic);
    heartbeat->has_tcp_traffic = heartbeat->tcp_traffic.sample_id != 0U;
    heartbeat->has_vpn_traffic =
        edge_vpn_sample(&session->vpn, monotonic_ms(), &heartbeat->vpn_traffic);
    heartbeat->signal_csq = 99U;
    heartbeat->signal_rssi_dbm = -1;
    heartbeat->mobile_registration_status = -1;
    heartbeat->supports_modem_control =
        session->config->bootstrap && session->app->config->modem_at_port[0] != '\0';
    safe_copy(heartbeat->log_level, sizeof(heartbeat->log_level), edge_log_level());
    edge_modem_info modem;
    bool modem_available = false;
    if (edge_modem_read_status(session->app->config->modem_status_path,
                               &modem, &modem_available)) {
        heartbeat->modem_available = modem_available;
        safe_copy(heartbeat->iccid, sizeof(heartbeat->iccid), modem.iccid);
        heartbeat->signal_csq = (uint32_t)modem.csq;
        heartbeat->signal_rssi_dbm = modem.rssi_dbm;
        heartbeat->signal_percent = modem.signal_percent;
        heartbeat->mobile_registered = modem.registered;
        heartbeat->mobile_registration_status = modem.registration_status;
        heartbeat->sim_state = modem.sim_state;
        safe_copy(heartbeat->apn, sizeof(heartbeat->apn), modem.apn);
        safe_copy(heartbeat->mobile_operator, sizeof(heartbeat->mobile_operator),
                  modem.mobile_operator);
        heartbeat->mobile_connected = modem.connected;
        safe_copy(heartbeat->mobile_ipv4, sizeof(heartbeat->mobile_ipv4),
                  modem.mobile_ipv4);
    }
    struct sysinfo info;
    if (sysinfo(&info) == 0)
        heartbeat->uptime_sec = (uint64_t)info.uptime;
    heartbeat->has_nonce = true;
    heartbeat->nonce = clock_nonce;
    heartbeat->active_config_version = session->active_revision;
    heartbeat->managed_endpoint_count = session->runtime_config.endpoint_count;
    heartbeat->managed_device_count = session->runtime_config.device_count;
    edge_spool_maintain(&session->spool);
    heartbeat->outbox_records = session->spool.outbox.count;
    heartbeat->outbox_bytes = session->spool.outbox.bytes;
    if (!send_envelope(session, envelope)) {
        invalidate_command_clock(session);
        return false;
    }
    session->last_report_ms = monotonic_ms();
    send_outbox_window(session);
    report_network_rollback(session->app);
    return true;
}

static void handle_firmware_update(edge_ws_session *session,
                                   const iot_edge_v1_FirmwareUpdateRequest *request) {
    uint8_t request_id[16] = {0};
    const uint64_t total_bytes = request->size_bytes;
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    const bool accepted =
        !edge_network_pending() &&
        edge_firmware_start(session->config->id, request, message, sizeof(message));
    if (!accepted && edge_network_pending())
        safe_copy(message, sizeof(message),
                  "network configuration is waiting for confirmation");
    send_firmware_result(
        session, request_id,
        accepted ? iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_ACCEPTED
                 : iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
        accepted ? "firmware update accepted" : message, 0U, total_bytes, 0U);
    char summary[160];
    snprintf(summary, sizeof(summary), "bytes=%" PRIu64 " %.120s",
             total_bytes, accepted ? "transport=ws" : message);
    log_uuid_task(session, "firmware-update", request_id,
                  accepted ? "accepted" : "rejected", summary);
    if (accepted) {
        memcpy(session->firmware_request_id, request_id,
               sizeof(session->firmware_request_id));
        session->firmware_total_bytes = total_bytes;
        session->firmware_operation_active = true;
        ev_timer_stop(session->app->loop, &session->firmware_timer);
        ev_timer_set(&session->firmware_timer, 0.25, 0.5);
        ev_timer_start(session->app->loop, &session->firmware_timer);
        (void)send_firmware_chunk_request(session, true);
    }
}

static void handle_firmware_chunk(edge_ws_session *session,
                                  const iot_edge_v1_FirmwareChunk *chunk) {
    char message[257] = {0};
    const edge_firmware_chunk_result result = edge_firmware_receive_chunk(
        session->config->id, chunk, message, sizeof(message));
    if (result == EDGE_FIRMWARE_CHUNK_NEXT || result == EDGE_FIRMWARE_CHUNK_WAIT)
        (void)send_firmware_chunk_request(session, result == EDGE_FIRMWARE_CHUNK_NEXT);
    if (result == EDGE_FIRMWARE_CHUNK_COMPLETE) {
        session->firmware_traffic_flush_pending = true;
        session->firmware_traffic_flush_followup = true;
        if (!send_status_report(session)) {
            edge_firmware_mark_traffic_flushed();
            session->firmware_traffic_flush_pending = false;
            session->firmware_traffic_flush_followup = false;
        }
    }
}

static void handle_modem_control(edge_ws_session *session,
                                 const iot_edge_v1_ModemControlRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size != 16U) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "invalid modem request id", request->apn);
        return;
    }
    memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    if (!edge_modem_validate_control(request, message, sizeof(message))) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                message, request->apn);
        return;
    }
    if (session->modem_worker_pid > 0 || session->modem_result_pending) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "another modem operation is still pending", request->apn);
        return;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                   0, sockets) != 0) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "cannot start modem worker", request->apn);
        return;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "cannot start modem worker", request->apn);
        return;
    }
    if (child == 0) {
        close(sockets[0]);
        modem_worker(session, sockets[1], request);
    }
    close(sockets[1]);
    session->modem_worker_pid = child;
    session->modem_worker_fd = sockets[0];
    session->modem_result_received = false;
    memcpy(session->modem_result_request_id, request_id,
           sizeof(session->modem_result_request_id));
    session->modem_result_action = request->action;
    session->modem_result_state =
        iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED;
    safe_copy(session->modem_result_message,
              sizeof(session->modem_result_message),
              "modem worker exited before returning a result");
    safe_copy(session->modem_result_apn, sizeof(session->modem_result_apn),
              request->apn);
    (void)send_modem_result(session, request_id, request->action,
                            iot_edge_v1_ModemControlState_MODEM_CONTROL_ACCEPTED,
                            "modem control accepted", request->apn);
    ev_io_stop(session->app->loop, &session->modem_io);
    ev_io_set(&session->modem_io, session->modem_worker_fd, EV_READ);
    ev_io_start(session->app->loop, &session->modem_io);
    ev_child_stop(session->app->loop, &session->modem_child);
    ev_child_set(&session->modem_child, child, 0);
    ev_child_start(session->app->loop, &session->modem_child);
    ev_timer_stop(session->app->loop, &session->modem_timer);
    ev_timer_set(&session->modem_timer, 60.0, 0.0);
    ev_timer_start(session->app->loop, &session->modem_timer);
}

static bool send_terminal_opened(edge_ws_session *session,
                                 const uint8_t terminal_id[16]) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_opened_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_opened.terminal_id,
               sizeof(output->payload.terminal_opened.terminal_id.bytes),
               terminal_id, 16U) &&
           send_envelope(session, output);
}

static void send_terminal_close(edge_ws_session *session, const uint8_t terminal_id[16],
                                int32_t exit_code, const char *reason) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_terminal_close_tag;
    edge_protocol_set_bytes(&output->payload.terminal_close.terminal_id,
                            sizeof(output->payload.terminal_close.terminal_id.bytes),
                            terminal_id, 16U);
    output->payload.terminal_close.exit_code = exit_code;
    safe_copy(output->payload.terminal_close.reason,
              sizeof(output->payload.terminal_close.reason), reason);
    send_envelope(session, output);
}

static bool send_terminal_data_ack(edge_ws_session *session,
                                   const uint8_t terminal_id[16],
                                   uint64_t sequence) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_data_ack_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_data_ack.terminal_id,
               sizeof(output->payload.terminal_data_ack.terminal_id.bytes),
               terminal_id, 16U) &&
           (output->payload.terminal_data_ack.sequence = sequence) != 0U &&
           send_envelope(session, output);
}

static void stage_terminal_input_ack(edge_ws_session *session, uint64_t sequence) {
    if (sequence == 0U)
        return;
    session->terminal_input_ack_sequence = sequence;
    session->terminal_input_ack_pending = true;
    if (send_terminal_data_ack(session, session->terminal_id, sequence))
        session->terminal_input_ack_pending = false;
}

static bool send_terminal_output(edge_ws_session *session) {
    if (!session->terminal_output_pending || session->terminal_output_size == 0U ||
        session->terminal_output_sequence == 0U)
        return false;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_data_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_data.terminal_id,
               sizeof(output->payload.terminal_data.terminal_id.bytes),
               session->terminal_id, sizeof(session->terminal_id)) &&
           edge_protocol_set_bytes(
               &output->payload.terminal_data.data,
               sizeof(output->payload.terminal_data.data.bytes),
               session->terminal_output, session->terminal_output_size) &&
           (output->payload.terminal_data.sequence =
                session->terminal_output_sequence) != 0U &&
           send_envelope(session, output);
}

static void fail_terminal(edge_ws_session *session, const char *reason) {
    uint8_t terminal_id[16];
    memcpy(terminal_id, session->terminal_id, sizeof(terminal_id));
    edge_terminal_close(terminal_id);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    session->terminal_open = false;
    send_terminal_close(session, terminal_id, -1, reason);
    reset_terminal_flow(session);
}

static void handle_terminal_open(edge_ws_session *session,
                                 const iot_edge_v1_TerminalOpen *request) {
    uint8_t terminal_id[16] = {0};
    if (request->terminal_id.size == 16U)
        memcpy(terminal_id, request->terminal_id.bytes, sizeof(terminal_id));
    if (session->terminal_open) {
        send_terminal_close(session, terminal_id, -1,
                            "another terminal is active on this platform");
        return;
    }
    char error[129] = {0};
    if (!edge_terminal_open(request, error, sizeof(error))) {
        send_terminal_close(session, terminal_id, -1, error);
        return;
    }
    reset_terminal_flow(session);
    memcpy(session->terminal_id, terminal_id, sizeof(session->terminal_id));
    session->terminal_open = true;
    if (!send_terminal_opened(session, terminal_id)) {
        edge_terminal_close(terminal_id);
        session->terminal_open = false;
        reset_terminal_flow(session);
        return;
    }
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    ev_timer_set(&session->terminal_timer, EDGE_TERMINAL_OUTPUT_POLL_INTERVAL,
                 EDGE_TERMINAL_OUTPUT_POLL_INTERVAL);
    ev_timer_start(session->app->loop, &session->terminal_timer);
}

static void dispatch_platform_message(edge_ws_session *session,
                                      iot_edge_v1_Envelope *envelope) {
    // Telemetry is node -> platform only; the receive wrapper owns allocations.
    if (envelope->which_payload == iot_edge_v1_Envelope_telemetry_batch_tag) {
        edge_ws_transport_close(&session->transport, 1002, "unexpected telemetry");
        return;
    }
    session->last_inbound_ms = monotonic_ms();

    switch (envelope->which_payload) {
    case iot_edge_v1_Envelope_hello_ack_tag: {
        iot_edge_v1_HelloAck *ack = &envelope->payload.hello_ack;
        if (ack->assigned_node_id.size != 16U ||
            ack->negotiated_protocol_version != EDGENODE_PROTOCOL_VERSION ||
            ack->session_epoch == 0U) {
            edge_ws_transport_close(&session->transport, 1002, "invalid hello ack");
            return;
        }
        const uint64_t hello_ack_received_ms = session->last_inbound_ms;
        if (ack->has_database_time_ms && session->hello_sent_monotonic_ms != 0U) {
            const bool was_valid = session->command_clock.valid;
            const bool accepted = edge_command_clock_accept_hello(
                &session->command_clock, ack->database_time_ms,
                session->hello_sent_monotonic_ms, hello_ack_received_ms);
            if (!accepted && was_valid)
                edge_acquisition_invalidate_command_clock(session->app->acquisition,
                                                         session->config->id);
        } else {
            invalidate_command_clock(session);
        }
        session->hello_sent_monotonic_ms = 0U;
        memcpy(session->node_id, ack->assigned_node_id.bytes, 16U);
        session->session_epoch = ack->session_epoch;
        session->enrolled = true;
        session->raw_telemetry = ack->raw_telemetry;
        session->external_io = ack->external_io && ack->raw_telemetry && access("/etc/init.d/dtu-io", X_OK) == 0;
        session->node_io = session->external_io && ack->node_io;
        if (!edge_acquisition_set_raw_telemetry(session->app->acquisition,
                                               session->config->id, session->raw_telemetry)) {
            edge_ws_transport_close(&session->transport, 1002, "cannot activate telemetry format");
            return;
        }
        if (!edge_acquisition_set_external_io(session->app->acquisition, session->config->id, session->external_io)) {
            edge_ws_transport_close(&session->transport, 1002, "cannot activate external IO");
            return;
        }
        syslog(LOG_INFO, "platform %s enrollment approved on existing WebSocket",
               session->config->name);
        const unsigned heartbeat = ack->heartbeat_interval_sec != 0U
                                       ? ack->heartbeat_interval_sec
                                       : session->app->config->heartbeat_interval_sec;
        session->report_interval_sec = (uint16_t)heartbeat;
        edge_retry_application_ready(&session->retry, monotonic_ms());
        ev_timer_stop(session->app->loop, &session->status_report_timer);
        ev_timer_set(&session->status_report_timer, (ev_tstamp)heartbeat, (ev_tstamp)heartbeat);
        ev_timer_start(session->app->loop, &session->status_report_timer);
        clear_report_snapshots(session);
        send_capability_report(session, true);
        (void)send_io_status(session);
        send_device_status(session, true);
        confirm_network_for_session(session);
        report_network_rollback(session->app);
        for (size_t i = 0; i < session->dtu_status_count; ++i) session->dtu_status_dirty[i] = true;
        send_dtu_status(session);
        send_outbox_window(session);
        send_pending_modem_result(session);
        break;
    }
    case iot_edge_v1_Envelope_heartbeat_ack_tag: {
        const iot_edge_v1_HeartbeatAck *ack = &envelope->payload.heartbeat_ack;
        {
            const bool was_valid = session->command_clock.valid;
            const bool accepted = edge_command_clock_accept_heartbeat(
                &session->command_clock, ack->has_nonce, ack->nonce,
                ack->has_database_time_ms, ack->database_time_ms,
                session->last_inbound_ms);
            if (!accepted && was_valid && !session->command_clock.valid)
                edge_acquisition_invalidate_command_clock(session->app->acquisition,
                                                         session->config->id);
        }
        edge_traffic_ack(&session->app->traffic,
            (size_t)(session - session->app->sessions),
            envelope->payload.heartbeat_ack.traffic_sample_id);
        edge_vpn_ack(&session->vpn, envelope->payload.heartbeat_ack.vpn_traffic_sample_id);
        const bool request_capability =
            envelope->payload.heartbeat_ack.request_capability_report;
        const bool request_device_status =
            envelope->payload.heartbeat_ack.request_device_status;
        if (session->enrolled && request_capability)
            send_capability_report(session, true);
        if (session->enrolled && request_device_status)
            send_device_status(session, true);
        if (session->firmware_traffic_flush_pending) {
            if (session->firmware_traffic_flush_followup) {
                session->firmware_traffic_flush_followup = false;
                if (!send_status_report(session)) {
                    edge_firmware_mark_traffic_flushed();
                    session->firmware_traffic_flush_pending = false;
                }
            } else {
                edge_firmware_mark_traffic_flushed();
                session->firmware_traffic_flush_pending = false;
            }
        }
        break;
    }
    case iot_edge_v1_Envelope_config_begin_tag:
    case iot_edge_v1_Envelope_config_item_tag:
    case iot_edge_v1_Envelope_config_commit_tag:
        if (session->enrolled)
            handle_config(session, envelope);
        break;
    case iot_edge_v1_Envelope_telemetry_ack_tag:
        for (pb_size_t index = 0; index < envelope->payload.telemetry_ack.accepted_record_ids_count;
             ++index) {
            const iot_edge_v1_TelemetryAck_accepted_record_ids_t *id =
                &envelope->payload.telemetry_ack.accepted_record_ids[index];
            if (id->size == 16U)
                edge_spool_outbox_ack(&session->spool, id->bytes);
        }
        send_outbox_window(session);
        break;
    case iot_edge_v1_Envelope_raw_packet_ack_tag:
        if (envelope->payload.raw_packet_ack.packet_id.size == 16U) {
            edge_spool_outbox_ack(
                &session->spool, envelope->payload.raw_packet_ack.packet_id.bytes);
            send_outbox_window(session);
        }
        break;
    case iot_edge_v1_Envelope_command_result_ack_tag:
        if (envelope->payload.command_result_ack.command_id.size == 16U) {
            edge_spool_outbox_ack(
                &session->spool, envelope->payload.command_result_ack.command_id.bytes);
            send_outbox_window(session);
        }
        break;
    case iot_edge_v1_Envelope_command_request_tag:
        if (session->enrolled)
            handle_device_command(session, &envelope->payload.command_request);
        break;
    case iot_edge_v1_Envelope_ping_tag:
        send_pong(session, envelope);
        break;
    case iot_edge_v1_Envelope_pong_tag:
        if (session->network_probe_nonce != 0U &&
            envelope->payload.pong.nonce == session->network_probe_nonce) {
            session->network_probe_nonce = 0U;
            confirm_network_for_session(session);
        }
        break;
    case iot_edge_v1_Envelope_network_config_request_tag:
        if (session->enrolled)
            handle_network_config(session, &envelope->payload.network_config_request);
        break;
    case iot_edge_v1_Envelope_vpn_config_request_tag:
        if (session->enrolled)
            handle_vpn_config(session, &envelope->payload.vpn_config_request);
        break;
    case iot_edge_v1_Envelope_firmware_update_request_tag:
        if (session->enrolled)
            handle_firmware_update(session, &envelope->payload.firmware_update_request);
        break;
    case iot_edge_v1_Envelope_firmware_chunk_tag:
        if (session->enrolled)
            handle_firmware_chunk(session, &envelope->payload.firmware_chunk);
        break;
    case iot_edge_v1_Envelope_modem_control_request_tag:
        if (session->enrolled && session->config->bootstrap)
            handle_modem_control(session, &envelope->payload.modem_control_request);
        break;
    case iot_edge_v1_Envelope_log_request_tag:
        if (session->enrolled)
            handle_log_request(session, envelope);
        break;
    case iot_edge_v1_Envelope_modbus_gateway_request_tag:
        handle_gateway_request(session, envelope);
        break;
    case iot_edge_v1_Envelope_log_level_request_tag:
        if (session->enrolled)
            handle_log_level_request(session, envelope);
        break;
    case iot_edge_v1_Envelope_serial_debug_request_tag: {
        const iot_edge_v1_SerialDebugRequest request = envelope->payload.serial_debug_request;
        if (!session->enrolled || request.session_id.size != 16) break;
        if (!strcmp(request.action, "open") && !session->serial_debug_active) {
            memcpy(session->serial_debug_id, request.session_id.bytes, 16);
            session->serial_debug_active = true;
            session->serial_debug_sequence = 0;
        }
        if (!session->serial_debug_active || memcmp(session->serial_debug_id, request.session_id.bytes, 16)) {
            iot_edge_v1_SerialDebugEvent rejected = iot_edge_v1_SerialDebugEvent_init_zero;
            edge_protocol_set_bytes(&rejected.session_id, 16, request.session_id.bytes, 16);
            rejected.sequence = 1;
            rejected.request_sequence = request.request_sequence;
            safe_copy(rejected.kind, sizeof(rejected.kind), "closed");
            safe_copy(rejected.message, sizeof(rejected.message), "another serial session is active or this session expired");
            if (init_envelope(session, envelope)) {
                envelope->which_payload = iot_edge_v1_Envelope_serial_debug_event_tag;
                envelope->payload.serial_debug_event = rejected;
                (void)send_envelope(session, envelope);
            }
            break;
        }
        if (!edge_acquisition_serial_request(session->app->acquisition, session->config->id, &request))
            close_serial_debug(session, "acquisition worker unavailable");
        break;
    }
    case iot_edge_v1_Envelope_terminal_open_tag:
        if (session->enrolled && edge_capability_has_terminal())
            handle_terminal_open(session, &envelope->payload.terminal_open);
        break;
    case iot_edge_v1_Envelope_terminal_data_tag:
        if (session->terminal_open) {
            const iot_edge_v1_TerminalData *data =
                &envelope->payload.terminal_data;
            if (data->terminal_id.size != 16U ||
                memcmp(data->terminal_id.bytes, session->terminal_id,
                       sizeof(session->terminal_id)) != 0) {
                fail_terminal(session, "terminal input identity mismatch");
                break;
            }
            uint64_t acked_sequence = 0U;
            const edge_terminal_input_result result =
                edge_terminal_write(data, &acked_sequence);
            if (result == EDGE_TERMINAL_INPUT_ACKED)
                stage_terminal_input_ack(session, acked_sequence);
            else if (result == EDGE_TERMINAL_INPUT_ERROR)
                fail_terminal(session, "terminal input sequence or write failed");
        }
        break;
    case iot_edge_v1_Envelope_terminal_data_ack_tag:
        if (session->terminal_open) {
            const iot_edge_v1_TerminalDataAck *ack =
                &envelope->payload.terminal_data_ack;
            if (ack->terminal_id.size != 16U || ack->sequence == 0U ||
                memcmp(ack->terminal_id.bytes, session->terminal_id,
                       sizeof(session->terminal_id)) != 0 ||
                (!session->terminal_output_pending &&
                 ack->sequence != session->terminal_output_acked_sequence) ||
                (session->terminal_output_pending &&
                 ack->sequence != session->terminal_output_sequence)) {
                fail_terminal(session, "terminal output acknowledgement mismatch");
            } else if (session->terminal_output_pending) {
                session->terminal_output_acked_sequence = ack->sequence;
                session->terminal_output_size = 0U;
                session->terminal_output_deadline_ms = 0U;
                session->terminal_output_pending = false;
                session->terminal_output_sent = false;
            }
        }
        break;
    case iot_edge_v1_Envelope_terminal_resize_tag:
        if (session->terminal_open) {
            const iot_edge_v1_TerminalResize *resize =
                &envelope->payload.terminal_resize;
            if (resize->terminal_id.size != 16U ||
                memcmp(resize->terminal_id.bytes, session->terminal_id,
                       sizeof(session->terminal_id)) != 0)
                fail_terminal(session, "terminal resize identity mismatch");
            else if (!edge_terminal_resize(resize))
                fail_terminal(session, "terminal resize failed");
        }
        break;
    case iot_edge_v1_Envelope_terminal_close_tag:
        if (session->terminal_open) {
            if (envelope->payload.terminal_close.terminal_id.size != 16U ||
                memcmp(envelope->payload.terminal_close.terminal_id.bytes,
                       session->terminal_id, sizeof(session->terminal_id)) != 0) {
                fail_terminal(session, "terminal close identity mismatch");
            } else {
                edge_terminal_close(session->terminal_id);
                ev_timer_stop(session->app->loop, &session->terminal_timer);
                session->terminal_open = false;
                reset_terminal_flow(session);
            }
        }
        break;
    case iot_edge_v1_Envelope_enrollment_pending_tag:
        syslog(LOG_INFO, "platform %s enrollment pending", session->config->name);
        // 待审批是有效应用响应，不应每 30 秒重连。仅发送心跳等待原连接获批。
        session->report_interval_sec = 300U;
        edge_retry_application_ready(&session->retry, monotonic_ms());
        ev_timer_stop(session->app->loop, &session->status_report_timer);
        ev_timer_set(&session->status_report_timer, 300.0, 300.0);
        ev_timer_start(session->app->loop, &session->status_report_timer);
        break;
    case iot_edge_v1_Envelope_enrollment_rejected_tag:
        syslog(LOG_WARNING, "platform %s enrollment rejected", session->config->name);
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK, 14U);
        edge_ws_transport_close(&session->transport, 1008,
                           "enrollment rejected");
        break;
    default:
        break;
    }
}

static void status_report_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    (void)send_status_report(session_from_status_report(timer));
}

static void websocket_message(void *user, void *data, size_t size, bool binary) {
    edge_ws_session *session = user;
    /* Never decode into the shared output scratch buffer. A text/empty frame
     * must not release another platform's last outbound telemetry pointers,
     * and sending a reply must not overwrite the request still being handled. */
    iot_edge_v1_Envelope incoming = iot_edge_v1_Envelope_init_zero;
    const char *error = NULL;
    if (!binary || !edge_protocol_decode(data, size, &incoming, &error) ||
        !valid_origin(session, &incoming)) {
        syslog(LOG_WARNING, "platform %s sent invalid nanopb envelope: %s",
               session->config->name, error != NULL ? error : "wrong origin or frame type");
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_PROTOCOL_DECODE, 8U);
        edge_ws_transport_close(&session->transport, 1002, "invalid envelope");
    } else {
        dispatch_platform_message(session, &incoming);
    }
    edge_protocol_release(&incoming);
}

static void status_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_app *app = app_from_status(timer);
    edge_terminal_reap();
    edge_status_platform platforms[EDGE_MAX_PLATFORMS];
    for (size_t index = 0U; index < app->config->platform_count; ++index) {
        const edge_ws_session *session = &app->sessions[index];
        platforms[index] = (edge_status_platform){
            .config = session->config,
            .runtime_config = &session->runtime_config,
            .staging_config = &session->spool.staging_config,
            .outbox = &session->spool.outbox,
            .phase = session->retry.phase,
            .last_heartbeat_ms = session->last_report_ms,
            .last_inbound_ms = session->last_inbound_ms,
            .heartbeat_interval_sec = session->report_interval_sec,
            .websocket_open = session->websocket_open,
            .enrolled = session->enrolled,
        };
    }
    const edge_status_snapshot snapshot = {
        .software_version = EDGE_SOFTWARE_VERSION,
        .now_ms = monotonic_ms(),
        .platforms = platforms,
        .platform_count = app->config->platform_count,
    };
    (void)edge_status_write(&snapshot);
    edge_process_report_progress();
}

static void firmware_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_firmware(timer);
    if (!session->enrolled)
        return;
    if (edge_firmware_receiving(session->config->id))
        (void)send_firmware_chunk_request(session, false);
    iot_edge_v1_FirmwareUpdateResult result = iot_edge_v1_FirmwareUpdateResult_init_zero;
    if (!edge_firmware_read_status(session->config->id, &result)) {
        if (session->firmware_operation_active && !edge_firmware_active()) {
            send_firmware_result(
                session, session->firmware_request_id,
                iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                "firmware worker exited unexpectedly", 0U,
                session->firmware_total_bytes, 0U);
            log_uuid_task(session, "firmware-update",
                          session->firmware_request_id, "failed",
                          "firmware worker exited unexpectedly");
            session->firmware_operation_active = false;
            ev_timer_stop(session->app->loop, &session->firmware_timer);
        }
        return;
    }
    uint8_t request_id[16] = {0};
    if (result.request_id.size == 16U)
        memcpy(request_id, result.request_id.bytes, sizeof(request_id));
    memcpy(session->firmware_request_id, request_id,
           sizeof(session->firmware_request_id));
    session->firmware_total_bytes = result.total_bytes;
    session->firmware_operation_active = true;
    const iot_edge_v1_FirmwareUpdateState state = result.state;
    char message[257];
    safe_copy(message, sizeof(message), result.message);
    send_firmware_result(session, request_id, state, message,
                         result.downloaded_bytes, result.total_bytes,
                         result.progress_percent);
    char summary[160];
    snprintf(summary, sizeof(summary), "progress=%u bytes=%" PRIu64 "/%" PRIu64
             " message=%.80s", result.progress_percent, result.downloaded_bytes,
             result.total_bytes, message);
    log_uuid_task(session, "firmware-update", request_id,
                  firmware_state_name(state), summary);
    if (state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED) {
        session->firmware_operation_active = false;
        ev_timer_stop(session->app->loop, &session->firmware_timer);
    }
}

static void send_pending_modem_result(edge_ws_session *session) {
    if (session->modem_result_pending && session->websocket_open && session->enrolled &&
        send_modem_result(session, session->modem_result_request_id,
                          session->modem_result_action, session->modem_result_state,
                          session->modem_result_message, session->modem_result_apn))
        session->modem_result_pending = false;
}

static void read_modem_worker_result(edge_ws_session *session) {
    if (session->modem_worker_fd >= 0) {
        edge_modem_worker_result result;
        const ssize_t size = recv(session->modem_worker_fd, &result, sizeof(result), 0);
        if (size == (ssize_t)sizeof(result)) {
            memcpy(session->modem_result_request_id, result.request_id,
                   sizeof(session->modem_result_request_id));
            session->modem_result_action = result.action;
            session->modem_result_state =
                result.success
                    ? iot_edge_v1_ModemControlState_MODEM_CONTROL_SUCCEEDED
                    : iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED;
            safe_copy(session->modem_result_message,
                      sizeof(session->modem_result_message), result.message);
            safe_copy(session->modem_result_apn, sizeof(session->modem_result_apn),
                      result.apn);
            session->modem_result_received = true;
            session->modem_result_pending = true;
            ev_io_stop(session->app->loop, &session->modem_io);
            close(session->modem_worker_fd);
            session->modem_worker_fd = -1;
        } else if (size == 0 ||
                   (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                    errno != EINTR)) {
            ev_io_stop(session->app->loop, &session->modem_io);
            close(session->modem_worker_fd);
            session->modem_worker_fd = -1;
        }
    }
}

static void modem_io(struct ev_loop *loop, struct ev_io *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem_io(watcher);
    read_modem_worker_result(session);
    send_pending_modem_result(session);
}

static void modem_child(struct ev_loop *loop, struct ev_child *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem_child(watcher);
    ev_child_stop(session->app->loop, &session->modem_child);
    ev_timer_stop(session->app->loop, &session->modem_timer);
    read_modem_worker_result(session);
    session->modem_worker_pid = 0;
    if (session->modem_worker_fd >= 0) {
        ev_io_stop(session->app->loop, &session->modem_io);
        close(session->modem_worker_fd);
        session->modem_worker_fd = -1;
    }
    if (!session->modem_result_received)
        session->modem_result_pending = true;
    send_pending_modem_result(session);
}

static void modem_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem(timer);
    ev_timer_stop(session->app->loop, &session->modem_timer);
    if (session->modem_worker_pid > 0) {
        safe_copy(session->modem_result_message,
                  sizeof(session->modem_result_message),
                  "modem operation timed out");
        (void)kill(session->modem_worker_pid, SIGKILL);
    }
}

static void network_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_network(timer);
    ev_timer_stop(session->app->loop, &session->network_timer);
    if (!session->websocket_open || !session->enrolled)
        return;
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return;
    envelope->which_payload = iot_edge_v1_Envelope_ping_tag;
    const int64_t timestamp = now_ms();
    session->network_probe_nonce =
        timestamp > 0 ? (uint64_t)timestamp : session->sequence;
    envelope->payload.ping.nonce = session->network_probe_nonce;
    if (!send_envelope(session, envelope))
        session->network_probe_nonce = 0U;
}

static void reload_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_reload(timer);
    ev_timer_stop(session->app->loop, &session->reload_timer);
    raise(SIGHUP);
}

static void terminal_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_terminal(timer);
    if (!session->terminal_open)
        return;
    if (session->terminal_input_ack_pending &&
        send_terminal_data_ack(session, session->terminal_id,
                               session->terminal_input_ack_sequence))
        session->terminal_input_ack_pending = false;

    uint64_t input_acked_sequence = 0U;
    const edge_terminal_input_result input_result =
        edge_terminal_flush(session->terminal_id, &input_acked_sequence);
    if (input_result == EDGE_TERMINAL_INPUT_ERROR) {
        fail_terminal(session, "terminal input write failed");
        return;
    }
    if (input_result == EDGE_TERMINAL_INPUT_ACKED)
        stage_terminal_input_ack(session, input_acked_sequence);

    const uint64_t now = monotonic_ms();
    if (session->terminal_output_pending) {
        if (now >= session->terminal_output_deadline_ms) {
            fail_terminal(session, "terminal output acknowledgement timed out");
            return;
        }
        if (!session->terminal_output_sent && send_terminal_output(session))
            session->terminal_output_sent = true;
        return;
    }

    bool closed = false;
    int32_t exit_code = 0;
    const ssize_t size =
        edge_terminal_read(session->terminal_id, session->terminal_output,
                           sizeof(session->terminal_output), &closed, &exit_code);
    if (size > 0) {
        if (session->terminal_output_sequence == UINT64_MAX) {
            fail_terminal(session, "terminal output sequence exhausted");
            return;
        }
        session->terminal_output_size = (size_t)size;
        ++session->terminal_output_sequence;
        session->terminal_output_pending = true;
        session->terminal_output_sent = send_terminal_output(session);
        session->terminal_output_deadline_ms = now + EDGE_TERMINAL_ACK_TIMEOUT_MS;
    }
    if (closed) {
        ev_timer_stop(session->app->loop, &session->terminal_timer);
        session->terminal_open = false;
        send_terminal_close(session, session->terminal_id, exit_code, "terminal closed");
        reset_terminal_flow(session);
    }
}

static void acquisition_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_app *app = app_from_acquisition(timer);
    edge_acquisition_tick(app->acquisition, monotonic_ms());
    for (size_t i = 0; i < app->config->platform_count; ++i) {
        send_dtu_status(&app->sessions[i]);
        send_outbox_window(&app->sessions[i]);
    }
    sync_acquisition_io(app);
}

static void acquisition_io(struct ev_loop *loop, struct ev_io *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_app *app = app_from_acquisition_io(watcher);
    edge_acquisition_tick(app->acquisition, monotonic_ms());
    for (size_t i = 0; i < app->config->platform_count; ++i) send_dtu_status(&app->sessions[i]);
    sync_acquisition_io(app);
}

static void start_connection(edge_ws_session *session) {
    edge_retry_attempt_started(&session->retry, monotonic_ms());
    if (!edge_url_make_platform_transport(session->config->url,
                                          session->transport_url,
                                          sizeof(session->transport_url))) {
        syslog(LOG_WARNING, "platform %s has an invalid WebSocket base URL",
               session->config->name);
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_CONFIG, 9U);
        schedule_reconnect(session);
        return;
    }
    syslog(LOG_INFO, "platform %s WebSocket connecting to %s", session->config->name,
           session->transport_url);
    if (!edge_ws_transport_connect(&session->transport, session->app->loop,
                                    session->transport_url, EDGENODE_MAX_WS_MESSAGE,
                                    session, websocket_open, websocket_message, websocket_close)) {
        syslog(LOG_WARNING, "platform %s WebSocket connect initialization failed",
               session->config->name);
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK, 15U);
        schedule_reconnect(session);
        return;
    }
    session->client_active = true;
    arm_reconnect_timer(session);
}

static void reconnect_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_reconnect(timer);
    const uint64_t current_ms = monotonic_ms();
    if (edge_retry_attempt_timed_out(&session->retry, current_ms)) {
        syslog(LOG_WARNING, "platform %s WebSocket connection attempt timed out",
               session->config->name);
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_NETWORK, 1U);
        edge_ws_transport_destroy(&session->transport);
        session->client_active = false;
        schedule_reconnect(session);
        return;
    }
    if (!edge_retry_should_start(&session->retry, current_ms)) {
        arm_reconnect_timer(session);
        return;
    }
    start_connection(session);
}

static void liveness_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_liveness(timer);
    if (!session->websocket_open)
        return;
    const uint64_t current_ms = monotonic_ms();
    const bool application_stalled =
        edge_retry_application_timed_out(&session->retry, current_ms);
    bool acknowledgement_stalled =
        edge_spool_outbox_timed_out(&session->spool, current_ms,
                                    EDGE_OUTBOX_ACK_TIMEOUT_MS);
    if (acknowledgement_stalled)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_OUTBOX_SEND, 2U);
    if (!application_stalled && acknowledgement_stalled) {
        const size_t retried = edge_memory_outbox_retry_expired(
            &session->spool.outbox, current_ms, EDGE_OUTBOX_ACK_TIMEOUT_MS, 2U);
        if (retried != SIZE_MAX) {
            acknowledgement_stalled = false;
            send_outbox_window(session);
        }
    }
    if (!application_stalled && !acknowledgement_stalled)
        return;
    const char *reason = application_stalled
                             ? "application handshake timed out"
                             : "outbox acknowledgement timed out";
    syslog(LOG_WARNING, "platform %s %s; reconnecting",
           session->config->name, reason);
    char detail[160];
    snprintf(detail, sizeof(detail), "platform=%s reason=%s",
             session->config->name, reason);
    edge_log_write("warn", "ws", "platform session stalled", detail);
    edge_log_local_fault(session->config->id, NULL, 0U,
                         EDGE_LOCAL_FAULT_NETWORK,
                         application_stalled ? 2U : 3U);
    edge_ws_transport_destroy(&session->transport);
    session->client_active = false;
    schedule_reconnect(session);
}

bool edge_ws_app_init(edge_ws_app *app, struct ev_loop *loop,
                      const edge_app_config *config) {
    if (app == NULL || loop == NULL || config == NULL)
        return false;
    memset(app, 0, sizeof(*app));
    edge_log_init();
    app->loop = loop;
    app->config_storage = *config;
    config = &app->config_storage;
    app->config = config;
    app->traffic.event_fd = app->traffic.query_fd = -1;
    if (!edge_vpn_recover_resources()) return false;
    for (size_t index = 0; index < config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        session->app = app;
        session->config = &config->platforms[index];
        if (!edge_vpn_init(&session->vpn, session->config->id, index)) return false;
        session->modem_worker_fd = -1;
        if (!edge_spool_init(&session->spool, session->config->id,
                             session->config->outbox_max_bytes)) {
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        session->active_revision = session->spool.active_config.revision;
        if (session->active_revision != 0U) {
            char error[256];
            if (!edge_runtime_config_load(&session->runtime_config,
                                          &session->spool.active_config,
                                          error, sizeof(error))) {
                syslog(LOG_ERR, "platform %s active config rejected: %s",
                       session->config->name, error);
                edge_log_local_fault(session->config->id, NULL, 0U,
                                     EDGE_LOCAL_FAULT_CONFIG, 5U);
                for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                    edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                    edge_spool_free(&app->sessions[cleanup].spool);
                }
                return false;
            }
        }
        ev_timer_init(&session->reconnect_timer, reconnect_timer, 0.0, 0.0);
        if (!edge_retry_init(&session->retry,
                             (uint32_t)session->config->reconnect_interval_sec * 1000U,
                             EDGE_CONNECT_TIMEOUT_SEC * 1000U)) {
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        ev_timer_init(&session->liveness_timer, liveness_timer, 0.0, 0.0);
        ev_timer_init(&session->status_report_timer, status_report_timer, 0.0, 0.0);
        ev_timer_init(&session->firmware_timer, firmware_timer, 0.0, 0.0);
        ev_timer_init(&session->modem_timer, modem_timer, 0.0, 0.0);
        ev_io_init(&session->modem_io, modem_io, 0, EV_READ);
        ev_child_init(&session->modem_child, modem_child, 0, 0);
        ev_timer_init(&session->network_timer, network_timer, 0.0, 0.0);
        ev_timer_init(&session->reload_timer, reload_timer, 0.0, 0.0);
        ev_timer_init(&session->terminal_timer, terminal_timer, 0.0, 0.0);
    }
    ev_timer_init(&app->acquisition_timer, acquisition_timer, 0.0, 0.0);
    ev_timer_init(&app->status_timer, status_timer, 0.0, 0.0);
    ev_io_init(&app->acquisition_io, acquisition_io, 0, EV_READ);
    edge_acquisition_source sources[EDGE_MAX_PLATFORMS];
    const size_t source_count = build_acquisition_sources(app, NULL, NULL, sources);
    app->acquisition = edge_acquisition_create(
        acquisition_telemetry, acquisition_command_result, app);
    edge_acquisition_set_sample_callback(app->acquisition, acquisition_sample);
    (void)edge_acquisition_set_gateway_enabled(app->acquisition, config->modbus_gateway);
    edge_acquisition_set_debug_callback(app->acquisition, acquisition_debug);
    edge_acquisition_set_dtu_callback(app->acquisition, acquisition_dtu);
    edge_acquisition_set_io_callback(app->acquisition, acquisition_io_capability);
    edge_acquisition_enable_serial_debug(app->acquisition, config->serial_port,
        config->serial_rs485, acquisition_serial);
    char acquisition_error[256] = {0};
    if (app->acquisition == NULL ||
        !edge_acquisition_apply_multi(app->acquisition, sources, source_count,
                                      monotonic_ms(), acquisition_error,
                                      sizeof(acquisition_error)) ||
        (config->modbus_gateway &&
         (!(app->gateway = prepare_gateway(sources, source_count, config->modbus_gateway_port)) ||
          !edge_modbus_gateway_set_unit_id(app->gateway, config->modbus_gateway_unit_id))) ||
        !edge_acquisition_start(app->acquisition, acquisition_error,
                                sizeof(acquisition_error))) {
        syslog(LOG_ERR, "acquisition or Modbus gateway initialization rejected: %s", acquisition_error[0] ? acquisition_error : "gateway map unavailable or LAN listener cannot bind");
        edge_log_local_fault(NULL, NULL, 0U, EDGE_LOCAL_FAULT_CONFIG, 6U);
        edge_acquisition_destroy(app->acquisition);
        app->acquisition = NULL;
        edge_modbus_gateway_close(app->gateway_server);
        edge_modbus_gateway_free(app->gateway);
        app->gateway_server = NULL; app->gateway = NULL;
        for (size_t cleanup = 0U; cleanup < config->platform_count; ++cleanup) {
            edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
            edge_spool_free(&app->sessions[cleanup].spool);
        }
        return false;
    }
    edge_modbus_gateway_set_writer(app->gateway_server, gateway_write, app);
    char logical_name[33] = {0};
    const char *previous = config->modbus_gateway_interface[0] ?
        config->modbus_gateway_interface : config->lan_interface;
    if (edge_network_gateway_migrate(previous, logical_name, app->gateway_error, sizeof(app->gateway_error))) {
        if (strcmp(logical_name, config->modbus_gateway_interface)) {
            edge_app_config candidate = app->config_storage;
            safe_copy(candidate.modbus_gateway_interface, sizeof(candidate.modbus_gateway_interface), logical_name);
            if (edge_config_save_modbus_gateway(&candidate))
                app->config_storage = candidate;
            else safe_copy(app->gateway_error, sizeof(app->gateway_error), "cannot atomically persist legacy gateway interface migration");
        }
    }
    app->gateway_network_watch = edge_network_watch_open(loop, gateway_network_changed, app);
    app->gateway_network_available = app->gateway_network_watch != NULL;
    if (!app->gateway_error[0])
        gateway_network_changed(app, app->gateway_network_watch != NULL);
    if (app->gateway_error[0])
        syslog(LOG_ERR, "Modbus gateway stopped: %s", app->gateway_error);
    export_gateway(app);
    return true;
}

void edge_ws_app_start(edge_ws_app *app) {
    if (app == NULL)
        return;
    _Static_assert(EDGE_MAX_PLATFORMS <= EDGE_TRAFFIC_MAX_PLATFORMS, "traffic platform capacity");
    if (edge_traffic_open(&app->traffic, app->config->platform_count, monotonic_ms())) {
        ev_io_init(&app->traffic_io, traffic_ready, app->traffic.event_fd, EV_READ);
        ev_io_start(app->loop, &app->traffic_io);
    } else {
        syslog(LOG_WARNING, "TCP traffic accounting unavailable");
    }
    ev_timer_set(&app->acquisition_timer, 0.0, 1.0);
    ev_timer_start(app->loop, &app->acquisition_timer);
    ev_timer_set(&app->status_timer, 0.1, 2.0);
    ev_timer_start(app->loop, &app->status_timer);
    sync_acquisition_io(app);
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        if (edge_firmware_has_status(app->sessions[index].config->id) ||
            edge_firmware_receiving(app->sessions[index].config->id)) {
            app->sessions[index].firmware_operation_active = true;
            ev_timer_set(&app->sessions[index].firmware_timer, 0.25, 0.5);
            ev_timer_start(app->loop, &app->sessions[index].firmware_timer);
        }
        start_connection(&app->sessions[index]);
    }
}

void edge_ws_app_stop(edge_ws_app *app) {
    if (app == NULL)
        return;
    if (app->traffic.event_fd >= 0) ev_io_stop(app->loop, &app->traffic_io);
    edge_network_watch_close(app->gateway_network_watch);
    app->gateway_network_watch = NULL;
    edge_traffic_close(&app->traffic);
    ev_timer_stop(app->loop, &app->acquisition_timer);
    ev_timer_stop(app->loop, &app->status_timer);
    edge_status_remove();
    ev_io_stop(app->loop, &app->acquisition_io);
    edge_modbus_gateway_close(app->gateway_server);
    app->gateway_server = NULL;
    edge_acquisition_destroy(app->acquisition);
    gateway_commands_stopped(app);
    app->acquisition = NULL;
    edge_modbus_gateway_free(app->gateway);
    app->gateway_server = NULL; app->gateway = NULL;
    remove("/tmp/edgenode-modbus.csv");
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        clear_report_snapshots(session);
        edge_vpn_shutdown(&session->vpn);
        ev_timer_stop(app->loop, &session->reconnect_timer);
        ev_timer_stop(app->loop, &session->liveness_timer);
        ev_timer_stop(app->loop, &session->status_report_timer);
        ev_timer_stop(app->loop, &session->firmware_timer);
        ev_timer_stop(app->loop, &session->modem_timer);
        ev_timer_stop(app->loop, &session->network_timer);
        ev_timer_stop(app->loop, &session->reload_timer);
        ev_timer_stop(app->loop, &session->terminal_timer);
        ev_io_stop(app->loop, &session->modem_io);
        ev_child_stop(app->loop, &session->modem_child);
        if (session->terminal_open) {
            edge_terminal_close(session->terminal_id);
            session->terminal_open = false;
        }
        reset_terminal_flow(session);
        if (session->modem_worker_pid > 0) {
            (void)kill(session->modem_worker_pid, SIGTERM);
            bool reaped = false;
            for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
                pid_t waited;
                do {
                    waited = waitpid(session->modem_worker_pid, NULL, WNOHANG);
                } while (waited < 0 && errno == EINTR);
                if (waited == session->modem_worker_pid ||
                    (waited < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                usleep(5000U);
            }
            if (!reaped) {
                (void)kill(session->modem_worker_pid, SIGKILL);
                while (waitpid(session->modem_worker_pid, NULL, 0) < 0 &&
                       errno == EINTR) {
                }
            }
            session->modem_worker_pid = 0;
        }
        if (session->modem_worker_fd >= 0) {
            close(session->modem_worker_fd);
            session->modem_worker_fd = -1;
        }
        edge_ws_transport_destroy(&session->transport);
        edge_spool_free(&session->spool);
        edge_runtime_config_free(&session->runtime_config);
        session->client_active = false;
        session->websocket_open = false;
    }
}

bool edge_ws_app_enqueue(edge_ws_app *app, const uint8_t origin_platform_id[16],
                         const uint8_t ack_id[16],
                         const iot_edge_v1_Envelope *envelope) {
    if (app == NULL || origin_platform_id == NULL || ack_id == NULL || envelope == NULL ||
        envelope->platform_id.size != 16U ||
        memcmp(envelope->platform_id.bytes, origin_platform_id, 16U) != 0)
        return false;
    edge_ws_session *session = NULL;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        if (memcmp(app->sessions[index].config->id, origin_platform_id, 16U) == 0) {
            session = &app->sessions[index];
            break;
        }
    }
    if (session == NULL)
        return false;
    size_t wire_size = 0U;
    const char *error = NULL;
    const bool encoded = edge_protocol_encode(envelope, app->wire,
                                               sizeof(app->wire), &wire_size, &error);
    const bool priority = envelope->which_payload == iot_edge_v1_Envelope_command_result_tag ||
        (envelope->which_payload == iot_edge_v1_Envelope_telemetry_batch_tag &&
         envelope->payload.telemetry_batch.records_count == 1U &&
         envelope->payload.telemetry_batch.records[0].protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO);
    if (!encoded || !edge_spool_outbox_put_priority(
            &session->spool, ack_id, app->wire, wire_size,
            priority)) {
        syslog(LOG_ERR, "cannot queue nanopb message for origin platform %s: %s",
               session->config->name, error != NULL ? error : "tmpfs spool rejected");
        edge_log_local_fault(origin_platform_id, NULL, 0U,
                             EDGE_LOCAL_FAULT_PARENT_OUTBOX, encoded ? 7U : 8U);
        return false;
    }
    send_outbox_window(session);
    return true;
}
