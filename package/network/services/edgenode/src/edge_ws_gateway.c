#include "edge_ws_internal.h"

void edge_ws_export_gateway(edge_ws_app *app) {
    if (!app->gateway) {
        remove("/tmp/edgenode-modbus.csv");
        return;
    }
    if (app->gateway && !edge_modbus_gateway_export(app->gateway, "/tmp/edgenode-modbus.csv")) {
        remove("/tmp/edgenode-modbus.csv");
        syslog(LOG_ERR, "cannot export Modbus gateway register map");
    }
}

void edge_ws_acquisition_sample(void *context, const uint8_t platform[16],
                                const edge_acquisition_sample *sample) {
    edge_ws_app *app = context;
    edge_modbus_gateway_update(app->gateway, platform, sample, monotonic_ms());
    if (sample == NULL)
        edge_ws_gateway_commands_stopped(app);
}

edge_modbus_gateway *edge_ws_prepare_gateway(
    const edge_acquisition_source *sources, size_t count, uint16_t port) {
    for (size_t s = 0; s < count; ++s) {
        const edge_runtime_config *config = sources[s].config;
        for (size_t i = 0; i < config->item_count; ++i) {
            const iot_edge_v1_ConfigItem *item = &config->items[i];
            if ((item->which_item == iot_edge_v1_ConfigItem_endpoint_tag &&
                 item->item.endpoint.enabled &&
                 item->item.endpoint.mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
                 item->item.endpoint.port == port) ||
                (item->which_item == iot_edge_v1_ConfigItem_dtu_tag &&
                 item->item.dtu.enabled &&
                 item->item.dtu.south_mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
                 item->item.dtu.south_port == port))
                return NULL;
        }
    }
    return edge_modbus_gateway_prepare(EDGE_GATEWAY_MAP_PATH, sources, count);
}

uint8_t edge_ws_gateway_write(void *context,
                              const edge_modbus_gateway_write *write,
                              uint8_t command_id[16], uint64_t deadline_ms) {
    edge_ws_app *app = context;
    const uint64_t now = monotonic_ms();
    if (now == 0U || now >= deadline_ms)
        return 4U;
    edge_ws_gateway_command *pending = NULL;
    for (size_t i = 0U; i < 8U; ++i) {
        /* A rebound listener can still have an old real command outstanding.
         * Do not merge another partial write with its pre-readback cache. */
        if (app->gateway_commands[i].active)
            return 6U;
        if (!pending)
            pending = &app->gateway_commands[i];
    }
    if (!random_bytes(command_id, 16U))
        return 4U;
    iot_edge_v1_CommandRequest request = iot_edge_v1_CommandRequest_init_zero;
    request.command_id.size = request.device_id.size = 16U;
    memcpy(request.command_id.bytes, command_id, 16U);
    memcpy(request.device_id.bytes, write->device_id, 16U);
    request.values_count = 1U;
    safe_copy(request.values[0].element_id, sizeof(request.values[0].element_id),
              write->element_id);
    request.values[0].has_expected = true;
    request.values[0].expected.kind = iot_edge_v1_ValueKind_VALUE_STRING;
    request.values[0].expected.which_value =
        iot_edge_v1_ScalarValue_string_value_tag;
    safe_copy(request.values[0].expected.value.string_value,
              sizeof(request.values[0].expected.value.string_value), write->value);
    request.external_io = write->external_io;
    const uint64_t remaining = deadline_ms - now;
    request.timeout_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    request.readback_count = 1U;
    request.has_start_before_ms = true;
    /* This is a trusted local monotonic deadline, not a remote DB timestamp.
     * The public acquisition API still stamps/checks the current generation. */
    request.start_before_ms = 1;
    pending->active = true;
    pending->external_io = write->external_io;
    memcpy(pending->command_id, command_id, 16U);
    memcpy(pending->platform_id, write->platform_id, 16U);
    memcpy(pending->device_id, write->device_id, 16U);
    char error[256] = {0};
    if (!edge_acquisition_command_for_platform_until(
            app->acquisition, write->platform_id, &request, deadline_ms,
            deadline_ms, error, sizeof(error))) {
        pending->active = false;
        return 6U;
    }
    return 0U;
}

