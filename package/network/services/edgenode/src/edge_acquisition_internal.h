#ifndef EDGENODE_ACQUISITION_INTERNAL_H
#define EDGENODE_ACQUISITION_INTERNAL_H

#include "edge_acquisition.h"
#include "edge_derived.h"
#include "edge_device_runtime.h"
#include "edge_dtu.h"
#include "edge_external_io.h"
#include "edge_industrial.h"
#include "edge_modbus.h"
#include "edge_process.h"
#include "edge_protocol.h"
#include "edge_s7.h"
#include "edge_sl651.h"
#include "edge_util.h"
#include "log.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/serial.h>
#endif

#define EDGE_IO_TIMEOUT_MS 800
#define EDGE_IO_LOG_REPEAT_MS 60000
#define EDGE_ACQUISITION_MAGIC 0x45414351U
#define EDGE_ACQUISITION_RESTART_MS 5000U
#define EDGE_ACQUISITION_WATCHDOG_MS 120000U
#define EDGE_LINK_EVENT_BUFFER_SIZE 8192U
#define EDGE_MODBUS_TCP_RESYNC_BYTES 1024U
#define EDGE_MODBUS_TCP_MAX_DROPPED_FRAMES 4U

typedef struct edge_acquisition_device edge_acquisition_device;
typedef struct edge_acquisition_link edge_acquisition_link;

typedef struct {
    _Alignas(_Atomic uint32_t) _Atomic uint32_t generation;
} edge_command_clock_generation;

typedef struct edge_acquisition_response {
    size_t references;
    size_t size;
    size_t request_size;
    int64_t observed_at_ms;
    uint8_t packet_id[16];
    uint64_t sequence;
    struct edge_acquisition_response *previous;
    uint8_t bytes[];
} edge_acquisition_response;

typedef struct {
    const iot_edge_v1_ConfigItem *item;
    iot_edge_v1_TelemetryValue value;
    bool valid;
    edge_acquisition_response *response;
} edge_acquisition_point;

struct edge_acquisition_link {
    const iot_edge_v1_EndpointConfig *endpoint;
    uint8_t owner_platform_id[16];
    uint16_t owner_priority;
    bool owner_bootstrap;
    int fd;
    int listen_fd;
    bool fins_ready;
    uint32_t fins_source_node, fins_destination_node;
    uint64_t generation;
};

struct edge_acquisition_device {
    uint8_t acquisition_id[16];
    bool acquisition_active;
    bool raw_telemetry;
    bool external_io_enabled;
    uint8_t configuration_digest[32];
    const iot_edge_v1_ExternalIoPointConfig *external_io[15];
    size_t external_io_count;
    uint64_t external_io_next_ms;
    uint64_t external_io_retry_ms;
    uint8_t *external_io_pending;
    size_t external_io_pending_size;
    uint8_t external_io_pending_id[16];
    bool external_io_unavailable;
    iot_edge_v1_CommandResult *external_io_result;
    uint64_t external_io_result_retry_ms;
    iot_edge_v1_RawPacket *debug_request;
    uint8_t received_packet_id[16];
    int64_t received_packet_time;
    edge_acquisition *owner;
    edge_acquisition_link *link;
    uint8_t platform_id[16];
    const iot_edge_v1_EndpointConfig *endpoint;
    const iot_edge_v1_DeviceConfig *config;
    edge_derived *derived;
    int64_t derived_deadline;
    iot_edge_v1_TelemetryRecord *derived_pending;
    edge_acquisition_point *points;
    size_t point_count;
    edge_modbus_read_point *modbus_points;
    size_t modbus_point_count;
    edge_modbus_read_group *modbus_groups;
    size_t modbus_group_count;
    edge_acquisition_response *read_response;
    uint64_t response_sequence;
    edge_device_runtime runtime;
    edge_sl651_session *sl651;
    uint8_t sl651_station[5];
    bool sl651_transmitter, sl651_query_active;
    bool command_start_check, command_start_expired;
    bool sl651_command_pending;
    uint64_t mapping_valid_until_monotonic_ms;
    uint32_t command_clock_generation;
    bool southbound_command_id_valid;
    uint8_t southbound_command_id[16];
    uint8_t sl651_command_id[16];
    uint64_t command_start_before_monotonic_ms;

