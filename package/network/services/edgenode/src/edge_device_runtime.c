#include "edge_device_runtime.h"

#include <string.h>

void edge_command_clock_reset(edge_command_clock *clock) {
    if (clock != NULL)
        memset(clock, 0, sizeof(*clock));
}

void edge_command_clock_invalidate(edge_command_clock *clock) {
    if (clock == NULL)
        return;
    clock->valid = false;
    clock->pending = false;
    clock->pending_nonce = 0U;
    clock->pending_sent_monotonic_ms = 0U;
    clock->sample_sent_monotonic_ms = 0U;
    clock->sample_received_monotonic_ms = 0U;
    clock->database_time_ms = 0;
}

static bool database_clock_sample_is_stable(const edge_command_clock *clock,
                                             int64_t database_time_ms,
                                             uint64_t sent_ms, uint64_t received_ms) {
    if (!clock->valid)
        return true;
    if (database_time_ms < clock->database_time_ms ||
        received_ms < clock->sample_sent_monotonic_ms)
        return false;

    const uint64_t earliest_elapsed = sent_ms > clock->sample_received_monotonic_ms
        ? sent_ms - clock->sample_received_monotonic_ms : 0U;
    const uint64_t latest_elapsed = received_ms - clock->sample_sent_monotonic_ms;
    if (latest_elapsed > UINT64_MAX / EDGE_COMMAND_CLOCK_RATE_ERROR_PPM)
        return false;
    const uint64_t scaled_error = latest_elapsed * EDGE_COMMAND_CLOCK_RATE_ERROR_PPM;
    const uint64_t rate_error = scaled_error / 1000000U +
        (scaled_error % 1000000U != 0U ? 1U : 0U);
    const uint64_t scaled_minimum_error =
        earliest_elapsed * EDGE_COMMAND_CLOCK_RATE_ERROR_PPM;
    const uint64_t minimum_rate_error = scaled_minimum_error / 1000000U +
        (scaled_minimum_error % 1000000U != 0U ? 1U : 0U);
    if (latest_elapsed > UINT64_MAX - rate_error - 2U)
        return false;

    /* Each DB timestamp may fall anywhere inside its request's measured RTT;
     * allow both RTTs, millisecond truncation, and positive or negative drift. */
    const uint64_t minimum_uncertainty = minimum_rate_error + 2U;
    const uint64_t minimum_delta = earliest_elapsed > minimum_uncertainty
        ? earliest_elapsed - minimum_uncertainty : 0U;
    const uint64_t maximum_delta = latest_elapsed + rate_error + 2U;
    const uint64_t database_delta = (uint64_t)database_time_ms -
                                    (uint64_t)clock->database_time_ms;
    return database_delta >= minimum_delta && database_delta <= maximum_delta;
}

static bool command_clock_nonce_was_issued(const edge_command_clock *clock,
                                           uint64_t nonce) {
    if (clock == NULL || nonce == 0U)
        return false;
    for (uint8_t index = 0U; index < clock->issued_nonce_count; ++index) {
        const uint8_t slot = (uint8_t)((clock->issued_nonce_next +
            EDGE_COMMAND_CLOCK_ISSUED_NONCES - clock->issued_nonce_count + index) %
            EDGE_COMMAND_CLOCK_ISSUED_NONCES);
        if (clock->issued_nonces[slot] == nonce)
            return true;
    }
    return false;
}

static void command_clock_remember_nonce(edge_command_clock *clock, uint64_t nonce) {
    if (nonce == 0U || command_clock_nonce_was_issued(clock, nonce))
        return;
    clock->issued_nonces[clock->issued_nonce_next] = nonce;
    clock->issued_nonce_next = (uint8_t)((clock->issued_nonce_next + 1U) %
                                         EDGE_COMMAND_CLOCK_ISSUED_NONCES);
    if (clock->issued_nonce_count < EDGE_COMMAND_CLOCK_ISSUED_NONCES)
        ++clock->issued_nonce_count;
}