bool edge_ws_consume_gateway_result(edge_ws_app *app,
                                    const uint8_t platform_id[16],
                                    const iot_edge_v1_CommandResult *result) {
    if (!result || result->command_id.size != 16U || result->device_id.size != 16U)
        return false;
    for (size_t i = 0U; i < 8U; ++i) {
        edge_ws_gateway_command *pending = &app->gateway_commands[i];
        if (!pending->active ||
            memcmp(pending->command_id, result->command_id.bytes, 16U) ||
            memcmp(pending->platform_id, platform_id, 16U) ||
            memcmp(pending->device_id, result->device_id.bytes, 16U))
            continue;
        const bool success =
            result->state == iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
        const bool external_io = pending->external_io;
        pending->active = false; /* Release before completion can submit the next point. */
        if (success && external_io) {
            /* Protocol reads already publish samples while parsing. External IO
             * reports the real output readback here, ahead of its ordinary period. */
            for (pb_size_t value = 0U; value < result->actual_values_count; ++value) {
                const iot_edge_v1_TelemetryValue *actual = &result->actual_values[value];
                edge_acquisition_sample sample = {
                    .valid = actual->has_value,
                    .observed_at_ms = result->completed_at_ms,
                    .value = actual->value,
                };
                memcpy(sample.device_id, result->device_id.bytes, 16U);
                safe_copy(sample.element_id, sizeof(sample.element_id),
                          actual->element_id);
                edge_modbus_gateway_update(app->gateway, platform_id, &sample,
                                           monotonic_ms());
            }
        }
        /* A replaced/destroyed server has no matching random command ID. */
        edge_modbus_gateway_write_result(app->gateway_server,
                                         result->command_id.bytes, success);
        return true;
    }
    return false;
}

void edge_ws_gateway_commands_stopped(edge_ws_app *app) {
    /* Only after the old worker is definitely stopped and its IPC is closed:
     * no further result can arrive, so these IDs may now be retired. */
    for (size_t i = 0U; i < 8U; ++i) {
        edge_ws_gateway_command *pending = &app->gateway_commands[i];
        if (!pending->active)
            continue;
        pending->active = false;
        edge_modbus_gateway_write_result(app->gateway_server, pending->command_id,
                                         false);
    }
}

void edge_ws_gateway_network_changed(void *context, bool available) {
    edge_ws_app *app = context;
    app->gateway_network_available = available;
    char device[33] = {0};
    const bool resolved = available &&
                          edge_network_gateway_resolve(
                              app->config->modbus_gateway_interface, true, device,
                              app->gateway_error, sizeof(app->gateway_error));
    if (!available)
        safe_copy(app->gateway_error, sizeof(app->gateway_error),
                  "network event subscription unavailable; restart required");
    if (resolved && app->config->modbus_gateway &&
        edge_modbus_gateway_listening(app->gateway_server) &&
        !strcmp(device, app->gateway_device)) {
        app->gateway_error[0] = '\0';
        return;
    }
    edge_modbus_gateway_close(app->gateway_server);
    app->gateway_server = NULL;
    app->gateway_device[0] = '\0';
    if (!resolved)
        return;
    app->gateway_error[0] = '\0';
    if (!app->config->modbus_gateway)
        return;
    if (!app->gateway) {
        edge_acquisition_source sources[EDGE_MAX_PLATFORMS];
        const size_t count =
            edge_ws_build_acquisition_sources(app, NULL, NULL, sources);
        app->gateway =
            edge_ws_prepare_gateway(sources, count, app->config->modbus_gateway_port);
        if (!app->gateway) {
            safe_copy(app->gateway_error, sizeof(app->gateway_error),
                      "gateway map unavailable");
            return;
        }
    }
    if (!edge_modbus_gateway_set_unit_id(app->gateway,
                                         app->config->modbus_gateway_unit_id)) {
        safe_copy(app->gateway_error, sizeof(app->gateway_error),
                  "gateway Unit ID must be between 1 and 247");
        return;
    }
    app->gateway_server = edge_modbus_gateway_listen(
        app->loop, app->gateway, device, app->config->modbus_gateway_port);
    if (!app->gateway_server) {
        safe_copy(app->gateway_error, sizeof(app->gateway_error),
                  "cannot bind logical interface L3 device");
        return;
    }
    safe_copy(app->gateway_device, sizeof(app->gateway_device), device);
    app->gateway_error[0] = '\0';
    edge_modbus_gateway_set_writer(app->gateway_server, edge_ws_gateway_write, app);
}

