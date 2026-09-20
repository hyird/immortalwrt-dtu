#include "edge_report.h"

#include <stdlib.h>
#include <string.h>
#include <pb_encode.h>

void edge_report_free(edge_report_snapshot *snapshot) {
    if (snapshot == NULL) return;
    free(snapshot->payload);
    memset(snapshot, 0, sizeof(*snapshot));
}

edge_report_decision edge_report_prepare(const edge_report_snapshot *sent,
    const iot_edge_v1_Envelope *envelope, bool force, edge_report_snapshot *candidate) {
    if (sent == NULL || envelope == NULL || candidate == NULL || candidate == sent)
        return EDGE_REPORT_ERROR;
    memset(candidate, 0, sizeof(*candidate));
    const pb_msgdesc_t *fields;
    const void *payload;
    switch (envelope->which_payload) {
    case iot_edge_v1_Envelope_capability_report_tag:
        fields = iot_edge_v1_CapabilityReport_fields;
        payload = &envelope->payload.capability_report;
        break;
    case iot_edge_v1_Envelope_device_status_report_tag:
        fields = iot_edge_v1_DeviceStatusReport_fields;
        payload = &envelope->payload.device_status_report;
        break;
    case iot_edge_v1_Envelope_dtu_status_tag:
        /* Trace batches are events, not replaceable snapshots. */
        if (envelope->payload.dtu_status.traces_count != 0)
            return EDGE_REPORT_ERROR;
        fields = iot_edge_v1_DtuStatus_fields;
        payload = &envelope->payload.dtu_status;
        break;
    default:
        return EDGE_REPORT_ERROR;
    }
    if (!pb_get_encoded_size(&candidate->size, fields, payload))
        return EDGE_REPORT_ERROR;
    candidate->payload = malloc(candidate->size != 0 ? candidate->size : 1);
    if (candidate->payload == NULL) return EDGE_REPORT_ERROR;
    pb_ostream_t stream = pb_ostream_from_buffer(candidate->payload, candidate->size);
    if (!pb_encode(&stream, fields, payload)) {
        edge_report_free(candidate);
        return EDGE_REPORT_ERROR;
    }
    candidate->tag = envelope->which_payload;
    if (!force && sent->payload != NULL && sent->tag == candidate->tag &&
        sent->size == candidate->size &&
        memcmp(sent->payload, candidate->payload, candidate->size) == 0) {
        edge_report_free(candidate);
        return EDGE_REPORT_UNCHANGED;
    }
    return EDGE_REPORT_SEND;
}
