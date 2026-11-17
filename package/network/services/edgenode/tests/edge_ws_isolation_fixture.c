/* Include the transport to inject a blocked synchronous connect and inspect
 * worker lifetime without exposing test controls in the production API. */
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <libwebsockets.h>
static struct lws *fixture_connect(const struct lws_client_connect_info *info);
#define lws_client_connect_via_info fixture_connect
#include "../src/edge_ws_transport.c"
#undef lws_client_connect_via_info


static struct lws *fixture_connect(const struct lws_client_connect_info *info) {
    if (!strcmp(info->address, "stall.invalid"))
        for (;;) pause();
    return lws_client_connect_via_info(info);
}

typedef struct {
    edge_ws_transport good;
    edge_ws_transport bad;
    struct ev_loop *loop;
    const char *mode;
    unsigned sent;
    unsigned received;
    unsigned bad_received;
    unsigned ticks;
    bool fault_injected;
    bool bad_closed;
    bool done;
    unsigned target;
    ev_tstamp last_reply;
    ev_tstamp max_gap;
} fixture;

static void good_opened(void *user) {
    fixture *state = user;
    const int fd = edge_ws_transport_socket_fd(&state->good);
    struct sockaddr_storage peer;
    socklen_t size = sizeof(peer);
    assert(fd >= 0 && getpeername(fd, (struct sockaddr *)&peer, &size) == 0);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
}
static void good_received(void *user, void *data, size_t size, bool binary) {
    fixture *state = user;
    assert(binary && size == sizeof(unsigned));
    unsigned sequence;
    memcpy(&sequence, data, size);
    assert(sequence == state->received);
    ++state->received;
    const ev_tstamp now = ev_now(state->loop);
    if (state->last_reply && now - state->last_reply > state->max_gap)
        state->max_gap = now - state->last_reply;
    state->last_reply = now;
}
static void good_closed(void *user, int code, const char *reason) {
    (void)user;
    fprintf(stderr, "healthy platform closed: %d %s\n", code, reason);
    abort();
}
static void bad_opened(void *user) {
    fixture *state = user;
    if (!strcmp(state->mode, "crash")) {
        assert(kill(state->bad.worker->pid, SIGKILL) == 0);
        state->fault_injected = true;
    } else if (!strcmp(state->mode, "freeze") || !strcmp(state->mode, "watchdog")) {
        assert(kill(state->bad.worker->pid, SIGSTOP) == 0);
        state->fault_injected = true;
    } else if (!strcmp(state->mode, "queue")) {
        assert(kill(state->bad.worker->pid, SIGSTOP) == 0);
        /* Tiny packets must hit the packet bound, not allocate 256K nodes. */
        unsigned count = 0;
        while (edge_ws_transport_send(&state->bad, "x", 1)) ++count;
        assert(count == EDGE_WS_QUEUE_PACKETS);
        state->fault_injected = true;
    } else if (!strcmp(state->mode, "destroy")) {
        /* Exercise reentrant teardown from inside an application callback. */
        edge_ws_transport_destroy(&state->bad);
        state->fault_injected = true;
    }
}
static void bad_received(void *user, void *data, size_t size, bool binary) {
    fixture *state = user;
    (void)data; (void)size; (void)binary;
    ++state->bad_received;
}
static void bad_closed(void *user, int code, const char *reason) {
    fixture *state = user;
    (void)code; (void)reason;
    state->bad_closed = true;
    edge_ws_transport_destroy(&state->bad);
}

static void tick(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)events;
    fixture *state = timer->data;
    ++state->ticks;
    assert(edge_ws_transport_socket_fd(&state->good) == -1);
    assert(state->ticks < 180);
    if (state->good.worker && state->good.worker->connected && state->sent < state->target) {
        assert(edge_ws_transport_send(&state->good, &state->sent, sizeof(state->sent)));
        ++state->sent;
    }
    if (state->received != state->target) return;
    assert(state->max_gap < 0.5);
    if (!strcmp(state->mode, "crash") || !strcmp(state->mode, "flood") ||
        !strcmp(state->mode, "oversize") || !strcmp(state->mode, "watchdog"))
        assert(state->bad_closed);
    if (!strcmp(state->mode, "stall")) assert(!state->bad_closed);
    if (!strcmp(state->mode, "freeze") || !strcmp(state->mode, "queue") ||
        !strcmp(state->mode, "destroy")) assert(state->fault_injected);
    state->done = true;
    ev_break(loop, EVBREAK_ALL);
}

static void child_reaped(struct ev_loop *loop, struct ev_child *watcher, int events) {
    (void)loop; (void)watcher; (void)events;
}

int main(int argc, char **argv) {
    assert(argc == 4);
    fixture state = {.loop = ev_default_loop(0), .mode = argv[3],
                     .target = !strcmp(argv[3], "watchdog") ? 120U : 30U};
    assert(state.loop);
    struct ev_child child;
    ev_child_init(&child, child_reaped, 0, 0);
    ev_child_start(state.loop, &child);
    /* Start the broken platform first: its synchronous connect must not delay B. */
    assert(edge_ws_transport_connect(&state.bad, state.loop, argv[2], 16384,
                                    &state, bad_opened, bad_received, bad_closed));
    assert(edge_ws_transport_connect(&state.good, state.loop, argv[1], 16384,
                                    &state, good_opened, good_received, good_closed));
    struct ev_timer timer;
    ev_timer_init(&timer, tick, 0.1, 0.1);
    timer.data = &state;
    ev_timer_start(state.loop, &timer);
    ev_run(state.loop, 0);
    assert(state.done);
    ev_timer_stop(state.loop, &timer);
    edge_ws_transport_destroy(&state.good);
    edge_ws_transport_destroy(&state.bad);
    /* Cleanup must reap even a SIGSTOP'd or resolver-stuck child, asynchronously. */
    const ev_tstamp deadline = ev_time() + 2;
    while (ev_time() < deadline) {
        ev_run(state.loop, EVRUN_NOWAIT);
        usleep(10000);
        int status;
        if (waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD) break;
    }
    ev_run(state.loop, EVRUN_NOWAIT);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    ev_child_stop(state.loop, &child);
    ev_loop_destroy(state.loop);
    printf("%s: healthy replies=%u max_gap=%.3f bad_messages=%u\n",
           state.mode, state.received, state.max_gap, state.bad_received);
    return 0;
}
