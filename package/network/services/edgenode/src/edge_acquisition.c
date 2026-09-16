#define _GNU_SOURCE

#include "edge_acquisition.h"

#include <dirent.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

#include "edge_device_runtime.h"
#include "edge_modbus.h"
#include "edge_process.h"
#include "edge_protocol.h"
#include "edge_s7.h"
#include "edge_industrial.h"
#include "edge_sl651.h"
#include "log.h"
#include "pb_decode.h"
#include "pb_encode.h"

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

typedef struct edge_acquisition_response {
    size_t references;
    size_t size;
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
    iot_edge_v1_RawPacket *debug_request;
    uint8_t received_packet_id[16];
    int64_t received_packet_time;
    edge_acquisition *owner;
    edge_acquisition_link *link;
    uint8_t platform_id[16];
    const iot_edge_v1_EndpointConfig *endpoint;
    const iot_edge_v1_DeviceConfig *config;
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
    uint64_t sl651_token;
    bool sl651_report_encoded;
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

struct edge_acquisition {
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
    uint8_t device_id[16];
    uint8_t platform_id[16];
    union {
        uint8_t telemetry[EDGENODE_MAX_WS_MESSAGE];
        iot_edge_v1_CommandResult command_result;
        iot_edge_v1_DeviceStatusReport status;
        iot_edge_v1_CommandRequest command_request;
        iot_edge_v1_SerialDebugRequest serial_request;
        iot_edge_v1_SerialDebugEvent serial_event;
    } payload;
} edge_acquisition_message;

static void serial_observe(edge_acquisition *acquisition, const char *path,
    const iot_edge_v1_SerialSettings *settings, const char *direction,
    const uint8_t *bytes, size_t size);
static bool serial_paused(const edge_acquisition_device *device);
static void serial_control(edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_SerialDebugRequest *request);
static void serial_tick(edge_acquisition *acquisition, uint64_t now);

static bool worker_sl651_report(edge_acquisition_device *device,
                                const iot_edge_v1_TelemetryRecord *record, uint32_t part);

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0U)
        snprintf(error, size, "%s", message);
}

static void copy_text(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    const char *text = input != NULL ? input : "";
    size_t length = strlen(text);
    if (length >= capacity) length = capacity - 1;
    memmove(output, text, length);
    output[length] = '\0';
}

static bool same_id(const void *field, const uint8_t id[16]) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return size == 16U && memcmp((const uint8_t *)field + sizeof(size), id, 16U) == 0;
}

static bool random_bytes(uint8_t *output, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t count = read(fd, output + offset, size - offset);
        if (count <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)count;
    }
    close(fd);
    return true;
}

static void record_id(uint64_t now_ms, uint8_t output[16]) {
    uint8_t random[10] = {0};
    if (!random_bytes(random, sizeof(random))) {
        for (size_t index = 0; index < sizeof(random); ++index)
            random[index] = (uint8_t)(now_ms >> ((index % 8U) * 8U));
    }
    edge_protocol_uuid_v7(now_ms, random, output);
}

static int64_t current_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static const char *protocol_name(const edge_acquisition_device *device) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651)
        return "SL651";
    return device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 ? "S7" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? "MC" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? "FINS" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? "DLT645" : "Modbus";
}

static const char *log_source(const edge_acquisition_device *device) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651)
        return "sl651";
    return device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 ? "s7" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? "mc" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? "fins" :
        device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? "dlt645" : "modbus";
}

static const char *mode_name(iot_edge_v1_LinkMode mode) {
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

static const char *io_result_name(edge_io_result result) {
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

static const char *device_label(const edge_acquisition_device *device) {
    if (device->config->device_code[0] != '\0')
        return device->config->device_code;
    if (device->config->name[0] != '\0')
        return device->config->name;
    return "unknown";
}

static void device_detail(const edge_acquisition_device *device, const char *operation,
                          char *detail, size_t size) {
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL) {
        snprintf(detail, size, "device=%.32s operation=%.16s protocol=%.12s serial=%.64s",
                 device_label(device), operation, protocol_name(device),
                 device->endpoint->serial.channel);
        return;
    }
    snprintf(detail, size,
             "device=%.32s operation=%.16s protocol=%.12s mode=%.12s endpoint=%.40s:%u interface=%.16s",
             device_label(device), operation, protocol_name(device),
             mode_name(device->endpoint->mode), device->endpoint->ip,
             device->endpoint->port, device->endpoint->interface_name);
}

static void log_io_result(edge_acquisition_device *device, edge_io_result result,
                          const char *operation) {
    const int64_t now = current_ms();
    const bool repeat_failure =
        result != EDGE_IO_OK && device->last_io_log_ms > 0 &&
        now - device->last_io_log_ms >= EDGE_IO_LOG_REPEAT_MS;
    if (device->has_last_io_result && device->last_io_result == result &&
        !repeat_failure)
        return;
    char detail[192];
    device_detail(device, operation, detail, sizeof(detail));
    if (result != EDGE_IO_OK && device->last_error[0] != '\0') {
        char extended[256];
        snprintf(extended, sizeof(extended), "%.160s error=%.80s",
                 detail, device->last_error);
        copy_text(detail, sizeof(detail), extended);
    }
    char message[96];
    snprintf(message, sizeof(message), "device io %s: %s",
             result == EDGE_IO_OK ? "ready" : "failed", io_result_name(result));
    edge_log_write(result == EDGE_IO_OK ? "info" : "warn", log_source(device),
                   message, detail);
    device->last_io_result = result;
    device->last_io_log_ms = now;
    device->has_last_io_result = true;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int open_link_monitor(void) {
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_nl address = {
        .nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_LINK,
    };
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool parse_link_event(const struct nlmsghdr *message, char name[IFNAMSIZ],
                             bool *up) {
    if (message == NULL || name == NULL || up == NULL ||
        (message->nlmsg_type != RTM_NEWLINK && message->nlmsg_type != RTM_DELLINK) ||
        message->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
        return false;
    const struct ifinfomsg *info = NLMSG_DATA(message);
    name[0] = '\0';
    bool carrier_known = false;
    bool carrier = false;
    int remaining = (int)message->nlmsg_len - NLMSG_LENGTH(sizeof(*info));
    for (const struct rtattr *attribute = IFLA_RTA(info); RTA_OK(attribute, remaining);
         attribute = RTA_NEXT(attribute, remaining)) {
        if (attribute->rta_type == IFLA_IFNAME) {
            const size_t size = RTA_PAYLOAD(attribute);
            if (size == 0U)
                continue;
            const size_t copy = size < IFNAMSIZ ? size : IFNAMSIZ - 1U;
            memcpy(name, RTA_DATA(attribute), copy);
            name[copy] = '\0';
        } else if (attribute->rta_type == IFLA_CARRIER &&
                   RTA_PAYLOAD(attribute) >= sizeof(uint8_t)) {
            carrier = *(const uint8_t *)RTA_DATA(attribute) != 0U;
            carrier_known = true;
        }
    }
    if (name[0] == '\0' && message->nlmsg_type != RTM_DELLINK &&
        if_indextoname((unsigned)info->ifi_index, name) == NULL)
        return false;
    if (name[0] == '\0')
        return false;
    *up = message->nlmsg_type == RTM_DELLINK
              ? false
              : carrier_known ? carrier : (info->ifi_flags & IFF_RUNNING) != 0;
    return true;
}

static bool link_event_matches_device(const edge_acquisition_device *device,
                                      const char *name) {
    if (device == NULL || name == NULL || name[0] == '\0')
        return false;
    const iot_edge_v1_EndpointConfig *endpoint = device->endpoint;
    if (device->link != NULL &&
        device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER)
        endpoint = device->link->endpoint;
    if (endpoint == NULL || endpoint->interface_name[0] == '\0')
        return false;
    if (strcmp(endpoint->interface_name, name) == 0)
        return true;
    char path[128];
    const int size = snprintf(path, sizeof(path), "/sys/class/net/%s/brif/%s",
                              endpoint->interface_name, name);
    return size > 0 && (size_t)size < sizeof(path) && access(path, F_OK) == 0;
}

static void handle_link_state(edge_acquisition *acquisition, const char *name, bool up) {
    if (acquisition == NULL || name == NULL || name[0] == '\0')
        return;
    for (size_t index = 0U; index < acquisition->device_count; ++index) {
        edge_acquisition_device *device = &acquisition->devices[index];
        if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_ETHERNET ||
            !link_event_matches_device(device, name))
            continue;
        const bool changed = !device->link_state_known || device->link_up != up;
        device->link_state_known = true;
        device->link_up = up;
        if (!changed)
            continue;
        char detail[192];
        device_detail(device, "link", detail, sizeof(detail));
        if (!up) {
            /* A physical link loss is an external transport event, not a
             * Modbus protocol timeout. Reset the stale socket so the next
             * carrier-up event can use the normal connect path. */
            edge_device_runtime_close(&device->runtime);
            close_fd(&device->link->listen_fd);
            copy_text(device->last_error, sizeof(device->last_error),
                      "network link down");
            device->last_io_result = EDGE_IO_OFFLINE;
            device->has_last_io_result = true;
            device->last_io_log_ms = current_ms();
            edge_log_write("warn", "network", "device network link down", detail);
        } else {
            device->last_error[0] = '\0';
            device->has_last_io_result = false;
            edge_log_write("info", "network", "device network link up", detail);
        }
    }
}

static void drain_link_events(edge_acquisition *acquisition, int link_fd) {
    uint8_t buffer[EDGE_LINK_EVENT_BUFFER_SIZE];
    for (;;) {
        const ssize_t size = recv(link_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (size <= 0)
            return;
        int remaining = (int)size;
        for (struct nlmsghdr *message = (struct nlmsghdr *)buffer;
             NLMSG_OK(message, remaining);
             message = NLMSG_NEXT(message, remaining)) {
            char name[IFNAMSIZ] = {0};
            bool up = false;
            if (parse_link_event(message, name, &up))
                handle_link_state(acquisition, name, up);
        }
    }
}

static void close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int wait_fd(int fd, short events) {
    struct pollfd descriptor = {.fd = fd, .events = events};
    for (;;) {
        const int result = poll(&descriptor, 1U, EDGE_IO_TIMEOUT_MS);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return -1;
        return (descriptor.revents & events) != 0 ? 0 : -1;
    }
}

static void debug_acquisition_state(edge_acquisition_device *device, const char *state) {
    if ((!device->endpoint->debug_enabled && !device->config->debug_enabled) || !device->owner->debug) return;
    iot_edge_v1_RawPacket packet = iot_edge_v1_RawPacket_init_zero;
    packet.debug = true;
    packet.acquisition_id.size = 16;
    memcpy(packet.acquisition_id.bytes, device->acquisition_id, 16);
    packet.packet_id.size = 16;
    record_id((uint64_t)current_ms(), packet.packet_id.bytes);
    packet.endpoint_id.size = 16;
    memcpy(packet.endpoint_id.bytes, device->endpoint->endpoint_id.bytes, 16);
    packet.device_id.size = 16;
    memcpy(packet.device_id.bytes, device->config->device_id.bytes, 16);
    packet.observed_at_ms = current_ms();
    copy_text(packet.direction, sizeof(packet.direction), "RX");
    copy_text(packet.acquisition_state, sizeof(packet.acquisition_state), state);
    device->owner->debug(device->owner->callback_context, device->platform_id, &packet);
}

static void finish_debug_acquisition(edge_acquisition_device *device, edge_io_result result) {
    bool partial = false;
    for (size_t index = 0; index < device->point_count; ++index) partial |= device->points[index].valid;
    debug_acquisition_state(device, result == EDGE_IO_OK ? "success" : partial ? "partial" : "failed");
}

static void debug_packet_with_id(edge_acquisition_device *device, const uint8_t id[16], const char *direction,
                         const uint8_t *data, size_t size, bool identified, bool device_only) {
    if ((!device->endpoint->debug_enabled && !device->config->debug_enabled) ||
        !device->owner->debug || !data || !size) return;
    for (size_t offset = 0; offset < size; offset += 4096U) {
        iot_edge_v1_RawPacket packet = iot_edge_v1_RawPacket_init_zero;
        packet.debug = true;
        packet.acquisition_id.size = 16;
        memcpy(packet.acquisition_id.bytes, device->acquisition_id, 16);
        packet.device_only = device_only;
        packet.packet_id.size = 16;
        memcpy(packet.packet_id.bytes, id, 16);
        packet.payload_offset = (uint32_t)offset;
        if (!strcmp(direction, "RX") && device->debug_request) {
            packet.reply_to_packet_id.size = 16;
            memcpy(packet.reply_to_packet_id.bytes, device->debug_request->packet_id.bytes, 16);
        }
        if (!strcmp(direction, "TX") && device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651) {
            bool has_reply = false;
            for (size_t index = 0; index < 16; ++index) has_reply |= device->received_packet_id[index] != 0;
            if (has_reply) {
                packet.reply_to_packet_id.size = 16;
                memcpy(packet.reply_to_packet_id.bytes, device->received_packet_id, 16);
            }
        }
        packet.endpoint_id.size = 16;
        memcpy(packet.endpoint_id.bytes, device->endpoint->endpoint_id.bytes, 16);
        if (identified) { packet.device_id.size = 16; memcpy(packet.device_id.bytes, device->config->device_id.bytes, 16); }
        packet.observed_at_ms = current_ms();
        copy_text(packet.direction, sizeof(packet.direction), direction);
        copy_text(packet.status, sizeof(packet.status), strcmp(direction, "TX") == 0 ? "sent" : "received");
        packet.payload.size = (pb_size_t)((size-offset) > 4096U ? 4096U : (size-offset));
        memcpy(packet.payload.bytes, data+offset, packet.payload.size);
        device->owner->debug(device->owner->callback_context, device->platform_id, &packet);
    }
}

static void debug_packet(edge_acquisition_device *device, const char *direction,
                         const uint8_t *data, size_t size, bool identified, bool device_only) {
    uint8_t id[16]; record_id((uint64_t)current_ms(), id);
    debug_packet_with_id(device, id, direction, data, size, identified, device_only);
}

static void debug_request_status(edge_acquisition_device *device, const char *status,
                                 const char *reason, bool finished) {
    iot_edge_v1_RawPacket *packet = device->debug_request;
    if (!packet) return;
    copy_text(packet->status, sizeof(packet->status), status);
    copy_text(packet->reason, sizeof(packet->reason), reason ? reason : "");
    if (device->owner->debug)
        device->owner->debug(device->owner->callback_context, device->platform_id, packet);
    if (finished) { free(packet); device->debug_request = NULL; }
}

static void debug_request_begin(edge_acquisition_device *device, const uint8_t *request, size_t size) {
    debug_request_status(device, "failed", "request_replaced", true);
    if ((!device->endpoint->debug_enabled && !device->config->debug_enabled) ||
        !device->owner->debug || !size || size > 4096U) return;
    iot_edge_v1_RawPacket *packet = calloc(1, sizeof(*packet));
    if (!packet) return;
    device->debug_request = packet;
    packet->acquisition_id.size = 16;
    memcpy(packet->acquisition_id.bytes, device->acquisition_id, 16);
    packet->debug = true;
    packet->packet_id.size = 16;
    record_id((uint64_t)current_ms(), packet->packet_id.bytes);
    packet->endpoint_id.size = 16;
    memcpy(packet->endpoint_id.bytes, device->config->endpoint_id.bytes, 16);
    packet->device_id.size = 16;
    memcpy(packet->device_id.bytes, device->config->device_id.bytes, 16);
    packet->observed_at_ms = current_ms();
    copy_text(packet->direction, sizeof(packet->direction), "TX");
    packet->payload.size = (pb_size_t)size;
    memcpy(packet->payload.bytes, request, size);
    debug_request_status(device, "sending", "", false);
}

static bool write_all(edge_acquisition_device *device, const uint8_t *data, size_t size) {
    if (serial_paused(device)) return false;
    const int fd = device->link->fd;
    size_t offset = 0U;
    while (offset < size) {
        if (wait_fd(fd, POLLOUT) != 0)
            return false;
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
            serial_observe(device->owner, device->endpoint->serial.channel,
                &device->endpoint->serial, "TX", data + offset, (size_t)count);
        if (!device->debug_request && device->config->protocol != iot_edge_v1_Protocol_PROTOCOL_DLT645)
            debug_packet(device, "TX", data + offset, (size_t)count, true, false);
        offset += (size_t)count;
    }
    return true;
}

static bool tcp_socket_is_broken(int fd) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    for (;;) {
        const int result = poll(&descriptor, 1U, 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return true;
        if ((descriptor.revents & POLLIN) != 0) {
            uint8_t probe = 0U;
            const ssize_t received = recv(fd, &probe, sizeof(probe),
                                          MSG_PEEK | MSG_DONTWAIT);
            if (received == 0)
                return true;
            if (received < 0 && errno != EINTR && errno != EAGAIN &&
                errno != EWOULDBLOCK)
                return true;
        }
        return false;
    }
}

static bool read_exact(edge_acquisition_device *device, uint8_t *data, size_t size) {
    const int fd = device->link->fd;
    size_t offset = 0U;
    while (offset < size) {
        if (wait_fd(fd, POLLIN) != 0)
            return false;
        const ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        /* The completed protocol frame receives one ID in exchange(). */
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
            serial_observe(device->owner, device->endpoint->serial.channel,
                &device->endpoint->serial, "RX", data + offset, (size_t)count);
        offset += (size_t)count;
    }
    return true;
}

static speed_t baud_rate(uint32_t value) {
    switch (value) {
    case 300U: return B300;
    case 600U: return B600;
    case 1200U: return B1200;
    case 2400U: return B2400;
    case 4800U: return B4800;
    case 9600U: return B9600;
    case 19200U: return B19200;
    case 38400U: return B38400;
#ifdef B57600
    case 57600U: return B57600;
#endif
#ifdef B115200
    case 115200U: return B115200;
#endif
#ifdef B230400
    case 230400U: return B230400;
#endif
#ifdef B460800
    case 460800U: return B460800;
#endif
    default: return (speed_t)0;
    }
}

static bool configure_serial(int fd, const iot_edge_v1_SerialSettings *settings) {
    const speed_t speed = baud_rate(settings->baud_rate);
    if (fd < 0 || speed == (speed_t)0)
        return false;
    struct termios options;
    if (tcgetattr(fd, &options) != 0)
        return false;
    cfmakeraw(&options);
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);
    options.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    options.c_cflag |= CLOCAL | CREAD;
    options.c_cflag |= settings->data_bits == 5U ? CS5
                       : settings->data_bits == 6U ? CS6
                       : settings->data_bits == 7U ? CS7 : CS8;
    if (strcmp(settings->parity, "even") == 0)
        options.c_cflag |= PARENB;
    else if (strcmp(settings->parity, "odd") == 0)
        options.c_cflag |= PARENB | PARODD;
    if (settings->stop_bits == 2U)
        options.c_cflag |= CSTOPB;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    /* Every exchange already bounds request completion before the scheduler can
     * select another task. On the mt76x8 RS485 UART, tcdrain() can nevertheless
     * wait forever after a silent slave and stall the acquisition worker. Apply
     * the settings immediately; the request path enforces the Modbus quiet gap. */
    if (tcsetattr(fd, TCSANOW, &options) != 0)
        return false;
#if defined(__linux__) && defined(TIOCSRS485)
    struct serial_rs485 mode;
    memset(&mode, 0, sizeof(mode));
    if (settings->rs485)
        mode.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    (void)ioctl(fd, TIOCSRS485, &mode);
#endif
    (void)tcflush(fd, TCIFLUSH);
    return true;
}

static void wait_serial_quiet(const edge_acquisition_device *device) {
    const iot_edge_v1_SerialSettings *settings = &device->endpoint->serial;
    const uint32_t quiet_us = edge_modbus_rtu_quiet_time_us(
        settings->baud_rate, (uint8_t)settings->data_bits,
        (uint8_t)settings->stop_bits, strcmp(settings->parity, "none") != 0);
    struct timespec remaining = {
        .tv_sec = quiet_us / 1000000U,
        .tv_nsec = (long)(quiet_us % 1000000U) * 1000L,
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

static int open_serial(const iot_edge_v1_SerialSettings *settings) {
    int fd = open(settings->channel, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (!configure_serial(fd, settings)) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool prepare_serial_task(edge_acquisition_device *device) {
    return device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL ||
           configure_serial(device->link->fd, &device->endpoint->serial);
}

static bool socket_address(const char *ip, uint32_t port, struct sockaddr_in *address) {
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &address->sin_addr) == 1;
}

static bool bind_interface(int fd, const char *name, char *error, size_t error_size) {
#ifdef SO_BINDTODEVICE
    if (name != NULL && name[0] != '\0' &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name) + 1U) != 0) {
        set_error(error, error_size, strerror(errno));
        return false;
    }
#else
    (void)fd;
    (void)name;
    (void)error;
    (void)error_size;
#endif
    return true;
}

static int open_tcp_client(edge_acquisition_device *device) {
    const iot_edge_v1_EndpointConfig *endpoint = device->endpoint;
    device->last_error[0] = '\0';
    struct sockaddr_in address;
    if (!socket_address(endpoint->ip, endpoint->port, &address)) {
        set_error(device->last_error, sizeof(device->last_error), "invalid endpoint address");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_error(device->last_error, sizeof(device->last_error), strerror(errno));
        return -1;
    }
    close_on_exec(fd);
    if (!bind_interface(fd, endpoint->interface_name, device->last_error,
                        sizeof(device->last_error))) {
        close(fd);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        set_error(device->last_error, sizeof(device->last_error), strerror(errno));
        close(fd);
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT) != 0) {
            set_error(device->last_error, sizeof(device->last_error),
                      errno == EINPROGRESS ? "connect timeout" : strerror(errno));
            close(fd);
            return -1;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
            set_error(device->last_error, sizeof(device->last_error),
                      error != 0 ? strerror(error) : strerror(errno));
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags) != 0) {
        set_error(device->last_error, sizeof(device->last_error), strerror(errno));
        close(fd);
        return -1;
    }
    device->last_error[0] = '\0';
    return fd;
}

static int accept_tcp_client(edge_acquisition_device *device) {
    if (device->link->listen_fd < 0) {
        struct sockaddr_in address;
        const iot_edge_v1_EndpointConfig *endpoint = device->link->endpoint;
        if (!socket_address(endpoint->ip, endpoint->port, &address))
            return -1;
        device->link->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (device->link->listen_fd < 0)
            return -1;
        close_on_exec(device->link->listen_fd);
        const int flags = fcntl(device->link->listen_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(device->link->listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close_fd(&device->link->listen_fd);
            return -1;
        }
        int enabled = 1;
        (void)setsockopt(device->link->listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                         sizeof(enabled));
        if (!bind_interface(device->link->listen_fd,
                            device->link->endpoint->interface_name, NULL, 0U)) {
            close_fd(&device->link->listen_fd);
            return -1;
        }
        if (bind(device->link->listen_fd, (const struct sockaddr *)&address,
                 sizeof(address)) != 0 || listen(device->link->listen_fd, 1) != 0) {
            close_fd(&device->link->listen_fd);
            return -1;
        }
    }
    int fd = accept(device->link->listen_fd, NULL, NULL);
    if (fd < 0)
        return -1;
    close_on_exec(fd);
    return fd;
}

static edge_io_result device_connect(void *context) {
    edge_acquisition_device *device = context;
    if (serial_paused(device)) return EDGE_IO_OFFLINE;
    if (device->link->fd >= 0)
        return EDGE_IO_OK;
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
        device->endpoint->interface_name[0] != '\0' && device->link_state_known &&
        !device->link_up) {
        copy_text(device->last_error, sizeof(device->last_error), "network link down");
        return EDGE_IO_OFFLINE;
    }
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        device->link->fd = open_serial(&device->endpoint->serial);
    else if (device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER)
        device->link->fd = accept_tcp_client(device);
    else
        device->link->fd = open_tcp_client(device);
    if (device->link->fd < 0) {
        log_io_result(device, EDGE_IO_OFFLINE, "connect");
        return EDGE_IO_OFFLINE;
    }
    device->link->fins_ready = false;
    ++device->link->generation;
    if (device->config->protocol != iot_edge_v1_Protocol_PROTOCOL_S7)
        log_io_result(device, EDGE_IO_OK, "connect");
    return EDGE_IO_OK;
}

static void device_disconnect(void *context) {
    edge_acquisition_device *device = context;
    close_fd(&device->link->fd);
    device->s7_pdu_length = 0U;
    device->s7_handshake_logged = false;
    device->industrial = device->config->industrial;
}

static bool receive_modbus(edge_acquisition_device *device, uint8_t *frame,
                            size_t capacity, size_t *size) {
    if (strcmp(device->config->modbus_mode, "RTU") == 0) {
        if (!read_exact(device, frame, 3U))
            return false;
        size_t remaining;
        if ((frame[1] & 0x80U) != 0U)
            remaining = 2U;
        else if (frame[1] >= 1U && frame[1] <= 4U)
            remaining = (size_t)frame[2] + 2U;
        else
            remaining = 5U;
        if (3U + remaining > capacity ||
            !read_exact(device, frame + 3U, remaining))
            return false;
        *size = 3U + remaining;
        return true;
    }
    /*
     * TCP is a byte stream, and some Modbus gateways send a banner or a
     * registration prefix immediately after accepting a connection.  The
     * 427R, for example, sends "546C502F0145" before its first MBAP frame.
     * Reading the first seven bytes unconditionally used to interpret that
     * prefix as an MBAP header, fail the length check, and close the socket.
     * Keep the TCP session and resynchronise on the next plausible MBAP
     * header instead.
     */
    uint8_t header[7];
    size_t header_size = 0U;
    size_t discarded = 0U;
    while (discarded <= EDGE_MODBUS_TCP_RESYNC_BYTES) {
        uint8_t byte = 0U;
        if (!read_exact(device, &byte, 1U))
            return false;
        if (header_size < sizeof(header))
            header[header_size++] = byte;
        if (header_size < sizeof(header))
            continue;

        const uint16_t protocol_id = (uint16_t)(((uint16_t)header[2] << 8U) |
                                                  header[3]);
        const size_t length = ((size_t)header[4] << 8U) | header[5];
        if (protocol_id == 0U && length >= 2U && length + 6U <= capacity) {
            memcpy(frame, header, sizeof(header));
            if (!read_exact(device, frame + sizeof(header), length - 1U))
                return false;
            *size = length + 6U;
            return true;
        }

        memmove(header, header + 1U, sizeof(header) - 1U);
        header_size = sizeof(header) - 1U;
        ++discarded;
    }
    return false;
}

static bool receive_s7(edge_acquisition_device *device, uint8_t *frame,
                        size_t capacity, size_t *size) {
    if (!read_exact(device, frame, 4U))
        return false;
    const size_t length = ((size_t)frame[2] << 8U) | frame[3];
    if (length < 7U || length > capacity ||
        !read_exact(device, frame + 4U, length - 4U))
        return false;
    *size = length;
    return true;
}

static bool receive_industrial(edge_acquisition_device *device, uint8_t *frame,
                                size_t capacity, size_t *size) {
    size_t header = edge_industrial_header_size(device->config);
    if (capacity < header || !read_exact(device, frame, 1)) return false;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645) {
        unsigned wake = 0;
        while (frame[0] == 0xfe && ++wake <= 4)
            if (!read_exact(device, frame, 1)) return false;
    }
    if (!read_exact(device, frame + 1, header - 1)) return false;
    size_t length = edge_industrial_frame_size(device->config, frame, header);
    if (length < header || length > capacity || !read_exact(device, frame + header, length - header)) return false;
    *size = length; return true;
}

static bool modbus_tcp_response_matches_request(const uint8_t *request,
                                                 size_t request_size,
                                                 const uint8_t *response,
                                                 size_t response_size) {
    if (request == NULL || request_size < 8U || response == NULL || response_size < 8U)
        return false;
    if (response[0] != request[0] || response[1] != request[1] ||
        response[2] != 0U || response[3] != 0U || response[6] != request[6])
        return false;
    const uint8_t expected_function = request[7];
    return response[7] == expected_function ||
           response[7] == (uint8_t)(expected_function | 0x80U);
}

static bool exchange(edge_acquisition_device *device, const uint8_t *request,
                     size_t request_size, uint8_t *response, size_t capacity,
                     size_t *response_size) {
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
        tcp_socket_is_broken(device->link->fd)) {
        close_fd(&device->link->fd);
        copy_text(device->last_error, sizeof(device->last_error),
                  "TCP peer disconnected before request");
        return false;
    }
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL) {
        (void)tcflush(device->link->fd, TCIFLUSH);
        wait_serial_quiet(device);
    } else if (device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
             device->config->heartbeat_payload.size != 0U) {
        for (;;) {
            int available = 0;
            const size_t heartbeat_size = device->config->heartbeat_payload.size;
            if (ioctl(device->link->fd, FIONREAD, &available) != 0 ||
                available < (int)heartbeat_size)
                break;
            uint8_t heartbeat[256];
            const ssize_t peeked = recv(device->link->fd, heartbeat,
                                        heartbeat_size, MSG_PEEK);
            if (peeked != (ssize_t)heartbeat_size ||
                memcmp(heartbeat, device->config->heartbeat_payload.bytes,
                       heartbeat_size) != 0)
                break;
            if (!read_exact(device, heartbeat, heartbeat_size))
                break;
        }
    }
    uint8_t sanitized[EDGE_INDUSTRIAL_MAX_FRAME];
    const uint8_t *logged_request = request;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645) {
        if (request_size > sizeof(sanitized)) return false;
        memcpy(sanitized, request, request_size);
        edge_industrial_redact(device->config->protocol, sanitized, request_size);
        logged_request = sanitized;
    }
    debug_request_begin(device, logged_request, request_size);
    if (!write_all(device, request, request_size)) {
        debug_request_status(device, "failed", "socket_write_failed", true);
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET ||
            device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7) {
            close_fd(&device->link->fd);
            copy_text(device->last_error, sizeof(device->last_error),
                      "TCP write failed");
        }
        return false;
    }
    device->last_activity_at_ms = current_ms();
    edge_log_packet(log_source(device), "tx", device_label(device), logged_request, request_size);
    debug_request_status(device, "waiting", "", false);
    bool received = false;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7) {
        received = receive_s7(device, response, capacity, response_size);
    } else if (edge_industrial_protocol(device->config->protocol)) {
        received = receive_industrial(device, response, capacity, response_size);
    } else {
        unsigned dropped = 0U;
        for (;;) {
            if (!receive_modbus(device, response, capacity, response_size))
                break;
            if (strcmp(device->config->modbus_mode, "RTU") == 0 ||
                modbus_tcp_response_matches_request(request, request_size,
                                                     response, *response_size)) {
                received = true;
                break;
            }
            edge_log_packet(log_source(device), "drop", device_label(device), response,
                            *response_size);
            if (++dropped >= EDGE_MODBUS_TCP_MAX_DROPPED_FRAMES)
                break;
        }
    }
    if (received) {
        const uint8_t *logged_response = response;
        if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645) {
            memcpy(sanitized, response, *response_size);
            edge_industrial_redact(device->config->protocol, sanitized, *response_size);
            logged_response = sanitized;
        }
        record_id((uint64_t)current_ms(), device->received_packet_id);
        device->received_packet_time = current_ms();
        debug_packet_with_id(device, device->received_packet_id, "RX", logged_response, *response_size, true, false);
        device->last_activity_at_ms = current_ms();
        edge_log_packet(log_source(device), "rx", device_label(device), logged_response,
                        *response_size);
    } else if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 ||
               edge_industrial_protocol(device->config->protocol)) {
        close_fd(&device->link->fd);
    }
    if (!received) debug_request_status(device, "failed", "response_timeout_or_connection_closed", true);
    return received;
}

