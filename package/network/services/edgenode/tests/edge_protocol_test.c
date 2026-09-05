#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_encode.h>

#include "edge_protocol.h"

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge protocol test failed: %s\n", message);
        exit(1);
    }
}

static void test_imei(void) {
    require(edge_protocol_validate_imei("490154203237518"), "valid IMEI rejected");
    require(!edge_protocol_validate_imei("490154203237519"), "bad check digit accepted");
    require(!edge_protocol_validate_imei("49015420323751"), "short IMEI accepted");
    require(!edge_protocol_validate_imei("49015420323751A"), "non-digit IMEI accepted");
}

static void test_hello_round_trip(void) {
    const uint8_t platform_id[16] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t random_bytes[10] = {0x10, 0x11, 0x12, 0x13, 0x14,
                                      0x15, 0x16, 0x17, 0x18, 0x19};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 1U,
                                        1784688000123LL, random_bytes),
            "envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_hello_tag;
    strcpy(envelope.payload.hello.imei, "490154203237518");
    strcpy(envelope.payload.hello.model, "openwrt-test");
    strcpy(envelope.payload.hello.software_version, "0.1.0");
    envelope.payload.hello.supported_protocol_versions_count = 1U;
    envelope.payload.hello.supported_protocol_versions[0] =
        EDGENODE_PROTOCOL_VERSION;
    envelope.payload.hello.supports_firmware_update = true;
    envelope.payload.hello.supports_firmware_stream = true;
    strcpy(envelope.payload.hello.iccid, "89860012345678901234");
    envelope.payload.hello.signal_csq = 23U;
    envelope.payload.hello.signal_rssi_dbm = -67;
    envelope.payload.hello.signal_percent = 74U;
    envelope.payload.hello.mobile_registered = true;
    envelope.payload.hello.mobile_registration_status = 1;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "encode failed");
    require(encoded_size > 0U, "empty encoded envelope");

    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_hello_tag, "wrong payload tag");
    require(strcmp(decoded.payload.hello.imei, "490154203237518") == 0,
            "IMEI changed during round trip");
    require(strcmp(decoded.payload.hello.iccid, "89860012345678901234") == 0,
            "ICCID changed during round trip");
    require(decoded.payload.hello.signal_csq == 23U &&
                decoded.payload.hello.signal_rssi_dbm == -67 &&
                decoded.payload.hello.signal_percent == 74U,
            "signal state changed during round trip");
    require(decoded.payload.hello.mobile_registered &&
                decoded.payload.hello.mobile_registration_status == 1,
            "mobile registration changed during round trip");
    require(decoded.payload.hello.supported_protocol_versions_count == 1U &&
                decoded.payload.hello.supported_protocol_versions[0] ==
                    EDGENODE_PROTOCOL_VERSION,
            "supported protocol advertisement changed during round trip");
    require(decoded.payload.hello.supports_firmware_update &&
                decoded.payload.hello.supports_firmware_stream,
            "firmware WS capability changed during round trip");
    require(decoded.message_id.size == 16U && (decoded.message_id.bytes[6] >> 4U) == 7U,
            "message id is not UUIDv7");
    require((decoded.message_id.bytes[8] & 0xc0U) == 0x80U, "bad UUID variant");
}

static void test_heartbeat_mobile_state_round_trip(void) {
    const uint8_t platform_id[16] = {0x01};
    const uint8_t random_bytes[10] = {0x20};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 2U,
                                        1784688030123LL, random_bytes),
            "heartbeat envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_heartbeat_tag;
    iot_edge_v1_Heartbeat *heartbeat = &envelope.payload.heartbeat;
    heartbeat->uptime_sec = 300U;
    strcpy(heartbeat->iccid, "89860012345678901234");
    heartbeat->signal_csq = 23U;
    heartbeat->signal_rssi_dbm = -67;
    heartbeat->signal_percent = 74U;
    heartbeat->mobile_registered = true;
    heartbeat->mobile_registration_status = 1;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "heartbeat encode failed");

    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "heartbeat decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_heartbeat_tag,
            "wrong heartbeat payload tag");
    const iot_edge_v1_Heartbeat *round_trip = &decoded.payload.heartbeat;
    require(strcmp(round_trip->iccid, "89860012345678901234") == 0,
            "heartbeat ICCID changed during round trip");
    require(round_trip->signal_csq == 23U && round_trip->signal_rssi_dbm == -67 &&
                round_trip->signal_percent == 74U && round_trip->mobile_registered &&
                round_trip->mobile_registration_status == 1,
            "heartbeat mobile state changed during round trip");
}