static bool accept_database_time(edge_command_clock *clock, int64_t database_time_ms,
                                 uint64_t sent_ms, uint64_t received_ms) {
    if (clock == NULL || database_time_ms <= 0 || sent_ms > received_ms ||
        received_ms - sent_ms > EDGE_COMMAND_CLOCK_MAX_RTT_MS ||
        !database_clock_sample_is_stable(clock, database_time_ms, sent_ms, received_ms)) {
        edge_command_clock_invalidate(clock);
        return false;
    }
    clock->database_time_ms = database_time_ms;
    clock->sample_sent_monotonic_ms = sent_ms;
    clock->sample_received_monotonic_ms = received_ms;
    clock->valid = true;
    return true;
}

bool edge_command_clock_accept_hello(edge_command_clock *clock,
                                     int64_t database_time_ms,
                                     uint64_t sent_monotonic_ms,
                                     uint64_t received_monotonic_ms) {
    if (clock == NULL)
        return false;
    clock->pending = false;
    clock->pending_nonce = 0U;
    return accept_database_time(clock, database_time_ms, sent_monotonic_ms,
                                received_monotonic_ms);
}

bool edge_command_clock_begin_heartbeat(edge_command_clock *clock,
                                        uint64_t sent_monotonic_ms,
                                        uint64_t *nonce) {
    if (clock == NULL || nonce == NULL)
        return false;
    if (clock->pending) {
        if (sent_monotonic_ms >= clock->pending_sent_monotonic_ms &&
            sent_monotonic_ms - clock->pending_sent_monotonic_ms <=
                EDGE_COMMAND_CLOCK_MAX_RTT_MS) {
            *nonce = clock->pending_nonce;
            return true;
        }
        edge_command_clock_invalidate(clock);
    }
    if (++clock->next_nonce == 0U)
        ++clock->next_nonce;
    clock->pending_nonce = clock->next_nonce;
    command_clock_remember_nonce(clock, clock->pending_nonce);
    clock->pending_sent_monotonic_ms = sent_monotonic_ms;
    clock->pending = true;
    *nonce = clock->pending_nonce;
    return true;
}

bool edge_command_clock_accept_heartbeat(edge_command_clock *clock,
                                         bool has_nonce, uint64_t nonce,
                                         bool has_database_time,
                                         int64_t database_time_ms,
                                         uint64_t received_monotonic_ms) {
    if (clock == NULL)
        return false;
    if (!clock->pending) {
        if (!has_nonce && has_database_time)
            edge_command_clock_invalidate(clock);
        /* Repeated responses to overlapping heartbeats and delayed stale ACKs
         * are not fresh samples; never let them poison a newer valid mapping. */
        return false;
    }
    if (!has_nonce || nonce == 0U) {
        edge_command_clock_invalidate(clock);
        return false;
    }
    if (nonce != clock->pending_nonce) {
        if (command_clock_nonce_was_issued(clock, nonce))
            return false;
        edge_command_clock_invalidate(clock);
        return false;
    }
    const uint64_t sent_ms = clock->pending_sent_monotonic_ms;
    clock->pending = false;
    clock->pending_nonce = 0U;
    clock->pending_sent_monotonic_ms = 0U;
    if (!has_database_time) {
        edge_command_clock_invalidate(clock);
        return false;
    }
    return accept_database_time(clock, database_time_ms, sent_ms,
                                received_monotonic_ms);
}