static bool parse_hex16(const char *text, uint16_t *value) {
    if (text == NULL || text[0] == '\0')
        return false;
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0' || parsed > 0xffffUL)
        return false;
    *value = (uint16_t)parsed;
    return true;
}

static edge_io_result handshake_acquisition(void *context) {
    edge_acquisition_device *device = context;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS) {
        device->industrial = device->config->industrial;
        if (device->link->fins_ready) {
            if (device->industrial.fins_source_node &&
                device->industrial.fins_source_node != device->link->fins_source_node) return EDGE_IO_PROTOCOL_ERROR;
            device->industrial.fins_source_node = device->link->fins_source_node;
            if (!device->industrial.fins_destination_node)
                device->industrial.fins_destination_node = device->link->fins_destination_node;
            device->industrial_generation = device->link->generation;
            return EDGE_IO_OK;
        }
        uint8_t request[20], response[24]; size_t size = 0;
        size_t sent = edge_fins_node_request((uint8_t)device->industrial.fins_source_node, request, sizeof(request));
        if (!sent || !exchange(device, request, sent, response, sizeof(response), &size)) return EDGE_IO_NO_RESPONSE;
        if (!edge_fins_node_response(response, size, &device->industrial)) {
            debug_request_status(device, "failed", "fins_node_negotiation_failed", true);
            close_fd(&device->link->fd); return EDGE_IO_OFFLINE;
        }
        device->link->fins_ready = true;
        device->link->fins_source_node = device->industrial.fins_source_node;
        device->link->fins_destination_node = response[23];
        device->industrial_generation = device->link->generation;
        debug_request_status(device, "success", "", true); return EDGE_IO_OK;
    }
    uint16_t local = 0x0100U;
    uint16_t remote = 0U;
    if (strcmp(device->config->s7_connection_mode, "TSAP") == 0) {
        if (!parse_hex16(device->config->s7_local_tsap, &local) ||
            !parse_hex16(device->config->s7_remote_tsap, &remote))
            return EDGE_IO_PROTOCOL_ERROR;
    } else {
        const uint16_t type = strcmp(device->config->s7_connection_type, "OP") == 0 ? 2U
                              : strcmp(device->config->s7_connection_type, "S7_BASIC") == 0
                                  ? 3U : 1U;
        remote = (uint16_t)((type << 8U) | ((device->config->s7_rack & 7U) << 5U) |
                            (device->config->s7_slot & 31U));
    }
    uint8_t request[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    size_t response_size = 0U;
    size_t request_size = edge_s7_build_cotp_connect(local, remote, request, sizeof(request));
    if (request_size == 0U ||
        !exchange(device, request, request_size, response, sizeof(response), &response_size)) {
        log_io_result(device, EDGE_IO_NO_RESPONSE, "s7-cotp");
        return EDGE_IO_NO_RESPONSE;
    }
    if (edge_s7_parse_cotp_confirm(response, response_size) != EDGE_S7_OK) {
        debug_request_status(device, "failed", "s7_cotp_invalid", true);
        log_io_result(device, EDGE_IO_PROTOCOL_ERROR, "s7-cotp");
        return EDGE_IO_PROTOCOL_ERROR;
    }
    debug_request_status(device, "success", "", true);
    const uint16_t reference = ++device->s7_reference;
    request_size = edge_s7_build_setup(reference, EDGE_S7_DEFAULT_PDU_LENGTH,
                                       request, sizeof(request));
    if (request_size == 0U ||
        !exchange(device, request, request_size, response, sizeof(response), &response_size)) {
        log_io_result(device, EDGE_IO_NO_RESPONSE, "s7-setup");
        return EDGE_IO_NO_RESPONSE;
    }
    if (edge_s7_parse_setup(response, response_size, reference,
                            &device->s7_pdu_length) != EDGE_S7_OK) {
        debug_request_status(device, "failed", "s7_setup_invalid", true);
        log_io_result(device, EDGE_IO_PROTOCOL_ERROR, "s7-setup");
        return EDGE_IO_PROTOCOL_ERROR;
    }
    debug_request_status(device, "success", "", true);
    if (!device->s7_handshake_logged) {
        char detail[192];
        device_detail(device, "s7-handshake", detail, sizeof(detail));
        edge_log_write("info", "s7", "s7 handshake succeeded", detail);
        device->s7_handshake_logged = true;
    }
    return EDGE_IO_OK;
}

static edge_io_result device_handshake(void *context) {
    edge_acquisition_device *device = context;
    const bool standalone = !device->acquisition_active;
    if (standalone) {
        record_id((uint64_t)current_ms(), device->acquisition_id);
        device->acquisition_active = true;
        debug_acquisition_state(device, "running");
    }
    const edge_io_result result = handshake_acquisition(context);
    if (standalone) {
        finish_debug_acquisition(device, result);
        device->acquisition_active = false;
    }
    return result;
}

static uint8_t modbus_function(const char *type) {
    if (strcmp(type, "COIL") == 0)
        return 1U;
    if (strcmp(type, "DISCRETE_INPUT") == 0)
        return 2U;
    if (strcmp(type, "HOLDING_REGISTER") == 0)
        return 3U;
    if (strcmp(type, "INPUT_REGISTER") == 0)
        return 4U;
    return 0U;
}

static edge_modbus_transport modbus_transport(const edge_acquisition_device *device) {
    return strcmp(device->config->modbus_mode, "RTU") == 0
               ? EDGE_MODBUS_RTU : EDGE_MODBUS_TCP;
}

static edge_s7_area s7_area(const char *area) {
    if (strcmp(area, "DB") == 0 || strcmp(area, "V") == 0)
        return EDGE_S7_AREA_DB;
    if (strcmp(area, "MK") == 0)
        return EDGE_S7_AREA_FLAGS;
    if (strcmp(area, "PE") == 0)
        return EDGE_S7_AREA_INPUTS;
    if (strcmp(area, "PA") == 0)
        return EDGE_S7_AREA_OUTPUTS;
    if (strcmp(area, "CT") == 0)
        return EDGE_S7_AREA_COUNTER;
    return EDGE_S7_AREA_TIMER;
}

static edge_s7_address s7_address(const iot_edge_v1_S7AreaConfig *point) {
    edge_s7_address address = {.area = s7_area(point->area),
                               .db_number = (uint16_t)(strcmp(point->area, "V") == 0
                                                          ? 1U : point->db_number),
                               .start_byte = point->start,
                               .start_bit = (uint8_t)point->start_bit,
                               .size = (uint16_t)point->size,
                               .bit_access = strcmp(point->data_type, "BOOL") == 0};
    return address;
}

static void release_response(edge_acquisition_response *response) {
    if (response != NULL && --response->references == 0U) {
        release_response(response->previous);
        free(response);
    }
}

