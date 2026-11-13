#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

struct uci_context;
struct uci_section;
struct ev_loop;
typedef struct edge_network_watch edge_network_watch;

/* Shared UCI identity/device parsing for capability reporting and gateway use. */
bool edge_network_interface_device(struct uci_context *context, struct uci_section *section,
                                    char device[33]);
bool edge_network_gateway_resolve(const char *logical_name, bool runtime,
                                   char device[33], char *error, size_t error_size);
/* Startup only: keep an existing logical identity, otherwise require exactly
 * one configured logical interface owning the old physical device. */
bool edge_network_gateway_migrate(const char *previous, char logical_name[33],
                                   char *error, size_t error_size);
edge_network_watch *edge_network_watch_open(struct ev_loop *loop,
    void (*changed)(void *context, bool available), void *context);
void edge_network_watch_close(edge_network_watch *watch);

bool edge_network_validate_static(const char *ip, uint32_t prefix_length,
                                  const char *gateway, char *error, size_t error_size);

bool edge_network_pending(void);

/* Validates, backs up, stages, and commits a bounded UCI interface transaction. */
bool edge_network_prepare(const iot_edge_v1_NetworkConfigRequest *request,
                          const char *protected_device_name, char *error,
                          size_t error_size);

/* Reloads network and starts a rollback watchdog. */
bool edge_network_activate(uint32_t rollback_timeout_sec,
                           const uint8_t platform_id[16],
                           const uint8_t request_id[16]);

/* Only the platform that started the pending change can confirm it. */
bool edge_network_confirm(const uint8_t platform_id[16],
                          uint8_t request_id[16]);

/* Returns the owner and request once after the watchdog restored the backup. */
bool edge_network_take_rollback(uint8_t platform_id[16],
                                uint8_t request_id[16]);
