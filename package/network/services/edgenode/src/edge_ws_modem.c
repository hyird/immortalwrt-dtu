#include "edge_ws_internal.h"

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

static bool send_modem_result(edge_ws_session *session, const uint8_t request_id[16],
                              iot_edge_v1_ModemControlAction action,
                              iot_edge_v1_ModemControlState state,
                              const char *message, const char *apn) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!edge_ws_init_envelope(session, output))
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
    const bool sent = edge_ws_send_envelope(session, output);
    if (state == iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_MODEM, (unsigned)action);
    if (!sent)
        edge_log_local_fault(session->config->id, NULL, 0U,
                             EDGE_LOCAL_FAULT_MODEM, 100U);
    char summary[160];
    snprintf(summary, sizeof(summary), "action=%d message=%s",
             (int)action, message != NULL ? message : "");
    edge_ws_log_uuid_task(session, "modem-control", request_id,
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

void edge_ws_handle_modem_control(edge_ws_session *session,
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

void edge_ws_send_pending_modem_result(edge_ws_session *session) {
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

void edge_ws_modem_io(struct ev_loop *loop, struct ev_io *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem_io(watcher);
    read_modem_worker_result(session);
    edge_ws_send_pending_modem_result(session);
}

void edge_ws_modem_child(struct ev_loop *loop, struct ev_child *watcher, int events) {
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
    edge_ws_send_pending_modem_result(session);
}

void edge_ws_modem_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
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
