#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_ACQUISITION_TICK_MS 1000U
#define EDGE_DEVICE_VALUE_MAX 512U
#define EDGE_DEVICE_WRITE_QUEUE 4U
#define EDGE_COMMAND_CLOCK_MAX_RTT_MS 2000U
#define EDGE_COMMAND_CLOCK_MAX_AGE_MS 930000U
#define EDGE_COMMAND_CLOCK_MAX_START_WINDOW_MS 60000U
#define EDGE_COMMAND_CLOCK_RATE_ERROR_PPM 1000U
#define EDGE_COMMAND_CLOCK_ISSUED_NONCES 16U

typedef enum {
    EDGE_DEVICE_MODBUS = 1,
    EDGE_DEVICE_S7 = 2,
    EDGE_DEVICE_MC = 3,
    EDGE_DEVICE_FINS = 4,
    EDGE_DEVICE_DLT645 = 5,
} edge_device_protocol;

typedef enum {
    EDGE_IO_OK = 0,
    EDGE_IO_NO_RESPONSE,
    EDGE_IO_OFFLINE,
    EDGE_IO_PROTOCOL_ERROR,
    EDGE_IO_COMMAND_START_EXPIRED,
} edge_io_result;

typedef enum {
    EDGE_COMMAND_SUCCEEDED = 0,
    EDGE_COMMAND_READBACK_MISMATCH,
    EDGE_COMMAND_DEVICE_OFFLINE,
    EDGE_COMMAND_TIMED_OUT,
    EDGE_COMMAND_FAILED,
    EDGE_COMMAND_REJECTED_START_EXPIRED,
} edge_command_result;

typedef struct {
    uint8_t command_id[16];
    char element_id[65];
    uint8_t value[EDGE_DEVICE_VALUE_MAX];
    size_t value_size;
    uint32_t fast_read_duration_sec;
    uint32_t fast_read_interval_sec;
    uint64_t start_before_monotonic_ms;
    uint64_t mapping_valid_until_monotonic_ms;
    uint32_t command_clock_generation;
} edge_write_command;

typedef struct {
    uint8_t bytes[EDGE_DEVICE_VALUE_MAX];
    size_t size;
    int64_t sampled_at_ms;
} edge_device_sample;

/* Samples allow at most 2s RTT and 930s age (the 900s status-report
 * interval plus 30s scheduling margin). A 1000ppm drift bound and millisecond
 * truncation are included in both sample-stability bounds and the conservative
 * deadline upper bound. Observed adjacent-sample jumps invalidate the mapping,
 * but unobserved database-clock steps have no protocol-level bound: a bounded,
 * stable DB clock remains a residual assumption. */
typedef struct {
    uint64_t next_nonce;
    uint64_t pending_nonce;
    /* Delayed issued nonces are ignored without accepting their DB sample. */
    uint64_t issued_nonces[EDGE_COMMAND_CLOCK_ISSUED_NONCES];
    uint8_t issued_nonce_next;
    uint8_t issued_nonce_count;
    uint64_t pending_sent_monotonic_ms;
    uint64_t sample_sent_monotonic_ms;
    uint64_t sample_received_monotonic_ms;
    int64_t database_time_ms;
    bool pending;
    bool valid;
} edge_command_clock;

void edge_command_clock_reset(edge_command_clock *clock);
void edge_command_clock_invalidate(edge_command_clock *clock);
bool edge_command_clock_accept_hello(edge_command_clock *clock,
                                     int64_t database_time_ms,
                                     uint64_t sent_monotonic_ms,
                                     uint64_t received_monotonic_ms);
bool edge_command_clock_begin_heartbeat(edge_command_clock *clock,
                                        uint64_t sent_monotonic_ms,
                                        uint64_t *nonce);
bool edge_command_clock_accept_heartbeat(edge_command_clock *clock,
                                         bool has_nonce, uint64_t nonce,
                                         bool has_database_time,
                                         int64_t database_time_ms,
                                         uint64_t received_monotonic_ms);
bool edge_command_clock_deadline(const edge_command_clock *clock,
                                 int64_t start_before_ms,
                                 uint64_t now_monotonic_ms,
                                 uint64_t *deadline_monotonic_ms,
                                 uint64_t *mapping_valid_until_monotonic_ms);

typedef struct {
    edge_io_result (*connect)(void *context);
    edge_io_result (*handshake)(void *context);
    edge_io_result (*read)(void *context, edge_device_sample *sample);
    edge_io_result (*write_readback)(void *context, const edge_write_command *command,
                                     edge_device_sample *actual);
    uint64_t (*monotonic_ms)(void *context);
    void (*disconnect)(void *context);
    void (*report)(void *context, const uint8_t platform_id[16],
                   const uint8_t device_id[16], const edge_device_sample *sample);
    void (*command_complete)(void *context, const uint8_t platform_id[16],
                             const uint8_t device_id[16], const uint8_t command_id[16],
                             edge_command_result result,
                             const edge_device_sample *actual);
} edge_device_driver;

typedef struct {
    edge_device_protocol protocol;
    uint8_t platform_id[16];
    uint8_t device_id[16];
    uint32_t report_interval_sec;
    uint64_t io_interval_ms;
    uint64_t next_io_at_ms;
    uint64_t next_report_at_ms;
    uint64_t fast_report_until_ms;
    uint64_t next_fast_report_at_ms;
    uint32_t fast_report_interval_sec;
    bool connected;
    bool handshaken;
    bool s7_tcp_client;
    bool debug_read;
    bool silent_background_read;
    bool initial_report_pending;
    bool has_sample;
    edge_device_sample latest;
    edge_write_command writes[EDGE_DEVICE_WRITE_QUEUE];
    uint8_t seen_command_ids[16][16];
    uint8_t seen_command_next;
    uint8_t seen_command_count;
    uint8_t write_head;
    uint8_t write_count;
    bool southbound_write_started;
    bool write_ack_received;
    edge_device_driver driver;
    void *driver_context;
} edge_device_runtime;

bool edge_device_runtime_init(edge_device_runtime *runtime,
                              edge_device_protocol protocol,
                              const uint8_t platform_id[16],
                              const uint8_t device_id[16],
                              uint32_t io_interval_ms,
                              uint32_t report_interval_sec,
                              uint64_t now_ms,
                              const edge_device_driver *driver,
                              void *driver_context);

bool edge_device_runtime_command_id_seen(const edge_device_runtime *runtime,
                                         const uint8_t command_id[16]);
bool edge_device_runtime_claim_command_id(edge_device_runtime *runtime,
                                          const uint8_t command_id[16]);
bool edge_device_runtime_enqueue_write(edge_device_runtime *runtime,
                                       const edge_write_command *command);
void edge_device_runtime_expire_write(edge_device_runtime *runtime, uint64_t now_ms);
void edge_device_runtime_reject_write(edge_device_runtime *runtime);

/* Call at least once per second. Slow calls never trigger a catch-up burst. */
void edge_device_runtime_tick(edge_device_runtime *runtime, uint64_t schedule_ms,
                              int64_t observed_at_ms);

void edge_device_runtime_close(edge_device_runtime *runtime);
