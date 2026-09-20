#include "edge_protocol.h"
#include "edge_spool.h"
#include "edge.pb.h"
#include "edge_config.h"
#include "edge_sha256.h"
#include <pb_encode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UUID conversion is the only UCI-owned dependency of the spool under test. */
bool edge_config_parse_uuid(const char *text, uint8_t output[16]) {
    unsigned value;
    for (size_t i = 0, pos = 0; i < 16; ++i) {
        if (text[pos] == '-') ++pos;
        if (sscanf(text + pos, "%2x", &value) != 1) return false;
        output[i] = (uint8_t)value;
        pos += 2;
    }
    return true;
}
void edge_config_format_uuid(const uint8_t value[16], char output[37]) {
    size_t pos = 0;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) output[pos++] = '-';
        snprintf(output + pos, 3, "%02x", value[i]);
        pos += 2;
    }
}
static void require(bool ok, const char *reason) {
    if (!ok) { fprintf(stderr, "%s\n", reason); exit(1); }
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    uint8_t id[16] = {1};
    edge_spool spool;
    require(edge_spool_init(&spool, id, 4096), "spool initialization failed");
    uint64_t revision = strtoull(argv[2], NULL, 10);
    if (!strcmp(argv[1], "read")) {
        require(spool.active_config.revision == revision, "checkpoint revision differs");
        edge_spool_free(&spool);
        return 0;
    }
    const bool empty = !strcmp(argv[1], "empty");
    iot_edge_v1_ConfigItem item = iot_edge_v1_ConfigItem_init_zero;
    item.revision = revision;
    item.kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DTU;
    item.which_item = iot_edge_v1_ConfigItem_dtu_tag;
    item.item.dtu.channel_id.size = 16;
    item.item.dtu.enabled = true;
    item.item.dtu.south_mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER;
    snprintf(item.item.dtu.name, sizeof(item.item.dtu.name), "test");
    snprintf(item.item.dtu.south_host, sizeof(item.item.dtu.south_host), "127.0.0.1");
    snprintf(item.item.dtu.north_host, sizeof(item.item.dtu.north_host), "127.0.0.1");
    item.item.dtu.south_port = 5000;
    item.item.dtu.north_port = 9000;
    item.item.dtu.max_clients = 3;
    item.item.dtu.queue_bytes = 4096;
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE], digest[32];
    pb_ostream_t output = pb_ostream_from_buffer(wire, sizeof(wire));
    require(pb_encode(&output, iot_edge_v1_ConfigItem_fields, &item), "encode failed");
    require(!edge_sha256(wire, output.bytes_written, item.sha256.bytes, 0), "hash failed");
    item.sha256.size = 32;
    require(!edge_sha256(item.sha256.bytes, empty ? 0 : 32, digest, 0), "snapshot hash failed");
    output = pb_ostream_from_buffer(wire, sizeof(wire));
    require(pb_encode(&output, iot_edge_v1_ConfigItem_fields, &item), "encode hash failed");
    require(edge_spool_config_begin(&spool, revision, empty ? 0 : 1, digest), "begin failed");
    if (!empty) require(edge_spool_config_put(&spool, revision, 0, item.sha256.bytes, wire, output.bytes_written), "put failed");
    bool committed = edge_spool_config_commit(&spool, revision, digest);
    require(committed != !strcmp(argv[1], "reject"), "unexpected commit result");
    edge_spool_free(&spool);
    return 0;
}
