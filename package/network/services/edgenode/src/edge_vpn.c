#include "edge_vpn.h"
#include "edge_version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uci.h>
#include <unistd.h>

#include "edge_process.h"

#define EDGE_VPN_INTERFACE "wg"
#define EDGE_VPN_PEER_SECTION "iot_server"
#define EDGE_VPN_PEER_DESCRIPTION "IoT VPN Hub"
#define EDGE_VPN_KEY_PATH "/etc/edgenode/vpn.key"
#define EDGE_VPN_OVERLAY_CIDR "100.96.0.0/11"
#define EDGE_VPN_VIRTUAL_POOL_CIDR "172.16.0.0/12"
#define EDGE_VPN_VIRTUAL_POOL_NETWORK 0xAC100000U /* 172.16.0.0/12 */
#define EDGE_VPN_VIRTUAL_POOL_MASK 0xFFF00000U
#define EDGE_VPN_AGENT_VERSION EDGE_SOFTWARE_VERSION
#define EDGE_VPN_FIREWALL_DIRECTORY "/tmp/edgenode"
#define EDGE_VPN_DSTNAT_INCLUDE EDGE_VPN_FIREWALL_DIRECTORY "/vpn-dstnat.nft"
#define EDGE_VPN_FORWARD_INCLUDE EDGE_VPN_FIREWALL_DIRECTORY "/vpn-forward.nft"
#define EDGE_VPN_SRCNAT_INCLUDE EDGE_VPN_FIREWALL_DIRECTORY "/vpn-srcnat.nft"
#define EDGE_VPN_LEGACY_DSTNAT_INCLUDE \
    "/usr/share/nftables.d/chain-pre/dstnat/30-edgenode-vpn.nft"
#define EDGE_VPN_LEGACY_FORWARD_INCLUDE \
    "/usr/share/nftables.d/chain-pre/forward/30-edgenode-vpn.nft"
#define EDGE_VPN_LEGACY_SRCNAT_INCLUDE \
    "/usr/share/nftables.d/chain-pre/srcnat/30-edgenode-vpn.nft"

typedef struct {
    uint32_t network;
    uint8_t prefix;
} edge_vpn_cidr;

typedef struct {
    const char *section;
    const char *path;
    const char *chain;
} edge_vpn_firewall_include;

static const edge_vpn_firewall_include firewall_includes[] = {
    {"edgenode_vpn_dstnat", EDGE_VPN_DSTNAT_INCLUDE, "dstnat"},
    {"edgenode_vpn_forward", EDGE_VPN_FORWARD_INCLUDE, "forward"},
    {"edgenode_vpn_srcnat", EDGE_VPN_SRCNAT_INCLUDE, "srcnat"},
};

static uint64_t applied_version;

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "VPN operation failed");
}

