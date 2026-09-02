#include "edge_device_runtime.h"

#include <string.h>

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
        (protocol != EDGE_DEVICE_MODBUS && protocol != EDGE_DEVICE_S7) ||
        (io_interval_ms != 0U && io_interval_ms != EDGE_DTU_IO_PERIOD_MS) ||
        (protocol == EDGE_DEVICE_S7 && driver->handshake == NULL))
        return false;

    memset(runtime, 0, sizeof(*runtime));
    runtime->protocol = protocol;
    memcpy(runtime->platform_id, platform_id, 16U);
    memcpy(runtime->device_id, device_id, 16U);
    runtime->report_interval_sec = report_interval_sec;
    runtime->next_io_at_ms = now_ms;
    runtime->next_report_at_ms = now_ms + (uint64_t)report_interval_sec * 1000U;
    runtime->initial_report_pending = true;
    runtime->driver = *driver;
    runtime->driver_context = driver_context;
    return true;
}

bool edge_device_runtime_enqueue_write(edge_device_runtime *runtime,
                                       const edge_write_command *command) {
    if (runtime == NULL || command == NULL || command->value_size == 0U ||
        command->value_size > EDGE_DEVICE_VALUE_MAX ||
        runtime->write_count >= EDGE_DEVICE_WRITE_QUEUE)
        return false;
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
    if (runtime->protocol == EDGE_DEVICE_S7 && !runtime->handshaken) {
        const edge_io_result handshaken = runtime->driver.handshake(runtime->driver_context);
        if (handshaken != EDGE_IO_OK)
            return handshaken;
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
     * handshake state so the next tick reconnects and negotiates from the
     * beginning. Modbus connections are deliberately kept open across a
     * timeout: some gateways send a banner or delayed response on the same
     * TCP stream and must not be forced through a reconnect loop.
     */
    if (runtime->protocol == EDGE_DEVICE_S7)
        close_connection(runtime);
}

static void handle_offline(edge_device_runtime *runtime) {
    /* A transport failure is different from a protocol timeout: the next
     * cycle must establish a new connection before sending another request. */
    close_connection(runtime);
}

void edge_device_runtime_tick(edge_device_runtime *runtime, uint64_t schedule_ms,
                              int64_t observed_at_ms) {
    if (runtime == NULL)
        return;

    bool reported_after_write = false;
    edge_device_sample write_actual = {0};

    if (schedule_ms >= runtime->next_io_at_ms) {
        runtime->next_io_at_ms = advance_deadline(runtime->next_io_at_ms,
                                                  EDGE_DTU_IO_PERIOD_MS, schedule_ms);
        edge_io_result result = ensure_ready(runtime);
        if (result == EDGE_IO_OK && runtime->write_count != 0U) {
            edge_device_sample actual = {0};
            const edge_write_command *command = &runtime->writes[runtime->write_head];
            if (runtime->driver.write_readback == NULL) {
                complete_write(runtime, EDGE_COMMAND_FAILED, NULL);
            } else {
                result = runtime->driver.write_readback(runtime->driver_context, command, &actual);
                if (result == EDGE_IO_OK) {
                    const bool verified = same_value(command, &actual);
                    actual.sampled_at_ms = observed_at_ms;
                    write_actual = actual;
                    if (verified) {
                        configure_fast_reporting(runtime, command, schedule_ms);
                    }
                    complete_write(runtime,
                                   verified ? EDGE_COMMAND_SUCCEEDED
                                            : EDGE_COMMAND_READBACK_MISMATCH,
                                   &actual);
                    reported_after_write = true;
                } else if (result == EDGE_IO_NO_RESPONSE) {
                    complete_write(runtime, EDGE_COMMAND_TIMED_OUT, NULL);
                    handle_no_response(runtime);
                } else if (result == EDGE_IO_OFFLINE) {
                    complete_write(runtime, EDGE_COMMAND_DEVICE_OFFLINE, NULL);
                    handle_offline(runtime);
                } else {
                    complete_write(runtime, EDGE_COMMAND_FAILED, NULL);
                }
            }
        }

        if (result == EDGE_IO_OK) {
            edge_device_sample sample = {0};
            result = runtime->driver.read(runtime->driver_context, &sample);
            if (result == EDGE_IO_OK && sample.size <= EDGE_DEVICE_VALUE_MAX) {
                sample.sampled_at_ms = observed_at_ms;
                runtime->latest = sample;
                runtime->has_sample = true;
            } else if (result == EDGE_IO_NO_RESPONSE) {
                handle_no_response(runtime);
            } else if (result == EDGE_IO_OFFLINE) {
                handle_offline(runtime);
            }
        } else if (result == EDGE_IO_NO_RESPONSE) {
            handle_no_response(runtime);
        } else if (result == EDGE_IO_OFFLINE) {
            handle_offline(runtime);
        }

        if (reported_after_write) {
            const edge_device_sample *sample = &write_actual;
            if (runtime->has_sample && runtime->latest.sampled_at_ms == observed_at_ms)
                sample = &runtime->latest;
            runtime->driver.report(runtime->driver_context, runtime->platform_id,
                                   runtime->device_id, sample);
        }
    }

    /* A newly applied configuration must become observable as soon as the
     * device produces its first valid sample. Keep the configured interval
     * for all later reports, and keep this pending across failed reads. */
    if (runtime->initial_report_pending && runtime->has_sample) {
        if (!reported_after_write)
            runtime->driver.report(runtime->driver_context, runtime->platform_id,
                                   runtime->device_id, &runtime->latest);
        runtime->initial_report_pending = false;
        runtime->next_report_at_ms =
            deadline_after_seconds(schedule_ms, runtime->report_interval_sec);
    }

    const uint64_t report_period = (uint64_t)runtime->report_interval_sec * 1000U;
    bool report_due = false;
    const bool fast_window_active =
        runtime->fast_report_until_ms != 0U && schedule_ms <= runtime->fast_report_until_ms;
    if (runtime->fast_report_until_ms != 0U &&
        runtime->next_fast_report_at_ms <= runtime->fast_report_until_ms &&
        schedule_ms >= runtime->next_fast_report_at_ms) {
        report_due = true;
        runtime->next_fast_report_at_ms =
            advance_deadline(runtime->next_fast_report_at_ms,
                             (uint64_t)runtime->fast_report_interval_sec * 1000U,
                             schedule_ms);
    }
    if (runtime->fast_report_until_ms != 0U &&
        schedule_ms >= runtime->fast_report_until_ms) {
        runtime->fast_report_until_ms = 0U;
        runtime->next_fast_report_at_ms = 0U;
        runtime->fast_report_interval_sec = 0U;
    }
    if (schedule_ms >= runtime->next_report_at_ms) {
        runtime->next_report_at_ms = advance_deadline(runtime->next_report_at_ms,
                                                      report_period, schedule_ms);
        if (!fast_window_active)
            report_due = true;
    }
    if (report_due && !reported_after_write && runtime->has_sample)
        runtime->driver.report(runtime->driver_context, runtime->platform_id,
                               runtime->device_id, &runtime->latest);
}

void edge_device_runtime_close(edge_device_runtime *runtime) {
    if (runtime == NULL)
        return;
    close_connection(runtime);
}