static bool retain_read_response(edge_acquisition_device *device,
                                 const uint8_t *bytes, size_t size) {
    if (size == 0U || size > 8192U)
        return false;
    edge_acquisition_response *response = malloc(sizeof(*response) + size);
    if (response == NULL)
        return false;
    response->references = 1U;
    response->previous = NULL;
    response->sequence = ++device->response_sequence;
    response->size = size;
    response->observed_at_ms = device->received_packet_time;
    memcpy(response->packet_id, device->received_packet_id, 16);
    memcpy(response->bytes, bytes, size);
    release_response(device->read_response);
    device->read_response = response;
    return true;
}

static edge_io_result read_industrial_point(edge_acquisition_device *device,
    const iot_edge_v1_IndustrialPointConfig *point, uint8_t *data, size_t capacity, size_t *size) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS &&
        device->industrial_generation != device->link->generation) {
        edge_io_result ready = device_handshake(device);
        if (ready != EDGE_IO_OK) return ready;
    }
    uint8_t request[EDGE_INDUSTRIAL_MAX_FRAME], response[EDGE_INDUSTRIAL_MAX_FRAME];
    size_t wire_size = 0, total = 0;
    for (unsigned sequence = 0; sequence <= 255; ++sequence) {
        size_t request_size = edge_industrial_request(device->config, &device->industrial, point,
            ++device->transaction, (uint8_t)sequence, NULL, 0, request, sizeof(request));
        size_t response_size = 0, count = 0; bool more = false;
        if (!request_size) return EDGE_IO_PROTOCOL_ERROR;
        if (!exchange(device, request, request_size, response, sizeof(response), &response_size)) return EDGE_IO_NO_RESPONSE;
        if (!edge_industrial_response(device->config, request, request_size, response, response_size,
                data + total, capacity - total, &count, &more) || response_size > 4096 - wire_size) {
            debug_request_status(device, "failed", "industrial_read_invalid", true);
            close_fd(&device->link->fd); return EDGE_IO_OFFLINE;
        }
        edge_acquisition_response *previous = sequence ? device->read_response : NULL;
        if (previous) ++previous->references;
        if (!retain_read_response(device, response, response_size)) {
            release_response(previous); return EDGE_IO_PROTOCOL_ERROR;
        }
        device->read_response->previous = previous;
        wire_size += response_size; total += count;
        if (total > edge_industrial_width(point) || (more && total == edge_industrial_width(point))) return EDGE_IO_PROTOCOL_ERROR;
        debug_request_status(device, "success", "", true);
        if (!more) {
            if (total != edge_industrial_width(point)) return EDGE_IO_PROTOCOL_ERROR;
            *size = total; return EDGE_IO_OK;
        }
    }
    return EDGE_IO_PROTOCOL_ERROR;
}

static edge_io_result read_modbus_range(edge_acquisition_device *device,
                                        uint8_t function, uint16_t address,
                                        uint16_t quantity, uint8_t *data,
                                        size_t capacity, size_t *data_size) {
    edge_modbus_request request = {
        .transport = modbus_transport(device),
        .transaction_id = ++device->transaction,
        .unit_id = (uint8_t)device->config->modbus_slave_id,
        .function = function,
        .address = address,
        .quantity = quantity};
    uint8_t output[EDGE_MODBUS_MAX_FRAME];
    uint8_t response[EDGE_MODBUS_MAX_FRAME];
    size_t output_size = 0U;
    size_t response_size = 0U;
    uint8_t exception = 0U;
    if (request.function == 0U ||
        edge_modbus_build_read(&request, output, sizeof(output), &output_size) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return device->link->fd < 0 ? EDGE_IO_OFFLINE : EDGE_IO_NO_RESPONSE;
    const edge_modbus_result parsed = edge_modbus_parse_response(
        &request, response, response_size, NULL, 0U, data, capacity, data_size, &exception);
    debug_request_status(device, parsed == EDGE_MODBUS_OK ? "success" : "failed",
        parsed == EDGE_MODBUS_OK ? "" : "modbus_exception_or_invalid_response", true);
    if (parsed != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!retain_read_response(device, response, response_size))
        return EDGE_IO_PROTOCOL_ERROR;
    return EDGE_IO_OK;
}

static edge_io_result read_modbus_point(edge_acquisition_device *device,
                                         const iot_edge_v1_ModbusRegisterConfig *point,
                                         uint8_t *data, size_t capacity, size_t *data_size) {
    const uint8_t function = modbus_function(point->register_type);
    const edge_io_result result = read_modbus_range(
        device, function, (uint16_t)point->address, (uint16_t)point->quantity,
        data, capacity, data_size);
    if (result == EDGE_IO_OK && (function == 1U || function == 2U) && *data_size != 0U) {
        data[0] = (uint8_t)(data[0] & 1U);
        *data_size = 1U;
    }
    return result;
}

static edge_io_result read_s7_point(edge_acquisition_device *device,
                                     const iot_edge_v1_S7AreaConfig *point,
                                     uint8_t *data, size_t capacity, size_t *data_size) {
    const edge_s7_address address = s7_address(point);
    const uint16_t reference = ++device->s7_reference;
    uint8_t output[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    const size_t output_size = edge_s7_build_read(reference, &address, output, sizeof(output));
    size_t response_size = 0U;
    uint8_t return_code = 0U;
    if (output_size == 0U)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return device->link->fd < 0 ? EDGE_IO_OFFLINE : EDGE_IO_NO_RESPONSE;
    const edge_s7_result result = edge_s7_parse_read(response, response_size, reference,
                                                     data, capacity, data_size, &return_code);
    debug_request_status(device, result == EDGE_S7_OK ? "success" : "failed",
        result == EDGE_S7_OK ? "" : "s7_exception_or_invalid_response", true);
    if (result != EDGE_S7_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!retain_read_response(device, response, response_size))
        return EDGE_IO_PROTOCOL_ERROR;
    if (address.bit_access && *data_size != 0U) {
        data[0] = data[0] != 0U ? 1U : 0U;
        *data_size = 1U;
    }
    return EDGE_IO_OK;
}

static uint16_t be16(const uint8_t *value) {
    return (uint16_t)(((uint16_t)value[0] << 8U) | value[1]);
}

static uint32_t be32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) | value[3];
}

static uint64_t be64(const uint8_t *value) {
    uint64_t output = 0U;
    for (size_t index = 0U; index < 8U; ++index)
        output = (output << 8U) | value[index];
    return output;
}

static void order_bytes(uint8_t *output, const uint8_t *input, size_t size,
                        const char *order) {
    memcpy(output, input, size);
    if (strcmp(order, "LITTLE_ENDIAN") == 0) {
        for (size_t left = 0U, right = size == 0U ? 0U : size - 1U; left < right;
             ++left, --right) {
            const uint8_t temporary = output[left];
            output[left] = output[right];
            output[right] = temporary;
        }
    } else if (strcmp(order, "BIG_ENDIAN_BYTE_SWAP") == 0) {
        for (size_t index = 0U; index + 1U < size; index += 2U) {
            const uint8_t temporary = output[index];
            output[index] = output[index + 1U];
            output[index + 1U] = temporary;
        }
    } else if (strcmp(order, "LITTLE_ENDIAN_BYTE_SWAP") == 0) {
        const size_t words = size / 2U;
        for (size_t left = 0U; left < words / 2U; ++left) {
            const size_t right = words - 1U - left;
            uint8_t temporary[2] = {output[left * 2U], output[left * 2U + 1U]};
            output[left * 2U] = output[right * 2U];
            output[left * 2U + 1U] = output[right * 2U + 1U];
            output[right * 2U] = temporary[0];
            output[right * 2U + 1U] = temporary[1];
        }
    }
}

static double rounded(double value, double scale, int32_t decimals) {
    value *= scale;
    if (decimals >= 0 && decimals <= 9) {
        double factor = 1.0;
        for (int32_t index = 0; index < decimals; ++index)
            factor *= 10.0;
        value = round(value * factor) / factor;
    }
    return value;
}

static void scalar_bool(iot_edge_v1_ScalarValue *value, bool input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_BOOL;
    value->which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    value->value.bool_value = input;
}

static void scalar_signed(iot_edge_v1_ScalarValue *value, int64_t input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_SIGNED;
    value->which_value = iot_edge_v1_ScalarValue_signed_value_tag;
    value->value.signed_value = input;
}

static void scalar_unsigned(iot_edge_v1_ScalarValue *value, uint64_t input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_UNSIGNED;
    value->which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    value->value.unsigned_value = input;
}

static void scalar_double(iot_edge_v1_ScalarValue *value, double input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_DOUBLE;
    value->which_value = iot_edge_v1_ScalarValue_double_value_tag;
    value->value.double_value = input;
}

static bool decode_scalar(const iot_edge_v1_ConfigItem *item, const uint8_t *raw,
                          size_t size, iot_edge_v1_ScalarValue *value) {
    const char *type = NULL;
    double scale = 1.0;
    int32_t decimals = -1;
    uint8_t ordered[EDGE_DEVICE_VALUE_MAX];
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *point = &item->item.modbus_register;
        type = point->data_type;
        scale = point->scale;
        decimals = point->decimals;
        order_bytes(ordered, raw, size, point->byte_order);
        raw = ordered;
    } else if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag) {
        const iot_edge_v1_IndustrialPointConfig *point = &item->item.industrial_point;
        type = point->data_type;
        if (!strcmp(type, "BCD") || !strcmp(type, "BCD_SIGNED") || !strcmp(type, "HEX")) {
            *value = (iot_edge_v1_ScalarValue)iot_edge_v1_ScalarValue_init_zero;
            if (!strcmp(type, "HEX")) {
                if (size != point->length || size > sizeof(value->value.bytes_value.bytes)) return false;
                value->kind = iot_edge_v1_ValueKind_VALUE_BYTES;
                value->which_value = iot_edge_v1_ScalarValue_bytes_value_tag;
                value->value.bytes_value.size = (pb_size_t)size;
                memcpy(value->value.bytes_value.bytes, raw, size);
                return true;
            }
            value->kind = iot_edge_v1_ValueKind_VALUE_DECIMAL;
            value->which_value = iot_edge_v1_ScalarValue_decimal_value_tag;
            return edge_meter_decode(point, raw, size, value->value.decimal_value, sizeof(value->value.decimal_value));
        }
        scale = point->scale; decimals = point->decimals;
        order_bytes(ordered, raw, size, point->byte_order); raw = ordered;
    } else {
        const iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        type = point->data_type;
        scale = point->scale;
        decimals = point->decimals;
    }
    *value = (iot_edge_v1_ScalarValue)iot_edge_v1_ScalarValue_init_zero;
    if (strcmp(type, "BOOL") == 0 && size >= 1U) {
        scalar_bool(value, raw[0] != 0U);
        return true;
    }
    if ((strcmp(type, "UINT8") == 0 || strcmp(type, "BYTE") == 0) && size >= 1U) {
        scalar_unsigned(value, raw[0]);
        return true;
    }
    if (strcmp(type, "INT8") == 0 && size >= 1U) {
        scalar_signed(value, (int8_t)raw[0]);
        return true;
    }
    uint64_t numeric = size >= 8U ? be64(raw) : size >= 4U ? be32(raw)
                               : size >= 2U ? be16(raw) : 0U;
    if (strcmp(type, "INT16") == 0 && size >= 2U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((int16_t)numeric, scale, decimals));
        else
            scalar_signed(value, (int16_t)numeric);
        return true;
    }
    if ((strcmp(type, "UINT16") == 0 || strcmp(type, "WORD") == 0) && size >= 2U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((uint16_t)numeric, scale, decimals));
        else
            scalar_unsigned(value, (uint16_t)numeric);
        return true;
    }
    if (strcmp(type, "INT32") == 0 && size >= 4U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((int32_t)numeric, scale, decimals));
        else
            scalar_signed(value, (int32_t)numeric);
        return true;
    }
    if ((strcmp(type, "UINT32") == 0 || strcmp(type, "DWORD") == 0) && size >= 4U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((uint32_t)numeric, scale, decimals));
        else
            scalar_unsigned(value, (uint32_t)numeric);
        return true;
    }
    if (strcmp(type, "INT64") == 0 && size >= 8U) {
        scalar_signed(value, (int64_t)numeric);
        return true;
    }
    if (strcmp(type, "UINT64") == 0 && size >= 8U) {
        scalar_unsigned(value, numeric);
        return true;
    }
    if ((strcmp(type, "FLOAT") == 0 || strcmp(type, "FLOAT32") == 0 ||
         strcmp(type, "REAL") == 0) && size >= 4U) {
        const uint32_t bits = (uint32_t)numeric;
        float parsed;
        memcpy(&parsed, &bits, sizeof(parsed));
        scalar_double(value, rounded(parsed, scale, decimals));
        return isfinite(value->value.double_value);
    }
    if ((strcmp(type, "DOUBLE") == 0 || strcmp(type, "LREAL") == 0) && size >= 8U) {
        double parsed;
        memcpy(&parsed, &numeric, sizeof(parsed));
        scalar_double(value, rounded(parsed, scale, decimals));
        return isfinite(value->value.double_value);
    }
    if (strcmp(type, "STRING") == 0 && size != 0U) {
        value->kind = iot_edge_v1_ValueKind_VALUE_STRING;
        value->which_value = iot_edge_v1_ScalarValue_string_value_tag;
        const size_t copy = size < sizeof(value->value.string_value) - 1U
                                ? size : sizeof(value->value.string_value) - 1U;
        memcpy(value->value.string_value, raw, copy);
        value->value.string_value[copy] = '\0';
        return true;
    }
    return false;
}

/* Share the decoded value with debug before history policy or outbox work. */
static void publish_debug_value(edge_acquisition_device *device, const uint8_t packet_id[16],
                                const iot_edge_v1_TelemetryValue *value) {
    if ((!device->endpoint->debug_enabled && !device->config->debug_enabled) || !device->owner->debug)
        return;
    iot_edge_v1_RawPacket packet = iot_edge_v1_RawPacket_init_zero;
    packet.debug = true;
    packet.packet_id.size = packet.endpoint_id.size = packet.device_id.size = packet.acquisition_id.size = 16;
    memcpy(packet.packet_id.bytes, packet_id, 16);
    memcpy(packet.endpoint_id.bytes, device->endpoint->endpoint_id.bytes, 16);
    memcpy(packet.device_id.bytes, device->config->device_id.bytes, 16);
    memcpy(packet.acquisition_id.bytes, device->acquisition_id, 16);
    copy_text(packet.direction, sizeof(packet.direction), "RX");
    copy_text(packet.status, sizeof(packet.status), "received");
    packet.observed_at_ms = current_ms();
    packet.has_parsed_value = true;
    packet.parsed_value = *value;
    device->owner->debug(device->owner->callback_context, device->platform_id, &packet);
}

static void fill_point_value(edge_acquisition_device *device, edge_acquisition_point *point, const uint8_t *raw, size_t size,
                              edge_acquisition_response *response) {
    if (response != NULL)
        ++response->references;
    release_response(point->response);
    point->response = response;
    point->value = (iot_edge_v1_TelemetryValue)iot_edge_v1_TelemetryValue_init_zero;
    if (point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *config = &point->item->item.modbus_register;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    } else if (point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag) {
        const iot_edge_v1_IndustrialPointConfig *config = &point->item->item.industrial_point;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    } else {
        const iot_edge_v1_S7AreaConfig *config = &point->item->item.s7_area;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    }
    point->value.has_value = decode_scalar(point->item, raw, size, &point->value.value);
    point->valid = point->value.has_value;
    if (point->valid && response) publish_debug_value(device, response->packet_id, &point->value);
}

static edge_io_result read_acquisition(void *context, edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    if (!prepare_serial_task(device))
        return EDGE_IO_OFFLINE;
    bool any = false;
    for (size_t index = 0U; index < device->point_count; ++index)
        device->points[index].valid = false;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS) {
        for (size_t group_index = 0U; group_index < device->modbus_group_count;
             ++group_index) {
            const edge_modbus_read_group *group = &device->modbus_groups[group_index];
            uint8_t grouped[EDGE_MODBUS_MAX_FRAME];
            size_t grouped_size = 0U;
            const edge_io_result result = read_modbus_range(
                device, group->function, group->address, group->quantity,
                grouped, sizeof(grouped), &grouped_size);
            if (result != EDGE_IO_OK) {
                log_io_result(device, result, "read");
                if (result == EDGE_IO_NO_RESPONSE &&
                    device->endpoint->transport ==
                        iot_edge_v1_Transport_TRANSPORT_SERIAL) {
                    /* A timed-out RS485 transaction can leave the mt76x8 UART
                     * driver unable to drain its transmit state. Close it now
                     * so the runtime reconnects before the next scan cycle
                     * instead of carrying poisoned state forward. */
                    close_fd(&device->link->fd);
                    return EDGE_IO_OFFLINE;
                }
                return result;
            }
            for (size_t point_index = 0U; point_index < device->modbus_point_count;
                 ++point_index) {
                const edge_modbus_read_point *planned = &device->modbus_points[point_index];
                uint8_t raw[EDGE_DEVICE_VALUE_MAX];
                size_t raw_size = 0U;
                if (!edge_modbus_extract_point(group, planned, grouped, grouped_size,
                                                raw, sizeof(raw), &raw_size))
                    continue;
                edge_acquisition_point *point = &device->points[planned->point_index];
                fill_point_value(device, point, raw, raw_size, device->read_response);
                any = any || point->valid;
            }
        }
        log_io_result(device, EDGE_IO_OK, "read");
        sample->bytes[0] = any || device->point_count == 0U ? 1U : 0U;
        sample->size = 1U;
        return EDGE_IO_OK;
    }
    for (size_t index = 0U; index < device->point_count; ++index) {
        edge_acquisition_point *point = &device->points[index];
        uint8_t raw[EDGE_DEVICE_VALUE_MAX];
        size_t raw_size = 0U;
        edge_io_result result = point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
            ? read_industrial_point(device, &point->item->item.industrial_point, raw, sizeof(raw), &raw_size)
            : read_s7_point(device, &point->item->item.s7_area, raw, sizeof(raw), &raw_size);
        if (result != EDGE_IO_OK) {
            log_io_result(device, result, "read");
            return result;
        }
        fill_point_value(device, point, raw, raw_size, device->read_response);
        any = any || point->valid;
    }
    log_io_result(device, EDGE_IO_OK, "read");
    sample->bytes[0] = any || device->point_count == 0U ? 1U : 0U;
    sample->size = 1U;
    return EDGE_IO_OK;
}

static edge_acquisition_point *find_point(edge_acquisition_device *device,
                                           const char *element_id) {
    for (size_t index = 0U; index < device->point_count; ++index) {
        const iot_edge_v1_ConfigItem *item = device->points[index].item;
        const char *current = item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
                                  ? item->item.modbus_register.element_id
                                  : item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                                      ? item->item.industrial_point.element_id : item->item.s7_area.element_id;
        if (strcmp(current, element_id) == 0)
            return &device->points[index];
    }
    return NULL;
}

static bool parse_signed(const char *text, int64_t *output) {
    char *end = NULL;
    errno = 0;
    const long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (int64_t)value;
    return true;
}

static bool parse_unsigned(const char *text, uint64_t *output) {
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (uint64_t)value;
    return true;
}

static bool parse_double_value(const char *text, double *output) {
    char *end = NULL;
    errno = 0;
    const double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value))
        return false;
    *output = value;
    return true;
}

static void put_be(uint8_t *output, uint64_t value, size_t size) {
    for (size_t index = 0U; index < size; ++index)
        output[index] = (uint8_t)(value >> ((size - 1U - index) * 8U));
}

