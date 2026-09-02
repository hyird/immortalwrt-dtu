#include "edge_firmware.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "edge_firmware_policy.h"
#include "edge_firmware_stream.h"
#include "edge_process.h"
#include "edge_sha256.h"

#define FIRMWARE_IMAGE "/tmp/edgenode/firmware.bin"
#define FIRMWARE_LOCK "/tmp/edgenode/firmware.lock"
#define OVERLAY_BINARY "/overlay/upper/usr/sbin/edgenode"
#define OVERLAY_BINARY_BACKUP "/tmp/edgenode/edgenode-overlay-backup"
#define FIRMWARE_STATUS_MAGIC 0x45444745U
#define FIRMWARE_SYSUPGRADE_HANDOFF_GRACE_MS 120000U
#define FIRMWARE_CHUNK_RETRY_MS 5000U
#define FIRMWARE_TRANSFER_TIMEOUT_MS 1800000U
#define FIRMWARE_MAX_SIZE (128U * 1024U * 1024U)

typedef struct {
    uint32_t magic;
    uint8_t request_id[16];
    uint32_t state;
    uint64_t downloaded_bytes;
    uint64_t total_bytes;
    uint32_t progress_percent;
    char message[257];
} firmware_status;

typedef struct {
    bool active;
    int image_fd;
    int lock_fd;
    uint8_t platform_id[16];
    iot_edge_v1_FirmwareUpdateRequest request;
    uint64_t offset;
    uint64_t last_request_ms;
    uint64_t last_progress_ms;
} firmware_transfer;

static firmware_transfer transfer = {
    .active = false,
    .image_fd = -1,
    .lock_fd = -1,
};

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "firmware error");
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void wait_for_sysupgrade_handoff(void) {
    const uint64_t deadline =
        monotonic_milliseconds() + FIRMWARE_SYSUPGRADE_HANDOFF_GRACE_MS;
    while (monotonic_milliseconds() < deadline) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
}

static void status_path(const uint8_t platform_id[16], char output[96]) {
    static const char hex[] = "0123456789abcdef";
    size_t offset = (size_t)snprintf(output, 96U, "/tmp/edgenode/firmware-");
    for (size_t index = 0; index < 16U && offset + 2U < 96U; ++index) {
        output[offset++] = hex[platform_id[index] >> 4U];
        output[offset++] = hex[platform_id[index] & 0x0FU];
    }
    snprintf(output + offset, 96U - offset, ".status");
}

static bool write_status(const uint8_t platform_id[16], const uint8_t request_id[16],
                          iot_edge_v1_FirmwareUpdateState state, const char *message,
                          uint64_t downloaded_bytes, uint64_t total_bytes) {
    char path[96];
    char temporary[104];
    status_path(platform_id, path);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    firmware_status value;
    memset(&value, 0, sizeof(value));
    value.magic = FIRMWARE_STATUS_MAGIC;
    memcpy(value.request_id, request_id, sizeof(value.request_id));
    value.state = (uint32_t)state;
    value.downloaded_bytes = downloaded_bytes;
    value.total_bytes = total_bytes;
    value.progress_percent =
        total_bytes == 0U
            ? 0U
            : (uint32_t)(downloaded_bytes >= total_bytes
                             ? 100U
                             : (downloaded_bytes * 100U) / total_bytes);
    snprintf(value.message, sizeof(value.message), "%s", message != NULL ? message : "");
    const int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const ssize_t written = write(output, &value, sizeof(value));
    const bool ok = written == (ssize_t)sizeof(value) && fsync(output) == 0;
    close(output);
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static bool sha256_file(const char *path, uint8_t output[32]) {
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = edge_sha256_starts(&context, 0) == 0;
    uint8_t buffer[4096];
    while (ok) {
        const size_t size = fread(buffer, 1U, sizeof(buffer), input);
        if (size != 0U && edge_sha256_update(&context, buffer, size) != 0)
            ok = false;
        if (size < sizeof(buffer)) {
            if (ferror(input) != 0)
                ok = false;
            break;
        }
    }
    if (ok)
        ok = edge_sha256_finish(&context, output) == 0;
    mbedtls_sha256_free(&context);
    fclose(input);
    return ok;
}

static bool copy_file(const char *source, const char *destination, mode_t mode) {
    const int input = open(source, O_RDONLY);
    if (input < 0)
        return errno == ENOENT;
    const int output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (output < 0) {
        close(input);
        return false;
    }
    bool ok = true;
    uint8_t buffer[4096];
    while (ok) {
        const ssize_t size = read(input, buffer, sizeof(buffer));
        if (size == 0)
            break;
        if (size < 0) {
            ok = errno == EINTR;
            continue;
        }
        ssize_t offset = 0;
        while (offset < size) {
            const ssize_t written =
                write(output, buffer + offset, (size_t)(size - offset));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                ok = false;
                break;
            }
            offset += written;
        }
    }
    if (ok)
        ok = fsync(output) == 0;
    close(output);
    close(input);
    if (!ok)
        unlink(destination);
    return ok;
}

