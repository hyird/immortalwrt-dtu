#define _GNU_SOURCE
#include "edge_dtu.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint8_t *bytes; size_t offset, size, capacity; } dtu_queue;
typedef struct {
    int fd;
    bool connecting;
    uint64_t retry_at, deadline;
    uint32_t backoff;
    dtu_queue queue;
} dtu_peer;

static uint64_t clock_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void disconnect_peer(dtu_peer *peer) {
    if (peer->fd >= 0) close(peer->fd);
    peer->fd = -1;
    peer->connecting = false;
    peer->queue.size = peer->queue.offset = 0;
    peer->backoff = peer->backoff ? peer->backoff * 2U : 1000U;
    if (peer->backoff > 30000U) peer->backoff = 30000U;
    peer->retry_at = clock_ms() + peer->backoff;
}

static void retry_connection(dtu_peer *peer) {
    /* Data accepted while disconnected has never been sent: retain it across dial failures. */
    const size_t size = peer->queue.size, offset = peer->queue.offset;
    disconnect_peer(peer);
    peer->queue.size = size;
    peer->queue.offset = offset;
}

static bool enqueue(dtu_queue *queue, const uint8_t *bytes, size_t size) {
    if (size > queue->capacity - queue->size) return false;
    if (queue->offset + queue->size + size > queue->capacity) {
        memmove(queue->bytes, queue->bytes + queue->offset, queue->size);
        queue->offset = 0;
    }
    memcpy(queue->bytes + queue->offset + queue->size, bytes, size);
    queue->size += size;
    return true;
}

static void trace(const iot_edge_v1_DtuConfig *config, iot_edge_v1_DtuStatus *status,
                  const char *direction, size_t slot, const uint8_t *bytes, size_t size) {
    if (!config->debug_enabled) return;
    const uint64_t now = clock_ms();
    uint64_t sequence = status->traces_count ? status->traces[status->traces_count - 1].sequence : 0;
    if (status->traces_count && now - status->traces[0].monotonic_ms >= 1000) status->traces_count = 0;
    if (status->traces_count >= 8) { ++status->omitted_traces; return; }
    iot_edge_v1_DtuTrace *entry = &status->traces[status->traces_count++];
    *entry = (iot_edge_v1_DtuTrace)iot_edge_v1_DtuTrace_init_zero;
    entry->sequence = sequence + 1;
    entry->monotonic_ms = now;
    snprintf(entry->direction, sizeof(entry->direction), "%s", direction);
    entry->client_slot = (uint32_t)slot;
    entry->total_bytes = (uint32_t)size;
    entry->payload.size = (pb_size_t)(size > sizeof(entry->payload.bytes) ? sizeof(entry->payload.bytes) : size);
    memcpy(entry->payload.bytes, bytes, entry->payload.size);
}