static bool encode_scalar(const iot_edge_v1_ConfigItem *item, const char *text,
                          uint8_t *output, size_t capacity, size_t *size) {
    const char *type;
    const char *order = "BIG_ENDIAN";
    size_t width = 0U;
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        type = item->item.modbus_register.data_type;
        order = item->item.modbus_register.byte_order;
        width = (size_t)item->item.modbus_register.quantity * 2U;
        if (strcmp(type, "BOOL") == 0)
            width = 1U;
    } else if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag) {
        const iot_edge_v1_IndustrialPointConfig *point = &item->item.industrial_point;
        type = point->data_type; order = point->byte_order; width = edge_industrial_width(point);
        if (!strcmp(type, "BCD") || !strcmp(type, "BCD_SIGNED") || !strcmp(type, "HEX"))
            return edge_meter_encode(point, text, output, capacity, size);
    } else {
        type = item->item.s7_area.data_type;
        width = item->item.s7_area.size;
        if (strcmp(type, "BOOL") == 0)
            width = 1U;
    }
    if (width == 0U || width > capacity)
        return false;
    uint8_t canonical[EDGE_DEVICE_VALUE_MAX] = {0};
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0U;
    double decimal = 0.0;
    if (strcmp(type, "BOOL") == 0) {
        if (strcmp(text, "0") != 0 && strcmp(text, "1") != 0)
            return false;
        canonical[0] = text[0] == '1' ? 1U : 0U;
    } else if (strcmp(type, "INT8") == 0 || strcmp(type, "INT16") == 0 ||
               strcmp(type, "INT32") == 0 || strcmp(type, "INT64") == 0) {
        if (!parse_signed(text, &signed_value))
            return false;
        if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag && width < 8U &&
            (signed_value < -(INT64_C(1) << (width * 8U - 1U)) ||
             signed_value > (INT64_C(1) << (width * 8U - 1U)) - 1))
            return false;
        put_be(canonical, (uint64_t)signed_value, width);
    } else if (strcmp(type, "UINT8") == 0 || strcmp(type, "BYTE") == 0 ||
               strcmp(type, "UINT16") == 0 || strcmp(type, "WORD") == 0 ||
               strcmp(type, "UINT32") == 0 || strcmp(type, "DWORD") == 0 ||
               strcmp(type, "UINT64") == 0) {
        if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag &&
            (text[0] < '0' || text[0] > '9'))
            return false;
        if (!parse_unsigned(text, &unsigned_value))
            return false;
        if (item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag && width < 8U &&
            unsigned_value >= (UINT64_C(1) << (width * 8U)))
            return false;
        put_be(canonical, unsigned_value, width);
    } else if (strcmp(type, "FLOAT") == 0 || strcmp(type, "FLOAT32") == 0 ||
               strcmp(type, "REAL") == 0) {
        if (width != 4U || !parse_double_value(text, &decimal))
            return false;
        const float narrowed = (float)decimal;
        if (!isfinite(narrowed))
            return false;
        uint32_t bits;
        memcpy(&bits, &narrowed, sizeof(bits));
        put_be(canonical, bits, width);
    } else if (strcmp(type, "DOUBLE") == 0 || strcmp(type, "LREAL") == 0) {
        if (width != 8U || !parse_double_value(text, &decimal))
            return false;
        uint64_t bits;
        memcpy(&bits, &decimal, sizeof(bits));
        put_be(canonical, bits, width);
    } else if (strcmp(type, "STRING") == 0) {
        const size_t length = strlen(text);
        if (length > width)
            return false;
        memcpy(canonical, text, length);
    } else {
        return false;
    }
    order_bytes(output, canonical, width, order);
    *size = width;
    return true;
}

static edge_io_result write_modbus(edge_acquisition_device *device,
                                    const iot_edge_v1_ModbusRegisterConfig *point,
                                    const edge_write_command *command,
                                    edge_device_sample *actual) {
    const uint8_t read_function = modbus_function(point->register_type);
    edge_modbus_request request = {
        .transport = modbus_transport(device),
        .transaction_id = ++device->transaction,
        .unit_id = (uint8_t)device->config->modbus_slave_id,
        .function = read_function == 1U ? 5U : point->quantity == 1U ? 6U : 16U,
        .address = (uint16_t)point->address,
        .quantity = (uint16_t)point->quantity};
    uint8_t output[EDGE_MODBUS_MAX_FRAME];
    uint8_t response[EDGE_MODBUS_MAX_FRAME];
    size_t output_size = 0U;
    size_t response_size = 0U;
    size_t ignored = 0U;
    uint8_t exception = 0U;
    if (edge_modbus_build_write(&request, command->value, command->value_size,
                                output, sizeof(output), &output_size) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return device->link->fd < 0 ? EDGE_IO_OFFLINE : EDGE_IO_NO_RESPONSE;
    if (edge_modbus_parse_response(&request, response, response_size, command->value,
                                   command->value_size, NULL, 0U, &ignored,
                                   &exception) != EDGE_MODBUS_OK) {
        debug_request_status(device, "failed", "modbus_write_exception", true);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    debug_request_status(device, "success", "", true);
    return read_modbus_point(device, point, actual->bytes, sizeof(actual->bytes),
                              &actual->size);
}

static edge_io_result write_s7(edge_acquisition_device *device,
                                const iot_edge_v1_S7AreaConfig *point,
                                const edge_write_command *command,
                                edge_device_sample *actual) {
    const edge_s7_address address = s7_address(point);
    const uint16_t reference = ++device->s7_reference;
    uint8_t output[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    const size_t output_size = edge_s7_build_write(reference, &address, command->value,
                                                   command->value_size, output,
                                                   sizeof(output));
    size_t response_size = 0U;
    uint8_t return_code = 0U;
    if (output_size == 0U)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    if (edge_s7_parse_write(response, response_size, reference, &return_code) != EDGE_S7_OK) {
        debug_request_status(device, "failed", "s7_write_exception", true);
        return EDGE_IO_PROTOCOL_ERROR;
    }
    debug_request_status(device, "success", "", true);
    return read_s7_point(device, point, actual->bytes, sizeof(actual->bytes), &actual->size);
}

static edge_io_result write_industrial(edge_acquisition_device *device,
    const iot_edge_v1_IndustrialPointConfig *point, const edge_write_command *command, edge_device_sample *actual) {
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS &&
        device->industrial_generation != device->link->generation) {
        edge_io_result ready = device_handshake(device);
        if (ready != EDGE_IO_OK) return ready;
    }
    uint8_t request[EDGE_INDUSTRIAL_MAX_FRAME], response[EDGE_INDUSTRIAL_MAX_FRAME];
    size_t n = edge_industrial_request(device->config, &device->industrial, point, ++device->transaction,
        0, command->value, command->value_size, request, sizeof(request));
    size_t received = 0, ignored = 0; bool more = false;
    if (!n) return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, request, n, response, sizeof(response), &received)) return EDGE_IO_NO_RESPONSE;
    if (!edge_industrial_response(device->config, request, n, response, received, NULL, 0, &ignored, &more)) {
        debug_request_status(device, "failed", "industrial_write_invalid", true);
        close_fd(&device->link->fd); return EDGE_IO_OFFLINE;
    }
    debug_request_status(device, "success", "", true);
    return read_industrial_point(device, point, actual->bytes, sizeof(actual->bytes), &actual->size);
}

static edge_io_result write_acquisition(void *context,
                                             const edge_write_command *command,
                                             edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    if (!prepare_serial_task(device))
        return EDGE_IO_OFFLINE;
    edge_acquisition_point *point = find_point(device, command->element_id);
    if (point == NULL)
        return EDGE_IO_PROTOCOL_ERROR;
    const edge_io_result result = point->item->which_item ==
                                          iot_edge_v1_ConfigItem_modbus_register_tag
                                      ? write_modbus(device,
                                            &point->item->item.modbus_register,
                                            command, actual)
                                      : point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                                          ? write_industrial(device, &point->item->item.industrial_point, command, actual)
                                      : write_s7(device, &point->item->item.s7_area,
                                            command, actual);
    if (result == EDGE_IO_OK)
        fill_point_value(device, point, actual->bytes, actual->size, device->read_response);
    return result;
}

static edge_io_result device_read(void *context, edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    record_id((uint64_t)current_ms(), device->acquisition_id);
    device->acquisition_active = true;
    debug_acquisition_state(device, "running");
    for (size_t index = 0; index < device->point_count; ++index) device->points[index].valid = false;
    const edge_io_result result = read_acquisition(context, sample);
    finish_debug_acquisition(device, result);
    device->acquisition_active = false;
    return result;
}

static edge_io_result device_write_readback(void *context, const edge_write_command *command,
                                           edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    memcpy(device->acquisition_id, command->command_id, 16);
    device->acquisition_active = true;
    debug_acquisition_state(device, "running");
    for (size_t index = 0; index < device->point_count; ++index) device->points[index].valid = false;
    const edge_io_result result = write_acquisition(context, command, actual);
    finish_debug_acquisition(device, result);
    device->acquisition_active = false;
    return result;
}

static void acquisition_status_local(edge_acquisition *acquisition,
                                      const uint8_t platform_id[16],
                                      iot_edge_v1_DeviceStatusReport *report);

static int sl651_timezone(const edge_acquisition_device *device) {
    const char *zone = device->config->timezone;
    unsigned hours = 8, minutes = 0;
    if (strlen(zone) != 6 || (zone[0] != '+' && zone[0] != '-') ||
        sscanf(zone + 1, "%2u:%2u", &hours, &minutes) != 2 || hours > 23 || minutes > 59)
        return 8 * 3600;
    return (zone[0] == '-' ? -1 : 1) * (int)(hours * 3600 + minutes * 60);
}
static void sl651_time(edge_acquisition_device *device, uint8_t output[6]) {
    time_t now = (time_t)(current_ms() / 1000 + sl651_timezone(device));
    struct tm local;
    gmtime_r(&now, &local);
    unsigned values[6] = {(unsigned)(local.tm_year + 1900) % 100,
                          (unsigned)local.tm_mon + 1,
                          (unsigned)local.tm_mday,
                          (unsigned)local.tm_hour,
                          (unsigned)local.tm_min,
                          (unsigned)local.tm_sec};
    for (size_t i = 0; i < 6; ++i)
        output[i] = (uint8_t)((values[i] / 10) * 16 + values[i] % 10);
}
static int64_t sl651_observed(edge_acquisition_device *device, const uint8_t *body) {
    unsigned values[6];
    for (size_t i = 0; i < 6; ++i)
        values[i] = (body[i + 2] >> 4U) * 10U + (body[i + 2] & 15U);
    struct tm local = {.tm_year = (int)values[0] + 100,
                       .tm_mon = (int)values[1] - 1,
                       .tm_mday = (int)values[2],
                       .tm_hour = (int)values[3],
                       .tm_min = (int)values[4],
                       .tm_sec = (int)values[5]};
    return ((int64_t)timegm(&local) - sl651_timezone(device)) * 1000;
}
static bool sl651_send(void *context, const uint8_t *bytes, size_t size,
                       const uint8_t acquisition_id[16], const uint8_t *reply_to_packet_id) {
    edge_acquisition_device *device = context;
    if (serial_paused(device)) return false;
    memcpy(device->acquisition_id, acquisition_id, 16);
    if (reply_to_packet_id) memcpy(device->received_packet_id, reply_to_packet_id, 16);
    else memset(device->received_packet_id, 0, 16);
    if (!device->sl651_transmitter)
        return true;
    if (device->link->fd < 0)
        return false;
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = write(device->link->fd, bytes + offset, size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        debug_packet(device, "TX", bytes + offset, (size_t)n, true, false);
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
            serial_observe(device->owner, device->endpoint->serial.channel,
                &device->endpoint->serial, "TX", bytes + offset, (size_t)n);
        offset += (size_t)n;
    }
    return true;
}
static void sl651_trace(void *context, const uint8_t *bytes, size_t size, uint8_t packet_id[16], uint8_t acquisition_id[16]) {
    edge_acquisition_device *device = context;
    static const uint8_t empty[16] = {0};
    if (!memcmp(acquisition_id, empty, 16)) record_id((uint64_t)current_ms(), acquisition_id);
    memcpy(device->acquisition_id, acquisition_id, 16);
    record_id((uint64_t)current_ms(), packet_id);
    memcpy(device->received_packet_id, packet_id, 16);
    debug_packet_with_id(device, packet_id, "RX", bytes, size, true, false);
}
static bool sl651_value(const iot_edge_v1_Sl651ElementConfig *element, const uint8_t *bytes,
                        size_t size, iot_edge_v1_ScalarValue *value) {
    if (!strcmp(element->encoding, "BCD")) {
        double decoded;
        if (!edge_sl651_bcd(bytes, size, element->digits, &decoded))
            return false;
        scalar_double(value, decoded);
        return true;
    }
    value->kind = iot_edge_v1_ValueKind_VALUE_STRING;
    value->which_value = iot_edge_v1_ScalarValue_string_value_tag;
    char *text = value->value.string_value;
    if (!strcmp(element->encoding, "TIME_YYMMDDHHMMSS")) {
        if (size != 5 && size != 6)
            return false;
        for (size_t i = 0; i < size; ++i)
            if ((bytes[i] & 15) > 9 || (bytes[i] >> 4) > 9)
                return false;
        snprintf(text, sizeof(value->value.string_value), "20%02X-%02X-%02XT%02X:%02X:%02X",
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], size == 6 ? bytes[5] : 0);
        return true;
    }
    if (size * 2 >= sizeof(value->value.string_value)) {
        if (strcmp(element->encoding, "HEX") || size > sizeof(value->value.bytes_value.bytes))
            return false;
        value->kind = iot_edge_v1_ValueKind_VALUE_BYTES;
        value->which_value = iot_edge_v1_ScalarValue_bytes_value_tag;
        value->value.bytes_value.size = (pb_size_t)size;
        memcpy(value->value.bytes_value.bytes, bytes, size);
        return true;
    }
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) {
        text[i * 2] = hex[bytes[i] >> 4];
        text[i * 2 + 1] = hex[bytes[i] & 15];
    }
    text[size * 2] = 0;
    return true;
}
static bool sl651_report(void *context, uint64_t token, const edge_sl651_frame *frame) {
    edge_acquisition_device *device = context;
    if (device->sl651_token != token)
        device->sl651_report_encoded = false;
    char function[3];
    snprintf(function, sizeof(function), "%02X", frame->function);
    bool response = false;
    for (size_t i = 0; i < device->point_count; ++i) {
        const iot_edge_v1_Sl651ElementConfig *element = &device->points[i].item->item.sl651_element;
        if (!strcmp(function, element->function_code) && element->response_element)
            response = true;
    }
    size_t guide_start = 8;
    for (size_t i = 0; i < device->point_count; ++i) {
        const iot_edge_v1_Sl651ElementConfig *element = &device->points[i].item->item.sl651_element;
        if (!strcmp(function, element->function_code) && element->response_element == response &&
            element->fixed_position && element->length &&
            element->byte_offset <= frame->body_size &&
            element->length <= frame->body_size - element->byte_offset &&
            guide_start < element->byte_offset + element->length)
            guide_start = element->byte_offset + element->length;
    }
    iot_edge_v1_TelemetryValue *values =
        calloc(device->point_count ? device->point_count : 1, sizeof(*values));
    if (!values)
        return false;
    size_t count = 0, binary_size = 0;
    for (size_t i = 0; i < device->point_count; ++i) {
        const iot_edge_v1_Sl651ElementConfig *element = &device->points[i].item->item.sl651_element;
        if (strcmp(function, element->function_code) || element->response_element != response)
            continue;
        const uint8_t *bytes;
        size_t size;
        if (!edge_sl651_field(frame->body, frame->body_size, element->fixed_position,
                              element->fixed_position ? element->byte_offset : guide_start,
                              element->guide.bytes, element->guide.size, element->length, &bytes,
                              &size))
            continue;
        iot_edge_v1_TelemetryValue *value = &values[count];
        bool binary =
            !strcmp(element->encoding, "JPEG") ||
            ((!strcmp(element->encoding, "HEX") || !strcmp(element->encoding, "DICT")) && size > 63);
        bool decoded = false;
        if (binary && size <= 8192 && size <= EDGE_SL651_BODY_MAX - binary_size) {
            value->encoded_value = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(size));
            if (value->encoded_value) {
                value->encoded_value->size = (pb_size_t)size;
                memcpy(value->encoded_value->bytes, bytes, size);
                copy_text(value->encoding, sizeof(value->encoding), element->encoding);
                binary_size += size;
                decoded = true;
            }
        } else if (!binary)
            decoded = sl651_value(element, bytes, size, &value->value);
        if (!decoded) {
            for (size_t j = 0; j < count; ++j)
                free(values[j].encoded_value);
            free(values);
            copy_text(device->last_error, sizeof(device->last_error),
                      "SL651 value exceeds telemetry encoding or is invalid");
            return false;
        }
        copy_text(value->element_id, sizeof(value->element_id), element->element_id);
        copy_text(value->name, sizeof(value->name), element->name);
        copy_text(value->unit, sizeof(value->unit), element->unit);
        value->has_value = !binary;
        ++count;
    }
    /* Multipart reports become decodable on the final received frame. */
    const size_t raw_count = edge_sl651_report_frame_count(device->sl651);
    const uint8_t *parsed_packet = raw_count ? edge_sl651_report_packet_id(device->sl651, raw_count - 1) : frame->packet_id;
    if (parsed_packet) for (size_t index = 0; index < count; ++index)
        publish_debug_value(device, parsed_packet, &values[index]);
    size_t starts[129] = {0}, parts = 0;
    for (size_t index = 0; index < count;) {
        if (parts == 128) {
            for (size_t j = 0; j < count; ++j)
                free(values[j].encoded_value);
            free(values);
            return false;
        }
        starts[parts++] = index;
        size_t bytes = 0, first = index;
        while (index < count && index - first < 8) {
            size_t estimated =
                512 + (values[index].encoded_value ? values[index].encoded_value->size : 0);
            if (bytes + estimated > 12288)
                break;
            bytes += estimated;
            ++index;
        }
        starts[parts] = index;
    }
    const size_t value_parts = parts;
    const size_t frame_count = edge_sl651_report_frame_count(device->sl651);
    pb_bytes_array_t **raw_frames = calloc(frame_count, sizeof(*raw_frames));
    pb_bytes_array_t **raw_ids = calloc(frame_count, sizeof(*raw_ids));
    bool queued = false;
    size_t raw_starts[257] = {0}, raw_parts = 0;
    if (frame_count == 0 || raw_frames == NULL || raw_ids == NULL)
        goto report_cleanup;
    size_t raw_bytes = 0;
    for (size_t index = 0; index < frame_count; ++index) {
        const uint8_t *bytes;
        size_t size;
        if (!edge_sl651_report_frame(device->sl651, index, &bytes, &size))
            goto report_cleanup;
        if (raw_bytes && raw_bytes + size + 8 > 12000) {
            raw_starts[++raw_parts] = index;
            raw_bytes = 0;
        }
        if (raw_parts + value_parts >= 256)
            goto report_cleanup;
        raw_frames[index] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(size));
        if (raw_frames[index] == NULL)
            goto report_cleanup;
        raw_ids[index] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(16));
        if (raw_ids[index] == NULL) goto report_cleanup;
        raw_ids[index]->size = 16;
        memcpy(raw_ids[index]->bytes, edge_sl651_report_packet_id(device->sl651, index), 16);
        raw_frames[index]->size = (pb_size_t)size;
        memcpy(raw_frames[index]->bytes, bytes, size);
        raw_bytes += size + 8;
    }
    raw_starts[++raw_parts] = frame_count;
    parts = value_parts + raw_parts;
    if (device->sl651_token != token) {
        device->sl651_token = token;
        device->sl651_parts = (uint32_t)parts;
        memset(device->sl651_committed, 0, sizeof(device->sl651_committed));
        for (size_t i = 0; i < parts; ++i)
            record_id((uint64_t)current_ms(), device->sl651_record_ids[i]);
    }
    device->sl651_report_encoded = true;
    queued = true;
    device->observed_at_ms = sl651_observed(device, frame->body);
    device->last_activity_at_ms = current_ms();
    for (size_t part = 0; part < parts; ++part) {
        if (device->sl651_committed[part])
            continue;
        iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
        edge_protocol_set_bytes(&record.record_id, sizeof(record.record_id.bytes),
                                device->sl651_record_ids[part], 16);
        edge_protocol_set_bytes(&record.report_id, sizeof(record.report_id.bytes),
                                frame->acquisition_id, 16);
        record.part_index = (uint32_t)part;
        record.part_count = (uint32_t)parts;
        edge_protocol_set_bytes(&record.device_id, sizeof(record.device_id.bytes),
                                device->config->device_id.bytes, 16);
        edge_protocol_set_bytes(&record.endpoint_id, sizeof(record.endpoint_id.bytes),
                                device->config->endpoint_id.bytes, 16);
        record.protocol = iot_edge_v1_Protocol_PROTOCOL_SL651;
        record.observed_at_ms = device->observed_at_ms;
        copy_text(record.function_code, sizeof(record.function_code), function);
        copy_text(record.direction, sizeof(record.direction), "UP");
        if (part < value_parts) {
            record.values = values + starts[part];
            record.values_count = (pb_size_t)(starts[part + 1] - starts[part]);
        } else {
            size_t raw_part = part - value_parts;
            record.raw_packet_ids = raw_ids + raw_starts[raw_part];
            record.raw_packet_ids_count = (pb_size_t)(raw_starts[raw_part + 1] - raw_starts[raw_part]);
            record.raw_payloads = raw_frames + raw_starts[raw_part];
            record.raw_payloads_count = (pb_size_t)(raw_starts[raw_part + 1] - raw_starts[raw_part]);
        }
        if (!worker_sl651_report(device, &record, (uint32_t)part))
            queued = false;
    }
