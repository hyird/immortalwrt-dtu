#undef NDEBUG
#include "edge_sl651.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t sent[EDGE_SL651_FRAME_MAX];
static size_t sent_size;
static unsigned reports, commands;
static unsigned failures;
static uint64_t token;
static bool send_bytes(void *context, const uint8_t *bytes, size_t size) {
    (void)context;
    memcpy(sent, bytes, size);
    sent_size = size;
    return true;
}
static bool report(void *context, uint64_t value, const edge_sl651_frame *frame) {
    (void)context;
    assert(frame->body_size >= 8);
    token = value;
    ++reports;
    return true;
}
static void command(void *context, const uint8_t id[16], bool success, const char *reason) {
    (void)context;
    (void)id;
    (void)reason;
    if (success)
        ++commands;
    else
        ++failures;
}
static size_t upstream(uint8_t *out, uint8_t function, uint8_t ending, unsigned serial) {
    const uint8_t bytes[] = {0x7E, 0x7E, 1, 0, 0, 0, 0, 1,    0,    0,    0,    0, 12, 2, 0,
                             0,    0x24, 1, 2, 3, 4, 5, 0xAB, 0x12, 0x12, 0x34, 3, 0,  0};
    memcpy(out, bytes, sizeof(bytes));
    out[10] = function;
    out[15] = (uint8_t)serial;
    out[26] = ending;
    uint16_t crc = edge_sl651_crc(out, 27);
    out[27] = (uint8_t)(crc >> 8U);
    out[28] = (uint8_t)crc;
    return sizeof(bytes);
}
static size_t packet(uint8_t *out, unsigned total, unsigned sequence, uint8_t ending,
                     const uint8_t *body, size_t size) {
    (void)upstream(out, 0x32, ending, 1);
    out[12] = (uint8_t)(size + 3);
    out[13] = 0x16;
    unsigned packed = total * 4096 + sequence;
    out[14] = (uint8_t)(packed >> 16);
    out[15] = (uint8_t)(packed >> 8);
    out[16] = (uint8_t)packed;
    memcpy(out + 17, body, size);
    out[17 + size] = ending;
    uint16_t crc = edge_sl651_crc(out, 18 + size);
    out[18 + size] = (uint8_t)(crc >> 8);
    out[19 + size] = (uint8_t)crc;
    return 20 + size;
}
static void test_missing_packets_and_timeout(void) {
    const uint8_t station[5] = {0, 0, 0, 0, 1}, time[6] = {0x24, 1, 2, 3, 4, 5};
    edge_sl651_callbacks callbacks = {send_bytes, report, command, NULL};
    edge_sl651_session *s = edge_sl651_create(3, station, callbacks, NULL);
    uint8_t input[64], body[12] = {0, 1, 0x24, 1, 2, 3, 4, 5, 0xAB, 0x12, 0x12, 0x34};
    reports = commands = failures = 0;
    sent_size = 0;
    size_t n = packet(input, 3, 3, 3, body + 10, 2);
    edge_sl651_receive(s, input, n, 1000, time);
    assert(!reports && sent[sent_size - 3] == 0x15 && sent[16] == 1);
    n = packet(input, 3, 1, 3, body, 8);
    edge_sl651_receive(s, input, n, 1001, time);
    assert(!reports && sent[sent_size - 3] == 0x15 && sent[16] == 2);
    n = packet(input, 3, 2, 3, body + 8, 2);
    edge_sl651_receive(s, input, n, 1002, time);
    assert(reports == 1 && sent[sent_size - 3] == 0x15);
    assert(edge_sl651_report_frame_count(s) == 3);
    for (size_t index = 0; index < 3; ++index) {
        const uint8_t *raw;
        size_t raw_size;
        assert(edge_sl651_report_frame(s, index, &raw, &raw_size));
        edge_sl651_frame original;
        assert(edge_sl651_parse(raw, raw_size, &original));
        assert(original.sequence == index + 1 && original.total == 3);
        uint8_t expected[64];
        size_t expected_size = packet(expected, 3, (unsigned)index + 1, 3,
                                      body + (index == 0 ? 0 : index == 1 ? 8 : 10),
                                      index == 0 ? 8 : 2);
        assert(raw_size == expected_size && memcmp(raw, expected, raw_size) == 0);
    }
    edge_sl651_commit(s, token + 1, 1002, time);
    assert(sent[sent_size - 3] == 0x15);
    edge_sl651_commit(s, token, 1002, time);
    assert(sent[sent_size - 3] == 4 && sent[16] == 3 && sent[18] == 1);
    /* A final packet repeated after EOT loss is safely confirmed again. */
    n = packet(input, 3, 3, 3, body + 10, 2);
    edge_sl651_receive(s, input, n, 1003, time);
    assert(reports == 1);
    edge_sl651_commit(s, token, 1003, time);
    assert(sent[sent_size - 3] == 4 && sent[16] == 3);
    uint8_t id[16] = {1};
    assert(edge_sl651_query(s, id, 0x37, NULL, 0, 2000, 1000, time));
    uint8_t request[64];
    size_t request_size = sent_size;
    memcpy(request, sent, sent_size);
    for (unsigned retry = 1; retry <= 2; ++retry) {
        edge_sl651_tick(s, 2000 + retry * 1000, time);
        assert(!failures && sent_size == request_size && !memcmp(request, sent, sent_size));
    }
    edge_sl651_tick(s, 5000, time);
    assert(failures == 1 && !edge_sl651_query(s, id, 0x37, NULL, 0, 5001, 1000, time));
    edge_sl651_destroy(s);
    s = edge_sl651_create(2, station, callbacks, NULL);
    reports = 0;
    sent_size = 0;
    n = upstream(input, 0x32, 3, 1);
    input[n - 1] ^= 1;
    edge_sl651_receive(s, input, n, 1000, time);
    assert(!reports && !sent_size);
    edge_sl651_destroy(s);
}
int main(void) {
    test_missing_packets_and_timeout();
    const uint8_t station[5] = {0, 0, 0, 0, 1}, time[6] = {0x24, 1, 2, 3, 4, 5};
    for (unsigned mode = 1; mode <= 4; ++mode) {
        reports = commands = 0;
        sent_size = 0;
        edge_sl651_callbacks callbacks = {send_bytes, report, command, NULL};
        edge_sl651_session *s = edge_sl651_create(mode, station, callbacks, NULL);
        assert(s);
        uint8_t input[64];
        size_t n = upstream(input, 0x32, 3, 1);
        edge_sl651_receive(s, input, 5, 1000, time);
        assert(reports == 0);
        edge_sl651_receive(s, input + 5, n - 5, 1000, time);
        assert(reports == 1 && sent_size == 0);
        edge_sl651_commit(s, token, 1000, time);
        assert((sent_size != 0) == (mode == 2));
        if (sent_size)
            assert(sent[sent_size - 3] == 4 && sent[15] == 1);
        uint8_t id[16] = {1};
        bool queried = edge_sl651_query(s, id, 0x37, NULL, 0, 2000, 3000, time);
        assert(queried == (mode != 1));
        if (queried) {
            size_t serial = mode == 3 ? 17 : 14;
            assert(sent[serial] == 0 && sent[serial + 1] == 0 && sent[sent_size - 3] == 5);
            n = upstream(input, 0x37, 0x17, 2);
            edge_sl651_receive(s, input, n, 2001, time);
            edge_sl651_commit(s, token, 2001, time);
            assert(!commands && sent[sent_size - 3] == 6);
            edge_sl651_tick(s, 5001, time);
            assert(sent[sent_size - 3] == 6);
            n = upstream(input, 0x37, 3, 3);
            edge_sl651_receive(s, input, n, 2002, time);
            edge_sl651_commit(s, token, 2002, time);
            assert(commands == 1 && sent[sent_size - 3] == 6);
            edge_sl651_command_committed(s, id, 2002, time);
            assert(sent[sent_size - 3] == 4);
        }
        edge_sl651_destroy(s);
    }
    uint8_t body[] = {0,    1,    0x24, 1,    2,    3,    4,    5,   0x50,
                      0x18, 0xAB, 0x12, 0xFF, 0xAB, 0x12, 0x12, 0x34};
    uint8_t guide[] = {0xAB, 0x12};
    const uint8_t *value;
    size_t size;
    assert(edge_sl651_field(body, sizeof(body), false, 0, guide, 2, 2, &value, &size));
    assert(value == body + 15 && size == 2);
    assert(edge_sl651_field(body, sizeof(body), true, 2, NULL, 0, 6, &value, &size));
    assert(value == body + 2 && size == 6);
    assert(!edge_sl651_field(body, sizeof(body), true, 16, NULL, 0, 2, &value, &size));
    double number;
    uint8_t negative[] = {0xFF, 0x12, 0x34};
    assert(edge_sl651_bcd(negative, 3, 2, &number));
    assert(number == -12.34);
    puts("SL651 mode and element tests passed");
    return 0;
}
