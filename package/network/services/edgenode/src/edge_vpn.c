#include "edge_vpn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/wireguard.h>
#include <netdb.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edge_process.h"

#define EDGE_VPN_INTERFACE "wg-iot"
#define EDGE_VPN_KEY_PATH "/etc/edgenode/vpn.key"
#define EDGE_VPN_NFT_TABLE "edgenode_vpn"
#define EDGE_VPN_OVERLAY_CIDR "100.96.0.0/11"
#define EDGE_VPN_VIRTUAL_POOL_NETWORK 0xAC1F0000U /* 172.31.0.0/16 */
#define EDGE_VPN_AGENT_VERSION "0.3.37"
#define EDGE_VPN_NETLINK_BUFFER_SIZE 8192U

typedef struct {
    uint32_t network;
    uint8_t prefix;
} edge_vpn_cidr;

typedef struct {
    uint8_t data[EDGE_VPN_NETLINK_BUFFER_SIZE];
    size_t size;
} edge_vpn_netlink_message;

typedef bool (*edge_vpn_netlink_handler)(const struct nlmsghdr *header, void *context);

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

static size_t align4(size_t value) {
    return (value + 3U) & ~(size_t)3U;
}

static bool netlink_append(edge_vpn_netlink_message *message,
                           const void *data, size_t size) {
    if (message == NULL || data == NULL || size > sizeof(message->data) - message->size)
        return false;
    memcpy(message->data + message->size, data, size);
    message->size += size;
    return true;
}

static bool netlink_begin(edge_vpn_netlink_message *message, uint16_t type,
                          uint16_t flags, uint32_t sequence) {
    if (message == NULL)
        return false;
    memset(message, 0, sizeof(*message));
    const struct nlmsghdr header = {
        .nlmsg_len = NLMSG_HDRLEN,
        .nlmsg_type = type,
        .nlmsg_flags = flags,
        .nlmsg_seq = sequence,
    };
    return netlink_append(message, &header, sizeof(header));
}

static bool netlink_attribute(edge_vpn_netlink_message *message, uint16_t type,
                              const void *data, size_t size) {
    if (message == NULL || size > UINT16_MAX - NLA_HDRLEN)
        return false;
    const struct nlattr attribute = {
        .nla_len = (uint16_t)(NLA_HDRLEN + size),
        .nla_type = type,
    };
    if (!netlink_append(message, &attribute, sizeof(attribute)) ||
        (size != 0U && !netlink_append(message, data, size)))
        return false;
    const size_t aligned = align4(message->size);
    if (aligned > sizeof(message->data))
        return false;
    memset(message->data + message->size, 0, aligned - message->size);
    message->size = aligned;
    return true;
}

static bool netlink_string_attribute(edge_vpn_netlink_message *message, uint16_t type,
                                     const char *value) {
    return value != NULL && netlink_attribute(message, type, value, strlen(value) + 1U);
}

static bool netlink_begin_nested(edge_vpn_netlink_message *message, uint16_t type,
                                 size_t *offset) {
    if (message == NULL || offset == NULL)
        return false;
    *offset = message->size;
    const struct nlattr attribute = {
        .nla_len = NLA_HDRLEN,
        .nla_type = (uint16_t)(type | NLA_F_NESTED),
    };
    return netlink_append(message, &attribute, sizeof(attribute));
}

static bool netlink_end_nested(edge_vpn_netlink_message *message, size_t offset) {
    if (message == NULL || offset > message->size ||
        message->size - offset > UINT16_MAX || offset + NLA_HDRLEN > message->size)
        return false;
    struct nlattr *attribute = (struct nlattr *)(void *)(message->data + offset);
    attribute->nla_len = (uint16_t)(message->size - offset);
    const size_t aligned = align4(message->size);
    if (aligned > sizeof(message->data))
        return false;
    memset(message->data + message->size, 0, aligned - message->size);
    message->size = aligned;
    return true;
}