report_cleanup:
    if (raw_ids != NULL) { for (size_t i = 0; i < frame_count; ++i) free(raw_ids[i]); free(raw_ids); }
    if (raw_frames != NULL) {
        for (size_t index = 0; index < frame_count; ++index)
            free(raw_frames[index]);
        free(raw_frames);
    }
    for (size_t j = 0; j < count; ++j)
        free(values[j].encoded_value);
    free(values);
    if (queued) {
        memcpy(device->acquisition_id, frame->acquisition_id, 16);
        finish_debug_acquisition(device, EDGE_IO_OK);
    }
    return queued;
}
static void sl651_command_result(void *context, const uint8_t id[16], bool success,
                                 const char *reason) {
    edge_acquisition_device *device = context;
    device->sl651_result = (iot_edge_v1_CommandResult)iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&device->sl651_result.command_id,
                            sizeof(device->sl651_result.command_id.bytes), id, 16);
    edge_protocol_set_bytes(&device->sl651_result.device_id,
                            sizeof(device->sl651_result.device_id.bytes),
                            device->config->device_id.bytes, 16);
    device->sl651_result.state = success ? iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED
                                         : iot_edge_v1_CommandState_COMMAND_STATE_FAILED;
    device->sl651_result.completed_at_ms = current_ms();
    copy_text(device->sl651_result.message, sizeof(device->sl651_result.message), reason);
    if (!success)
        for (size_t i = 0; i < device->owner->device_count; ++i) {
            edge_acquisition_device *other = &device->owner->devices[i];
            if (other->sl651 && other->link == device->link &&
                !memcmp(other->sl651_station, device->sl651_station, 5))
                edge_sl651_quarantine(other->sl651);
        }
    device->sl651_result_pending = true;
    (void)device->owner->command(device->owner->callback_context, device->platform_id,
                                 &device->sl651_result);
}
static void sl651_poll(edge_acquisition_device *device, uint64_t now) {
    uint8_t time[6];
    sl651_time(device, time);
    edge_sl651_tick(device->sl651, now, time);
    if (device->sl651_result_pending)
        (void)device->owner->command(device->owner->callback_context, device->platform_id,
                                     &device->sl651_result);
    /* A shared physical stream is read once and broadcast to its configured station sessions. */
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (other == device)
            break;
        if (other->sl651 && other->link == device->link)
            return;
    }
    for (size_t i = 0; i < device->owner->device_count; ++i)
        if (device->owner->devices[i].link == device->link && device->owner->devices[i].sl651 &&
            !edge_sl651_ready(device->owner->devices[i].sl651))
            return;
    if (device_connect(device) != EDGE_IO_OK)
        return;
    struct pollfd descriptor = {.fd = device->link->fd, .events = POLLIN};
    if (poll(&descriptor, 1, 0) <= 0)
        return;
    uint8_t bytes[4096];
    ssize_t n = read(device->link->fd, bytes, sizeof(bytes));
    if (n > 0 && device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        serial_observe(device->owner, device->endpoint->serial.channel,
            &device->endpoint->serial, "RX", bytes, (size_t)n);
    if (n <= 0) {
        close_fd(&device->link->fd);
        for (size_t i = 0; i < device->owner->device_count; ++i)
            if (device->owner->devices[i].sl651 && device->owner->devices[i].link == device->link)
                edge_sl651_reset(device->owner->devices[i].sl651);
        return;
    }
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (!other->sl651 || other->link != device->link)
            continue;
        bool first = true;
        for (size_t j = 0; j < i; ++j) {
            edge_acquisition_device *previous = &device->owner->devices[j];
            if (previous->link == other->link && !memcmp(previous->platform_id, other->platform_id, 16)) first = false;
        }
        bool framed = (size_t)n >= 17 && bytes[0] == 0x7e && bytes[1] == 0x7e &&
            ((((size_t)bytes[11] & 15U) << 8U) + bytes[12] + 17U == (size_t)n);
        bool identified = false;
        if (framed) for (size_t j = 0; j < device->owner->device_count; ++j) {
            const edge_acquisition_device *candidate = &device->owner->devices[j];
            if (candidate->sl651 && candidate->link == other->link &&
                !memcmp(candidate->platform_id, other->platform_id, 16) &&
                !memcmp(candidate->sl651_station, bytes + 3, 5)) identified = true;
        }
        if (first && other->endpoint->debug_enabled && !identified)
            debug_packet(other, "RX", bytes, (size_t)n, false, false);
        sl651_time(other, time);
        edge_sl651_receive(other->sl651, bytes, (size_t)n, now, time);
    }
}

/* Upload chunks share one logical report ID; the platform commits only after
 * every chunk is present. Small cycles use one record directly. */
static bool publish_acquisition_cycle(edge_acquisition_device *device,
                                       const uint8_t platform_id[16],
                                       iot_edge_v1_TelemetryRecord *record) {
    size_t encoded = 0;
    if (!pb_get_encoded_size(&encoded, iot_edge_v1_TelemetryRecord_fields, record))
        return false;
    if (encoded <= 14000)
        return device->owner->telemetry(device->owner->callback_context, platform_id, record);
    const size_t value_parts = (record->values_count + 7U) / 8U;
    const size_t raw_parts = (record->raw_payloads_count + 1U) / 2U;
    const size_t parts = value_parts + raw_parts;
    if (parts > 256U) return false;
    for (size_t index = 0; index < parts; ++index) {
        iot_edge_v1_TelemetryRecord part = *record;
        edge_protocol_set_bytes(&part.report_id, sizeof(part.report_id.bytes), record->record_id.bytes, 16);
        uint8_t id[16]; record_id((uint64_t)current_ms(), id);
        edge_protocol_set_bytes(&part.record_id, sizeof(part.record_id.bytes), id, 16);
        part.part_index = (uint32_t)index;
        part.part_count = (uint32_t)parts;
        part.values_count = part.raw_payloads_count = part.raw_packet_ids_count = 0;
        part.values = NULL; part.raw_payloads = NULL; part.raw_packet_ids = NULL;
        if (index < value_parts) {
            const size_t offset = index * 8U;
            part.values = record->values + offset;
            part.values_count = (pb_size_t)(record->values_count - offset);
            if (part.values_count > 8U) part.values_count = 8U;
        } else {
            const size_t offset = (index - value_parts) * 2U;
            part.raw_payloads = record->raw_payloads + offset;
            part.raw_packet_ids = record->raw_packet_ids + offset;
            part.raw_payloads_count = (pb_size_t)(record->raw_payloads_count - offset);
            if (part.raw_payloads_count > 2U) part.raw_payloads_count = 2U;
            part.raw_packet_ids_count = part.raw_payloads_count;
        }
        if (!pb_get_encoded_size(&encoded, iot_edge_v1_TelemetryRecord_fields, &part) || encoded > 14000 ||
            !device->owner->telemetry(device->owner->callback_context, platform_id, &part)) return false;
    }
    return true;
}

static void device_report(void *context, const uint8_t platform_id[16],
                          const uint8_t device_id[16], const edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    (void)sample;
    iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
    const size_t capacity = device->point_count;
    size_t raw_capacity = 0;
    for (size_t index = 0; index < capacity; ++index)
        if (device->points[index].valid)
            for (const edge_acquisition_response *part = device->points[index].response; part; part = part->previous)
                ++raw_capacity;
    if (raw_capacity > 512) { syslog(LOG_ERR, "acquisition cycle exceeds raw packet limit"); return; }
    record.values = calloc(capacity, sizeof(*record.values));
    record.raw_payloads = calloc(raw_capacity, sizeof(*record.raw_payloads));
    record.raw_packet_ids = calloc(raw_capacity, sizeof(*record.raw_packet_ids));
    const edge_acquisition_response **responses = calloc(raw_capacity, sizeof(*responses));
    if (!record.values || !record.raw_payloads || !record.raw_packet_ids || !responses) goto cleanup;
    for (size_t index = 0; index < capacity; ++index) {
        const edge_acquisition_point *point = &device->points[index];
        if (!point->valid || !point->response) continue;
        record.values[record.values_count++] = point->value;
        for (const edge_acquisition_response *part = point->response; part; part = part->previous) {
            size_t raw = 0;
            while (raw < record.raw_payloads_count && responses[raw] != part) ++raw;
            if (raw < record.raw_payloads_count) continue;
            responses[raw] = part;
            ++record.raw_payloads_count;
        }
    }
    /* Point definitions may be in a different order from the read plan. */
    for (size_t i = 1; i < record.raw_payloads_count; ++i) {
        const edge_acquisition_response *response = responses[i];
        size_t j = i;
        while (j > 0 && responses[j-1]->sequence > response->sequence) {
            responses[j] = responses[j-1]; --j;
        }
        responses[j] = response;
    }
    for (size_t raw = 0; raw < record.raw_payloads_count; ++raw) {
        const edge_acquisition_response *response = responses[raw];
        record.raw_payloads[raw] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(response->size));
        record.raw_packet_ids[raw] = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(16));
        if (!record.raw_payloads[raw] || !record.raw_packet_ids[raw]) goto cleanup;
        record.raw_payloads[raw]->size = (pb_size_t)response->size;
        memcpy(record.raw_payloads[raw]->bytes, response->bytes, response->size);
        record.raw_packet_ids[raw]->size = 16;
        memcpy(record.raw_packet_ids[raw]->bytes, response->packet_id, 16);
    }
    record.raw_packet_ids_count = record.raw_payloads_count;
    if (!record.values_count || !record.raw_payloads_count) goto cleanup;
    record.observed_at_ms = responses[0]->observed_at_ms;
    device->observed_at_ms = responses[record.raw_payloads_count-1]->observed_at_ms;
    edge_protocol_set_bytes(&record.record_id, sizeof(record.record_id.bytes), device->acquisition_id, 16);
    edge_protocol_set_bytes(&record.device_id, sizeof(record.device_id.bytes), device->config->device_id.bytes, 16);
    edge_protocol_set_bytes(&record.endpoint_id, sizeof(record.endpoint_id.bytes), device->config->endpoint_id.bytes, 16);
    record.protocol = device->config->protocol;
    copy_text(record.function_code, sizeof(record.function_code), "POLL");
    copy_text(record.function_name, sizeof(record.function_name), "瀹氭椂閲囬泦");
    copy_text(record.direction, sizeof(record.direction), "UP");
    iot_edge_v1_DeviceStatusReport status = iot_edge_v1_DeviceStatusReport_init_zero;
    acquisition_status_local(device->owner, platform_id, &status);
    for (pb_size_t i = 0; i < status.devices_count; ++i)
        if (memcmp(status.devices[i].device_id.bytes, device_id, 16) == 0) {
            record.has_device_status = true; record.device_status = status.devices[i]; break;
        }
    if (!publish_acquisition_cycle(device, platform_id, &record))
        syslog(LOG_ERR, "cannot queue complete acquisition cycle");
cleanup:
    for (size_t index = 0; index < raw_capacity; ++index) {
        if (record.raw_payloads) free(record.raw_payloads[index]);
        if (record.raw_packet_ids) free(record.raw_packet_ids[index]);
    }
    free(responses); free(record.raw_payloads); free(record.raw_packet_ids); free(record.values);
}

static iot_edge_v1_CommandState command_state(edge_command_result result) {
    switch (result) {
    case EDGE_COMMAND_SUCCEEDED:
        return iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
    case EDGE_COMMAND_READBACK_MISMATCH:
        return iot_edge_v1_CommandState_COMMAND_STATE_READBACK_MISMATCH;
    case EDGE_COMMAND_DEVICE_OFFLINE:
        return iot_edge_v1_CommandState_COMMAND_STATE_DEVICE_OFFLINE;
    case EDGE_COMMAND_TIMED_OUT:
        return iot_edge_v1_CommandState_COMMAND_STATE_TIMED_OUT;
    default:
        return iot_edge_v1_CommandState_COMMAND_STATE_FAILED;
    }
}

static void device_command_complete(void *context, const uint8_t platform_id[16],
                                    const uint8_t device_id[16],
                                    const uint8_t command_id[16],
                                    edge_command_result result,
                                    const edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    (void)device_id;
    iot_edge_v1_CommandResult output = iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&output.command_id, sizeof(output.command_id.bytes),
                            command_id, 16U);
    edge_protocol_set_bytes(&output.device_id, sizeof(output.device_id.bytes),
                            device->config->device_id.bytes,
                            device->config->device_id.size);
    output.state = command_state(result);
    output.completed_at_ms = current_ms();
    copy_text(output.message, sizeof(output.message),
              result == EDGE_COMMAND_SUCCEEDED ? "write and readback succeeded"
              : result == EDGE_COMMAND_READBACK_MISMATCH ? "write readback mismatch"
              : result == EDGE_COMMAND_DEVICE_OFFLINE ? "device offline"
              : result == EDGE_COMMAND_TIMED_OUT ? "device response timed out"
                                                 : "device command failed");
    const edge_write_command *command = &device->runtime.writes[device->runtime.write_head];
    edge_acquisition_point *point = find_point(device, command->element_id);
    if (point != NULL && actual != NULL && actual->size != 0U) {
        fill_point_value(device, point, actual->bytes, actual->size, device->read_response);
        if (point->valid) {
            output.actual_values[0] = point->value;
            output.actual_values_count = 1U;
        }
    }
    (void)device->owner->command(device->owner->callback_context,
                                 platform_id, &output);
}

static const edge_device_driver kDriver = {
    .connect = device_connect,
    .handshake = device_handshake,
    .read = device_read,
    .write_readback = device_write_readback,
    .disconnect = device_disconnect,
    .report = device_report,
    .command_complete = device_command_complete};

static void free_devices(edge_acquisition_device *devices, size_t count) {
    if (devices == NULL)
        return;
    for (size_t index = 0U; index < count; ++index) {
        if (devices[index].sl651)
            edge_sl651_destroy(devices[index].sl651);
        else
            edge_device_runtime_close(&devices[index].runtime);
        for (size_t point = 0U; devices[index].points != NULL &&
                                point < devices[index].point_count; ++point)
            release_response(devices[index].points[point].response);
        release_response(devices[index].read_response);
        free(devices[index].debug_request);
        free(devices[index].points);
        free(devices[index].modbus_points);
        free(devices[index].modbus_groups);
    }
    free(devices);
}

static void free_links(edge_acquisition_link *links, size_t count) {
    if (links == NULL)
        return;
    for (size_t index = 0U; index < count; ++index) {
        close_fd(&links[index].fd);
        close_fd(&links[index].listen_fd);
    }
    free(links);
}

void edge_acquisition_set_debug_callback(edge_acquisition *acquisition, edge_acquisition_debug_callback callback) {
    if (acquisition) acquisition->debug = callback;
}

void edge_acquisition_enable_serial_debug(edge_acquisition *acquisition, const char *path,
    bool rs485, edge_acquisition_serial_callback callback) {
    if (!acquisition) return;
    copy_text(acquisition->serial_path, sizeof(acquisition->serial_path), path);
    acquisition->serial_rs485 = rs485;
    acquisition->serial_callback = callback;
    for (size_t i = 0; i < 4; ++i) acquisition->serial_sessions[i].fd = -1;
    if (path && path[0]) acquisition->worker_required = true;
}

edge_acquisition *edge_acquisition_create(
    edge_acquisition_telemetry_callback telemetry,
    edge_acquisition_command_callback command, void *callback_context) {
    if (telemetry == NULL || command == NULL)
        return NULL;
    edge_acquisition *value = calloc(1U, sizeof(*value));
    if (value != NULL) {
        value->telemetry = telemetry;
        value->command = command;
        value->callback_context = callback_context;
        value->worker_fd = -1;
        for (size_t index = 0U; index < 4U; ++index)
            value->cached_status[index] =
                (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    }
    return value;
}

static bool point_for_device(const iot_edge_v1_ConfigItem *item,
                             const iot_edge_v1_DeviceConfig *device) {
    if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651)
        return item->which_item == iot_edge_v1_ConfigItem_sl651_element_tag &&
               same_id(&item->item.sl651_element.device_id, device->device_id.bytes);
    if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS)
        return item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag &&
               same_id(&item->item.modbus_register.device_id, device->device_id.bytes);
    if (edge_industrial_protocol(device->protocol))
        return item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag &&
               same_id(&item->item.industrial_point.device_id, device->device_id.bytes);
    return item->which_item == iot_edge_v1_ConfigItem_s7_area_tag &&
           same_id(&item->item.s7_area.device_id, device->device_id.bytes);
}

static bool source_precedes(const edge_acquisition_source *left,
                            const edge_acquisition_source *right) {
    if (left->priority != right->priority)
        return left->priority < right->priority;
    if (left->bootstrap != right->bootstrap)
        return left->bootstrap;
    return memcmp(left->platform_id, right->platform_id, 16U) < 0;
}