bool edge_command_clock_deadline(const edge_command_clock *clock,
                                 int64_t start_before_ms,
                                 uint64_t now_monotonic_ms,
                                 uint64_t *deadline_monotonic_ms,
                                 uint64_t *mapping_valid_until_monotonic_ms) {
    if (clock == NULL || deadline_monotonic_ms == NULL ||
        mapping_valid_until_monotonic_ms == NULL || !clock->valid ||
        start_before_ms <= 0 || now_monotonic_ms < clock->sample_received_monotonic_ms ||
        now_monotonic_ms - clock->sample_received_monotonic_ms >=
            EDGE_COMMAND_CLOCK_MAX_AGE_MS ||
        clock->sample_received_monotonic_ms > UINT64_MAX - EDGE_COMMAND_CLOCK_MAX_AGE_MS ||
        now_monotonic_ms < clock->sample_sent_monotonic_ms)
        return false;
    *mapping_valid_until_monotonic_ms =
        clock->sample_received_monotonic_ms + EDGE_COMMAND_CLOCK_MAX_AGE_MS;

    const uint64_t elapsed = now_monotonic_ms - clock->sample_sent_monotonic_ms;
    if (elapsed > UINT64_MAX / EDGE_COMMAND_CLOCK_RATE_ERROR_PPM)
        return false;
    const uint64_t scaled_error = elapsed * EDGE_COMMAND_CLOCK_RATE_ERROR_PPM;
    const uint64_t rate_error = scaled_error / 1000000U +
                                (scaled_error % 1000000U != 0U ? 1U : 0U);
    const uint64_t database_time = (uint64_t)clock->database_time_ms;
    if (database_time > UINT64_MAX - elapsed ||
        database_time + elapsed > UINT64_MAX - rate_error - 1U)
        return false;
    /* The sample is no earlier than the monotonic send boundary. Advancing the
     * sample by the entire send-to-now interval is therefore conservative;
     * +1ms covers database timestamp truncation, and the ppm term bounds drift
     * only while the database clock remains stable. */
    const uint64_t upper_database_now = database_time + elapsed + rate_error + 1U;
    if ((uint64_t)start_before_ms <= upper_database_now)
        return false;
    const uint64_t remaining = (uint64_t)start_before_ms - upper_database_now;
    if (remaining > EDGE_COMMAND_CLOCK_MAX_START_WINDOW_MS)
        return false;
    const uint64_t monotonic_remaining =
        (remaining * 1000000U) / (1000000U + EDGE_COMMAND_CLOCK_RATE_ERROR_PPM);
    /* Round early for the next drift interval and two separately rounded ppm
     * terms; deadlines with too little safe margin are rejected outright. */
    if (monotonic_remaining <= 2U ||
        now_monotonic_ms > UINT64_MAX - (monotonic_remaining - 2U))
        return false;
    *deadline_monotonic_ms = now_monotonic_ms + monotonic_remaining - 2U;
    return *deadline_monotonic_ms > now_monotonic_ms;
}

static uint64_t runtime_now(const edge_device_runtime *runtime, uint64_t fallback) {
    return runtime->driver.monotonic_ms != NULL
               ? runtime->driver.monotonic_ms(runtime->driver_context)
               : fallback;
}

static uint64_t advance_deadline(uint64_t current, uint64_t period, uint64_t now) {
    if (current > now)
        return current;
    const uint64_t elapsed = now - current;
    const uint64_t steps = elapsed / period + 1U;
    if (steps > (UINT64_MAX - current) / period)
        return now + period;
    return current + steps * period;
}

static uint64_t deadline_after_seconds(uint64_t now, uint32_t seconds) {
    const uint64_t delay = (uint64_t)seconds * 1000U;
    return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}

static void configure_fast_reporting(edge_device_runtime *runtime,
                                     const edge_write_command *command,
                                     uint64_t now_ms) {
    runtime->fast_report_until_ms = 0U;
    runtime->next_fast_report_at_ms = 0U;
    runtime->fast_report_interval_sec = 0U;
    if (command->fast_read_duration_sec == 0U || command->fast_read_interval_sec == 0U)
        return;
    runtime->fast_report_until_ms =
        deadline_after_seconds(now_ms, command->fast_read_duration_sec);
    runtime->next_fast_report_at_ms =
        deadline_after_seconds(now_ms, command->fast_read_interval_sec);
    runtime->fast_report_interval_sec = command->fast_read_interval_sec;
}

static void close_connection(edge_device_runtime *runtime) {
    if ((runtime->connected || runtime->handshaken) && runtime->driver.disconnect != NULL)
        runtime->driver.disconnect(runtime->driver_context);
    runtime->connected = false;
    runtime->handshaken = false;
}

