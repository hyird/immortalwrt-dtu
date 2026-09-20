#include "edge_firmware_stream.h"

bool edge_firmware_stream_request_due(uint64_t now_ms, uint64_t last_request_ms,
    uint32_t retry_interval_ms, bool advanced) {
    return advanced || last_request_ms == 0U ||
        (now_ms >= last_request_ms && now_ms - last_request_ms >= retry_interval_ms);
}

edge_firmware_stream_decision edge_firmware_stream_evaluate(
    uint64_t expected_offset, uint64_t total_size, uint64_t chunk_offset,
    size_t chunk_size, size_t maximum_chunk_size, bool eof) {
    if (total_size == 0U || expected_offset > total_size || chunk_size == 0U ||
        chunk_size > maximum_chunk_size || chunk_offset > total_size ||
        (uint64_t)chunk_size > total_size - chunk_offset)
        return EDGE_FIRMWARE_STREAM_INVALID;
    if (chunk_offset < expected_offset)
        return (uint64_t)chunk_size <= expected_offset - chunk_offset
                   ? EDGE_FIRMWARE_STREAM_DUPLICATE
                   : EDGE_FIRMWARE_STREAM_INVALID;
    if (chunk_offset > expected_offset)
        return EDGE_FIRMWARE_STREAM_GAP;
    const bool completes = chunk_offset + (uint64_t)chunk_size == total_size;
    if (eof != completes)
        return EDGE_FIRMWARE_STREAM_INVALID;
    return completes ? EDGE_FIRMWARE_STREAM_COMPLETE
                     : EDGE_FIRMWARE_STREAM_ACCEPT;
}
