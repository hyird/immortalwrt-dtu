#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* Exercise the production local-command adapter and result consumer without
 * starting remote platform sessions. Unused application sections are discarded. */
#include "../src/edge_ws.c"

typedef struct {
    edge_ws_app *app;
    struct ev_loop *loop;
    int plc_listener, plc_fd;
    uint16_t plc_port, gateway_port;
    uint16_t registers[64];
    bool coil, hold_writes, fail_write, mismatch_write;
    uint8_t input[260], held[260];
    size_t used, held_size;
    unsigned reads, writes, samples, reports, results;
    char directory[64], map_path[96];
    iot_edge_v1_ConfigItem items[7];
    edge_runtime_config config;
    uint8_t platform[16];
} fixture;

static fixture *current_fixture;
static uint8_t north_unit = 1U;

static uint16_t word(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] << 8U | bytes[1]);
}

static void put_word(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void send_all(int fd, const uint8_t *bytes, size_t size) {
    while (size) {
        const ssize_t sent = send(fd, bytes, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        assert(sent > 0);
        bytes += (size_t)sent;
        size -= (size_t)sent;
    }
}

static int listener(uint16_t *port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    *port = ntohs(address.sin_port);
    assert(listen(fd, 8) == 0);
    return fd;
}

static void respond_plc(fixture *test, const uint8_t *request, size_t size) {
    const uint8_t function = request[7];
    const uint16_t address = word(request + 8);
    uint8_t response[260];
    memcpy(response, request, 8);
    size_t response_size = 12U;
    if (function == 3U || function == 1U) {
        ++test->reads;
        const uint16_t quantity = word(request + 10);
        if (function == 1U) {
            assert(address == 5U && quantity == 1U);
            response[8] = 1U;
            response[9] = test->coil ? 1U : 0U;
            response_size = 10U;
        } else {
            assert((size_t)address + quantity <= 64U);
            response[8] = (uint8_t)(quantity * 2U);
            for (uint16_t i = 0U; i < quantity; ++i)
                put_word(response + 9U + i * 2U, test->registers[address + i]);
            response_size = 9U + quantity * 2U;
        }
    } else {
        assert(function == 5U || function == 6U || function == 16U);
        ++test->writes;
        if (test->fail_write) {
            test->fail_write = false;
            response[7] |= 0x80U;
            response[8] = 4U;
            response_size = 9U;
        } else if (function == 5U) {
            assert(address == 5U);
            test->coil = word(request + 10) == 0xff00U;
            memcpy(response + 8, request + 8, 4U);
        } else if (function == 6U) {
            assert(address < 64U);
            test->registers[address] = word(request + 10);
            if (test->mismatch_write) {
                test->mismatch_write = false;
                --test->registers[address];
            }
            memcpy(response + 8, request + 8, 4U);
        } else {
            const uint16_t quantity = word(request + 10);
            assert((size_t)address + quantity <= 64U && size == 13U + quantity * 2U);
            for (uint16_t i = 0U; i < quantity; ++i)
                test->registers[address + i] = word(request + 13U + i * 2U);
            memcpy(response + 8, request + 8, 4U);
        }
    }
    put_word(response + 4, (uint16_t)(response_size - 6U));
    send_all(test->plc_fd, response, response_size);
}

static void service_plc(fixture *test) {
    if (test->plc_fd < 0) {
        struct pollfd ready = {.fd = test->plc_listener, .events = POLLIN};
        if (poll(&ready, 1U, 0) > 0) {
            test->plc_fd = accept4(test->plc_listener, NULL, NULL, SOCK_CLOEXEC);
            assert(test->plc_fd >= 0);
        }
    }
    if (test->plc_fd < 0 || test->held_size)
        return;
    const ssize_t received = recv(test->plc_fd, test->input + test->used,
        sizeof(test->input) - test->used, MSG_DONTWAIT);
    if (received < 0) {
        assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        return;
    }
    if (received == 0) {
        close(test->plc_fd);
        test->plc_fd = -1;
        test->used = 0U;
        return;
    }
    test->used += (size_t)received;
    if (test->used < 6U)
        return;
    const size_t size = 6U + word(test->input + 4);
    assert(size <= sizeof(test->input) && test->used <= size);
    if (test->used != size)
        return;
    const bool writing = test->input[7] == 5U || test->input[7] == 6U || test->input[7] == 16U;
    if (writing && test->hold_writes) {
        memcpy(test->held, test->input, size);
        test->held_size = size;
    } else {
        respond_plc(test, test->input, size);
    }
    test->used = 0U;
}

static void step(fixture *test) {
    ev_run(test->loop, EVRUN_NOWAIT);
    service_plc(test);
    edge_acquisition_tick(test->app->acquisition, monotonic_ms());
    ev_run(test->loop, EVRUN_NOWAIT);
    usleep(1000U);
}

static void spin(fixture *test, unsigned duration_ms) {
    const uint64_t until = monotonic_ms() + duration_ms;
    while (monotonic_ms() < until)
        step(test);
}

static bool store_record(void *context, const uint8_t platform[16],
                          const iot_edge_v1_TelemetryRecord *record) {
    (void)context;
    assert(!memcmp(platform, current_fixture->platform, 16U));
    assert(record->values_count ==
        (record->protocol == iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO ? 1U : 4U));
    ++current_fixture->reports;
    return true;
}

static void store_sample(void *context, const uint8_t platform[16],
                           const edge_acquisition_sample *sample) {
    if (sample != NULL) {
        assert(sample->valid);
        ++current_fixture->samples;
    }
    acquisition_sample(context, platform, sample);
}

static bool store_result(void *context, const uint8_t platform[16],
                          const iot_edge_v1_CommandResult *result) {
    ++current_fixture->results;
    assert(consume_gateway_result(context, platform, result));
    return true; /* Local IDs never enter any platform outbox. */
}

static void id(void *field, unsigned value) {
    const pb_size_t size = 16U;
    memcpy(field, &size, sizeof(size));
    memset((uint8_t *)field + sizeof(size), 0, 16U);
    *((uint8_t *)field + sizeof(size)) = (uint8_t)value;
}

static unsigned pending_count(const fixture *test) {
    unsigned count = 0U;
    for (size_t i = 0U; i < 8U; ++i)
        count += test->app->gateway_commands[i].active ? 1U : 0U;
    return count;
}

static void setup(fixture *test, bool short_period, bool external_io) {
    memset(test, 0, sizeof(*test));
    test->plc_fd = -1;
    test->platform[0] = 3U;
    test->loop = ev_loop_new(0);
    assert(test->loop != NULL);
    test->app = calloc(1U, sizeof(*test->app));
    assert(test->app != NULL);
    test->app->loop = test->loop;
    strcpy(test->directory, "gateway-write-XXXXXX");
    assert(mkdtemp(test->directory) != NULL);
    snprintf(test->map_path, sizeof(test->map_path), "%s/map", test->directory);
    test->plc_listener = listener(&test->plc_port);
    const int reserve = listener(&test->gateway_port);
    close(reserve);
    iot_edge_v1_EndpointConfig *endpoint = &test->items[0].item.endpoint;
    test->items[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    test->items[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    id(&endpoint->endpoint_id, 1U);
    endpoint->enabled = true;
    endpoint->transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    endpoint->mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    endpoint->protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(endpoint->ip, "127.0.0.1");
    endpoint->port = test->plc_port;
    iot_edge_v1_DeviceConfig *device = &test->items[1].item.device;
    test->items[1].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DEVICE;
    test->items[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    id(&device->device_id, 2U);
    id(&device->endpoint_id, 1U);
    device->enabled = true;
    strcpy(device->device_code, "GATEWAY-WRITE");
    device->protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(device->modbus_mode, "TCP");
    device->modbus_slave_id = 1U;
    device->io_interval_ms = short_period ? 0U : 300000U;
    device->report_interval_sec = short_period ? 3U : 300U;
    const char *names[] = {"readonly", "wide", "word", "flag"};
    const char *types[] = {"UINT16", "UINT32", "UINT16", "BOOL"};
    const uint16_t addresses[] = {10U, 20U, 30U, 5U};
    for (unsigned i = 0U; i < 4U; ++i) {
        test->items[i + 2U].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_MODBUS_REGISTER;
        test->items[i + 2U].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
        iot_edge_v1_ModbusRegisterConfig *point = &test->items[i + 2U].item.modbus_register;
        id(&point->device_id, 2U);
        strcpy(point->element_id, names[i]);
        strcpy(point->data_type, types[i]);
        strcpy(point->register_type, i == 3U ? "COIL" : "HOLDING_REGISTER");
        strcpy(point->byte_order, "BIG_ENDIAN");
        point->address = addresses[i];
        point->quantity = i == 1U ? 2U : 1U;
        point->scale = 1.0;
        point->writable = i != 0U;
    }
    if (external_io) {
        test->items[6].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_EXTERNAL_IO;
        test->items[6].which_item = iot_edge_v1_ConfigItem_external_io_tag;
        iot_edge_v1_ExternalIoPointConfig *point = &test->items[6].item.external_io;
        id(&point->device_id, 2U);
        id(&point->model_id, 4U);
        strcpy(point->point_id, "00000000-0000-4000-8000-000000000004");
        strcpy(point->name, "external output");
        strcpy(point->channel, "do0");
        point->kind = iot_edge_v1_ExternalIoKind_EXTERNAL_IO_DO;
    }
    test->config = (edge_runtime_config){.items = test->items, .item_count = external_io ? 7U : 6U,
        .endpoint_count = 1U, .device_count = 1U, .revision = 1U};
    edge_acquisition_source source = {.platform_id = test->platform, .config = &test->config,
        .external_io = external_io};
    test->app->gateway = edge_modbus_gateway_prepare(test->map_path, &source, 1U);
    assert(test->app->gateway != NULL);
    test->app->gateway_server = edge_modbus_gateway_listen(test->loop, test->app->gateway,
        "lo", test->gateway_port);
    assert(test->app->gateway_server != NULL);
    edge_modbus_gateway_set_writer(test->app->gateway_server, gateway_write, test->app);
    current_fixture = test;
    test->app->acquisition = edge_acquisition_create(store_record, store_result, test->app);
    assert(test->app->acquisition != NULL);
    edge_acquisition_set_sample_callback(test->app->acquisition, store_sample);
    char error[256] = {0};
    assert(edge_acquisition_apply_multi(test->app->acquisition, &source, 1U,
        monotonic_ms(), error, sizeof(error)));
    test->registers[10] = 7U;
    test->registers[20] = 0x1122U;
    test->registers[21] = 0x3344U;
    test->registers[30] = 9U;
    assert(edge_acquisition_start(test->app->acquisition, error, sizeof(error)));
    const uint64_t until = monotonic_ms() + 3000U;
    const unsigned initial_samples = external_io ? 5U : 4U;
    const unsigned initial_reports = external_io ? 2U : 1U;
    while ((test->samples < initial_samples || test->reports < initial_reports) &&
           monotonic_ms() < until)
        step(test);
    assert(test->samples == initial_samples && test->reads == 4U &&
           test->reports == initial_reports);
}

static void teardown(fixture *test) {
    edge_modbus_gateway_close(test->app->gateway_server);
    test->app->gateway_server = NULL;
    edge_acquisition_destroy(test->app->acquisition);
    gateway_commands_stopped(test->app);
    edge_modbus_gateway_free(test->app->gateway);
    if (test->plc_fd >= 0)
        close(test->plc_fd);
    close(test->plc_listener);
    ev_loop_destroy(test->loop);
    assert(unlink(test->map_path) == 0);
    assert(rmdir(test->directory) == 0);
    free(test->app);
}

static int connect_north(fixture *test) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    const struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(test->gateway_port)};
    assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

static void request(int fd, uint16_t transaction, uint8_t function,
                     uint16_t address, uint16_t quantity_or_value,
                     const uint8_t *values, size_t bytes) {
    uint8_t frame[260] = {0};
    put_word(frame, transaction);
    frame[6] = north_unit;
    frame[7] = function;
    put_word(frame + 8, address);
    put_word(frame + 10, quantity_or_value);
    size_t size = 12U;
    if (values != NULL) {
        frame[12] = (uint8_t)bytes;
        memcpy(frame + 13, values, bytes);
        size = 13U + bytes;
    }
    put_word(frame + 4, (uint16_t)(size - 6U));
    send_all(fd, frame, size);
}

static size_t response(fixture *test, int fd, uint16_t transaction, uint8_t bytes[260]) {
    const uint64_t until = monotonic_ms() + 5000U;
    size_t used = 0U;
    while (monotonic_ms() < until) {
        step(test);
        const ssize_t received = recv(fd, bytes + used, 260U - used, MSG_DONTWAIT);
        if (received < 0) {
            assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            continue;
        }
        assert(received > 0);
        used += (size_t)received;
        if (used >= 6U && used == 6U + word(bytes + 4)) {
            assert(word(bytes) == transaction && bytes[6] == north_unit);
            return used;
        }
    }
    assert(!"northbound response timed out");
    return 0U;
}

static void expect_echo(fixture *test, int fd, uint16_t transaction, uint8_t function,
                         uint16_t address, uint16_t value) {
    uint8_t bytes[260];
    assert(response(test, fd, transaction, bytes) == 12U);
    assert(bytes[7] == function && word(bytes + 8) == address && word(bytes + 10) == value);
}

static void expect_read(fixture *test, int fd, uint16_t transaction,
                         uint16_t address, uint16_t quantity, const uint16_t *expected) {
    request(fd, transaction, 3U, address, quantity, NULL, 0U);
    uint8_t bytes[260];
    assert(response(test, fd, transaction, bytes) == 9U + quantity * 2U);
    assert(bytes[7] == 3U && bytes[8] == quantity * 2U);
    for (uint16_t i = 0U; i < quantity; ++i)
        assert(word(bytes + 9U + i * 2U) == expected[i]);
}

static void wait_held(fixture *test) {
    const uint64_t until = monotonic_ms() + 3000U;
    while (!test->held_size && monotonic_ms() < until)
        step(test);
    assert(test->held_size && pending_count(test) == 1U);
}

static void release_write(fixture *test) {
    assert(test->held_size);
    test->hold_writes = false;
    respond_plc(test, test->held, test->held_size);
    test->held_size = 0U;
}

static void verify_reads_follow_acquisition(void) {
    fixture test;
    setup(&test, true, false);
    const int north = connect_north(&test);
    const uint16_t original[] = {7U, 0x1122U, 0x3344U, 9U};
    const uint64_t until = monotonic_ms() + 1300U;
    uint16_t transaction = 1U;
    while (monotonic_ms() < until) {
        expect_read(&test, north, transaction++, 0U, 4U, original);
        assert(test.reads == 4U && test.samples == 4U && test.reports == 1U);
        spin(&test, 20U);
    }
    /* Change the real device, not the gateway cache. Only the second configured
     * ordinary scan makes these values visible despite frequent slave reads. */
    test.registers[10] = 8U;
    const uint64_t next_scan = monotonic_ms() + 4000U;
    while ((test.samples < 8U || test.reports < 2U) && monotonic_ms() < next_scan)
        step(&test);
    assert(test.reads == 8U && test.samples == 8U && test.reports == 2U);
    const uint16_t next[] = {8U, 0x1122U, 0x3344U, 9U};
    expect_read(&test, north, transaction, 0U, 4U, next);
    assert(test.reads == 8U);
    close(north);
    teardown(&test);
}

static void verify_write_chain(void) {
    fixture test;
    setup(&test, false, false);
    int first = connect_north(&test);
    int second = connect_north(&test);
    int reader = connect_north(&test);
    uint8_t bytes[260];

    request(first, 1U, 6U, 0U, 99U, NULL, 0U);
    expect_echo(&test, first, 1U, 6U, 0U, 99U);
    assert(test.writes == 0U && test.registers[10] == 7U);
    const uint16_t readonly[] = {7U};
    expect_read(&test, reader, 2U, 0U, 1U, readonly);
    const uint8_t ignored[] = {0U, 1U, 0U, 2U};
    request(first, 3U, 16U, 100U, 2U, ignored, sizeof(ignored));
    expect_echo(&test, first, 3U, 16U, 100U, 2U);
    assert(test.writes == 0U && pending_count(&test) == 0U);
    const uint8_t malformed[] = {1U, 2U, 3U};
    request(first, 4U, 16U, 1U, 2U, malformed, sizeof(malformed));
    assert(response(&test, first, 4U, bytes) == 9U && bytes[7] == 0x90U && bytes[8] == 3U);
    assert(test.writes == 0U);

    /* Concurrent partial writes must merge with the first command's REAL
     * readback, while another client's reads stay responsive throughout. */
    test.hold_writes = true;
    request(first, 5U, 6U, 1U, 0xaaaaU, NULL, 0U);
    wait_held(&test);
    request(second, 6U, 6U, 2U, 0xbbbbU, NULL, 0U);
    spin(&test, 50U);
    assert(pending_count(&test) == 1U);
    iot_edge_v1_CommandResult foreign = iot_edge_v1_CommandResult_init_zero;
    foreign.command_id.size = foreign.device_id.size = 16U;
    foreign.device_id.bytes[0] = 2U;
    memcpy(foreign.command_id.bytes, test.app->gateway_commands[0].command_id, 16U);
    foreign.state = iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
    assert(!consume_gateway_result(test.app, (const uint8_t[16]){9U}, &foreign));
    assert(pending_count(&test) == 1U);
    const uint16_t before[] = {0x1122U, 0x3344U};
    expect_read(&test, reader, 7U, 1U, 2U, before);
    assert(recv(first, bytes, sizeof(bytes), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    release_write(&test);
    expect_echo(&test, first, 5U, 6U, 1U, 0xaaaaU);
    expect_echo(&test, second, 6U, 6U, 2U, 0xbbbbU);
    assert(test.writes == 2U && test.registers[20] == 0xaaaaU && test.registers[21] == 0xbbbbU);
    const uint16_t after[] = {0xaaaaU, 0xbbbbU};
    expect_read(&test, reader, 8U, 1U, 2U, after);
    assert(pending_count(&test) == 0U && test.results == 2U);

    request(first, 9U, 5U, 0U, 0xff00U, NULL, 0U);
    expect_echo(&test, first, 9U, 5U, 0U, 0xff00U);
    assert(test.coil && test.writes == 3U);
    request(reader, 10U, 1U, 0U, 1U, NULL, 0U);
    assert(response(&test, reader, 10U, bytes) == 10U && bytes[9] == 1U);
    const uint8_t clear[] = {0U};
    request(first, 11U, 15U, 0U, 1U, clear, sizeof(clear));
    expect_echo(&test, first, 11U, 15U, 0U, 1U);
    assert(!test.coil && test.writes == 4U);

    test.fail_write = true;
    request(first, 12U, 6U, 3U, 123U, NULL, 0U);
    assert(response(&test, first, 12U, bytes) == 9U && bytes[7] == 0x86U && bytes[8] == 4U);
    assert(test.registers[30] == 9U && pending_count(&test) == 0U);
    const uint16_t unchanged[] = {9U};
    expect_read(&test, reader, 13U, 3U, 1U, unchanged);

    test.mismatch_write = true;
    request(first, 20U, 6U, 3U, 10U, NULL, 0U);
    assert(response(&test, first, 20U, bytes) == 9U && bytes[7] == 0x86U && bytes[8] == 4U);
    assert(test.registers[30] == 9U);
    expect_read(&test, reader, 21U, 3U, 1U, unchanged);

    const uint8_t successful_batch[] = {0xcaU, 0xfeU, 0xbaU, 0xbeU, 0U, 9U};
    const unsigned before_batch = test.writes;
    request(first, 19U, 16U, 1U, 3U, successful_batch, sizeof(successful_batch));
    expect_echo(&test, first, 19U, 16U, 1U, 3U);
    assert(test.writes == before_batch + 2U && test.registers[20] == 0xcafeU &&
        test.registers[21] == 0xbabeU && test.registers[30] == 9U);

    /* A disconnected batch must stop after the already-submitted first point.
     * The newly accepted connection cannot inherit its late acknowledgement. */
    test.hold_writes = true;
    const uint8_t batch[] = {0x12U, 0x34U, 0x56U, 0x78U, 0x99U, 0x99U};
    const unsigned writes_before = test.writes;
    request(first, 14U, 16U, 1U, 3U, batch, sizeof(batch));
    wait_held(&test);
    close(first);
    spin(&test, 30U);
    first = connect_north(&test);
    expect_read(&test, first, 15U, 3U, 1U, unchanged);
    release_write(&test);
    const uint64_t completed = monotonic_ms() + 3000U;
    while (pending_count(&test) && monotonic_ms() < completed)
        step(&test);
    spin(&test, 100U);
    assert(pending_count(&test) == 0U && test.writes == writes_before + 1U);
    assert(test.registers[30] == 9U);
    assert(recv(first, bytes, sizeof(bytes), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    const uint16_t real[] = {0x1234U, 0x5678U};
    expect_read(&test, first, 16U, 1U, 2U, real);

    test.hold_writes = true;
    request(second, 22U, 16U, 1U, 3U, batch, sizeof(batch));
    wait_held(&test);
    request(reader, 25U, 6U, 3U, 123U, NULL, 0U);
    spin(&test, 50U); /* Old-ID write is queued behind the real in-flight write. */
    edge_acquisition_source replacement_source = {.platform_id = test.platform, .config = &test.config};
    edge_modbus_gateway *replacement = edge_modbus_gateway_prepare(test.map_path, &replacement_source, 1U);
    assert(replacement && edge_modbus_gateway_set_unit_id(replacement, 247U));
    const unsigned map_replace_writes = test.writes;
    edge_modbus_gateway_replace(test.app->gateway_server, replacement);
    edge_modbus_gateway_free(test.app->gateway);
    test.app->gateway = replacement;
    north_unit = 247U;
    close(first); close(second); close(reader);
    first = connect_north(&test);
    second = connect_north(&test);
    reader = connect_north(&test);
    north_unit = 1U;
    request(first, 26U, 6U, 3U, 123U, NULL, 0U);
    north_unit = 247U;
    spin(&test, 50U);
    assert(recv(first, bytes, sizeof(bytes), MSG_DONTWAIT) == 0);
    close(first);
    first = connect_north(&test);
    release_write(&test);
    const uint64_t map_replaced = monotonic_ms() + 3000U;
    while (pending_count(&test) && monotonic_ms() < map_replaced)
        step(&test);
    spin(&test, 100U);
    assert(pending_count(&test) == 0U && test.writes == map_replace_writes + 1U);
    assert(test.registers[30] == 9U);
    expect_read(&test, reader, 23U, 1U, 2U, real);

    /* Destroy/recreate the listening server while a real command is in flight:
     * only its ID survives in the app, never a dangling server/client pointer. */
    test.hold_writes = true;
    request(second, 17U, 16U, 1U, 3U, batch, sizeof(batch));
    wait_held(&test);
    const unsigned replacing_writes = test.writes;
    edge_modbus_gateway_close(test.app->gateway_server);
    test.app->gateway_server = edge_modbus_gateway_listen(test.loop, test.app->gateway,
        "lo", test.gateway_port);
    assert(test.app->gateway_server != NULL && pending_count(&test) == 1U);
    edge_modbus_gateway_set_writer(test.app->gateway_server, gateway_write, test.app);
    close(first); close(second); close(reader);
    first = connect_north(&test);
    request(first, 24U, 6U, 2U, 0xffffU, NULL, 0U);
    assert(response(&test, first, 24U, bytes) == 9U && bytes[7] == 0x86U && bytes[8] == 6U);
    assert(pending_count(&test) == 1U);
    release_write(&test);
    const uint64_t replaced = monotonic_ms() + 3000U;
    while (pending_count(&test) && monotonic_ms() < replaced)
        step(&test);
    spin(&test, 100U);
    assert(pending_count(&test) == 0U && test.writes == replacing_writes + 1U);
    assert(test.registers[30] == 9U);
    expect_read(&test, first, 18U, 1U, 2U, real);
    close(first);
    teardown(&test);
    north_unit = 1U;
}

static void verify_external_output_readback(void) {
    fixture test;
    setup(&test, false, true);
    const int north = connect_north(&test);
    /* The protocol Bool is bit 0; the external output is the next dense bit. */
    request(north, 1U, 5U, 1U, 0xff00U, NULL, 0U);
    expect_echo(&test, north, 1U, 5U, 1U, 0xff00U);
    assert(test.writes == 0U && test.results == 1U && pending_count(&test) == 0U);
    uint8_t bytes[260];
    request(north, 2U, 2U, 1U, 1U, NULL, 0U);
    assert(response(&test, north, 2U, bytes) == 10U && bytes[9] == 1U);
    /* The ordinary stub snapshot still says zero. This one is the actual
     * confirmed write readback, not a fabricated optimistic expected value. */
    close(north);
    teardown(&test);
}

int main(void) {
    verify_reads_follow_acquisition();
    verify_write_chain();
    verify_external_output_readback();
    puts("Modbus gateway acquisition/write integration tests passed");
    return 0;
}
