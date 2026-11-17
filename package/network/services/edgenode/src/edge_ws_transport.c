#include "edge_ws_transport.h"
#include "edge_process.h"
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>

#if defined(LWS_WITHOUT_EXTENSIONS) || !defined(LWS_WITH_LIBEV)
#error "EdgeNode requires libwebsockets permessage-deflate and built-in libev support"
#endif

typedef struct edge_ws_packet edge_ws_packet;
typedef struct {
    struct lws_context *context;
    struct lws *socket;
    edge_ws_packet *head;
    edge_ws_packet *tail;
    size_t queued_bytes;
    size_t queued_packets;
    uint8_t *incoming;
    size_t incoming_size;
    size_t message_limit;
    bool stopping;
    bool connected;
    bool notified;
    bool closing;
    bool incoming_binary;
    uint16_t close_code;
    char close_reason[124];
    char url[1024];
    char path[1024];
    char host[1024];
    void *user;
    void (*opened)(void *user);
    void (*received)(void *user, void *data, size_t size, bool binary);
    void (*closed)(void *user, int code, const char *reason);
} edge_native_transport;

static void native_destroy(edge_native_transport *transport);
static void worker_sent(void *user, size_t size);

#define EDGE_WS_QUEUE_LIMIT (256U * 1024U)
#define EDGE_WS_UNACKNOWLEDGED_TIMEOUT_MS 10000U

struct edge_ws_packet {
    edge_ws_packet *next;
    size_t size;
    unsigned char bytes[];
};

static void disconnected(edge_native_transport *transport, int code, const char *reason) {
    transport->socket = NULL;
    transport->connected = false;
    if (!transport->stopping && !transport->notified) {
        transport->notified = true;
        transport->closed(transport->user, code, reason);
    }
}

static void remember_close(edge_native_transport *transport, uint16_t code,
                           const void *reason, size_t size) {
    const unsigned char *text = reason;
    transport->close_code = code;
    if (size >= sizeof(transport->close_reason))
        size = sizeof(transport->close_reason) - 1U;
    for (size_t i = 0; i < size; ++i)
        transport->close_reason[i] = text[i] < 0x20 || text[i] == 0x7f ? ' ' : (char)text[i];
    transport->close_reason[size] = '\0';
}

