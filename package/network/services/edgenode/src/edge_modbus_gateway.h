#pragma once

#include "edge_acquisition.h"

/* FC03/04 share compact configured non-Bool values, packed from word address 0
 * at widths 1/2/4; FC01/02 share Bool bits packed from bit address 0.
 * Only data values are exposed: no type, quality, time or per-point padding.
 * All words and multiword values are most-significant first. Legal unmapped,
 * disabled and deleted addresses in all four 16-bit spaces read as zero. */
/* Persistent sorting-token stride only; never a live register span. */
#define EDGE_GATEWAY_SLOT_WORDS 10U
#define EDGE_GATEWAY_MAX_POINTS (65536U / EDGE_GATEWAY_SLOT_WORDS)

typedef struct edge_modbus_gateway edge_modbus_gateway;
typedef struct edge_modbus_gateway_server edge_modbus_gateway_server;
struct ev_loop;

typedef struct {
    uint8_t platform_id[16], device_id[16];
    char element_id[65];
    char value[129];
    bool external_io;
} edge_modbus_gateway_write;

typedef uint8_t (*edge_modbus_gateway_write_callback)(void *context,
    const edge_modbus_gateway_write *write, uint8_t command_id[16], uint64_t deadline_ms);
void edge_modbus_gateway_set_writer(edge_modbus_gateway_server *server,
    edge_modbus_gateway_write_callback callback, void *context);
void edge_modbus_gateway_write_result(edge_modbus_gateway_server *server,
    const uint8_t command_id[16], bool success);
/* The request is structurally/range validated by reply; cursor starts at zero.
 * 1 yields the next writable target, 0 finishes, negatives are exception codes.
 * Enumeration and validation have no I/O or cache mutation. */
int edge_modbus_gateway_next_write(const edge_modbus_gateway *gateway,
    const uint8_t *request, size_t size, uint32_t *cursor,
    edge_modbus_gateway_write *write);

/* Prepare and validate compact views before configuration commit. Removed
 * identities retain only internal sorting tokens, not primary addresses.
 * Failure leaves the current live map untouched. */
edge_modbus_gateway *
edge_modbus_gateway_prepare(const char *path, const edge_acquisition_source *sources, size_t count);
void edge_modbus_gateway_free(edge_modbus_gateway *gateway);
/* Apply an explicitly confirmed permutation atomically. A stale snapshot,
 * duplicate, inactive or foreign-platform token is rejected. Samples move with
 * their identities. Omitted internal identity slots stay fixed; compact value
 * addresses, including other platforms, are recomputed from identity order. */
bool edge_modbus_gateway_reorder(edge_modbus_gateway *gateway, const char *path,
                                 const uint8_t platform[16],
                                 const iot_edge_v1_ModbusGatewayOrder *order);
bool edge_modbus_gateway_digest(const edge_modbus_gateway *gateway,
                                const uint8_t platform[16], uint8_t digest[32]);
bool edge_modbus_gateway_export(const edge_modbus_gateway *gateway, const char *path);
void edge_modbus_gateway_update(edge_modbus_gateway *gateway, const uint8_t platform[16],
                                const edge_acquisition_sample *sample, uint64_t now_ms);
/* Complete MBAP ADU. Zero means invalid framing/unit and closes the connection.
 * Writes are fully validated, but only prepare an echo: production transport
 * must wait for next_write targets to finish real write/readback before sending. */
size_t edge_modbus_gateway_reply(const edge_modbus_gateway *gateway, const uint8_t *request,
                                 size_t size, uint8_t response[260], uint64_t now_ms);

/* Zero is the legacy omitted value (Unit ID 1); other values must be 1..247.
 * Set on a prepared map before replace, never mutate a live map. */
bool edge_modbus_gateway_set_unit_id(edge_modbus_gateway *gateway, uint32_t unit_id);

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
