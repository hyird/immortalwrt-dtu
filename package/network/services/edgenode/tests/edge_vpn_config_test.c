#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge vpn config test failed: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static char *read_source(void) {
    FILE *file = fopen(EDGENODE_VPN_SOURCE, "rb");
    require(file != NULL, "cannot open edge_vpn.c");
    require(fseek(file, 0, SEEK_END) == 0, "cannot seek edge_vpn.c");
    const long length = ftell(file);
    require(length > 0, "edge_vpn.c is empty");
    rewind(file);
    char *source = malloc((size_t)length + 1U);
    require(source != NULL, "cannot allocate source buffer");
    require(fread(source, 1U, (size_t)length, file) == (size_t)length,
            "cannot read edge_vpn.c");
    source[length] = '\0';
    fclose(file);
    return source;
}

int main(void) {
    char *source = read_source();
    require(strstr(source, "#define EDGE_VPN_INTERFACE \"wg\"") != NULL,
            "managed interface is not named wg");
    require(strstr(source,
                   "#define EDGE_VPN_VIRTUAL_POOL_NETWORK 0xAC000000U") != NULL &&
                strstr(source,
                       "#define EDGE_VPN_VIRTUAL_POOL_MASK 0xFF000000U") != NULL,
            "virtual LAN pool is not 172.0.0.0/8");
    require(strstr(source, "\"proto\", \"wireguard\"") != NULL,
            "wg is not managed by the native netifd WireGuard protocol");
    require(strstr(source, "\"wireguard_\" EDGE_VPN_INTERFACE") != NULL,
            "native netifd peer section is missing");
    require(strstr(source, "configure_firewall_uci(true)") != NULL,
            "wg and VPN rules are not managed through firewall UCI");
    require(strstr(source, "\"type\", \"nftables\"") != NULL &&
                strstr(source, "\"position\", \"chain-prepend\"") != NULL &&
                strstr(source, "\"chain\"") != NULL,
            "VPN mapping is not registered as firewall4 UCI includes");
    require(strstr(source, "EDGE_VPN_FIREWALL_DIRECTORY \"/vpn-dstnat.nft\"") != NULL &&
                strstr(source, "EDGE_VPN_FIREWALL_DIRECTORY \"/vpn-forward.nft\"") != NULL &&
                strstr(source, "EDGE_VPN_FIREWALL_DIRECTORY \"/vpn-srcnat.nft\"") != NULL,
            "VPN firewall include paths are not runtime-managed");
    require(strstr(source, "dnat ip prefix to ip daddr map") != NULL,
            "VPN NAT does not preserve host bits across mapped prefixes");
    require(strstr(source, "ip\", \"link\", \"add") == NULL &&
                strstr(source, "WG_CMD_SET_DEVICE") == NULL,
            "edge still configures WireGuard outside netifd");
    free(source);
    return EXIT_SUCCESS;
}
