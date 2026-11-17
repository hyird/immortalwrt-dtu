#define _GNU_SOURCE
#include "edge_config.h"
#include "edge_network.h"
#include "edge_process.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <uci.h>

static char directory[] = "/tmp/edge-gateway-config-XXXXXX";
static char savedir[256];
static bool fail_pending, corrupt_backup, fail_restore_commit, restoring;
static int detach_result = -1, reload_result;

static const char *sandbox_path(const char *path, char output[512]) {
    const char *prefix = "/tmp/edgenode/";
    if (strncmp(path, prefix, strlen(prefix)))
        return path;
    const int length = snprintf(output, 512, "%s/%s", directory, path + strlen(prefix));
    assert(length > 0 && length < 512);
    return output;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    if (fail_pending && !strcmp(path, "/tmp/edgenode/network.pending") &&
        (flags & O_CREAT)) {
        errno = EACCES;
        return -1;
    }
    char mapped[512];
    return __real_open(sandbox_path(path, mapped), flags, mode);
}

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    char mapped[512];
    if (!strcmp(path, "/tmp/edgenode/network.backup") && !strcmp(mode, "r"))
        restoring = true;
    if (corrupt_backup && !strcmp(path, "/tmp/edgenode/network.backup") &&
        !strcmp(mode, "r")) {
        snprintf(mapped, sizeof(mapped), "%s/malformed-backup", directory);
        return __real_fopen(mapped, mode);
    }
    return __real_fopen(sandbox_path(path, mapped), mode);
}

int __real_access(const char *path, int mode);
int __wrap_access(const char *path, int mode) {
    char mapped[512];
    return __real_access(sandbox_path(path, mapped), mode);
}

int __real_unlink(const char *path);
int __wrap_unlink(const char *path) {
    char mapped[512];
    return __real_unlink(sandbox_path(path, mapped));
}

int __wrap_edge_process_run(const char *const argv[], int input, int output) {
    assert(!strcmp(argv[0], "/etc/init.d/network") &&
           !strcmp(argv[1], "reload") && !argv[2] && input == -1 && output == -1);
    char path[512];
    snprintf(path, sizeof(path), "%s/reloads", directory);
    FILE *file = __real_fopen(path, "a");
    assert(file && fputc('R', file) == 'R' && !fclose(file));
    return reload_result;
}

int __wrap_edge_process_detach(void) { return detach_result; }
void __wrap_edge_process_close_inherited_fds(int keep) { assert(keep == -1); }
unsigned __wrap_sleep(unsigned seconds) { assert(seconds == 1U); return 0; }

/* Link-time wrapping changes only the test's real libuci configuration root.
 * Production keeps its normal /etc/config path and has no test switches. */
struct uci_context *__real_uci_alloc_context(void);
struct uci_context *__wrap_uci_alloc_context(void) {
    struct uci_context *context = __real_uci_alloc_context();
    if (context) {
        assert(uci_set_confdir(context, directory) == UCI_OK);
        assert(uci_set_conf2dir(context, NULL) == UCI_OK);
        /* set_savedir adds a path; it does not remove the host's default
         * delta directory. Remove that default before installing ours. */
        while (!uci_list_empty(&context->delta_path)) {
            struct uci_element *entry = list_to_element(context->delta_path.next);
            entry->list.next->prev = entry->list.prev;
            entry->list.prev->next = entry->list.next;
            free(entry);
        }
        assert(uci_set_savedir(context, savedir) == UCI_OK);
        if (fail_restore_commit && restoring) {
            char missing[512];
            snprintf(missing, sizeof(missing), "%s/missing", directory);
            assert(uci_set_confdir(context, missing) == UCI_OK);
        }
    }
    return context;
}