static bool netlink_finish(edge_vpn_netlink_message *message) {
    if (message == NULL || message->size < NLMSG_HDRLEN || message->size > UINT32_MAX)
        return false;
    ((struct nlmsghdr *)(void *)message->data)->nlmsg_len = (uint32_t)message->size;
    return true;
}

static bool netlink_for_each_attribute(const uint8_t *data, size_t size,
                                       bool (*handler)(uint16_t, const void *, size_t,
                                                       void *),
                                       void *context) {
    if (data == NULL || handler == NULL)
        return false;
    size_t offset = 0U;
    while (offset + NLA_HDRLEN <= size) {
        const struct nlattr *attribute =
            (const struct nlattr *)(const void *)(data + offset);
        if (attribute->nla_len < NLA_HDRLEN || offset + attribute->nla_len > size)
            return false;
        if (!handler((uint16_t)(attribute->nla_type & NLA_TYPE_MASK),
                     data + offset + NLA_HDRLEN,
                     attribute->nla_len - NLA_HDRLEN, context))
            return false;
        offset += align4(attribute->nla_len);
    }
    return offset == size || (offset < size && size - offset < NLA_HDRLEN);
}

static bool netlink_request(int protocol, edge_vpn_netlink_message *message,
                            edge_vpn_netlink_handler handler, void *context) {
    if (!netlink_finish(message))
        return false;
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return false;
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
        close(fd);
        return false;
    }
    const struct sockaddr_nl destination = {.nl_family = AF_NETLINK};
    const struct iovec iov = {.iov_base = message->data, .iov_len = message->size};
    const struct msghdr outgoing = {
        .msg_name = (void *)&destination,
        .msg_namelen = sizeof(destination),
        .msg_iov = (struct iovec *)(void *)&iov,
        .msg_iovlen = 1U,
    };
    if (sendmsg(fd, &outgoing, 0) < 0) {
        close(fd);
        return false;
    }
    const struct nlmsghdr *request_header =
        (const struct nlmsghdr *)(const void *)message->data;
    const uint32_t sequence = request_header->nlmsg_seq;
    const bool dump = (request_header->nlmsg_flags & NLM_F_DUMP) != 0U;
    uint8_t buffer[EDGE_VPN_NETLINK_BUFFER_SIZE];
    for (;;) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0) {
            close(fd);
            return false;
        }
        int remaining = (int)received;
        for (struct nlmsghdr *header = (struct nlmsghdr *)(void *)buffer;
             NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
            if (header->nlmsg_seq != sequence)
                continue;
            if (header->nlmsg_type == NLMSG_ERROR) {
                if (header->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                    close(fd);
                    return false;
                }
                const struct nlmsgerr *error =
                    (const struct nlmsgerr *)NLMSG_DATA(header);
                if (error->error != 0) {
                    close(fd);
                    return false;
                }
                if (!dump) {
                    close(fd);
                    return true;
                }
                continue;
            }
            if (header->nlmsg_type == NLMSG_DONE) {
                close(fd);
                return true;
            }
            if (handler != NULL && !handler(header, context)) {
                close(fd);
                return false;
            }
        }
    }
}

static bool family_attribute(uint16_t type, const void *data, size_t size, void *context) {
    if (type == CTRL_ATTR_FAMILY_ID && data != NULL && size >= sizeof(uint16_t))
        memcpy(context, data, sizeof(uint16_t));
    return true;
}

