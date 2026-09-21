#include "edge_traffic.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* No packet contents, process-wide endpoint guesses, firewall rules or NAT changes. */
static void incomplete(edge_traffic *traffic) {
    for (size_t i = 0; i < traffic->platform_count; ++i)
        traffic->windows[i].complete = false;
}

static uint64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static const unsigned char *attribute(const void *input, size_t length,
                                      unsigned type, size_t *value_size) {
    const unsigned char *cursor = input;
    while (length >= sizeof(struct nlattr)) {
        struct nlattr header;
        memcpy(&header, cursor, sizeof(header));
        if (header.nla_len < sizeof(header) || header.nla_len > length) return NULL;
        if ((header.nla_type & NLA_TYPE_MASK) == type) {
            *value_size = header.nla_len - sizeof(header);
            return cursor + sizeof(header);
        }
        size_t step = NLA_ALIGN(header.nla_len);
        if (step > length) return NULL;
        cursor += step;
        length -= step;
    }
    return NULL;
}

static bool copy_attribute(const void *input, size_t size, unsigned type,
                            void *output, size_t expected) {
    size_t length = 0;
    const void *value = attribute(input, size, type, &length);
    if (value == NULL || length != expected) return false;
    memcpy(output, value, expected);
    return true;
}

static bool counter(const void *input, size_t size, unsigned type, uint64_t *result) {
    size_t length = 0;
    const void *nested = attribute(input, size, type, &length);
    unsigned char bytes[8];
    if (!nested || !copy_attribute(nested, length, CTA_COUNTERS_BYTES, bytes, sizeof(bytes)))
        return false;
    *result = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i) *result = (*result << 8U) | bytes[i];
    return true;
}

static bool tuple(const void *input, size_t size, edge_traffic_flow *entry) {
    size_t tuple_size = 0, ip_size = 0, proto_size = 0;
    const void *original = attribute(input, size, CTA_TUPLE_ORIG, &tuple_size);
    if (!original) return false;
    const void *ip = attribute(original, tuple_size, CTA_TUPLE_IP, &ip_size);
    const void *proto = attribute(original, tuple_size, CTA_TUPLE_PROTO, &proto_size);
    uint8_t protocol = 0;
    if (!ip || !proto || !copy_attribute(proto, proto_size, CTA_PROTO_NUM, &protocol, 1) ||
        protocol != IPPROTO_TCP) return false;
    unsigned source = entry->family == AF_INET ? CTA_IP_V4_SRC : CTA_IP_V6_SRC;
    unsigned destination = entry->family == AF_INET ? CTA_IP_V4_DST : CTA_IP_V6_DST;
    size_t address_size = entry->family == AF_INET ? 4U : 16U;
    return copy_attribute(ip, ip_size, source, entry->source, address_size) &&
        copy_attribute(ip, ip_size, destination, entry->destination, address_size) &&
        copy_attribute(proto, proto_size, CTA_PROTO_SRC_PORT, &entry->source_port, 2) &&
        copy_attribute(proto, proto_size, CTA_PROTO_DST_PORT, &entry->destination_port, 2) &&
        copy_attribute(input, size, CTA_ID, &entry->id, 4);
}

static bool same_tuple(const edge_traffic_flow *left, const edge_traffic_flow *right) {
    return left->family == right->family && left->source_port == right->source_port &&
        left->destination_port == right->destination_port &&
        memcmp(left->source, right->source, sizeof(left->source)) == 0 &&
        memcmp(left->destination, right->destination, sizeof(left->destination)) == 0;
}

static void accumulate(uint64_t *total, uint64_t *previous, uint64_t current, bool *complete) {
    /* A stale dump can arrive after a newer event; never subtract or double count. */
    if (current <= *previous) return;
    uint64_t delta = current - *previous;
    if (delta > UINT64_MAX - *total) {
        *complete = false;
        *total = UINT64_MAX;
    } else {
        *total += delta;
    }
    *previous = current;
}