    uint8_t *sl651_request;
    size_t sl651_request_size;
    uint64_t sl651_token;
    bool sl651_report_encoded;
    bool sl651_report_raw;
    uint32_t sl651_parts;
    bool sl651_committed[256];
    uint8_t sl651_record_ids[256][16];
    iot_edge_v1_CommandResult sl651_result;
    bool sl651_result_pending;
    uint16_t transaction;
    uint16_t s7_reference;
    uint16_t s7_pdu_length;
    int64_t observed_at_ms;
    int64_t last_activity_at_ms;
    edge_io_result last_io_result;
    int64_t last_io_log_ms;
    int64_t local_io_log_ms[6];
    edge_io_result local_io_result[6];
    bool local_io_seen[6];
    int64_t last_sample_failure_log_ms;
    int64_t last_queue_failure_log_ms;
    int64_t last_report_success_log_ms;
    char last_error[128];
    bool has_last_io_result;
    bool s7_handshake_logged;
    iot_edge_v1_IndustrialConnectionConfig industrial;
    uint64_t industrial_generation;
    bool link_state_known;
    bool link_up;
};

typedef struct {
    bool active, manual;
    int fd;
    uint8_t platform_id[16], id[16];
    uint64_t last_request, event_sequence, expires_ms, dropped_bytes;
    iot_edge_v1_SerialSettings settings;
    uint8_t pending[1024];
    size_t pending_size, pending_offset;
    uint64_t pending_request, pending_deadline;
} edge_serial_session;

typedef struct {
    iot_edge_v1_DtuConfig config;
    uint8_t platform_id[16];
    pid_t pid;
    uint64_t retry_at;
} edge_dtu_channel;

struct edge_acquisition {
    edge_acquisition_sample_callback sample_callback;
    bool gateway_enabled;
    edge_acquisition_dtu_callback dtu_callback;
    edge_acquisition_io_callback io_callback;
    edge_external_io_snapshot io_capability;
    bool io_capability_seen;
    uint64_t io_next_read_ms;
    struct {
        bool pending;
        uint64_t retry_ms;
        iot_edge_v1_CommandResult result;
        edge_device_runtime commands;
    } node_io[4];
    bool io_connected;
    bool io_enabled;
    uint64_t io_reconnect_ms;
    bool io_quiesce_requested;
    bool io_quiesce_sent;
    bool io_quiesced_ack;
    uint64_t io_quiesce_token;
    edge_dtu_channel dtu[EDGE_DTU_MAX_CHANNELS];
    size_t dtu_count;
    edge_acquisition_serial_callback serial_callback;
    char serial_path[97];
    bool serial_rs485;
    edge_serial_session serial_sessions[4];
    edge_acquisition_debug_callback debug;
    edge_acquisition_telemetry_callback telemetry;
    edge_acquisition_command_callback command;
    void *callback_context;
    edge_acquisition_device *devices;
    size_t device_count;
    edge_acquisition_link *links;
    size_t link_count;
    uint8_t platform_ids[4][16];
    edge_command_clock_generation *command_clock_generations;
    iot_edge_v1_DeviceStatusReport cached_status[4];
    size_t platform_count;
    pid_t worker_pid;
    int worker_fd;
    uint64_t worker_restart_at_ms;
    uint64_t worker_last_event_ms;
    bool worker_required;
    bool worker_child;
};

typedef enum {
    EDGE_ACQUISITION_EVENT_SAMPLE = 23,
    EDGE_ACQUISITION_CONTROL_GATEWAY = 24,
    EDGE_ACQUISITION_EVENT_EXTERNAL_IO = 14,
    EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_COMMIT = 15,
    EDGE_ACQUISITION_EVENT_IO_CAPABILITY = 16,
    EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_ENABLE = 17,
    EDGE_ACQUISITION_EVENT_EXTERNAL_IO_COMMAND = 18,
    EDGE_ACQUISITION_CONTROL_EXTERNAL_IO_COMMAND_COMMIT = 19,
    EDGE_ACQUISITION_CONTROL_IO_QUIESCE = 20,
    EDGE_ACQUISITION_CONTROL_IO_RESUME = 21,
    EDGE_ACQUISITION_EVENT_IO_QUIESCED = 22,
    EDGE_ACQUISITION_CONTROL_RAW_TELEMETRY = 13,
    EDGE_ACQUISITION_EVENT_DTU = 12,
    EDGE_ACQUISITION_CONTROL_SERIAL = 10,
    EDGE_ACQUISITION_EVENT_SERIAL = 11,
    EDGE_ACQUISITION_EVENT_DEBUG = 9,
    EDGE_ACQUISITION_EVENT_TELEMETRY = 1,
    EDGE_ACQUISITION_EVENT_COMMAND_RESULT = 2,
    EDGE_ACQUISITION_EVENT_STATUS = 3,
    EDGE_ACQUISITION_CONTROL_COMMAND = 4,
    EDGE_ACQUISITION_CONTROL_STOP = 5,
    EDGE_ACQUISITION_EVENT_SL651 = 6,
    EDGE_ACQUISITION_CONTROL_SL651_COMMIT = 7,
    EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT = 8,
} edge_acquisition_message_type;

typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t payload_size;
    uint64_t report_token;
    uint32_t report_part;
    uint64_t command_start_before_monotonic_ms;
    uint64_t mapping_valid_until_monotonic_ms;
    uint32_t command_clock_generation;
    uint8_t device_id[16];
    uint8_t platform_id[16];
    union {
        uint8_t telemetry[EDGENODE_MAX_WS_MESSAGE];
        iot_edge_v1_CommandResult command_result;
        iot_edge_v1_DeviceStatusReport status;
        iot_edge_v1_CommandRequest command_request;
        iot_edge_v1_SerialDebugRequest serial_request;
        iot_edge_v1_SerialDebugEvent serial_event;
        iot_edge_v1_DtuStatus dtu_status;
        edge_external_io_snapshot io_capability;
        edge_acquisition_sample sample;
    } payload;
} edge_acquisition_message;

static inline void set_error(char *error, size_t size, const char *message) {
    edge_set_error(error, size, message);
}

static inline void copy_text(char *output, size_t capacity, const char *input) {
    edge_copy_text(output, capacity, input);
}

static inline int64_t current_ms(void) {
    return edge_realtime_ms();
}

static inline uint64_t monotonic_milliseconds(void) {
    return edge_monotonic_ms();
}

static inline bool random_bytes(uint8_t *output, size_t size) {
    return edge_random_bytes(output, size);
}

static inline speed_t baud_rate(uint32_t value) {
    return edge_serial_baud_speed(value);
}

static inline void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static inline bool same_id(const void *field, const uint8_t id[16]) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return size == 16U && memcmp((const uint8_t *)field + sizeof(size), id, 16U) == 0;
}

static inline void record_id(uint64_t now_ms, uint8_t output[16]) {
    uint8_t random[10] = {0};
    if (!random_bytes(random, sizeof(random))) {
        for (size_t index = 0; index < sizeof(random); ++index)
            random[index] = (uint8_t)(now_ms >> ((index % 8U) * 8U));
    }
    edge_protocol_uuid_v7(now_ms, random, output);
}

static inline size_t acquisition_message_size(uint32_t payload_size) {
    return offsetof(edge_acquisition_message, payload) + payload_size;
}

static inline void acquisition_init_message(edge_acquisition_message *message,
                                            uint32_t type,
                                            uint32_t payload_size) {
    memset(message, 0, offsetof(edge_acquisition_message, payload));
    message->magic = EDGE_ACQUISITION_MAGIC;
    message->type = type;
    message->payload_size = payload_size;
}

static inline size_t command_clock_platform_index(const edge_acquisition *acquisition,
                                                  const uint8_t platform_id[16]) {
    if (acquisition == NULL || platform_id == NULL)
        return SIZE_MAX;
    for (size_t index = 0U; index < acquisition->platform_count; ++index)
        if (memcmp(acquisition->platform_ids[index], platform_id, 16U) == 0)
            return index;
    return SIZE_MAX;
}

static inline uint32_t command_clock_current_generation(const edge_acquisition *acquisition,
                                                        const uint8_t platform_id[16]) {
    const size_t index = command_clock_platform_index(acquisition, platform_id);
    if (index == SIZE_MAX || acquisition->command_clock_generations == NULL)
        return 0U;
    const edge_command_clock_generation *generation =
        &acquisition->command_clock_generations[index];
    return atomic_load_explicit(&generation->generation, memory_order_seq_cst);
}

static inline bool command_clock_generation_is_current(const edge_acquisition *acquisition,
                                                       const uint8_t platform_id[16],
                                                       uint32_t generation) {
    return generation != 0U &&
        command_clock_current_generation(acquisition, platform_id) == generation;
}

