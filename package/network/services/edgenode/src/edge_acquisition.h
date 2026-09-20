#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"
#include "edge_runtime_config.h"

typedef struct edge_acquisition edge_acquisition;
typedef void (*edge_acquisition_dtu_callback)(void *context, const uint8_t platform_id[16],
    const iot_edge_v1_DtuStatus *status);
void edge_acquisition_set_dtu_callback(edge_acquisition *acquisition, edge_acquisition_dtu_callback callback);

typedef bool (*edge_acquisition_telemetry_callback)(
    void *context, const uint8_t platform_id[16],
    const iot_edge_v1_TelemetryRecord *record);
typedef void (*edge_acquisition_debug_callback)(void *context, const uint8_t platform_id[16],
    const iot_edge_v1_RawPacket *packet);
void edge_acquisition_set_debug_callback(edge_acquisition *acquisition, edge_acquisition_debug_callback callback);

typedef void (*edge_acquisition_serial_callback)(void *context, const uint8_t platform_id[16],
    const iot_edge_v1_SerialDebugEvent *event);
void edge_acquisition_enable_serial_debug(edge_acquisition *acquisition, const char *path,
    bool rs485, edge_acquisition_serial_callback callback);
bool edge_acquisition_serial_request(edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_SerialDebugRequest *request);

typedef bool (*edge_acquisition_command_callback)(
    void *context, const uint8_t platform_id[16],
    const iot_edge_v1_CommandResult *result);

typedef struct {
    const uint8_t *platform_id;
    uint16_t priority;
    bool bootstrap;
    const edge_runtime_config *config;
} edge_acquisition_source;

edge_acquisition *edge_acquisition_create(
    edge_acquisition_telemetry_callback telemetry,
    edge_acquisition_command_callback command, void *callback_context);

/*
 * Builds a replacement runtime without opening device connections.
 * Accepts Modbus RTU/TCP, S7 TCP Client and passive SL651 serial/TCP sessions.
 */
bool edge_acquisition_apply(edge_acquisition *acquisition,
                            const edge_runtime_config *config,
                            uint64_t now_ms, char *error, size_t error_size);

/*
 * Merges platform configs into one worker. Serial channels and TCP Server
 * listeners are opened once and their platform tasks are executed serially.
 */
bool edge_acquisition_apply_multi(edge_acquisition *acquisition,
                                  const edge_acquisition_source *sources,
                                  size_t source_count, uint64_t now_ms,
                                  char *error, size_t error_size);

/* Starts the supervised acquisition worker without blocking the WebSocket loop. */
bool edge_acquisition_start(edge_acquisition *acquisition,
                            char *error, size_t error_size);
void edge_acquisition_stop(edge_acquisition *acquisition);
int edge_acquisition_event_fd(const edge_acquisition *acquisition);

/* Drains worker events and restarts a failed worker; it performs no device I/O. */
void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms);

void edge_acquisition_status(edge_acquisition *acquisition,
                             iot_edge_v1_DeviceStatusReport *report);
void edge_acquisition_status_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    iot_edge_v1_DeviceStatusReport *report);

bool edge_acquisition_command(edge_acquisition *acquisition,
                              const iot_edge_v1_CommandRequest *request,
                              char *error, size_t error_size);
bool edge_acquisition_command_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_CommandRequest *request,
    char *error, size_t error_size);

size_t edge_acquisition_device_count(const edge_acquisition *acquisition);
size_t edge_acquisition_resource_count(const edge_acquisition *acquisition);

void edge_acquisition_destroy(edge_acquisition *acquisition);
