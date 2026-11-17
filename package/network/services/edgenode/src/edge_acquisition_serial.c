#define _GNU_SOURCE

#include "edge_acquisition_internal.h"

bool serial_paused(const edge_acquisition_device *device) {
    if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL) return false;
    for (size_t i = 0; i < 4; ++i) {
        const edge_serial_session *session = &device->owner->serial_sessions[i];
        if (session->active && session->manual &&
            !strcmp(session->settings.channel, device->endpoint->serial.channel)) return true;
    }
    return false;
}

static void serial_emit(edge_acquisition *acquisition, edge_serial_session *session,
                        uint64_t request, const char *kind, const char *direction,
                        const uint8_t *bytes, size_t size, const char *reason) {
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_EVENT_SERIAL,
                             sizeof(message.payload.serial_event));
    memcpy(message.platform_id, session->platform_id, 16);
    iot_edge_v1_SerialDebugEvent *event = &message.payload.serial_event;
    memset(event, 0, sizeof(*event));
    edge_protocol_set_bytes(&event->session_id, 16, session->id, 16);
    event->request_sequence = request;
    event->sequence = ++session->event_sequence;
    event->manual = session->manual;
    event->occurred_at_ms = current_ms();
    event->dropped_bytes = session->dropped_bytes;
    event->has_settings = true;
    event->settings = session->settings;
    copy_text(event->kind, sizeof(event->kind), kind);
    copy_text(event->direction, sizeof(event->direction), direction);
    copy_text(event->message, sizeof(event->message), reason);
    edge_protocol_set_bytes(&event->data, sizeof(event->data.bytes), bytes, size);
    const size_t wire_size = acquisition_message_size(message.payload_size);
    if (send(acquisition->worker_fd, &message, wire_size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)wire_size)
        session->dropped_bytes += size;
}

void serial_observe(edge_acquisition *acquisition, const char *path,
                    const iot_edge_v1_SerialSettings *settings, const char *direction,
                    const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < 4; ++i) {
        edge_serial_session *session = &acquisition->serial_sessions[i];
        if (!session->active || strcmp(session->settings.channel, path)) continue;
        if (!session->manual) session->settings = *settings;
        for (size_t offset = 0; offset < size;) {
            const size_t part = size - offset > 1024 ? 1024 : size - offset;
            serial_emit(acquisition, session, 0, "data", direction, bytes + offset, part, "");
            offset += part;
        }
    }
}

static void serial_release(edge_acquisition *acquisition, edge_serial_session *session,
                           uint64_t request, const char *reason) {
    if (session->fd >= 0) close(session->fd);
    session->fd = -1;
    session->manual = false;
    serial_emit(acquisition, session, request, "closed", "", NULL, 0, reason);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

static bool serial_configured(edge_acquisition *acquisition, edge_serial_session *session,
                              bool update_settings) {
    for (size_t i = 0; i < acquisition->device_count; ++i) {
        const edge_acquisition_device *device = &acquisition->devices[i];
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL &&
            !strcmp(device->endpoint->serial.channel, session->settings.channel)) {
            if (update_settings) session->settings = device->endpoint->serial;
            return true;
        }
    }
    return false;
}

static bool serial_settings_valid(const iot_edge_v1_SerialSettings *settings) {
    return baud_rate(settings->baud_rate) != 0 && settings->data_bits >= 5 &&
        settings->data_bits <= 8 && (settings->stop_bits == 1 || settings->stop_bits == 2) &&
        (!strcmp(settings->parity, "none") || !strcmp(settings->parity, "even") || !strcmp(settings->parity, "odd"));
}