static inline bool command_first_write_is_valid(edge_acquisition_device *device) {
    const uint64_t now = monotonic_milliseconds();
    const uint32_t generation =
        command_clock_current_generation(device->owner, device->platform_id);
    if (device->command_clock_generation == 0U ||
        generation != device->command_clock_generation || now == 0U ||
        now >= device->mapping_valid_until_monotonic_ms ||
        now >= device->command_start_before_monotonic_ms) {
        device->command_start_expired = true;
        return false;
    }
    return true;
}

static inline bool automatic_debug_enabled(const edge_acquisition_device *device) {
    return !device->runtime.silent_background_read;
}

static inline const char *protocol_name(const edge_acquisition_device *device) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651)
        return "SL651";
    return device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 ? "S7" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? "MC" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? "FINS" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? "DLT645" : "Modbus";
}

static inline const char *log_source(const edge_acquisition_device *device) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651)
        return "sl651";
    return device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 ? "s7" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? "mc" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? "fins" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? "dlt645" : "modbus";
}

static inline const char *mode_name(iot_edge_v1_LinkMode mode) {
    switch (mode) {
    case iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER:
        return "tcp-server";
    case iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT:
        return "tcp-client";
    case iot_edge_v1_LinkMode_LINK_MODE_SERIAL:
        return "serial";
    default:
        return "unknown";
    }
}

static inline const char *io_result_name(edge_io_result result) {
    switch (result) {
    case EDGE_IO_OK:
        return "ok";
    case EDGE_IO_NO_RESPONSE:
        return "no-response";
    case EDGE_IO_OFFLINE:
        return "offline";
    case EDGE_IO_PROTOCOL_ERROR:
        return "protocol-error";
    default:
        return "failed";
    }
}

static inline const char *device_label(const edge_acquisition_device *device) {
    if (device->config->device_code[0] != '\0')
        return device->config->device_code;
    if (device->config->name[0] != '\0')
        return device->config->name;
    return "unknown";
}

static inline void scalar_bool(iot_edge_v1_ScalarValue *value, bool input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_BOOL;
    value->which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    value->value.bool_value = input;
}

static inline void scalar_signed(iot_edge_v1_ScalarValue *value, int64_t input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_SIGNED;
    value->which_value = iot_edge_v1_ScalarValue_signed_value_tag;
    value->value.signed_value = input;
}

static inline void scalar_unsigned(iot_edge_v1_ScalarValue *value, uint64_t input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_UNSIGNED;
    value->which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    value->value.unsigned_value = input;
}

static inline void scalar_double(iot_edge_v1_ScalarValue *value, double input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_DOUBLE;
    value->which_value = iot_edge_v1_ScalarValue_double_value_tag;
    value->value.double_value = input;
}

/* Core functions (edge_acquisition.c) */
bool edge_acq_worker_send(edge_acquisition *acquisition, uint32_t type,
                          const uint8_t platform_id[16],
                          const void *payload, uint32_t payload_size);
edge_acquisition_device *edge_acq_find_device(edge_acquisition *acquisition,
                                              const uint8_t platform_id[16],
                                              const uint8_t id[16]);
void edge_acq_publish_gateway_value(edge_acquisition_device *device,
                                    const iot_edge_v1_TelemetryValue *value,
                                    int64_t observed_at_ms);
bool edge_acq_update_derived(edge_acquisition_device *device,
                             const iot_edge_v1_TelemetryValue *values,
                             size_t count, int64_t now);
void edge_acq_acquisition_status_local(edge_acquisition *acquisition,
                                       const uint8_t platform_id[16],
                                       iot_edge_v1_DeviceStatusReport *report);
void edge_acq_log_report_result(edge_acquisition_device *device,
                                edge_local_report_result result, size_t frames);
bool edge_acq_publish_acquisition_cycle(edge_acquisition_device *device,
                                        const uint8_t platform_id[16],
                                        iot_edge_v1_TelemetryRecord *record);
bool edge_acq_publish_derived_expiry(edge_acquisition_device *device);
void edge_acq_drain_worker(edge_acquisition *acquisition, uint64_t now_ms);

/* Transport & Netlink functions (edge_acquisition_transport.c) */
void edge_acq_device_detail(const edge_acquisition_device *device, const char *operation,
                            char *detail, size_t size);
void edge_acq_log_io_result(edge_acquisition_device *device, edge_io_result result,
                            const char *operation);