static void test_terminal_opened_round_trip(void) {
    const uint8_t platform_id[16] = {0x01};
    const uint8_t random_bytes[10] = {0x21};
    const uint8_t terminal_id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 3U,
                                        1784688040123LL, random_bytes),
            "terminal opened envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_terminal_opened_tag;
    require(edge_protocol_set_bytes(&envelope.payload.terminal_opened.terminal_id,
                                    sizeof(envelope.payload.terminal_opened.terminal_id.bytes),
                                    terminal_id, sizeof(terminal_id)),
            "terminal opened id setup failed");

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "terminal opened encode failed");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "terminal opened decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_terminal_opened_tag &&
                decoded.payload.terminal_opened.terminal_id.size == 16U &&
                memcmp(decoded.payload.terminal_opened.terminal_id.bytes, terminal_id, 16U) == 0,
            "terminal opened acknowledgement changed identity");
}

static void test_terminal_flow_control_round_trip(void) {
    const uint8_t platform_id[16] = {0x01};
    const uint8_t random_bytes[10] = {0x22};
    const uint8_t terminal_id[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                     0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
                                     0x1c, 0x1d, 0x1e, 0x1f};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 4U,
                                        1784688050123LL, random_bytes),
            "terminal data envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_terminal_data_tag;
    require(edge_protocol_set_bytes(&envelope.payload.terminal_data.terminal_id,
                                    sizeof(envelope.payload.terminal_data.terminal_id.bytes),
                                    terminal_id, sizeof(terminal_id)) &&
                edge_protocol_set_bytes(&envelope.payload.terminal_data.data,
                                        sizeof(envelope.payload.terminal_data.data.bytes),
                                        (const uint8_t *)"input", 5U),
            "terminal data setup failed");
    envelope.payload.terminal_data.sequence = 7U;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "terminal data encode failed");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error) &&
                decoded.which_payload == iot_edge_v1_Envelope_terminal_data_tag &&
                decoded.payload.terminal_data.sequence == 7U,
            error != NULL ? error : "terminal data sequence changed");

    envelope.which_payload = iot_edge_v1_Envelope_terminal_data_ack_tag;
    memset(&envelope.payload.terminal_data_ack, 0,
           sizeof(envelope.payload.terminal_data_ack));
    require(edge_protocol_set_bytes(
                &envelope.payload.terminal_data_ack.terminal_id,
                sizeof(envelope.payload.terminal_data_ack.terminal_id.bytes),
                terminal_id, sizeof(terminal_id)),
            "terminal acknowledgement id setup failed");
    envelope.payload.terminal_data_ack.sequence = 7U;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error) &&
                edge_protocol_decode(encoded, encoded_size, &decoded, &error) &&
                decoded.which_payload ==
                    iot_edge_v1_Envelope_terminal_data_ack_tag &&
                decoded.payload.terminal_data_ack.sequence == 7U,
            error != NULL ? error : "terminal acknowledgement changed");
}

static void test_modem_profile_round_trip(void) {
    const uint8_t platform_id[16] = {0x02};
    const uint8_t random_bytes[10] = {0x30};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 3U,
                                        1784688060123LL, random_bytes),
            "modem envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_modem_control_request_tag;
    iot_edge_v1_ModemControlRequest *request =
        &envelope.payload.modem_control_request;
    memset(request->request_id.bytes, 0x01, sizeof(request->request_id.bytes));
    request->request_id.size = 16U;
    request->action =
        iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE;
    strcpy(request->apn, "private.mnc001.mcc460.gprs");
    strcpy(request->username, "edge-user");
    strcpy(request->password, "secret");
    request->pdp_type = iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6;
    request->auth_type = iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP;
    strcpy(request->pin_code, "1234");
    request->redial_after_apply = true;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "modem profile encode failed");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "modem profile decode failed");
    const iot_edge_v1_ModemControlRequest *profile =
        &decoded.payload.modem_control_request;
    require(profile->action ==
                iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE &&
                strcmp(profile->apn, "private.mnc001.mcc460.gprs") == 0 &&
                strcmp(profile->username, "edge-user") == 0 &&
                strcmp(profile->password, "secret") == 0 &&
                profile->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6 &&
                profile->auth_type ==
                    iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP &&
                strcmp(profile->pin_code, "1234") == 0 &&
                profile->redial_after_apply,
            "modem profile changed during round trip");
}

