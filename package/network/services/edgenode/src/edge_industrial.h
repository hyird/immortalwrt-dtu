#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edge.pb.h"

#define EDGE_INDUSTRIAL_MAX_FRAME 4096U

bool edge_industrial_protocol(iot_edge_v1_Protocol protocol);
size_t edge_industrial_width(const iot_edge_v1_IndustrialPointConfig *point);
bool edge_industrial_valid_device(const iot_edge_v1_DeviceConfig *device);
bool edge_industrial_valid_point(const iot_edge_v1_DeviceConfig *device,
                                 const iot_edge_v1_IndustrialPointConfig *point);
/* Header starts after DL/T645 wake-up bytes. Zero means an invalid header. */
size_t edge_industrial_header_size(const iot_edge_v1_DeviceConfig *device);
size_t edge_industrial_frame_size(const iot_edge_v1_DeviceConfig *device,
                                  const uint8_t *header, size_t size);
size_t edge_fins_node_request(uint8_t source, uint8_t *output, size_t capacity);
bool edge_fins_node_response(const uint8_t *frame, size_t size,
                              iot_edge_v1_IndustrialConnectionConfig *connection);
/* sequence=0 is an initial DL/T645 read; positive values request continuation. */
size_t edge_industrial_request(const iot_edge_v1_DeviceConfig *device,
                                const iot_edge_v1_IndustrialConnectionConfig *connection,
                                const iot_edge_v1_IndustrialPointConfig *point,
                                uint16_t serial, uint8_t sequence,
                                const uint8_t *write_value, size_t write_size,
                                uint8_t *output, size_t capacity);
bool edge_industrial_response(const iot_edge_v1_DeviceConfig *device,
                               const uint8_t *request, size_t request_size,
                               const uint8_t *frame, size_t frame_size,
                               uint8_t *value, size_t capacity, size_t *value_size,
                               bool *more);
void edge_industrial_redact(iot_edge_v1_Protocol protocol, uint8_t *frame, size_t size);
bool edge_meter_decode(const iot_edge_v1_IndustrialPointConfig *point,
                        const uint8_t *raw, size_t size, char *text, size_t capacity);
bool edge_meter_encode(const iot_edge_v1_IndustrialPointConfig *point,
                        const char *text, uint8_t *raw, size_t capacity, size_t *size);
