#define _GNU_SOURCE
#include "edge_config.h"
#include "edge_network.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <uci.h>

static char directory[] = "/tmp/edge-gateway-config-XXXXXX";
static char savedir[256];

/* Link-time wrapping changes only the test's real libuci configuration root.
 * Production keeps its normal /etc/config path and has no test switches. */
struct uci_context *__real_uci_alloc_context(void);
struct uci_context *__wrap_uci_alloc_context(void) {
    struct uci_context *context = __real_uci_alloc_context();
    if (context) {
        assert(uci_set_confdir(context, directory) == UCI_OK);
        assert(uci_set_savedir(context, savedir) == UCI_OK);
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
