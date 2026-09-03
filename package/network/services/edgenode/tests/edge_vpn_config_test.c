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
    require(strstr(source, "\"proto\", \"wireguard\"") != NULL,
            "wg is not managed by the native netifd WireGuard protocol");
    require(strstr(source, "\"wireguard_\" EDGE_VPN_INTERFACE") != NULL,
            "native netifd peer section is missing");
    require(strstr(source, "configure_lan_membership(true)") != NULL,
            "wg is not added to the LAN firewall zone");
    require(strstr(source, "/usr/share/nftables.d/chain-pre/dstnat/") != NULL &&
                strstr(source, "/usr/share/nftables.d/chain-pre/forward/") != NULL &&
                strstr(source, "/usr/share/nftables.d/chain-pre/srcnat/") != NULL,
            "VPN mapping is not integrated with firewall4 includes");
    require(strstr(source, "ip\", \"link\", \"add") == NULL &&
                strstr(source, "WG_CMD_SET_DEVICE") == NULL,
            "edge still configures WireGuard outside netifd");
    free(source);
    return EXIT_SUCCESS;
}
