#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#endif

static inline int64_t edge_monotonic_ms_signed(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static inline uint64_t edge_monotonic_ms(void) {
    const int64_t value = edge_monotonic_ms_signed();
    return value > 0 ? (uint64_t)value : 0U;
}

static inline int64_t edge_realtime_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static inline void edge_copy_text(char *output, size_t capacity, const char *input) {
    if (output == NULL || capacity == 0U)
        return;
    const char *text = input != NULL ? input : "";
    size_t length = strlen(text);
    if (length >= capacity)
        length = capacity - 1U;
    memmove(output, text, length);
    output[length] = '\0';
}

static inline void edge_set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0U)
        edge_copy_text(error, size, message != NULL ? message : "error");
}

static inline int edge_hex_value(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static inline void edge_bytes_to_hex(const uint8_t *input, size_t size,
                                     char *output, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    if (input == NULL || output == NULL || capacity < size * 2U + 1U)
        return;
    for (size_t index = 0U; index < size; ++index) {
        output[index * 2U] = digits[input[index] >> 4U];
        output[index * 2U + 1U] = digits[input[index] & 0x0fU];
    }
    output[size * 2U] = '\0';
}

static inline bool edge_hex_to_bytes(const char *input, uint8_t *output, size_t size) {
    if (input == NULL || output == NULL)
        return false;
    for (size_t index = 0U; index < size; ++index) {
        const int high = edge_hex_value(input[index * 2U]);
        const int low = edge_hex_value(input[index * 2U + 1U]);
        if (high < 0 || low < 0)
            return false;
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

static inline void edge_format_uuid_bytes(const uint8_t value[16], char output[37]) {
    static const char digits[] = "0123456789abcdef";
    size_t written = 0U;
    for (size_t index = 0U; index < 16U; ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U)
            output[written++] = '-';
        output[written++] = digits[value[index] >> 4U];
        output[written++] = digits[value[index] & 0x0fU];
    }
    output[written] = '\0';
}

static inline bool edge_parse_uuid_bytes(const char *text, uint8_t output[16]) {
    static const size_t hyphens[] = {8U, 13U, 18U, 23U};
    if (text == NULL || output == NULL || strlen(text) != 36U)
        return false;
    for (size_t index = 0U; index < sizeof(hyphens) / sizeof(hyphens[0]); ++index)
        if (text[hyphens[index]] != '-')
            return false;
    size_t byte = 0U;
    for (size_t index = 0U; index < 36U;) {
        if (text[index] == '-') {
            ++index;
            continue;
        }
        if (index + 1U >= 36U || byte >= 16U)
            return false;
        const int high = edge_hex_value(text[index]);
        const int low = edge_hex_value(text[index + 1U]);
        if (high < 0 || low < 0)
            return false;
        output[byte++] = (uint8_t)((high << 4U) | low);
        index += 2U;
    }
    return byte == 16U;
}

#ifndef _WIN32
static inline bool edge_random_bytes(uint8_t *output, size_t size) {
    if (output == NULL)
        return false;
    size_t offset = 0U;
#ifdef __linux__
    while (offset < size) {
        const ssize_t count = getrandom(output + offset, size - offset, 0);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    if (offset == size)
        return true;
#endif
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    while (offset < size) {
        const ssize_t count = read(fd, output + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)count;
    }
    close(fd);
    return true;
}

static inline speed_t edge_serial_baud_speed(uint32_t value) {
    switch (value) {
    case 300U: return B300;
    case 600U: return B600;
    case 1200U: return B1200;
    case 2400U: return B2400;
    case 4800U: return B4800;
    case 9600U: return B9600;
    case 19200U: return B19200;
    case 38400U: return B38400;
#ifdef B57600
    case 57600U: return B57600;
#endif
#ifdef B115200
    case 115200U: return B115200;
#endif
#ifdef B230400
    case 230400U: return B230400;
#endif
#ifdef B460800
    case 460800U: return B460800;
#endif
    default: return (speed_t)0;
    }
}
#endif