static void test_cpp_protobuf_wire_contract(void) {
    iot_edge_v1_NetworkConfigRequest request =
        iot_edge_v1_NetworkConfigRequest_init_zero;
    const uint8_t request_id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    require(edge_protocol_set_bytes(&request.request_id, sizeof(request.request_id.bytes),
                                    request_id, sizeof(request_id)),
            "network request id setup failed");
    request.interfaces_count = 1U;
    iot_edge_v1_NetworkInterfaceConfig *interface = &request.interfaces[0];
    strcpy(interface->name, "eth0");
    interface->mode = iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_DHCP;
    interface->operation =
        iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT;
    strcpy(interface->logical_name, "lan");
    strcpy(interface->device, "eth0");
    strcpy(interface->previous_logical_name, "old");
    request.rollback_timeout_sec = 30U;

    uint8_t encoded[128];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    require(pb_encode(&stream, iot_edge_v1_NetworkConfigRequest_fields, &request),
            PB_GET_ERROR(&stream));
    const uint8_t cpp_protobuf_wire[] = {
        0x0a, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x12, 0x1a, 0x0a, 0x04,
        0x65, 0x74, 0x68, 0x30, 0x10, 0x01, 0x48, 0x01, 0x52, 0x03, 0x6c,
        0x61, 0x6e, 0x5a, 0x04, 0x65, 0x74, 0x68, 0x30, 0x62, 0x03, 0x6f,
        0x6c, 0x64, 0x18, 0x1e,
    };
    require(stream.bytes_written == sizeof(cpp_protobuf_wire) &&
                memcmp(encoded, cpp_protobuf_wire, sizeof(cpp_protobuf_wire)) == 0,
            "nanopb wire differs from the C++ Protobuf golden vector");
}

static void test_cpp_protobuf_config_digest_contract(void) {
    iot_edge_v1_ConfigItem item = iot_edge_v1_ConfigItem_init_zero;
    item.revision = 7U;
    item.kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    item.which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    const uint8_t id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    iot_edge_v1_EndpointConfig *endpoint = &item.item.endpoint;
    require(edge_protocol_set_bytes(&endpoint->endpoint_id,
                                    sizeof(endpoint->endpoint_id.bytes), id, sizeof(id)),
            "config endpoint id setup failed");
    strcpy(endpoint->name, "x");
    endpoint->transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    endpoint->mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    endpoint->protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(endpoint->ip, "1.2.3.4");
    endpoint->port = 502U;
    endpoint->enabled = true;
    strcpy(endpoint->interface_name, "eth0");

    uint8_t encoded[128];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    require(pb_encode(&stream, iot_edge_v1_ConfigItem_fields, &item),
            PB_GET_ERROR(&stream));
    const uint8_t cpp_protobuf_wire[] = {
        0x08, 0x07, 0x18, 0x01, 0x52, 0x2f, 0x0a, 0x10, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x12, 0x01, 0x78, 0x18, 0x01, 0x20, 0x02, 0x28, 0x02,
        0x32, 0x07, 0x31, 0x2e, 0x32, 0x2e, 0x33, 0x2e, 0x34, 0x38, 0xf6,
        0x03, 0x48, 0x01, 0x52, 0x04, 0x65, 0x74, 0x68, 0x30,
    };
    require(stream.bytes_written == sizeof(cpp_protobuf_wire) &&
                memcmp(encoded, cpp_protobuf_wire, sizeof(cpp_protobuf_wire)) == 0,
            "config item wire differs from the C++ Protobuf digest contract");
}

static void test_reject_text_or_oversized_input(void) {
    iot_edge_v1_Envelope decoded;
    const char *error = NULL;
    const uint8_t json[] = "{\"type\":\"hello\"}";
    require(!edge_protocol_decode(json, sizeof(json) - 1U, &decoded, &error),
            "JSON WebSocket body was accepted as protobuf");
    require(!edge_protocol_decode(json, EDGENODE_MAX_WS_MESSAGE + 1U, &decoded, &error),
            "oversized WebSocket body was accepted");
}

static void test_firmware_chunk_round_trip(void) {
    iot_edge_v1_Envelope envelope = iot_edge_v1_Envelope_init_zero;
    uint8_t platform_id[16] = {1U};
    uint8_t random[10] = {2U};
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 1U,
                                        1, random),
            "firmware chunk envelope setup failed");
    envelope.which_payload = iot_edge_v1_Envelope_firmware_chunk_tag;
    iot_edge_v1_FirmwareChunk *chunk = &envelope.payload.firmware_chunk;
    chunk->request_id.size = 16U;
    memset(chunk->request_id.bytes, 3, chunk->request_id.size);
    chunk->offset = 4096U;
    chunk->data.size = 8192U;
    memset(chunk->data.bytes, 0x5a, chunk->data.size);
    chunk->eof = true;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded),
                                 &encoded_size, &error),
            error != NULL ? error : "firmware chunk encode failed");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "firmware chunk decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_firmware_chunk_tag &&
                decoded.payload.firmware_chunk.offset == 4096U &&
                decoded.payload.firmware_chunk.data.size == 8192U &&
                decoded.payload.firmware_chunk.eof,
            "firmware WS chunk changed during round trip");
}