static int websocket_callback(struct lws *socket, enum lws_callback_reasons reason,
                              void *user, void *input, size_t size) {
    edge_native_transport *transport = lws_context_user(lws_get_context(socket));
    if (!transport || transport->stopping) return 0;
    switch (reason) {
    case LWS_CALLBACK_WS_EXT_DEFAULTS:
        if (user && strcmp(user, "permessage-deflate") == 0)
            snprintf(input, size, "compression_level=9; mem_level=8");
        break;
    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
        /* A stuck uplink must reconnect; the per-platform outbox keeps its
         * unacknowledged telemetry and command results for replay. */
        const unsigned int timeout = EDGE_WS_UNACKNOWLEDGED_TIMEOUT_MS;
        const int fd = lws_get_socket_fd(socket);
        if (fd < 0 || setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                                 &timeout, sizeof(timeout)) != 0) {
            syslog(LOG_ERR, "cannot configure websocket TCP user timeout: %s",
                   fd < 0 ? "socket unavailable" : strerror(errno));
            return -1;
        }
        transport->socket = socket;
        transport->connected = true;
        transport->opened(transport->user);
        break;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (transport->closing) return -1;
        if (!transport->incoming_size)
            transport->incoming_binary = lws_frame_is_binary(socket);
        if (size > transport->message_limit - transport->incoming_size) {
            remember_close(transport, 1009, "message too large", 17);
            lws_close_reason(socket, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, NULL, 0);
            return -1;
        }
        if (!transport->incoming) {
            transport->incoming = malloc(transport->message_limit);
            if (!transport->incoming) {
                remember_close(transport, 1011, "receive allocation failed", 25);
                lws_close_reason(socket, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
                return -1;
            }
        }
        if (size) memcpy(transport->incoming + transport->incoming_size, input, size);
        transport->incoming_size += size;
        if (lws_is_final_fragment(socket) && !lws_remaining_packet_payload(socket)) {
            transport->received(transport->user, transport->incoming,
                                transport->incoming_size, transport->incoming_binary);
            transport->incoming_size = 0;
        }
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (transport->closing) {
            lws_close_reason(socket, (enum lws_close_status)transport->close_code,
                             (unsigned char *)transport->close_reason,
                             strlen(transport->close_reason));
            return -1;
        }
        edge_ws_packet *packet = transport->head;
        if (!packet) break;
        /* 每次 writable 仅提交一条完整业务消息；LWS 管理压缩及部分写入。 */
        int written = lws_write(socket, packet->bytes + LWS_PRE, packet->size,
                                LWS_WRITE_BINARY);
        if (written < 0 || (size_t)written < packet->size) return -1;
        transport->head = packet->next;
        if (!transport->head) transport->tail = NULL;
        transport->queued_bytes -= packet->size;
        --transport->queued_packets;
        worker_sent(transport->user, packet->size);
        free(packet);
        if (transport->head) lws_callback_on_writable(socket);
        break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        disconnected(transport, 1006, input ? (const char *)input : "connection failed");
        break;
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
        const unsigned char *payload = input;
        /* LWS supplies the close frame payload, without a terminating NUL. */
        if (size >= 2U)
            remember_close(transport, (uint16_t)((payload[0] << 8) | payload[1]),
                           payload + 2, size - 2U);
        else
            remember_close(transport, 1005, "peer closed without status", 26);
        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
        disconnected(transport, transport->close_code ? transport->close_code : 1006,
                     transport->close_code ? transport->close_reason :
                     "connection lost without close frame");
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { .name = "edgenode", .callback = websocket_callback, .rx_buffer_size = 4096 },
    { .name = NULL }
};

/* WS 原生 Ping/Pong 负责连接存活；应用状态上报不续期连接看门狗。 */
static const lws_retry_bo_t idle_policy = {
    .secs_since_valid_ping = 300,
    .secs_since_valid_hangup = 360,
};

static const struct lws_extension extensions[] = {
    { "permessage-deflate", lws_extension_callback_pm_deflate, "permessage-deflate" },
    { NULL, NULL, NULL }
};

static bool native_connect(edge_native_transport *transport, struct ev_loop *loop,
                               const char *url, size_t message_limit, void *user,
                               void (*opened)(void *),
                               void (*received)(void *, void *, size_t, bool),
                               void (*closed)(void *, int, const char *)) {
    native_destroy(transport);
    if (!loop || !url || strlen(url) >= sizeof(transport->url) || !message_limit ||
        message_limit > INT_MAX || !opened || !received || !closed) return false;
    transport->user = user;
    transport->opened = opened;
    transport->received = received;
    transport->closed = closed;
    transport->message_limit = message_limit;
    strcpy(transport->url, url);
    const char *scheme, *address, *path;
    int port;
    if (lws_parse_uri(transport->url, &scheme, &address, &port, &path) ||
        (strcmp(scheme, "ws") && strcmp(scheme, "wss"))) return false;
    if (snprintf(transport->path, sizeof(transport->path), "/%s", path) >=
        (int)sizeof(transport->path)) return false;
    bool tls = strcmp(scheme, "wss") == 0;
    bool ipv6 = strchr(address, ':') != NULL;
    if (snprintf(transport->host, sizeof(transport->host), ipv6 ? "[%s]:%d" : "%s:%d",
                 address, port) >= (int)sizeof(transport->host)) return false;
    /* 借用 EdgeNode 已有 libev loop，LWS 不创建或销毁应用事件循环。
     * TLS、压缩残留、socket watcher 与传输定时器均由原生 libev 后端驱动。 */
    void *loops[] = { loop };
    struct lws_context_creation_info info = {
        .port = CONTEXT_PORT_NO_LISTEN,
        .protocols = protocols,
        .extensions = extensions,
        .options = LWS_SERVER_OPTION_LIBEV | LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT,
        .foreign_loops = loops,
        .user = transport,
        .client_ssl_ca_filepath = "/etc/ssl/certs/ca-certificates.crt",
        .timeout_secs = 30,
        .fd_limit_per_thread = 8,
    };
    transport->context = lws_create_context(&info);
    if (!transport->context) return false;
    struct lws_client_connect_info connection = {
        .context = transport->context,
        .address = address,
        .port = port,
        .path = transport->path,
        .host = transport->host,
        .ssl_connection = tls ? LCCSCF_USE_SSL : 0,
        .local_protocol_name = "edgenode",
        .pwsi = &transport->socket,
        .retry_and_idle_policy = &idle_policy,
    };
    /* 不发送子协议、不弱化证书策略；不主动启用周期 WebSocket Ping。 */
    transport->notified = true; /* 同步失败由调用方安排一次重连。 */
    if (!lws_client_connect_via_info(&connection)) {
        native_destroy(transport);
        return false;
    }
    transport->notified = false;
    return true;
}

static bool native_send(edge_native_transport *transport, const void *data, size_t size) {
    if (!transport->connected || transport->closing || !data || !size ||
        size > transport->message_limit || transport->queued_packets >= 256U ||
        size > EDGE_WS_QUEUE_LIMIT - transport->queued_bytes)
        return false;
    edge_ws_packet *packet = malloc(sizeof(*packet) + LWS_PRE + size);
    if (!packet) return false;
    packet->next = NULL;
    packet->size = size;
    memcpy(packet->bytes + LWS_PRE, data, size);
    if (transport->tail) transport->tail->next = packet;
    else transport->head = packet;
    transport->tail = packet;
    transport->queued_bytes += size;
    ++transport->queued_packets;
    lws_callback_on_writable(transport->socket);
    return true;
}

static void native_close(edge_native_transport *transport, uint16_t code, const char *reason) {
    if (!transport->socket) return;
    transport->closing = true;
    remember_close(transport, code, reason, reason ? strlen(reason) : 0U);
    lws_callback_on_writable(transport->socket);
}

static void native_destroy(edge_native_transport *transport) {
    transport->stopping = true;
    if (transport->context) lws_context_destroy(transport->context);
    while (transport->head) {
        edge_ws_packet *next = transport->head->next;
        free(transport->head);
        transport->head = next;
    }
    free(transport->incoming);
    memset(transport, 0, sizeof(*transport));
}

/* A platform's resolver, TLS engine and inflater live in a disposable process.
 * SOCK_SEQPACKET preserves envelopes and bounds kernel buffering. Both sides
 * use nonblocking I/O; a stalled peer cannot hold the shared application loop.
 * The parent services at most four inbound messages per 10 ms per platform. */
#define EDGE_WS_IPC_MESSAGE_LIMIT 16384U
#define EDGE_WS_IPC_BATCH 4U
#define EDGE_WS_IPC_INTERVAL 0.01
#define EDGE_WS_QUEUE_PACKETS 256U

enum { IPC_OPEN = 1, IPC_BINARY, IPC_TEXT, IPC_CLOSE, IPC_PROGRESS, IPC_SENT };
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t code;
    unsigned char bytes[EDGE_WS_IPC_MESSAGE_LIMIT];
} edge_ws_ipc;