static bool hide_overlay_binary(void) {
    unlink(OVERLAY_BINARY_BACKUP);
    if (access(OVERLAY_BINARY, F_OK) != 0)
        return errno == ENOENT;
    if (!copy_file(OVERLAY_BINARY, OVERLAY_BINARY_BACKUP, 0700))
        return false;
    if (unlink(OVERLAY_BINARY) == 0)
        return true;
    unlink(OVERLAY_BINARY_BACKUP);
    return false;
}

static void restore_overlay_binary(void) {
    if (access(OVERLAY_BINARY_BACKUP, F_OK) != 0)
        return;
    if (copy_file(OVERLAY_BINARY_BACKUP, OVERLAY_BINARY, 0755))
        unlink(OVERLAY_BINARY_BACKUP);
}

static void firmware_child(const uint8_t platform_id[16],
                           const iot_edge_v1_FirmwareUpdateRequest *request,
                           int lock_fd) {
    const uint8_t *request_id = request->request_id.bytes;
    struct stat info;
    if (stat(FIRMWARE_IMAGE, &info) != 0 || info.st_size < 0 ||
        (uint64_t)info.st_size != request->size_bytes) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware size mismatch", request->size_bytes,
                           request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        close(lock_fd);
        _exit(1);
    }
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_VERIFYING,
                       "verifying firmware sha256", request->size_bytes,
                       request->size_bytes);
    uint8_t actual[32];
    if (!sha256_file(FIRMWARE_IMAGE, actual) ||
        memcmp(actual, request->sha256.bytes, sizeof(actual)) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware sha256 mismatch", request->size_bytes,
                           request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    const char *validate[] = {"sysupgrade", "-T", FIRMWARE_IMAGE, NULL};
    if (edge_process_run(validate, -1, -1) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "sysupgrade rejected firmware during validation",
                           request->size_bytes, request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    if (request->keep_settings && !hide_overlay_binary()) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "cannot prepare overlay binary replacement",
                           request->size_bytes, request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FLASHING,
                       "firmware verified; sysupgrade is starting", request->size_bytes,
                       request->size_bytes);
    sleep(2U);
    const char *upgrade_keep[] = {"sysupgrade", "-v", FIRMWARE_IMAGE, NULL};
    const char *upgrade_clean[] = {"sysupgrade", "-v", "-n", FIRMWARE_IMAGE, NULL};
    const int upgrade_status = edge_process_run_timeout(
        request->keep_settings ? upgrade_keep : upgrade_clean, -1, -1, 300000U);
    if (edge_firmware_sysupgrade_may_have_handed_off(upgrade_status)) {
        /*
         * A successful OpenWrt handoff replaces procd and disconnects the ubus
         * client that sysupgrade is waiting on. The sysupgrade wrapper may
         * therefore return or be killed by stage2 before stage2 opens the image.
         * Keep both the image and the hidden overlay binary in place until stage2
         * kills this old process during the normal reboot path. Only exec failure
         * proves that no handoff could have started; otherwise the grace period
         * expires before the ordinary failure cleanup below.
         */
        wait_for_sysupgrade_handoff();
    }
    if (request->keep_settings)
        restore_overlay_binary();
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                       upgrade_status == 127
                           ? "sysupgrade command not found"
                           : "sysupgrade handoff did not reboot the device",
                       request->size_bytes, request->size_bytes);
    unlink(FIRMWARE_IMAGE);
    close(lock_fd);
    _exit(1);
}

