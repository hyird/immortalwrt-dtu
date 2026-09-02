#pragma once

#include <stdbool.h>
#include <stddef.h>

bool edge_url_valid_platform_base(const char *value);
bool edge_url_make_platform_transport(const char *base, char *output,
                                      size_t capacity);