static bool apply_gateway_settings(edge_ws_app *app,
                                   const iot_edge_v1_ModbusGatewaySettings *settings,
                                   char *error, size_t error_size) {
    if (!settings->port || settings->port > 65535U || settings->unit_id > 247U) {
        snprintf(error, error_size, "监听端口或从站 ID 无效（ID 必须为 1..247）");
        return false;
    }
    char device[33] = {0};
    if (!edge_network_gateway_resolve(settings->interface_name, settings->enabled,
                                      device, error, error_size))
        return false;
    if (settings->enabled && !app->gateway_network_available) {
        snprintf(error, error_size, "网络事件监听不可用，请重启服务");
        return false;
    }
    edge_app_config candidate = app->config_storage;
    candidate.modbus_gateway = settings->enabled;
    candidate.modbus_gateway_port = (uint16_t)settings->port;
    candidate.modbus_gateway_unit_id =
        settings->unit_id ? (uint8_t)settings->unit_id : 1U;
    safe_copy(candidate.modbus_gateway_interface,
              sizeof(candidate.modbus_gateway_interface), settings->interface_name);
    const bool reuse = settings->enabled &&
                       edge_modbus_gateway_listening(app->gateway_server) &&
                       app->config->modbus_gateway_port == settings->port &&
                       !strcmp(app->gateway_device, device);
    edge_modbus_gateway *map = NULL;
    edge_modbus_gateway_server *server = NULL;
    if (settings->enabled) {
        edge_acquisition_source sources[EDGE_MAX_PLATFORMS];
        const size_t count =
            edge_ws_build_acquisition_sources(app, NULL, NULL, sources);
        map = edge_ws_prepare_gateway(sources, count, candidate.modbus_gateway_port);
        if (!map) {
            snprintf(error, error_size, "地址映射不可用、容量已满或监听端口冲突");
            return false;
        }
        if (!edge_modbus_gateway_set_unit_id(map,
                                             candidate.modbus_gateway_unit_id)) {
            edge_modbus_gateway_free(map);
            snprintf(error, error_size, "从站 ID 无效");
            return false;
        }
        server = reuse ? app->gateway_server
                       : edge_modbus_gateway_listen(app->loop, map, device,
                                                    candidate.modbus_gateway_port);
        if (!server) {
            edge_modbus_gateway_free(map);
            snprintf(error, error_size, "无法绑定监听接口或端口");
            return false;
        }
        edge_modbus_gateway_set_writer(server, edge_ws_gateway_write, app);
    }
    if (!edge_acquisition_set_gateway_enabled(app->acquisition, settings->enabled)) {
        if (!reuse)
            edge_modbus_gateway_close(server);
        edge_modbus_gateway_free(map);
        snprintf(error, error_size, "采集进程忙，请稍后重试");
        return false;
    }
    if (!edge_config_save_modbus_gateway(&candidate)) {
        if (!edge_acquisition_set_gateway_enabled(app->acquisition,
                                                  app->config->modbus_gateway))
            syslog(LOG_ERR, "cannot restore gateway sampling mode after UCI failure");
        if (!reuse)
            edge_modbus_gateway_close(server);
        edge_modbus_gateway_free(map);
        snprintf(error, error_size, "无法持久保存网关配置");
        return false;
    }
    if (reuse)
        edge_modbus_gateway_replace(server, map);
    else
        edge_modbus_gateway_close(app->gateway_server);
    edge_modbus_gateway_free(app->gateway);
    app->gateway = map;
    app->gateway_server = server;
    app->config_storage = candidate;
    safe_copy(app->gateway_device, sizeof(app->gateway_device),
              settings->enabled ? device : "");
    app->gateway_error[0] = '\0';
    edge_ws_export_gateway(app);
    return true;
}

