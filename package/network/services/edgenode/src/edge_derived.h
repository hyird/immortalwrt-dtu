#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "edge_runtime_config.h"

typedef struct edge_derived edge_derived;
bool edge_derived_validate(const iot_edge_v1_DerivedPointConfig *rule);
bool edge_derived_validate_config(const edge_runtime_config *config);
edge_derived *edge_derived_create(const edge_runtime_config *config, const uint8_t device_id[16]);
void edge_derived_free(edge_derived *state);
bool edge_derived_update(edge_derived *state, const iot_edge_v1_TelemetryValue *values, size_t count, int64_t at);
size_t edge_derived_count(const edge_derived *state);
void edge_derived_values(const edge_derived *state, iot_edge_v1_TelemetryValue *output);
int64_t edge_derived_deadline(const edge_derived *state, int64_t now);

/* Same bounded expression grammar as the platform; no script execution. */
typedef bool (*edge_expression_variable)(void *, const char *, double *);
bool edge_expression_evaluate(const char *text, edge_expression_variable variable, void *context, double *result);
