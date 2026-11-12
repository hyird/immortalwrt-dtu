#pragma once

#include "edge_acquisition.h"

/* One persistent slot: type, quality, 64-bit value, 64-bit Unix milliseconds.
 * All words and multiword values are most-significant first. */
#define EDGE_GATEWAY_SLOT_WORDS 10U
#define EDGE_GATEWAY_MAX_POINTS (65536U / EDGE_GATEWAY_SLOT_WORDS)

typedef struct edge_modbus_gateway edge_modbus_gateway;
typedef struct edge_modbus_gateway_server edge_modbus_gateway_server;
struct ev_loop;

/* Reserves addresses atomically before configuration commit. Removed identities
 * retain their reservations. Failure leaves the current live map untouched. */
edge_modbus_gateway *
edge_modbus_gateway_prepare(const char *path, const edge_acquisition_source *sources, size_t count);
void edge_modbus_gateway_free(edge_modbus_gateway *gateway);
/* Apply an explicitly confirmed permutation atomically. A stale snapshot,
 * duplicate, inactive or foreign-platform slot is rejected. Samples move with
 * their identities. Slots omitted from the request retain their addresses. */
bool edge_modbus_gateway_reorder(edge_modbus_gateway *gateway, const char *path,
                                 const uint8_t platform[16],
                                 const iot_edge_v1_ModbusGatewayOrder *order);
bool edge_modbus_gateway_digest(const edge_modbus_gateway *gateway,
                                const uint8_t platform[16], uint8_t digest[32]);
bool edge_modbus_gateway_export(const edge_modbus_gateway *gateway, const char *path);
void edge_modbus_gateway_update(edge_modbus_gateway *gateway, const uint8_t platform[16],
                                const edge_acquisition_sample *sample, uint64_t now_ms);
/* Complete MBAP ADU. Zero means invalid framing/unit, and closes the connection. */
size_t edge_modbus_gateway_reply(const edge_modbus_gateway *gateway, const uint8_t *request,
                                 size_t size, uint8_t response[260], uint64_t now_ms);

edge_modbus_gateway_server *edge_modbus_gateway_listen(struct ev_loop *loop,
                                                       edge_modbus_gateway *gateway,
                                                       const char *interface_name, uint16_t port);
void edge_modbus_gateway_replace(edge_modbus_gateway_server *server, edge_modbus_gateway *gateway);
void edge_modbus_gateway_close(edge_modbus_gateway_server *server);

bool edge_modbus_gateway_listening(const edge_modbus_gateway_server *server);

edge_modbus_gateway *edge_modbus_gateway_load(const char *path);
bool edge_modbus_gateway_describe(const edge_modbus_gateway *gateway, const uint8_t platform[16],
                                  const uint8_t device[16], const char *element,
                                  iot_edge_v1_ModbusGatewayPoint *output, uint64_t now_ms);

bool edge_modbus_gateway_points(const edge_modbus_gateway *gateway, const uint8_t platform[16],
                                const edge_runtime_config *config, bool enabled, bool listening,
                                const iot_edge_v1_ModbusGatewayRequest *request,
                                iot_edge_v1_ModbusGatewayResult *result, uint64_t now_ms);