static bool family_message(const struct nlmsghdr *header, void *context) {
    if (header == NULL || header->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
        return false;
    const uint8_t *payload = (const uint8_t *)NLMSG_DATA(header) + GENL_HDRLEN;
    const size_t size = header->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    return netlink_for_each_attribute(payload, size, family_attribute, context);
}

static bool wireguard_family(uint16_t *family) {
    if (family == NULL)
        return false;
    *family = 0U;
    edge_vpn_netlink_message message;
    const struct genlmsghdr generic = {
        .cmd = CTRL_CMD_GETFAMILY,
        .version = 1U,
    };
    if (!netlink_begin(&message, GENL_ID_CTRL, NLM_F_REQUEST | NLM_F_ACK, 1U) ||
        !netlink_append(&message, &generic, sizeof(generic)) ||
        !netlink_string_attribute(&message, CTRL_ATTR_FAMILY_NAME, WG_GENL_NAME) ||
        !netlink_request(NETLINK_GENERIC, &message, family_message, family))
        return false;
    return *family != 0U;
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

static bool wireguard_begin(edge_vpn_netlink_message *message, uint16_t family,
                            uint8_t command) {
    const struct genlmsghdr generic = {
        .cmd = command,
        .version = WG_GENL_VERSION,
    };
    const uint16_t flags =
        (uint16_t)(NLM_F_REQUEST | NLM_F_ACK |
                   (command == WG_CMD_GET_DEVICE ? NLM_F_DUMP : 0U));
    return netlink_begin(message, family, flags, 1U) &&
           netlink_append(message, &generic, sizeof(generic)) &&
           netlink_string_attribute(message, WGDEVICE_A_IFNAME, EDGE_VPN_INTERFACE);
}

static bool set_private_key(const char private_key[65]) {
    uint8_t key[32];
    uint16_t family = 0U;
    edge_vpn_netlink_message message;
    if (!hex_to_bytes(private_key, key, sizeof(key)) || !wireguard_family(&family) ||
        !wireguard_begin(&message, family, WG_CMD_SET_DEVICE) ||
        !netlink_attribute(&message, WGDEVICE_A_PRIVATE_KEY, key, sizeof(key)))
        return false;
    return netlink_request(NETLINK_GENERIC, &message, NULL, NULL);
}

typedef struct {
    uint8_t key[32];
    bool found;
} edge_vpn_public_key;

static bool public_key_attribute(uint16_t type, const void *data, size_t size,
                                 void *context) {
    edge_vpn_public_key *result = context;
    if (type == WGDEVICE_A_PUBLIC_KEY && data != NULL && size == sizeof(result->key)) {
        memcpy(result->key, data, sizeof(result->key));
        result->found = true;
    }
    return true;
}

static bool public_key_message(const struct nlmsghdr *header, void *context) {
    if (header == NULL || header->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
        return false;
    const uint8_t *payload = (const uint8_t *)NLMSG_DATA(header) + GENL_HDRLEN;
    const size_t size = header->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    return netlink_for_each_attribute(payload, size, public_key_attribute, context);
}

static bool read_public_key(char output[65]) {
    uint16_t family = 0U;
    edge_vpn_netlink_message message;
    edge_vpn_public_key result = {0};
    if (!wireguard_family(&family) ||
        !wireguard_begin(&message, family, WG_CMD_GET_DEVICE) ||
        !netlink_request(NETLINK_GENERIC, &message, public_key_message, &result) ||
        !result.found)
        return false;
    return base64_encode_key(result.key, (char *)output);
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
                             struct sockaddr_storage *output, socklen_t *output_size) {
    if (output == NULL || output_size == NULL)
        return false;
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
        if ((address->ai_family != AF_INET && address->ai_family != AF_INET6) ||
            address->ai_addrlen > sizeof(*output))
            continue;
        memset(output, 0, sizeof(*output));
        memcpy(output, address->ai_addr, address->ai_addrlen);
        *output_size = (socklen_t)address->ai_addrlen;
        resolved = true;
    }
    freeaddrinfo(addresses);
    return resolved;
}

static bool netlink_set_config(const char private_key[65], const char *hub_public_key,
                               const struct sockaddr *endpoint, socklen_t endpoint_size) {
    uint8_t private_key_bytes[32];
    uint8_t hub_key[32];
    uint16_t family = 0U;
    edge_vpn_netlink_message message;
    if (!hex_to_bytes(private_key, private_key_bytes, sizeof(private_key_bytes)) ||
        !base64_decode_key(hub_public_key, hub_key) || endpoint == NULL ||
        (endpoint->sa_family != AF_INET && endpoint->sa_family != AF_INET6) ||
        (endpoint_size != sizeof(struct sockaddr_in) &&
         endpoint_size != sizeof(struct sockaddr_in6)) ||
        !wireguard_family(&family) ||
        !wireguard_begin(&message, family, WG_CMD_SET_DEVICE) ||
        !netlink_attribute(&message, WGDEVICE_A_PRIVATE_KEY,
                           private_key_bytes, sizeof(private_key_bytes)))
        return false;
    const uint32_t device_flags = WGDEVICE_F_REPLACE_PEERS;
    const uint32_t peer_flags = WGPEER_F_REPLACE_ALLOWEDIPS;
    const uint16_t keepalive = 25U;
    const uint16_t address_family = AF_INET;
    const uint8_t prefix = 11U;
    struct in_addr allowed_address;
    size_t peers_offset = 0U;
    size_t peer_offset = 0U;
    size_t allowed_ips_offset = 0U;
    size_t allowed_ip_offset = 0U;
    if (inet_pton(AF_INET, "100.96.0.0", &allowed_address) != 1 ||
        !netlink_attribute(&message, WGDEVICE_A_FLAGS,
                           &device_flags, sizeof(device_flags)) ||
        !netlink_begin_nested(&message, WGDEVICE_A_PEERS, &peers_offset) ||
        !netlink_begin_nested(&message, 0U, &peer_offset) ||
        !netlink_attribute(&message, WGPEER_A_PUBLIC_KEY, hub_key, sizeof(hub_key)) ||
        !netlink_attribute(&message, WGPEER_A_ENDPOINT, endpoint, endpoint_size) ||
        !netlink_attribute(&message, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL,
                           &keepalive, sizeof(keepalive)) ||
        !netlink_attribute(&message, WGPEER_A_FLAGS, &peer_flags, sizeof(peer_flags)) ||
        !netlink_begin_nested(&message, WGPEER_A_ALLOWEDIPS, &allowed_ips_offset) ||
        !netlink_begin_nested(&message, 0U, &allowed_ip_offset) ||
        !netlink_attribute(&message, WGALLOWEDIP_A_FAMILY,
                           &address_family, sizeof(address_family)) ||
        !netlink_attribute(&message, WGALLOWEDIP_A_IPADDR,
                           &allowed_address, sizeof(allowed_address)) ||
        !netlink_attribute(&message, WGALLOWEDIP_A_CIDR_MASK, &prefix, sizeof(prefix)) ||
        !netlink_end_nested(&message, allowed_ip_offset) ||
        !netlink_end_nested(&message, allowed_ips_offset) ||
        !netlink_end_nested(&message, peer_offset) ||
        !netlink_end_nested(&message, peers_offset))
        return false;
    return netlink_request(NETLINK_GENERIC, &message, NULL, NULL);
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

static bool run_ip_overlay_route(void) {
    const char *const command[] = {"ip", "route", "replace", EDGE_VPN_OVERLAY_CIDR,
                                   "dev", EDGE_VPN_INTERFACE, NULL};
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
    struct sockaddr_storage endpoint;
    socklen_t endpoint_size = 0;
    if (!resolve_endpoint(request->hub_endpoint, request->hub_listen_port,
                          &endpoint, &endpoint_size)) {
        set_error(error, error_size, "cannot resolve VPN hub endpoint");
        return false;
    }
    char private_key[65];
    if (!ensure_interface(private_key) ||
        !netlink_set_config(private_key, request->hub_public_key,
                            (const struct sockaddr *)&endpoint, endpoint_size) ||
        !run_ip_address(edge_address) || !run_ip_up() || !run_ip_overlay_route() ||
        !apply_firewall(request)) {
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