static bool shared_resource(const iot_edge_v1_EndpointConfig *left,
                            const iot_edge_v1_EndpointConfig *right) {
    if (left->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL &&
        right->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        return strcmp(left->serial.channel, right->serial.channel) == 0;
    return left->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
           right->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
           left->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
           right->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
           left->port == right->port;
}

static edge_acquisition_link *assign_link(
    edge_acquisition_link *links, size_t *link_count,
    const iot_edge_v1_EndpointConfig *endpoint,
    const edge_acquisition_source *source) {
    const bool shareable = endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL ||
                           endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    if (shareable) {
        for (size_t index = 0U; index < *link_count; ++index) {
            edge_acquisition_link *link = &links[index];
            if (!shared_resource(link->endpoint, endpoint))
                continue;
            edge_acquisition_source owner = {
                .platform_id = link->owner_platform_id,
                .priority = link->owner_priority,
                .bootstrap = link->owner_bootstrap,
            };
            if (source_precedes(source, &owner)) {
                link->endpoint = endpoint;
                memcpy(link->owner_platform_id, source->platform_id, 16U);
                link->owner_priority = source->priority;
                link->owner_bootstrap = source->bootstrap;
            }
            return link;
        }
    }
    edge_acquisition_link *link = &links[(*link_count)++];
    memset(link, 0, sizeof(*link));
    link->endpoint = endpoint;
    memcpy(link->owner_platform_id, source->platform_id, 16U);
    link->owner_priority = source->priority;
    link->owner_bootstrap = source->bootstrap;
    link->fd = -1;
    link->listen_fd = -1;
    return link;
}

bool edge_acquisition_apply_multi(edge_acquisition *acquisition,
                                  const edge_acquisition_source *sources,
                                  size_t source_count, uint64_t now_ms,
                                  char *error, size_t error_size) {
    if (acquisition == NULL || sources == NULL || source_count == 0U ||
        source_count > 4U || acquisition->worker_pid > 0) {
        set_error(error, error_size, "invalid acquisition configuration");
        return false;
    }
    size_t enabled = 0U;
    for (size_t source_index = 0U; source_index < source_count; ++source_index) {
        if (sources[source_index].platform_id == NULL ||
            sources[source_index].config == NULL) {
            set_error(error, error_size, "invalid platform acquisition source");
            return false;
        }
        const edge_runtime_config *config = sources[source_index].config;
        for (uint32_t index = 0U; index < config->item_count; ++index)
            if (config->items[index].which_item == iot_edge_v1_ConfigItem_device_tag &&
                config->items[index].item.device.enabled)
                ++enabled;
    }
    edge_acquisition_device *devices = enabled != 0U ? calloc(enabled, sizeof(*devices)) : NULL;
    edge_acquisition_link *links = enabled != 0U ? calloc(enabled, sizeof(*links)) : NULL;
    if (enabled != 0U && (devices == NULL || links == NULL)) {
        free(devices);
        free(links);
        set_error(error, error_size, "acquisition memory allocation failed");
        return false;
    }
    size_t order[4] = {0U, 1U, 2U, 3U};
    for (size_t index = 1U; index < source_count; ++index) {
        const size_t value = order[index];
        size_t position = index;
        while (position != 0U &&
               source_precedes(&sources[value], &sources[order[position - 1U]])) {
            order[position] = order[position - 1U];
            --position;
        }
        order[position] = value;
    }
    size_t output = 0U;
    size_t link_count = 0U;
    for (size_t source_position = 0U; source_position < source_count; ++source_position) {
        const edge_acquisition_source *source = &sources[order[source_position]];
        const edge_runtime_config *config = source->config;
        for (uint32_t index = 0U; index < config->item_count; ++index) {
            const iot_edge_v1_ConfigItem *item = &config->items[index];
            if (item->which_item != iot_edge_v1_ConfigItem_device_tag ||
                !item->item.device.enabled)
                continue;
            const iot_edge_v1_DeviceConfig *device = &item->item.device;
            const iot_edge_v1_EndpointConfig *endpoint =
                edge_runtime_config_endpoint(config, device->endpoint_id.bytes);
            if (endpoint == NULL || !endpoint->enabled ||
                (device->protocol != iot_edge_v1_Protocol_PROTOCOL_MODBUS &&
                 device->protocol != iot_edge_v1_Protocol_PROTOCOL_S7 &&
                 device->protocol != iot_edge_v1_Protocol_PROTOCOL_SL651 && !edge_industrial_protocol(device->protocol))) {
                free_devices(devices, output);
                free_links(links, link_count);
                set_error(error, error_size,
                          "endpoint protocol is not supported");
                return false;
            }
            if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 &&
                (endpoint->transport != iot_edge_v1_Transport_TRANSPORT_ETHERNET ||
                 endpoint->mode != iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT)) {
                free_devices(devices, output);
                free_links(links, link_count);
                set_error(error, error_size, "S7 requires TCP Client endpoint");
                return false;
            }
            edge_acquisition_device *runtime = &devices[output];
            runtime->owner = acquisition;
            runtime->endpoint = endpoint;
            runtime->config = device;
            runtime->industrial = device->industrial;
            memcpy(runtime->platform_id, source->platform_id, 16U);
            runtime->link = assign_link(links, &link_count, endpoint, source);
            for (uint32_t point = 0U; point < config->item_count; ++point)
                if (point_for_device(&config->items[point], device))
                    ++runtime->point_count;
            if (runtime->point_count != 0U) {
                runtime->points = calloc(runtime->point_count,
                                         sizeof(*runtime->points));
                if (runtime->points == NULL) {
                    free_devices(devices, output + 1U);
                    free_links(links, link_count);
                    set_error(error, error_size, "point memory allocation failed");
                    return false;
                }
                size_t point_output = 0U;
                for (uint32_t point = 0U; point < config->item_count; ++point)
                    if (point_for_device(&config->items[point], device))
                        runtime->points[point_output++].item = &config->items[point];
                if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS) {
                    runtime->modbus_points = calloc(runtime->point_count,
                                                    sizeof(*runtime->modbus_points));
                    runtime->modbus_groups = calloc(runtime->point_count,
                                                    sizeof(*runtime->modbus_groups));
                    if (runtime->modbus_points == NULL || runtime->modbus_groups == NULL) {
                        free_devices(devices, output + 1U);
                        free_links(links, link_count);
                        set_error(error, error_size, "Modbus plan memory allocation failed");
                        return false;
                    }
                    runtime->modbus_point_count = runtime->point_count;
                    for (size_t point = 0U; point < runtime->point_count; ++point) {
                        const iot_edge_v1_ModbusRegisterConfig *register_config =
                            &runtime->points[point].item->item.modbus_register;
                        runtime->modbus_points[point] = (edge_modbus_read_point){
                            .point_index = point,
                            .function = modbus_function(register_config->register_type),
                            .address = (uint16_t)register_config->address,
                            .quantity = (uint16_t)register_config->quantity,
                        };
                    }
                    if (!edge_modbus_plan_reads(
                            runtime->modbus_points, runtime->modbus_point_count,
                            device->modbus_merge_gap, device->modbus_max_quantity,
                            runtime->modbus_groups, runtime->point_count,
                            &runtime->modbus_group_count)) {
                        free_devices(devices, output + 1U);
                        free_links(links, link_count);
                        set_error(error, error_size, "invalid Modbus read plan");
                        return false;
                    }
                }
            }
            if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_SL651) {
                uint8_t station[5];
                const char *code = device->device_code;
                size_t size = strlen(code);
                if (!size || size > 10) {
                    free_devices(devices, output + 1);
                    free_links(links, link_count);
                    set_error(error, error_size, "SL651 requires a BCD station address");
                    return false;
                }
                char padded[11] = "0000000000";
                memcpy(padded + 10 - size, code, size);
                for (size_t i = 0; i < 10; ++i)
                    if (padded[i] < '0' || padded[i] > '9') {
                        free_devices(devices, output + 1);
                        free_links(links, link_count);
                        set_error(error, error_size, "SL651 station address must be decimal");
                        return false;
                    }
                for (size_t i = 0; i < 5; ++i)
                    station[i] = (uint8_t)((padded[i * 2] - '0') * 16 + padded[i * 2 + 1] - '0');
                edge_sl651_callbacks callbacks = {sl651_send, sl651_report, sl651_command_result, sl651_trace};
                runtime->sl651 =
                    edge_sl651_create(device->sl651_response_mode, station, callbacks, runtime);
                if (!runtime->sl651) {
                    free_devices(devices, output + 1);
                    free_links(links, link_count);
                    set_error(error, error_size, "SL651 session allocation failed");
                    return false;
                }
                memcpy(runtime->sl651_station, station, 5);
                runtime->sl651_transmitter = true;
                for (size_t i = 0; i < output; ++i)
                    if (devices[i].link == runtime->link) {
                        if (!devices[i].sl651 ||
                            (endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL &&
                             memcmp(&endpoint->serial, &devices[i].endpoint->serial,
                                    sizeof(endpoint->serial)))) {
                            free_devices(devices, output + 1);
                            free_links(links, link_count);
                            set_error(
                                error, error_size,
                                "SL651 passive stream requires matching exclusive serial settings");
                            return false;
                        }
                        if (!memcmp(devices[i].sl651_station, runtime->sl651_station, 5)) {
                            unsigned previous_mode = devices[i].config->sl651_response_mode;
                            unsigned current_mode = device->sl651_response_mode;
                            if ((previous_mode ? previous_mode : 1) !=
                                (current_mode ? current_mode : 1)) {
                                free_devices(devices, output + 1);
                                free_links(links, link_count);
                                set_error(error, error_size,
                                          "shared SL651 station response modes conflict");
                                return false;
                            }
                            runtime->sl651_transmitter = false;
                        }
                    }
                ++output;
                continue;
            }
            for (size_t i = 0; i < output; ++i)
                if (devices[i].link == runtime->link && devices[i].sl651) {
                    free_devices(devices, output + 1);
                    free_links(links, link_count);
                    set_error(error, error_size,
                              "SL651 cannot share a physical stream with a polling protocol");
                    return false;
                }
            const edge_device_protocol protocol =
                device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS
                    ? EDGE_DEVICE_MODBUS : device->protocol == iot_edge_v1_Protocol_PROTOCOL_MC ? EDGE_DEVICE_MC :
                    device->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS ? EDGE_DEVICE_FINS :
                    device->protocol == iot_edge_v1_Protocol_PROTOCOL_DLT645 ? EDGE_DEVICE_DLT645 : EDGE_DEVICE_S7;
            if (!edge_device_runtime_init(&runtime->runtime, protocol,
                                          source->platform_id,
                                          device->device_id.bytes,
                                          device->io_interval_ms,
                                          device->report_interval_sec, now_ms,
                                          &kDriver, runtime)) {
                free_devices(devices, output + 1U);
                free_links(links, link_count);
                set_error(error, error_size,
                          "device runtime initialization failed");
                return false;
            }
            ++output;
        }
    }
    free_devices(acquisition->devices, acquisition->device_count);
    free_links(acquisition->links, acquisition->link_count);
    acquisition->devices = devices;
    acquisition->device_count = output;
    acquisition->links = links;
    acquisition->link_count = link_count;
    acquisition->platform_count = source_count;
    for (size_t index = 0U; index < source_count; ++index) {
        memcpy(acquisition->platform_ids[index], sources[index].platform_id, 16U);
        acquisition->cached_status[index] =
            (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    }
    acquisition->worker_required = output != 0U || acquisition->serial_path[0] != '\0';
    char detail[96];
    snprintf(detail, sizeof(detail), "platforms=%zu devices=%zu resources=%zu",
             source_count, output, link_count);
    edge_log_write("info", "config", "acquisition config applied", detail);
    return true;
}

bool edge_acquisition_apply(edge_acquisition *acquisition,
                            const edge_runtime_config *config,
                            uint64_t now_ms, char *error, size_t error_size) {
    const uint8_t platform_id[16] = {0};
    const edge_acquisition_source source = {
        .platform_id = platform_id,
        .priority = 0U,
        .bootstrap = true,
        .config = config,
    };
    return edge_acquisition_apply_multi(acquisition, &source, 1U, now_ms,
                                        error, error_size);
}

static void acquisition_status_local(edge_acquisition *acquisition,
                                      const uint8_t platform_id[16],
                                      iot_edge_v1_DeviceStatusReport *report) {
    if (acquisition == NULL || platform_id == NULL || report == NULL)
        return;
    *report = (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    const size_t capacity = sizeof(report->devices) / sizeof(report->devices[0]);
    for (size_t index = 0U; index < acquisition->device_count &&
                           report->devices_count < capacity; ++index) {
        edge_acquisition_device *device = &acquisition->devices[index];
        if (memcmp(device->platform_id, platform_id, 16U) != 0)
            continue;
        iot_edge_v1_DeviceStatus *status = &report->devices[report->devices_count++];
        edge_protocol_set_bytes(&status->device_id, sizeof(status->device_id.bytes),
                                 device->config->device_id.bytes, 16U);
        const bool connected = device->link->fd >= 0;
        if (connected) {
            copy_text(status->state, sizeof(status->state), "connected");
            status->client_count =
                device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET ? 1U : 0U;
        } else if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET &&
                   device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT) {
            copy_text(status->state, sizeof(status->state), "reconnecting");
        } else {
            copy_text(status->state, sizeof(status->state), "disconnected");
        }
        if (device->has_last_io_result && device->last_io_result != EDGE_IO_OK)
            copy_text(status->reason, sizeof(status->reason),
                      device->last_error[0] != '\0' ? device->last_error
                                                     : io_result_name(device->last_io_result));
        else
            copy_text(status->reason, sizeof(status->reason), "");
        status->last_activity_at_ms = device->last_activity_at_ms;
    }
}

static edge_acquisition_device *find_device(edge_acquisition *acquisition,
                                             const uint8_t platform_id[16],
                                             const uint8_t id[16]) {
    for (size_t index = 0U; index < acquisition->device_count; ++index)
        if (memcmp(acquisition->devices[index].platform_id, platform_id, 16U) == 0 &&
            same_id(&acquisition->devices[index].config->device_id, id))
            return &acquisition->devices[index];
    return NULL;
}

static bool writable_point(const edge_acquisition_point *point) {
    return point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
               ? point->item->item.modbus_register.writable
               : point->item->which_item == iot_edge_v1_ConfigItem_industrial_point_tag
                   ? point->item->item.industrial_point.writable : point->item->item.s7_area.writable;
}

static bool build_write_command(edge_acquisition *acquisition,
                                const uint8_t platform_id[16],
                                const iot_edge_v1_CommandRequest *request,
                                edge_acquisition_device **output_device,
                                edge_write_command *output_command,
                                char *error, size_t error_size) {
    if (acquisition == NULL || platform_id == NULL || request == NULL ||
        request->command_id.size != 16U ||
        request->device_id.size != 16U || request->values_count != 1U) {
        set_error(error, error_size, "edge command requires one device and one value");
        return false;
    }
    edge_acquisition_device *device =
        find_device(acquisition, platform_id, request->device_id.bytes);
    if (device == NULL) {
        set_error(error, error_size, "edge device is not active in this platform config");
        return false;
    }
    const iot_edge_v1_CommandValue *value = &request->values[0];
    edge_acquisition_point *point = find_point(device, value->element_id);
    if (point == NULL || !writable_point(point)) {
        set_error(error, error_size, "command element is missing or not writable");
        return false;
    }
    if (!value->has_expected ||
        value->expected.which_value != iot_edge_v1_ScalarValue_string_value_tag) {
        set_error(error, error_size, "command value must be a string scalar");
        return false;
    }
    edge_write_command command = {0};
    memcpy(command.command_id, request->command_id.bytes, 16U);
    copy_text(command.element_id, sizeof(command.element_id), value->element_id);
    command.fast_read_duration_sec = request->fast_read_duration_sec != 0U
                                         ? request->fast_read_duration_sec
                                         : device->config->command_fast_read_duration_sec;
    command.fast_read_interval_sec = request->fast_read_interval_sec != 0U
                                         ? request->fast_read_interval_sec
                                         : device->config->command_fast_read_interval_sec;
    if (command.fast_read_duration_sec > 3600U ||
        command.fast_read_interval_sec > 3600U) {
        set_error(error, error_size, "command fast-read policy is invalid");
        return false;
    }
    if (command.fast_read_duration_sec != 0U && command.fast_read_interval_sec == 0U)
        command.fast_read_interval_sec = 1U;
    if (!encode_scalar(point->item, value->expected.value.string_value,
                       command.value, sizeof(command.value), &command.value_size)) {
        set_error(error, error_size, "command value cannot be encoded for the configured type");
        return false;
    }
    *output_device = device;
    *output_command = command;
    return true;
}

