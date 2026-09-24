#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge_device_runtime.h"
#include "edge_modbus.h"
#include "edge_s7.h"

static void require_true(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void test_modbus(void) {
    edge_modbus_request read = {.transport = EDGE_MODBUS_TCP,
                                 .transaction_id = 0x1234,
                                 .unit_id = 1,
                                 .function = 3,
                                 .address = 2,
                                 .quantity = 2};
    uint8_t frame[EDGE_MODBUS_MAX_FRAME];
    size_t size = 0U;
    require_true(edge_modbus_build_read(&read, frame, sizeof(frame), &size) == EDGE_MODBUS_OK,
                 "Modbus TCP read build failed");
    const uint8_t expected_read[] = {0x12, 0x34, 0, 0, 0, 6, 1, 3, 0, 2, 0, 2};
    require_true(size == sizeof(expected_read) && memcmp(frame, expected_read, size) == 0,
                 "Modbus TCP read frame differs from wire contract");

    const uint8_t read_response[] = {0x12, 0x34, 0, 0, 0, 7, 1, 3, 4, 0x11, 0x22, 0x33, 0x44};
    uint8_t value[8];
    size_t value_size = 0U;
    uint8_t exception = 0U;
    require_true(edge_modbus_parse_response(&read, read_response, sizeof(read_response),
                                             NULL, 0U, value, sizeof(value), &value_size,
                                             &exception) == EDGE_MODBUS_OK,
                 "Modbus TCP read response parse failed");
    require_true(value_size == 4U && value[0] == 0x11U && value[3] == 0x44U,
                 "Modbus read response data is wrong");

    edge_modbus_request write = read;
    write.transaction_id = 9U;
    write.function = 6U;
    write.quantity = 1U;
    const uint8_t desired[] = {0xab, 0xcd};
    require_true(edge_modbus_build_write(&write, desired, sizeof(desired), frame,
                                          sizeof(frame), &size) == EDGE_MODBUS_OK,
                 "Modbus FC06 build failed");
    require_true(edge_modbus_parse_response(&write, frame, size, desired, sizeof(desired),
                                             value, sizeof(value), &value_size,
                                             &exception) == EDGE_MODBUS_OK,
                 "Modbus FC06 echo validation failed");
    const uint8_t wrong[] = {0xab, 0xce};
    require_true(edge_modbus_parse_response(&write, frame, size, wrong, sizeof(wrong),
                                             value, sizeof(value), &value_size,
                                             &exception) == EDGE_MODBUS_WRONG_RESPONSE,
                 "Modbus accepted a write response with the wrong echoed value");

    edge_modbus_request rtu = {.transport = EDGE_MODBUS_RTU,
                                .unit_id = 1,
                                .function = 3,
                                .address = 0,
                                .quantity = 1};
    require_true(edge_modbus_build_read(&rtu, frame, sizeof(frame), &size) == EDGE_MODBUS_OK,
                 "Modbus RTU read build failed");
    require_true(size == 8U && edge_modbus_crc16(frame, 6U) ==
                                  (uint16_t)(frame[6] | ((uint16_t)frame[7] << 8U)),
                 "Modbus RTU CRC is wrong");

    require_true(edge_modbus_rtu_quiet_time_us(9600U, 8U, 1U, false) == 3646U,
                 "Modbus RTU 3.5T calculation is wrong for 9600 8N1");
    require_true(edge_modbus_rtu_quiet_time_us(19200U, 8U, 1U, true) == 2006U,
                 "Modbus RTU 3.5T calculation is wrong for parity framing");

    edge_modbus_read_point points[] = {
        {.point_index = 0U, .function = 3U, .address = 3U, .quantity = 1U},
        {.point_index = 1U, .function = 3U, .address = 15U, .quantity = 2U},
        {.point_index = 2U, .function = 3U, .address = 17U, .quantity = 2U},
        {.point_index = 3U, .function = 3U, .address = 1U, .quantity = 1U},
        {.point_index = 4U, .function = 3U, .address = 19U, .quantity = 1U},
    };
    edge_modbus_read_group groups[5];
    size_t group_count = 0U;
    require_true(edge_modbus_plan_reads(points, 5U, 100U, 125U, groups, 5U,
                                        &group_count),
                 "Modbus read plan failed");
    require_true(group_count == 1U && groups[0].function == 3U &&
                     groups[0].address == 1U && groups[0].quantity == 19U,
                 "Modbus points were not merged into the expected 1..19 read");
    require_true(points[0].point_index == 3U && points[1].point_index == 0U,
                 "Modbus read points were not sorted without losing source indexes");

    uint8_t grouped[38];
    for (size_t index = 0U; index < sizeof(grouped); ++index)
        grouped[index] = (uint8_t)index;
    uint8_t extracted[8];
    size_t extracted_size = 0U;
    require_true(edge_modbus_extract_point(&groups[0], &points[2], grouped,
                                            sizeof(grouped), extracted,
                                            sizeof(extracted), &extracted_size) &&
                     extracted_size == 4U && extracted[0] == 28U && extracted[3] == 31U,
                 "Modbus grouped register extraction used the wrong offset");

    edge_modbus_read_point split_points[] = {
        {.point_index = 0U, .function = 3U, .address = 1U, .quantity = 1U},
        {.point_index = 1U, .function = 3U, .address = 15U, .quantity = 2U},
        {.point_index = 2U, .function = 4U, .address = 16U, .quantity = 1U},
    };
    require_true(edge_modbus_plan_reads(split_points, 3U, 2U, 125U, groups, 5U,
                                        &group_count) && group_count == 3U,
                 "Modbus planner merged across a large gap or function boundary");

    const edge_modbus_read_group coil_group = {
        .function = 1U, .address = 8U, .quantity = 10U};
    const edge_modbus_read_point coil_point = {
        .point_index = 0U, .function = 1U, .address = 17U, .quantity = 1U};
    const uint8_t coil_data[] = {0U, 0x02U};
    require_true(edge_modbus_extract_point(&coil_group, &coil_point, coil_data,
                                            sizeof(coil_data), extracted,
                                            sizeof(extracted), &extracted_size) &&
                     extracted_size == 1U && extracted[0] == 1U,
                 "Modbus grouped coil extraction used the wrong bit offset");
}

static void test_s7(void) {
    uint8_t frame[EDGE_S7_MAX_FRAME];
    size_t size = edge_s7_build_cotp_connect(0x0100U, 0x0101U, frame, sizeof(frame));
    const uint8_t expected_cotp[] = {0x03, 0x00, 0x00, 0x16, 0x11, 0xe0, 0x00, 0x00,
                                     0x00, 0x01, 0x00, 0xc0, 0x01, 0x0a, 0xc1, 0x02,
                                     0x01, 0x00, 0xc2, 0x02, 0x01, 0x01};
    require_true(size == sizeof(expected_cotp) && memcmp(frame, expected_cotp, size) == 0,
                 "S7 COTP connect request is wrong");
    const uint8_t cotp_confirm[] = {0x03, 0x00, 0x00, 0x0b, 0x06, 0xd0,
                                    0x00, 0x01, 0x00, 0x06, 0x00};
    require_true(edge_s7_parse_cotp_confirm(cotp_confirm, sizeof(cotp_confirm)) == EDGE_S7_OK,
                 "S7 COTP confirmation rejected");

    size = edge_s7_build_setup(7U, EDGE_S7_DEFAULT_PDU_LENGTH, frame, sizeof(frame));
    require_true(size == 25U && frame[11] == 0U && frame[12] == 7U &&
                     frame[23] == 0x01U && frame[24] == 0xe0U,
                 "S7 setup request is wrong");
    const uint8_t setup[] = {0x03, 0x00, 0x00, 0x1b, 0x02, 0xf0, 0x80, 0x32, 0x03,
                             0x00, 0x00, 0x00, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00,
                             0x00, 0xf0, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0xe0};
    uint16_t pdu = 0U;
    require_true(edge_s7_parse_setup(setup, sizeof(setup), 7U, &pdu) == EDGE_S7_OK &&
                     pdu == EDGE_S7_DEFAULT_PDU_LENGTH,
                 "S7 setup response parse failed");

    const edge_s7_address address = {.area = EDGE_S7_AREA_DB,
                                     .db_number = 1,
                                     .start_byte = 2,
                                     .start_bit = 0,
                                     .size = 2,
                                     .bit_access = false};
    size = edge_s7_build_read(8U, &address, frame, sizeof(frame));
    require_true(size == 31U && frame[17] == 4U && frame[18] == 1U &&
                     frame[25] == 0U && frame[26] == 1U && frame[27] == 0x84U &&
                     frame[30] == 16U,
                 "S7 ReadVar request is wrong");
    const uint8_t read_response[] = {0x03, 0x00, 0x00, 0x1b, 0x02, 0xf0, 0x80, 0x32, 0x03,
                                     0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x06, 0x00,
                                     0x00, 0x04, 0x01, 0xff, 0x04, 0x00, 0x10, 0x12, 0x34};
    uint8_t data[8];
    size_t data_size = 0U;
    uint8_t return_code = 0U;
    require_true(edge_s7_parse_read(read_response, sizeof(read_response), 8U, data,
                                    sizeof(data), &data_size, &return_code) == EDGE_S7_OK,
                 "S7 ReadVar response parse failed");
    require_true(data_size == 2U && data[0] == 0x12U && data[1] == 0x34U,
                 "S7 ReadVar returned the wrong bytes");

    const uint8_t desired[] = {0x12, 0x34};
    size = edge_s7_build_write(9U, &address, desired, sizeof(desired), frame, sizeof(frame));
    require_true(size == 37U && frame[17] == 5U && frame[31] == 0U &&
                     frame[32] == 4U && frame[35] == 0x12U && frame[36] == 0x34U,
                 "S7 WriteVar request is wrong");
    const uint8_t write_response[] = {0x03, 0x00, 0x00, 0x16, 0x02, 0xf0, 0x80, 0x32, 0x03,
                                      0x00, 0x00, 0x00, 0x09, 0x00, 0x02, 0x00, 0x01, 0x00,
                                      0x00, 0x05, 0x01, 0xff};
    require_true(edge_s7_parse_write(write_response, sizeof(write_response), 9U,
                                     &return_code) == EDGE_S7_OK,
                 "S7 WriteVar response parse failed");
}

typedef struct {
    unsigned connects;
    unsigned handshakes;
    unsigned reads;
    unsigned writes;
    unsigned disconnects;
    unsigned reports;
    unsigned completions;
    edge_command_result last_command_result;
    edge_io_result next_read_result;
    unsigned read_timeouts;
    unsigned read_protocol_errors;
    edge_io_result next_write_result;
    edge_io_result next_connect_result;
    edge_io_result next_handshake_result;
    uint8_t last_platform[16];
    edge_device_runtime *runtime;
    unsigned debug_reads;
    unsigned silent_reads;
    unsigned silent_writes;
} fake_device;

static edge_io_result fake_connect(void *context) {
    fake_device *fake = context;
    ++fake->connects;
    const edge_io_result result = fake->next_connect_result;
    fake->next_connect_result = EDGE_IO_OK;
    return result;
}

static edge_io_result fake_handshake(void *context) {
    fake_device *fake = context;
    ++fake->handshakes;
    const edge_io_result result = fake->next_handshake_result;
    fake->next_handshake_result = EDGE_IO_OK;
    return result;
}

static edge_io_result fake_read(void *context, edge_device_sample *sample) {
    fake_device *fake = context;
    ++fake->reads;
    if (fake->runtime != NULL && fake->runtime->debug_read)
        ++fake->debug_reads;
    if (fake->runtime != NULL && fake->runtime->silent_background_read)
        ++fake->silent_reads;
    if (fake->read_timeouts != 0U) {
        --fake->read_timeouts;
        return EDGE_IO_NO_RESPONSE;
    }
    if (fake->read_protocol_errors != 0U) {
        --fake->read_protocol_errors;
        return EDGE_IO_PROTOCOL_ERROR;
    }
    const edge_io_result result = fake->next_read_result;
    fake->next_read_result = EDGE_IO_OK;
    if (result != EDGE_IO_OK)
        return result;
    sample->bytes[0] = (uint8_t)fake->reads;
    sample->size = 1U;
    return EDGE_IO_OK;
}

static edge_io_result fake_write(void *context, const edge_write_command *command,
                                 edge_device_sample *actual) {
    fake_device *fake = context;
    ++fake->writes;
    if (fake->runtime != NULL && fake->runtime->silent_background_read)
        ++fake->silent_writes;
    const edge_io_result result = fake->next_write_result;
    fake->next_write_result = EDGE_IO_OK;
    if (result != EDGE_IO_OK) return result;
    memcpy(actual->bytes, command->value, command->value_size);
    actual->size = command->value_size;
    return EDGE_IO_OK;
}

static void fake_disconnect(void *context) {
    ++((fake_device *)context)->disconnects;
}

static void fake_report(void *context, const uint8_t platform_id[16],
                        const uint8_t device_id[16], const edge_device_sample *sample) {
    fake_device *fake = context;
    (void)device_id;
    require_true(sample->size != 0U, "runtime reported an empty sample");
    ++fake->reports;
    memcpy(fake->last_platform, platform_id, 16U);
}

static void fake_complete(void *context, const uint8_t platform_id[16],
                          const uint8_t device_id[16], const uint8_t command_id[16],
                          edge_command_result result, const edge_device_sample *actual) {
    fake_device *fake = context;
    (void)platform_id;
    (void)device_id;
    (void)command_id;
    require_true((result == EDGE_COMMAND_SUCCEEDED ||
                  result == EDGE_COMMAND_READBACK_MISMATCH) == (actual != NULL),
                 "command completion readback mismatch");
    ++fake->completions;
    fake->last_command_result = result;
}

static void test_io_and_reporting(void) {
    uint8_t platform_id[16] = {1U};
    uint8_t device_id[16] = {2U};
    fake_device fake = {0};
    edge_device_driver driver = {.connect = fake_connect,
                                 .handshake = fake_handshake,
                                 .read = fake_read,
                                 .write_readback = fake_write,
                                 .disconnect = fake_disconnect,
                                 .report = fake_report,
                                 .command_complete = fake_complete};
    edge_device_runtime runtime;
    require_true(!edge_device_runtime_init(&runtime, EDGE_DEVICE_S7, platform_id, device_id,
                                           500U, 3U, 0U, &driver, &fake),
                 "runtime accepted an interval below scheduler resolution");
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7, platform_id, device_id,
                                          EDGE_ACQUISITION_TICK_MS, 3U, 0U, &driver, &fake),
                 "runtime initialization failed");

    edge_device_runtime_tick(&runtime, 0U, 1000000);
    edge_device_runtime_tick(&runtime, 500U, 1000500);
    require_true(fake.reads == 1U && fake.reports == 0U,
                 "initial sample was reported before the ordinary interval");

    edge_write_command command = {.value = {0x55U}, .value_size = 1U};
    command.command_id[0] = 3U;
    require_true(edge_device_runtime_enqueue_write(&runtime, &command),
                 "write command enqueue failed");
    edge_device_runtime_tick(&runtime, 1000U, 1001000);
    edge_device_runtime_tick(&runtime, 2000U, 1002000);
    require_true(fake.reads == 2U && fake.writes == 1U && fake.completions == 1U &&
                     fake.last_command_result == EDGE_COMMAND_SUCCEEDED,
                 "one-second scan or prioritized write did not verify readback");
    require_true(fake.reports == 0U,
                 "successful write automatically triggered telemetry");

    edge_device_runtime_tick(&runtime, 3000U, 1003000);
    require_true(fake.reads == 3U && fake.reports == 1U &&
                     memcmp(fake.last_platform, platform_id, 16U) == 0,
                 "runtime did not report a successful read on the ordinary interval");

    fake.next_read_result = EDGE_IO_NO_RESPONSE;
    edge_device_runtime_tick(&runtime, 4000U, 1004000);
    require_true(fake.disconnects == 1U && fake.connects == 2U &&
                     fake.handshakes == 2U && fake.reads == 5U &&
                     runtime.latest.bytes[0] == 5U,
                 "S7 did not immediately reconnect, handshake and reread after timeout");
    edge_device_runtime_tick(&runtime, 5000U, 1005000);
    require_true(fake.connects == 2U && fake.handshakes == 2U && fake.reads == 6U,
                 "S7 did not reuse the recovered connection on the next cycle");
    edge_device_runtime_close(&runtime);

    fake_device modbus = {0};
    edge_device_runtime modbus_runtime;
    require_true(edge_device_runtime_init(&modbus_runtime, EDGE_DEVICE_MODBUS,
                                           platform_id, device_id,
                                           EDGE_ACQUISITION_TICK_MS, 3U, 0U,
                                           &driver, &modbus),
                 "Modbus runtime initialization failed");
    edge_device_runtime_tick(&modbus_runtime, 0U, 1000000);
    modbus.next_read_result = EDGE_IO_NO_RESPONSE;
    edge_device_runtime_tick(&modbus_runtime, 1000U, 1001000);
    require_true(modbus.disconnects == 0U && modbus.connects == 1U,
                 "Modbus timeout unexpectedly closed the TCP state");
    edge_device_runtime_tick(&modbus_runtime, 2000U, 1002000);
    require_true(modbus.connects == 1U && modbus.reads == 3U,
                 "Modbus did not reuse the existing connection after a timeout");
    modbus.next_read_result = EDGE_IO_OFFLINE;
    edge_device_runtime_tick(&modbus_runtime, 3000U, 1003000);
    require_true(modbus.disconnects == 1U,
                 "Modbus transport failure did not close the broken connection");
    edge_device_runtime_tick(&modbus_runtime, 4000U, 1004000);
    require_true(modbus.connects == 2U,
                 "Modbus transport failure did not reconnect before the next request");
    edge_device_runtime_close(&modbus_runtime);
    require_true(modbus.disconnects == 2U,
                 "Modbus runtime did not close during explicit shutdown");
}

