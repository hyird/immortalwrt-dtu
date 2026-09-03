#include "edge_modem.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edge_protocol.h"

#define EDGE_MODEM_CONTROL_TIMEOUT_SEC 65U

static void initialize_info(edge_modem_info *info) {
    memset(info, 0, sizeof(*info));
    info->registration_status = -1;
    info->csq = 99;
    info->rssi_dbm = -1;
    info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_UNKNOWN;
}

bool edge_modem_read_status(const char *path, edge_modem_info *info, bool *available) {
    if (path == NULL || info == NULL || available == NULL)
        return false;
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    initialize_info(info);
    *available = false;
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *newline = strpbrk(line, "\r\n");
        if (newline != NULL)
            *newline = '\0';
        char *separator = strchr(line, '=');
        if (separator == NULL)
            continue;
        *separator++ = '\0';
        if (strcmp(line, "available") == 0)
            *available = strcmp(separator, "1") == 0;
        else if (strcmp(line, "registered") == 0)
            info->registered = strcmp(separator, "1") == 0;
        else if (strcmp(line, "registration_status") == 0)
            info->registration_status = atoi(separator);
        else if (strcmp(line, "imei") == 0 && strlen(separator) < sizeof(info->imei))
            memcpy(info->imei, separator, strlen(separator) + 1U);
        else if (strcmp(line, "iccid") == 0 && strlen(separator) < sizeof(info->iccid))
            memcpy(info->iccid, separator, strlen(separator) + 1U);
        else if (strcmp(line, "csq") == 0)
            info->csq = atoi(separator);
        else if (strcmp(line, "rssi_dbm") == 0)
            info->rssi_dbm = atoi(separator);
        else if (strcmp(line, "signal_percent") == 0)
            info->signal_percent = (unsigned)strtoul(separator, NULL, 10);
        else if (strcmp(line, "sim_state") == 0)
            info->sim_state = (iot_edge_v1_ModemSimState)atoi(separator);
        else if (strcmp(line, "apn") == 0 && strlen(separator) < sizeof(info->apn))
            memcpy(info->apn, separator, strlen(separator) + 1U);
        else if (strcmp(line, "mobile_operator") == 0 &&
                 strlen(separator) < sizeof(info->mobile_operator))
            memcpy(info->mobile_operator, separator, strlen(separator) + 1U);
        else if (strcmp(line, "connected") == 0)
            info->connected = strcmp(separator, "1") == 0;
        else if (strcmp(line, "mobile_ipv4") == 0 &&
                 strlen(separator) < sizeof(info->mobile_ipv4))
            memcpy(info->mobile_ipv4, separator, strlen(separator) + 1U);
    }
    const bool success = !ferror(file);
    fclose(file);
    return success;
}

static bool valid_apn(const char *apn, bool allow_empty) {
    if (apn == NULL || strlen(apn) > 100U)
        return false;
    if (*apn == '\0')
        return allow_empty;
    size_t label_length = 0U;
    for (const unsigned char *cursor = (const unsigned char *)apn;
         *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            if (label_length == 0U || label_length > 63U || cursor[-1] == '-')
                return false;
            label_length = 0U;
            continue;
        }
        if (!(('a' <= *cursor && *cursor <= 'z') ||
              ('A' <= *cursor && *cursor <= 'Z') ||
              ('0' <= *cursor && *cursor <= '9') || *cursor == '-'))
            return false;
        if (label_length == 0U && *cursor == '-')
            return false;
        ++label_length;
    }
    return label_length != 0U && label_length <= 63U && apn[strlen(apn) - 1U] != '-';
}

static bool valid_pin(const char *pin) {
    const size_t length = strlen(pin);
    if (length == 0U)
        return true;
    if (length < 4U || length > 8U)
        return false;
    for (const char *cursor = pin; *cursor != '\0'; ++cursor)
        if (*cursor < '0' || *cursor > '9')
            return false;
    return true;
}

static bool valid_credential(const char *value) {
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor)
        if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == '"')
            return false;
    return true;
}

