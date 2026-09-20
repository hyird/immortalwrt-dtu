#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

#include "edge_firmware_stream.h"

int main(void) {
    assert(edge_firmware_stream_request_due(1000, 0, 3000, false));
    for (uint64_t now = 1000; now < 4000; ++now)
        assert(!edge_firmware_stream_request_due(now, 1000, 3000, false));
    assert(edge_firmware_stream_request_due(4000, 1000, 3000, false));
    assert(edge_firmware_stream_request_due(1001, 1000, 3000, true));
    assert(!edge_firmware_stream_request_due(999, 1000, 3000, false));
    assert(edge_firmware_stream_evaluate(0U, 10U, 0U, 4U, 8U, false) ==
           EDGE_FIRMWARE_STREAM_ACCEPT);
    assert(edge_firmware_stream_evaluate(4U, 10U, 4U, 6U, 8U, true) ==
           EDGE_FIRMWARE_STREAM_COMPLETE);
    assert(edge_firmware_stream_evaluate(4U, 10U, 0U, 4U, 8U, false) ==
           EDGE_FIRMWARE_STREAM_DUPLICATE);
    assert(edge_firmware_stream_evaluate(4U, 10U, 6U, 2U, 8U, false) ==
           EDGE_FIRMWARE_STREAM_GAP);
    assert(edge_firmware_stream_evaluate(4U, 10U, 3U, 2U, 8U, false) ==
           EDGE_FIRMWARE_STREAM_INVALID);
    assert(edge_firmware_stream_evaluate(0U, 10U, 0U, 10U, 8U, true) ==
           EDGE_FIRMWARE_STREAM_INVALID);
    assert(edge_firmware_stream_evaluate(0U, 10U, 0U, 4U, 8U, true) ==
           EDGE_FIRMWARE_STREAM_INVALID);
    puts("edge firmware stream tests passed");
    return 0;
}
