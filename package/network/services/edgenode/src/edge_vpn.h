#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edge_vpn_plan.h"

typedef struct {
    bool primed, pending;
    uint64_t raw_rx, raw_tx, upload, download, ack_upload, ack_download, ack_ms, sequence;
    uint64_t pending_upload, pending_download;
    iot_edge_v1_TcpTraffic report;
} edge_vpn_traffic;

typedef struct {
    edge_vpn_plan plan;
    uint64_t applied_version;
    bool enabled;
    edge_vpn_traffic traffic;
} edge_vpn_session;

bool edge_vpn_init(edge_vpn_session *session, const uint8_t platform_id[16], size_t slot);
bool edge_vpn_recover_resources(void);
bool edge_vpn_collect_capability(edge_vpn_session *session, iot_edge_v1_VpnCapabilities *capability);
bool edge_vpn_apply(edge_vpn_session *session, const iot_edge_v1_VpnConfigRequest *request,
                    char *error, size_t error_size);
void edge_vpn_shutdown(edge_vpn_session *session);
bool edge_vpn_sample(edge_vpn_session *session, uint64_t now_ms, iot_edge_v1_TcpTraffic *report);
void edge_vpn_ack(edge_vpn_session *session, uint64_t sample_id);
