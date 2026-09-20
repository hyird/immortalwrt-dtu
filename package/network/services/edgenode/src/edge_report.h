#pragma once

#include "edge.pb.h"

/* Only replaceable snapshots belong here, never telemetry, traces or ACKs. */
typedef struct {
    uint8_t *payload;
    size_t size;
    pb_size_t tag;
} edge_report_snapshot;

typedef enum {
    EDGE_REPORT_ERROR = -1,
    EDGE_REPORT_UNCHANGED = 0,
    EDGE_REPORT_SEND = 1,
} edge_report_decision;

/* Compare protobuf payloads, excluding changing Envelope IDs/timestamps.
 * The caller commits candidate only after transport acceptance. */
edge_report_decision edge_report_prepare(const edge_report_snapshot *sent,
    const iot_edge_v1_Envelope *envelope, bool force, edge_report_snapshot *candidate);
void edge_report_free(edge_report_snapshot *snapshot);
