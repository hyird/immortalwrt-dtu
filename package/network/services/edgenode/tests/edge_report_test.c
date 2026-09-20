#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge_report.h"

static iot_edge_v1_Envelope envelope;

static void require(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main(void) {
    edge_report_snapshot sent = {0}, candidate = {0}, other_platform = {0};
    envelope.which_payload = iot_edge_v1_Envelope_capability_report_tag;
    strcpy(envelope.payload.capability_report.network_stack, "netifd");
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "first snapshot must send");
    /* Transport failure must not mark a report delivered. */
    edge_report_free(&candidate);
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "failed send must remain retryable");
    sent = candidate;
    memset(&candidate, 0, sizeof(candidate));
    for (unsigned i = 0; i < 1000; ++i) {
        envelope.sequence = i;
        envelope.created_at_ms = i;
        require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_UNCHANGED,
                "envelope metadata must not retransmit an unchanged snapshot");
    }
    require(candidate.payload == NULL, "unchanged comparison must release temporary memory");
    require(edge_report_prepare(&other_platform, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "one platform cannot suppress another platform's first snapshot");
    edge_report_free(&candidate);
    require(edge_report_prepare(&sent, &envelope, true, &candidate) == EDGE_REPORT_SEND,
            "explicit recovery request must resend identical content");
    edge_report_free(&candidate);
    envelope.payload.capability_report.supports_dtu = true;
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "capability change must be immediate");
    edge_report_free(&candidate);
    envelope.payload.capability_report.interfaces_count = 17;
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_ERROR,
            "invalid encoding must never be classified as unchanged");
    edge_report_free(&candidate);
    edge_report_free(&sent);
    memset(&envelope, 0, sizeof(envelope));
    envelope.which_payload = iot_edge_v1_Envelope_device_status_report_tag;
    envelope.payload.device_status_report.devices_count = 1;
    strcpy(envelope.payload.device_status_report.devices[0].state, "online");
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "first device state");
    sent = candidate;
    memset(&candidate, 0, sizeof(candidate));
    envelope.payload.device_status_report.devices[0].last_activity_at_ms = 1;
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "real device activity is not envelope metadata");
    edge_report_free(&candidate);
    edge_report_free(&sent);
    memset(&envelope, 0, sizeof(envelope));
    envelope.which_payload = iot_edge_v1_Envelope_dtu_status_tag;
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "empty first DTU snapshot");
    sent = candidate;
    memset(&candidate, 0, sizeof(candidate));
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_UNCHANGED,
            "duplicate idle DTU state");
    envelope.payload.dtu_status.upstream_bytes = 1;
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "DTU traffic counters retain real-time changes");
    edge_report_free(&candidate);
    envelope.payload.dtu_status.traces_count = 1;
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_ERROR,
            "DTU trace events must bypass deduplication");
    const pb_size_t events[] = {iot_edge_v1_Envelope_telemetry_batch_tag,
        iot_edge_v1_Envelope_raw_packet_tag, iot_edge_v1_Envelope_command_result_tag,
        iot_edge_v1_Envelope_firmware_update_result_tag, iot_edge_v1_Envelope_heartbeat_tag,
        iot_edge_v1_Envelope_telemetry_ack_tag, iot_edge_v1_Envelope_terminal_data_tag};
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        envelope.which_payload = events[i];
        require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_ERROR,
                "event, ACK, liveness and business traffic must bypass deduplication");
    }
    edge_report_free(&sent);
    envelope.which_payload = iot_edge_v1_Envelope_device_status_report_tag;
    memset(&envelope.payload, 0, sizeof(envelope.payload));
    require(edge_report_prepare(&sent, &envelope, false, &candidate) == EDGE_REPORT_SEND,
            "reconnected session sends a fresh snapshot");
    edge_report_free(&candidate);
    puts("edge report tests passed (1000 redundant snapshots suppressed)");
    return 0;
}
