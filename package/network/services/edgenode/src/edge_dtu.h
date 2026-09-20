#pragma once

#include "edge.pb.h"

#define EDGE_DTU_MAX_CHANNELS 8U
#define EDGE_DTU_MAX_CLIENTS 16U

typedef bool (*edge_dtu_status_callback)(void *context, const iot_edge_v1_DtuStatus *status);

/* Runs one channel in its owning child process until terminated. */
void edge_dtu_run(const iot_edge_v1_DtuConfig *config,
                  edge_dtu_status_callback callback, void *context);