struct edge_ws_bridge {
    struct ev_loop *loop;
    struct ev_io io;
    struct ev_timer timer;
    struct ev_timer watchdog;
    pid_t pid;
    int fd;
    int socket_fd;
    size_t limit;
    edge_ws_packet *head;
    edge_ws_packet *tail;
    size_t bytes;
    size_t packets;
    ev_tstamp last_progress;
    bool connected;
    bool closing;
    bool close_sent;
    bool notified;
    bool polling;
    bool retired;
    bool cooldown;
    uint16_t close_code;
    char close_reason[124];
    void *user;
    void (*opened)(void *);
    void (*received)(void *, void *, size_t, bool);
    void (*closed)(void *, int, const char *);
};

typedef struct {
    edge_native_transport native;
    struct ev_loop *loop;
    struct ev_io io;
    struct ev_timer timer;
    struct ev_timer progress_timer;
    int fd;
} edge_ws_worker;

static bool ipc_send(int fd, uint32_t type, uint32_t code,
                     const void *data, size_t size) {
    if (size > EDGE_WS_IPC_MESSAGE_LIMIT) return false;
    edge_ws_ipc message;
    message.type = type;
    message.size = (uint32_t)size;
    message.code = code;
    if (size) memcpy(message.bytes, data, size);
    const size_t length = offsetof(edge_ws_ipc, bytes) + size;
    return send(fd, &message, length, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)length;
}

