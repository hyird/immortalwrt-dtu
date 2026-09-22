#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ev.h>
#include <libwebsockets.h>

typedef struct edge_ws_packet edge_ws_packet;
typedef struct {
    struct lws_context *context;
    struct lws *socket;
    edge_ws_packet *head;
    edge_ws_packet *tail;
    size_t queued_bytes;
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
} edge_ws_transport;

/* 每个平台独占连接、压缩字典与有界发送队列；仅 writable 回调写入网络。 */
bool edge_ws_transport_connect(edge_ws_transport *transport, struct ev_loop *loop,
                               const char *url, size_t message_limit, void *user,
                               void (*opened)(void *),
                               void (*received)(void *, void *, size_t, bool),
                               void (*closed)(void *, int, const char *));
bool edge_ws_transport_send(edge_ws_transport *transport, const void *data, size_t size);
void edge_ws_transport_close(edge_ws_transport *transport, uint16_t code, const char *reason);
void edge_ws_transport_destroy(edge_ws_transport *transport);
