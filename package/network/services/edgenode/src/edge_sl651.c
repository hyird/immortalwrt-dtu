#include "edge_sl651.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8U | p[1]); }
uint16_t edge_sl651_crc(const uint8_t *p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (uint16_t)((crc >> 1U) ^ ((crc & 1U) ? 0xA001U : 0));
    }
    return crc;
}
static bool decimal_byte(uint8_t p) { return (p >> 4U) <= 9 && (p & 15U) <= 9; }
static unsigned decimal(uint8_t p) { return (p >> 4U) * 10U + (p & 15U); }
static bool report_body(const uint8_t *p, size_t n) {
    if (n < 8 || be16(p) == 0)
        return false;
    for (size_t i = 2; i < 8; ++i)
        if (!decimal_byte(p[i]))
            return false;
    unsigned month = decimal(p[3]), day = decimal(p[4]);
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 && day >= 1 &&
           day <= days[month - 1] + (month == 2 && decimal(p[2]) % 4 == 0) && decimal(p[5]) < 24 &&
           decimal(p[6]) < 60 && decimal(p[7]) < 60;
}
bool edge_sl651_parse(const uint8_t *p, size_t n, edge_sl651_frame *f) {
    if (!p || !f || n < 17 || p[0] != 0x7E || p[1] != 0x7E || (p[11] & 0xF0U) != 0 || !p[2] ||
        (p[13] != 2 && p[13] != 0x16) || (p[n - 3] != 3 && p[n - 3] != 0x17) ||
        (be16(p + 11) & 0xFFFU) + 17U != n || edge_sl651_crc(p, n - 2) != be16(p + n - 2))
        return false;
    for (size_t i = 3; i < 8; ++i)
        if (!decimal_byte(p[i]))
            return false;
    memset(f, 0, sizeof(*f));
    memcpy(f->station, p + 3, 5);
    f->center = p[2];
    memcpy(f->password, p + 8, 2);
    f->function = p[10];
    f->ending = p[n - 3];
    f->body = p + 14;
    f->body_size = n - 17;
    if (p[13] == 0x16) {
        if (f->body_size < 3)
            return false;
        uint32_t packed = (uint32_t)p[14] << 16U | (uint32_t)p[15] << 8U | p[16];
        f->total = (uint16_t)(packed >> 12U);
        f->sequence = (uint16_t)(packed & 0xFFFU);
        if (!f->total || !f->sequence || f->sequence > f->total)
            return false;
        f->body += 3;
        f->body_size -= 3;
    } else if (!report_body(f->body, f->body_size))
        return false;
    return true;
}
static size_t downlink(const edge_sl651_frame *f, uint8_t ending, uint16_t seq, const uint8_t *body,
                       size_t size, uint8_t *out, size_t cap) {
    size_t extra = f->total ? 3 : 0;
    if (size + extra > 4095 || cap < size + extra + 17)
        return 0;
    out[0] = out[1] = 0x7E;
    memcpy(out + 2, f->station, 5);
    out[7] = f->center;
    memcpy(out + 8, f->password, 2);
    out[10] = f->function;
    out[11] = (uint8_t)(0x80U | ((size + extra) >> 8U));
    out[12] = (uint8_t)(size + extra);
    out[13] = extra ? 0x16 : 2;
    if (extra) {
        uint32_t packed = (uint32_t)f->total << 12U | seq;
        out[14] = (uint8_t)(packed >> 16U);
        out[15] = (uint8_t)(packed >> 8U);
        out[16] = (uint8_t)packed;
    }
    memcpy(out + 14 + extra, body, size);
    size_t n = 14 + extra + size;
    out[n++] = ending;
    uint16_t crc = edge_sl651_crc(out, n);
    out[n++] = (uint8_t)(crc >> 8U);
    out[n++] = (uint8_t)crc;
    return n;
}
size_t edge_sl651_confirm(const edge_sl651_frame *f, uint8_t ending, uint16_t seq,
                          const uint8_t time[6], uint8_t *out, size_t cap) {
    uint8_t body[8] = {0};
    if (f->body_size >= 2)
        memcpy(body, f->body, 2);
    memcpy(body + 2, time, 6);
    return downlink(f, ending, seq ? seq : f->total, body, 8, out, cap);
}
bool edge_sl651_field(const uint8_t *body, size_t n, bool fixed, size_t offset,
                      const uint8_t *guide, size_t gn, size_t length, const uint8_t **value,
                      size_t *vn) {
    if (!body || !value || !vn)
        return false;
    if (fixed) {
        if (!length || offset > n || length > n - offset)
            return false;
        *value = body + offset;
        *vn = length;
        return true;
    }
    if (!guide || (gn != 2 && gn != 3))
        return false;
    for (size_t pos = offset > 8 ? offset : 8; pos < n;) {
        size_t g = body[pos] == 0xFF ? 3 : 2;
        if (g > n - pos)
            return false;
        size_t size = body[pos + g - 1] >> 3U;
        if (body[pos] >= 0xF0 && body[pos] != 0xFF) {
            if (body[pos + 1] != body[pos])
                return false;
            switch (body[pos]) {
            case 0xF0:
                size = 5;
                break;
            case 0xF1:
                size = 6;
                break;
            case 0xF2:
            case 0xF3:
                size = n - pos - g;
                break;
            default:
                if (g != gn || memcmp(body + pos, guide, g))
                    return false;
                size = length;
                break;
            }
        }
        if (!size || size > n - pos - g)
            return false;
        if (g == gn && !memcmp(body + pos, guide, g)) {
            if (length && length != size)
                return false;
            *value = body + pos + g;
            *vn = size;
            return true;
        }
        pos += g + size;
    }
    return false;
}
bool edge_sl651_bcd(const uint8_t *p, size_t n, unsigned digits, double *value) {
    if (!p || !n || !value || digits > 7)
        return false;
    bool negative = *p == 0xFF;
    if (negative) {
        ++p;
        --n;
    }
    if (!n)
        return false;
    double number = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!decimal_byte(p[i]))
            return false;
        number = number * 100 + decimal(p[i]);
    }
    number /= pow(10, digits);
    if (negative)
        number = -number;
    if (!isfinite(number))
        return false;
    *value = number;
    return true;
}

