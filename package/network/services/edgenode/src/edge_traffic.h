#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edge.pb.h"

#define EDGE_TRAFFIC_MAX_FLOWS 128U
#define EDGE_TRAFFIC_MAX_PLATFORMS 16U

typedef struct {
    uint64_t upload, download;
    uint64_t acknowledged_upload, acknowledged_download;
    uint64_t sampled_upload, sampled_download;
    uint64_t acknowledged_at_ms, sampled_at_ms;
    uint64_t sequence;
    bool complete, pending;
    iot_edge_v1_TcpTraffic report;
} edge_traffic_window;

typedef struct {
    bool used, bound, seen;
    uint8_t family, source[16], destination[16];
    uint16_t source_port, destination_port;
    uint32_t id;
    size_t platform;
    uint64_t upload, download;
} edge_traffic_flow;

typedef struct {
    int event_fd, query_fd;
    uint32_t query_sequence;
    size_t platform_count;
    edge_traffic_window windows[EDGE_TRAFFIC_MAX_PLATFORMS];
    edge_traffic_flow flows[EDGE_TRAFFIC_MAX_FLOWS];
} edge_traffic;

/* Linux conntrack IP-byte accounting, not payload sizing or SIM billing. */
bool edge_traffic_open(edge_traffic *traffic, size_t platforms, uint64_t now_ms);
void edge_traffic_close(edge_traffic *traffic);
void edge_traffic_register(edge_traffic *traffic, size_t platform, int socket_fd);
void edge_traffic_drain(edge_traffic *traffic);
void edge_traffic_sample(edge_traffic *traffic, size_t platform, uint64_t now_ms,
                         iot_edge_v1_TcpTraffic *report);
void edge_traffic_ack(edge_traffic *traffic, size_t platform, uint64_t sample_id);
/* Shared by the netlink receive path and malformed/event-order tests. */
bool edge_traffic_ingest(edge_traffic *traffic, const void *message, size_t size);