bool edge_device_runtime_init(edge_device_runtime *runtime,
                              edge_device_protocol protocol,
                              const uint8_t platform_id[16],
                              const uint8_t device_id[16],
                              uint32_t io_interval_ms,
                              uint32_t report_interval_sec,
                              uint64_t now_ms,
                              const edge_device_driver *driver,
                              void *driver_context) {
    if (runtime == NULL || platform_id == NULL || device_id == NULL || driver == NULL ||
        driver->connect == NULL || driver->read == NULL || driver->report == NULL ||
        driver->command_complete == NULL || report_interval_sec == 0U ||
        (protocol < EDGE_DEVICE_MODBUS || protocol > EDGE_DEVICE_DLT645) ||
        (io_interval_ms != 0U && (io_interval_ms < 1000U || io_interval_ms > 3600000U)) ||
        ((protocol == EDGE_DEVICE_S7 || protocol == EDGE_DEVICE_FINS) && driver->handshake == NULL))
        return false;

    memset(runtime, 0, sizeof(*runtime));
    runtime->protocol = protocol;
    memcpy(runtime->platform_id, platform_id, 16U);
    memcpy(runtime->device_id, device_id, 16U);
    runtime->report_interval_sec = report_interval_sec;
    (void)io_interval_ms;
    runtime->io_interval_ms = EDGE_ACQUISITION_TICK_MS;
    runtime->next_io_at_ms = now_ms;
    runtime->next_report_at_ms = deadline_after_seconds(now_ms, report_interval_sec);
    runtime->initial_report_pending = true;
    runtime->driver = *driver;
    runtime->driver_context = driver_context;
    return true;
}

bool edge_device_runtime_command_id_seen(const edge_device_runtime *runtime,
                                         const uint8_t command_id[16]) {
    if (runtime == NULL || command_id == NULL)
        return false;
    for (uint8_t index = 0U; index < runtime->seen_command_count; ++index) {
        const uint8_t slot = (uint8_t)((runtime->seen_command_next + 16U -
                                        runtime->seen_command_count + index) % 16U);
        if (memcmp(runtime->seen_command_ids[slot], command_id, 16U) == 0)
            return true;
    }
    return false;
}

bool edge_device_runtime_claim_command_id(edge_device_runtime *runtime,
                                          const uint8_t command_id[16]) {
    if (runtime == NULL || command_id == NULL ||
        edge_device_runtime_command_id_seen(runtime, command_id))
        return false;
    memcpy(runtime->seen_command_ids[runtime->seen_command_next], command_id, 16U);
    runtime->seen_command_next = (uint8_t)((runtime->seen_command_next + 1U) % 16U);
    if (runtime->seen_command_count < 16U)
        ++runtime->seen_command_count;
    return true;
}

bool edge_device_runtime_enqueue_write(edge_device_runtime *runtime,
                                       const edge_write_command *command) {
    if (runtime == NULL || command == NULL || command->value_size == 0U ||
        command->value_size > EDGE_DEVICE_VALUE_MAX ||
        command->start_before_monotonic_ms == 0U)
        return false;
    for (uint8_t index = 0U; index < runtime->seen_command_count; ++index) {
        const uint8_t slot = (uint8_t)((runtime->seen_command_next + 16U -
                                        runtime->seen_command_count + index) % 16U);
        if (memcmp(runtime->seen_command_ids[slot], command->command_id, 16U) == 0)
            return true;
    }
    if (runtime->write_count >= EDGE_DEVICE_WRITE_QUEUE)
        return false;
    if (!edge_device_runtime_claim_command_id(runtime, command->command_id))
        return true;
    const uint8_t tail = (uint8_t)((runtime->write_head + runtime->write_count) %
                                   EDGE_DEVICE_WRITE_QUEUE);
    runtime->writes[tail] = *command;
    ++runtime->write_count;
    return true;
}

static edge_io_result ensure_ready(edge_device_runtime *runtime) {
    if (!runtime->connected) {
        const edge_io_result connected = runtime->driver.connect(runtime->driver_context);
        if (connected != EDGE_IO_OK)
            return connected;
        runtime->connected = true;
    }
    if ((runtime->protocol == EDGE_DEVICE_S7 || runtime->protocol == EDGE_DEVICE_FINS) && !runtime->handshaken) {
        const edge_io_result handshaken = runtime->driver.handshake(runtime->driver_context);
        if (handshaken != EDGE_IO_OK) {
            if (runtime->protocol == EDGE_DEVICE_S7)
                close_connection(runtime);
            return handshaken;
        }
        runtime->handshaken = true;
    }
    return EDGE_IO_OK;
}

static void complete_write(edge_device_runtime *runtime, edge_command_result result,
                           const edge_device_sample *actual) {
    const edge_write_command *command = &runtime->writes[runtime->write_head];
    runtime->driver.command_complete(runtime->driver_context, runtime->platform_id,
                                     runtime->device_id, command->command_id, result, actual);
    runtime->write_head = (uint8_t)((runtime->write_head + 1U) % EDGE_DEVICE_WRITE_QUEUE);
    --runtime->write_count;
}