bool edge_firmware_start(const uint8_t platform_id[16],
                         const iot_edge_v1_FirmwareUpdateRequest *request,
                         char *error, size_t error_size) {
    if (platform_id == NULL || request == NULL || request->request_id.size != 16U ||
        request->sha256.size != 32U || request->size_bytes == 0U ||
        request->size_bytes > FIRMWARE_MAX_SIZE || request->download_url[0] != '\0') {
        set_error(error, error_size, "firmware request is invalid");
        return false;
    }
    if (transfer.active) {
        if (memcmp(transfer.platform_id, platform_id, 16U) == 0 &&
            memcmp(transfer.request.request_id.bytes, request->request_id.bytes, 16U) == 0 &&
            memcmp(transfer.request.sha256.bytes, request->sha256.bytes, 32U) == 0 &&
            transfer.request.size_bytes == request->size_bytes &&
            transfer.request.keep_settings == request->keep_settings) {
            set_error(error, error_size, "firmware WS transfer resumed");
            return true;
        }
        set_error(error, error_size, "another firmware update is active");
        return false;
    }
    const int lock = open(FIRMWARE_LOCK, O_WRONLY | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0)
            close(lock);
        set_error(error, error_size, "another firmware update is active");
        return false;
    }
    unlink(FIRMWARE_IMAGE);
    const int image = open(FIRMWARE_IMAGE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (image < 0) {
        (void)flock(lock, LOCK_UN);
        close(lock);
        set_error(error, error_size, "cannot create firmware image");
        return false;
    }
    memset(&transfer, 0, sizeof(transfer));
    transfer.active = true;
    transfer.image_fd = image;
    transfer.lock_fd = lock;
    memcpy(transfer.platform_id, platform_id, 16U);
    transfer.request = *request;
    transfer.last_progress_ms = monotonic_milliseconds();
    if (!write_status(platform_id, request->request_id.bytes,
                      iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_DOWNLOADING,
                      "receiving firmware over WebSocket", 0U, request->size_bytes)) {
        close(image);
        (void)flock(lock, LOCK_UN);
        close(lock);
        unlink(FIRMWARE_IMAGE);
        memset(&transfer, 0, sizeof(transfer));
        transfer.image_fd = -1;
        transfer.lock_fd = -1;
        set_error(error, error_size, "cannot persist firmware transfer state");
        return false;
    }
    set_error(error, error_size, "firmware WS transfer initialized");
    return true;
}

static void clear_transfer(bool remove_image) {
    if (transfer.image_fd >= 0)
        close(transfer.image_fd);
    if (transfer.lock_fd >= 0) {
        (void)flock(transfer.lock_fd, LOCK_UN);
        close(transfer.lock_fd);
    }
    if (remove_image)
        unlink(FIRMWARE_IMAGE);
    memset(&transfer, 0, sizeof(transfer));
    transfer.image_fd = -1;
    transfer.lock_fd = -1;
}

static edge_firmware_chunk_result fail_transfer(const char *message,
                                                char *error,
                                                size_t error_size) {
    if (transfer.active) {
        (void)write_status(
            transfer.platform_id, transfer.request.request_id.bytes,
            iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
            message, transfer.offset, transfer.request.size_bytes);
    }
    set_error(error, error_size, message);
    clear_transfer(true);
    return EDGE_FIRMWARE_CHUNK_FAILED;
}

bool edge_firmware_receiving(const uint8_t platform_id[16]) {
    return platform_id != NULL && transfer.active &&
           memcmp(transfer.platform_id, platform_id, 16U) == 0;
}

bool edge_firmware_chunk_request(const uint8_t platform_id[16],
                                 iot_edge_v1_FirmwareChunkRequest *request,
                                 bool force) {
    if (!edge_firmware_receiving(platform_id) || request == NULL)
        return false;
    const uint64_t now = monotonic_milliseconds();
    if (now != 0U && transfer.last_progress_ms != 0U &&
        now >= transfer.last_progress_ms &&
        now - transfer.last_progress_ms >= FIRMWARE_TRANSFER_TIMEOUT_MS) {
        (void)fail_transfer("firmware WS transfer timed out", NULL, 0U);
        return false;
    }
    if (!force && transfer.last_request_ms != 0U && now >= transfer.last_request_ms &&
        now - transfer.last_request_ms < FIRMWARE_CHUNK_RETRY_MS)
        return false;
    memset(request, 0, sizeof(*request));
    request->request_id.size = 16U;
    memcpy(request->request_id.bytes, transfer.request.request_id.bytes, 16U);
    request->offset = transfer.offset;
    transfer.last_request_ms = now;
    return true;
}