static void config_file(const char *name, const char *contents) {
    char path[256];
    assert(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
    FILE *file = fopen(path, "w");
    assert(file && fputs(contents, file) >= 0 && fclose(file) == 0);
}

static void node_file(const char *unit) {
    char contents[512];
    snprintf(contents, sizeof(contents),
        "config node 'node'\n option model 'test'\n option modbus_gateway '0'\n"
        " option modbus_gateway_interface 'br-lan'\n%s", unit);
    config_file("edgenode", contents);
}

static char *network_snapshot(void) {
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    char *text = NULL;
    size_t size = 0;
    FILE *output = open_memstream(&text, &size);
    assert(context && output && uci_load(context, "network", &package) == UCI_OK);
    assert(uci_export(context, output, package, true) == UCI_OK && !fclose(output));
    uci_unload(context, package);
    uci_free_context(context);
    return text;
}

static void expect_network(const char *expected) {
    char *actual = network_snapshot();
    assert(!strcmp(actual, expected));
    free(actual);
}

static bool fixture_exists(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    return access(path, F_OK) == 0;
}

static unsigned reload_count(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/reloads", directory);
    FILE *file = fopen(path, "r");
    unsigned count = 0;
    if (file) {
        while (fgetc(file) != EOF)
            ++count;
        assert(!ferror(file) && !fclose(file));
    }
    return count;
}

static void reset_network(void) {
    fail_pending = corrupt_backup = fail_restore_commit = restoring = false;
    detach_result = -1;
    reload_result = 0;
    const char *files[] = {"network.backup", "network.pending", "network.confirmed",
        "network.rolled-back", "reloads", "delta/network", "malformed-backup"};
    char path[512];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        assert(!unlink(path) || errno == ENOENT);
    }
    config_file("network",
        "config interface 'lan'\n option proto 'static'\n option device 'eth-test'\n"
        " option ipaddr '192.0.2.1'\n list dns '192.0.2.53'\n list dns '192.0.2.54'\n"
        "config interface 'guest'\n option proto 'dhcp'\n option device 'guest-test'\n"
        "config route\n option interface 'lan'\n option target '198.51.100.0/24'\n");
    config_file("malformed-backup", "config interface 'unterminated\n");
}

static iot_edge_v1_NetworkConfigRequest network_request(void) {
    iot_edge_v1_NetworkConfigRequest request = iot_edge_v1_NetworkConfigRequest_init_zero;
    request.request_id.size = 16U;
    memset(request.request_id.bytes, 0x42, 16U);
    request.rollback_timeout_sec = 30U;
    request.interfaces_count = 2U;
    request.interfaces[0].operation =
        iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_DELETE;
    strcpy(request.interfaces[0].logical_name, "guest");
    request.interfaces[1].operation =
        iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT;
    strcpy(request.interfaces[1].logical_name, "newtest");
    strcpy(request.interfaces[1].device, "lo");
    request.interfaces[1].mode = iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_DHCP;
    return request;
}

static void prepare_network(const iot_edge_v1_NetworkConfigRequest *request) {
    char error[257] = {0};
    assert(edge_network_prepare(request, "protected-test", error, sizeof(error)));
    assert(fixture_exists("network.backup"));
}

static void timeout_worker(const uint8_t platform[16], const uint8_t request[16],
                           int expected_status) {
    detach_result = 0;
    const pid_t worker = fork();
    assert(worker >= 0);
    if (!worker) {
        (void)edge_network_activate(30U, platform, request);
        _exit(99);
    }
    int status = 0;
    assert(waitpid(worker, &status, 0) == worker);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected_status);
}

