#include "edge_vpn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "edge_process.h"

#define EDGE_VPN_INTERFACE "wg-iot"
#define EDGE_VPN_KEY_PATH "/etc/edgenode/vpn.key"
#define EDGE_VPN_UAPI_PATH "/var/run/wireguard/wg-iot.sock"
#define EDGE_VPN_NFT_TABLE "edgenode_vpn"
#define EDGE_VPN_OVERLAY_CIDR "100.96.0.0/11"
#define EDGE_VPN_VIRTUAL_POOL_NETWORK 0xAC1F0000U /* 172.31.0.0/16 */
#define EDGE_VPN_AGENT_VERSION "0.3.34"

typedef struct {
    uint32_t network;
    uint8_t prefix;
} edge_vpn_cidr;

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

static bool read_all(int fd, char *output, size_t capacity) {
    if (output == NULL || capacity < 2U)
        return false;
    size_t used = 0U;
    while (used + 1U < capacity) {
        const ssize_t count = read(fd, output + used, capacity - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return false;
        break;
    }
    output[used] = '\0';
    return used + 1U < capacity;
}

static bool uapi_request(const char *request, char *response, size_t response_size) {
    if (request == NULL || response == NULL || response_size < 2U)
        return false;
    if (mkdir("/var/run/wireguard", 0755) != 0 && errno != EEXIST)
        return false;

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", EDGE_VPN_UAPI_PATH) >=
        (int)sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    const bool connected = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
    const bool sent = connected && write_all(fd, request, strlen(request)) &&
                      shutdown(fd, SHUT_WR) == 0;
    const bool received = sent && read_all(fd, response, response_size);
    close(fd);
    return received;
}

static bool response_ok(const char *response) {
    const char *value = strstr(response != NULL ? response : "", "errno=");
    if (value == NULL)
        return false;
    value += strlen("errno=");
    return *value == '0' && (value[1] == '\n' || value[1] == '\0');
}

static bool hex_byte(char high, char low, uint8_t *output) {
    const char *digits = "0123456789abcdefABCDEF";
    const char *first = strchr(digits, high);
    const char *second = strchr(digits, low);
    if (first == NULL || second == NULL || output == NULL)
        return false;
    const unsigned high_value = (unsigned)(first - digits) & 0x0fU;
    const unsigned low_value = (unsigned)(second - digits) & 0x0fU;
    *output = (uint8_t)((high_value << 4U) | low_value);
    return true;
}

static void bytes_to_hex(const uint8_t *input, size_t size, char *output, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    if (output == NULL || capacity < size * 2U + 1U)
        return;
    for (size_t index = 0U; index < size; ++index) {
        output[index * 2U] = digits[input[index] >> 4U];
        output[index * 2U + 1U] = digits[input[index] & 0x0fU];
    }
    output[size * 2U] = '\0';
}

static bool hex_to_bytes(const char *input, uint8_t *output, size_t size) {
    if (input == NULL || output == NULL || strlen(input) != size * 2U)
        return false;
    for (size_t index = 0U; index < size; ++index)
        if (!hex_byte(input[index * 2U], input[index * 2U + 1U], &output[index]))
            return false;
    return true;
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
        const int third = index + 2U == 43U ? 0 : base64_value(input[index + 2U]);
        const int fourth = index + 3U == 43U ? 0 : base64_value(input[index + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (index + 2U == 43U && input[index + 2U] != '=') ||
            (index + 3U == 43U && input[index + 3U] != '='))
            return false;
        if (output_index < 32U)
            output[output_index++] = (uint8_t)((first << 2U) | (second >> 4U));
        if (index + 2U < 43U && output_index < 32U)
            output[output_index++] = (uint8_t)((second << 4U) | (third >> 2U));
        if (index + 3U < 43U && output_index < 32U)
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

static bool valid_hex_key(const char *value) {
    uint8_t bytes[32];
    return hex_to_bytes(value, bytes, sizeof(bytes));
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
        if (!valid_hex_key(buffer))
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
    char key[65];
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

static bool interface_exists(void) {
    const char *const command[] = {"ip", "link", "show", "dev", EDGE_VPN_INTERFACE, NULL};
    return edge_process_run(command, -1, -1) == 0;
}

static bool add_interface(void) {
    const char *const command[] = {"ip", "link", "add", "dev", EDGE_VPN_INTERFACE,
                                   "type", "wireguard", NULL};
    return edge_process_run(command, -1, -1) == 0;
}

static bool delete_interface(void) {
    const char *const command[] = {"ip", "link", "del", "dev", EDGE_VPN_INTERFACE, NULL};
    return edge_process_run(command, -1, -1) == 0 || !interface_exists();
}

static bool set_private_key(const char private_key[65]) {
    char request[160];
    if (snprintf(request, sizeof(request), "set=1\nprivate_key=%s\n\n", private_key) >=
        (int)sizeof(request))
        return false;
    char response[1024];
    return uapi_request(request, response, sizeof(response)) && response_ok(response);
}

static bool read_public_key(char output[65]) {
    char response[2048];
    if (!uapi_request("get=1\n\n", response, sizeof(response)))
        return false;
    const char *value = strstr(response, "public_key=");
    if (value == NULL)
        return false;
    value += strlen("public_key=");
    const char *end = strchr(value, '\n');
    if (end == NULL || (size_t)(end - value) != 64U)
        return false;
    char hex[65];
    memcpy(hex, value, 64U);
    hex[64] = '\0';
    uint8_t bytes[32];
    if (!hex_to_bytes(hex, bytes, sizeof(bytes)) || !base64_encode_key(bytes, (char *)output))
        return false;
    return true;
}

static bool ensure_interface(char private_key[65]) {
    if (!load_or_create_private_key(private_key))
        return false;
    if (!interface_exists() && !add_interface())
        return false;
    if (set_private_key(private_key))
        return true;
    if (!delete_interface() || !add_interface())
        return false;
    return set_private_key(private_key);
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
        (virtual_cidr->network & 0xffff0000U) != EDGE_VPN_VIRTUAL_POOL_NETWORK ||
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
        if (last_colon != NULL && first_colon == last_colon &&
            last_colon[1] != '\0') {
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

static bool resolve_endpoint(const char *input, unsigned fallback_port,
                             char output[272]) {
    char host[256] = {0};
    char port[6] = {0};
    if (!split_endpoint(input, fallback_port, host, port))
        return false;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses) != 0 || addresses == NULL)
        return false;
    bool resolved = false;
    for (const struct addrinfo *address = addresses; address != NULL && !resolved;
         address = address->ai_next) {
        char numeric_host[256] = {0};
        char numeric_port[6] = {0};
        if (getnameinfo(address->ai_addr, address->ai_addrlen, numeric_host,
                        sizeof(numeric_host), numeric_port, sizeof(numeric_port),
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0)
            continue;
        if (address->ai_family == AF_INET6)
            resolved = snprintf(output, 272U, "[%s]:%s", numeric_host, numeric_port) < 272;
        else
            resolved = snprintf(output, 272U, "%s:%s", numeric_host, numeric_port) < 272;
    }
    freeaddrinfo(addresses);
    return resolved;
}

static bool uapi_set_config(const char private_key[65], const char *hub_public_key,
                            const char *endpoint, unsigned listen_port) {
    uint8_t hub_key[32];
    if (!base64_decode_key(hub_public_key, hub_key))
        return false;
    char hub_key_hex[65];
    bytes_to_hex(hub_key, sizeof(hub_key), hub_key_hex, sizeof(hub_key_hex));
    char request[4096];
    const int length = snprintf(
        request, sizeof(request),
        "set=1\nprivate_key=%s\nlisten_port=%u\nreplace_peers=true\n"
        "public_key=%s\nendpoint=%s\npersistent_keepalive_interval=25\n"
        "replace_allowed_ips=true\nallowed_ip=%s\n\n",
        private_key, listen_port, hub_key_hex, endpoint, EDGE_VPN_OVERLAY_CIDR);
    if (length < 0 || length >= (int)sizeof(request))
        return false;
    char response[1024];
    return uapi_request(request, response, sizeof(response)) && response_ok(response);
}

static bool run_ip_address(const char *address) {
    const char *const command[] = {"ip", "address", "replace", address, "dev",
                                   EDGE_VPN_INTERFACE, NULL};
    return edge_process_run(command, -1, -1) == 0;
}

static bool run_ip_up(void) {
    const char *const command[] = {"ip", "link", "set", "up", "dev", EDGE_VPN_INTERFACE,
                                   NULL};
    return edge_process_run(command, -1, -1) == 0;
}

static bool append_script(char *script, size_t capacity, size_t *used, const char *format,
                          const char *first, const char *second) {
    if (script == NULL || used == NULL || format == NULL)
        return false;
    const int written = snprintf(script + *used, capacity - *used, format,
                                 first != NULL ? first : "", second != NULL ? second : "");
    if (written < 0 || (size_t)written >= capacity - *used)
        return false;
    *used += (size_t)written;
    return true;
}

static bool run_nft_script(const char *script) {
    if (script == NULL ||
        (mkdir("/tmp/edgenode", 0700) != 0 && errno != EEXIST))
        return false;
    const char *const remove_table[] = {"nft", "delete", "table", "inet", EDGE_VPN_NFT_TABLE,
                                        NULL};
    (void)edge_process_run(remove_table, -1, -1);
    char path[128];
    if (snprintf(path, sizeof(path), "/tmp/edgenode/vpn.%ld.nft", (long)getpid()) >=
        (int)sizeof(path))
        return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const bool written = write_all(fd, script, strlen(script));
    close(fd);
    if (!written) {
        unlink(path);
        return false;
    }
    const char *const command[] = {"nft", "-f", path, NULL};
    const bool success = edge_process_run(command, -1, -1) == 0;
    unlink(path);
    return success;
}

static bool apply_firewall(const iot_edge_v1_VpnConfigRequest *request) {
    char script[16384];
    size_t used = 0U;
    const char *header =
        "add table inet " EDGE_VPN_NFT_TABLE "\n"
        "add chain inet " EDGE_VPN_NFT_TABLE " prerouting { type nat hook prerouting priority -100; policy accept; }\n"
        "add chain inet " EDGE_VPN_NFT_TABLE " postrouting { type nat hook postrouting priority 100; policy accept; }\n"
        "add chain inet " EDGE_VPN_NFT_TABLE " forward { type filter hook forward priority 0; policy accept; }\n";
    if (snprintf(script, sizeof(script), "%s", header) >= (int)sizeof(script))
        return false;
    used = strlen(script);
    bool have_nat = false;
    bool have_route = false;
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
            if (!append_script(script, sizeof(script), &used,
                               "add rule inet " EDGE_VPN_NFT_TABLE
                               " prerouting iifname \"" EDGE_VPN_INTERFACE
                               "\" ip daddr %s dnat to %s\n",
                               virtual_text, target_text))
                return false;
            have_nat = true;
            if (!append_script(script, sizeof(script), &used,
                               "add rule inet " EDGE_VPN_NFT_TABLE
                               " forward iifname \"" EDGE_VPN_INTERFACE
                               "\" ip daddr %s accept\n",
                               target_text, NULL))
                return false;
        } else if (!append_script(script, sizeof(script), &used,
                                  "add rule inet " EDGE_VPN_NFT_TABLE
                                  " forward iifname \"" EDGE_VPN_INTERFACE
                                  "\" ip daddr %s accept\n",
                                  virtual_text, NULL)) {
            return false;
        }
        have_route = true;
    }
    if (have_nat &&
        !append_script(script, sizeof(script), &used,
                       "add rule inet " EDGE_VPN_NFT_TABLE
                       " postrouting oifname != \"" EDGE_VPN_INTERFACE
                       "\" ip saddr " EDGE_VPN_OVERLAY_CIDR " masquerade\n",
                       NULL, NULL))
        return false;
    if (have_route &&
        !append_script(script, sizeof(script), &used,
                       "add rule inet " EDGE_VPN_NFT_TABLE
                       " forward oifname \"" EDGE_VPN_INTERFACE
                       "\" ct state established,related accept\n",
                       NULL, NULL))
        return false;
    return run_nft_script(script);
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
    char private_key[65];
    if (!ensure_interface(private_key))
        return false;
    char public_key[65];
    if (!read_public_key(public_key))
        return false;
    capability->supports_vpn = true;
    safe_copy(capability->wireguard_version, sizeof(capability->wireguard_version), "kernel");
    FILE *version = fopen("/sys/module/wireguard/version", "r");
    if (version != NULL) {
        char value[33] = {0};
        if (fgets(value, sizeof(value), version) != NULL) {
            value[strcspn(value, "\r\n")] = '\0';
            if (value[0] != '\0')
                safe_copy(capability->wireguard_version,
                          sizeof(capability->wireguard_version), value);
        }
        fclose(version);
    }
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
    if (!parse_edge_address(request->edge_address, edge_address)) {
        set_error(error, error_size, "invalid VPN edge address");
        return false;
    }
    uint8_t hub_key[32];
    if (request->hub_listen_port == 0U || request->hub_listen_port > 65535U ||
        !base64_decode_key(request->hub_public_key, hub_key)) {
        set_error(error, error_size, "invalid VPN hub key or port");
        return false;
    }
    bool have_nat = false;
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
        if (strcmp(route->mode, "nat") == 0)
            have_nat = true;
        for (pb_size_t previous = 0U; previous < index; ++previous) {
            if (!request->routes[previous].enabled)
                continue;
            edge_vpn_cidr previous_virtual;
            edge_vpn_cidr previous_target;
            if (route_valid(&request->routes[previous], &previous_virtual, &previous_target) &&
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
    char endpoint[272];
    if (!resolve_endpoint(request->hub_endpoint, request->hub_listen_port, endpoint)) {
        set_error(error, error_size, "cannot resolve VPN hub endpoint");
        return false;
    }
    char private_key[65];
    if (!ensure_interface(private_key) ||
        !uapi_set_config(private_key, request->hub_public_key, endpoint,
                         request->hub_listen_port) ||
        !run_ip_address(edge_address) || !run_ip_up() || !apply_firewall(request)) {
        edge_vpn_shutdown();
        set_error(error, error_size,
                  have_nat ? "cannot apply WireGuard or nftables NAT configuration"
                            : "cannot apply WireGuard or nftables configuration");
        return false;
    }
    applied_version = request->config_version;
    return true;
#endif
}

void edge_vpn_shutdown(void) {
#if defined(__linux__)
    const char *const remove_table[] = {"nft", "delete", "table", "inet", EDGE_VPN_NFT_TABLE,
                                        NULL};
    (void)edge_process_run(remove_table, -1, -1);
    (void)delete_interface();
#endif
}