static void test_initial_report_waits_for_first_success(void) {
    uint8_t platform_id[16] = {6U};
    uint8_t device_id[16] = {7U};
    fake_device fake = {.next_read_result = EDGE_IO_OFFLINE};
    edge_device_driver driver = {.connect = fake_connect,
                                 .handshake = fake_handshake,
                                 .read = fake_read,
                                 .write_readback = fake_write,
                                 .disconnect = fake_disconnect,
                                 .report = fake_report,
                                 .command_complete = fake_complete};
    edge_device_runtime runtime;
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7,
                                          platform_id, device_id,
                                          EDGE_ACQUISITION_TICK_MS, 3U, 0U,
                                          &driver, &fake),
                 "initial-report runtime initialization failed");

    edge_device_runtime_tick(&runtime, 0U, 3000000);
    require_true(fake.reports == 0U && runtime.initial_report_pending,
                 "failed first read produced telemetry or cleared its pending report");
    edge_device_runtime_tick(&runtime, 1000U, 3001000);
    require_true(fake.reports == 0U && runtime.initial_report_pending,
                 "successful non-due scan reported before the ordinary interval");
    edge_device_runtime_tick(&runtime, 3000U, 3003000);
    require_true(fake.reports == 1U && !runtime.initial_report_pending,
                 "first successful due scan did not clear the pending initial report");
    edge_device_runtime_tick(&runtime, 4000U, 3004000);
    require_true(fake.reports == 1U,
                 "non-due scan emitted telemetry after the first report");
    edge_device_runtime_close(&runtime);
}