void edge_acq_log_incomplete_sample(edge_acquisition_device *device);
int edge_acq_open_link_monitor(void);
void edge_acq_drain_link_events(edge_acquisition *acquisition, int link_fd);
void edge_acq_debug_acquisition_state(edge_acquisition_device *device, const char *state);
void edge_acq_finish_debug_acquisition(edge_acquisition_device *device, edge_io_result result);
void edge_acq_debug_packet_with_id(edge_acquisition_device *device, const uint8_t id[16],
                                   const char *direction, const uint8_t *data,
                                   size_t size, bool identified, bool device_only);
void edge_acq_debug_packet(edge_acquisition_device *device, const char *direction,
                           const uint8_t *data, size_t size, bool identified, bool device_only);
void edge_acq_debug_request_status(edge_acquisition_device *device, const char *status,
                                   const char *reason, bool finished);
void edge_acq_debug_request_begin(edge_acquisition_device *device,
                                  const uint8_t *request, size_t size);
int edge_acq_open_serial(const iot_edge_v1_SerialSettings *settings);
bool edge_acq_prepare_serial_task(edge_acquisition_device *device);
edge_io_result edge_acq_device_connect(void *context);
void edge_acq_device_disconnect(void *context);
bool edge_acq_exchange_with_receive_timeout(edge_acquisition_device *device,
                                            const uint8_t *request, size_t request_size,
                                            uint8_t *response, size_t capacity,
                                            size_t *response_size,
                                            bool *receive_timed_out);
bool edge_acq_exchange(edge_acquisition_device *device, const uint8_t *request,
                       size_t request_size, uint8_t *response, size_t capacity,
                       size_t *response_size);
bool edge_acq_write_command_exchange(edge_acquisition_device *device,
                                     const uint8_t *request, size_t request_size,
                                     uint8_t *response, size_t capacity,
                                     size_t *response_size);
edge_io_result edge_acq_failed_command_exchange(const edge_acquisition_device *device);
edge_io_result edge_acq_device_handshake(void *context);

/* Codec & Protocol Read/Write functions (edge_acquisition_codec.c) */
uint8_t edge_acq_modbus_function(const char *type);
void edge_acq_release_response(edge_acquisition_response *response);
void edge_acq_publish_debug_value(edge_acquisition_device *device,
                                  const uint8_t packet_id[16],
                                  const iot_edge_v1_TelemetryValue *value);
void edge_acq_fill_point_value(edge_acquisition_device *device,
                               edge_acquisition_point *point,
                               const uint8_t *raw, size_t size,
                               edge_acquisition_response *response);
edge_acquisition_point *edge_acq_find_point(edge_acquisition_device *device,
                                            const char *element_id);
edge_io_result edge_acq_device_read(void *context, edge_device_sample *sample);
edge_io_result edge_acq_device_write_readback(void *context,
                                              const edge_write_command *command,
                                              edge_device_sample *actual);
bool edge_acq_build_write_command(edge_acquisition *acquisition,
                                  const uint8_t platform_id[16],
                                  const iot_edge_v1_CommandRequest *request,
                                  uint64_t start_before_monotonic_ms,
                                  uint64_t mapping_valid_until_monotonic_ms,
                                  uint32_t command_clock_generation,
                                  edge_acquisition_device **output_device,
                                  edge_write_command *output_command,
                                  char *error, size_t error_size);

/* SL651 functions (edge_acquisition_sl651.c) */
void edge_acq_sl651_time(edge_acquisition_device *device, uint8_t output[6]);
bool edge_acq_sl651_send(void *context, const uint8_t *bytes, size_t size,
                         const uint8_t acquisition_id[16],
                         const uint8_t *reply_to_packet_id);
void edge_acq_sl651_trace(void *context, const uint8_t *bytes, size_t size,
                          uint8_t packet_id[16], uint8_t acquisition_id[16]);
void edge_acq_sl651_diagnostic(void *context, edge_sl651_fault_reason reason);
bool edge_acq_sl651_report(void *context, uint64_t token, const edge_sl651_frame *frame);
void edge_acq_sl651_command_result(void *context, const uint8_t id[16],
                                   bool success, const char *reason);
void edge_acq_sl651_poll(edge_acquisition_device *device, uint64_t now);
bool edge_acq_sl651_query_request(edge_acquisition_device *device,
                                  const iot_edge_v1_CommandRequest *request,
                                  uint64_t start_before_monotonic_ms,
                                  uint64_t mapping_valid_until_monotonic_ms,
                                  uint32_t command_clock_generation,
                                  char *error, size_t error_size);