bool edge_traffic_ingest(edge_traffic *traffic, const void *message, size_t size) {
    struct nlmsghdr header;
    if (!traffic || size < sizeof(header)) return false;
    memcpy(&header, message, sizeof(header));
    if (header.nlmsg_len > size || header.nlmsg_len < NLMSG_LENGTH(sizeof(struct nfgenmsg)))
        return false;
    if ((header.nlmsg_type >> 8U) != NFNL_SUBSYS_CTNETLINK) return true;
    unsigned operation = header.nlmsg_type & 0xffU;
    if (operation != IPCTNL_MSG_CT_NEW && operation != IPCTNL_MSG_CT_DELETE) return true;
    struct nfgenmsg generic;
    memcpy(&generic, (const unsigned char *)message + NLMSG_HDRLEN, sizeof(generic));
    if (generic.nfgen_family != AF_INET && generic.nfgen_family != AF_INET6) return true;
    const void *attrs = (const unsigned char *)message + NLMSG_LENGTH(sizeof(generic));
    size_t attrs_size = header.nlmsg_len - NLMSG_LENGTH(sizeof(generic));
    edge_traffic_flow entry = {0};
    entry.family = generic.nfgen_family;
    if (!tuple(attrs, attrs_size, &entry)) return true; /* Non-TCP/unrelated entries. */
    edge_traffic_flow *match = NULL;
    for (size_t i = 0; i < EDGE_TRAFFIC_MAX_FLOWS; ++i) {
        edge_traffic_flow *flow = &traffic->flows[i];
        if (!flow->used || !same_tuple(flow, &entry)) continue;
        if (flow->bound) {
            if (flow->id != entry.id || flow->destination_port != entry.destination_port ||
                memcmp(flow->destination, entry.destination, sizeof(entry.destination)) != 0) continue;
            match = flow;
            break;
        }
        match = flow;
    }
    if (!match) return true;
    edge_traffic_window *window = &traffic->windows[match->platform];
    match->bound = true;
    match->seen = true;
    match->id = entry.id;
    match->destination_port = entry.destination_port;
    memcpy(match->destination, entry.destination, sizeof(entry.destination));
    uint64_t upload = 0, download = 0;
    if (!counter(attrs, attrs_size, CTA_COUNTERS_ORIG, &upload) ||
        !counter(attrs, attrs_size, CTA_COUNTERS_REPLY, &download)) {
        window->complete = false;
    } else {
        accumulate(&window->upload, &match->upload, upload, &window->complete);
        accumulate(&window->download, &match->download, download, &window->complete);
    }
    if (operation == IPCTNL_MSG_CT_DELETE) memset(match, 0, sizeof(*match));
    return true;
}

/* A bounded nonblocking read. Sender pid and truncation are checked before parsing. */
static int receive_messages(edge_traffic *traffic, int fd, uint32_t sequence, bool *done) {
    unsigned char buffer[32768];
    struct sockaddr_nl sender = {0};
    struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer)};
    struct msghdr msg = {.msg_name = &sender, .msg_namelen = sizeof(sender),
                         .msg_iov = &iov, .msg_iovlen = 1};
    ssize_t received = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (received < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    if (received == 0 || (msg.msg_flags & MSG_TRUNC) || sender.nl_pid != 0) return -1;
    size_t remaining = (size_t)received;
    const unsigned char *cursor = buffer;
    while (remaining >= sizeof(struct nlmsghdr)) {
        struct nlmsghdr header;
        memcpy(&header, cursor, sizeof(header));
        if (header.nlmsg_len < sizeof(header) || header.nlmsg_len > remaining) return -1;
        if (!sequence || header.nlmsg_seq == sequence) {
            if (header.nlmsg_flags & NLM_F_DUMP_INTR) return -1;
            if (header.nlmsg_type == NLMSG_ERROR || header.nlmsg_type == NLMSG_OVERRUN) return -1;
            if (header.nlmsg_type == NLMSG_DONE) {
                int error = 0;
                if (header.nlmsg_len >= NLMSG_LENGTH(sizeof(error)))
                    memcpy(&error, cursor + NLMSG_HDRLEN, sizeof(error));
                if (error != 0) return -1;
                *done = true;
            } else if (!edge_traffic_ingest(traffic, cursor, header.nlmsg_len)) return -1;
        }
        size_t step = NLMSG_ALIGN(header.nlmsg_len);
        if (step > remaining) return -1;
        remaining -= step;
        cursor += step;
    }
    return remaining == 0 ? 1 : -1;
}

void edge_traffic_drain(edge_traffic *traffic) {
    if (traffic->event_fd < 0) return;
    bool done = false;
    for (size_t i = 0; i < 256; ++i) {
        int result = receive_messages(traffic, traffic->event_fd, 0, &done);
        if (result < 0) incomplete(traffic);
        if (result <= 0) return;
    }
    /* The event watcher will continue draining without a business polling timer. */
}