static void test_fast_reporting_after_write(void) {
    uint8_t platform_id[16] = {4U};
    uint8_t device_id[16] = {5U};
    fake_device fake = {0};
    edge_device_driver driver = {.connect = fake_connect,
                                 .handshake = fake_handshake,
                                 .read = fake_read,
                                 .write_readback = fake_write,
                                 .disconnect = fake_disconnect,
                                 .report = fake_report,
                                 .command_complete = fake_complete};
    edge_device_runtime runtime;
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_MODBUS,
                                          platform_id, device_id,
                                          EDGE_ACQUISITION_TICK_MS, 10U, 0U,
                                          &driver, &fake),
                 "fast-report runtime initialization failed");
    edge_device_runtime_tick(&runtime, 0U, 2000000);

    edge_write_command command = {
        .value = {0x33U},
        .value_size = 1U,
        .fast_read_duration_sec = 12U,
        .fast_read_interval_sec = 4U,
    };
    require_true(edge_device_runtime_enqueue_write(&runtime, &command),
                 "fast-report write command enqueue failed");
    edge_device_runtime_tick(&runtime, 1000U, 2001000);
    require_true(fake.reports == 0U && fake.writes == 1U && fake.completions == 1U,
                 "verified write automatically triggered telemetry");

    edge_device_runtime_tick(&runtime, 5000U, 2005000);
    edge_device_runtime_tick(&runtime, 9000U, 2009000);
    require_true(fake.reports == 2U,
                 "fast-report window did not use the command interval");

    edge_device_runtime_tick(&runtime, 10000U, 2010000);
    require_true(fake.reports == 2U,
                 "regular reporting interrupted an active fast-report window");
    edge_device_runtime_tick(&runtime, 13000U, 2013000);
    require_true(fake.reports == 3U && runtime.fast_report_until_ms == 0U,
                 "fast-report window did not include its inclusive final due sample or expire");

    edge_device_runtime_tick(&runtime, 20000U, 2020000);
    require_true(fake.reports == 4U,
                 "regular reporting did not resume after the fast-report window");
    edge_device_runtime_close(&runtime);
}

