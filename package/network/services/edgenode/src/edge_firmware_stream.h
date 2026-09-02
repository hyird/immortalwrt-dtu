#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EDGE_FIRMWARE_STREAM_INVALID = 0,
    EDGE_FIRMWARE_STREAM_ACCEPT = 1,
    EDGE_FIRMWARE_STREAM_COMPLETE = 2,
    EDGE_FIRMWARE_STREAM_DUPLICATE = 3,
    EDGE_FIRMWARE_STREAM_GAP = 4,
} edge_firmware_stream_decision;

edge_firmware_stream_decision edge_firmware_stream_evaluate(
    uint64_t expected_offset, uint64_t total_size, uint64_t chunk_offset,
    size_t chunk_size, size_t maximum_chunk_size, bool eof);
