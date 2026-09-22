#include "edge_ws_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    edge_ws_transport transport;
    struct ev_loop *loop;
    unsigned char payload[4096];
    unsigned received;
    bool failed;
} fixture;

static void opened(void *user) {
    fixture *state = user;
    if (!edge_ws_transport_send(&state->transport, "ack", 3) ||
        !edge_ws_transport_send(&state->transport, state->payload, sizeof(state->payload)) ||
        !edge_ws_transport_send(&state->transport, state->payload, sizeof(state->payload))) {
        state->failed = true;
        ev_break(state->loop, EVBREAK_ALL);
    }
}

static void received(void *user, void *data, size_t size, bool binary) {
    fixture *state = user;
    const void *expected = state->received ? (const void *)state->payload : (const void *)"ack";
    size_t expected_size = state->received ? sizeof(state->payload) : 3;
    if (!binary || size != expected_size || memcmp(data, expected, size)) {
        fprintf(stderr, "receive mismatch index=%u binary=%d size=%zu expected=%zu\n",
                state->received, binary, size, expected_size);
        state->failed = true;
        ev_break(state->loop, EVBREAK_ALL);
        return;
    }
    if (++state->received == 3)
        edge_ws_transport_close(&state->transport, 1000, "verified");
}

static void closed(void *user, int code, const char *reason) {
    fixture *state = user;
    if (state->received != 3) {
        fprintf(stderr, "premature close: %d %s (%u messages)\n", code, reason, state->received);
        state->failed = true;
    }
    ev_break(state->loop, EVBREAK_ALL);
}

static void timed_out(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)events;
    fixture *state = timer->data;
    state->failed = true;
    fprintf(stderr, "transport timeout\n");
    ev_break(loop, EVBREAK_ALL);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (getenv("EDGE_WS_TEST_LOG")) lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG, NULL);
    fixture state = {0};
    state.loop = ev_loop_new(0);
    if (!state.loop) return 2;
    uint32_t random = 12345;
    for (size_t index = 0; index < sizeof(state.payload); ++index) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        state.payload[index] = (unsigned char)random;
    }
    struct ev_timer deadline;
    ev_timer_init(&deadline, timed_out, 10, 0);
    deadline.data = &state;
    ev_timer_start(state.loop, &deadline);
    if (!edge_ws_transport_connect(&state.transport, state.loop, argv[1], 16384,
                                   &state, opened, received, closed)) state.failed = true;
    else ev_run(state.loop, 0);
    edge_ws_transport_destroy(&state.transport);
    /* LWS 必须不销毁调用方持有的 loop。 */
    ev_timer_stop(state.loop, &deadline);
    ev_run(state.loop, EVRUN_NOWAIT);
    ev_loop_destroy(state.loop);
    return state.failed || state.received != 3 ? 1 : 0;
}