static bool gateway_points(edge_ws_session *session,
                           const iot_edge_v1_ModbusGatewayRequest *request,
                           iot_edge_v1_ModbusGatewayResult *result) {
    edge_modbus_gateway *loaded = NULL;
    const edge_modbus_gateway *map = session->app->gateway;
    if (!map) {
        loaded = edge_modbus_gateway_load(EDGE_GATEWAY_MAP_PATH);
        map = loaded;
        if (!map) {
            safe_copy(result->message, sizeof(result->message),
                      "无法读取寄存器地址预留表");
            return false;
        }
    }
    if (!edge_modbus_gateway_digest(map, session->config->id,
                                    result->mapping_digest.bytes)) {
        edge_modbus_gateway_free(loaded);
        safe_copy(result->message, sizeof(result->message), "无法计算映射表版本");
        return false;
    }
    result->mapping_digest.size = 32;
    const bool success = edge_modbus_gateway_points(
        map, session->config->id, &session->runtime_config,
        session->app->config->modbus_gateway,
        edge_modbus_gateway_listening(session->app->gateway_server), request,
        result, monotonic_ms());
    edge_modbus_gateway_free(loaded);
    return success;
}

static bool apply_gateway_order(edge_ws_session *session,
                                const iot_edge_v1_ModbusGatewayRequest *request,
                                char *error, size_t error_size) {
    edge_ws_app *app = session->app;
    if (request->has_settings || request->include_points ||
        !app->config->modbus_gateway ||
        !edge_modbus_gateway_listening(app->gateway_server)) {
        snprintf(error, error_size, "请启用网关并单独提交地址排序");
        return false;
    }
    if (!edge_modbus_gateway_reorder(app->gateway, EDGE_GATEWAY_MAP_PATH,
                                     session->config->id, &request->order)) {
        snprintf(error, error_size,
                 "映射已变化、排序无效或保存失败；请重新读取后排序");
        return false;
    }
    edge_modbus_gateway_replace(app->gateway_server, app->gateway);
    edge_ws_export_gateway(app);
    return true;
}

void edge_ws_handle_gateway_request(edge_ws_session *session,
                                    const iot_edge_v1_Envelope *input) {
    const iot_edge_v1_ModbusGatewayRequest request =
        input->payload.modbus_gateway_request;
    if (request.request_id.size != 16U)
        return;
    char error[257] = {0};
    const bool success =
        request.has_order
            ? apply_gateway_order(session, &request, error, sizeof(error))
            : !request.has_settings ||
                  apply_gateway_settings(session->app, &request.settings, error,
                                         sizeof(error));
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!edge_ws_init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_modbus_gateway_result_tag;
    iot_edge_v1_ModbusGatewayResult *result =
        &output->payload.modbus_gateway_result;
    edge_protocol_set_bytes(&result->request_id, sizeof(result->request_id.bytes),
                            request.request_id.bytes, 16U);
    result->success = success;
    result->order_applied = success && request.has_order;
    result->order_supported = true;
    safe_copy(result->message, sizeof(result->message), success ? "ok" : error);
    result->has_settings = true;
    result->settings.enabled = session->app->config->modbus_gateway;
    result->settings.port = session->app->config->modbus_gateway_port;
    result->settings.unit_id = session->app->config->modbus_gateway_unit_id;
    safe_copy(result->settings.interface_name,
              sizeof(result->settings.interface_name),
              session->app->config->modbus_gateway_interface);
    if (success && !request.has_settings && session->app->gateway_error[0])
        safe_copy(result->message, sizeof(result->message),
                  session->app->gateway_error);
    if (result->success && request.include_points)
        result->success = gateway_points(session, &request, result);
    if (!edge_ws_send_envelope(session, output))
        syslog(LOG_WARNING, "Modbus gateway configuration reply was not sent");
}
