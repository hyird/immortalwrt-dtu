#define _GNU_SOURCE

#include "edge_acquisition_internal.h"

void device_detail(const edge_acquisition_device *device, const char *operation,
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

void log_io_result(edge_acquisition_device *device, edge_io_result result,
                   const char *operation) {
    const int64_t now = current_ms();
    const unsigned stage = !strcmp(operation, "connect") ? 1U :
        !strcmp(operation, "s7-cotp") ? 2U :
        !strcmp(operation, "s7-setup") ? 3U :
        !strcmp(operation, "handshake") ? 4U : 5U;
    if (!device->local_io_seen[stage] || device->local_io_result[stage] != result ||
        (result != EDGE_IO_OK && now - device->local_io_log_ms[stage] >= EDGE_IO_LOG_REPEAT_MS)) {
        edge_log_local_io(device->platform_id, device->config->device_id.bytes,
                          (unsigned)device->config->protocol, operation, (unsigned)result);
        device->local_io_seen[stage] = true;
        device->local_io_result[stage] = result;
        device->local_io_log_ms[stage] = now;
    }
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

void log_incomplete_sample(edge_acquisition_device *device) {
    unsigned invalid = 0U;
    for (size_t index = 0U; index < device->point_count; ++index)
        invalid += !device->points[index].valid;
    if (invalid == 0U) {
        device->last_sample_failure_log_ms = 0;
        return;
    }
    const int64_t now = current_ms();
    if (device->last_sample_failure_log_ms == 0 ||
        now - device->last_sample_failure_log_ms >= EDGE_IO_LOG_REPEAT_MS) {
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_SAMPLE_INCOMPLETE, invalid);
        device->last_sample_failure_log_ms = now;
    }
}

int open_link_monitor(void) {
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
            edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                                 (unsigned)device->config->protocol,
                                 EDGE_LOCAL_FAULT_NETWORK, 1U);
        } else {
            device->last_error[0] = '\0';
            device->has_last_io_result = false;
            edge_log_write("info", "network", "device network link up", detail);
        }
    }
}