void serial_control(edge_acquisition *acquisition, const uint8_t platform_id[16],
                    const iot_edge_v1_SerialDebugRequest *request) {
    if (request->session_id.size != 16 || request->request_sequence == 0) return;
    edge_serial_session *session = NULL;
    for (size_t i = 0; i < 4; ++i)
        if (acquisition->serial_sessions[i].active &&
            !memcmp(acquisition->serial_sessions[i].platform_id, platform_id, 16)) {
            session = &acquisition->serial_sessions[i]; break;
        }
    if (!strcmp(request->action, "open")) {
        if (session) {
            // Replaying OPEN never resets an existing session or its manual-write sequence.
            if (!memcmp(session->id, request->session_id.bytes, 16)) {
                serial_emit(acquisition, session, request->request_sequence, "state", "", NULL, 0, "");
                return;
            }
            edge_serial_session rejected = {.fd = -1};
            memcpy(rejected.platform_id, platform_id, 16);
            memcpy(rejected.id, request->session_id.bytes, 16);
            serial_emit(acquisition, &rejected, request->request_sequence, "closed", "", NULL, 0,
                        "this platform already has a serial debug session");
            return;
        }
        for (size_t i = 0; i < 4; ++i)
            if (!acquisition->serial_sessions[i].active) { session = &acquisition->serial_sessions[i]; break; }
        if (!session) return;
        memset(session, 0, sizeof(*session));
        session->fd = -1;
        memcpy(session->platform_id, platform_id, 16);
        memcpy(session->id, request->session_id.bytes, 16);
        session->settings = request->settings;
        if (!request->has_settings || acquisition->serial_path[0] == '\0' ||
            strcmp(request->settings.channel, acquisition->serial_path)) {
            serial_release(acquisition, session, request->request_sequence, "serial port is not advertised");
            return;
        }
        for (size_t i = 0; i < acquisition->dtu_count; ++i)
            if (acquisition->dtu[i].config.south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL &&
                !strcmp(acquisition->dtu[i].config.serial.channel, request->settings.channel)) {
                serial_release(acquisition, session, request->request_sequence, "serial port is owned by DTU passthrough");
                return;
            }
        session->active = true;
        session->settings.baud_rate = 9600;
        session->settings.data_bits = 8;
        session->settings.stop_bits = 1;
        session->settings.rs485 = acquisition->serial_rs485;
        copy_text(session->settings.parity, sizeof(session->settings.parity), "none");
        (void)serial_configured(acquisition, session, true);
    } else if (!session || memcmp(session->id, request->session_id.bytes, 16)) {
        if (strcmp(request->action, "close")) {
            edge_serial_session missing = {.fd = -1};
            memcpy(missing.platform_id, platform_id, 16); memcpy(missing.id, request->session_id.bytes, 16);
            serial_emit(acquisition, &missing, request->request_sequence, "closed", "", NULL, 0,
                        "serial debug session expired or acquisition restarted");
        }
        return;
    }
    if (request->request_sequence <= session->last_request) return;
    session->last_request = request->request_sequence;
    if (!strcmp(request->action, "close")) {
        serial_release(acquisition, session, request->request_sequence, "serial debug closed"); return;
    }
    session->expires_ms = monotonic_milliseconds() + 60000;
    if (!strcmp(request->action, "keepalive")) {
        serial_emit(acquisition, session, request->request_sequence, "state", "", NULL, 0, ""); return;
    }
    if (session->pending_size) {
        serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "serial write still pending"); return;
    }
    if (!strcmp(request->action, "manual")) {
        if (!request->has_settings || strcmp(request->settings.channel, session->settings.channel) ||
            !serial_settings_valid(&request->settings)) {
            serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "invalid serial settings"); return;
        }
        for (size_t i = 0; i < 4; ++i)
            if (&acquisition->serial_sessions[i] != session && acquisition->serial_sessions[i].active &&
                acquisition->serial_sessions[i].manual &&
                !strcmp(acquisition->serial_sessions[i].settings.channel, session->settings.channel)) {
                serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0,
                            "another platform controls this serial port"); return;
            }
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *device = &acquisition->devices[i];
            if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL ||
                strcmp(device->endpoint->serial.channel, session->settings.channel)) continue;
            if (device->runtime.write_count || (device->sl651 &&
                (!edge_sl651_ready(device->sl651) || device->sl651_query_active || device->sl651_result_pending))) {
                serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0,
                            "device transaction pending; retry after it completes"); return;
            }
        }
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *device = &acquisition->devices[i];
            if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL ||
                strcmp(device->endpoint->serial.channel, session->settings.channel)) continue;
            edge_device_runtime_close(&device->runtime);
            close_fd(&device->link->fd);
            if (device->sl651) edge_sl651_reset(device->sl651);
        }
        for (size_t i = 0; i < 4; ++i)
            if (acquisition->serial_sessions[i].active &&
                !strcmp(acquisition->serial_sessions[i].settings.channel, session->settings.channel))
                close_fd(&acquisition->serial_sessions[i].fd);
        session->settings = request->settings;
        session->fd = open_serial(&session->settings);
        if (session->fd < 0) {
            session->manual = false;
            serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "cannot configure serial port"); return;
        }
        session->manual = true;
    } else if (!strcmp(request->action, "monitor")) {
        close_fd(&session->fd);
        session->manual = false;
        (void)serial_configured(acquisition, session, true);
    } else if (!strcmp(request->action, "write")) {
        if (!session->manual || request->data.size == 0 || request->data.size > sizeof(session->pending)) {
            serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0,
                        "pause automatic acquisition before manual sending"); return;
        }
        memcpy(session->pending, request->data.bytes, request->data.size);
        session->pending_size = request->data.size;
        session->pending_offset = 0;
        session->pending_request = request->request_sequence;
        session->pending_deadline = monotonic_milliseconds() + 10000;
        return;
    } else if (strcmp(request->action, "open")) {
        serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "unknown serial action"); return;
    }
    serial_emit(acquisition, session, request->request_sequence, "state", "", NULL, 0, "");
}