/* 1 = complete message, 0 = would block, -1 = closed or malformed. */
static int ipc_receive(int fd, edge_ws_ipc *message, int *passed_fd) {
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct iovec iov = {.iov_base = message, .iov_len = sizeof(*message)};
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    *passed_fd = -1;
    const ssize_t length = recvmsg(fd, &header, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&header); length >= 0 && item;
         item = CMSG_NXTHDR(&header, item)) {
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS &&
            item->cmsg_len >= CMSG_LEN(sizeof(int)))
            memcpy(passed_fd, CMSG_DATA(item), sizeof(*passed_fd));
    }
    if ((header.msg_flags & MSG_CTRUNC) || length < (ssize_t)offsetof(edge_ws_ipc, bytes) ||
        length > (ssize_t)sizeof(*message) || message->size > sizeof(message->bytes) ||
        (size_t)length != offsetof(edge_ws_ipc, bytes) + message->size ||
        (*passed_fd >= 0 && message->type != IPC_OPEN)) {
        if (*passed_fd >= 0) close(*passed_fd);
        *passed_fd = -1;
        return -1;
    }
    return 1;
}

static void worker_opened(void *user) {
    edge_ws_worker *worker = user;
    const int socket_fd = lws_get_socket_fd(worker->native.socket);
    edge_ws_ipc message;
    memset(&message, 0, offsetof(edge_ws_ipc, bytes));
    message.type = IPC_OPEN;
    struct iovec iov = {.iov_base = &message, .iov_len = offsetof(edge_ws_ipc, bytes)};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *item = CMSG_FIRSTHDR(&header);
    item->cmsg_level = SOL_SOCKET;
    item->cmsg_type = SCM_RIGHTS;
    item->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(item), &socket_fd, sizeof(socket_fd));
    if (sendmsg(worker->fd, &header, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)iov.iov_len)
        _exit(1);
}

static void worker_sent(void *user, size_t size) {
    edge_ws_worker *worker = user;
    if (!ipc_send(worker->fd, IPC_SENT, (uint32_t)size, NULL, 0)) _exit(1);
    if (!ev_is_active(&worker->io) && !ev_is_active(&worker->timer) &&
        worker->native.queued_packets < EDGE_WS_QUEUE_PACKETS &&
        worker->native.queued_bytes <= EDGE_WS_QUEUE_LIMIT - EDGE_WS_IPC_MESSAGE_LIMIT)
        ev_io_start(worker->loop, &worker->io);
}

static void worker_received(void *user, void *data, size_t size, bool binary) {
    edge_ws_worker *worker = user;
    /* No unbounded inbound queue: overload terminates only this connection.
     * Reliable application messages retain their existing acknowledgement path. */
    if (!ipc_send(worker->fd, binary ? IPC_BINARY : IPC_TEXT, 0, data, size)) _exit(1);
}

static void worker_closed(void *user, int code, const char *reason) {
    edge_ws_worker *worker = user;
    const size_t size = reason ? strnlen(reason, 123U) : 0;
    (void)ipc_send(worker->fd, IPC_CLOSE, (uint32_t)code, reason, size);
    ev_break(worker->loop, EVBREAK_ALL);
}

static void worker_service(edge_ws_worker *worker) {
    unsigned i = 0;
    for (; i < EDGE_WS_IPC_BATCH; ++i) {
        /* Backpressure propagates through IPC instead of discarding accepted
         * outbound messages when the network queue is full. */
        if (worker->native.queued_packets >= EDGE_WS_QUEUE_PACKETS ||
            worker->native.queued_bytes > EDGE_WS_QUEUE_LIMIT - EDGE_WS_IPC_MESSAGE_LIMIT) {
            ev_io_stop(worker->loop, &worker->io);
            if (!ev_is_active(&worker->timer)) {
                ev_timer_set(&worker->timer, EDGE_WS_IPC_INTERVAL, 0.0);
                ev_timer_start(worker->loop, &worker->timer);
            }
            return;
        }
        edge_ws_ipc message;
        int passed_fd;
        const int result = ipc_receive(worker->fd, &message, &passed_fd);
        if (passed_fd >= 0) { close(passed_fd); _exit(1); }
        if (!result) {
            if (!ev_is_active(&worker->io))
                ev_io_start(worker->loop, &worker->io);
            return;
        }
        if (result < 0) _exit(0);
        if (message.type == IPC_CLOSE && message.size < 124U) {
            message.bytes[message.size] = 0;
            native_close(&worker->native, (uint16_t)message.code, (char *)message.bytes);
            return;
        }
        if (message.type != IPC_BINARY ||
            !native_send(&worker->native, message.bytes, message.size)) _exit(1);
    }
    ev_io_stop(worker->loop, &worker->io);
    if (!ev_is_active(&worker->timer)) {
        ev_timer_set(&worker->timer, EDGE_WS_IPC_INTERVAL, 0.0);
        ev_timer_start(worker->loop, &worker->timer);
    }
}