static void test_configured_read_interval(void) {
    uint8_t platform_id[16] = {1U}, device_id[16] = {2U};
    edge_device_driver driver = {.connect = fake_connect, .handshake = fake_handshake,
        .read = fake_read, .write_readback = fake_write, .disconnect = fake_disconnect,
        .report = fake_report, .command_complete = fake_complete};
    for (unsigned protocol = EDGE_DEVICE_MODBUS; protocol <= EDGE_DEVICE_DLT645; ++protocol) {
        fake_device fake = {0};
        edge_device_runtime runtime;
        require_true(edge_device_runtime_init(&runtime, (edge_device_protocol)protocol,
            platform_id, device_id, 0U, 30U, 0U, &driver, &fake), "configured runtime init failed");
        for (uint64_t now = 0; now < 30000U; now += 1000U)
            edge_device_runtime_tick(&runtime, now, (int64_t)now);
        require_true(fake.reads == 30U, "one-second scan cycle was not applied to a slow configured interval");
        edge_device_runtime_tick(&runtime, 30000U, 30000);
        require_true(fake.reads == 31U, "slow configured interval suppressed the 1-second scan cycle");
        edge_device_runtime_tick(&runtime, 100000U, 100000);
        require_true(fake.reads == 32U && runtime.next_io_at_ms == 101000U,
            "late scheduling produced catch-up reads or lost the 1-second period");
        edge_write_command command = {.value = {0x55U}, .value_size = 1U};
        require_true(edge_device_runtime_enqueue_write(&runtime, &command), "write enqueue failed");
        edge_device_runtime_tick(&runtime, 101000U, 101000);
        require_true(fake.writes == 1U && runtime.next_io_at_ms == 102000U,
            "configured read interval delayed a control command or changed its 1-second schedule");
        edge_device_runtime_close(&runtime);
        memset(&fake, 0, sizeof(fake));
        require_true(edge_device_runtime_init(&runtime, (edge_device_protocol)protocol,
            platform_id, device_id, 5000U, 30U, 0U, &driver, &fake), "explicit interval rejected");
        edge_device_runtime_tick(&runtime, 0U, 0);
        edge_device_runtime_tick(&runtime, 1000U, 1000);
        require_true(fake.reads == 2U, "explicit slow interval was not covered by 1-second scanning");
        for (uint64_t now = 2000U; now <= 5000U; now += 1000U)
            edge_device_runtime_tick(&runtime, now, (int64_t)now);
        require_true(fake.reads == 6U, "explicit slow interval stopped the 1-second scan cycle");
        edge_device_runtime_close(&runtime);
    }
}