static void safe_copy(char *output, size_t capacity, const char *input) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static bool write_all(int fd, const void *data, size_t size) {
    const uint8_t *bytes = data;
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t written = write(fd, bytes + offset, size - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static int hex_value(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static bool hex_to_bytes(const char *input, uint8_t *output, size_t size) {
    if (input == NULL || output == NULL || strlen(input) != size * 2U)
        return false;
    for (size_t index = 0U; index < size; ++index) {
        const int high = hex_value(input[index * 2U]);
        const int low = hex_value(input[index * 2U + 1U]);
        if (high < 0 || low < 0)
            return false;
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

static void bytes_to_hex(const uint8_t *input, size_t size, char *output, size_t capacity) {
    static const char alphabet[] = "0123456789abcdef";
    if (input == NULL || output == NULL || capacity < size * 2U + 1U)
        return;
    for (size_t index = 0U; index < size; ++index) {
        output[index * 2U] = alphabet[input[index] >> 4U];
        output[index * 2U + 1U] = alphabet[input[index] & 0x0fU];
    }
    output[size * 2U] = '\0';
}

static int base64_value(char value) {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

static bool base64_decode_key(const char *input, uint8_t output[32]) {
    if (input == NULL || output == NULL || strlen(input) != 44U || input[43] != '=')
        return false;
    size_t output_index = 0U;
    for (size_t index = 0U; index < 44U; index += 4U) {
        const int first = base64_value(input[index]);
        const int second = base64_value(input[index + 1U]);
        const int third = input[index + 2U] == '=' ? 0 : base64_value(input[index + 2U]);
        const int fourth = input[index + 3U] == '=' ? 0 : base64_value(input[index + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0)
            return false;
        if (output_index < 32U)
            output[output_index++] = (uint8_t)((first << 2U) | (second >> 4U));
        if (input[index + 2U] != '=' && output_index < 32U)
            output[output_index++] = (uint8_t)((second << 4U) | (third >> 2U));
        if (input[index + 3U] != '=' && output_index < 32U)
            output[output_index++] = (uint8_t)((third << 6U) | fourth);
    }
    return output_index == 32U;
}

static bool base64_encode_key(const uint8_t input[32], char output[45]) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (input == NULL || output == NULL)
        return false;
    size_t out = 0U;
    for (size_t index = 0U; index < 32U; index += 3U) {
        const size_t left = 32U - index;
        const uint32_t value = ((uint32_t)input[index] << 16U) |
                               (left > 1U ? (uint32_t)input[index + 1U] << 8U : 0U) |
                               (left > 2U ? input[index + 2U] : 0U);
        output[out++] = alphabet[(value >> 18U) & 0x3fU];
        output[out++] = alphabet[(value >> 12U) & 0x3fU];
        output[out++] = left > 1U ? alphabet[(value >> 6U) & 0x3fU] : '=';
        output[out++] = left > 2U ? alphabet[value & 0x3fU] : '=';
    }
    output[out] = '\0';
    return out == 44U;
}

static bool load_or_create_private_key(char output[65]) {
    if (mkdir("/etc/edgenode", 0700) != 0 && errno != EEXIST)
        return false;
    const int existing = open(EDGE_VPN_KEY_PATH, O_RDONLY | O_CLOEXEC);
    if (existing >= 0) {
        char buffer[96] = {0};
        const ssize_t count = read(existing, buffer, sizeof(buffer) - 1U);
        close(existing);
        if (count <= 0)
            return false;
        buffer[count] = '\0';
        char *end = strpbrk(buffer, "\r\n \t");
        if (end != NULL)
            *end = '\0';
        uint8_t key[32];
        if (!hex_to_bytes(buffer, key, sizeof(key)))
            return false;
        safe_copy(output, 65U, buffer);
        return true;
    }
    if (errno != ENOENT)
        return false;

    uint8_t random_bytes[32];
    const int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0 || read(random_fd, random_bytes, sizeof(random_bytes)) !=
                              (ssize_t)sizeof(random_bytes)) {
        if (random_fd >= 0)
            close(random_fd);
        return false;
    }
    close(random_fd);
    random_bytes[0] &= 248U;
    random_bytes[31] &= 127U;
    random_bytes[31] |= 64U;
    char key[65] = {0};
    bytes_to_hex(random_bytes, sizeof(random_bytes), key, sizeof(key));
    const int created = open(EDGE_VPN_KEY_PATH,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (created < 0) {
        if (errno == EEXIST)
            return load_or_create_private_key(output);
        return false;
    }
    const bool written = write_all(created, key, strlen(key)) && fsync(created) == 0;
    close(created);
    if (!written) {
        unlink(EDGE_VPN_KEY_PATH);
        return false;
    }
    safe_copy(output, 65U, key);
    return true;
}

static bool private_key_base64(const char private_hex[65], char output[45]) {
    uint8_t private_bytes[32];
    return hex_to_bytes(private_hex, private_bytes, sizeof(private_bytes)) &&
           base64_encode_key(private_bytes, output);
}

static bool read_public_key(const char private_key[45], char output[45]) {
    if (mkdir("/tmp/edgenode", 0700) != 0 && errno != EEXIST)
        return false;
    char input_path[] = "/tmp/edgenode/wg-private.XXXXXX";
    char output_path[] = "/tmp/edgenode/wg-public.XXXXXX";
    const int input = mkstemp(input_path);
    if (input < 0)
        return false;
    unlink(input_path);
    const int result = mkstemp(output_path);
    if (result < 0) {
        close(input);
        return false;
    }
    unlink(output_path);
    char private_line[46];
    snprintf(private_line, sizeof(private_line), "%s\n", private_key);
    bool success = write_all(input, private_line, strlen(private_line)) &&
                   lseek(input, 0, SEEK_SET) == 0;
    const char *const command[] = {"wg", "pubkey", NULL};
    if (success)
        success = edge_process_run(command, input, result) == 0 &&
                  lseek(result, 0, SEEK_SET) == 0;
    char public_line[64] = {0};
    if (success) {
        const ssize_t count = read(result, public_line, sizeof(public_line) - 1U);
        success = count > 0;
        if (success) {
            public_line[count] = '\0';
            public_line[strcspn(public_line, "\r\n \t")] = '\0';
            uint8_t public_bytes[32];
            success = base64_decode_key(public_line, public_bytes);
        }
    }
    close(result);
    close(input);
    if (success)
        safe_copy(output, 45U, public_line);
    return success;
}

static bool parse_cidr(const char *value, edge_vpn_cidr *output) {
    if (value == NULL || output == NULL || strlen(value) > 18U)
        return false;
    const char *slash = strchr(value, '/');
    if (slash == NULL || slash == value || strchr(slash + 1, '/') != NULL)
        return false;
    char address_text[16];
    const size_t address_size = (size_t)(slash - value);
    if (address_size >= sizeof(address_text))
        return false;
    memcpy(address_text, value, address_size);
    address_text[address_size] = '\0';
    struct in_addr address;
    if (inet_pton(AF_INET, address_text, &address) != 1)
        return false;
    char *end = NULL;
    errno = 0;
    const unsigned long prefix = strtoul(slash + 1, &end, 10);
    if (errno != 0 || end == slash + 1 || *end != '\0' || prefix == 0U || prefix > 32U)
        return false;
    const uint32_t mask = 0xffffffffU << (32U - (unsigned)prefix);
    output->network = ntohl(address.s_addr) & mask;
    output->prefix = (uint8_t)prefix;
    return true;
}

static void format_ipv4(uint32_t value, char output[16]) {
    struct in_addr address;
    address.s_addr = htonl(value);
    (void)inet_ntop(AF_INET, &address, output, 16U);
}

static void format_cidr(const edge_vpn_cidr *cidr, char output[20]) {
    char address[16];
    format_ipv4(cidr->network, address);
    snprintf(output, 20U, "%s/%u", address, (unsigned)cidr->prefix);
}

static bool private_network(uint32_t network) {
    return (network & 0xff000000U) == 0x0a000000U ||
           (network & 0xfff00000U) == 0xac100000U ||
           (network & 0xffff0000U) == 0xc0a80000U;
}

static bool route_valid(const iot_edge_v1_VpnRoute *route, edge_vpn_cidr *virtual_cidr,
                        edge_vpn_cidr *target_cidr) {
    if (route == NULL || virtual_cidr == NULL || target_cidr == NULL ||
        !parse_cidr(route->virtual_cidr, virtual_cidr) ||
        !parse_cidr(route->target_cidr, target_cidr) ||
        virtual_cidr->prefix != target_cidr->prefix ||
        (virtual_cidr->network & EDGE_VPN_VIRTUAL_POOL_MASK) !=
            EDGE_VPN_VIRTUAL_POOL_NETWORK ||
        !private_network(target_cidr->network))
        return false;
    if (strcmp(route->mode, "nat") == 0)
        return strcmp(route->nat_mode, "masquerade") == 0;
    if (strcmp(route->mode, "routed") == 0)
        return strcmp(route->nat_mode, "none") == 0;
    return false;
}

static bool parse_port(const char *value, unsigned *output) {
    if (value == NULL || output == NULL || value[0] == '\0')
        return false;
    char *end = NULL;
    errno = 0;
    const unsigned long port = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || port == 0U || port > 65535U)
        return false;
    *output = (unsigned)port;
    return true;
}

static bool split_endpoint(const char *input, unsigned fallback_port,
                           char host[256], char port[6]) {
    if (input == NULL || input[0] == '\0' || host == NULL || port == NULL ||
        fallback_port == 0U || fallback_port > 65535U)
        return false;
    const size_t length = strlen(input);
    if (input[0] == '[') {
        const char *closing = strchr(input + 1, ']');
        if (closing == NULL || closing == input + 1 ||
            (closing[1] != '\0' && closing[1] != ':'))
            return false;
        const size_t host_size = (size_t)(closing - input - 1);
        if (host_size >= 256U)
            return false;
        memcpy(host, input + 1, host_size);
        host[host_size] = '\0';
        if (closing[1] == ':' && !parse_port(closing + 2, &fallback_port))
            return false;
    } else {
        const char *last_colon = strrchr(input, ':');
        const char *first_colon = strchr(input, ':');
        if (last_colon != NULL && first_colon == last_colon && last_colon[1] != '\0') {
            if (strlen(last_colon + 1) >= 6U || !parse_port(last_colon + 1, &fallback_port))
                return false;
            const size_t host_size = (size_t)(last_colon - input);
            if (host_size == 0U || host_size >= 256U)
                return false;
            memcpy(host, input, host_size);
            host[host_size] = '\0';
        } else {
            if (length >= 256U)
                return false;
            safe_copy(host, 256U, input);
        }
    }
    snprintf(port, 6U, "%u", fallback_port);
    return host[0] != '\0';
}

static bool set_uci_option(struct uci_context *context, struct uci_package *package,
                           struct uci_section *section, const char *name,
                           const char *value) {
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = name,
        .value = value,
    };
    return uci_set(context, &pointer) == UCI_OK;
}

static bool add_uci_list(struct uci_context *context, struct uci_package *package,
                         struct uci_section *section, const char *name,
                         const char *value) {
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = name,
        .value = value,
    };
    return uci_add_list(context, &pointer) == UCI_OK;
}

static bool delete_uci_section(struct uci_context *context, struct uci_package *package,
                               struct uci_section *section) {
    struct uci_ptr pointer = {.p = package, .s = section};
    return uci_delete(context, &pointer) == UCI_OK;
}

static bool add_named_section(struct uci_context *context, struct uci_package *package,
                              const char *type, const char *name,
                              struct uci_section **output) {
    struct uci_section *section = NULL;
    if (uci_add_section(context, package, type, &section) != UCI_OK)
        return false;
    struct uci_ptr rename = {
        .p = package,
        .s = section,
        .value = name,
    };
    if (uci_rename(context, &rename) != UCI_OK)
        return false;
    *output = section;
    return true;
}

static bool remove_network_sections(struct uci_context *context,
                                    struct uci_package *package, bool *changed) {
    char names[32][64];
    size_t count = 0U;
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        const struct uci_section *section = uci_to_section(element);
        if ((strcmp(section->e.name, EDGE_VPN_INTERFACE) == 0 ||
             strcmp(section->type, "wireguard_" EDGE_VPN_INTERFACE) == 0) &&
            count < 32U) {
            safe_copy(names[count++], sizeof(names[0]), section->e.name);
        }
    }
    for (size_t index = 0U; index < count; ++index) {
        struct uci_section *section = uci_lookup_section(context, package, names[index]);
        if (section != NULL && !delete_uci_section(context, package, section))
            return false;
        if (section != NULL)
            *changed = true;
    }
    return true;
}

static bool configure_network(const char *private_key, const char *edge_address,
                              const char *hub_public_key, const char *endpoint_host,
                              const char *endpoint_port) {
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "network", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        return false;
    }

    bool changed = false;
    bool success = remove_network_sections(context, package, &changed);
    struct uci_section *interface = NULL;
    struct uci_section *peer = NULL;
    if (success)
        success = add_named_section(context, package, "interface", EDGE_VPN_INTERFACE,
                                    &interface);
    if (success)
        success = set_uci_option(context, package, interface, "proto", "wireguard") &&
                  set_uci_option(context, package, interface, "private_key", private_key) &&
                  add_uci_list(context, package, interface, "addresses", edge_address);
    if (success)
        success = add_named_section(context, package, "wireguard_" EDGE_VPN_INTERFACE,
                                    EDGE_VPN_PEER_SECTION, &peer);
    if (success)
        success = set_uci_option(context, package, peer, "description",
                                 EDGE_VPN_PEER_DESCRIPTION) &&
                  set_uci_option(context, package, peer, "public_key", hub_public_key) &&
                  set_uci_option(context, package, peer, "endpoint_host", endpoint_host) &&
                  set_uci_option(context, package, peer, "endpoint_port", endpoint_port) &&
                  set_uci_option(context, package, peer, "persistent_keepalive", "25") &&
                  set_uci_option(context, package, peer, "route_allowed_ips", "1") &&
                  add_uci_list(context, package, peer, "allowed_ips",
                               EDGE_VPN_OVERLAY_CIDR) &&
                  add_uci_list(context, package, peer, "allowed_ips",
                               EDGE_VPN_VIRTUAL_POOL_CIDR);
    if (success)
        success = uci_save(context, package) == UCI_OK &&
                  uci_commit(context, &package, false) == UCI_OK;
    if (package != NULL)
        uci_unload(context, package);
    uci_free_context(context);
    if (!success)
        return false;
    const char *const reload[] = {"ubus", "call", "network", "reload", NULL};
    const char *const up[] = {"ifup", EDGE_VPN_INTERFACE, NULL};
    return edge_process_run(reload, -1, -1) == 0 &&
           edge_process_run(up, -1, -1) == 0;
}

static bool remove_network_configuration(void) {
    const char *const down[] = {"ifdown", EDGE_VPN_INTERFACE, NULL};
    (void)edge_process_run(down, -1, -1);
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "network", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        return false;
    }
    bool changed = false;
    bool success = remove_network_sections(context, package, &changed);
    if (success && changed)
        success = uci_save(context, package) == UCI_OK &&
                  uci_commit(context, &package, false) == UCI_OK;
    if (package != NULL)
        uci_unload(context, package);
    uci_free_context(context);
    if (!success)
        return false;
    if (!changed)
        return true;
    const char *const reload[] = {"ubus", "call", "network", "reload", NULL};
    return edge_process_run(reload, -1, -1) == 0;
}

