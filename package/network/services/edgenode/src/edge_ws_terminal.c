#include "edge_ws_internal.h"

void edge_ws_reset_terminal_flow(edge_ws_session *session) {
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

static bool send_terminal_opened(edge_ws_session *session,
                                 const uint8_t terminal_id[16]) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!edge_ws_init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_opened_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_opened.terminal_id,
               sizeof(output->payload.terminal_opened.terminal_id.bytes),
               terminal_id, 16U) &&
           edge_ws_send_envelope(session, output);
}

static void send_terminal_close(edge_ws_session *session, const uint8_t terminal_id[16],
                                int32_t exit_code, const char *reason) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!edge_ws_init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_terminal_close_tag;
    edge_protocol_set_bytes(&output->payload.terminal_close.terminal_id,
                            sizeof(output->payload.terminal_close.terminal_id.bytes),
                            terminal_id, 16U);
    output->payload.terminal_close.exit_code = exit_code;
    safe_copy(output->payload.terminal_close.reason,
              sizeof(output->payload.terminal_close.reason), reason);
    edge_ws_send_envelope(session, output);
}

static bool send_terminal_data_ack(edge_ws_session *session,
                                   const uint8_t terminal_id[16],
                                   uint64_t sequence) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!edge_ws_init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_data_ack_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_data_ack.terminal_id,
               sizeof(output->payload.terminal_data_ack.terminal_id.bytes),
               terminal_id, 16U) &&
           (output->payload.terminal_data_ack.sequence = sequence) != 0U &&
           edge_ws_send_envelope(session, output);
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
    if (!edge_ws_init_envelope(session, output))
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
           edge_ws_send_envelope(session, output);
}

void edge_ws_fail_terminal(edge_ws_session *session, const char *reason) {
    uint8_t terminal_id[16];
    memcpy(terminal_id, session->terminal_id, sizeof(terminal_id));
    edge_terminal_close(terminal_id);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    session->terminal_open = false;
    send_terminal_close(session, terminal_id, -1, reason);
    edge_ws_reset_terminal_flow(session);
}

void edge_ws_handle_terminal_open(edge_ws_session *session,
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
    edge_ws_reset_terminal_flow(session);
    memcpy(session->terminal_id, terminal_id, sizeof(session->terminal_id));
    session->terminal_open = true;
    if (!send_terminal_opened(session, terminal_id)) {
        edge_terminal_close(terminal_id);
        session->terminal_open = false;
        edge_ws_reset_terminal_flow(session);
        return;
    }
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    ev_timer_set(&session->terminal_timer, EDGE_TERMINAL_OUTPUT_POLL_INTERVAL,
                 EDGE_TERMINAL_OUTPUT_POLL_INTERVAL);
    ev_timer_start(session->app->loop, &session->terminal_timer);
}

void edge_ws_handle_terminal_data(edge_ws_session *session,
                                  const iot_edge_v1_TerminalData *data) {
    if (!session->terminal_open)
        return;
    if (data->terminal_id.size != 16U ||
        memcmp(data->terminal_id.bytes, session->terminal_id,
               sizeof(session->terminal_id)) != 0) {
        edge_ws_fail_terminal(session, "terminal input identity mismatch");
        return;
    }
    uint64_t acked_sequence = 0U;
    const edge_terminal_input_result result =
        edge_terminal_write(data, &acked_sequence);
    if (result == EDGE_TERMINAL_INPUT_ACKED)
        stage_terminal_input_ack(session, acked_sequence);
    else if (result == EDGE_TERMINAL_INPUT_ERROR)
        edge_ws_fail_terminal(session, "terminal input sequence or write failed");
}

void edge_ws_handle_terminal_data_ack(edge_ws_session *session,
                                      const iot_edge_v1_TerminalDataAck *ack) {
    if (!session->terminal_open)
        return;
    if (ack->terminal_id.size != 16U || ack->sequence == 0U ||
        memcmp(ack->terminal_id.bytes, session->terminal_id,
               sizeof(session->terminal_id)) != 0 ||
        (!session->terminal_output_pending &&
         ack->sequence != session->terminal_output_acked_sequence) ||
        (session->terminal_output_pending &&
         ack->sequence != session->terminal_output_sequence)) {
        edge_ws_fail_terminal(session, "terminal output acknowledgement mismatch");
    } else if (session->terminal_output_pending) {
        session->terminal_output_acked_sequence = ack->sequence;
        session->terminal_output_size = 0U;
        session->terminal_output_deadline_ms = 0U;
        session->terminal_output_pending = false;
        session->terminal_output_sent = false;
    }
}

void edge_ws_handle_terminal_resize(edge_ws_session *session,
                                    const iot_edge_v1_TerminalResize *resize) {
    if (!session->terminal_open)
        return;
    if (resize->terminal_id.size != 16U ||
        memcmp(resize->terminal_id.bytes, session->terminal_id,
               sizeof(session->terminal_id)) != 0)
        edge_ws_fail_terminal(session, "terminal resize identity mismatch");
    else if (!edge_terminal_resize(resize))
        edge_ws_fail_terminal(session, "terminal resize failed");
}

void edge_ws_handle_terminal_close(edge_ws_session *session,
                                   const iot_edge_v1_TerminalClose *close_msg) {
    if (!session->terminal_open)
        return;
    if (close_msg->terminal_id.size != 16U ||
        memcmp(close_msg->terminal_id.bytes, session->terminal_id,
               sizeof(session->terminal_id)) != 0) {
        edge_ws_fail_terminal(session, "terminal close identity mismatch");
    } else {
        edge_terminal_close(session->terminal_id);
        ev_timer_stop(session->app->loop, &session->terminal_timer);
        session->terminal_open = false;
        edge_ws_reset_terminal_flow(session);
    }
}

void edge_ws_terminal_timer(struct ev_loop *loop, struct ev_timer *timer,
                            int events) {
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
        edge_ws_fail_terminal(session, "terminal input write failed");
        return;
    }
    if (input_result == EDGE_TERMINAL_INPUT_ACKED)
        stage_terminal_input_ack(session, input_acked_sequence);

    const uint64_t now = monotonic_ms();
    if (session->terminal_output_pending) {
        if (now >= session->terminal_output_deadline_ms) {
            edge_ws_fail_terminal(session, "terminal output acknowledgement timed out");
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
            edge_ws_fail_terminal(session, "terminal output sequence exhausted");
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
        edge_ws_reset_terminal_flow(session);
    }
}