static void test_s7_immediate_retry_is_bounded(void) {
    const uint8_t platform_id[16] = {1U}, device_id[16] = {2U};
    const edge_device_driver driver = {
        .connect = fake_connect, .handshake = fake_handshake,
        .read = fake_read, .write_readback = fake_write,
        .disconnect = fake_disconnect, .report = fake_report,
        .command_complete = fake_complete};
    for (unsigned scenario = 0U; scenario < 3U; ++scenario) {
        fake_device fake = {0};
        edge_device_runtime runtime;
        require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7,
            platform_id, device_id, 1000U, 30U, 0U, &driver, &fake), "S7 init failed");
        edge_device_runtime_tick(&runtime, 0U, 0);
        edge_write_command command = {.value = {0x55U}, .value_size = 1U};
        require_true(edge_device_runtime_enqueue_write(&runtime, &command), "enqueue failed");
        edge_device_runtime_tick(&runtime, 1000U, 1000);
        require_true(fake.writes == 1U && fake.completions == 1U,
            "priority write was not completed exactly once");
        fake.read_timeouts = scenario == 0U ? 2U : 1U;
        if (scenario == 1U) fake.next_connect_result = EDGE_IO_OFFLINE;
        if (scenario == 2U) fake.next_handshake_result = EDGE_IO_NO_RESPONSE;
        edge_device_runtime_tick(&runtime, 2000U, 2000);
        require_true(fake.connects == 2U &&
            fake.handshakes == (scenario == 1U ? 1U : 2U) &&
            fake.reads == (scenario == 0U ? 3U : 2U),
            "S7 read recovery exceeded one reconnect/re-read or read before recovery");
        require_true(!runtime.connected && !runtime.handshaken &&
            fake.disconnects == (scenario == 1U ? 1U : 2U),
            "failed S7 recovery retained stale connection state");
        require_true(fake.writes == 1U && fake.completions == 1U &&
            runtime.next_io_at_ms == 3000U,
            "S7 read recovery replayed the completed write or changed acquisition cadence");
        edge_device_runtime_tick(&runtime, 2500U, 2500);
        require_true(fake.connects == 2U && fake.writes == 1U,
            "failed recovery retried before the next acquisition deadline");
        edge_device_runtime_tick(&runtime, 3000U, 3000);
        require_true(fake.connects == 3U && runtime.connected && runtime.handshaken &&
            fake.writes == 1U && fake.completions == 1U,
            "S7 failed to resume next cycle or replayed the write");
        edge_device_runtime_close(&runtime);
    }
}