static bool sl651_encode(const iot_edge_v1_Sl651ElementConfig *element, const char *text,
                         uint8_t *out, size_t cap) {
    size_t length = element->length;
    if (!length || length > cap)
        return false;
    memset(out, 0, length);
    if (!strcmp(element->encoding, "BCD")) {
        if (element->digits > 7 || length > 31)
            return false;
        char *end;
        errno = 0;
        double number = strtod(text, &end);
        if (end == text || *end || errno || !isfinite(number))
            return false;
        bool negative = number < 0;
        size_t sign = negative ? 1 : 0;
        if (length <= sign)
            return false;
        char decimal[96];
        int count = snprintf(decimal, sizeof(decimal), "%.*f", (int)element->digits, fabs(number));
        if (count <= 0 || (size_t)count >= sizeof(decimal))
            return false;
        size_t digits = 0;
        for (int i = 0; i < count; ++i)
            if (decimal[i] != '.')
                decimal[digits++] = decimal[i];
        if (digits > (length - sign) * 2)
            return false;
        size_t padding = (length - sign) * 2 - digits;
        for (size_t i = 0; i < digits; ++i) {
            if (decimal[i] < '0' || decimal[i] > '9')
                return false;
            size_t position = padding + i;
            out[sign + position / 2] |= (uint8_t)((decimal[i] - '0') << (position % 2 ? 0 : 4));
        }
        if (negative)
            out[0] = 0xFF;
        return true;
    }
    size_t digits = strlen(text);
    if (!digits || digits > length * 2)
        return false;
    for (size_t i = 0; i < digits; ++i) {
        char ch = text[i];
        unsigned digit = ch >= '0' && ch <= '9'   ? (unsigned)(ch - '0')
                         : ch >= 'A' && ch <= 'F' ? (unsigned)(ch - 'A' + 10)
                         : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10)
                                                  : 16;
        if (digit > 15)
            return false;
        size_t position = length * 2 - digits + i;
        out[position / 2] |= (uint8_t)(digit << (position % 2 ? 0 : 4));
    }
    return true;
}
static bool sl651_query_request(edge_acquisition_device *device,
                                const iot_edge_v1_CommandRequest *request, char *error,
                                size_t error_size) {
    if (request->command_id.size != 16 || !request->values_count || request->values_count > 8) {
        set_error(error, error_size, "SL651 command requires configured values");
        return false;
    }
    uint8_t body[4087] = {0};
    bool occupied[4087] = {0};
    size_t size = 0;
    const iot_edge_v1_Sl651ElementConfig *elements[8] = {0};
    const char *function = NULL;
    for (pb_size_t i = 0; i < request->values_count; ++i) {
        const iot_edge_v1_CommandValue *value = &request->values[i];
        if (!value->has_expected ||
            value->expected.which_value != iot_edge_v1_ScalarValue_string_value_tag)
            return false;
        for (size_t j = 0; j < device->point_count; ++j) {
            const iot_edge_v1_Sl651ElementConfig *element =
                &device->points[j].item->item.sl651_element;
            if (!strcmp(element->element_id, value->element_id) && element->writable &&
                !element->response_element) {
                if (elements[i]) {
                    set_error(error, error_size, "ambiguous SL651 command element");
                    return false;
                }
                elements[i] = element;
            }
        }
        if (!elements[i] || (function && strcmp(function, elements[i]->function_code))) {
            set_error(error, error_size, "SL651 command elements must share a writable function");
            return false;
        }
        function = elements[i]->function_code;
    }
    for (unsigned pass = 0; pass < 2; ++pass)
        for (pb_size_t i = 0; i < request->values_count; ++i) {
            const iot_edge_v1_Sl651ElementConfig *element = elements[i];
            if (element->fixed_position != (pass == 0))
                continue;
            size_t offset = size;
            if (element->fixed_position) {
                if (element->byte_offset < 8) {
                    set_error(error, error_size, "SL651 fixed command overlaps serial/time");
                    return false;
                }
                offset = element->byte_offset - 8;
            } else {
                if (element->guide.size > sizeof(body) - size)
                    return false;
                memcpy(body + size, element->guide.bytes, element->guide.size);
                offset += element->guide.size;
            }
            if (offset > sizeof(body) || element->length > sizeof(body) - offset ||
                !sl651_encode(element, request->values[i].expected.value.string_value,
                              body + offset, sizeof(body) - offset)) {
                set_error(error, error_size, "invalid SL651 command value or length");
                return false;
            }
            for (size_t j = offset; j < offset + element->length; ++j) {
                if (occupied[j]) {
                    set_error(error, error_size, "overlapping SL651 command elements");
                    return false;
                }
                occupied[j] = true;
            }
            if (size < offset + element->length)
                size = offset + element->length;
        }
    char *end;
    unsigned long code = strtoul(function, &end, 16);
    if (strlen(function) != 2 || *end || code > 255)
        return false;
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (other->link == device->link &&
            !memcmp(other->sl651_station, device->sl651_station, 5) && other->sl651_query_active) {
            set_error(error, error_size, "SL651 physical station command is busy");
            return false;
        }
    }
    bool previous = device->sl651_transmitter;
    device->sl651_transmitter = true;
    uint8_t time[6];
    sl651_time(device, time);
    if (!edge_sl651_query(device->sl651, request->command_id.bytes, (uint8_t)code, body, size,
                          monotonic_milliseconds(), request->timeout_ms, time)) {
        device->sl651_transmitter = previous;
        set_error(error, error_size,
                  "SL651 command requires an online M2/M3/M4 station without a pending or "
                  "timed-out query");
        return false;
    }
    for (size_t i = 0; i < device->owner->device_count; ++i) {
        edge_acquisition_device *other = &device->owner->devices[i];
        if (other != device && other->link == device->link &&
            !memcmp(other->sl651_station, device->sl651_station, 5))
            other->sl651_transmitter = false;
    }
    device->sl651_query_active = true;
    return true;
}

static bool acquisition_command_local(edge_acquisition *acquisition,
                                      const uint8_t platform_id[16],
                                      const iot_edge_v1_CommandRequest *request,
                                      char *error, size_t error_size) {
    if (request && request->device_id.size == 16) {
        edge_acquisition_device *station =
            find_device(acquisition, platform_id, request->device_id.bytes);
        if (station && serial_paused(station)) {
            set_error(error, error_size, "serial port is paused for manual debugging");
            return false;
        }
        if (station && station->sl651)
            return sl651_query_request(station, request, error, error_size);
    }
    edge_acquisition_device *device = NULL;
    edge_write_command command;
    if (!build_write_command(acquisition, platform_id, request, &device, &command,
                             error, error_size))
        return false;
    if (!edge_device_runtime_enqueue_write(&device->runtime, &command)) {
        set_error(error, error_size, "device write queue is full");
        return false;
    }
    return true;
}

static size_t acquisition_message_size(uint32_t payload_size) {
    return offsetof(edge_acquisition_message, payload) + payload_size;
}

static bool worker_send(edge_acquisition *acquisition, uint32_t type,
                        const uint8_t platform_id[16],
                        const void *payload, uint32_t payload_size) {
    if (acquisition == NULL || acquisition->worker_fd < 0 || platform_id == NULL ||
        payload == NULL ||
        payload_size > sizeof(((edge_acquisition_message *)0)->payload))
        return false;
    edge_acquisition_message message;
    memset(&message, 0, sizeof(message));
    message.magic = EDGE_ACQUISITION_MAGIC;
    message.type = type;
    message.payload_size = payload_size;
    memcpy(message.platform_id, platform_id, sizeof(message.platform_id));
    memcpy(&message.payload, payload, payload_size);
    for (;;) {
        const ssize_t sent = send(acquisition->worker_fd, &message,
                                  acquisition_message_size(payload_size), MSG_NOSIGNAL);
        if (sent == (ssize_t)acquisition_message_size(payload_size))
            return true;
        if (sent < 0 && errno == EINTR)
            continue;
        return false;
    }
}

static bool worker_sl651_report(edge_acquisition_device *device,
                                const iot_edge_v1_TelemetryRecord *record, uint32_t part) {
    edge_acquisition_message message;
    memset(&message, 0, sizeof(message));
    message.magic = EDGE_ACQUISITION_MAGIC;
    message.type = EDGE_ACQUISITION_EVENT_SL651;
    memcpy(message.platform_id, device->platform_id, 16);
    memcpy(message.device_id, device->config->device_id.bytes, 16);
    message.report_token = device->sl651_token;
    message.report_part = part;
    pb_ostream_t stream =
        pb_ostream_from_buffer(message.payload.telemetry, sizeof(message.payload.telemetry));
    if (!pb_encode(&stream, iot_edge_v1_TelemetryRecord_fields, record))
        return false;
    message.payload_size = (uint32_t)stream.bytes_written;
    const size_t size = acquisition_message_size(message.payload_size);
    return send(device->owner->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) ==
           (ssize_t)size;
}

static void worker_debug(void *context, const uint8_t platform_id[16], const iot_edge_v1_RawPacket *packet) {
    edge_acquisition *acquisition = context;
    edge_acquisition_message message;
    memset(&message, 0, sizeof(message));
    message.magic = EDGE_ACQUISITION_MAGIC;
    message.type = EDGE_ACQUISITION_EVENT_DEBUG;
    memcpy(message.platform_id, platform_id, 16);
    pb_ostream_t stream = pb_ostream_from_buffer(message.payload.telemetry, sizeof(message.payload.telemetry));
    if (!pb_encode(&stream, iot_edge_v1_RawPacket_fields, packet)) {
        syslog(LOG_WARNING, "debug packet dropped: worker encoding limit");
        return;
    }
    message.payload_size = (uint32_t)stream.bytes_written;
    const size_t size = acquisition_message_size(message.payload_size);
    if (send(acquisition->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)size)
        syslog(LOG_WARNING, "debug packet dropped: acquisition worker backpressure");
}

static bool worker_telemetry(void *context,
                             const uint8_t platform_id[16],
                             const iot_edge_v1_TelemetryRecord *record) {
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE];
    pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&stream, iot_edge_v1_TelemetryRecord_fields, record)) {
        syslog(LOG_ERR, "cannot encode complete telemetry record: %s", PB_GET_ERROR(&stream));
        return false;
    }
    // Pointers are process-local. Transfer serialized values, never the C struct.
    return worker_send(context, EDGE_ACQUISITION_EVENT_TELEMETRY, platform_id,
                       wire, (uint32_t)stream.bytes_written);
}

static bool worker_command_result(void *context,
                                  const uint8_t platform_id[16],
                                  const iot_edge_v1_CommandResult *result) {
    return worker_send(context, EDGE_ACQUISITION_EVENT_COMMAND_RESULT, platform_id,
                       result, sizeof(*result));
}

static void worker_send_command_failure(edge_acquisition *acquisition,
                                        const uint8_t platform_id[16],
                                        const iot_edge_v1_CommandRequest *request,
                                        const char *error) {
    iot_edge_v1_CommandResult result = iot_edge_v1_CommandResult_init_zero;
    if (request->command_id.size == 16U)
        edge_protocol_set_bytes(&result.command_id, sizeof(result.command_id.bytes),
                                request->command_id.bytes, 16U);
    if (request->device_id.size == 16U)
        edge_protocol_set_bytes(&result.device_id, sizeof(result.device_id.bytes),
                                request->device_id.bytes, 16U);
    result.state = iot_edge_v1_CommandState_COMMAND_STATE_FAILED;
    result.completed_at_ms = current_ms();
    copy_text(result.message, sizeof(result.message), error);
    (void)worker_command_result(acquisition, platform_id, &result);
}

static bool serial_paused(const edge_acquisition_device *device) {
    if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL) return false;
    for (size_t i = 0; i < 4; ++i) {
        const edge_serial_session *session = &device->owner->serial_sessions[i];
        if (session->active && session->manual &&
            !strcmp(session->settings.channel, device->endpoint->serial.channel)) return true;
    }
    return false;
}

static void serial_emit(edge_acquisition *acquisition, edge_serial_session *session,
    uint64_t request, const char *kind, const char *direction, const uint8_t *bytes,
    size_t size, const char *reason) {
    edge_acquisition_message message = {0};
    message.magic = EDGE_ACQUISITION_MAGIC;
    message.type = EDGE_ACQUISITION_EVENT_SERIAL;
    message.payload_size = sizeof(message.payload.serial_event);
    memcpy(message.platform_id, session->platform_id, 16);
    iot_edge_v1_SerialDebugEvent *event = &message.payload.serial_event;
    edge_protocol_set_bytes(&event->session_id, 16, session->id, 16);
    event->request_sequence = request;
    event->sequence = ++session->event_sequence;
    event->manual = session->manual;
    event->occurred_at_ms = current_ms();
    event->dropped_bytes = session->dropped_bytes;
    event->has_settings = true;
    event->settings = session->settings;
    copy_text(event->kind, sizeof(event->kind), kind);
    copy_text(event->direction, sizeof(event->direction), direction);
    copy_text(event->message, sizeof(event->message), reason);
    edge_protocol_set_bytes(&event->data, sizeof(event->data.bytes), bytes, size);
    const size_t wire_size = acquisition_message_size(message.payload_size);
    if (send(acquisition->worker_fd, &message, wire_size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)wire_size)
        session->dropped_bytes += size;
}

static void serial_observe(edge_acquisition *acquisition, const char *path,
    const iot_edge_v1_SerialSettings *settings, const char *direction,
    const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < 4; ++i) {
        edge_serial_session *session = &acquisition->serial_sessions[i];
        if (!session->active || strcmp(session->settings.channel, path)) continue;
        if (!session->manual) session->settings = *settings;
        for (size_t offset = 0; offset < size;) {
            const size_t part = size - offset > 1024 ? 1024 : size - offset;
            serial_emit(acquisition, session, 0, "data", direction, bytes + offset, part, "");
            offset += part;
        }
    }
}

static void serial_release(edge_acquisition *acquisition, edge_serial_session *session,
    uint64_t request, const char *reason) {
    if (session->fd >= 0) close(session->fd);
    session->fd = -1;
    session->manual = false;
    serial_emit(acquisition, session, request, "closed", "", NULL, 0, reason);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

static bool serial_configured(edge_acquisition *acquisition, edge_serial_session *session, bool update_settings) {
    for (size_t i = 0; i < acquisition->device_count; ++i) {
        const edge_acquisition_device *device = &acquisition->devices[i];
        if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL &&
            !strcmp(device->endpoint->serial.channel, session->settings.channel)) {
            if (update_settings) session->settings = device->endpoint->serial;
            return true;
        }
    }
    return false;
}

static bool serial_settings_valid(const iot_edge_v1_SerialSettings *settings) {
    return baud_rate(settings->baud_rate) != 0 && settings->data_bits >= 5 &&
        settings->data_bits <= 8 && (settings->stop_bits == 1 || settings->stop_bits == 2) &&
        (!strcmp(settings->parity, "none") || !strcmp(settings->parity, "even") || !strcmp(settings->parity, "odd"));
}

static void serial_control(edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_SerialDebugRequest *request) {
    if (request->session_id.size != 16 || request->request_sequence == 0) return;
    edge_serial_session *session = NULL;
    for (size_t i = 0; i < 4; ++i)
        if (acquisition->serial_sessions[i].active &&
            !memcmp(acquisition->serial_sessions[i].platform_id, platform_id, 16)) {
            session = &acquisition->serial_sessions[i]; break;
        }
    if (!strcmp(request->action, "open")) {
        if (session) {
            // Replaying OPEN never resets an existing session or its manual-write sequence.
            if (!memcmp(session->id, request->session_id.bytes, 16)) {
                serial_emit(acquisition, session, request->request_sequence, "state", "", NULL, 0, "");
                return;
            }
            edge_serial_session rejected = {.fd = -1};
            memcpy(rejected.platform_id, platform_id, 16);
            memcpy(rejected.id, request->session_id.bytes, 16);
            serial_emit(acquisition, &rejected, request->request_sequence, "closed", "", NULL, 0,
                        "this platform already has a serial debug session");
            return;
        }
        for (size_t i = 0; i < 4; ++i)
            if (!acquisition->serial_sessions[i].active) { session = &acquisition->serial_sessions[i]; break; }
        if (!session) return;
        memset(session, 0, sizeof(*session));
        session->fd = -1;
        memcpy(session->platform_id, platform_id, 16);
        memcpy(session->id, request->session_id.bytes, 16);
        session->settings = request->settings;
        if (!request->has_settings || acquisition->serial_path[0] == '\0' ||
            strcmp(request->settings.channel, acquisition->serial_path)) {
            serial_release(acquisition, session, request->request_sequence, "serial port is not advertised");
            return;
        }
        session->active = true;
        session->settings.baud_rate = 9600;
        session->settings.data_bits = 8;
        session->settings.stop_bits = 1;
        session->settings.rs485 = acquisition->serial_rs485;
        copy_text(session->settings.parity, sizeof(session->settings.parity), "none");
        (void)serial_configured(acquisition, session, true);
    } else if (!session || memcmp(session->id, request->session_id.bytes, 16)) {
        if (strcmp(request->action, "close")) {
            edge_serial_session missing = {.fd = -1};
            memcpy(missing.platform_id, platform_id, 16); memcpy(missing.id, request->session_id.bytes, 16);
            serial_emit(acquisition, &missing, request->request_sequence, "closed", "", NULL, 0,
                        "serial debug session expired or acquisition restarted");
        }
        return;
    }
    if (request->request_sequence <= session->last_request) return;
    session->last_request = request->request_sequence;
    if (!strcmp(request->action, "close")) {
        serial_release(acquisition, session, request->request_sequence, "serial debug closed"); return;
    }
    session->expires_ms = monotonic_milliseconds() + 60000;
    if (!strcmp(request->action, "keepalive")) {
        serial_emit(acquisition, session, request->request_sequence, "state", "", NULL, 0, ""); return;
    }
    if (session->pending_size) {
        serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "serial write still pending"); return;
    }
    if (!strcmp(request->action, "manual")) {
        if (!request->has_settings || strcmp(request->settings.channel, session->settings.channel) ||
            !serial_settings_valid(&request->settings)) {
            serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "invalid serial settings"); return;
        }
        for (size_t i = 0; i < 4; ++i)
            if (&acquisition->serial_sessions[i] != session && acquisition->serial_sessions[i].active &&
                acquisition->serial_sessions[i].manual &&
                !strcmp(acquisition->serial_sessions[i].settings.channel, session->settings.channel)) {
                serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0,
                            "another platform controls this serial port"); return;
            }
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *device = &acquisition->devices[i];
            if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL ||
                strcmp(device->endpoint->serial.channel, session->settings.channel)) continue;
            if (device->runtime.write_count || (device->sl651 &&
                (!edge_sl651_ready(device->sl651) || device->sl651_query_active || device->sl651_result_pending))) {
                serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0,
                            "device transaction pending; retry after it completes"); return;
            }
        }
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *device = &acquisition->devices[i];
            if (device->endpoint->transport != iot_edge_v1_Transport_TRANSPORT_SERIAL ||
                strcmp(device->endpoint->serial.channel, session->settings.channel)) continue;
            edge_device_runtime_close(&device->runtime);
            close_fd(&device->link->fd);
            if (device->sl651) edge_sl651_reset(device->sl651);
        }
        for (size_t i = 0; i < 4; ++i)
            if (acquisition->serial_sessions[i].active &&
                !strcmp(acquisition->serial_sessions[i].settings.channel, session->settings.channel))
                close_fd(&acquisition->serial_sessions[i].fd);
        session->settings = request->settings;
        session->fd = open_serial(&session->settings);
        if (session->fd < 0) {
            session->manual = false;
            serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "cannot configure serial port"); return;
        }
        session->manual = true;
    } else if (!strcmp(request->action, "monitor")) {
        close_fd(&session->fd);
        session->manual = false;
        (void)serial_configured(acquisition, session, true);
    } else if (!strcmp(request->action, "write")) {
        if (!session->manual || request->data.size == 0 || request->data.size > sizeof(session->pending)) {
            serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0,
                        "pause automatic acquisition before manual sending"); return;
        }
        memcpy(session->pending, request->data.bytes, request->data.size);
        session->pending_size = request->data.size;
        session->pending_offset = 0;
        session->pending_request = request->request_sequence;
        session->pending_deadline = monotonic_milliseconds() + 10000;
        return;
    } else if (strcmp(request->action, "open")) {
        serial_emit(acquisition, session, request->request_sequence, "error", "", NULL, 0, "unknown serial action"); return;
    }
    serial_emit(acquisition, session, request->request_sequence, "state", "", NULL, 0, "");
}

static void serial_tick(edge_acquisition *acquisition, uint64_t now) {
    for (size_t i = 0; i < 4; ++i) {
        edge_serial_session *session = &acquisition->serial_sessions[i];
        if (!session->active) continue;
        if (now >= session->expires_ms) { serial_release(acquisition, session, 0, "serial debug lease expired"); continue; }
        if (session->fd < 0 && !session->manual) {
            if (serial_configured(acquisition, session, false)) continue;
            bool held = false;
            for (size_t j = 0; j < 4; ++j)
                if (i != j && acquisition->serial_sessions[j].active &&
                    acquisition->serial_sessions[j].fd >= 0 &&
                    !strcmp(acquisition->serial_sessions[j].settings.channel, session->settings.channel)) held = true;
            if (held) continue;
            session->fd = open_serial(&session->settings);
            if (session->fd < 0) { serial_release(acquisition, session, 0, "cannot open serial port"); continue; }
        }
        if (session->pending_size) {
            const ssize_t count = write(session->fd, session->pending + session->pending_offset,
                session->pending_size - session->pending_offset);
            if (count > 0) {
                serial_observe(acquisition, session->settings.channel, &session->settings, "TX",
                    session->pending + session->pending_offset, (size_t)count);
                session->pending_offset += (size_t)count;
            } else if ((count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ||
                       now >= session->pending_deadline) {
                serial_release(acquisition, session, session->pending_request,
                    "serial write failed; some bytes may have been sent"); continue;
            }
            if (session->pending_offset == session->pending_size) {
                session->pending_size = session->pending_offset = 0;
                serial_emit(acquisition, session, session->pending_request, "sent", "", NULL, 0, "");
            }
        }
        if (session->fd < 0) continue;
        struct pollfd descriptor = {.fd = session->fd, .events = POLLIN};
        if (poll(&descriptor, 1, 0) <= 0) continue;
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            serial_release(acquisition, session, 0, "serial device disconnected"); continue;
        }
        if (descriptor.revents & POLLIN) {
            uint8_t bytes[1024];
            const ssize_t count = read(session->fd, bytes, sizeof(bytes));
            if (count > 0) serial_observe(acquisition, session->settings.channel, &session->settings, "RX", bytes, (size_t)count);
            else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                serial_release(acquisition, session, 0, "serial read failed");
        }
    }
}