static void network_rollback_checks(void) {
    const uint8_t platform[16] = {1, 2, 3};
    uint8_t owner[16], request_id[16];
    char error[257] = {0};
    iot_edge_v1_NetworkConfigRequest request = network_request();
    reset_network();
    char *baseline = network_snapshot();
    /* A partial in-memory prepare failure must restore the whole package. */
    strcpy(request.interfaces[1].previous_logical_name, "missing");
    assert(!edge_network_prepare(&request, "protected-test", error, sizeof(error)));
    expect_network(baseline);
    assert(reload_count() == 1U && !fixture_exists("network.backup"));
    for (unsigned failure = 0; failure < 3U; ++failure) {
        reset_network();
        corrupt_backup = failure == 0U;
        fail_restore_commit = failure == 1U;
        reload_result = failure == 2U ? 7 : 0;
        assert(!edge_network_prepare(&request, "protected-test", error, sizeof(error)));
        fail_restore_commit = false;
        assert(strstr(error, "rollback failed") && fixture_exists("network.backup"));
        assert(reload_count() == (failure == 2U ? 1U : 0U));
        corrupt_backup = false;
        assert(!edge_network_prepare(&request, "protected-test", error, sizeof(error)));
        assert(strstr(error, "requires recovery") && fixture_exists("network.backup"));
    }
    request.interfaces[1].previous_logical_name[0] = '\0';
    /* Both synchronous activation failure paths must commit before reloading.
     * Saved deltas from a failed transaction must not survive restoration. */
    for (unsigned pending_failure = 0; pending_failure < 2U; ++pending_failure) {
        reset_network();
        prepare_network(&request);
        config_file("delta/network",
            "network.lan.proto='dhcp'\nnetwork.injected='interface'\n");
        fail_pending = pending_failure != 0U;
        assert(!edge_network_activate(30U, platform, request.request_id.bytes));
        expect_network(baseline);
        assert(reload_count() == 1U && !edge_network_pending());
        assert(!fixture_exists("network.backup") &&
               !edge_network_take_rollback(owner, request_id));
    }
    /* Import, actual libuci commit I/O, and reload failures retain recovery
     * material and never claim a successful rollback. */
    for (unsigned failure = 0; failure < 3U; ++failure) {
        for (unsigned pending_failure = 0; pending_failure < 2U; ++pending_failure) {
            reset_network();
            prepare_network(&request);
            fail_pending = pending_failure != 0U;
            corrupt_backup = failure == 0U;
            fail_restore_commit = failure == 1U;
            reload_result = failure == 2U ? 7 : 0;
            assert(!edge_network_activate(30U, platform, request.request_id.bytes));
            fail_restore_commit = false;
            assert(fixture_exists("network.backup"));
            assert(reload_count() == (failure == 2U ? 1U : 0U));
            assert(edge_network_pending() == !pending_failure);
            assert(!edge_network_take_rollback(owner, request_id));
            assert(!edge_network_prepare(&request, "protected-test", error, sizeof(error)));
        }
    }
    reset_network();
    prepare_network(&request);
    timeout_worker(platform, request.request_id.bytes, 0);
    expect_network(baseline);
    assert(reload_count() == 2U && !edge_network_pending());
    assert(!fixture_exists("network.backup"));
    assert(edge_network_take_rollback(owner, request_id));
    assert(!memcmp(owner, platform, 16U) &&
           !memcmp(request_id, request.request_id.bytes, 16U));
    assert(!edge_network_take_rollback(owner, request_id));
    for (unsigned failure = 0; failure < 3U; ++failure) {
        reset_network();
        prepare_network(&request);
        corrupt_backup = failure == 0U;
        fail_restore_commit = failure == 1U;
        reload_result = failure == 2U ? 7 : 0;
        timeout_worker(platform, request.request_id.bytes, 1);
        fail_restore_commit = false;
        assert(edge_network_pending() && fixture_exists("network.backup"));
        assert(reload_count() == (failure == 2U ? 2U : 1U));
        assert(!edge_network_take_rollback(owner, request_id));
        if (failure == 2U)
            expect_network(baseline);
    }
    reset_network();
    char path[512];
    snprintf(path, sizeof(path), "%s/malformed-backup", directory);
    assert(!unlink(path));
    free(baseline);
}

