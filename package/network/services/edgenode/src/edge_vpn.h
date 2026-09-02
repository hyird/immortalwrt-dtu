#pragma once

#include <stdbool.h>
#include <stddef.h>

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
