#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge_url.h"

static void require_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge URL test failed: %s\n", message);
        exit(1);
    }
}

static void test_platform_base(void) {
    require_true(edge_url_valid_platform_base("http://8.160.189.44:3000"),
                 "HTTP platform URL was rejected");
    require_true(edge_url_valid_platform_base("https://i.a-z.xin"),
                 "HTTPS platform URL was rejected");
    require_true(edge_url_valid_platform_base("http://edge.local///"),
                 "trailing slashes were rejected");
    require_true(!edge_url_valid_platform_base("ws://edge.local"),
                 "WebSocket URL was accepted as a platform base");
    require_true(!edge_url_valid_platform_base("http://edge.local/path"),
                 "platform path was accepted");
    require_true(!edge_url_valid_platform_base("http://user@edge.local"),
                 "platform credentials were accepted");
    require_true(!edge_url_valid_platform_base("http://edge.local?query"),
                 "platform query was accepted");
    require_true(!edge_url_valid_platform_base("http://edge.local bad"),
                 "platform whitespace was accepted");
}

static void test_platform_transport(void) {
    char output[320];
    require_true(edge_url_make_platform_transport("http://8.160.189.44:3000",
                                                  output, sizeof(output)),
                 "HTTP transport URL was not generated");
    require_true(strcmp(output, "ws://8.160.189.44:3000/edge/v1/connect") == 0,
                 "HTTP platform did not map to WS");
    require_true(edge_url_make_platform_transport("https://i.a-z.xin/", output,
                                                  sizeof(output)),
                 "HTTPS transport URL was not generated");
    require_true(strcmp(output, "wss://i.a-z.xin/edge/v1/connect") == 0,
                 "HTTPS platform did not map to WSS");
}

static void test_firmware_resource(void) {
    require_true(edge_url_valid_http_resource(
                     "http://8.160.189.44:3000/edge/v1/firmware/1/download?token=x"),
                 "HTTP firmware URL was rejected");
    require_true(edge_url_valid_http_resource(
                     "https://i.a-z.xin/edge/v1/firmware/1/download?token=x"),
                 "HTTPS firmware URL was rejected");
    require_true(!edge_url_valid_http_resource("ftp://edge.local/firmware.bin"),
                 "unsupported firmware URL scheme was accepted");
    require_true(!edge_url_valid_http_resource("http:///firmware.bin"),
                 "firmware URL without a host was accepted");
    require_true(!edge_url_valid_http_resource("http://user@edge.local/file"),
                 "firmware URL credentials were accepted");
    require_true(!edge_url_valid_http_resource("http://edge.local/file bad"),
                 "firmware URL whitespace was accepted");
}

int main(void) {
    test_platform_base();
    test_platform_transport();
    test_firmware_resource();
    puts("edge URL tests passed");
    return 0;
}
