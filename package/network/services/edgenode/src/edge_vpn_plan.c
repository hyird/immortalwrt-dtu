#include "edge_vpn_plan.h"
#include "edge_config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t network; unsigned prefix; } cidr;

static bool parse(const char *text, cidr *out) {
    unsigned a, b, c, d, prefix;
    char extra;
    if (!text || sscanf(text, "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &prefix, &extra) != 5 ||
        a > 255U || b > 255U || c > 255U || d > 255U || prefix > 32U) return false;
    const uint32_t address = (a << 24U) | (b << 16U) | (c << 8U) | d;
    const uint32_t mask = prefix == 0U ? 0U : UINT32_MAX << (32U - prefix);
    out->network = address & mask; out->prefix = prefix;
    return (address & ~mask) == 0U;
}

static bool private_cidr(const cidr *value) {
    return (value->prefix >= 8U && (value->network & 0xff000000U) == 0x0a000000U) ||
        (value->prefix >= 12U && (value->network & 0xfff00000U) == 0xac100000U) ||
        (value->prefix >= 16U && (value->network & 0xffff0000U) == 0xc0a80000U);
}

static bool append(char *out, size_t size, size_t *used, const char *format, ...) {
    va_list arguments; va_start(arguments, format);
    int count = vsnprintf(out + *used, size - *used, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= size - *used) return false;
    *used += (size_t)count; return true;
}

bool edge_vpn_plan_init(edge_vpn_plan *plan, const uint8_t platform_id[16], size_t slot) {
    if (!plan || !platform_id || slot >= EDGE_MAX_PLATFORMS) return false;
    char id[33]; unsigned nonzero = 0U;
    for (size_t i = 0; i < 16U; ++i) {
        snprintf(id + i * 2U, 3U, "%02x", platform_id[i]); nonzero |= platform_id[i];
    }
    if (!nonzero) return false;
    memset(plan, 0, sizeof(*plan));
    snprintf(plan->namespace_name, sizeof(plan->namespace_name), "envpn_%s", id);
    snprintf(plan->host_link, sizeof(plan->host_link), "envpn%uh", (unsigned)slot);
    snprintf(plan->transport_link, sizeof(plan->transport_link), "envpn%uw", (unsigned)slot);
    snprintf(plan->host_ip, sizeof(plan->host_ip), "169.254.240.%u", (unsigned)slot * 4U + 1U);
    snprintf(plan->peer_host, sizeof(plan->peer_host), "169.254.240.%u", (unsigned)slot * 4U + 2U);
    snprintf(plan->host_address, sizeof(plan->host_address), "%s/30", plan->host_ip);
    snprintf(plan->peer_address, sizeof(plan->peer_address), "%s/30", plan->peer_host);
    snprintf(plan->key_path, sizeof(plan->key_path), "/etc/edgenode/vpn-%s.key", id);
    snprintf(plan->rules_path, sizeof(plan->rules_path), "/tmp/edgenode/vpn-%s.nft", id);
    snprintf(plan->root_rules_path, sizeof(plan->root_rules_path), "/tmp/edgenode/vpn-root-%s.nft", id);
    snprintf(plan->include_section, sizeof(plan->include_section), "edgenode_vpn_%s", id);
    return true;
}

bool edge_vpn_plan_rules(const edge_vpn_plan *plan, const iot_edge_v1_VpnConfigRequest *request,
                         char *rules, size_t capacity, char *error, size_t error_size) {
    if (!plan || !request || !rules || !capacity) return false;
    rules[0] = '\0'; size_t used = 0U;
    cidr edge;
    if (!parse(request->edge_address, &edge) || edge.prefix != 32U ||
        (edge.network & 0xffe00000U) != 0x64600000U || request->routes_count > 16U) goto invalid;
    for (pb_size_t i = 0; i < request->routes_count; ++i) {
        const iot_edge_v1_VpnRoute *route = &request->routes[i]; cidr virtual_net, real_net;
        if (!route->enabled) continue;
        if (!parse(route->virtual_cidr, &virtual_net) || !parse(route->target_cidr, &real_net) ||
            !private_cidr(&real_net) || !private_cidr(&virtual_net) ||
            virtual_net.prefix != real_net.prefix || virtual_net.prefix == 0U ||
            !((strcmp(route->mode, "nat") == 0 && strcmp(route->nat_mode, "masquerade") == 0) ||
              (strcmp(route->mode, "routed") == 0 && strcmp(route->nat_mode, "none") == 0)) ||
            (virtual_net.network & 0xfff00000U) != 0xac100000U || virtual_net.prefix < 12U) goto invalid;
        for (pb_size_t previous = 0; previous < i; ++previous) {
            cidr other;
            if (!request->routes[previous].enabled) continue;
            if (!parse(request->routes[previous].virtual_cidr, &other)) goto invalid;
            const unsigned common = other.prefix < virtual_net.prefix ? other.prefix : virtual_net.prefix;
            const uint32_t mask = UINT32_MAX << (32U - common);
            if ((other.network & mask) == (virtual_net.network & mask)) goto invalid;
        }
    }
    if (!append(rules, capacity, &used,
        "table ip edgenode_vpn {\n chain prerouting { type nat hook prerouting priority dstnat; policy accept;\n"
        " iifname \"wg\" ip daddr %s dnat to %s\n", request->edge_address, plan->host_ip)) goto overflow;
    for (pb_size_t i = 0; i < request->routes_count; ++i) {
        const iot_edge_v1_VpnRoute *route = &request->routes[i];
        if (route->enabled && strcmp(route->mode, "nat") == 0 && !append(rules, capacity, &used,
            " iifname \"wg\" ip daddr %s dnat ip prefix to ip daddr map { %s : %s }\n",
            route->virtual_cidr, route->virtual_cidr, route->target_cidr)) goto overflow;
    }
    if (!append(rules, capacity, &used,
        " }\n chain forward { type filter hook forward priority filter; policy drop;\n"
        " ct state established,related accept\n"
        " iifname \"wg\" oifname \"uplink\" ip daddr %s accept\n", plan->host_ip)) goto overflow;
    for (pb_size_t i = 0; i < request->routes_count; ++i) {
        const iot_edge_v1_VpnRoute *route = &request->routes[i];
        if (route->enabled && !append(rules, capacity, &used,
            " iifname \"wg\" oifname \"uplink\" ip daddr %s accept\n",
            strcmp(route->mode, "nat") == 0 ? route->target_cidr : route->virtual_cidr)) goto overflow;
    }
    if (!append(rules, capacity, &used,
        " }\n chain postrouting { type nat hook postrouting priority srcnat; policy accept;\n"
        " iifname \"wg\" oifname \"uplink\" snat to %s\n }\n}\n", plan->peer_host)) goto overflow;
    return true;
invalid:
    if (error && error_size) snprintf(error, error_size, "invalid VPN address or route mapping");
    rules[0] = '\0'; return false;
overflow:
    if (error && error_size) snprintf(error, error_size, "VPN rules exceed bounded capacity");
    rules[0] = '\0'; return false;
}
