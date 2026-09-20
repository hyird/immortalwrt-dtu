#include "edge_dtu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool congested_status(void *context, const iot_edge_v1_DtuStatus *status) {
    (void)context;
    (void)status;
    return false;
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    iot_edge_v1_DtuConfig config = iot_edge_v1_DtuConfig_init_zero;
    config.channel_id.size = 16;
    config.enabled = true;
    config.max_clients = 3;
    config.queue_bytes = 4096;
    config.north_port = (uint32_t)atoi(argv[1]);
    snprintf(config.north_host, sizeof(config.north_host), "127.0.0.1");
    snprintf(config.south_host, sizeof(config.south_host), "127.0.0.1");
    config.south_port = (uint32_t)atoi(argv[2]);
    config.south_mode = !strcmp(argv[3], "serial") ? iot_edge_v1_LinkMode_LINK_MODE_SERIAL :
        !strcmp(argv[3], "client") ? iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT :
        iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    config.uplink_only = !strcmp(argv[4], "uplink");
    config.debug_enabled = !strcmp(argv[4], "blocked-debug");
    config.registration.size = 4;
    memcpy(config.registration.bytes, "\0REG", 4);
    if (!strncmp(argv[4], "heartbeat", 9)) {
        config.heartbeat.size = 4;
        memcpy(config.heartbeat.bytes, "\0HB\xff", 4);
        config.heartbeat_interval_sec = !strcmp(argv[4], "heartbeat") ? 1 : 0;
    }
    if (config.south_mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL) {
        config.has_serial = true;
        snprintf(config.serial.channel, sizeof(config.serial.channel), "%s", argv[2]);
        config.serial.baud_rate = 9600;
        config.serial.data_bits = 8;
        config.serial.stop_bits = 1;
        snprintf(config.serial.parity, sizeof(config.serial.parity), "none");
        config.serial_frame_ms = 20;
    }
    edge_dtu_run(&config, config.debug_enabled ? congested_status : NULL, NULL);
    return 1;
}