static bool same_value(const edge_write_command *command, const edge_device_sample *actual) {
    return command->value_size == actual->size &&
           memcmp(command->value, actual->bytes, actual->size) == 0;
}

static void handle_no_response(edge_device_runtime *runtime) {
    /*
     * S7 must not reuse a timed-out COTP/S7 session; reset both TCP and
     * handshake state before reconnecting and negotiating from the beginning.
     * Modbus connections are deliberately kept open across a
     * timeout: some gateways send a banner or delayed response on the same
     * TCP stream and must not be forced through a reconnect loop.
     */
    if (runtime->protocol != EDGE_DEVICE_MODBUS)
        close_connection(runtime);
}

static void handle_offline(edge_device_runtime *runtime) {
    /* A transport failure is different from a protocol timeout: the next
     * cycle must establish a new connection before sending another request. */
    close_connection(runtime);
}

void edge_device_runtime_reject_write(edge_device_runtime *runtime) {
    if (runtime == NULL || runtime->write_count == 0U)
        return;
    runtime->southbound_write_started = false;
    runtime->write_ack_received = false;
    complete_write(runtime, EDGE_COMMAND_REJECTED_START_EXPIRED, NULL);
}

static bool reject_expired_write(edge_device_runtime *runtime, uint64_t now_ms) {
    if (runtime->write_count == 0U)
        return false;
    const edge_write_command *command = &runtime->writes[runtime->write_head];
    const bool start_is_future = runtime->driver.monotonic_ms == NULL
        ? now_ms < command->start_before_monotonic_ms
        : now_ms != 0U && now_ms < command->start_before_monotonic_ms;
    const bool mapping_is_future = command->mapping_valid_until_monotonic_ms == 0U ||
        now_ms < command->mapping_valid_until_monotonic_ms;
    if (start_is_future && mapping_is_future)
        return false;
    edge_device_runtime_reject_write(runtime);
    return true;
}

void edge_device_runtime_expire_write(edge_device_runtime *runtime, uint64_t now_ms) {
    if (runtime != NULL)
        (void)reject_expired_write(runtime, runtime_now(runtime, now_ms));
}

