#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

bool edge_firmware_start(const uint8_t platform_id[16],
                         const iot_edge_v1_FirmwareUpdateRequest *request,
                         char *error, size_t error_size);

typedef enum {
    EDGE_FIRMWARE_CHUNK_FAILED = 0,
    EDGE_FIRMWARE_CHUNK_NEXT = 1,
    EDGE_FIRMWARE_CHUNK_COMPLETE = 2,
} edge_firmware_chunk_result;

edge_firmware_chunk_result edge_firmware_receive_chunk(
    const uint8_t platform_id[16], const iot_edge_v1_FirmwareChunk *chunk,
    char *error, size_t error_size);

/* Builds the next pull request. Non-forced retries are rate-limited. */
bool edge_firmware_chunk_request(const uint8_t platform_id[16],
                                 iot_edge_v1_FirmwareChunkRequest *request,
                                 bool force);
bool edge_firmware_receiving(const uint8_t platform_id[16]);

/* Returns and removes the latest child-process status for this platform. */
bool edge_firmware_read_status(const uint8_t platform_id[16],
                               iot_edge_v1_FirmwareUpdateResult *result);
bool edge_firmware_active(void);
bool edge_firmware_has_status(const uint8_t platform_id[16]);