static void worker_io_cb(struct ev_loop *loop, struct ev_io *io, int events) {
    (void)loop;
    (void)events;
    worker_service(io->data);
}

static void worker_timer_cb(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    worker_service(timer->data);
}

static void worker_progress_cb(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_worker *worker = timer->data;
    if (!ipc_send(worker->fd, IPC_PROGRESS, 0, NULL, 0)) _exit(1);
}

static void worker_run(int fd, pid_t parent, const char *url, size_t limit) {
    /* Do not inherit libev signal handlers, PTYs, other platforms' sockets or
     * acquisition descriptors. A main-process death must also kill the worker. */
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(1);
    (void)prctl(PR_SET_NAME, "edgenode-ws", 0, 0, 0);
    edge_process_close_inherited_fds(fd);
    edge_ws_worker worker = {.fd = fd};
    worker.loop = ev_loop_new(EVFLAG_NOENV);
    if (!worker.loop) _exit(1);
    ev_io_init(&worker.io, worker_io_cb, fd, EV_READ);
    worker.io.data = &worker;
    ev_io_start(worker.loop, &worker.io);
    ev_timer_init(&worker.timer, worker_timer_cb, EDGE_WS_IPC_INTERVAL, 0.0);
    worker.timer.data = &worker;
    ev_timer_init(&worker.progress_timer, worker_progress_cb, 1.0, 1.0);
    worker.progress_timer.data = &worker;
    ev_timer_start(worker.loop, &worker.progress_timer);
    if (!native_connect(&worker.native, worker.loop, url, limit, &worker,
                        worker_opened, worker_received, worker_closed)) _exit(1);
    ev_run(worker.loop, 0);
    /* All native allocations and sockets belong to this worker. Exiting avoids
     * making parent progress depend on library shutdown. */
    _exit(0);
}

static void bridge_update_io(edge_ws_bridge *bridge) {
    if (bridge->retired || bridge->notified || bridge->fd < 0) {
        ev_io_stop(bridge->loop, &bridge->io);
        return;
    }
    int events = 0;
    if (!bridge->cooldown) {
        events |= EV_READ;
        if ((bridge->head && !bridge->closing) ||
            (bridge->closing && !bridge->close_sent))
            events |= EV_WRITE;
    }
    if (events == 0) {
        ev_io_stop(bridge->loop, &bridge->io);
    } else if (!ev_is_active(&bridge->io) ||
               (bridge->io.events & (EV_READ | EV_WRITE)) != events) {
        ev_io_stop(bridge->loop, &bridge->io);
        ev_io_set(&bridge->io, bridge->fd, events);
        ev_io_start(bridge->loop, &bridge->io);
    }
}

static void bridge_notify(edge_ws_bridge *bridge, int code, const char *reason) {
    if (bridge->notified) return;
    bridge->notified = true;
    bridge->connected = false;
    ev_io_stop(bridge->loop, &bridge->io);
    ev_timer_stop(bridge->loop, &bridge->timer);
    ev_timer_stop(bridge->loop, &bridge->watchdog);
    bridge->closed(bridge->user, code, reason);
}

