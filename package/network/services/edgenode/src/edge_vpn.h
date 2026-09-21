#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

/*
 * Edge VPN uses the WireGuard kernel UAPI directly.  This keeps the image
 * free of wireguard-go, wg-quick, and a second VPN daemon while still making
 * configuration idempotent and restart-safe.
 */
bool edge_vpn_collect_capability(iot_edge_v1_VpnCapabilities *capability);

/* Applies one complete desired state. The private key never crosses this API. */
bool edge_vpn_apply(const iot_edge_v1_VpnConfigRequest *request,
                    char *error, size_t error_size);

/* Removes the managed interface and firewall rules during a clean shutdown. */
void edge_vpn_shutdown(void);

/* Interval counters for the managed WireGuard interface. Missing interface returns false. */
bool edge_vpn_sample(uint64_t now_ms, iot_edge_v1_TcpTraffic *report);
void edge_vpn_ack(uint64_t sample_id);