int main(void) {
    assert(mkdtemp(directory));
    snprintf(savedir, sizeof(savedir), "%s/delta", directory);
    assert(!mkdir(savedir, 0700));
    const char *network =
        "config interface 'lan'\n option proto 'static'\n option type 'bridge'\n option ifname 'eth0 eth1'\n"
        "config interface 'guest'\n option proto 'dhcp'\n option device 'eth2'\n";
    config_file("network", network);
    char device[33], logical[33], error[257] = {0};
    assert(edge_network_gateway_resolve("lan", false, device, error, sizeof(error)));
    assert(!strcmp(device, "br-lan"));
    assert(edge_network_gateway_resolve("guest", false, device, error, sizeof(error)));
    assert(!strcmp(device, "eth2"));
    /* Disabled settings use the same configured-identity validation: no live
     * device is required, but physical and unknown logical names are rejected. */
    assert(!edge_network_gateway_resolve("br-lan", false, device, error, sizeof(error)));
    assert(!edge_network_gateway_resolve("eth0", false, device, error, sizeof(error)));
    assert(!edge_network_gateway_resolve("missing", false, device, error, sizeof(error)));
    assert(!edge_network_gateway_resolve("loopback", false, device, error, sizeof(error)));
    assert(edge_network_gateway_migrate("br-lan", logical, error, sizeof(error)));
    assert(!strcmp(logical, "lan"));
    assert(edge_network_gateway_migrate("lan", logical, error, sizeof(error)));
    assert(!strcmp(logical, "lan"));
    assert(!edge_network_gateway_migrate("unknown", logical, error, sizeof(error)));
    assert(!logical[0] && strstr(error, "no configured"));
    node_file("");
    edge_app_config config, loaded;
    assert(edge_config_load(&config, error, sizeof(error)));
    assert(!config.modbus_gateway && config.modbus_gateway_unit_id == 1);
    assert(edge_network_gateway_migrate(config.modbus_gateway_interface, logical, error, sizeof(error)));
    strcpy(config.modbus_gateway_interface, logical);
    config.modbus_gateway_unit_id = 247;
    assert(edge_config_save_modbus_gateway(&config));
    assert(edge_config_load(&loaded, error, sizeof(error)));
    assert(!loaded.modbus_gateway && loaded.modbus_gateway_port == 502 &&
        loaded.modbus_gateway_unit_id == 247 && !strcmp(loaded.modbus_gateway_interface, "lan"));
    config.modbus_gateway_unit_id = 0;
    assert(!edge_config_save_modbus_gateway(&config));
    assert(edge_config_load(&loaded, error, sizeof(error)) && loaded.modbus_gateway_unit_id == 247);
    const char *invalid[] = {"0", "248", "256", "4294967295", "-1", "garbage"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char unit[128];
        snprintf(unit, sizeof(unit), " option modbus_gateway_unit_id '%s'\n", invalid[i]);
        node_file(unit);
        assert(!edge_config_load(&loaded, error, sizeof(error)));
        assert(strstr(error, "between 1 and 247"));
    }
    config_file("network",
        "config interface 'lan'\n option proto 'static'\n option device 'br-lan'\n"
        "config interface 'other'\n option proto 'dhcp'\n option device 'br-lan'\n");
    assert(!edge_network_gateway_migrate("br-lan", logical, error, sizeof(error)));
    assert(!logical[0] && strstr(error, "ambiguous"));
    /* Logical deletion/rename cannot silently select another interface. */
    config_file("network", "config interface 'renamed'\n option proto 'static'\n option device 'br-lan'\n");
    assert(!edge_network_gateway_resolve("lan", false, device, error, sizeof(error)));
    config_file("network", "config interface 'lan'\n option proto 'static'\n option device 'new-device'\n");
    assert(edge_network_gateway_resolve("lan", false, device, error, sizeof(error)));
    assert(!strcmp(device, "new-device"));
    network_rollback_checks();
    char path[256];
    snprintf(path, sizeof(path), "%s/network", directory); assert(!unlink(path));
    snprintf(path, sizeof(path), "%s/edgenode", directory); assert(!unlink(path));
    snprintf(path, sizeof(path), "%s/delta/edgenode", directory);
    assert(!unlink(path) || errno == ENOENT);
    assert(!rmdir(savedir));
    assert(!rmdir(directory));
    puts("Modbus gateway UCI configuration checks passed");
    return 0;
}
