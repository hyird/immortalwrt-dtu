#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "edge.pb.h"

typedef struct {
    char imei[16];
    char iccid[23];
    char apn[101];
    char mobile_operator[65];
    char mobile_ipv4[16];
    int registration_status;
    int csq;
    int rssi_dbm;
    unsigned signal_percent;
    iot_edge_v1_ModemSimState sim_state;
    bool registered;
    bool connected;
} edge_modem_info;

bool edge_modem_read_status(const char *path, edge_modem_info *info, bool *available);
bool edge_modem_validate_control(const iot_edge_v1_ModemControlRequest *request,
                                 char *message, size_t message_size);
bool edge_modem_control(const char *port,
                        const iot_edge_v1_ModemControlRequest *request,
                        char *message, size_t message_size);
