#include "edge_ws_transport.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(LWS_WITHOUT_EXTENSIONS) || !defined(LWS_WITH_LIBEV)
#error "EdgeNode requires libwebsockets permessage-deflate and built-in libev support"
#endif

#define EDGE_WS_QUEUE_LIMIT (256U * 1024U)

struct edge_ws_packet {
    edge_ws_packet *next;
    size_t size;
    unsigned char bytes[];
};

static void disconnected(edge_ws_transport *transport, int code, const char *reason) {
    transport->socket = NULL;
    transport->connected = false;
    if (!transport->stopping && !transport->notified) {
        transport->notified = true;
        transport->closed(transport->user, code, reason);
    }
}

static int websocket_callback(struct lws *socket, enum lws_callback_reasons reason,
                              void *user, void *input, size_t size) {
    edge_ws_transport *transport = lws_context_user(lws_get_context(socket));
    if (!transport || transport->stopping) return 0;
    switch (reason) {
    case LWS_CALLBACK_WS_EXT_DEFAULTS:
        if (user && strcmp(user, "permessage-deflate") == 0)
            snprintf(input, size, "compression_level=9; mem_level=8");
        break;
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        transport->socket = socket;
        transport->connected = true;
        transport->opened(transport->user);
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (transport->closing) return -1;
        if (!transport->incoming_size)
            transport->incoming_binary = lws_frame_is_binary(socket);
        if (size > transport->message_limit - transport->incoming_size) {
            lws_close_reason(socket, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, NULL, 0);
            return -1;
        }
        if (!transport->incoming) {
            transport->incoming = malloc(transport->message_limit);
            if (!transport->incoming) return -1;
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
        free(packet);
        if (transport->head) lws_callback_on_writable(socket);
        break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        disconnected(transport, 1006, input ? (const char *)input : "connection failed");
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        disconnected(transport, 1000, "connection closed");
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

/* 由应用 Heartbeat/ACK 看门狗负责保活，不使用 LWS 默认 40 秒 Ping。 */
static const lws_retry_bo_t idle_policy = {0};

static const struct lws_extension extensions[] = {
    { "permessage-deflate", lws_extension_callback_pm_deflate, "permessage-deflate" },
    { NULL, NULL, NULL }
};

bool edge_ws_transport_connect(edge_ws_transport *transport, struct ev_loop *loop,
                               const char *url, size_t message_limit, void *user,
                               void (*opened)(void *),
                               void (*received)(void *, void *, size_t, bool),
                               void (*closed)(void *, int, const char *)) {
    edge_ws_transport_destroy(transport);
    if (!url || strlen(url) >= sizeof(transport->url) || !message_limit ||
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
        edge_ws_transport_destroy(transport);
        return false;
    }
    transport->notified = false;
    return true;
}

bool edge_ws_transport_send(edge_ws_transport *transport, const void *data, size_t size) {
    if (!transport->connected || transport->closing || !data || !size ||
        size > transport->message_limit || size > EDGE_WS_QUEUE_LIMIT - transport->queued_bytes)
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
    lws_callback_on_writable(transport->socket);
    return true;
}

void edge_ws_transport_close(edge_ws_transport *transport, uint16_t code, const char *reason) {
    if (!transport->socket) return;
    transport->closing = true;
    transport->close_code = code;
    snprintf(transport->close_reason, sizeof(transport->close_reason), "%s", reason ? reason : "");
    lws_callback_on_writable(transport->socket);
}

void edge_ws_transport_destroy(edge_ws_transport *transport) {
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
