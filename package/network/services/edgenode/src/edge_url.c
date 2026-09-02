#include "edge_url.h"

#include <stdio.h>
#include <string.h>

static bool split_http_scheme(const char *value, const char **host,
                              const char **websocket_scheme) {
    if (value == NULL || host == NULL)
        return false;
    if (strncmp(value, "https://", 8U) == 0) {
        *host = value + 8U;
        if (websocket_scheme != NULL)
            *websocket_scheme = "wss";
        return true;
    }
    if (strncmp(value, "http://", 7U) == 0) {
        *host = value + 7U;
        if (websocket_scheme != NULL)
            *websocket_scheme = "ws";
        return true;
    }
    return false;
}

bool edge_url_valid_platform_base(const char *value) {
    const char *host = NULL;
    if (!split_http_scheme(value, &host, NULL) || host[0] == '\0' || host[0] == '/')
        return false;

    bool trailing_slashes = false;
    for (const unsigned char *cursor = (const unsigned char *)host;
         *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            trailing_slashes = true;
            continue;
        }
        if (trailing_slashes || *cursor <= 0x20U || *cursor == 0x7fU ||
            *cursor == '@' || *cursor == '?' || *cursor == '#' || *cursor == '\\')
            return false;
    }
    return true;
}

bool edge_url_make_platform_transport(const char *base, char *output,
                                      size_t capacity) {
    if (output == NULL || capacity == 0U || !edge_url_valid_platform_base(base))
        return false;

    const char *host = NULL;
    const char *scheme = NULL;
    if (!split_http_scheme(base, &host, &scheme))
        return false;
    size_t host_size = strlen(host);
    while (host_size != 0U && host[host_size - 1U] == '/')
        --host_size;
    const int size = snprintf(output, capacity, "%s://%.*s/edge/v1/connect",
                              scheme, (int)host_size, host);
    return host_size != 0U && size > 0 && (size_t)size < capacity;
}
