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

int main(void) {
    /* Five-minute reporting must not postpone application liveness probes. */
    for (unsigned timeout = 15000U; timeout <= 90000U; timeout += 75000U) {
        edge_retry idle;
        require_true(edge_retry_init(&idle, 5000U, 30000U), "idle init");
        edge_retry_application_ready(&idle, 0U, timeout);
        uint64_t last_reply = 0U, last_probe = 0U;
        for (uint64_t tick = 1000U; tick <= 600000U; tick += 1000U) {
            require_true(!edge_retry_application_timed_out(&idle, tick),
                         "long heartbeat caused a healthy session to reconnect");
            if (edge_retry_probe_due(tick, last_reply, last_probe, timeout)) {
                last_probe = tick;
                last_reply = tick;
                edge_retry_application_alive(&idle, tick, timeout);
            }
        }
        require_true(edge_retry_application_timed_out(&idle, last_reply + timeout),
                     "missing application replies did not time out");
        require_true(!edge_retry_probe_due(last_probe + 1, last_reply, last_probe, timeout),
                     "probe rate was not bounded");
    }
    edge_retry retry;
    require_true(edge_retry_init(&retry, 5000U, 30000U), "retry initialization failed");
    require_true(edge_retry_should_start(&retry, 0U), "initial connection was not ready");

    uint64_t now = 0U;
    for (unsigned cycle = 0U; cycle < 1000U; ++cycle) {
        edge_retry_attempt_started(&retry, now);
        require_true(!edge_retry_attempt_timed_out(&retry, now + 29999U),
                     "connection attempt timed out too early");
        require_true(edge_retry_attempt_timed_out(&retry, now + 30000U),
                     "stalled connection attempt did not time out");

        now += 30000U;
        edge_retry_failed(&retry, now);
        require_true(!edge_retry_should_start(&retry, now + 4999U),
                     "retry started before the configured delay");
        require_true(edge_retry_should_start(&retry, now + 5000U),
                     "retry loop stopped scheduling attempts");
        now += 5000U;
    }

    edge_retry_attempt_started(&retry, now);
    edge_retry_transport_connected(&retry, now, 30000U);
    require_true(!edge_retry_attempt_timed_out(&retry, UINT64_MAX),
                 "open transport retained the connection attempt watchdog");
    require_true(!edge_retry_should_start(&retry, UINT64_MAX),
                 "open transport scheduled a duplicate connection");
    require_true(!edge_retry_application_timed_out(&retry, now + 29999U),
                 "application handshake timed out too early");
    require_true(edge_retry_application_timed_out(&retry, now + 30000U),
                 "missing application handshake did not time out");

    edge_retry_application_alive(&retry, now + 10000U, 30000U);
    require_true(edge_retry_application_timed_out(&retry, now + 30000U),
                 "pre-enrollment traffic bypassed the handshake deadline");
    edge_retry_application_ready(&retry, now + 20000U, 60000U);
    require_true(!edge_retry_application_timed_out(&retry, now + 79999U),
                 "enrolled session timed out too early");
    require_true(edge_retry_application_timed_out(&retry, now + 80000U),
                 "silent enrolled session did not time out");
    edge_retry_application_ready(&retry, now + 20000U, 60000U);
    edge_retry_application_alive(&retry, now + 70000U, 60000U);
    require_true(!edge_retry_application_timed_out(&retry, now + 129999U),
                 "valid enrolled traffic did not refresh the watchdog");

    edge_retry_failed(&retry, now);
    require_true(edge_retry_should_start(&retry, now + 5000U),
                 "disconnect did not resume the retry loop");
    puts("edge retry tests passed");
    return EXIT_SUCCESS;
}
