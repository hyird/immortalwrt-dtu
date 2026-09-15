#ifndef EDGE_SL651_H
#define EDGE_SL651_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EDGE_SL651_FRAME_MAX 4112U
#define EDGE_SL651_BODY_MAX 65536U
#define EDGE_SL651_PACKET_MAX 4095U

typedef struct {
    uint8_t station[5], center, password[2], function, ending;
    uint16_t total, sequence;
    const uint8_t *body;
    size_t body_size;
    const uint8_t *raw;
    size_t raw_size;
    uint8_t packet_id[16];
    uint8_t acquisition_id[16];
} edge_sl651_frame;

typedef struct edge_sl651_session edge_sl651_session;
typedef struct {
    bool (*send)(void *context, const uint8_t *bytes, size_t size,
                 const uint8_t acquisition_id[16], const uint8_t *reply_to_packet_id);
    /* A report token is committed only after all its records reach the node outbox. */
    bool (*report)(void *context, uint64_t token, const edge_sl651_frame *frame);
    void (*command)(void *context, const uint8_t id[16], bool success, const char *reason);
    void (*trace)(void *context, const uint8_t *bytes, size_t size, uint8_t packet_id[16], uint8_t acquisition_id[16]);
} edge_sl651_callbacks;

uint16_t edge_sl651_crc(const uint8_t *bytes, size_t size);
bool edge_sl651_parse(const uint8_t *bytes, size_t size, edge_sl651_frame *frame);
size_t edge_sl651_confirm(const edge_sl651_frame *frame, uint8_t ending, uint16_t sequence,
                          const uint8_t time[6], uint8_t *output, size_t capacity);
bool edge_sl651_field(const uint8_t *body, size_t size, bool fixed, size_t offset,
                      const uint8_t *guide, size_t guide_size, size_t length, const uint8_t **value,
                      size_t *value_size);
bool edge_sl651_bcd(const uint8_t *bytes, size_t size, unsigned digits, double *value);
edge_sl651_session *edge_sl651_create(unsigned mode, const uint8_t station[5],
                                      edge_sl651_callbacks callbacks, void *context);
void edge_sl651_destroy(edge_sl651_session *session);
void edge_sl651_reset(edge_sl651_session *session);
void edge_sl651_receive(edge_sl651_session *session, const uint8_t *bytes, size_t size,
                        uint64_t now, const uint8_t time[6]);
void edge_sl651_tick(edge_sl651_session *session, uint64_t now, const uint8_t time[6]);
void edge_sl651_commit(edge_sl651_session *session, uint64_t token, uint64_t now,
                       const uint8_t time[6]);
void edge_sl651_quarantine(edge_sl651_session *session);
bool edge_sl651_ready(const edge_sl651_session *session);
size_t edge_sl651_report_frame_count(const edge_sl651_session *session);
bool edge_sl651_report_frame(const edge_sl651_session *session, size_t index,
                              const uint8_t **bytes, size_t *size);
const uint8_t *edge_sl651_report_packet_id(const edge_sl651_session *session, size_t index);
void edge_sl651_command_committed(edge_sl651_session *session, const uint8_t id[16], uint64_t now,
                                  const uint8_t time[6]);
bool edge_sl651_query(edge_sl651_session *session, const uint8_t id[16], uint8_t function,
                      const uint8_t *body, size_t size, uint64_t now, uint32_t timeout,
                      const uint8_t time[6]);
#endif