void edge_device_runtime_tick(edge_device_runtime *runtime, uint64_t schedule_ms,
                              int64_t observed_at_ms) {
    if (runtime == NULL)
        return;

    const bool expired_before_io =
        reject_expired_write(runtime, runtime_now(runtime, schedule_ms));
    bool sampled_this_cycle = false;
    const bool fast_report_due = runtime->fast_report_until_ms != 0U &&
        runtime->next_fast_report_at_ms <= runtime->fast_report_until_ms &&
        schedule_ms >= runtime->next_fast_report_at_ms;
    const bool fast_window_active = runtime->fast_report_until_ms != 0U &&
        schedule_ms <= runtime->fast_report_until_ms;
    bool report_due = fast_report_due;
    if (fast_report_due)
        runtime->next_fast_report_at_ms = advance_deadline(
            runtime->next_fast_report_at_ms,
            (uint64_t)runtime->fast_report_interval_sec * 1000U, schedule_ms);
    if (schedule_ms >= runtime->next_report_at_ms) {
        runtime->next_report_at_ms = advance_deadline(runtime->next_report_at_ms,
            (uint64_t)runtime->report_interval_sec * 1000U, schedule_ms);
        if (!fast_window_active)
            report_due = true;
    }
    if (runtime->fast_report_until_ms != 0U && schedule_ms >= runtime->fast_report_until_ms) {
        runtime->fast_report_until_ms = 0U;
        runtime->next_fast_report_at_ms = 0U;
        runtime->fast_report_interval_sec = 0U;
    }
    runtime->debug_read = report_due || runtime->write_count != 0U;

    const bool io_due = schedule_ms >= runtime->next_io_at_ms;
    if (io_due || runtime->write_count != 0U) {
        if (io_due)
            runtime->next_io_at_ms = advance_deadline(runtime->next_io_at_ms,
                                                      runtime->io_interval_ms, schedule_ms);
        runtime->silent_background_read = !runtime->debug_read && runtime->write_count == 0U;
        const bool command_cycle = runtime->write_count != 0U || expired_before_io;
        edge_io_result result = ensure_ready(runtime);
        const bool expired_after_ready = runtime->write_count != 0U &&
            reject_expired_write(runtime, runtime_now(runtime, schedule_ms));
        if (expired_after_ready)
            result = EDGE_IO_OK;
        if (result == EDGE_IO_OK && runtime->write_count != 0U &&
            !expired_before_io && !expired_after_ready) {
            edge_device_sample actual = {0};
            const edge_write_command *command = &runtime->writes[runtime->write_head];
            runtime->southbound_write_started = false;
            runtime->write_ack_received = false;
            if (runtime->driver.write_readback == NULL) {
                complete_write(runtime, EDGE_COMMAND_FAILED, NULL);
            } else {
                result = runtime->driver.write_readback(runtime->driver_context, command, &actual);
                if (result == EDGE_IO_COMMAND_START_EXPIRED &&
                    !runtime->southbound_write_started) {
                    complete_write(runtime, EDGE_COMMAND_REJECTED_START_EXPIRED, NULL);
                    result = EDGE_IO_OK;
                } else if (result == EDGE_IO_OK) {
                    const bool verified = same_value(command, &actual);
                    actual.sampled_at_ms = observed_at_ms;
                    if (verified)
                        configure_fast_reporting(runtime, command, schedule_ms);
                    complete_write(runtime,
                                   verified ? EDGE_COMMAND_SUCCEEDED
                                            : EDGE_COMMAND_READBACK_MISMATCH,
                                   &actual);
                } else if (result == EDGE_IO_NO_RESPONSE) {
                    complete_write(runtime, EDGE_COMMAND_TIMED_OUT, NULL);
                    handle_no_response(runtime);
                } else if (result == EDGE_IO_OFFLINE) {
                    complete_write(runtime, EDGE_COMMAND_DEVICE_OFFLINE, NULL);
                    handle_offline(runtime);
                } else {
                    complete_write(runtime, EDGE_COMMAND_FAILED, NULL);
                    if (runtime->protocol == EDGE_DEVICE_S7)
                        close_connection(runtime);
                }
            }
        }

        if (result == EDGE_IO_OK && !command_cycle && io_due) {
            edge_device_sample sample = {0};
            result = runtime->driver.read(runtime->driver_context, &sample);
            if ((result == EDGE_IO_NO_RESPONSE || result == EDGE_IO_PROTOCOL_ERROR) &&
                runtime->protocol == EDGE_DEVICE_S7) {
                /* A timed-out or invalid S7 response poisons the negotiated session.
                 * Retry the read once on fresh TCP/COTP/S7 handshakes; never replay
                 * a write, and bound recovery so other devices can still run. */
                close_connection(runtime);
                result = ensure_ready(runtime);
                if (result == EDGE_IO_OK) {
                    memset(&sample, 0, sizeof(sample));
                    result = runtime->driver.read(runtime->driver_context, &sample);
                } else {
                    close_connection(runtime);
                }
            }
            if (result == EDGE_IO_OK && sample.size <= EDGE_DEVICE_VALUE_MAX) {
                sample.sampled_at_ms = observed_at_ms;
                runtime->latest = sample;
                runtime->has_sample = true;
                sampled_this_cycle = true;
            } else if (result == EDGE_IO_NO_RESPONSE) {
                handle_no_response(runtime);
            } else if (result == EDGE_IO_OFFLINE) {
                handle_offline(runtime);
            } else if (result == EDGE_IO_PROTOCOL_ERROR && runtime->protocol == EDGE_DEVICE_S7) {
                close_connection(runtime);
            }
        } else if (result == EDGE_IO_NO_RESPONSE) {
            handle_no_response(runtime);
        } else if (result == EDGE_IO_OFFLINE) {
            handle_offline(runtime);
        } else if (result == EDGE_IO_PROTOCOL_ERROR && runtime->protocol == EDGE_DEVICE_S7) {
            close_connection(runtime);
        }

        runtime->silent_background_read = false;
    }

    if (runtime->initial_report_pending && report_due && sampled_this_cycle)
        runtime->initial_report_pending = false;
    if (report_due && sampled_this_cycle)
        runtime->driver.report(runtime->driver_context, runtime->platform_id,
                               runtime->device_id, &runtime->latest);
}

void edge_device_runtime_close(edge_device_runtime *runtime) {
    if (runtime == NULL)
        return;
    close_connection(runtime);
}
