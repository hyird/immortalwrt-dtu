#include "edge_vpn.h"
#include "edge_config.h"
#include "edge_version.h"
#include "edge_process.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <uci.h>
#include <unistd.h>

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

static bool load_or_create_private_key(const char *key_path, char output[65]) {
    if (mkdir("/etc/edgenode", 0700) != 0 && errno != EEXIST)
        return false;
    const int existing = open(key_path, O_RDONLY | O_CLOEXEC);
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
    const int created = open(key_path,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (created < 0) {
        if (errno == EEXIST)
            return load_or_create_private_key(key_path, output);
        return false;
    }
    const bool written = write_all(created, key, strlen(key)) && fsync(created) == 0;
    close(created);
    if (!written) {
        unlink(key_path);
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


static int run(const char *const args[]) { return edge_process_run_timeout(args, -1, -1, 15000U); }
static int run_namespace(const edge_vpn_session *session, const char *const args[], int output) {
    const char *command[40] = {"ip", "netns", "exec", session->plan.namespace_name};
    size_t count = 4U;
    for (size_t i = 0U; args[i] != NULL; ++i) {
        if (count >= 39U) return -1;
        command[count++] = args[i];
    }
    command[count] = NULL;
    return edge_process_run_timeout(command, -1, output, 15000U);
}

static bool option(struct uci_context *context, struct uci_package *package,
                    struct uci_section *section, const char *name, const char *value) {
    struct uci_ptr pointer = {.p = package, .s = section, .option = name, .value = value};
    return uci_set(context, &pointer) == UCI_OK;
}

static bool firewall_resource(edge_vpn_session *session, bool enabled) {
    struct uci_context *context = uci_alloc_context(); struct uci_package *package = NULL;
    if (!context || uci_load(context, "firewall", &package) != UCI_OK) {
        if (context) uci_free_context(context);
        return false;
    }
    struct uci_section *lan = NULL; struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        const char *name = uci_lookup_option_string(context, section, "name");
        if (strcmp(section->type, "zone") == 0 && name && strcmp(name, "lan") == 0) { lan = section; break; }
    }
    bool contains = false, success = lan != NULL, changed = enabled;
    if (lan) {
        struct uci_option *devices = uci_lookup_option(context, lan, "device");
        if (devices && devices->type == UCI_TYPE_LIST) {
            uci_foreach_element(&devices->v.list, element) if (strcmp(element->name, session->plan.host_link) == 0) contains = true;
        } else if (devices && devices->type == UCI_TYPE_STRING) contains = strcmp(devices->v.string, session->plan.host_link) == 0;
    }
    if (success && enabled != contains) {
        changed = true;
        struct uci_ptr pointer = {.p = package, .s = lan, .option = "device", .value = session->plan.host_link};
        success = (enabled ? uci_add_list(context, &pointer) : uci_del_list(context, &pointer)) == UCI_OK;
    }
    struct uci_section *include = uci_lookup_section(context, package, session->plan.include_section);
    if (success && !enabled && include) {
        changed = true;
        struct uci_ptr pointer = {.p = package, .s = include}; success = uci_delete(context, &pointer) == UCI_OK;
    }
    if (success && enabled && !include) {
        success = uci_add_section(context, package, "include", &include) == UCI_OK;
        if (success) { struct uci_ptr pointer = {.p = package, .s = include, .value = session->plan.include_section}; success = uci_rename(context, &pointer) == UCI_OK; }
    }
    if (success && enabled) success = option(context, package, include, "type", "nftables") &&
        option(context, package, include, "path", session->plan.root_rules_path) &&
        option(context, package, include, "position", "chain-post") && option(context, package, include, "chain", "srcnat");
    if (success && changed) success = uci_save(context, package) == UCI_OK && uci_commit(context, &package, false) == UCI_OK;
    uci_unload(context, package); uci_free_context(context);
    if (!success || !changed) return success;
    const char *const reload[] = {"/etc/init.d/firewall", "reload", NULL}; return run(reload) == 0;
}

static bool collect_transfer(edge_vpn_session *session) {
    if (!session->enabled) return false;
    char temporary[] = "/tmp/edgenode/vpn-transfer.XXXXXX";
    const int file = mkstemp(temporary); if (file < 0) return false;
    const char *const command[] = {"wg", "show", "wg", "transfer", NULL};
    bool success = run_namespace(session, command, file) == 0;
    char data[192] = {0};
    if (success) { success = lseek(file, 0, SEEK_SET) == 0; if (success) success = read(file, data, sizeof(data) - 1U) > 0; }
    close(file); unlink(temporary);
    char public_key[45]; unsigned long long rx = 0U, tx = 0U;
    if (!success || sscanf(data, "%44s %llu %llu", public_key, &rx, &tx) != 3) return false;
    edge_vpn_traffic *traffic = &session->traffic;
    traffic->download += traffic->primed && rx >= traffic->raw_rx ? rx - traffic->raw_rx : rx;
    traffic->upload += traffic->primed && tx >= traffic->raw_tx ? tx - traffic->raw_tx : tx;
    traffic->raw_rx = rx; traffic->raw_tx = tx; traffic->primed = true;
    return true;
}

bool edge_vpn_init(edge_vpn_session *session, const uint8_t platform_id[16], size_t slot) {
    if (!session) return false;
    memset(session, 0, sizeof(*session));
    if (!edge_vpn_plan_init(&session->plan, platform_id, slot)) return false;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        session->traffic.ack_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return true;
}

static bool managed_id(const char *name, const char *prefix) {
    const size_t prefix_size = strlen(prefix);
    if (strlen(name) != prefix_size + 32U || strncmp(name, prefix, prefix_size) != 0) return false;
    for (size_t i = prefix_size; name[i]; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}

static bool link_exists(const char *name) {
    char path[80]; snprintf(path, sizeof(path), "/sys/class/net/%s", name);
    return access(path, F_OK) == 0;
}

bool edge_vpn_recover_resources(void) {
    // The app owns cleanup on startup, including platforms removed or reordered in UCI.
    DIR *directory = opendir("/var/run/netns");
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) if (managed_id(entry->d_name, "envpn_")) {
            const char *const remove[] = {"ip", "netns", "delete", entry->d_name, NULL};
            if (run(remove) != 0) { closedir(directory); return false; }
        }
        closedir(directory);
    } else if (errno != ENOENT) return false;
    for (size_t slot = 0; slot < EDGE_MAX_PLATFORMS; ++slot) {
        char name[16];
        for (size_t kind = 0; kind < 2U; ++kind) {
            snprintf(name, sizeof(name), "envpn%u%c", (unsigned)slot, kind ? 'w' : 'h');
            if (link_exists(name)) {
                const char *const remove[] = {"ip", "link", "delete", name, NULL};
                if (run(remove) != 0) return false;
            }
        }
    }
    struct uci_context *context = uci_alloc_context(); struct uci_package *package = NULL;
    if (!context) return false;
    bool success = uci_load(context, "network", &package) == UCI_OK, legacy = false;
    if (success) {
        struct uci_section *peer = uci_lookup_section(context, package, "iot_server");
        const char *description = peer ? uci_lookup_option_string(context, peer, "description") : NULL;
        legacy = peer && strcmp(peer->type, "wireguard_wg") == 0 && description && strcmp(description, "IoT VPN Hub") == 0;
        if (legacy) {
            const char *const names[] = {"iot_server", "wg"};
            for (size_t i = 0; success && i < 2U; ++i) {
                struct uci_section *section = uci_lookup_section(context, package, names[i]);
                if (section) { struct uci_ptr pointer = {.p = package, .s = section}; success = uci_delete(context, &pointer) == UCI_OK; }
            }
            if (success) success = uci_save(context, package) == UCI_OK && uci_commit(context, &package, false) == UCI_OK;
        }
        uci_unload(context, package); package = NULL;
    }
    bool changed = false;
    if (success) success = uci_load(context, "firewall", &package) == UCI_OK;
    if (success) {
        struct uci_element *element, *next;
        uci_foreach_element_safe(&package->sections, next, element) {
            struct uci_section *section = uci_to_section(element);
            const char *path = uci_lookup_option_string(context, section, "path");
            if (strcmp(section->type, "include") == 0 &&
                (managed_id(section->e.name, "edgenode_vpn_") ||
                 (legacy && path && (strcmp(path, "/tmp/edgenode/vpn-dstnat.nft") == 0 ||
                    strcmp(path, "/tmp/edgenode/vpn-forward.nft") == 0 || strcmp(path, "/tmp/edgenode/vpn-srcnat.nft") == 0)))) {
                struct uci_ptr pointer = {.p = package, .s = section};
                success = uci_delete(context, &pointer) == UCI_OK; changed = true;
                if (!success) break;
                continue;
            }
            const char *zone = uci_lookup_option_string(context, section, "name");
            if (strcmp(section->type, "zone") != 0 || !zone || strcmp(zone, "lan") != 0) continue;
            for (size_t slot = 0; success && slot < EDGE_MAX_PLATFORMS; ++slot) {
                char name[16]; snprintf(name, sizeof(name), "envpn%uh", (unsigned)slot);
                struct uci_option *devices = uci_lookup_option(context, section, "device");
                bool present = devices && devices->type == UCI_TYPE_STRING && strcmp(devices->v.string, name) == 0;
                if (devices && devices->type == UCI_TYPE_LIST) {
                    struct uci_element *device;
                    uci_foreach_element(&devices->v.list, device) if (strcmp(device->name, name) == 0) present = true;
                }
                if (present) { struct uci_ptr pointer = {.p = package, .s = section, .option = "device", .value = name}; success = uci_del_list(context, &pointer) == UCI_OK; changed = true; }
            }
            if (success && legacy) { struct uci_ptr pointer = {.p = package, .s = section, .option = "network", .value = "wg"}; success = uci_del_list(context, &pointer) == UCI_OK; changed = true; }
        }
        if (success && changed) success = uci_save(context, package) == UCI_OK && uci_commit(context, &package, false) == UCI_OK;
        uci_unload(context, package);
    }
    uci_free_context(context);
    if (success && legacy) {
        // Preserve the legacy private key for rollback; only remove daemon-owned runtime rules.
        unlink("/usr/share/nftables.d/chain-pre/dstnat/30-edgenode-vpn.nft");
        unlink("/usr/share/nftables.d/chain-pre/forward/30-edgenode-vpn.nft");
        unlink("/usr/share/nftables.d/chain-pre/srcnat/30-edgenode-vpn.nft");
        const char *const reload[] = {"ubus", "call", "network", "reload", NULL}; success = run(reload) == 0;
    }
    if (success && changed) { const char *const reload[] = {"/etc/init.d/firewall", "reload", NULL}; success = run(reload) == 0; }
    return success;
}

bool edge_vpn_collect_capability(edge_vpn_session *session, iot_edge_v1_VpnCapabilities *capability) {
    if (!session || !capability || access("/proc/self/ns/net", F_OK) != 0) return false;
    memset(capability, 0, sizeof(*capability));
    char private_hex[65], private_key[45], public_key[45];
    if (!load_or_create_private_key(session->plan.key_path, private_hex) || !private_key_base64(private_hex, private_key) || !read_public_key(private_key, public_key)) return false;
    capability->supports_vpn = true;
    safe_copy(capability->wireguard_version, sizeof(capability->wireguard_version), "kernel-netns");
    safe_copy(capability->agent_version, sizeof(capability->agent_version), EDGE_SOFTWARE_VERSION);
    safe_copy(capability->public_key, sizeof(capability->public_key), public_key);
    return true;
}

bool edge_vpn_apply(edge_vpn_session *session, const iot_edge_v1_VpnConfigRequest *request, char *error, size_t error_size) {
    if (!session || !request || request->request_id.size != 16U || !request->config_version) { set_error(error, error_size, "invalid VPN request identity or version"); return false; }
    if (request->config_version < session->applied_version) { set_error(error, error_size, "stale VPN configuration version"); return false; }
    if (request->config_version == session->applied_version && request->enabled == session->enabled) return true;
    if (!request->enabled) { edge_vpn_shutdown(session); session->applied_version = request->config_version; return true; }
    char rules[8192], private_hex[65], private_key[45], endpoint_host[256], endpoint_port[6], endpoint[272]; uint8_t public_key[32];
    if (!edge_vpn_plan_rules(&session->plan, request, rules, sizeof(rules), error, error_size) ||
        !base64_decode_key(request->hub_public_key, public_key) ||
        !split_endpoint(request->hub_endpoint, request->hub_listen_port, endpoint_host, endpoint_port)) { set_error(error, error_size, "invalid VPN network configuration"); return false; }
    if (!load_or_create_private_key(session->plan.key_path, private_hex) || !private_key_base64(private_hex, private_key) ||
        (mkdir("/tmp/edgenode", 0755) != 0 && errno != EEXIST)) { set_error(error, error_size, "cannot load platform VPN key"); return false; }
    snprintf(endpoint, sizeof(endpoint), strchr(endpoint_host, ':') ? "[%s]:%s" : "%s:%s", endpoint_host, endpoint_port);
    edge_vpn_shutdown(session);
    char secret[] = "/tmp/edgenode/vpn-private.XXXXXX"; const int key_file = mkstemp(secret);
    if (key_file < 0) { set_error(error, error_size, "cannot stage platform VPN key"); return false; }
    bool success = write_all(key_file, private_key, strlen(private_key)); close(key_file);
    char peer_link[16]; safe_copy(peer_link, sizeof(peer_link), session->plan.host_link); peer_link[strlen(peer_link) - 1U] = 'p';
    const char *const namespace_add[] = {"ip", "netns", "add", session->plan.namespace_name, NULL};
    const char *const wg_add[] = {"ip", "link", "add", session->plan.transport_link, "type", "wireguard", NULL};
    const char *const wg_set[] = {"wg", "set", session->plan.transport_link, "private-key", secret, "peer", request->hub_public_key, "endpoint", endpoint, "allowed-ips", "100.96.0.0/11,172.16.0.0/12", "persistent-keepalive", "120", NULL};
    const char *const wg_move[] = {"ip", "link", "set", session->plan.transport_link, "netns", session->plan.namespace_name, NULL};
    const char *const wg_rename[] = {"ip", "link", "set", session->plan.transport_link, "name", "wg", NULL};
    const char *const links[] = {"ip", "link", "add", session->plan.host_link, "type", "veth", "peer", "name", peer_link, NULL};
    const char *const peer_move[] = {"ip", "link", "set", peer_link, "netns", session->plan.namespace_name, NULL};
    const char *const peer_rename[] = {"ip", "link", "set", peer_link, "name", "uplink", NULL};
    const char *const host_address[] = {"ip", "addr", "add", session->plan.host_address, "dev", session->plan.host_link, NULL};
    const char *const host_up[] = {"ip", "link", "set", session->plan.host_link, "up", NULL};
    const char *const peer_address[] = {"ip", "addr", "add", session->plan.peer_address, "dev", "uplink", NULL};
    const char *const peer_up[] = {"ip", "link", "set", "uplink", "up", NULL};
    const char *const wg_address[] = {"ip", "addr", "add", request->edge_address, "dev", "wg", NULL};
    const char *const wg_up[] = {"ip", "link", "set", "wg", "mtu", "1280", "up", NULL};
    const char *const loopback[] = {"ip", "link", "set", "lo", "up", NULL};
    const char *const default_route[] = {"ip", "route", "add", "default", "via", session->plan.host_ip, "dev", "uplink", NULL};
    const char *const overlay[] = {"ip", "route", "add", "100.96.0.0/11", "dev", "wg", NULL};
    const char *const virtual_routes[] = {"ip", "route", "add", "172.16.0.0/12", "dev", "wg", NULL};
    // DNAT targets may themselves be inside another platform's virtual pool.
    // Incoming WG packets always use the LAN transit table after translation.
    const char *const lan_route[] = {"ip", "route", "add", "table", "200", "default", "via", session->plan.host_ip, "dev", "uplink", NULL};
    const char *const lan_rule[] = {"ip", "rule", "add", "priority", "100", "iif", "wg", "lookup", "200", NULL};
    const char *const forwarding[] = {"sysctl", "-q", "-w", "net.ipv4.ip_forward=1", "net.ipv4.conf.all.rp_filter=0", "net.ipv4.conf.uplink.rp_filter=0", "net.ipv4.conf.wg.rp_filter=0", NULL};
    const char *const nft[] = {"nft", "-f", session->plan.rules_path, NULL};
    success = success && run(namespace_add) == 0 && run(wg_add) == 0 && run(wg_set) == 0;
    unlink(secret);
    success = success && run(wg_move) == 0 && run_namespace(session, wg_rename, -1) == 0 && run(links) == 0 && run(peer_move) == 0 &&
        run_namespace(session, peer_rename, -1) == 0 && run(host_address) == 0 && run(host_up) == 0 && run_namespace(session, peer_address, -1) == 0 &&
        run_namespace(session, peer_up, -1) == 0 && run_namespace(session, wg_address, -1) == 0 && run_namespace(session, loopback, -1) == 0 &&
        run_namespace(session, default_route, -1) == 0 && run_namespace(session, overlay, -1) == 0 && run_namespace(session, virtual_routes, -1) == 0 &&
        run_namespace(session, lan_route, -1) == 0 && run_namespace(session, lan_rule, -1) == 0 && run_namespace(session, forwarding, -1) == 0 &&
        write_atomic(session->plan.rules_path, rules) && run_namespace(session, nft, -1) == 0;
    char root_rules[256]; snprintf(root_rules, sizeof(root_rules), "iifname \"%s\" ip saddr %s masquerade\n", session->plan.host_link, session->plan.peer_host);
    // Install restrictions and root return-path translation before allowing WG traffic.
    success = success && write_atomic(session->plan.root_rules_path, root_rules) && firewall_resource(session, true) && run_namespace(session, wg_up, -1) == 0;
    if (!success) { edge_vpn_shutdown(session); set_error(error, error_size, "cannot apply platform-isolated VPN"); return false; }
    session->enabled = true; session->applied_version = request->config_version; session->traffic.primed = false;
    (void)collect_transfer(session);
    return true;
}

void edge_vpn_shutdown(edge_vpn_session *session) {
    if (!session || !session->plan.namespace_name[0]) return;
    (void)collect_transfer(session);
    session->enabled = false; session->traffic.primed = false;
    const char *const namespace_remove[] = {"ip", "netns", "delete", session->plan.namespace_name, NULL};
    const char *const link_remove[] = {"ip", "link", "delete", session->plan.host_link, NULL};
    const char *const transport_remove[] = {"ip", "link", "delete", session->plan.transport_link, NULL};
    char namespace_path[80]; snprintf(namespace_path, sizeof(namespace_path), "/var/run/netns/%s", session->plan.namespace_name);
    if (access(namespace_path, F_OK) == 0) (void)run(namespace_remove);
    if (link_exists(session->plan.host_link)) (void)run(link_remove);
    if (link_exists(session->plan.transport_link)) (void)run(transport_remove);
    (void)firewall_resource(session, false);
    unlink(session->plan.rules_path); unlink(session->plan.root_rules_path);
}

bool edge_vpn_sample(edge_vpn_session *session, uint64_t now_ms, iot_edge_v1_TcpTraffic *report) {
    if (!session || !report) return false;
    (void)collect_transfer(session);
    edge_vpn_traffic *traffic = &session->traffic;
    if (!traffic->ack_ms) traffic->ack_ms = now_ms;
    if (!traffic->pending) {
        if (traffic->upload == traffic->ack_upload && traffic->download == traffic->ack_download) { memset(report, 0, sizeof(*report)); return false; }
        traffic->pending_upload = traffic->upload; traffic->pending_download = traffic->download;
        traffic->report.upload_bytes = traffic->upload - traffic->ack_upload;
        traffic->report.download_bytes = traffic->download - traffic->ack_download;
        traffic->report.interval_ms = now_ms >= traffic->ack_ms ? now_ms - traffic->ack_ms : 0U;
        traffic->report.sample_id = ++traffic->sequence; traffic->pending = true;
    }
    *report = traffic->report; return true;
}
void edge_vpn_ack(edge_vpn_session *session, uint64_t sample_id) {
    if (!session || !session->traffic.pending || !sample_id || sample_id != session->traffic.report.sample_id) return;
    session->traffic.ack_upload = session->traffic.pending_upload; session->traffic.ack_download = session->traffic.pending_download;
    session->traffic.ack_ms += session->traffic.report.interval_ms; session->traffic.pending = false;
}