static int open_netlink(unsigned groups) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_NETFILTER);
    if (fd < 0) return -1;
    struct sockaddr_nl local = {.nl_family = AF_NETLINK, .nl_groups = groups};
    int capacity = 262144;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity));
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool edge_traffic_open(edge_traffic *traffic, size_t platforms, uint64_t now_ms) {
    memset(traffic, 0, sizeof(*traffic));
    traffic->event_fd = traffic->query_fd = -1;
    if (platforms > EDGE_TRAFFIC_MAX_PLATFORMS) return false;
    traffic->platform_count = platforms;
    for (size_t i = 0; i < platforms; ++i) traffic->windows[i].acknowledged_at_ms = now_ms;
    /* Enable before opening any platform sockets, so SYN/TLS/HTTP are included.
     * Only accounting is enabled; no filtering, NAT or existing sysctls are restored/overwritten on exit. */
    FILE *accounting = fopen("/proc/sys/net/netfilter/nf_conntrack_acct", "r+");
    if (!accounting) return false;
    int value = fgetc(accounting);
    bool enabled = value == '1';
    if (!enabled) {
        rewind(accounting);
        enabled = fputs("1\n", accounting) >= 0 && fflush(accounting) == 0;
    }
    if (fclose(accounting) != 0) enabled = false;
    if (!enabled) return false;
    traffic->event_fd = open_netlink(1U << (NFNLGRP_CONNTRACK_DESTROY - 1U));
    traffic->query_fd = open_netlink(0);
    if (traffic->event_fd < 0 || traffic->query_fd < 0) {
        edge_traffic_close(traffic);
        return false;
    }
    for (size_t i = 0; i < platforms; ++i) traffic->windows[i].complete = true;
    return true;
}

void edge_traffic_close(edge_traffic *traffic) {
    if (traffic->event_fd >= 0) close(traffic->event_fd);
    if (traffic->query_fd >= 0) close(traffic->query_fd);
    traffic->event_fd = traffic->query_fd = -1;
}

void edge_traffic_register(edge_traffic *traffic, size_t platform, int socket_fd) {
    if (platform >= traffic->platform_count || traffic->event_fd < 0) return;
    edge_traffic_drain(traffic);
    struct sockaddr_storage address = {0}, peer = {0};
    socklen_t size = sizeof(address), peer_size = sizeof(peer);
    edge_traffic_flow binding = {0};
    if (getsockname(socket_fd, (struct sockaddr *)&address, &size) != 0) goto failure;
    binding.family = (uint8_t)address.ss_family;
    binding.platform = platform;
    binding.used = true;
    if (address.ss_family == AF_INET) {
        const struct sockaddr_in *ip = (const struct sockaddr_in *)&address;
        memcpy(binding.source, &ip->sin_addr, 4);
        binding.source_port = ip->sin_port;
    } else if (address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *ip = (const struct sockaddr_in6 *)&address;
        memcpy(binding.source, &ip->sin6_addr, 16);
        binding.source_port = ip->sin6_port;
    } else goto failure;
    if (!binding.source_port) goto failure;
    if (getpeername(socket_fd, (struct sockaddr *)&peer, &peer_size) != 0) {
        /* getpeername rejects SYN_SENT. The original-destination socket option
         * supplies the exact already-tracked TCP peer before the handshake. */
        int level = address.ss_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
        int option = address.ss_family == AF_INET ? SO_ORIGINAL_DST : IP6T_SO_ORIGINAL_DST;
        peer_size = sizeof(peer);
        if (getsockopt(socket_fd, level, option, &peer, &peer_size) != 0) goto failure;
    }
    if (peer.ss_family != address.ss_family) goto failure;
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *ip = (const struct sockaddr_in *)&peer;
        memcpy(binding.destination, &ip->sin_addr, 4);
        binding.destination_port = ip->sin_port;
    } else {
        const struct sockaddr_in6 *ip = (const struct sockaddr_in6 *)&peer;
        memcpy(binding.destination, &ip->sin6_addr, 16);
        binding.destination_port = ip->sin6_port;
        const struct sockaddr_in6 *local = (const struct sockaddr_in6 *)&address;
        if (IN6_IS_ADDR_V4MAPPED(&local->sin6_addr) && IN6_IS_ADDR_V4MAPPED(&ip->sin6_addr)) {
            binding.family = AF_INET;
            memmove(binding.source, binding.source + 12, 4);
            memmove(binding.destination, binding.destination + 12, 4);
            memset(binding.source + 4, 0, 12);
            memset(binding.destination + 4, 0, 12);
        }
    }
    if (!binding.destination_port) goto failure;
    /* Do not steal an old platform's still-live tuple after an unlikely port reuse. */
    for (size_t i = 0; i < EDGE_TRAFFIC_MAX_FLOWS; ++i) {
        edge_traffic_flow *old = &traffic->flows[i];
        if (!old->used || !same_tuple(old, &binding)) continue;
        traffic->windows[old->platform].complete = false;
        /* An unresolved old tuple must not acquire the new platform's counters. */
        if (!old->bound) memset(old, 0, sizeof(*old));
        goto failure;
    }
    for (size_t i = 0; i < EDGE_TRAFFIC_MAX_FLOWS; ++i) {
        if (traffic->flows[i].used) continue;
        traffic->flows[i] = binding;
        return;
    }