static bool flush(dtu_peer *peer, bool serial, uint64_t *counter,
                  const iot_edge_v1_DtuConfig *config, iot_edge_v1_DtuStatus *status, size_t slot, size_t limit) {
    dtu_queue *queue = &peer->queue;
    const size_t requested = queue->size < limit ? queue->size : limit;
    ssize_t size = serial ? write(peer->fd, queue->bytes + queue->offset, requested)
                         : send(peer->fd, queue->bytes + queue->offset, requested, MSG_NOSIGNAL);
    if (size < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    if (size == 0) return false;
    trace(config, status, slot ? "south-tx" : "north-tx", slot,
        queue->bytes + queue->offset, (size_t)size);
    queue->offset += (size_t)size;
    queue->size -= (size_t)size;
    *counter += (uint64_t)size;
    if (!queue->size) queue->offset = 0;
    return true;
}

static void keepalive(int fd) {
    int on = 1, idle = 60, interval = 10, count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/* DNS may block only this channel's child, never acquisition or other relays. */
static int tcp_socket(const char *host, uint32_t port, bool listener) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    char service[8];
    snprintf(service, sizeof(service), "%u", port);
    if (listener) hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host[0] ? host : NULL, service, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        if (listener) {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            if (!bind(fd, address->ai_addr, address->ai_addrlen) &&
                !listen(fd, EDGE_DTU_MAX_CLIENTS)) break;
        } else {
            keepalive(fd);
            if (!connect(fd, address->ai_addr, address->ai_addrlen) || errno == EINPROGRESS) break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static speed_t baud_speed(uint32_t baud) {
    switch (baud) {
    case 300: return B300; case 600: return B600; case 1200: return B1200;
    case 2400: return B2400; case 4800: return B4800; case 9600: return B9600;
    case 19200: return B19200; case 38400: return B38400; case 57600: return B57600;
    case 115200: return B115200; case 230400: return B230400; default: return 0;
    }
}

static int serial_open(const iot_edge_v1_SerialSettings *settings) {
    int fd = open(settings->channel, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct termios attrs;
    if (ioctl(fd, TIOCEXCL) || tcgetattr(fd, &attrs)) goto failed;
    cfmakeraw(&attrs);
    attrs.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    attrs.c_cflag |= CLOCAL | CREAD;
    attrs.c_cflag |= settings->data_bits == 5 ? CS5 : settings->data_bits == 6 ? CS6 :
                    settings->data_bits == 7 ? CS7 : CS8;
    if (!strcmp(settings->parity, "even")) attrs.c_cflag |= PARENB;
    if (!strcmp(settings->parity, "odd")) attrs.c_cflag |= PARENB | PARODD;
    if (settings->stop_bits == 2) attrs.c_cflag |= CSTOPB;
    attrs.c_cc[VMIN] = 1;
    attrs.c_cc[VTIME] = 0;
    speed_t speed = baud_speed(settings->baud_rate);
    if (!speed || cfsetispeed(&attrs, speed) || cfsetospeed(&attrs, speed) ||
        tcsetattr(fd, TCSANOW, &attrs)) goto failed;
    if (settings->rs485) {
        struct serial_rs485 rs485 = {0};
        rs485.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
        rs485.delay_rts_before_send = (settings->rs485_rts_delay_before_us + 999U) / 1000U;
        rs485.delay_rts_after_send = (settings->rs485_rts_delay_after_us + 999U) / 1000U;
        if (ioctl(fd, TIOCSRS485, &rs485)) goto failed;
    }
    return fd;
failed:
    close(fd);
    return -1;
}

static bool connected(dtu_peer *peer) {
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(peer->fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) return false;
    peer->connecting = false;
    peer->backoff = 0;
    return true;
}

void edge_dtu_run(const iot_edge_v1_DtuConfig *config,
                  edge_dtu_status_callback callback, void *context) {
    const bool serial = config->south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL;
    const bool server = config->south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    const size_t count = server ? config->max_clients : 1U;
    if (!count || count > EDGE_DTU_MAX_CLIENTS || config->queue_bytes < 4096U ||
        config->queue_bytes > 65536U || config->heartbeat_interval_sec > 86400U ||
        (config->heartbeat_interval_sec && !config->heartbeat.size)) return;
    dtu_peer peers[EDGE_DTU_MAX_CLIENTS + 1] = {0};
    for (size_t i = 0; i <= count; ++i) {
        peers[i].fd = -1;
        peers[i].queue.capacity = config->queue_bytes;
        peers[i].queue.bytes = malloc(config->queue_bytes);
        if (!peers[i].queue.bytes) _exit(1);
    }
    dtu_peer *north = &peers[0];
    int listener = -1;
    uint64_t listen_retry = 0, frame_deadline = 0, next_status = 0;
    size_t registration_offset = 0;
    const uint64_t heartbeat_interval = (uint64_t)config->heartbeat_interval_sec * 1000U;
    uint64_t heartbeat_at = 0;
    size_t heartbeat_offset = 0, heartbeat_before = 0;
    bool heartbeat_pending = false;
    iot_edge_v1_DtuStatus status = iot_edge_v1_DtuStatus_init_zero;
    iot_edge_v1_DtuStatus published = iot_edge_v1_DtuStatus_init_zero;
    status.channel_id.size = 16;
    memcpy(status.channel_id.bytes, config->channel_id.bytes, 16);
    for (;;) {
        const uint64_t now = clock_ms();
        if (north->fd < 0 && now >= north->retry_at) {
            north->fd = tcp_socket(config->north_host, config->north_port, false);
            if (north->fd < 0) retry_connection(north);
            else {
                north->connecting = true; north->deadline = now + 10000U; registration_offset = 0;
                heartbeat_pending = false; heartbeat_offset = heartbeat_before = 0;
            }
        }
        if (server && listener < 0 && now >= listen_retry) {
            listener = tcp_socket(config->south_host, config->south_port, true);
            listen_retry = now + 3000U;
        }
        if (!server && peers[1].fd < 0 && now >= peers[1].retry_at) {
            peers[1].fd = serial ? serial_open(&config->serial) :
                tcp_socket(config->south_host, config->south_port, false);
            if (peers[1].fd < 0) disconnect_peer(&peers[1]);
            else { peers[1].connecting = !serial; peers[1].deadline = now + 10000U; }
        }
        if (heartbeat_interval && north->fd >= 0 && !north->connecting &&
            registration_offset == config->registration.size && !heartbeat_pending && now >= heartbeat_at) {
            heartbeat_pending = true;
            heartbeat_offset = 0;
            /* Drain only bytes already queued before the heartbeat; new uplink cannot starve it. */
            heartbeat_before = north->queue.size;
        }
        struct pollfd fds[EDGE_DTU_MAX_CLIENTS + 2] = {0};
        fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (size_t i = 0; i <= count; ++i) {
            dtu_peer *peer = &peers[i];
            short events = 0;
            if (peer->connecting || (peer->queue.size && (!i ? now >= frame_deadline : true)) ||
                (!i && (registration_offset < config->registration.size || heartbeat_pending))) events |= POLLOUT;
            if (!peer->connecting && (!i || north->queue.size < north->queue.capacity)) events |= POLLIN;
            fds[i + 1] = (struct pollfd){.fd = peer->fd, .events = events};
        }
        int ready = poll(fds, count + 2U, 20);
        if (ready < 0 && errno != EINTR) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { close(listener); listener = -1; }
        if (fds[0].revents & POLLIN) {
            int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                size_t slot = 1;
                while (slot <= count && peers[slot].fd >= 0) ++slot;
                if (slot > count) close(fd);
                else { peers[slot].fd = fd; peers[slot].backoff = 0; keepalive(fd); }
            }
        }
        for (size_t i = 0; i <= count; ++i) {
            dtu_peer *peer = &peers[i];
            const short events = fds[i + 1].revents;
            if (peer->fd < 0 || fds[i + 1].fd != peer->fd) continue;
            if (peer->connecting) {
                if ((events & (POLLOUT | POLLERR | POLLHUP)) && connected(peer)) {
                    if (!i) heartbeat_at = clock_ms() + heartbeat_interval;
                    continue;
                }
                if ((events & (POLLOUT | POLLERR | POLLHUP)) || now >= peer->deadline) retry_connection(peer);
                continue;
            }
            if (events & POLLIN) {
                uint8_t bytes[4096];
                size_t capacity = i ? north->queue.capacity - north->queue.size : sizeof(bytes);
                if (capacity > sizeof(bytes)) capacity = sizeof(bytes);
                ssize_t n = capacity ? read(peer->fd, bytes, capacity) : -1;
                if (n > 0) {
                    trace(config, &status, i ? "south-rx" : "north-rx", i, bytes, (size_t)n);
                    if (i) {
                        enqueue(&north->queue, bytes, (size_t)n);
                        frame_deadline = serial ? now + config->serial_frame_ms : 0;
                        if (north->queue.size == north->queue.capacity) frame_deadline = 0;
                    } else if (!config->uplink_only) {
                        for (size_t j = 1; j <= count; ++j) {
                            if (peers[j].fd < 0 || peers[j].connecting) continue;
                            if ((size_t)n > peers[j].queue.capacity - peers[j].queue.size && peers[j].queue.size &&
                                !flush(&peers[j], serial, &status.downstream_bytes, config, &status, j, SIZE_MAX))
                                disconnect_peer(&peers[j]);
                            if (peers[j].fd >= 0 && !enqueue(&peers[j].queue, bytes, (size_t)n)) {
                                disconnect_peer(&peers[j]);
                                snprintf(status.error, sizeof(status.error), "slow south client disconnected: queue full");
                            }
                        }
                    }
                } else if (n == 0 || (capacity && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    disconnect_peer(peer);
                    snprintf(status.error, sizeof(status.error), "%s disconnected; unsent bytes discarded", i ? "south" : "north");
                    continue;
                }
            }
            if ((events & POLLOUT) && peer->fd >= 0) {
                if (!i && registration_offset < config->registration.size) {
                    ssize_t n = send(peer->fd, config->registration.bytes + registration_offset,
                                     config->registration.size - registration_offset, MSG_NOSIGNAL);
                    if (n > 0) {
                        trace(config, &status, "registration", 0, config->registration.bytes + registration_offset, (size_t)n);
                        registration_offset += (size_t)n;
                        if (registration_offset == config->registration.size)
                            heartbeat_at = clock_ms() + heartbeat_interval;
                    }
                    else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) disconnect_peer(peer);
                } else if (!i && heartbeat_pending && !heartbeat_before) {
                    ssize_t n = send(peer->fd, config->heartbeat.bytes + heartbeat_offset,
                                     config->heartbeat.size - heartbeat_offset, MSG_NOSIGNAL);
                    if (n > 0) {
                        trace(config, &status, "heartbeat", 0, config->heartbeat.bytes + heartbeat_offset, (size_t)n);
                        heartbeat_offset += (size_t)n;
                        if (heartbeat_offset == config->heartbeat.size) {
                            heartbeat_pending = false;
                            heartbeat_at = clock_ms() + heartbeat_interval;
                        }
                    } else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) disconnect_peer(peer);
                } else if (peer->queue.size) {
                    const uint64_t before = status.upstream_bytes;
                    if (!flush(peer, serial && i, i ? &status.downstream_bytes : &status.upstream_bytes,
                               config, &status, i, !i && heartbeat_pending ? heartbeat_before : SIZE_MAX))
                        disconnect_peer(peer);
                    if (!i && heartbeat_pending) heartbeat_before -= (size_t)(status.upstream_bytes - before);
                }
            }
            if (peer->fd >= 0 && (events & (POLLERR | POLLHUP | POLLNVAL)) && !(events & POLLIN)) disconnect_peer(peer);
        }
        status.client_count = 0;
        status.queued_bytes = (uint32_t)north->queue.size;
        for (size_t i = 1; i <= count; ++i) {
            if (peers[i].fd >= 0 && !peers[i].connecting) ++status.client_count;
            status.queued_bytes += (uint32_t)peers[i].queue.size;
        }
        snprintf(status.north_state, sizeof(status.north_state), "%s", north->fd < 0 ? "reconnecting" : north->connecting ? "connecting" : "connected");
        snprintf(status.south_state, sizeof(status.south_state), "%s", status.client_count ? "connected" : server && listener >= 0 ? "listening" : "reconnecting");
        if (now >= next_status && memcmp(&status, &published, sizeof(status))) {
            if (!callback || callback(context, &status)) published = status;
            next_status = now + 1000U;
        }
    }
    for (size_t i = 0; i <= count; ++i) { disconnect_peer(&peers[i]); free(peers[i].queue.bytes); }
    if (listener >= 0) close(listener);
}