static bool firewall_network_contains(struct uci_context *context,
                                      struct uci_section *section,
                                      const char *value) {
    struct uci_option *option = uci_lookup_option(context, section, "network");
    if (option == NULL)
        return false;
    if (option->type == UCI_TYPE_STRING)
        return strcmp(option->v.string, value) == 0;
    if (option->type != UCI_TYPE_LIST)
        return false;
    struct uci_element *element;
    uci_foreach_element(&option->v.list, element) {
        if (strcmp(element->name, value) == 0)
            return true;
    }
    return false;
}

static bool remove_firewall_include_sections(struct uci_context *context,
                                             struct uci_package *package,
                                             bool *changed) {
    for (size_t index = 0U;
         index < sizeof(firewall_includes) / sizeof(firewall_includes[0]); ++index) {
        struct uci_section *section =
            uci_lookup_section(context, package, firewall_includes[index].section);
        if (section == NULL)
            continue;
        if (!delete_uci_section(context, package, section))
            return false;
        *changed = true;
    }
    return true;
}

static bool add_firewall_include_sections(struct uci_context *context,
                                          struct uci_package *package) {
    for (size_t index = 0U;
         index < sizeof(firewall_includes) / sizeof(firewall_includes[0]); ++index) {
        struct uci_section *section = NULL;
        if (!add_named_section(context, package, "include",
                               firewall_includes[index].section, &section) ||
            !set_uci_option(context, package, section, "type", "nftables") ||
            !set_uci_option(context, package, section, "path",
                            firewall_includes[index].path) ||
            !set_uci_option(context, package, section, "position", "chain-prepend") ||
            !set_uci_option(context, package, section, "chain",
                            firewall_includes[index].chain))
            return false;
    }
    return true;
}