failure:
    traffic->windows[platform].complete = false;
}

static bool refresh(edge_traffic *traffic) {
    if (traffic->query_fd < 0) return false;
    edge_traffic_drain(traffic);
    struct {
        struct nlmsghdr header;
        struct nfgenmsg generic;
    } request = {0};
    request.header.nlmsg_len = sizeof(request);
    request.header.nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8U) | IPCTNL_MSG_CT_GET;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    if (++traffic->query_sequence == 0) ++traffic->query_sequence;
    request.header.nlmsg_seq = traffic->query_sequence;
    request.generic.nfgen_family = AF_UNSPEC;
    request.generic.version = NFNETLINK_V0;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(traffic->query_fd, &request, sizeof(request), 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) != (ssize_t)sizeof(request)) return false;
    for (size_t i = 0; i < EDGE_TRAFFIC_MAX_FLOWS; ++i) traffic->flows[i].seen = false;
    uint64_t started = milliseconds();
    if (!started) return false;
    bool done = false;
    while (!done) {
        uint64_t now = milliseconds();
        if (!now || now - started >= 100U) return false;
        struct pollfd fd = {.fd = traffic->query_fd, .events = POLLIN};
        int ready = poll(&fd, 1, (int)(100U - (now - started)));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if (receive_messages(traffic, traffic->query_fd, traffic->query_sequence, &done) < 0)
            return false;
    }
    edge_traffic_drain(traffic);
    for (size_t i = 0; i < EDGE_TRAFFIC_MAX_FLOWS; ++i) {
        edge_traffic_flow *flow = &traffic->flows[i];
        if (!flow->used || flow->seen) continue;
        /* Missing final event or NOTRACK: do not silently publish an exact zero. */
        traffic->windows[flow->platform].complete = false;
        memset(flow, 0, sizeof(*flow));
    }
    return true;
}

void edge_traffic_sample(edge_traffic *traffic, size_t platform, uint64_t now_ms,
                         iot_edge_v1_TcpTraffic *report) {
    if (platform >= traffic->platform_count) {
        memset(report, 0, sizeof(*report));
        return;
    }
    if (!refresh(traffic)) {
        incomplete(traffic);
        /* Abort an interrupted multipart dump before the next sample. */
        if (traffic->query_fd >= 0) {
            close(traffic->query_fd);
            traffic->query_fd = open_netlink(0);
        }
    }
    edge_traffic_window *window = &traffic->windows[platform];
    if (!window->pending) {
        window->sampled_upload = window->upload;
        window->sampled_download = window->download;
        window->sampled_at_ms = now_ms;
        window->report.upload_bytes = window->upload - window->acknowledged_upload;
        window->report.download_bytes = window->download - window->acknowledged_download;
        window->report.interval_ms = now_ms >= window->acknowledged_at_ms
            ? now_ms - window->acknowledged_at_ms : 0;
        window->report.sample_id = ++window->sequence;
        window->pending = true;
    }
    *report = window->report;
}

void edge_traffic_ack(edge_traffic *traffic, size_t platform, uint64_t sample_id) {
    if (platform >= traffic->platform_count) return;
    edge_traffic_window *window = &traffic->windows[platform];
    if (!window->pending || !sample_id || sample_id != window->report.sample_id) return;
    window->acknowledged_upload = window->sampled_upload;
    window->acknowledged_download = window->sampled_download;
    window->acknowledged_at_ms = window->sampled_at_ms;
    window->pending = false;
}
