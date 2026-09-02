#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "edge_config.h"
#include "edge_memory.h"
#include "edge_retry.h"
#include "edge_runtime_config.h"

#define EDGE_STATUS_PATH "/tmp/edgenode/status.json"

typedef struct {
    const edge_platform_config *config;
    const edge_runtime_config *runtime_config;
    const edge_memory_config_set *staging_config;
    const edge_memory_outbox *outbox;
    edge_retry_phase phase;
    uint64_t last_heartbeat_ms;
    uint64_t last_inbound_ms;
    uint16_t heartbeat_interval_sec;
    bool websocket_open;
    bool enrolled;
} edge_status_platform;

typedef struct {
    const char *software_version;
    uint64_t now_ms;
    const edge_status_platform *platforms;
    size_t platform_count;
} edge_status_snapshot;

bool edge_status_render(FILE *output, const edge_status_snapshot *snapshot);
bool edge_status_write(const edge_status_snapshot *snapshot);
void edge_status_remove(void);