edge_firmware_chunk_result edge_firmware_receive_chunk(
    const uint8_t platform_id[16], const iot_edge_v1_FirmwareChunk *chunk,
    char *error, size_t error_size) {
    if (!edge_firmware_receiving(platform_id)) {
        set_error(error, error_size, "no firmware WS transfer is receiving");
        return EDGE_FIRMWARE_CHUNK_FAILED;
    }
    if (chunk == NULL || chunk->request_id.size != 16U ||
        memcmp(chunk->request_id.bytes, transfer.request.request_id.bytes, 16U) != 0)
        return fail_transfer("firmware WS chunk identity mismatch", error, error_size);
    if (chunk->error[0] != '\0')
        return fail_transfer(chunk->error, error, error_size);
    const edge_firmware_stream_decision decision = edge_firmware_stream_evaluate(
        transfer.offset, transfer.request.size_bytes, chunk->offset,
        chunk->data.size, sizeof(chunk->data.bytes), chunk->eof);
    if (decision == EDGE_FIRMWARE_STREAM_INVALID)
        return fail_transfer("firmware WS chunk bounds are invalid", error, error_size);
    if (decision == EDGE_FIRMWARE_STREAM_DUPLICATE) {
        set_error(error, error_size, "duplicate firmware WS chunk ignored");
        return EDGE_FIRMWARE_CHUNK_NEXT;
    }
    if (decision == EDGE_FIRMWARE_STREAM_GAP) {
        set_error(error, error_size, "firmware WS chunk gap; retrying");
        return EDGE_FIRMWARE_CHUNK_NEXT;
    }
    const uint64_t next_offset = transfer.offset + chunk->data.size;

    size_t written_total = 0U;
    while (written_total < chunk->data.size) {
        const ssize_t written = pwrite(
            transfer.image_fd, chunk->data.bytes + written_total,
            chunk->data.size - written_total,
            (off_t)(transfer.offset + written_total));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return fail_transfer("cannot write firmware WS chunk", error, error_size);
        written_total += (size_t)written;
    }
    transfer.offset = next_offset;
    transfer.last_progress_ms = monotonic_milliseconds();
    (void)write_status(
        transfer.platform_id, transfer.request.request_id.bytes,
        iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_DOWNLOADING,
        "receiving firmware over WebSocket", transfer.offset,
        transfer.request.size_bytes);
    if (decision == EDGE_FIRMWARE_STREAM_ACCEPT) {
        set_error(error, error_size, "firmware WS chunk accepted");
        return EDGE_FIRMWARE_CHUNK_NEXT;
    }
    if (fsync(transfer.image_fd) != 0)
        return fail_transfer("cannot flush firmware image", error, error_size);
    close(transfer.image_fd);
    transfer.image_fd = -1;
    (void)write_status(
        transfer.platform_id, transfer.request.request_id.bytes,
        iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_VERIFYING,
        "verifying firmware sha256", transfer.offset, transfer.request.size_bytes);

    const int lock = transfer.lock_fd;
    const iot_edge_v1_FirmwareUpdateRequest request = transfer.request;
    uint8_t worker_platform_id[16];
    memcpy(worker_platform_id, transfer.platform_id, sizeof(worker_platform_id));
    const int worker = edge_process_detach();
    if (worker < 0)
        return fail_transfer("cannot start firmware verification worker", error, error_size);
    if (worker == 0) {
        edge_process_close_inherited_fds(lock);
        firmware_child(worker_platform_id, &request, lock);
    }
    close(lock);
    memset(&transfer, 0, sizeof(transfer));
    transfer.image_fd = -1;
    transfer.lock_fd = -1;
    set_error(error, error_size, "firmware WS transfer completed");
    return EDGE_FIRMWARE_CHUNK_COMPLETE;
}

bool edge_firmware_read_status(const uint8_t platform_id[16],
                               iot_edge_v1_FirmwareUpdateResult *result) {
    if (platform_id == NULL || result == NULL)
        return false;
    char path[96];
    status_path(platform_id, path);
    const int input = open(path, O_RDONLY);
    if (input < 0)
        return false;
    firmware_status value;
    const ssize_t size = read(input, &value, sizeof(value));
    close(input);
    if (size != (ssize_t)sizeof(value) || value.magic != FIRMWARE_STATUS_MAGIC)
        return false;
    unlink(path);
    memset(result, 0, sizeof(*result));
    result->request_id.size = 16U;
    memcpy(result->request_id.bytes, value.request_id, 16U);
    result->state = (iot_edge_v1_FirmwareUpdateState)value.state;
    result->downloaded_bytes = value.downloaded_bytes;
    result->total_bytes = value.total_bytes;
    result->progress_percent = value.progress_percent;
    snprintf(result->message, sizeof(result->message), "%s", value.message);
    return true;
}

bool edge_firmware_active(void) {
    if (transfer.active)
        return true;
    const int lock = open(FIRMWARE_LOCK, O_WRONLY | O_CREAT, 0600);
    if (lock < 0)
        return true;
    const bool active = flock(lock, LOCK_EX | LOCK_NB) != 0;
    if (!active)
        (void)flock(lock, LOCK_UN);
    close(lock);
    return active;
}

bool edge_firmware_has_status(const uint8_t platform_id[16]) {
    if (platform_id == NULL)
        return false;
    char path[96];
    status_path(platform_id, path);
    return access(path, F_OK) == 0;
}
