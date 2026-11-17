#pragma once

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
#include "edge_util.h"
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

#ifndef EDGE_GATEWAY_MAP_PATH
#define EDGE_GATEWAY_MAP_PATH "/etc/edgenode-modbus.map"
#endif

static inline int64_t now_ms(void) {
    return edge_realtime_ms();
}

static inline uint64_t monotonic_ms(void) {
    return edge_monotonic_ms();
}

static inline bool random_bytes(uint8_t *output, size_t size) {
    return edge_random_bytes(output, size);
}

static inline void safe_copy(char *output, size_t capacity, const char *input) {
    edge_copy_text(output, capacity, input);
}

static inline edge_ws_session *session_from_reconnect(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reconnect_timer));
}

static inline edge_ws_session *session_from_liveness(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, liveness_timer));
}

static inline edge_ws_session *session_from_status_report(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, status_report_timer));
}

static inline edge_ws_session *session_from_firmware(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, firmware_timer));
}

static inline edge_ws_session *session_from_modem(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, modem_timer));
}

static inline edge_ws_session *session_from_modem_io(struct ev_io *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, modem_io));
}

static inline edge_ws_session *session_from_modem_child(struct ev_child *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, modem_child));
}

static inline edge_ws_session *session_from_network(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, network_timer));
}

static inline edge_ws_session *session_from_reload(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reload_timer));
}

static inline edge_ws_session *session_from_terminal(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, terminal_timer));
}

static inline edge_ws_app *app_from_acquisition(struct ev_timer *timer) {
    return (edge_ws_app *)((uint8_t *)timer - offsetof(edge_ws_app, acquisition_timer));
}

static inline edge_ws_app *app_from_status(struct ev_timer *timer) {
    return (edge_ws_app *)((uint8_t *)timer - offsetof(edge_ws_app, status_timer));
}

static inline edge_ws_app *app_from_acquisition_io(struct ev_io *watcher) {
    return (edge_ws_app *)((uint8_t *)watcher - offsetof(edge_ws_app, acquisition_io));
}

edge_ws_session *edge_ws_session_for_platform(edge_ws_app *app,
                                              const uint8_t platform_id[16]);
void edge_ws_log_uuid_task(const edge_ws_session *session, const char *type,
                           const uint8_t id[16], const char *state,
                           const char *summary);
bool edge_ws_init_envelope(edge_ws_session *session,
                           iot_edge_v1_Envelope *envelope);
bool edge_ws_send_envelope(edge_ws_session *session,
                           iot_edge_v1_Envelope *envelope);
size_t edge_ws_build_acquisition_sources(
    edge_ws_app *app, const edge_ws_session *replacement_session,
    const edge_runtime_config *replacement_config,
    edge_acquisition_source sources[EDGE_MAX_PLATFORMS]);

void edge_ws_export_gateway(edge_ws_app *app);
void edge_ws_acquisition_sample(void *context, const uint8_t platform[16],
                                const edge_acquisition_sample *sample);
edge_modbus_gateway *edge_ws_prepare_gateway(
    const edge_acquisition_source *sources, size_t count, uint16_t port);
uint8_t edge_ws_gateway_write(void *context,
                              const edge_modbus_gateway_write *write,
                              uint8_t command_id[16], uint64_t deadline_ms);
bool edge_ws_consume_gateway_result(edge_ws_app *app,
                                    const uint8_t platform_id[16],
                                    const iot_edge_v1_CommandResult *result);
void edge_ws_gateway_commands_stopped(edge_ws_app *app);
void edge_ws_gateway_network_changed(void *context, bool available);
void edge_ws_handle_gateway_request(edge_ws_session *session,
                                    const iot_edge_v1_Envelope *input);

#define export_gateway edge_ws_export_gateway
#define acquisition_sample edge_ws_acquisition_sample
#define prepare_gateway edge_ws_prepare_gateway
#define gateway_write edge_ws_gateway_write
#define consume_gateway_result edge_ws_consume_gateway_result
#define gateway_commands_stopped edge_ws_gateway_commands_stopped
#define gateway_network_changed edge_ws_gateway_network_changed
#define handle_gateway_request edge_ws_handle_gateway_request

void edge_ws_handle_modem_control(edge_ws_session *session,
                                  const iot_edge_v1_ModemControlRequest *request);
void edge_ws_send_pending_modem_result(edge_ws_session *session);
void edge_ws_modem_io(struct ev_loop *loop, struct ev_io *watcher, int events);
void edge_ws_modem_child(struct ev_loop *loop, struct ev_child *watcher, int events);
void edge_ws_modem_timer(struct ev_loop *loop, struct ev_timer *timer, int events);

#define handle_modem_control edge_ws_handle_modem_control
#define send_pending_modem_result edge_ws_send_pending_modem_result

void edge_ws_reset_terminal_flow(edge_ws_session *session);
void edge_ws_fail_terminal(edge_ws_session *session, const char *reason);
void edge_ws_handle_terminal_open(edge_ws_session *session,
                                  const iot_edge_v1_TerminalOpen *request);
void edge_ws_handle_terminal_data(edge_ws_session *session,
                                  const iot_edge_v1_TerminalData *data);
void edge_ws_handle_terminal_data_ack(edge_ws_session *session,
                                      const iot_edge_v1_TerminalDataAck *ack);
void edge_ws_handle_terminal_resize(edge_ws_session *session,
                                    const iot_edge_v1_TerminalResize *resize);
void edge_ws_handle_terminal_close(edge_ws_session *session,
                                   const iot_edge_v1_TerminalClose *close_msg);
void edge_ws_terminal_timer(struct ev_loop *loop, struct ev_timer *timer,
                            int events);

#define reset_terminal_flow edge_ws_reset_terminal_flow
#define fail_terminal edge_ws_fail_terminal
#define handle_terminal_open edge_ws_handle_terminal_open
#define handle_terminal_data edge_ws_handle_terminal_data
#define handle_terminal_data_ack edge_ws_handle_terminal_data_ack
#define handle_terminal_resize edge_ws_handle_terminal_resize
#define handle_terminal_close edge_ws_handle_terminal_close