static void test_s7_protocol_error_reconnects(void) {
    const uint8_t platform_id[16] = {1U}, device_id[16] = {2U};
    const edge_device_driver driver = {
        .connect = fake_connect, .handshake = fake_handshake,
        .read = fake_read, .write_readback = fake_write,
        .disconnect = fake_disconnect, .report = fake_report,
        .command_complete = fake_complete};

    fake_device fake = {0};
    edge_device_runtime runtime;
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7,
        platform_id, device_id, 1000U, 30U, 0U, &driver, &fake), "S7 init failed");
    fake.next_handshake_result = EDGE_IO_PROTOCOL_ERROR;
    edge_device_runtime_tick(&runtime, 0U, 0);
    require_true(fake.disconnects == 1U && !runtime.connected && !runtime.handshaken &&
        fake.reads == 0U, "invalid S7 handshake kept the old TCP session");
    edge_device_runtime_tick(&runtime, 1000U, 1000);
    require_true(fake.connects == 2U && fake.handshakes == 2U && fake.reads == 1U,
        "invalid S7 handshake did not reconnect before reading");

    fake.read_protocol_errors = 1U;
    edge_device_runtime_tick(&runtime, 2000U, 2000);
    require_true(fake.disconnects == 2U && fake.connects == 3U &&
        fake.handshakes == 3U && fake.reads == 3U && runtime.connected,
        "invalid S7 response did not reconnect, renegotiate and retry once");

    fake.read_protocol_errors = 2U;
    edge_device_runtime_tick(&runtime, 3000U, 3000);
    require_true(fake.disconnects == 4U && fake.connects == 4U &&
        fake.handshakes == 4U && fake.reads == 5U && !runtime.connected,
        "repeated invalid S7 responses kept a stale session or retried unboundedly");
    edge_device_runtime_tick(&runtime, 4000U, 4000);
    require_true(fake.connects == 5U && fake.handshakes == 5U && runtime.connected,
        "S7 protocol error did not recover at the next scheduled acquisition");

    fake.next_write_result = EDGE_IO_PROTOCOL_ERROR;
    edge_write_command command = {.value = {0x55U}, .value_size = 1U};
    require_true(edge_device_runtime_enqueue_write(&runtime, &command), "enqueue failed");
    edge_device_runtime_tick(&runtime, 5000U, 5000);
    require_true(fake.writes == 1U && fake.completions == 1U &&
        fake.last_command_result == EDGE_COMMAND_FAILED && !runtime.connected,
        "invalid S7 write response kept the TCP session or replayed the write");
    edge_device_runtime_tick(&runtime, 6000U, 6000);
    require_true(fake.connects == 6U && fake.handshakes == 6U &&
        fake.writes == 1U && runtime.connected,
        "S7 write failure did not re-handshake before the next read");
    edge_device_runtime_close(&runtime);
}

