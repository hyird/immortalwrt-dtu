#define _GNU_SOURCE
#include "edge_modbus_gateway.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ev.h>
#include <net/if.h>
#include <poll.h>
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
    uint8_t input[260], output[260], command_id[16];
    size_t used, output_size, sent;
    uint64_t deadline_ms;
    uint32_t cursor;
    bool queued, pending;
} edge_gateway_client;

struct edge_modbus_gateway_server {
    ev_io listener;
    struct ev_loop *loop;
    edge_modbus_gateway *gateway;
    edge_modbus_gateway_write_callback writer;
    void *writer_context;
    edge_gateway_client *active;
    edge_gateway_client clients[EDGE_GATEWAY_CLIENTS];
};

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void pump_writes(edge_modbus_gateway_server *server);

static void close_client(edge_gateway_client *client) {
    if (client->io.fd >= 0) {
        ev_io_stop(client->server->loop, &client->io);
        ev_timer_stop(client->server->loop, &client->deadline);
        close(client->io.fd);
        client->io.fd = -1;
    }
    client->queued = false;
    /* A submitted command still owns this slot until its matching result. */
    if (!client->pending && client->server->active == client)
        client->server->active = NULL;
}

static bool client_connected(edge_gateway_client *client) {
    if (client->io.fd < 0)
        return false;
    struct pollfd state = {.fd = client->io.fd, .events = POLLIN | POLLRDHUP};
    const int ready = poll(&state, 1, 0);
    if (ready < 0 && errno == EINTR)
        return true;
    if (ready < 0 || (state.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL))) {
        close_client(client);
        return false;
    }
    return true;
}

static void respond(edge_gateway_client *client, uint8_t exception) {
    client->queued = false;
    if (client->server->active == client)
        client->server->active = NULL;
    if (!client_connected(client))
        return;
    if (exception) {
        memcpy(client->output, client->input, 8);
        client->output[4] = 0;
        client->output[5] = 3;
        client->output[7] |= 0x80;
        client->output[8] = exception;
        client->output_size = 9;
    }
    client->sent = 0;
    ev_io_stop(client->server->loop, &client->io);
    ev_io_set(&client->io, client->io.fd, EV_WRITE);
    ev_io_start(client->server->loop, &client->io);
}

static void next_write(edge_gateway_client *client) {
    edge_modbus_gateway_server *server = client->server;
    if (!client_connected(client))
        return;
    if (monotonic_ms() >= client->deadline_ms) {
        close_client(client);
        return;
    }
    edge_modbus_gateway_write write;
    const int next = edge_modbus_gateway_next_write(server->gateway, client->input,
                                                     client->used, &client->cursor, &write);
    if (next <= 0) {
        respond(client, next < 0 ? (uint8_t)-next : 0);
        return;
    }
    if (!server->writer) {
        respond(client, 4);
        return;
    }
    client->pending = true;
    const uint8_t error = server->writer(server->writer_context, &write,
                                         client->command_id, client->deadline_ms);
    if (error) {
        client->pending = false;
        respond(client, error);
    }
}

static void pump_writes(edge_modbus_gateway_server *server) {
    if (server->active)
        return;
    for (size_t i = 0; i < EDGE_GATEWAY_CLIENTS; ++i) {
        edge_gateway_client *client = &server->clients[i];
        if (!client->queued)
            continue;
        if (!client_connected(client))
            continue;
        /* Revalidate after waiting: a partial write must merge the latest real
         * readback, not the sample visible before another client's write. */
        client->output_size = edge_modbus_gateway_reply(server->gateway, client->input,
            client->used, client->output, monotonic_ms());
        if (!client->output_size) {
            close_client(client);
            continue;
        }
        if (client->output[7] & 0x80) {
            respond(client, 0);
            continue;
        }
        client->cursor = 0;
        server->active = client;
        next_write(client);
        if (server->active)
            return;
    }
}

void edge_modbus_gateway_set_writer(edge_modbus_gateway_server *server,
    edge_modbus_gateway_write_callback callback, void *context) {
    if (!server)
        return;
    server->writer = callback;
    server->writer_context = context;
}

void edge_modbus_gateway_write_result(edge_modbus_gateway_server *server,
    const uint8_t command_id[16], bool success) {
    if (!server || !command_id || !server->active || !server->active->pending ||
        memcmp(server->active->command_id, command_id, 16))
        return;
    edge_gateway_client *client = server->active;
    client->pending = false;
    if (!client_connected(client) || monotonic_ms() >= client->deadline_ms) {
        close_client(client);
    } else if (!success) {
        respond(client, 4);
    } else {
        next_write(client);
    }
    pump_writes(server);
}

static void client_timeout(struct ev_loop *loop, ev_timer *watcher, int events) {
    (void)loop;
    (void)events;
    edge_gateway_client *client = watcher->data;
    close_client(client);
    pump_writes(client->server);
}

static void client_ready(struct ev_loop *loop, ev_io *watcher, int events) {
    edge_gateway_client *client = watcher->data;
    if (events & EV_ERROR) {
        close_client(client);
        pump_writes(client->server);
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
        const uint8_t function = client->input[7];
        const bool writing = function == 5 || function == 6 || function == 15 || function == 16;
        ev_io_stop(loop, watcher);
        if (writing && !(client->output[7] & 0x80)) {
            uint32_t cursor = 0;
            edge_modbus_gateway_write write;
            const int next = edge_modbus_gateway_next_write(client->server->gateway,
                client->input, client->used, &cursor, &write);
            if (next < 0) {
                respond(client, (uint8_t)-next);
            } else if (!next) {
                respond(client, 0); /* Read-only/unmapped writes are valid no-ops. */
            } else {
                client->deadline_ms = monotonic_ms() + 30000U;
                ev_timer_again(loop, &client->deadline);
                client->queued = true;
                pump_writes(client->server);
            }
        } else {
            respond(client, 0);
        }
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
            if (server->clients[i].io.fd < 0 && !server->clients[i].pending) {
                client = &server->clients[i];
                break;
            }
        if (!client) {
            close(fd);
            continue;
        }
        client->used = client->sent = client->output_size = 0;
        client->queued = false;
        client->cursor = 0;
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
    /* Pending slots remain reserved; their late result stops, never replays the PDU. */
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