static bool configure_firewall_uci(bool enabled) {
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "firewall", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        return false;
    }
    struct uci_section *lan = NULL;
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        const char *name = uci_lookup_option_string(context, section, "name");
        if (strcmp(section->type, "zone") == 0 && name != NULL &&
            strcmp(name, "lan") == 0) {
            lan = section;
            break;
        }
    }
    bool success = lan != NULL;
    bool changed = false;
    if (success)
        success = remove_firewall_include_sections(context, package, &changed);
    const bool contains = success && firewall_network_contains(context, lan,
                                                               EDGE_VPN_INTERFACE);
    if (success && enabled && !contains) {
        success = add_uci_list(context, package, lan, "network", EDGE_VPN_INTERFACE);
        changed = success;
    } else if (success && !enabled && contains) {
        struct uci_ptr pointer = {
            .p = package,
            .s = lan,
            .option = "network",
            .value = EDGE_VPN_INTERFACE,
        };
        success = uci_del_list(context, &pointer) == UCI_OK;
        changed = success;
    }
    if (success && enabled) {
        success = add_firewall_include_sections(context, package);
        changed = success;
    }
    if (success && changed)
        success = uci_save(context, package) == UCI_OK &&
                  uci_commit(context, &package, false) == UCI_OK;
    if (package != NULL)
        uci_unload(context, package);
    uci_free_context(context);
    return success;
}