static bool worker_receive_control(edge_acquisition *acquisition, bool *stop) {
    edge_acquisition_message message;
    const ssize_t size = recv(acquisition->worker_fd, &message, sizeof(message),
                              MSG_DONTWAIT);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return true;
    if (size < 0 && errno == EINTR)
        return true;
    if (size <= 0)
        return false;
    if ((size_t)size < offsetof(edge_acquisition_message, payload) ||
        message.magic != EDGE_ACQUISITION_MAGIC ||
        message.payload_size > sizeof(message.payload) ||
        acquisition_message_size(message.payload_size) != (size_t)size)
        return false;
    if (message.type == EDGE_ACQUISITION_CONTROL_SL651_COMMIT ||
        message.type == EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT) {
        edge_acquisition_device *device =
            find_device(acquisition, message.platform_id, message.device_id);
        if (!device || !device->sl651)
            return true;
        uint8_t time[6];
        sl651_time(device, time);
        if (message.type == EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT) {
            if (message.payload_size != 16 || !device->sl651_result_pending ||
                memcmp(message.payload.telemetry, device->sl651_result.command_id.bytes, 16))
                return true;
            device->sl651_result_pending = false;
            device->sl651_query_active = false;
            edge_sl651_command_committed(device->sl651, message.payload.telemetry,
                                         monotonic_milliseconds(), time);
            return true;
        }
        if (message.report_token != device->sl651_token ||
            message.report_part >= device->sl651_parts)
            return true;
        device->sl651_committed[message.report_part] = true;
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *other = &acquisition->devices[i];
            if (!other->sl651 || other->link != device->link ||
                memcmp(other->sl651_station, device->sl651_station, 5))
                continue;
            if (!other->sl651_report_encoded)
                return true;
            for (uint32_t part = 0; part < other->sl651_parts; ++part)
                if (!other->sl651_committed[part])
                    return true;
        }
        for (size_t i = 0; i < acquisition->device_count; ++i) {
            edge_acquisition_device *other = &acquisition->devices[i];
            if (!other->sl651 || other->link != device->link ||
                memcmp(other->sl651_station, device->sl651_station, 5))
                continue;
            sl651_time(other, time);
            edge_sl651_commit(other->sl651, other->sl651_token, monotonic_milliseconds(), time);
        }
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_STOP) {
        *stop = true;
        return true;
    }
    if (message.type == EDGE_ACQUISITION_CONTROL_SERIAL &&
        message.payload_size == sizeof(message.payload.serial_request)) {
        serial_control(acquisition, message.platform_id, &message.payload.serial_request);
        return true;
    }
    if (message.type != EDGE_ACQUISITION_CONTROL_COMMAND ||
        message.payload_size != sizeof(message.payload.command_request))
        return false;
    char error[256] = {0};
    if (!acquisition_command_local(acquisition, message.platform_id,
                                   &message.payload.command_request,
                                   error, sizeof(error)))
        worker_send_command_failure(acquisition, message.platform_id,
                                    &message.payload.command_request,
                                    error[0] != '\0' ? error : "device command rejected");
    return true;
}

static void acquisition_worker(edge_acquisition *acquisition, int worker_fd) {
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _exit(1);
    edge_process_close_inherited_fds(worker_fd);
    acquisition->worker_child = true;
    acquisition->worker_pid = 0;
    acquisition->worker_fd = worker_fd;
    acquisition->debug = worker_debug;
    acquisition->telemetry = worker_telemetry;
    acquisition->command = worker_command_result;
    acquisition->callback_context = acquisition;
    int link_fd = open_link_monitor();
    if (link_fd < 0)
        edge_log_write("warn", "network", "cannot subscribe to link events",
                       "device reconnect will rely on transport errors");

    bool stop = false;
    uint64_t next_tick = monotonic_milliseconds();
    while (!stop) {
        const uint64_t current = monotonic_milliseconds();
        int timeout = current >= next_tick ? 0 : (int)(next_tick - current);
        if (timeout > 20)
            timeout = 20;
        struct pollfd descriptors[2] = {
            {.fd = worker_fd, .events = POLLIN},
            {.fd = link_fd, .events = POLLIN},
        };
        const nfds_t descriptor_count = link_fd >= 0 ? 2U : 1U;
        const int ready = poll(descriptors, descriptor_count, timeout);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready > 0 && link_fd >= 0 &&
            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close_fd(&link_fd);
            edge_log_write("warn", "network", "link event monitor stopped",
                           "device reconnect will rely on transport errors");
        } else if (ready > 0 && link_fd >= 0 &&
                   (descriptors[1].revents & POLLIN) != 0) {
            drain_link_events(acquisition, link_fd);
        }
        if (ready > 0 &&
            (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            break;
        if (ready > 0 && (descriptors[0].revents & POLLIN) != 0 &&
            !worker_receive_control(acquisition, &stop))
            goto done;
        if (stop)
            break;
        const uint64_t schedule_ms = monotonic_milliseconds();
        serial_tick(acquisition, schedule_ms);
        if (schedule_ms < next_tick)
            continue;
        for (size_t index = 0U; index < acquisition->device_count && !stop; ++index) {
            if (serial_paused(&acquisition->devices[index])) {
                /* Manual control owns the port; leave other physical links running. */
            } else if (acquisition->devices[index].sl651)
                sl651_poll(&acquisition->devices[index], schedule_ms);
            else
                edge_device_runtime_tick(&acquisition->devices[index].runtime, schedule_ms,
                                         current_ms());
            while (!stop) {
                struct pollfd pending = {.fd = worker_fd, .events = POLLIN};
                if (poll(&pending, 1U, 0) <= 0)
                    break;
                if (!worker_receive_control(acquisition, &stop))
                    goto done;
            }
        }
        for (size_t index = 0U; index < acquisition->platform_count; ++index) {
            iot_edge_v1_DeviceStatusReport status;
            acquisition_status_local(acquisition, acquisition->platform_ids[index],
                                     &status);
            if (!worker_send(acquisition, EDGE_ACQUISITION_EVENT_STATUS,
                             acquisition->platform_ids[index], &status,
                             sizeof(status)))
                goto done;
        }
        next_tick = monotonic_milliseconds() + EDGE_DTU_IO_PERIOD_MS;
    }
done:
    close_fd(&link_fd);
    close(worker_fd);
    _exit(0);
}

static bool start_worker(edge_acquisition *acquisition,
                         char *error, size_t error_size) {
    if (!acquisition->worker_required)
        return true;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        set_error(error, error_size, "cannot create acquisition worker channel");
        return false;
    }
    for (size_t index = 0U; index < acquisition->platform_count; ++index)
        acquisition_status_local(acquisition, acquisition->platform_ids[index],
                                 &acquisition->cached_status[index]);
    const pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        set_error(error, error_size, "cannot start acquisition worker");
        return false;
    }
    if (child == 0) {
        close(sockets[0]);
        acquisition_worker(acquisition, sockets[1]);
    }
    close(sockets[1]);
    const int flags = fcntl(sockets[0], F_GETFL, 0);
    if (flags < 0 || fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(sockets[0]);
        (void)kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
        set_error(error, error_size, "cannot configure acquisition worker channel");
        return false;
    }
    acquisition->worker_pid = child;
    acquisition->worker_fd = sockets[0];
    acquisition->worker_last_event_ms = monotonic_milliseconds();
    acquisition->worker_restart_at_ms = 0U;
    return true;
}

bool edge_acquisition_start(edge_acquisition *acquisition,
                            char *error, size_t error_size) {
    if (acquisition == NULL || acquisition->worker_pid > 0 || acquisition->worker_child) {
        set_error(error, error_size, "invalid acquisition worker state");
        return false;
    }
    return start_worker(acquisition, error, error_size);
}

int edge_acquisition_event_fd(const edge_acquisition *acquisition) {
    return acquisition != NULL && !acquisition->worker_child
               ? acquisition->worker_fd
               : -1;
}

static void worker_reaped(edge_acquisition *acquisition, uint64_t now_ms) {
    if (acquisition->worker_fd >= 0)
        close(acquisition->worker_fd);
    acquisition->worker_fd = -1;
    acquisition->worker_pid = 0;
    acquisition->worker_restart_at_ms = now_ms + EDGE_ACQUISITION_RESTART_MS;
    for (size_t platform = 0U; platform < acquisition->platform_count; ++platform)
        for (pb_size_t index = 0U;
             index < acquisition->cached_status[platform].devices_count; ++index) {
            copy_text(acquisition->cached_status[platform].devices[index].state,
                      sizeof(acquisition->cached_status[platform].devices[index].state),
                      "worker-restarting");
            copy_text(acquisition->cached_status[platform].devices[index].reason,
                      sizeof(acquisition->cached_status[platform].devices[index].reason),
                      "acquisition worker exited");
        }
}

static void drain_worker(edge_acquisition *acquisition, uint64_t now_ms) {
    if (acquisition->worker_pid <= 0 || acquisition->worker_fd < 0)
        return;
    for (;;) {
        edge_acquisition_message message;
        const ssize_t size = recv(acquisition->worker_fd, &message, sizeof(message), 0);
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (size <= 0) {
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(acquisition->worker_pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
                worker_reaped(acquisition, now_ms);
            break;
        }
        if ((size_t)size < offsetof(edge_acquisition_message, payload) ||
            message.magic != EDGE_ACQUISITION_MAGIC ||
            message.payload_size > sizeof(message.payload) ||
            acquisition_message_size(message.payload_size) != (size_t)size)
            continue;
        acquisition->worker_last_event_ms = now_ms;
        if (message.type == EDGE_ACQUISITION_EVENT_SERIAL &&
            message.payload_size == sizeof(message.payload.serial_event)) {
            if (acquisition->serial_callback)
                acquisition->serial_callback(acquisition->callback_context, message.platform_id,
                    &message.payload.serial_event);
        } else if (message.type == EDGE_ACQUISITION_EVENT_DEBUG) {
            iot_edge_v1_RawPacket packet = iot_edge_v1_RawPacket_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(message.payload.telemetry, message.payload_size);
            if (pb_decode(&stream, iot_edge_v1_RawPacket_fields, &packet) && acquisition->debug)
                acquisition->debug(acquisition->callback_context, message.platform_id, &packet);
            pb_release(iot_edge_v1_RawPacket_fields, &packet);
        } else if (message.type == EDGE_ACQUISITION_EVENT_TELEMETRY ||
            message.type == EDGE_ACQUISITION_EVENT_SL651) {
            iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(message.payload.telemetry,
                                                        message.payload_size);
            if (pb_decode(&stream, iot_edge_v1_TelemetryRecord_fields, &record)) {
                const bool stored = acquisition->telemetry(acquisition->callback_context,
                                                           message.platform_id, &record);
                if (stored && message.type == EDGE_ACQUISITION_EVENT_SL651) {
                    message.type = EDGE_ACQUISITION_CONTROL_SL651_COMMIT;
                    message.payload_size = 0;
                    (void)send(acquisition->worker_fd, &message, acquisition_message_size(0),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
                }
            } else
                syslog(LOG_ERR, "cannot decode telemetry from acquisition worker");
            pb_release(iot_edge_v1_TelemetryRecord_fields, &record);
        } else if (message.type == EDGE_ACQUISITION_EVENT_COMMAND_RESULT &&
                   message.payload_size == sizeof(message.payload.command_result)) {
            const iot_edge_v1_CommandResult *result = &message.payload.command_result;
            const bool stored =
                acquisition->command(acquisition->callback_context, message.platform_id, result);
            edge_acquisition_device *device =
                result->device_id.size == 16
                    ? find_device(acquisition, message.platform_id, result->device_id.bytes)
                    : NULL;
            if (stored && device && device->sl651 && result->command_id.size == 16) {
                uint8_t id[16];
                memcpy(id, result->command_id.bytes, 16);
                memcpy(message.device_id, result->device_id.bytes, 16);
                memcpy(message.payload.telemetry, id, 16);
                message.type = EDGE_ACQUISITION_CONTROL_SL651_COMMAND_COMMIT;
                message.payload_size = 16;
                (void)send(acquisition->worker_fd, &message, acquisition_message_size(16),
                           MSG_DONTWAIT | MSG_NOSIGNAL);
            }
        } else if (message.type == EDGE_ACQUISITION_EVENT_STATUS &&
                   message.payload_size == sizeof(message.payload.status)) {
            for (size_t index = 0U; index < acquisition->platform_count; ++index)
                if (memcmp(acquisition->platform_ids[index], message.platform_id,
                           16U) == 0) {
                    acquisition->cached_status[index] = message.payload.status;
                    break;
                }
        }
    }
}

void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms) {
    if (acquisition == NULL || acquisition->worker_child)
        return;
    drain_worker(acquisition, now_ms);
    if (acquisition->worker_pid > 0 && acquisition->worker_fd >= 0 &&
        now_ms - acquisition->worker_last_event_ms >= EDGE_ACQUISITION_WATCHDOG_MS) {
        edge_log_write("error", "acquisition", "acquisition worker stalled",
                       "watchdog=120s action=restart");
        (void)kill(acquisition->worker_pid, SIGKILL);
        close(acquisition->worker_fd);
        acquisition->worker_fd = -1;
    }
    if (acquisition->worker_pid > 0 && acquisition->worker_fd < 0) {
        int status = 0;
        const pid_t waited = waitpid(acquisition->worker_pid, &status, WNOHANG);
        if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
            worker_reaped(acquisition, now_ms);
    }
    if (acquisition->worker_required && acquisition->worker_pid == 0 &&
        now_ms >= acquisition->worker_restart_at_ms) {
        char error[128];
        if (!start_worker(acquisition, error, sizeof(error)))
            acquisition->worker_restart_at_ms = now_ms + EDGE_ACQUISITION_RESTART_MS;
    }
}

void edge_acquisition_status(edge_acquisition *acquisition,
                             iot_edge_v1_DeviceStatusReport *report) {
    const uint8_t platform_id[16] = {0};
    edge_acquisition_status_for_platform(acquisition, platform_id, report);
}

void edge_acquisition_status_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    iot_edge_v1_DeviceStatusReport *report) {
    if (acquisition == NULL || platform_id == NULL || report == NULL)
        return;
    *report = (iot_edge_v1_DeviceStatusReport)iot_edge_v1_DeviceStatusReport_init_zero;
    for (size_t index = 0U; index < acquisition->platform_count; ++index)
        if (memcmp(acquisition->platform_ids[index], platform_id, 16U) == 0) {
            *report = acquisition->cached_status[index];
            return;
        }
}

bool edge_acquisition_command(edge_acquisition *acquisition,
                              const iot_edge_v1_CommandRequest *request,
                              char *error, size_t error_size) {
    const uint8_t platform_id[16] = {0};
    return edge_acquisition_command_for_platform(acquisition, platform_id, request,
                                                 error, error_size);
}

bool edge_acquisition_command_for_platform(
    edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_CommandRequest *request,
    char *error, size_t error_size) {
    if (acquisition == NULL || acquisition->worker_pid <= 0 ||
        acquisition->worker_fd < 0 || platform_id == NULL) {
        set_error(error, error_size, "acquisition worker is unavailable");
        return false;
    }
    edge_acquisition_device *device =
        request && request->device_id.size == 16
            ? find_device(acquisition, platform_id, request->device_id.bytes)
            : NULL;
    if (!device || !device->sl651) {
        edge_write_command command;
        if (!build_write_command(acquisition, platform_id, request, &device, &command, error,
                                 error_size))
            return false;
    } else if (request->command_id.size != 16 || !request->values_count ||
               request->values_count > 8) {
        set_error(error, error_size, "invalid SL651 command");
        return false;
    }
    edge_acquisition_message message;
    memset(&message, 0, sizeof(message));
    message.magic = EDGE_ACQUISITION_MAGIC;
    message.type = EDGE_ACQUISITION_CONTROL_COMMAND;
    message.payload_size = sizeof(message.payload.command_request);
    memcpy(message.platform_id, platform_id, sizeof(message.platform_id));
    message.payload.command_request = *request;
    const size_t size = acquisition_message_size(message.payload_size);
    const ssize_t sent = send(acquisition->worker_fd, &message, size,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent != (ssize_t)size) {
        set_error(error, error_size, "acquisition worker command queue is full");
        return false;
    }
    return true;
}

bool edge_acquisition_serial_request(edge_acquisition *acquisition, const uint8_t platform_id[16],
    const iot_edge_v1_SerialDebugRequest *request) {
    if (!acquisition || acquisition->worker_fd < 0 || !platform_id || !request) return false;
    edge_acquisition_message message = {0};
    message.magic = EDGE_ACQUISITION_MAGIC;
    message.type = EDGE_ACQUISITION_CONTROL_SERIAL;
    message.payload_size = sizeof(message.payload.serial_request);
    memcpy(message.platform_id, platform_id, 16);
    message.payload.serial_request = *request;
    const size_t size = acquisition_message_size(message.payload_size);
    return send(acquisition->worker_fd, &message, size, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)size;
}

void edge_acquisition_stop(edge_acquisition *acquisition) {
    if (acquisition == NULL || acquisition->worker_child)
        return;
    if (acquisition->worker_pid <= 0)
        return;
    if (acquisition->worker_fd >= 0) {
        edge_acquisition_message message = {
            .magic = EDGE_ACQUISITION_MAGIC,
            .type = EDGE_ACQUISITION_CONTROL_STOP,
            .payload_size = 0U,
        };
        (void)send(acquisition->worker_fd, &message,
                   acquisition_message_size(0U), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
        pid_t waited;
        do {
            waited = waitpid(acquisition->worker_pid, NULL, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
            goto stopped;
        usleep(5000U);
    }
    (void)kill(acquisition->worker_pid, SIGTERM);
    for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
        const pid_t waited = waitpid(acquisition->worker_pid, NULL, WNOHANG);
        if (waited == acquisition->worker_pid || (waited < 0 && errno == ECHILD))
            goto stopped;
        usleep(5000U);
    }
    (void)kill(acquisition->worker_pid, SIGKILL);
    while (waitpid(acquisition->worker_pid, NULL, 0) < 0 && errno == EINTR) {
    }
stopped:
    if (acquisition->worker_fd >= 0)
        close(acquisition->worker_fd);
    acquisition->worker_fd = -1;
    acquisition->worker_pid = 0;
}

void edge_acquisition_destroy(edge_acquisition *acquisition) {
    if (acquisition == NULL)
        return;
    edge_acquisition_stop(acquisition);
    free_devices(acquisition->devices, acquisition->device_count);
    free_links(acquisition->links, acquisition->link_count);
    free(acquisition);
}

size_t edge_acquisition_device_count(const edge_acquisition *acquisition) {
    return acquisition != NULL ? acquisition->device_count : 0U;
}

size_t edge_acquisition_resource_count(const edge_acquisition *acquisition) {
    return acquisition != NULL ? acquisition->link_count : 0U;
}