void drain_link_events(edge_acquisition *acquisition, int link_fd) {
    uint8_t buffer[EDGE_LINK_EVENT_BUFFER_SIZE];
    for (;;) {
        const ssize_t size = recv(link_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (size <= 0)
            return;
        unsigned int remaining = (unsigned int)size;
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

static int wait_fd_with_timeout(int fd, short events, bool *timed_out) {
    if (timed_out != NULL)
        *timed_out = false;
    struct pollfd descriptor = {.fd = fd, .events = events};
    for (;;) {
        const int result = poll(&descriptor, 1U, EDGE_IO_TIMEOUT_MS);
        if (result < 0 && errno == EINTR)
            continue;
        if (result == 0) {
            if (timed_out != NULL)
                *timed_out = true;
            return -1;
        }
        if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return -1;
        return (descriptor.revents & events) != 0 ? 0 : -1;
    }
}

static int wait_fd(int fd, short events) {
    return wait_fd_with_timeout(fd, events, NULL);
}

void debug_acquisition_state(edge_acquisition_device *device, const char *state) {
    if (!automatic_debug_enabled(device) ||
        (!device->endpoint->debug_enabled && !device->config->debug_enabled) || !device->owner->debug) return;
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

void finish_debug_acquisition(edge_acquisition_device *device, edge_io_result result) {
    bool partial = false;
    for (size_t index = 0; index < device->point_count; ++index) partial |= device->points[index].valid;
    debug_acquisition_state(device, result == EDGE_IO_OK ? "success" : partial ? "partial" : "failed");
}

void debug_packet_with_id(edge_acquisition_device *device, const uint8_t id[16], const char *direction,
                          const uint8_t *data, size_t size, bool identified, bool device_only) {
    if (!automatic_debug_enabled(device) ||
        (!device->endpoint->debug_enabled && !device->config->debug_enabled) ||
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

void debug_packet(edge_acquisition_device *device, const char *direction,
                  const uint8_t *data, size_t size, bool identified, bool device_only) {
    uint8_t id[16]; record_id((uint64_t)current_ms(), id);
    debug_packet_with_id(device, id, direction, data, size, identified, device_only);
}

void debug_request_status(edge_acquisition_device *device, const char *status,
                          const char *reason, bool finished) {
    iot_edge_v1_RawPacket *packet = device->debug_request;
    if (!packet) return;
    copy_text(packet->status, sizeof(packet->status), status);
    copy_text(packet->reason, sizeof(packet->reason), reason ? reason : "");
    if (device->owner->debug)
        device->owner->debug(device->owner->callback_context, device->platform_id, packet);
    if (finished) { free(packet); device->debug_request = NULL; }
}

void debug_request_begin(edge_acquisition_device *device, const uint8_t *request, size_t size) {
    debug_request_status(device, "failed", "request_replaced", true);
    if (!automatic_debug_enabled(device) ||
        (!device->endpoint->debug_enabled && !device->config->debug_enabled) ||
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

static bool command_start_expired(edge_acquisition_device *device) {
    if (!device->command_start_check || device->runtime.southbound_write_started)
        return false;
    const uint64_t now = monotonic_milliseconds();
    if (now != 0U && now < device->command_start_before_monotonic_ms &&
        now < device->mapping_valid_until_monotonic_ms &&
        command_clock_generation_is_current(device->owner, device->platform_id,
                                            device->command_clock_generation))
        return false;
    device->command_start_expired = true;
    return true;
}

static bool write_all(edge_acquisition_device *device, const uint8_t *data, size_t size) {
    const int fd = device->link->fd;
    size_t offset = 0U;
    while (offset < size) {
        if (command_start_expired(device) || serial_paused(device)) return false;
        if (wait_fd(fd, POLLOUT) != 0) {
            (void)command_start_expired(device);
            return false;
        }
        const bool guarded_first_write = device->command_start_check &&
            !device->runtime.southbound_write_started;
        /* This narrows (but cannot atomically close) the check-to-write race. */
        if (guarded_first_write && !command_first_write_is_valid(device))
            return false;
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        if (guarded_first_write) {
            device->runtime.southbound_write_started = true;
            if (device->runtime.write_count != 0U) {
                memcpy(device->southbound_command_id,
                       device->runtime.writes[device->runtime.write_head].command_id, 16U);
                device->southbound_command_id_valid = true;
            }
        }
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

static bool read_exact_with_timeout(edge_acquisition_device *device, uint8_t *data,
                                    size_t size, bool *timed_out) {
    if (timed_out != NULL)
        *timed_out = false;
    const int fd = device->link->fd;
    size_t offset = 0U;
    while (offset < size) {
        bool wait_timed_out = false;
        if (wait_fd_with_timeout(fd, POLLIN, &wait_timed_out) != 0) {
            if (timed_out != NULL)
                *timed_out = wait_timed_out;
            return false;
        }
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

static bool read_exact(edge_acquisition_device *device, uint8_t *data, size_t size) {
    return read_exact_with_timeout(device, data, size, NULL);
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

int open_serial(const iot_edge_v1_SerialSettings *settings) {
    int fd = open(settings->channel, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (!configure_serial(fd, settings)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool prepare_serial_task(edge_acquisition_device *device) {
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

edge_io_result device_connect(void *context) {
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

void device_disconnect(void *context) {
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
                       size_t capacity, size_t *size, bool *timed_out) {
    if (timed_out != NULL)
        *timed_out = false;
    if (!read_exact_with_timeout(device, frame, 4U, timed_out))
        return false;
    const size_t length = ((size_t)frame[2] << 8U) | frame[3];
    if (length < 7U || length > capacity ||
        !read_exact_with_timeout(device, frame + 4U, length - 4U, timed_out))
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

bool exchange_with_receive_timeout(edge_acquisition_device *device,
                                   const uint8_t *request, size_t request_size,
                                   uint8_t *response, size_t capacity,
                                   size_t *response_size,
                                   bool *receive_timed_out) {
    if (receive_timed_out != NULL)
        *receive_timed_out = false;
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
    if (automatic_debug_enabled(device))
        edge_log_packet(log_source(device), "tx", device_label(device), logged_request, request_size);
    debug_request_status(device, "waiting", "", false);
    bool received = false;
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7) {
        received = receive_s7(device, response, capacity, response_size,
                              receive_timed_out);
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
            if (automatic_debug_enabled(device))
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
        if (automatic_debug_enabled(device))
            edge_log_packet(log_source(device), "rx", device_label(device), logged_response,
                            *response_size);
    } else if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 ||
               edge_industrial_protocol(device->config->protocol)) {
        close_fd(&device->link->fd);
    }
    if (!received) debug_request_status(device, "failed", "response_timeout_or_connection_closed", true);
    return received;
}

bool exchange(edge_acquisition_device *device, const uint8_t *request,
              size_t request_size, uint8_t *response, size_t capacity,
              size_t *response_size) {
    return exchange_with_receive_timeout(device, request, request_size, response,
                                         capacity, response_size, NULL);
}

bool write_command_exchange(edge_acquisition_device *device,
                            const uint8_t *request, size_t request_size,
                            uint8_t *response, size_t capacity,
                            size_t *response_size) {
    device->command_start_expired = false;
    device->command_start_check = true;
    const bool exchanged = exchange(device, request, request_size, response,
                                    capacity, response_size);
    device->command_start_check = false;
    return exchanged;
}

edge_io_result failed_command_exchange(const edge_acquisition_device *device) {
    if (device->command_start_expired && !device->runtime.southbound_write_started)
        return EDGE_IO_COMMAND_START_EXPIRED;
    return device->link->fd < 0 ? EDGE_IO_OFFLINE : EDGE_IO_NO_RESPONSE;
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
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_PROTOCOL_DECODE, 4U);
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
        edge_log_local_fault(device->platform_id, device->config->device_id.bytes,
                             (unsigned)device->config->protocol,
                             EDGE_LOCAL_FAULT_PROTOCOL_DECODE, 5U);
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

edge_io_result device_handshake(void *context) {
    edge_acquisition_device *device = context;
    const bool standalone = !device->acquisition_active;
    if (standalone) {
        record_id((uint64_t)current_ms(), device->acquisition_id);
        device->acquisition_active = true;
        debug_acquisition_state(device, "running");
    }
    const edge_io_result result = handshake_acquisition(context);
    if (device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_FINS)
        log_io_result(device, result, "handshake");
    if (standalone) {
        finish_debug_acquisition(device, result);
        device->acquisition_active = false;
    }
    return result;
}
