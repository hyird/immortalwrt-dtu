#define _GNU_SOURCE
#include "edge_modbus_gateway.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ev.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define EDGE_GATEWAY_CLIENTS 8

typedef struct {
    ev_io io;
    ev_timer deadline;
    struct edge_modbus_gateway_server *server;
    uint8_t input[260], output[260];
    size_t used, output_size, sent;
} edge_gateway_client;

struct edge_modbus_gateway_server {
    ev_io listener;
    struct ev_loop *loop;
    edge_modbus_gateway *gateway;
    edge_gateway_client clients[EDGE_GATEWAY_CLIENTS];
};

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void close_client(edge_gateway_client *client) {
    if (client->io.fd < 0)
        return;
    ev_io_stop(client->server->loop, &client->io);
    ev_timer_stop(client->server->loop, &client->deadline);
    close(client->io.fd);
    client->io.fd = -1;
}

static void client_timeout(struct ev_loop *loop, ev_timer *watcher, int events) {
    (void)loop;
    (void)events;
    close_client(watcher->data);
}

static void client_ready(struct ev_loop *loop, ev_io *watcher, int events) {
    edge_gateway_client *client = watcher->data;
    if (events & EV_ERROR) {
        close_client(client);
        return;
    }
    /* One outstanding ADU per connection bounds buffers and applies TCP backpressure.
     * Read only the exact current frame, leaving pipelined frames in the socket. */
    if (events & EV_READ) {
        size_t target = 6;
        if (client->used >= 6) {
            const size_t length = ((size_t)client->input[4] << 8) | client->input[5];
            if (client->input[2] || client->input[3] || length < 2 || length > 254) {
                close_client(client);
                return;
            }
            target += length;
        }
        const ssize_t received =
            recv(watcher->fd, client->input + client->used, target - client->used, 0);
        if (!received) {
            close_client(client);
            return;
        }
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                close_client(client);
            return;
        }
        client->used += (size_t)received;
        if (client->used < 6)
            return;
        const size_t length = ((size_t)client->input[4] << 8) | client->input[5];
        if (client->input[2] || client->input[3] || length < 2 || length > 254) {
            close_client(client);
            return;
        }
        target = 6 + length;
        if (client->used != target)
            return;
        client->output_size = edge_modbus_gateway_reply(
            client->server->gateway, client->input, client->used, client->output, monotonic_ms());
        if (!client->output_size) {
            close_client(client);
            return;
        }
        client->sent = 0;
        ev_io_stop(loop, watcher);
        ev_io_set(watcher, watcher->fd, EV_WRITE);
        ev_io_start(loop, watcher);
    } else if (events & EV_WRITE) {
        const ssize_t sent = send(watcher->fd, client->output + client->sent,
                                  client->output_size - client->sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (!sent || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                close_client(client);
            return;
        }
        client->sent += (size_t)sent;
        if (client->sent != client->output_size)
            return;
        client->used = client->sent = client->output_size = 0;
        ev_timer_again(loop, &client->deadline);
        ev_io_stop(loop, watcher);
        ev_io_set(watcher, watcher->fd, EV_READ);
        ev_io_start(loop, watcher);
    }
}

static void accept_client(struct ev_loop *loop, ev_io *watcher, int events) {
    edge_modbus_gateway_server *server = watcher->data;
    if (events & EV_ERROR) {
        syslog(LOG_ERR, "Modbus gateway listener failed");
        ev_io_stop(loop, watcher);
        close(watcher->fd);
        watcher->fd = -1;
        return;
    }
    /* Bound work per callback even when the accept queue stays full. */
    for (size_t accepted = 0; accepted < EDGE_GATEWAY_CLIENTS; ++accepted) {
        const int fd = accept4(watcher->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                syslog(LOG_ERR, "Modbus gateway accept failed: %s", strerror(errno));
                ev_io_stop(loop, watcher);
                close(watcher->fd);
                watcher->fd = -1;
            }
            return;
        }
        edge_gateway_client *client = NULL;
        for (size_t i = 0; i < EDGE_GATEWAY_CLIENTS; ++i)
            if (server->clients[i].io.fd < 0) {
                client = &server->clients[i];
                break;
            }
        if (!client) {
            close(fd);
            continue;
        }
        client->used = client->sent = client->output_size = 0;
        ev_io_set(&client->io, fd, EV_READ);
        ev_io_start(loop, &client->io);
        ev_timer_again(loop, &client->deadline);
    }
}

edge_modbus_gateway_server *edge_modbus_gateway_listen(struct ev_loop *loop,
                                                       edge_modbus_gateway *gateway,
                                                       const char *interface_name, uint16_t port) {
    if (!gateway || !interface_name || !interface_name[0] || strlen(interface_name) >= IFNAMSIZ ||
        !port)
        return NULL;
    edge_modbus_gateway_server *server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    server->loop = loop;
    server->gateway = gateway;
    for (size_t i = 0; i < EDGE_GATEWAY_CLIENTS; ++i) {
        edge_gateway_client *client = &server->clients[i];
        client->server = server;
        ev_io_init(&client->io, client_ready, -1, EV_READ);
        client->io.data = client;
        ev_timer_init(&client->deadline, client_timeout, 0, 30);
        client->deadline.data = client;
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        free(server);
        return NULL;
    }
    const int one = 1;
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {.s_addr = INADDR_ANY}};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                   (socklen_t)strlen(interface_name) + 1) ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) ||
        listen(fd, EDGE_GATEWAY_CLIENTS)) {
        close(fd);
        free(server);
        return NULL;
    }
    ev_io_init(&server->listener, accept_client, fd, EV_READ);
    server->listener.data = server;
    ev_io_start(loop, &server->listener);
    return server;
}

void edge_modbus_gateway_replace(edge_modbus_gateway_server *server, edge_modbus_gateway *gateway) {
    if (!server)
        return;
    /* Discard responses built against the previous configuration. */
    for (size_t i = 0; i < EDGE_GATEWAY_CLIENTS; ++i)
        close_client(&server->clients[i]);
    server->gateway = gateway;
}

void edge_modbus_gateway_close(edge_modbus_gateway_server *server) {
    if (!server)
        return;
    for (size_t i = 0; i < EDGE_GATEWAY_CLIENTS; ++i)
        close_client(&server->clients[i]);
    ev_io_stop(server->loop, &server->listener);
    if (server->listener.fd >= 0)
        close(server->listener.fd);
    free(server);
}

bool edge_modbus_gateway_listening(const edge_modbus_gateway_server *server) {
    return server && server->listener.fd >= 0 && ev_is_active(&server->listener);
}