bool edge_modem_validate_control(const iot_edge_v1_ModemControlRequest *request,
                                 char *message, size_t message_size) {
    if (message != NULL && message_size != 0U)
        message[0] = '\0';
    const iot_edge_v1_ModemControlAction action =
        request != NULL ? request->action
                        : iot_edge_v1_ModemControlAction_MODEM_CONTROL_UNSPECIFIED;
    const bool apply =
        action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE;
    const bool redial = action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_REDIAL;
    const bool auth_none = request != NULL &&
        request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_NONE;
    const bool auth_valid = request != NULL &&
        (auth_none || request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP ||
         request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_CHAP ||
         request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP);
    const bool pdp_valid = request != NULL &&
        (request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4 ||
         request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV6 ||
         request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6);
    if (request == NULL || (!apply && !redial) ||
        (apply &&
         (!pdp_valid || !auth_valid ||
          !valid_apn(request->apn, request->automatic_apn) ||
          (request->automatic_apn && request->apn[0] != '\0') ||
          (!request->automatic_apn && request->apn[0] == '\0') ||
          !valid_pin(request->pin_code) || !valid_credential(request->username) ||
          !valid_credential(request->password) ||
          (auth_none && (request->username[0] != '\0' || request->password[0] != '\0')) ||
          (!auth_none && (request->username[0] == '\0' || request->password[0] == '\0'))))) {
        if (message != NULL && message_size != 0U)
            snprintf(message, message_size, "invalid modem control request");
        return false;
    }
    return true;
}

static bool write_all(int fd, const char *data, size_t size) {
    while (size != 0U) {
        const ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += (size_t)written;
            size -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static void copy_message(char *destination, size_t capacity, const char *source) {
    if (destination == NULL || capacity == 0U)
        return;
    if (source == NULL)
        source = "";
    while (*source == '\r' || *source == '\n' || *source == ' ' || *source == '\t')
        ++source;
    size_t length = strcspn(source, "\r\n");
    if (length >= capacity)
        length = capacity - 1U;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool run_4ginfo_control(const iot_edge_v1_ModemControlRequest *request,
                               char *message, size_t message_size) {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    if (pipe(input) != 0 || pipe(output) != 0) {
        if (input[0] >= 0) {
            close(input[0]);
            close(input[1]);
        }
        if (output[0] >= 0) {
            close(output[0]);
            close(output[1]);
        }
        snprintf(message, message_size, "cannot start 4GINFO control");
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        snprintf(message, message_size, "cannot start 4GINFO control");
        return false;
    }
    if (child == 0) {
        close(input[1]);
        close(output[0]);
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(output[1], STDERR_FILENO) < 0)
            _exit(EXIT_FAILURE);
        close(input[0]);
        close(output[1]);
        (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1)
            _exit(EXIT_FAILURE);
        if (request->action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE)
            execl("/usr/sbin/4ginfo-modemctl", "4ginfo-modemctl", "apply-stdin", NULL);
        else
            execl("/usr/sbin/4ginfo-modemctl", "4ginfo-modemctl", "redial", NULL);
        _exit(EXIT_FAILURE);
    }
    close(input[0]);
    close(output[1]);

    bool input_ok = true;
    if (request->action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE) {
        char payload[768];
        const int length = snprintf(
            payload, sizeof(payload),
            "apn=%s\nautomatic_apn=%u\nusername=%s\npassword=%s\npdp_type=%u\n"
            "auth_type=%u\npin_code=%s\nredial_after_apply=%u\n",
            request->apn, request->automatic_apn ? 1U : 0U, request->username,
            request->password, (unsigned)request->pdp_type, (unsigned)request->auth_type,
            request->pin_code, request->redial_after_apply ? 1U : 0U);
        input_ok = length > 0 && (size_t)length < sizeof(payload) &&
                   write_all(input[1], payload, (size_t)length);
    }
    close(input[1]);

    char output_text[512] = {0};
    size_t used = 0U;
    while (used + 1U < sizeof(output_text)) {
        const ssize_t count = read(output[0], output_text + used,
                                   sizeof(output_text) - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            output_text[used] = '\0';
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    close(output[0]);

    int status = 0;
    bool waited = false;
    for (unsigned elapsed = 0U; elapsed < EDGE_MODEM_CONTROL_TIMEOUT_SEC; ++elapsed) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            waited = true;
            break;
        }
        if (result < 0 && errno != EINTR)
            break;
        sleep(1U);
    }
    if (!waited) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, &status, 0);
    }
    if (!input_ok) {
        snprintf(message, message_size, "cannot send request to 4GINFO");
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS)
        copy_message(message, message_size, output_text[0] != '\0' ? output_text :
                     "4GINFO modem operation succeeded");
    else
        copy_message(message, message_size, output_text[0] != '\0' ? output_text :
                     "4GINFO modem operation failed");
    return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

bool edge_modem_control(const char *port,
                        const iot_edge_v1_ModemControlRequest *request,
                        char *message, size_t message_size) {
    (void)port;
    if (!edge_modem_validate_control(request, message, message_size))
        return false;
    if (access("/usr/sbin/4ginfo-modemctl", X_OK) != 0) {
        snprintf(message, message_size, "4GINFO service is unavailable");
        return false;
    }
    return run_4ginfo_control(request, message, message_size);
}