static void bridge_service(edge_ws_bridge *bridge) {
    if (bridge->retired || bridge->notified) return;
    bridge->polling = true;
    unsigned received_count = 0;
    if (!bridge->cooldown) {
        for (; received_count < EDGE_WS_IPC_BATCH && !bridge->notified; ++received_count) {
            edge_ws_ipc message;
            int passed_fd;
            const int result = ipc_receive(bridge->fd, &message, &passed_fd);
            if (!result) break;
            if (result < 0) {
                bridge_notify(bridge, 1006, "platform transport worker stopped");
                break;
            }
            bridge->last_progress = ev_now(bridge->loop);
            if (message.type == IPC_OPEN && !bridge->connected && !message.size && passed_fd >= 0) {
                bridge->socket_fd = passed_fd;
                bridge->connected = true;
                bridge->opened(bridge->user);
                bridge->socket_fd = -1;
            } else if (message.type == IPC_SENT && !message.size && message.code &&
                       message.code <= bridge->bytes && bridge->packets) {
                bridge->bytes -= message.code;
                --bridge->packets;
            } else if ((message.type == IPC_BINARY || message.type == IPC_TEXT) &&
                       bridge->connected && message.size <= bridge->limit) {
                if (!bridge->closing)
                    bridge->received(bridge->user, message.bytes, message.size,
                                     message.type == IPC_BINARY);
            } else if (message.type == IPC_PROGRESS && !message.size) {
                /* Worker progress is separate from platform/application liveness. */
            } else if (message.type == IPC_CLOSE && message.size < 124U) {
                message.bytes[message.size] = 0;
                bridge_notify(bridge, (int)message.code, (char *)message.bytes);
            } else {
                bridge_notify(bridge, 1006, "invalid platform worker message");
            }
            if (passed_fd >= 0) close(passed_fd);
            /* Callbacks may destroy or replace the public transport. */
            if (bridge->retired) break;
        }
    }
    bool send_blocked = false;
    unsigned sent_count = 0;
    if (!bridge->retired && !bridge->notified) {
        for (; sent_count < EDGE_WS_IPC_BATCH && bridge->head && !bridge->closing; ++sent_count) {
            edge_ws_packet *packet = bridge->head;
            if (!ipc_send(bridge->fd, IPC_BINARY, 0, packet->bytes, packet->size)) {
                send_blocked = true;
                break;
            }
            bridge->head = packet->next;
            if (!bridge->head) bridge->tail = NULL;
            free(packet);
        }
        if (bridge->closing && !bridge->close_sent) {
            if (ipc_send(bridge->fd, IPC_CLOSE, bridge->close_code,
                         bridge->close_reason, strlen(bridge->close_reason)))
                bridge->close_sent = true;
            else
                send_blocked = true;
        }
    }
    if (!bridge->retired && !bridge->notified) {
        if (received_count == EDGE_WS_IPC_BATCH ||
            sent_count == EDGE_WS_IPC_BATCH || send_blocked) {
            bridge->cooldown = true;
            if (!ev_is_active(&bridge->timer)) {
                ev_timer_set(&bridge->timer, EDGE_WS_IPC_INTERVAL, 0.0);
                ev_timer_start(bridge->loop, &bridge->timer);
            }
        }
        bridge_update_io(bridge);
    }
    bridge->polling = false;
}

static void bridge_io_cb(struct ev_loop *loop, struct ev_io *io, int events) {
    (void)loop;
    (void)events;
    bridge_service(io->data);
}

static void bridge_cooldown_cb(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_bridge *bridge = timer->data;
    bridge->cooldown = false;
    bridge_service(bridge);
}

static void bridge_watchdog_cb(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_bridge *bridge = timer->data;
    if (!bridge->retired && !bridge->notified &&
        ev_now(bridge->loop) - bridge->last_progress > (bridge->connected ? 10.0 : 30.0)) {
        if (waitpid(bridge->pid, NULL, WNOHANG) == 0)
            (void)kill(bridge->pid, SIGKILL);
        bridge_notify(bridge, 1006, "platform transport worker timed out");
    }
}

/* Reaping is asynchronous, including SIGKILL of an unresponsive resolver.
 * libev's existing SIGCHLD watcher may already have reaped the process. */
static void bridge_reap(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)events;
    edge_ws_bridge *bridge = timer->data;
    const pid_t result = waitpid(bridge->pid, NULL, WNOHANG);
    if (result == bridge->pid || (result < 0 && errno == ECHILD)) {
        ev_timer_stop(loop, timer);
        free(bridge);
    }
}