static void test_s7_tcp_client_scan_and_report_intervals(void) {
    const uint8_t platform_id[16] = {1U}, device_id[16] = {2U};
    const edge_device_driver driver = {
        .connect = fake_connect, .handshake = fake_handshake,
        .read = fake_read, .write_readback = fake_write,
        .disconnect = fake_disconnect, .report = fake_report,
        .command_complete = fake_complete};
    fake_device fake = {0};
    edge_device_runtime runtime;
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7,
        platform_id, device_id, EDGE_ACQUISITION_TICK_MS, 300U, 0U, &driver, &fake),
        "S7 300-second report runtime init failed");
    runtime.s7_tcp_client = true;
    fake.runtime = &runtime;

    edge_device_runtime_tick(&runtime, 0U, 0);
    require_true(fake.reads == 1U && fake.reports == 0U && !runtime.debug_read &&
        fake.silent_reads == 1U,
        "initial S7 scan reported before the ordinary interval or was not silent");
    edge_device_runtime_tick(&runtime, 1000U, 1000);
    require_true(fake.reads == 2U && fake.connects == 1U && fake.handshakes == 1U &&
        fake.disconnects == 0U && fake.reports == 0U && !runtime.debug_read &&
        runtime.silent_background_read == false && fake.silent_reads == 2U,
        "background 1-second scan did not reuse the session or isolate silent debug state");
    for (uint64_t now = 2000U; now <= 299000U; now += 1000U)
        edge_device_runtime_tick(&runtime, now, (int64_t)now);
    require_true(fake.reads == 300U && fake.reports == 0U && !runtime.debug_read &&
        fake.debug_reads == 0U && fake.silent_reads == 300U,
        "background S7 scans disturbed reporting or emitted automatic debug reads");
    edge_device_runtime_tick(&runtime, 300000U, 300000);
    require_true(fake.reads == 301U && fake.reports == 1U && runtime.debug_read &&
        fake.debug_reads == 1U && fake.connects == 1U && fake.handshakes == 1U &&
        fake.disconnects == 0U,
        "due S7 report did not use a fresh scan on the persistent session");

    fake.next_read_result = EDGE_IO_OFFLINE;
    edge_device_runtime_tick(&runtime, 600000U, 600000);
    require_true(fake.reports == 1U && runtime.latest.sampled_at_ms == 300000,
        "failed due S7 scan reported a stale snapshot");
    edge_device_runtime_close(&runtime);
    require_true(fake.disconnects == 1U, "S7 client session was not closed at shutdown");

    fake_device explicit_command = {0};
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7,
        platform_id, device_id, EDGE_ACQUISITION_TICK_MS, 300U, 0U, &driver, &explicit_command),
        "S7 command runtime init failed");
    runtime.s7_tcp_client = true;
    explicit_command.runtime = &runtime;
    edge_device_runtime_tick(&runtime, 0U, 0);
    edge_device_runtime_tick(&runtime, 1000U, 1000);
    runtime.debug_read = false;
    edge_write_command explicit_write = {.value = {0x55U}, .value_size = 1U};
    require_true(edge_device_runtime_enqueue_write(&runtime, &explicit_write),
        "S7 explicit command enqueue failed");
    edge_device_runtime_tick(&runtime, 2000U, 2000);
    require_true(explicit_command.writes == 1U && explicit_command.silent_writes == 0U &&
        explicit_command.silent_reads == 2U && explicit_command.reports == 0U,
        "silent background-read state suppressed an explicit S7 command");
    explicit_command.next_write_result = EDGE_IO_PROTOCOL_ERROR;
    edge_write_command failed_write = {.value = {0x66U}, .value_size = 1U};
    require_true(edge_device_runtime_enqueue_write(&runtime, &failed_write),
        "S7 failed command enqueue failed");
    edge_device_runtime_tick(&runtime, 3000U, 3000);
    require_true(explicit_command.writes == 2U && explicit_command.completions == 2U &&
        explicit_command.last_command_result == EDGE_COMMAND_FAILED && !runtime.connected,
        "failed write on a persistent S7 session was replayed or left the session open");
    edge_device_runtime_tick(&runtime, 4000U, 4000);
    require_true(explicit_command.writes == 2U && explicit_command.connects == 2U &&
        explicit_command.handshakes == 2U && runtime.connected,
        "persistent S7 client did not reconnect for the next read without replaying its write");
    edge_device_runtime_close(&runtime);

    fake_device explicit_interval = {0};
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7,
        platform_id, device_id, 300000U, 300U, 0U, &driver, &explicit_interval),
        "explicit S7 300-second scan interval rejected");
    runtime.s7_tcp_client = true;
    edge_device_runtime_tick(&runtime, 0U, 0);
    edge_device_runtime_tick(&runtime, 1000U, 1000);
    require_true(explicit_interval.reads == 2U && explicit_interval.reports == 0U,
        "explicit slow S7 interval was not covered by 1-second scanning");
    edge_device_runtime_tick(&runtime, 300000U, 300000);
    require_true(explicit_interval.reads == 3U && explicit_interval.reports == 1U,
        "S7 ordinary report deadline was not respected independently of scan cadence");
    edge_device_runtime_close(&runtime);
}

