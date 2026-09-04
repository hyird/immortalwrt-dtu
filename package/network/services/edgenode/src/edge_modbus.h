#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_MODBUS_MAX_FRAME 260U

typedef enum {
    EDGE_MODBUS_TCP = 1,
    EDGE_MODBUS_RTU = 2,
} edge_modbus_transport;

typedef enum {
    EDGE_MODBUS_OK = 0,
    EDGE_MODBUS_INVALID_ARGUMENT,
    EDGE_MODBUS_TRUNCATED,
    EDGE_MODBUS_WRONG_RESPONSE,
    EDGE_MODBUS_BAD_CRC,
    EDGE_MODBUS_EXCEPTION,
    EDGE_MODBUS_OUTPUT_TOO_SMALL,
} edge_modbus_result;

typedef struct {
    edge_modbus_transport transport;
    uint16_t transaction_id;
    uint8_t unit_id;
    uint8_t function;
    uint16_t address;
    uint16_t quantity;
} edge_modbus_request;

typedef struct {
    size_t point_index;
    uint8_t function;
    uint16_t address;
    uint16_t quantity;
} edge_modbus_read_point;

typedef struct {
    uint8_t function;
    uint16_t address;
    uint16_t quantity;
} edge_modbus_read_group;

uint16_t edge_modbus_crc16(const uint8_t *data, size_t size);

/* Returns the minimum Modbus RTU silent interval (3.5 character times). */
uint32_t edge_modbus_rtu_quiet_time_us(uint32_t baud_rate, uint8_t data_bits,
                                       uint8_t stop_bits, bool parity_enabled);

/*
 * Sorts points by function/address and creates the same bounded read groups as
 * the platform collector. A zero max_quantity is treated as the legacy
 * default of 125 registers.
 */
bool edge_modbus_plan_reads(edge_modbus_read_point *points, size_t point_count,
                            uint32_t merge_gap, uint32_t max_quantity,
                            edge_modbus_read_group *groups, size_t group_capacity,
                            size_t *group_count);

/* Extracts one configured point from a grouped read response payload. */
bool edge_modbus_extract_point(const edge_modbus_read_group *group,
                               const edge_modbus_read_point *point,
                               const uint8_t *data, size_t data_size,
                               uint8_t *output, size_t capacity,
                               size_t *output_size);

edge_modbus_result edge_modbus_build_read(const edge_modbus_request *request,
                                          uint8_t *output, size_t capacity,
                                          size_t *output_size);

/*
 * data is the Modbus wire value: two bytes for FC06, packed coils for FC0F,
 * and big-endian register bytes for FC10. FC05 accepts one byte (0 or 1).
 */
edge_modbus_result edge_modbus_build_write(const edge_modbus_request *request,
                                           const uint8_t *data, size_t data_size,
                                           uint8_t *output, size_t capacity,
                                           size_t *output_size);

/*
 * Validates transaction/unit/function, exception response, RTU CRC, byte count,
 * and the address/quantity echo of write responses. Read data is copied to data.
 */
edge_modbus_result edge_modbus_parse_response(const edge_modbus_request *request,
                                              const uint8_t *frame, size_t frame_size,
                                              const uint8_t *expected_write_data,
                                              size_t expected_write_data_size,
                                              uint8_t *data, size_t capacity,
                                              size_t *data_size,
                                              uint8_t *exception_code);
