#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"
#include "edge_runtime_config.h"
#include "edge_external_io.h"

typedef struct edge_acquisition edge_acquisition;
/* Pointer-free local sample; independent of platform telemetry negotiation. */
typedef struct {
    uint8_t device_id[16];
    char element_id[65];
    iot_edge_v1_ScalarValue value;
    bool valid;
    int64_t observed_at_ms;
} edge_acquisition_sample;
typedef void (*edge_acquisition_sample_callback)(void *context,
    const uint8_t platform_id[16], const edge_acquisition_sample *sample);
/* NULL sample invalidates that platform's cache when the worker exits. */
bool edge_acquisition_set_gateway_enabled(edge_acquisition *acquisition, bool enabled);
void edge_acquisition_set_sample_callback(edge_acquisition *acquisition,
    edge_acquisition_sample_callback callback);
typedef void (*edge_acquisition_dtu_callback)(void *context, const uint8_t platform_id[16],
    const iot_edge_v1_DtuStatus *status);
void edge_acquisition_set_dtu_callback(edge_acquisition *acquisition, edge_acquisition_dtu_callback callback);
typedef void (*edge_acquisition_io_callback)(void *context, const edge_external_io_snapshot *snapshot);
void edge_acquisition_set_io_callback(edge_acquisition *acquisition, edge_acquisition_io_callback callback);
const edge_external_io_snapshot *edge_acquisition_io_capability(const edge_acquisition *acquisition);

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
    bool raw_telemetry;
    bool external_io;
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
/* Pause new IO work and wait until pending IO records and results are queued. */
bool edge_acquisition_prepare_io_swap(edge_acquisition *acquisition);
void edge_acquisition_stop(edge_acquisition *acquisition);
int edge_acquisition_event_fd(const edge_acquisition *acquisition);
bool edge_acquisition_set_raw_telemetry(edge_acquisition *acquisition,
                                       const uint8_t platform_id[16], bool enabled);
bool edge_acquisition_set_external_io(edge_acquisition *acquisition,
                                      const uint8_t platform_id[16], bool enabled);

/* Drains worker events and restarts a failed worker; it performs no device I/O. */
void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms);

void edge_acquisition_status(edge_acquisition *acquisition,
                             iot_edge_v1_DeviceStatusReport *report);
void edge_acquisition_status_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    iot_edge_v1_DeviceStatusReport *report);

bool edge_acquisition_command_for_platform_until(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_CommandRequest *request,
    uint64_t start_before_monotonic_ms, uint64_t mapping_valid_until_monotonic_ms,
    char *error, size_t error_size);
void edge_acquisition_invalidate_command_clock(
    edge_acquisition *acquisition, const uint8_t platform_id[16]);

size_t edge_acquisition_device_count(const edge_acquisition *acquisition);
size_t edge_acquisition_resource_count(const edge_acquisition *acquisition);

void edge_acquisition_destroy(edge_acquisition *acquisition);
