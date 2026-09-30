#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edge.pb.h"

typedef struct {
    char namespace_name[40];
    char host_link[16];
    char transport_link[16];
    char host_address[20];
    char peer_address[20];
    char peer_host[16];
    char host_ip[16];
    char key_path[96];
    char rules_path[96];
    char root_rules_path[96];
    char include_section[48];
} edge_vpn_plan;

/* Names use the complete platform UUID; short link names are scoped to slots
 * owned by this application. CIDRs are private transit links, never VPN pools. */
bool edge_vpn_plan_init(edge_vpn_plan *plan, const uint8_t platform_id[16], size_t slot);
bool edge_vpn_plan_rules(const edge_vpn_plan *plan,
                         const iot_edge_v1_VpnConfigRequest *request,
                         char *rules, size_t capacity, char *error, size_t error_size);