void serial_tick(edge_acquisition *acquisition, uint64_t now) {
    for (size_t i = 0; i < 4; ++i) {
        edge_serial_session *session = &acquisition->serial_sessions[i];
        if (!session->active) continue;
        if (now >= session->expires_ms) { serial_release(acquisition, session, 0, "serial debug lease expired"); continue; }
        if (session->fd < 0 && !session->manual) {
            if (serial_configured(acquisition, session, false)) continue;
            bool held = false;
            for (size_t j = 0; j < 4; ++j)
                if (i != j && acquisition->serial_sessions[j].active &&
                    acquisition->serial_sessions[j].fd >= 0 &&
                    !strcmp(acquisition->serial_sessions[j].settings.channel, session->settings.channel)) held = true;
            if (held) continue;
            session->fd = open_serial(&session->settings);
            if (session->fd < 0) { serial_release(acquisition, session, 0, "cannot open serial port"); continue; }
        }
        if (session->pending_size) {
            const ssize_t count = write(session->fd, session->pending + session->pending_offset,
                session->pending_size - session->pending_offset);
            if (count > 0) {
                serial_observe(acquisition, session->settings.channel, &session->settings, "TX",
                    session->pending + session->pending_offset, (size_t)count);
                session->pending_offset += (size_t)count;
            } else if ((count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ||
                       now >= session->pending_deadline) {
                serial_release(acquisition, session, session->pending_request,
                    "serial write failed; some bytes may have been sent"); continue;
            }
            if (session->pending_offset == session->pending_size) {
                session->pending_size = session->pending_offset = 0;
                serial_emit(acquisition, session, session->pending_request, "sent", "", NULL, 0, "");
            }
        }
        if (session->fd < 0) continue;
        struct pollfd descriptor = {.fd = session->fd, .events = POLLIN};
        if (poll(&descriptor, 1, 0) <= 0) continue;
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            serial_release(acquisition, session, 0, "serial device disconnected"); continue;
        }
        if (descriptor.revents & POLLIN) {
            uint8_t bytes[1024];
            const ssize_t count = read(session->fd, bytes, sizeof(bytes));
            if (count > 0) serial_observe(acquisition, session->settings.channel, &session->settings, "RX", bytes, (size_t)count);
            else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                serial_release(acquisition, session, 0, "serial read failed");
        }
    }
}

void edge_acquisition_enable_serial_debug(edge_acquisition *acquisition, const char *path,
                                          bool rs485, edge_acquisition_serial_callback callback) {
    if (!acquisition) return;
    copy_text(acquisition->serial_path, sizeof(acquisition->serial_path), path);
    acquisition->serial_rs485 = rs485;
    acquisition->serial_callback = callback;
    for (size_t i = 0; i < 4; ++i) acquisition->serial_sessions[i].fd = -1;
    if (path && path[0]) acquisition->worker_required = true;
}

bool edge_acquisition_serial_request(edge_acquisition *acquisition, const uint8_t platform_id[16],
                                     const iot_edge_v1_SerialDebugRequest *request) {
    if (!acquisition || acquisition->worker_fd < 0 || !platform_id || !request) return false;
    edge_acquisition_message message;
    acquisition_init_message(&message, EDGE_ACQUISITION_CONTROL_SERIAL,
                             sizeof(message.payload.serial_request));
    memcpy(message.platform_id, platform_id, 16);
    message.payload.serial_request = *request;
    const size_t size = acquisition_message_size(message.payload_size);
    return send(acquisition->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)size;
}