static void test_universal_scan_reporting_and_fast_due(void) {
    const uint8_t platform_id[16] = {9U}, device_id[16] = {8U};
    const edge_device_driver driver = {
        .connect = fake_connect, .handshake = fake_handshake,
        .read = fake_read, .write_readback = fake_write,
        .disconnect = fake_disconnect, .report = fake_report,
        .command_complete = fake_complete};
    for (unsigned protocol = EDGE_DEVICE_MODBUS; protocol <= EDGE_DEVICE_DLT645; ++protocol) {
        fake_device fake = {0};
        edge_device_runtime runtime;
        require_true(edge_device_runtime_init(&runtime, (edge_device_protocol)protocol,
            platform_id, device_id, 300000U, 300U, 0U, &driver, &fake),
            "slow configured interval was rejected");
        fake.runtime = &runtime;
        for (uint64_t now = 0; now < 300000U; now += EDGE_ACQUISITION_TICK_MS)
            edge_device_runtime_tick(&runtime, now, (int64_t)now);
        require_true(fake.reads == 300U && fake.reports == 0U &&
                     fake.silent_reads == 300U && fake.debug_reads == 0U,
            "polling was not silent before the first ordinary due");
        edge_device_runtime_tick(&runtime, 300000U, 300000);
        require_true(fake.reads == 301U && fake.reports == 1U &&
                     fake.debug_reads == 1U && fake.silent_reads == 300U,
            "ordinary due did not debug/report exactly its fresh sample");
        edge_device_runtime_tick(&runtime, 900000U, 900000);
        require_true(fake.reads == 302U && fake.reports == 2U &&
                     runtime.next_io_at_ms == 901000U,
            "late tick caused catch-up reads or missed its due report");
        edge_device_runtime_close(&runtime);
    }

    fake_device fake = {0};
    edge_device_runtime runtime;
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_MODBUS,
        platform_id, device_id, 300000U, 300U, 0U, &driver, &fake),
        "fast-window runtime init failed");
    edge_device_runtime_tick(&runtime, 0U, 0);
    edge_write_command command = {.value = {0x44U}, .value_size = 1U,
        .fast_read_duration_sec = 10U, .fast_read_interval_sec = 4U};
    require_true(edge_device_runtime_enqueue_write(&runtime, &command),
        "priority write enqueue failed");
    edge_device_runtime_tick(&runtime, 1000U, 1000);
    require_true(fake.writes == 1U && fake.completions == 1U && fake.reports == 0U &&
        fake.reads == 1U, "write was not prioritized or generated an extra read/report");
    edge_device_runtime_tick(&runtime, 5000U, 5000);
    require_true(fake.reads == 2U && fake.reports == 1U,
        "fast due did not report its newly read sample");
    fake.next_read_result = EDGE_IO_OFFLINE;
    edge_device_runtime_tick(&runtime, 9000U, 9000);
    require_true(fake.reads == 3U && fake.reports == 1U,
        "failed fast-due scan reported stale data");
    edge_device_runtime_tick(&runtime, 20000U, 20000);
    require_true(fake.reads == 4U && fake.reports == 1U,
        "expired fast window reported outside a due round");
    edge_device_runtime_close(&runtime);

    fake = (fake_device){0};
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_MODBUS,
        platform_id, device_id, EDGE_ACQUISITION_TICK_MS, 3U, 0U, &driver, &fake),
        "failed-due runtime init failed");
    edge_device_runtime_tick(&runtime, 0U, 0);
    require_true(fake.reads == 1U && fake.reports == 0U,
        "initial sample reported before the ordinary interval");
    fake.next_read_result = EDGE_IO_OFFLINE;
    edge_device_runtime_tick(&runtime, 3000U, 3000);
    require_true(fake.reads == 2U && fake.reports == 0U &&
        runtime.initial_report_pending,
        "failed first due reported a stale sample or cleared pending state");
    edge_device_runtime_tick(&runtime, 4000U, 4000);
    require_true(fake.reads == 3U && fake.reports == 0U,
        "successful non-due scan reported after a failed due");
    edge_device_runtime_tick(&runtime, 6000U, 6000);
    require_true(fake.reads == 4U && fake.reports == 1U &&
        !runtime.initial_report_pending,
        "first valid due sample did not complete the initial report");
    edge_device_runtime_close(&runtime);

    fake = (fake_device){0};
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_MODBUS,
        platform_id, device_id, EDGE_ACQUISITION_TICK_MS, 2U, 0U, &driver, &fake),
        "Modbus silent/due runtime init failed");
    fake.runtime = &runtime;
    edge_device_runtime_tick(&runtime, 0U, 0);
    edge_device_runtime_tick(&runtime, 1000U, 1000);
    require_true(fake.reads == 2U && fake.silent_reads == 2U && fake.debug_reads == 0U &&
        fake.reports == 0U, "background Modbus scan emitted debug or telemetry");
    fake.read_timeouts = 1U;
    edge_device_runtime_tick(&runtime, 2000U, 2000);
    require_true(fake.reads == 3U && fake.debug_reads == 1U && fake.reports == 0U &&
        runtime.initial_report_pending,
        "timed-out Modbus due reported stale telemetry or lost its due state");
    edge_device_runtime_tick(&runtime, 3000U, 3000);
    require_true(fake.reads == 4U && fake.reports == 0U,
        "non-due Modbus scan reported stale data after timeout");
    edge_device_runtime_tick(&runtime, 4000U, 4000);
    require_true(fake.reads == 5U && fake.debug_reads == 2U && fake.reports == 1U &&
        !runtime.initial_report_pending,
        "fresh Modbus due sample did not emit debug and telemetry once");
    edge_device_runtime_close(&runtime);

    fake = (fake_device){0};
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_MODBUS,
        platform_id, device_id, EDGE_ACQUISITION_TICK_MS, 3U, 0U, &driver, &fake),
        "write-at-due runtime init failed");
    edge_device_runtime_tick(&runtime, 0U, 0);
    edge_write_command due_command = {.value = {0x44U}, .value_size = 1U};
    require_true(edge_device_runtime_enqueue_write(&runtime, &due_command),
        "due write enqueue failed");
    edge_device_runtime_tick(&runtime, 3000U, 3000);
    require_true(fake.writes == 1U && fake.completions == 1U &&
        fake.reads == 1U && fake.reports == 0U,
        "due write did not take priority over read or reported its readback as telemetry");
    edge_device_runtime_tick(&runtime, 4000U, 4000);
    require_true(fake.reads == 2U && fake.reports == 0U,
        "non-due read reported after the write consumed the due round");
    edge_device_runtime_tick(&runtime, 6000U, 6000);
    require_true(fake.reads == 3U && fake.reports == 1U,
        "next ordinary due did not report a fresh sample after write priority");
    edge_device_runtime_close(&runtime);
}

int main(void) {
    test_s7_tcp_client_scan_and_report_intervals();
    test_configured_read_interval();
    test_fast_reporting_after_write();
    test_initial_report_waits_for_first_success();
    test_io_and_reporting();
    test_s7_immediate_retry_is_bounded();
    test_s7_protocol_error_reconnects();
    test_modbus();
    test_s7();
    test_universal_scan_reporting_and_fast_due();
    puts("edge runtime tests passed");
    return 0;
}
