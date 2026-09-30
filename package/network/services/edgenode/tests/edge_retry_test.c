#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "edge_retry.h"

static void require_true(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "edge retry test failed: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_liveness(void) {
    edge_retry idle;
    require_true(edge_retry_init(&idle, 5000U, 30000U), "idle init");
    edge_retry_application_ready(&idle, 0U);
    // Idle time and missing status reports never determine transport liveness.
    require_true(!edge_retry_application_timed_out(&idle, 900000U), "report interval closed session");
    require_true(!edge_retry_application_timed_out(&idle, UINT64_MAX), "idle WS closed by application timer");
    require_true(!edge_retry_should_start(&idle, UINT64_MAX), "idle WS scheduled another connection");
    // A real transport close still schedules bounded reconnect.
    edge_retry_failed(&idle, 1800000U);
    require_true(!edge_retry_should_start(&idle, 1804999U), "early transport retry");
    require_true(edge_retry_should_start(&idle, 1805000U), "transport close failed to reconnect");
}
static void test_backoff(void) {
    edge_retry retry;
    require_true(edge_retry_init(&retry, 5000U, 30000U), "retry init");
    require_true(edge_retry_should_start(&retry, 0U), "initial attempt delayed");
    const uint32_t delays[] = {5000U, 10000U, 20000U, 40000U, 80000U, 160000U, 300000U, 300000U};
    uint64_t now = 0;
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
        edge_retry_attempt_started(&retry, now);
        require_true(!edge_retry_attempt_timed_out(&retry, now + 29999U), "early connect timeout");
        require_true(edge_retry_attempt_timed_out(&retry, now + 30000U), "missing connect timeout");
        now += 30000U;
        edge_retry_failed(&retry, now);
        require_true(edge_retry_delay_ms(&retry, now) == delays[i], "wrong exponential backoff");
        edge_retry_failed(&retry, now + 1U);
        require_true(edge_retry_delay_ms(&retry, now) == delays[i], "duplicate close rescheduled retry");
        require_true(!edge_retry_should_start(&retry, now + delays[i] - 1U), "early retry");
        now += delays[i];
        require_true(edge_retry_should_start(&retry, now), "retry stopped");
    }
    edge_retry_attempt_started(&retry, now);
    edge_retry_application_ready(&retry, now);
    edge_retry_failed(&retry, now + 1001U);
    require_true(edge_retry_delay_ms(&retry, now + 1001U) == 300000U, "flapping reset backoff");
    now += 301001U;
    edge_retry_attempt_started(&retry, now);
    edge_retry_application_ready(&retry, now);
    edge_retry_failed(&retry, now + 300001U);
    require_true(edge_retry_delay_ms(&retry, now + 300001U) == 5000U, "stable connection did not reset backoff");

    require_true(edge_retry_init(&retry, 600000U, 30000U), "long configured delay init");
    edge_retry_failed(&retry, 0);
    require_true(edge_retry_delay_ms(&retry, 0) == 600000U, "configured longer delay shortened");
    edge_retry_attempt_started(&retry, UINT64_MAX - 1U);
    edge_retry_failed(&retry, UINT64_MAX - 1U);
    require_true(retry.deadline_ms == UINT64_MAX, "deadline overflow");
}

static void test_handshake(void) {
    edge_retry retry;
    require_true(edge_retry_init(&retry, 5000U, 30000U), "handshake init");
    edge_retry_attempt_started(&retry, 0);
    edge_retry_transport_connected(&retry, 0, 30000U);
    require_true(!edge_retry_attempt_timed_out(&retry, UINT64_MAX), "transport retained connect watchdog");
    require_true(!edge_retry_should_start(&retry, UINT64_MAX), "duplicate connection scheduled");
    require_true(!edge_retry_application_timed_out(&retry, 29999U), "early handshake timeout");
    require_true(edge_retry_application_timed_out(&retry, 30000U), "missing handshake timeout");
    require_true(edge_retry_application_timed_out(&retry, 30000U), "unvalidated traffic extended handshake");
    // HelloAck and EnrollmentPending complete negotiation; WS owns subsequent liveness.
    edge_retry_application_ready(&retry, 20000U);
    require_true(!edge_retry_application_timed_out(&retry, 319999U), "five-minute heartbeat disconnected");
    require_true(!edge_retry_application_timed_out(&retry, 1219999U), "reply failed to renew watchdog");
    require_true(!edge_retry_application_timed_out(&retry, UINT64_MAX), "report inactivity expired WS session");
}

int main(void) {
    test_liveness();
    test_backoff();
    test_handshake();
    puts("edge retry tests passed");
    return EXIT_SUCCESS;
}
