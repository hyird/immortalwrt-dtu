#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

#define EDGE_LOG_RESULT_LIMIT 48U

void edge_log_init(void);

bool edge_log_set_level(const char *level);
const char *edge_log_level(void);
bool edge_log_enabled(const char *level);

void edge_log_write(const char *level, const char *source, const char *message,
                    const char *detail);

/* Bounded, local-only acquisition diagnostics. These records are not returned
 * by edge_log_query and contain no endpoint, credentials or packet contents. */
typedef enum {
  EDGE_LOCAL_REPORT_QUEUED = 0,
  EDGE_LOCAL_REPORT_FRAME_LIMIT = 1,
  EDGE_LOCAL_REPORT_ALLOCATION = 2,
  EDGE_LOCAL_REPORT_EMPTY = 3,
  EDGE_LOCAL_REPORT_ENQUEUE = 4,
  EDGE_LOCAL_REPORT_INVALID = 5,
} edge_local_report_result;

typedef enum {
  EDGE_LOCAL_FAULT_SAMPLE_INCOMPLETE = 1,
  EDGE_LOCAL_FAULT_SL651_FRAME,
  EDGE_LOCAL_FAULT_SL651_BUFFER,
  EDGE_LOCAL_FAULT_SL651_REPORT,
  EDGE_LOCAL_FAULT_WORKER_ENCODE,
  EDGE_LOCAL_FAULT_WORKER_SEND,
  EDGE_LOCAL_FAULT_PARENT_DECODE,
  EDGE_LOCAL_FAULT_PARENT_OUTBOX,
  EDGE_LOCAL_FAULT_OUTBOX_CORRUPT,
  EDGE_LOCAL_FAULT_OUTBOX_SEND,
  EDGE_LOCAL_FAULT_COMMAND,
  EDGE_LOCAL_FAULT_NETWORK,
  EDGE_LOCAL_FAULT_CONFIG,
  EDGE_LOCAL_FAULT_OUTBOX_EVICT,
  EDGE_LOCAL_FAULT_PROTOCOL_DECODE,
  EDGE_LOCAL_FAULT_CAPTURE,
  EDGE_LOCAL_FAULT_DTU,
  EDGE_LOCAL_FAULT_VPN,
  EDGE_LOCAL_FAULT_FIRMWARE,
  EDGE_LOCAL_FAULT_MODEM,
  EDGE_LOCAL_FAULT_NETWORK_CONFIG,
} edge_local_fault;

void edge_log_local_io(const uint8_t platform_id[16], const uint8_t device_id[16],
                       unsigned protocol, const char *operation, unsigned result);
void edge_log_local_report(const uint8_t platform_id[16], const uint8_t device_id[16],
                           unsigned protocol, edge_local_report_result result,
                           unsigned points, unsigned frames);
void edge_log_local_config(const uint8_t platform_id[16], const uint8_t device_id[16],
                           unsigned protocol, unsigned transport,
                           unsigned scan_ms, unsigned report_sec);
void edge_log_local_fault(const uint8_t platform_id[16], const uint8_t device_id[16],
                          unsigned protocol, edge_local_fault fault, unsigned reason);

void edge_log_packet(const char *source, const char *direction, const char *device,
                     const uint8_t *data, size_t size);

void edge_log_query(const iot_edge_v1_LogRequest *request,
                    iot_edge_v1_LogResult *result);