/* Serial debug functions (edge_acquisition_serial.c) */
bool edge_acq_serial_paused(const edge_acquisition_device *device);
void edge_acq_serial_observe(edge_acquisition *acquisition, const char *path,
                             const iot_edge_v1_SerialSettings *settings,
                             const char *direction, const uint8_t *bytes, size_t size);
void edge_acq_serial_control(edge_acquisition *acquisition, const uint8_t platform_id[16],
                             const iot_edge_v1_SerialDebugRequest *request);
void edge_acq_serial_tick(edge_acquisition *acquisition, uint64_t now);

/* External I/O functions (edge_acquisition_extio.c) */
const iot_edge_v1_ExternalIoPointConfig *edge_acq_external_io_command_point(
    edge_acquisition_device *device, const iot_edge_v1_CommandRequest *request);
size_t edge_acq_node_io_platform_index(const edge_acquisition *acquisition,
                                       const uint8_t platform_id[16]);
bool edge_acq_node_io_request_valid(const edge_acquisition *acquisition,
                                    const iot_edge_v1_CommandRequest *request);
bool edge_acq_external_io_command(edge_acquisition *acquisition,
                                  const edge_acquisition_message *message);
void edge_acq_external_io_tick(edge_acquisition *acquisition, uint64_t now);

/* Short internal aliases */
#define worker_send edge_acq_worker_send
#define find_device edge_acq_find_device
#define publish_gateway_value edge_acq_publish_gateway_value
#define update_derived edge_acq_update_derived
#define acquisition_status_local edge_acq_acquisition_status_local
#define log_report_result edge_acq_log_report_result
#define publish_acquisition_cycle edge_acq_publish_acquisition_cycle
#define publish_derived_expiry edge_acq_publish_derived_expiry
#define drain_worker edge_acq_drain_worker

#define device_detail edge_acq_device_detail
#define log_io_result edge_acq_log_io_result
#define log_incomplete_sample edge_acq_log_incomplete_sample
#define open_link_monitor edge_acq_open_link_monitor
#define drain_link_events edge_acq_drain_link_events
#define debug_acquisition_state edge_acq_debug_acquisition_state
#define finish_debug_acquisition edge_acq_finish_debug_acquisition
#define debug_packet_with_id edge_acq_debug_packet_with_id
#define debug_packet edge_acq_debug_packet
#define debug_request_status edge_acq_debug_request_status
#define debug_request_begin edge_acq_debug_request_begin
#define open_serial edge_acq_open_serial
#define prepare_serial_task edge_acq_prepare_serial_task
#define device_connect edge_acq_device_connect
#define device_disconnect edge_acq_device_disconnect
#define exchange_with_receive_timeout edge_acq_exchange_with_receive_timeout
#define exchange edge_acq_exchange
#define write_command_exchange edge_acq_write_command_exchange
#define failed_command_exchange edge_acq_failed_command_exchange
#define device_handshake edge_acq_device_handshake

#define modbus_function edge_acq_modbus_function
#define release_response edge_acq_release_response
#define publish_debug_value edge_acq_publish_debug_value
#define fill_point_value edge_acq_fill_point_value
#define find_point edge_acq_find_point
#define device_read edge_acq_device_read
#define device_write_readback edge_acq_device_write_readback
#define build_write_command edge_acq_build_write_command

#define sl651_time edge_acq_sl651_time
#define sl651_send edge_acq_sl651_send
#define sl651_trace edge_acq_sl651_trace
#define sl651_diagnostic edge_acq_sl651_diagnostic
#define sl651_report edge_acq_sl651_report
#define sl651_command_result edge_acq_sl651_command_result
#define sl651_poll edge_acq_sl651_poll
#define sl651_query_request edge_acq_sl651_query_request

#define serial_paused edge_acq_serial_paused
#define serial_observe edge_acq_serial_observe
#define serial_control edge_acq_serial_control
#define serial_tick edge_acq_serial_tick

#define external_io_command_point edge_acq_external_io_command_point
#define node_io_platform_index edge_acq_node_io_platform_index
#define node_io_request_valid edge_acq_node_io_request_valid
#define external_io_command edge_acq_external_io_command
#define external_io_tick edge_acq_external_io_tick

#endif
