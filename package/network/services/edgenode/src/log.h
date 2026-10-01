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
} edge_local_report_result;

void edge_log_local_io(const uint8_t platform_id[16], const uint8_t device_id[16],
                       const char *operation, unsigned result);
void edge_log_local_report(const uint8_t platform_id[16], const uint8_t device_id[16],
                           edge_local_report_result result, unsigned points,
                           unsigned frames);
void edge_log_local_config(const uint8_t platform_id[16], const uint8_t device_id[16],
                           unsigned scan_ms, unsigned report_sec);

void edge_log_packet(const char *source, const char *direction, const char *device,
                     const uint8_t *data, size_t size);

void edge_log_query(const iot_edge_v1_LogRequest *request,
                    iot_edge_v1_LogResult *result);
