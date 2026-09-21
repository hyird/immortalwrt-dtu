#include "edge_traffic.h"
#include <arpa/inet.h>
#include <assert.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>

static unsigned char wire[512];
static size_t length;
static size_t begin(unsigned type) {
    size_t start = length;
    struct nlattr header = {.nla_len = sizeof(header), .nla_type = (uint16_t)type};
    memcpy(wire + length, &header, sizeof(header));
    length += sizeof(header);
    return start;
}
static void end(size_t start) {
    uint16_t size = (uint16_t)(length - start);
    memcpy(wire + start, &size, sizeof(size));
    length = NLA_ALIGN(length);
}
static void attr(unsigned type, const void *value, size_t size) {
    size_t start = begin(type);
    memcpy(wire + length, value, size);
    length += size;
    end(start);
}
static void bytes(unsigned type, uint64_t value) {
    unsigned char encoded[8];
    for (int i = 7; i >= 0; --i) { encoded[i] = (unsigned char)value; value >>= 8U; }
    size_t start = begin(type | NLA_F_NESTED);
    attr(CTA_COUNTERS_BYTES, encoded, sizeof(encoded));
    end(start);
}
static void message(const edge_traffic_flow *flow, uint64_t up, uint64_t down, bool destroyed) {
    memset(wire, 0, sizeof(wire));
    length = NLMSG_LENGTH(sizeof(struct nfgenmsg));
    struct nfgenmsg generic = {.nfgen_family = flow->family};
    memcpy(wire + NLMSG_HDRLEN, &generic, sizeof(generic));
    size_t tuple = begin(CTA_TUPLE_ORIG | NLA_F_NESTED);
    size_t ip = begin(CTA_TUPLE_IP | NLA_F_NESTED);
    bool v4 = flow->family == AF_INET;
    attr(v4 ? CTA_IP_V4_SRC : CTA_IP_V6_SRC, flow->source, v4 ? 4 : 16);
    attr(v4 ? CTA_IP_V4_DST : CTA_IP_V6_DST, flow->destination, v4 ? 4 : 16);
    end(ip);
    size_t proto = begin(CTA_TUPLE_PROTO | NLA_F_NESTED);
    uint8_t tcp = IPPROTO_TCP;
    attr(CTA_PROTO_NUM, &tcp, 1);
    attr(CTA_PROTO_SRC_PORT, &flow->source_port, 2);
    attr(CTA_PROTO_DST_PORT, &flow->destination_port, 2);
    end(proto);
    end(tuple);
    attr(CTA_ID, &flow->id, 4);
    bytes(CTA_COUNTERS_ORIG, up);
    bytes(CTA_COUNTERS_REPLY, down);
    struct nlmsghdr header = {.nlmsg_len = (uint32_t)length,
        .nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8U) |
            (destroyed ? IPCTNL_MSG_CT_DELETE : IPCTNL_MSG_CT_NEW)};
    memcpy(wire, &header, sizeof(header));
}
static void ingest(edge_traffic *traffic, const edge_traffic_flow *flow,
                   uint64_t up, uint64_t down, bool destroyed) {
    message(flow, up, down, destroyed);
    assert(edge_traffic_ingest(traffic, wire, length));
}
int main(void) {
    edge_traffic traffic = {.event_fd = -1, .query_fd = -1, .platform_count = 2};
    traffic.windows[0].complete = traffic.windows[1].complete = true;
    edge_traffic_flow first = {.used = true, .family = AF_INET, .platform = 0,
        .source = {192, 0, 2, 1}, .destination = {198, 51, 100, 1},
        .source_port = htons(45000), .destination_port = htons(443), .id = 11};
    edge_traffic_flow second = first;
    second.platform = 1;
    second.destination[3] = 2; /* Same local port must NOT merge platforms. */
    second.id = 12;
    traffic.flows[0] = first;
    traffic.flows[1] = second;
    ingest(&traffic, &first, 60, 60, false); /* SYN/SYN-ACK, no application payload. */
    ingest(&traffic, &second, 111, 222, false);
    ingest(&traffic, &first, 100, 100, false); /* Pure ACK bytes. */
    ingest(&traffic, &first, 160, 100, false); /* A retransmission counts again. */
    ingest(&traffic, &first, 100, 60, false); /* Stale dump never subtracts. */
    assert(traffic.windows[0].upload == 160 && traffic.windows[0].download == 100);
    assert(traffic.windows[1].upload == 111 && traffic.windows[1].download == 222);
    edge_traffic_flow unrelated = first;
    unrelated.destination_port = htons(80);
    ingest(&traffic, &unrelated, 9999, 9999, false);
    assert(traffic.windows[0].upload == 160);
    assert(traffic.windows[0].complete);

    iot_edge_v1_TcpTraffic report;
    edge_traffic_sample(&traffic, 0, 300000, &report);
    assert(report.upload_bytes == 160 && report.download_bytes == 100);
    assert(report.interval_ms == 300000 && report.sample_id == 1);
    ingest(&traffic, &first, 200, 140, false);
    edge_traffic_ack(&traffic, 1, 1); /* Other platform cannot clear this window. */
    edge_traffic_ack(&traffic, 0, 2); /* Mismatched/delayed acknowledgment. */
    edge_traffic_sample(&traffic, 0, 600000, &report);
    assert(report.upload_bytes == 160 && report.sample_id == 1);
    edge_traffic_ack(&traffic, 0, 1);
    edge_traffic_sample(&traffic, 0, 900000, &report);
    assert(report.upload_bytes == 40 && report.download_bytes == 40);
    assert(report.interval_ms == 600000 && report.sample_id == 2);
    edge_traffic_ack(&traffic, 0, 1);
    assert(traffic.windows[0].pending); /* Duplicate old ACK cannot clear new sample. */
    edge_traffic_ack(&traffic, 0, 2);

    /* Reconnect keeps the old flow until its final destroy event, including FIN/ACK. */
    edge_traffic_flow reconnect = first;
    reconnect.source_port = htons(45001);
    reconnect.id = 13;
    traffic.flows[2] = reconnect;
    ingest(&traffic, &reconnect, 60, 60, false);
    ingest(&traffic, &first, 280, 220, true);
    assert(!traffic.flows[0].used && traffic.flows[2].used);
    edge_traffic_sample(&traffic, 0, 1200000, &report);
    assert(report.upload_bytes == 140 && report.download_bytes == 140);
    assert(traffic.windows[1].upload == 111);

    edge_traffic_flow ipv6 = {.used = true, .family = AF_INET6, .platform = 1,
        .source_port = htons(45000), .destination_port = htons(443), .id = 14};
    assert(inet_pton(AF_INET6, "2001:db8::1", ipv6.source) == 1);
    assert(inet_pton(AF_INET6, "2001:db8::2", ipv6.destination) == 1);
    traffic.flows[3] = ipv6;
    ingest(&traffic, &ipv6, UINT64_C(9007199254740993), 80, false);
    assert(traffic.windows[1].upload == UINT64_C(9007199254741104));
    assert(traffic.windows[1].download == 302);
    message(&ipv6, 1, 2, false);
    assert(!edge_traffic_ingest(&traffic, wire, length - 1));
    assert(!edge_traffic_ingest(&traffic, wire, 2));
    traffic.windows[1].upload = UINT64_MAX - 1;
    ingest(&traffic, &ipv6, UINT64_MAX, 80, false);
    assert(traffic.windows[1].upload == UINT64_MAX && !traffic.windows[1].complete);
    puts("TCP traffic isolation, interval ACK, reconnect, IPv6 and uint64 tests passed");
    return 0;
}
