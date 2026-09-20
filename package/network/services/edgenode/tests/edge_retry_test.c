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
    const uint32_t timeouts[] = {15000U, 90000U, 900000U};
    for (unsigned i = 0; i < sizeof(timeouts) / sizeof(timeouts[0]); ++i) {
        const uint32_t timeout = timeouts[i];
        edge_retry idle;
        require_true(edge_retry_init(&idle, 5000U, 30000U), "idle init");
        edge_retry_application_ready(&idle, 0U, timeout);
        uint64_t last_reply = 0U, last_probe = 0U;
        unsigned probes = 0;
        for (uint64_t tick = 1000U; tick <= 1800000U; tick += 1000U) {
            require_true(!edge_retry_application_timed_out(&idle, tick),
                         "healthy session reconnected between heartbeats");
            if (edge_retry_probe_due(tick, last_reply, last_probe, timeout)) {
                last_probe = last_reply = tick;
                ++probes;
                edge_retry_application_alive(&idle, tick, timeout);
            }
        }
        if (timeout == 900000U)
            require_true(probes == 6U, "five-minute probes sent extra packets");
        require_true(edge_retry_application_timed_out(&idle, last_reply + timeout),
                     "missing replies did not time out");
        require_true(!edge_retry_probe_due(last_probe + 1, last_reply, last_probe, timeout),
                     "probe rate was not bounded");
    }
    require_true(!edge_retry_probe_due(299999U, 0U, 0U, 900000U), "early probe");
    require_true(edge_retry_probe_due(300000U, 0U, 0U, 900000U), "missing idle probe");
    require_true(!edge_retry_probe_due(300000U, 299000U, 0U, 900000U), "business reply did not suppress probe");
    require_true(!edge_retry_probe_due(300000U, 0U, 300000U, 900000U), "heartbeat did not suppress duplicate probe");
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
    edge_retry_application_ready(&retry, now, 900000U);
    edge_retry_application_alive(&retry, now + 1000U, 900000U);
    edge_retry_failed(&retry, now + 1001U);
    require_true(edge_retry_delay_ms(&retry, now + 1001U) == 300000U, "flapping reset backoff");
    now += 301001U;
    edge_retry_attempt_started(&retry, now);
    edge_retry_application_ready(&retry, now, 900000U);
    edge_retry_application_alive(&retry, now + 300000U, 900000U);
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
    edge_retry_application_alive(&retry, 10000U, 900000U);
    require_true(edge_retry_application_timed_out(&retry, 30000U), "unvalidated traffic extended handshake");
    // HelloAck and EnrollmentPending establish liveness; subsequent heartbeats renew it.
    edge_retry_application_ready(&retry, 20000U, 900000U);
    require_true(!edge_retry_application_timed_out(&retry, 319999U), "five-minute heartbeat disconnected");
    edge_retry_application_alive(&retry, 320000U, 900000U);
    require_true(!edge_retry_application_timed_out(&retry, 1219999U), "reply failed to renew watchdog");
    require_true(edge_retry_application_timed_out(&retry, 1220000U), "silent connection never expired");
}

int main(void) {
    test_liveness();
    test_backoff();
    test_handshake();
    puts("edge retry tests passed");
    return EXIT_SUCCESS;
}
