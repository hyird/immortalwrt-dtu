#include "edge_vpn_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "VPN plan: %s\n", message); exit(EXIT_FAILURE); }
}

int main(int argc, char **argv) {
    const uint8_t a[16] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 1};
    const uint8_t b[16] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 2};
    edge_vpn_plan first, second;
    require(edge_vpn_plan_init(&first, a, 0U), "first platform");
    require(edge_vpn_plan_init(&second, b, 1U), "second platform");
    require(strcmp(first.namespace_name, second.namespace_name) != 0, "full UUID avoids prefix collision");
    require(strcmp(first.key_path, second.key_path) != 0, "separate keys");
    require(strcmp(first.peer_host, second.peer_host) != 0, "distinct LAN transit sources");
    require(strcmp(first.root_rules_path, second.root_rules_path) != 0, "separate firewall lifecycle");
    const uint8_t zero[16] = {0}; edge_vpn_plan invalid;
    require(!edge_vpn_plan_init(&invalid, zero, 0U), "zero identity rejected");
    require(!edge_vpn_plan_init(&invalid, a, 4U), "platform count bounded");
    iot_edge_v1_VpnConfigRequest request = iot_edge_v1_VpnConfigRequest_init_zero;
    strcpy(request.edge_address, "100.96.0.2/32"); request.routes_count = 1;
    iot_edge_v1_VpnRoute *route = &request.routes[0];
    route->enabled = true; strcpy(route->mode, "nat"); strcpy(route->nat_mode, "masquerade");
    strcpy(route->virtual_cidr, "172.24.1.0/24"); strcpy(route->target_cidr, "192.168.1.0/24");
    char rules[8192], error[128];
    require(edge_vpn_plan_rules(&first, &request, rules, sizeof(rules), error, sizeof(error)), "valid NAT mapping");
    require(strstr(rules, "172.24.1.0/24 : 192.168.1.0/24") != NULL, "host bits preserved");
    if (argc >= 2 && strcmp(argv[1], "--rules") == 0) {
        if (argc == 3 && strcmp(argv[2], "second") == 0)
            require(edge_vpn_plan_rules(&second, &request, rules, sizeof(rules), error, sizeof(error)), "second platform rules");
        fputs(rules, stdout); return EXIT_SUCCESS;
    }
    require(edge_vpn_plan_rules(&second, &request, rules, sizeof(rules), error, sizeof(error)), "same VPN addresses accepted on another platform");
    require(strstr(rules, "snat to 169.254.240.6") != NULL, "second platform transit identity");
    strcpy(route->target_cidr, "172.18.1.0/24");
    require(edge_vpn_plan_rules(&first, &request, rules, sizeof(rules), error, sizeof(error)), "physical LAN can overlap virtual pool");
    request.routes_count = 2; request.routes[1] = *route;
    require(!edge_vpn_plan_rules(&first, &request, rules, sizeof(rules), error, sizeof(error)), "overlapping mappings in one platform rejected");
    request.routes_count = 1; strcpy(route->virtual_cidr, "172.24.1.1/24");
    require(!edge_vpn_plan_rules(&first, &request, rules, sizeof(rules), error, sizeof(error)), "non-network CIDR rejected");
    strcpy(route->virtual_cidr, "172.24.1.0/24"); strcpy(route->target_cidr, "8.8.8.0/24");
    require(!edge_vpn_plan_rules(&first, &request, rules, sizeof(rules), error, sizeof(error)), "public LAN target rejected");
    strcpy(route->target_cidr, "192.168.1.0/24"); strcpy(route->mode, "routed"); strcpy(route->nat_mode, "none");
    require(edge_vpn_plan_rules(&first, &request, rules, sizeof(rules), error, sizeof(error)), "routed mapping supported");
    require(strstr(rules, "prefix to") == NULL, "routed mapping is not prefix translated");
    require(!edge_vpn_plan_rules(&first, &request, rules, 16U, error, sizeof(error)), "output capacity bounded");
    return EXIT_SUCCESS;
}