typedef struct {
    uint8_t *bytes;
    size_t size;
    unsigned retries;
} sl_packet;
struct edge_sl651_session {
    unsigned mode;
    uint8_t station[5];
    bool bound, quarantined;
    edge_sl651_callbacks callbacks;
    void *context;
    edge_sl651_frame header, report;
    uint8_t receive[EDGE_SL651_FRAME_MAX * 2];
    size_t receive_size;
    sl_packet *packets;
    uint16_t total;
    size_t assembled_size;
    uint64_t packet_deadline;
    uint8_t *report_body;
    bool awaiting_commit, report_submitted;
    uint64_t token, next_token, publication_deadline;
    uint64_t now;
    uint8_t time[6];
    bool querying, report_matches, report_confirm, command_requested, command_persisted;
    uint8_t command_id[16], query_function;
    uint8_t query[EDGE_SL651_FRAME_MAX];
    size_t query_size;
    uint64_t query_deadline;
    uint32_t timeout;
    unsigned retries;
    bool seen_confirm[64];
    uint8_t seen[64][9];
    size_t seen_count, seen_next;
};
static void packets_free(edge_sl651_session *s) {
    if (s->packets)
        for (size_t i = 0; i < s->total; ++i)
            free(s->packets[i].bytes);
    free(s->packets);
    s->packets = NULL;
    s->total = 0;
    s->assembled_size = 0;
    s->packet_deadline = 0;
}
static void query_finish(edge_sl651_session *s, bool success, const char *reason) {
    if (!s->querying)
        return;
    s->querying = false;
    if (!success)
        s->quarantined = true;
    if (s->callbacks.command)
        s->callbacks.command(s->context, s->command_id, success, reason);
}
edge_sl651_session *edge_sl651_create(unsigned mode, const uint8_t station[5],
                                      edge_sl651_callbacks cb, void *context) {
    if (mode > 4 || !station || !cb.send || !cb.report)
        return NULL;
    edge_sl651_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->mode = mode ? mode : 1;
    memcpy(s->station, station, 5);
    s->callbacks = cb;
    s->context = context;
    s->next_token = 1;
    return s;
}
void edge_sl651_reset(edge_sl651_session *s) {
    if (!s)
        return;
    query_finish(s, false, "SL651 disconnected");
    packets_free(s);
    free(s->report_body);
    s->report_body = NULL;
    s->bound = s->quarantined = s->awaiting_commit = s->report_submitted = false;
    s->receive_size = 0;
    s->seen_count = s->seen_next = 0;
}
void edge_sl651_destroy(edge_sl651_session *s) {
    if (s) {
        edge_sl651_reset(s);
        free(s);
    }
}
static bool confirm(edge_sl651_session *s, const edge_sl651_frame *f, uint8_t ending,
                    uint16_t seq) {
    uint8_t out[32];
    size_t n = edge_sl651_confirm(f, ending, seq, s->time, out, sizeof(out));
    return n && s->callbacks.send(s->context, out, n);
}
static bool missing(edge_sl651_session *s) {
    for (uint16_t i = 0; i < s->total; ++i)
        if (!s->packets[i].bytes) {
            if (s->packets[i].retries >= 2) {
                query_finish(s, false, "SL651 packet timeout");
                packets_free(s);
                return false;
            }
            if (confirm(s, &s->header, 0x15, (uint16_t)(i + 1)))
                ++s->packets[i].retries;
            s->packet_deadline = s->now + 30000;
            return true;
        }
    return false;
}
static void submit(edge_sl651_session *s, const edge_sl651_frame *f, uint8_t *body, size_t n) {
    if (!report_body(body, n)) {
        free(body);
        return;
    }
    uint8_t identity[9];
    identity[0] = f->function;
    memcpy(identity + 1, body, 8);
    for (size_t i = 0; i < s->seen_count; ++i) {
        if (memcmp(s->seen[i], identity, 9)) continue;
        edge_sl651_frame previous = *f;
        previous.body = body;
        previous.body_size = n;
        if ((s->mode == 2 || f->total || s->seen_confirm[i]) && f->function != 0x2F)
            (void)confirm(s, &previous, f->total || f->ending == 3 ? 4 : 6, 0);
        free(body);
        return;
    }
    s->report = *f;
    s->report.body = body;
    s->report.body_size = n;
    s->report_body = body;
    s->awaiting_commit = true;
    s->token = s->next_token++;
    s->publication_deadline = s->now + 3000;
    s->report_matches = s->querying && s->query_function == f->function;
    s->report_confirm = false;
    s->report_submitted = s->callbacks.report(s->context, s->token, &s->report);
}
static void consume_frame(edge_sl651_session *s, const edge_sl651_frame *f) {
    if (memcmp(f->station, s->station, 5) || (s->mode == 1 && (f->total || f->ending != 3)))
        return;
    if (s->bound && (s->header.center != f->center || memcmp(s->header.password, f->password, 2)))
        return;
    if (!s->bound) {
        s->header = *f;
        s->header.body = NULL;
        s->header.body_size = 0;
        s->bound = true;
    }
    if (!f->total) {
        uint8_t *body = malloc(f->body_size);
        if (!body)
            return;
        memcpy(body, f->body, f->body_size);
        submit(s, f, body, f->body_size);
        return;
    }
    if (s->packets && (s->header.function != f->function || s->total != f->total ||
                       (f->sequence == 1 && s->packets[0].bytes &&
                        (s->packets[0].size != f->body_size ||
                         memcmp(s->packets[0].bytes, f->body, f->body_size)))))
        packets_free(s);
    if (!s->packets) {
        s->packets = calloc(f->total, sizeof(*s->packets));
        if (!s->packets)
            return;
        s->total = f->total;
        s->header = *f;
        s->header.body = NULL;
        s->header.body_size = 0;
    }
    sl_packet *packet = &s->packets[f->sequence - 1];
    if (packet->bytes) {
        if (packet->size != f->body_size || memcmp(packet->bytes, f->body, f->body_size))
            return;
    } else {
        if (!f->body_size || f->body_size > EDGE_SL651_BODY_MAX - s->assembled_size) {
            query_finish(s, false, "SL651 body limit");
            packets_free(s);
            return;
        }
        packet->bytes = malloc(f->body_size);
        if (!packet->bytes)
            return;
        memcpy(packet->bytes, f->body, f->body_size);
        packet->size = f->body_size;
        s->assembled_size += f->body_size;
    }
    if (s->packets[0].bytes) {
        s->header.body = s->packets[0].bytes;
        s->header.body_size = s->packets[0].size;
    }
    bool complete = true;
    for (size_t i = 0; i < s->total; ++i)
        if (!s->packets[i].bytes) {
            complete = false;
            break;
        }
    if (!complete) {
        if (f->ending == 3)
            (void)missing(s);
        else
            s->packet_deadline = s->now + 30000;
        return;
    }
    uint8_t *body = malloc(s->assembled_size);
    if (!body)
        return;
    size_t offset = 0;
    for (size_t i = 0; i < s->total; ++i) {
        memcpy(body + offset, s->packets[i].bytes, s->packets[i].size);
        offset += s->packets[i].size;
    }
    edge_sl651_frame report = s->header;
    report.ending = 3;
    s->packet_deadline = 0;
    submit(s, &report, body, offset);
}
static void drain(edge_sl651_session *s) {
    while (!s->awaiting_commit && s->receive_size >= 17) {
        if (s->receive[0] != 0x7E || s->receive[1] != 0x7E) {
            memmove(s->receive, s->receive + 1, --s->receive_size);
            continue;
        }
        size_t n = (be16(s->receive + 11) & 0xFFFU) + 17;
        if (n > s->receive_size)
            break;
        edge_sl651_frame frame;
        if (edge_sl651_parse(s->receive, n, &frame))
            consume_frame(s, &frame);
        s->receive_size -= n;
        memmove(s->receive, s->receive + n, s->receive_size);
    }
}
void edge_sl651_receive(edge_sl651_session *s, const uint8_t *p, size_t n, uint64_t now,
                        const uint8_t time[6]) {
    if (!s || !p || !time)
        return;
    s->now = now;
    memcpy(s->time, time, 6);
    if (n > sizeof(s->receive) - s->receive_size) {
        edge_sl651_reset(s);
        return;
    }
    memcpy(s->receive + s->receive_size, p, n);
    s->receive_size += n;
    drain(s);
}
void edge_sl651_commit(edge_sl651_session *s, uint64_t token, uint64_t now, const uint8_t time[6]) {
    if (!s || !s->awaiting_commit || s->token != token)
        return;
    s->now = now;
    memcpy(s->time, time, 6);
    bool final = s->report.total || s->report.ending == 3;
    if (s->report_matches && final && !s->command_persisted) {
        if (!s->command_requested && s->callbacks.command) {
            s->command_requested = true;
            s->callbacks.command(s->context, s->command_id, true, "SL651 response accepted");
        }
        return;
    }
    if (s->report_matches && final)
        s->querying = false;
    if ((s->mode == 2 || s->report.total || s->report_matches || s->report_confirm) &&
        s->report.function != 0x2F)
        (void)confirm(s, &s->report, final ? 4 : 6, 0);
    if (s->report_matches && !final) {
        /* A lost next response is recovered by repeating this ACK, not ENQ. */
        s->query_size = edge_sl651_confirm(&s->report, 6, 0, s->time, s->query, sizeof(s->query));
        s->retries = 0;
        s->query_deadline = now + s->timeout;
    }
    s->seen_confirm[s->seen_next] = s->report_matches || s->report_confirm;
    uint8_t *seen = s->seen[s->seen_next];
    seen[0] = s->report.function;
    memcpy(seen + 1, s->report.body, 8);
    s->seen_next = (s->seen_next + 1) % 64;
    if (s->seen_count < 64)
        ++s->seen_count;
    free(s->report_body);
    s->report_body = NULL;
    s->awaiting_commit = s->report_submitted = false;
    drain(s);
}
void edge_sl651_tick(edge_sl651_session *s, uint64_t now, const uint8_t time[6]) {
    if (!s)
        return;
    s->now = now;
    memcpy(s->time, time, 6);
    if (s->awaiting_commit && now >= s->publication_deadline) {
        s->report_submitted = false;
        s->publication_deadline = now + 3000;
    }
    if (s->awaiting_commit && !s->report_submitted)
        s->report_submitted = s->callbacks.report(s->context, s->token, &s->report);
    if (s->packet_deadline && now >= s->packet_deadline)
        (void)missing(s);
    if (s->querying && !s->awaiting_commit && now >= s->query_deadline) {
        if (s->packet_deadline)
            s->query_deadline = now + 30000;
        else if (s->retries >= 2)
            query_finish(s, false, "SL651 query timeout");
        else {
            (void)s->callbacks.send(s->context, s->query, s->query_size);
            ++s->retries;
            s->query_deadline = now + s->timeout;
        }
    }
}
bool edge_sl651_query(edge_sl651_session *s, const uint8_t id[16], uint8_t function,
                      const uint8_t *body, size_t size, uint64_t now, uint32_t timeout,
                      const uint8_t time[6]) {
    if (!s || !id || !time || !s->bound || s->mode == 1 || s->querying || s->awaiting_commit ||
        s->quarantined || size > (s->mode == 3 ? 4084U : 4087U) || (!body && size))
        return false;
    uint8_t content[4095] = {0};
    memcpy(content + 2, time, 6);
    if (size)
        memcpy(content + 8, body, size);
    edge_sl651_frame header = s->header;
    header.total = s->mode == 3 ? 1 : 0;
    header.function = function;
    s->query_size =
        downlink(&header, 5, header.total, content, size + 8, s->query, sizeof(s->query));
    if (!s->query_size || !s->callbacks.send(s->context, s->query, s->query_size))
        return false;
    s->command_requested = s->command_persisted = false;
    s->querying = true;
    s->query_function = function;
    memcpy(s->command_id, id, 16);
    s->timeout = timeout < 100 ? 3000 : timeout > 60000 ? 60000 : timeout;
    s->query_deadline = now + s->timeout;
    s->retries = 0;
    return true;
}

bool edge_sl651_ready(const edge_sl651_session *s) { return s && !s->awaiting_commit; }
void edge_sl651_command_committed(edge_sl651_session *s, const uint8_t id[16], uint64_t now,
                                  const uint8_t time[6]) {
    if (!s || !s->querying || !s->command_requested || memcmp(s->command_id, id, 16))
        return;
    s->command_persisted = true;
    edge_sl651_commit(s, s->token, now, time);
}

void edge_sl651_quarantine(edge_sl651_session *s) {
    if (s)
        s->quarantined = true;
}