static bool ensure_firewall_directories(void) {
    return mkdir(EDGE_VPN_FIREWALL_DIRECTORY, 0755) == 0 || errno == EEXIST;
}

static bool write_atomic(const char *path, const char *content) {
    char temporary[256];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof(temporary))
        return false;
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const bool written = write_all(fd, content, strlen(content)) && fsync(fd) == 0;
    close(fd);
    if (!written || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static bool append_rule(char *script, size_t capacity, size_t *used,
                        const char *format, const char *first, const char *second,
                        const char *third) {
    const int written = snprintf(script + *used, capacity - *used, format,
                                 first != NULL ? first : "", second != NULL ? second : "",
                                 third != NULL ? third : "");
    if (written < 0 || (size_t)written >= capacity - *used)
        return false;
    *used += (size_t)written;
    return true;
}

static bool configure_firewall(const iot_edge_v1_VpnConfigRequest *request) {
    if (!ensure_firewall_directories())
        return false;
    char dstnat[8192] = {0};
    char forward[8192] = {0};
    char srcnat[8192] = {0};
    size_t dstnat_used = 0U;
    size_t forward_used = 0U;
    size_t srcnat_used = 0U;
    bool have_nat = false;
    for (pb_size_t index = 0U; index < request->routes_count; ++index) {
        const iot_edge_v1_VpnRoute *route = &request->routes[index];
        if (!route->enabled)
            continue;
        edge_vpn_cidr virtual_cidr;
        edge_vpn_cidr target_cidr;
        if (!route_valid(route, &virtual_cidr, &target_cidr))
            return false;
        char virtual_text[20];
        char target_text[20];
        format_cidr(&virtual_cidr, virtual_text);
        format_cidr(&target_cidr, target_text);
        if (strcmp(route->mode, "nat") == 0) {
            if (!append_rule(dstnat, sizeof(dstnat), &dstnat_used,
                             "iifname \"" EDGE_VPN_INTERFACE
                             "\" ip daddr %s dnat ip prefix to ip daddr map { %s : %s }\n",
                             virtual_text, virtual_text, target_text) ||
                !append_rule(forward, sizeof(forward), &forward_used,
                             "iifname \"" EDGE_VPN_INTERFACE
                             "\" ip daddr %s accept\n",
                             target_text, NULL, NULL) ||
                !append_rule(forward, sizeof(forward), &forward_used,
                             "oifname \"" EDGE_VPN_INTERFACE
                             "\" ip saddr %s ip daddr "
                             EDGE_VPN_VIRTUAL_POOL_CIDR " accept\n",
                             target_text, NULL, NULL) ||
                !append_rule(srcnat, sizeof(srcnat), &srcnat_used,
                             "oifname \"" EDGE_VPN_INTERFACE
                             "\" ip saddr %s ip daddr "
                             EDGE_VPN_VIRTUAL_POOL_CIDR
                             " snat ip prefix to ip saddr map { %s : %s }\n",
                             target_text, target_text, virtual_text))
                return false;
            have_nat = true;
        } else if (!append_rule(forward, sizeof(forward), &forward_used,
                                "iifname \"" EDGE_VPN_INTERFACE
                                "\" ip daddr %s accept\n",
                                virtual_text, NULL, NULL)) {
            return false;
        }
    }
    if (!append_rule(forward, sizeof(forward), &forward_used,
                     "iifname \"" EDGE_VPN_INTERFACE "\" drop\n", NULL, NULL, NULL))
        return false;
    if (have_nat &&
        !append_rule(srcnat, sizeof(srcnat), &srcnat_used,
                     "oifname != \"" EDGE_VPN_INTERFACE "\" ip saddr "
                     EDGE_VPN_OVERLAY_CIDR " masquerade\n",
                     NULL, NULL, NULL))
        return false;
    if (!write_atomic(EDGE_VPN_DSTNAT_INCLUDE, dstnat) ||
        !write_atomic(EDGE_VPN_FORWARD_INCLUDE, forward) ||
        !write_atomic(EDGE_VPN_SRCNAT_INCLUDE, srcnat) ||
        !configure_firewall_uci(true))
        return false;
    unlink(EDGE_VPN_LEGACY_DSTNAT_INCLUDE);
    unlink(EDGE_VPN_LEGACY_FORWARD_INCLUDE);
    unlink(EDGE_VPN_LEGACY_SRCNAT_INCLUDE);
    const char *const reload[] = {"/etc/init.d/firewall", "reload", NULL};
    return edge_process_run(reload, -1, -1) == 0;
}

static bool parse_edge_address(const char *value, char canonical[20]) {
    edge_vpn_cidr cidr;
    if (!parse_cidr(value, &cidr) || cidr.prefix != 32U ||
        (cidr.network & 0xffe00000U) != 0x64600000U)
        return false;
    format_cidr(&cidr, canonical);
    return true;
}

bool edge_vpn_collect_capability(iot_edge_v1_VpnCapabilities *capability) {
#if !defined(__linux__)
    (void)capability;
    return false;
#else
    if (capability == NULL)
        return false;
    memset(capability, 0, sizeof(*capability));
    char private_hex[65];
    char private_key[45];
    char public_key[45];
    if (!load_or_create_private_key(private_hex) ||
        !private_key_base64(private_hex, private_key) ||
        !read_public_key(private_key, public_key))
        return false;
    capability->supports_vpn = true;
    safe_copy(capability->wireguard_version, sizeof(capability->wireguard_version),
              "netifd");
    safe_copy(capability->agent_version, sizeof(capability->agent_version),
              EDGE_VPN_AGENT_VERSION);
    safe_copy(capability->public_key, sizeof(capability->public_key), public_key);
    return true;
#endif
}

bool edge_vpn_apply(const iot_edge_v1_VpnConfigRequest *request,
                    char *error, size_t error_size) {
#if !defined(__linux__)
    (void)request;
    set_error(error, error_size, "unsupported_platform");
    return false;
#else
    if (request == NULL || request->request_id.size != 16U || request->config_version == 0U) {
        set_error(error, error_size, "invalid VPN request identity or version");
        return false;
    }
    if (request->config_version < applied_version) {
        set_error(error, error_size, "stale VPN configuration version");
        return false;
    }
    if (request->config_version == applied_version)
        return true;
    if (!request->enabled) {
        edge_vpn_shutdown();
        applied_version = request->config_version;
        return true;
    }
    char edge_address[20];
    uint8_t hub_key[32];
    char endpoint_host[256] = {0};
    char endpoint_port[6] = {0};
    if (!parse_edge_address(request->edge_address, edge_address) ||
        request->hub_listen_port == 0U || request->hub_listen_port > 65535U ||
        !base64_decode_key(request->hub_public_key, hub_key) ||
        !split_endpoint(request->hub_endpoint, request->hub_listen_port,
                        endpoint_host, endpoint_port)) {
        set_error(error, error_size, "invalid VPN network configuration");
        return false;
    }
    for (pb_size_t index = 0U; index < request->routes_count; ++index) {
        const iot_edge_v1_VpnRoute *route = &request->routes[index];
        if (!route->enabled)
            continue;
        edge_vpn_cidr virtual_cidr;
        edge_vpn_cidr target_cidr;
        if (!route_valid(route, &virtual_cidr, &target_cidr)) {
            set_error(error, error_size, "invalid VPN route mapping");
            return false;
        }
        for (pb_size_t previous = 0U; previous < index; ++previous) {
            if (!request->routes[previous].enabled)
                continue;
            edge_vpn_cidr previous_virtual;
            edge_vpn_cidr previous_target;
            if (route_valid(&request->routes[previous], &previous_virtual,
                            &previous_target) &&
                previous_virtual.network == virtual_cidr.network &&
                previous_virtual.prefix == virtual_cidr.prefix) {
                set_error(error, error_size, "duplicate VPN route mapping");
                return false;
            }
        }
    }
    if (request->routes_count > 16U) {
        set_error(error, error_size, "too many VPN route mappings");
        return false;
    }

    char private_hex[65];
    char private_key[45];
    if (!load_or_create_private_key(private_hex) ||
        !private_key_base64(private_hex, private_key) ||
        !configure_network(private_key, edge_address, request->hub_public_key,
                           endpoint_host, endpoint_port) ||
        !configure_firewall(request)) {
        edge_vpn_shutdown();
        set_error(error, error_size,
                  "cannot apply OpenWrt WireGuard or firewall4 configuration");
        return false;
    }
    applied_version = request->config_version;
    return true;
#endif
}

void edge_vpn_shutdown(void) {
#if defined(__linux__)
    unlink(EDGE_VPN_DSTNAT_INCLUDE);
    unlink(EDGE_VPN_FORWARD_INCLUDE);
    unlink(EDGE_VPN_SRCNAT_INCLUDE);
    unlink(EDGE_VPN_LEGACY_DSTNAT_INCLUDE);
    unlink(EDGE_VPN_LEGACY_FORWARD_INCLUDE);
    unlink(EDGE_VPN_LEGACY_SRCNAT_INCLUDE);
    (void)configure_firewall_uci(false);
    const char *const firewall_reload[] = {"/etc/init.d/firewall", "reload", NULL};
    (void)edge_process_run(firewall_reload, -1, -1);
    (void)remove_network_configuration();
#endif
}