static void test_vpn_config_round_trip(void) {
    iot_edge_v1_Envelope envelope = iot_edge_v1_Envelope_init_zero;
    const uint8_t platform_id[16] = {4U};
    const uint8_t random[10] = {5U};
    const uint8_t request_id[16] = {6U};
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 8U,
                                        1, random),
            "VPN envelope setup failed");
    envelope.which_payload = iot_edge_v1_Envelope_vpn_config_request_tag;
    iot_edge_v1_VpnConfigRequest *request = &envelope.payload.vpn_config_request;
    require(edge_protocol_set_bytes(&request->request_id,
                                    sizeof(request->request_id.bytes), request_id,
                                    sizeof(request_id)),
            "VPN request id setup failed");
    request->config_version = 9U;
    strcpy(request->hub_public_key,
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    strcpy(request->hub_endpoint, "vpn.example.com");
    request->hub_listen_port = 51820U;
    strcpy(request->edge_address, "100.96.0.20/32");
    request->routes_count = 1U;
    strcpy(request->routes[0].route_id, "route-1");
    strcpy(request->routes[0].virtual_cidr, "172.31.10.0/24");
    strcpy(request->routes[0].target_cidr, "192.168.10.0/24");
    strcpy(request->routes[0].mode, "nat");
    strcpy(request->routes[0].nat_mode, "masquerade");
    request->routes[0].enabled = true;
    request->enabled = true;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "VPN request encode failed");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error) &&
                decoded.which_payload == iot_edge_v1_Envelope_vpn_config_request_tag,
            error != NULL ? error : "VPN request decode failed");
    const iot_edge_v1_VpnConfigRequest *round_trip = &decoded.payload.vpn_config_request;
    require(round_trip->request_id.size == 16U && round_trip->config_version == 9U &&
                strcmp(round_trip->hub_endpoint, "vpn.example.com") == 0 &&
                round_trip->routes_count == 1U &&
                strcmp(round_trip->routes[0].virtual_cidr, "172.31.10.0/24") == 0 &&
                strcmp(round_trip->routes[0].nat_mode, "masquerade") == 0 &&
                round_trip->enabled,
            "VPN request changed during round trip");
}

static void test_complete_telemetry(void) {
    const uint8_t platform[16] = {1}, random[10] = {2};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform, NULL, 1, 1, 1, random),
            "telemetry init");
    envelope.which_payload = iot_edge_v1_Envelope_telemetry_batch_tag;
    envelope.payload.telemetry_batch.records_count = 1;
    iot_edge_v1_TelemetryRecord *record = &envelope.payload.telemetry_batch.records[0];
    // More than both the old 16-value boundary and the reported 22-point device.
    record->values_count = 100;
    record->values = calloc(record->values_count, sizeof(*record->values));
    require(record->values != NULL, "telemetry allocation");
    for (pb_size_t i = 0; i < record->values_count; ++i) {
        snprintf(record->values[i].element_id, sizeof(record->values[i].element_id), "point-%u", i);
        record->values[i].has_value = true;
        record->values[i].value.which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
        record->values[i].value.value.unsigned_value = i;
    }
    record->has_device_status = true;
    strcpy(record->device_status.state, "connected");
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE];
    size_t size;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, wire, sizeof(wire), &size, &error), "complete telemetry encode");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(wire, size, &decoded, &error), "complete telemetry decode");
    const iot_edge_v1_TelemetryRecord *actual = &decoded.payload.telemetry_batch.records[0];
    require(decoded.payload.telemetry_batch.records_count == 1 && actual->values_count == 100,
            "one scan must remain one complete record");
    require(actual->values[99].value.value.unsigned_value == 99 && actual->has_device_status &&
            strcmp(actual->device_status.state, "connected") == 0, "telemetry lost values or status");
    // Re-encoding is the reconnect/outbox replay path.
    require(edge_protocol_encode(&decoded, wire, sizeof(wire), &size, &error), "telemetry replay");
    edge_protocol_release(&decoded);
    edge_protocol_release(&envelope);
}

int main(void) {
    test_complete_telemetry();
    test_imei();
    test_hello_round_trip();
    test_heartbeat_mobile_state_round_trip();
    require(EDGENODE_PROTOCOL_VERSION == 6U,
            "firmware streaming did not advance the wire protocol");
    test_terminal_opened_round_trip();
    test_terminal_flow_control_round_trip();
    test_modem_profile_round_trip();
    test_cpp_protobuf_wire_contract();
    test_cpp_protobuf_config_digest_contract();
    test_firmware_chunk_round_trip();
    test_vpn_config_round_trip();
    test_reject_text_or_oversized_input();
    puts("edge protocol tests passed");
    return 0;
}