bool edge_ws_transport_connect(edge_ws_transport *transport, struct ev_loop *loop,
                               const char *url, size_t limit, void *user,
                               void (*opened)(void *),
                               void (*received)(void *, void *, size_t, bool),
                               void (*closed)(void *, int, const char *)) {
    edge_ws_transport_destroy(transport);
    if (!loop || !url || strlen(url) >= 1024U || !limit ||
        limit > EDGE_WS_IPC_MESSAGE_LIMIT || !opened || !received || !closed) return false;
    edge_ws_bridge *bridge = calloc(1, sizeof(*bridge));
    if (!bridge) return false;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets)) {
        free(bridge);
        return false;
    }
    /* Linux doubles SO_SNDBUF; still a fixed per-platform kernel allocation. */
    const int capacity = 65536;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) ||
        setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity))) {
        close(sockets[0]); close(sockets[1]); free(bridge);
        return false;
    }
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) worker_run(sockets[1], parent, url, limit);
    close(sockets[1]);
    if (pid < 0) {
        close(sockets[0]); free(bridge);
        return false;
    }
    bridge->loop = loop;
    bridge->pid = pid;
    bridge->fd = sockets[0];
    bridge->socket_fd = -1;
    bridge->limit = limit;
    bridge->last_progress = ev_now(loop);
    bridge->user = user;
    bridge->opened = opened;
    bridge->received = received;
    bridge->closed = closed;
    ev_io_init(&bridge->io, bridge_io_cb, bridge->fd, EV_READ);
    bridge->io.data = bridge;
    ev_timer_init(&bridge->timer, bridge_cooldown_cb, EDGE_WS_IPC_INTERVAL, 0.0);
    bridge->timer.data = bridge;
    ev_timer_init(&bridge->watchdog, bridge_watchdog_cb, 1.0, 1.0);
    bridge->watchdog.data = bridge;
    transport->worker = bridge;
    ev_io_start(loop, &bridge->io);
    ev_timer_start(loop, &bridge->watchdog);
    return true;
}

bool edge_ws_transport_send(edge_ws_transport *transport, const void *data, size_t size) {
    edge_ws_bridge *bridge = transport->worker;
    if (!bridge || !bridge->connected || bridge->closing || bridge->notified ||
        !data || !size || size > bridge->limit ||
        size > EDGE_WS_QUEUE_LIMIT - bridge->bytes || bridge->packets >= EDGE_WS_QUEUE_PACKETS)
        return false;
    edge_ws_packet *packet = malloc(sizeof(*packet) + size);
    if (!packet) return false;
    packet->next = NULL;
    packet->size = size;
    memcpy(packet->bytes, data, size);
    if (bridge->tail) bridge->tail->next = packet;
    else bridge->head = packet;
    bridge->tail = packet;
    bridge->bytes += size;
    ++bridge->packets;
    if (!bridge->polling)
        bridge_update_io(bridge);
    return true;
}

size_t edge_ws_transport_queued_bytes(const edge_ws_transport *transport) {
    return transport->worker ? transport->worker->bytes : 0;
}

int edge_ws_transport_socket_fd(const edge_ws_transport *transport) {
    return transport->worker ? transport->worker->socket_fd : -1;
}

void edge_ws_transport_close(edge_ws_transport *transport, uint16_t code, const char *reason) {
    edge_ws_bridge *bridge = transport->worker;
    if (!bridge || !bridge->connected || bridge->notified) return;
    bridge->closing = true;
    bridge->close_code = code;
    snprintf(bridge->close_reason, sizeof(bridge->close_reason), "%s", reason ? reason : "");
    if (!bridge->polling)
        bridge_update_io(bridge);
}

void edge_ws_transport_destroy(edge_ws_transport *transport) {
    edge_ws_bridge *bridge = transport->worker;
    if (!bridge) return;
    transport->worker = NULL;
    bridge->retired = true;
    bridge->connected = false;
    ev_io_stop(bridge->loop, &bridge->io);
    ev_timer_stop(bridge->loop, &bridge->timer);
    ev_timer_stop(bridge->loop, &bridge->watchdog);
    close(bridge->fd);
    bridge->fd = -1;
    while (bridge->head) {
        edge_ws_packet *next = bridge->head->next;
        free(bridge->head);
        bridge->head = next;
    }
    const pid_t result = waitpid(bridge->pid, NULL, WNOHANG);
    if (!bridge->polling && (result == bridge->pid || (result < 0 && errno == ECHILD))) {
        free(bridge);
        return;
    }
    if (result == 0) (void)kill(bridge->pid, SIGKILL);
    ev_timer_init(&bridge->timer, bridge_reap, EDGE_WS_IPC_INTERVAL, EDGE_WS_IPC_INTERVAL);
    bridge->timer.data = bridge;
    ev_timer_start(bridge->loop, &bridge->timer);
}
