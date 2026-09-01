#include "edge_terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edge_capability.h"
#include "edge_config.h"
#include "edge_process.h"

typedef struct {
    bool active;
    int master;
    pid_t child;
    uint8_t id[16];
    uint8_t pending_input[4096];
    size_t pending_input_size;
    size_t pending_input_offset;
    uint64_t pending_input_sequence;
    uint64_t last_input_sequence;
} terminal_state;

static terminal_state terminals[EDGE_MAX_PLATFORMS];

static void terminate_and_reap(pid_t child) {
    if (child <= 0)
        return;
    (void)kill(child, SIGHUP);
    for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
        pid_t waited;
        do {
            waited = waitpid(child, NULL, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == child || (waited < 0 && errno == ECHILD))
            return;
        usleep(5000U);
    }
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message);
}

static terminal_state *find_terminal(const uint8_t terminal_id[16]) {
    if (terminal_id == NULL)
        return NULL;
    for (size_t index = 0U; index < EDGE_MAX_PLATFORMS; ++index)
        if (terminals[index].active &&
            memcmp(terminals[index].id, terminal_id, 16U) == 0)
            return &terminals[index];
    return NULL;
}

static terminal_state *terminal_from_field(const void *field) {
    if (field == NULL)
        return NULL;
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    if (size != 16U)
        return NULL;
    return find_terminal((const uint8_t *)field + sizeof(size));
}

static terminal_state *available_terminal(void) {
    for (size_t index = 0U; index < EDGE_MAX_PLATFORMS; ++index)
        if (!terminals[index].active)
            return &terminals[index];
    return NULL;
}

bool edge_terminal_open(const iot_edge_v1_TerminalOpen *request,
                        char *error, size_t error_size) {
    if (!edge_capability_has_terminal()) {
        set_error(error, error_size, "terminal PTY is unavailable");
        return false;
    }
    if (request == NULL || request->terminal_id.size != 16U ||
        request->columns < 20U || request->columns > 500U ||
        request->rows < 5U || request->rows > 200U) {
        set_error(error, error_size, "terminal request is invalid");
        return false;
    }
    if (find_terminal(request->terminal_id.bytes) != NULL) {
        set_error(error, error_size, "terminal is already active");
        return false;
    }
    terminal_state *terminal = available_terminal();
    if (terminal == NULL) {
        set_error(error, error_size, "terminal capacity reached");
        return false;
    }
    struct winsize size = {
        .ws_row = (unsigned short)request->rows,
        .ws_col = (unsigned short)request->columns,
    };
    int master = -1;
    const pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) {
        set_error(error, error_size, "cannot open terminal pty");
        return false;
    }
    if (child == 0) {
        edge_process_close_inherited_fds(-1);
        setenv("TERM", "xterm-256color", 1);
        execl("/bin/ash", "ash", "-l", (char *)NULL);
        _exit(127);
    }
    const int flags = fcntl(master, F_GETFL, 0);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(master);
        terminate_and_reap(child);
        set_error(error, error_size, "cannot configure terminal pty");
        return false;
    }
    memset(terminal, 0, sizeof(*terminal));
    terminal->active = true;
    terminal->master = master;
    terminal->child = child;
    memcpy(terminal->id, request->terminal_id.bytes, sizeof(terminal->id));
    return true;
}

edge_terminal_input_result edge_terminal_flush(const uint8_t terminal_id[16],
                                               uint64_t *acked_sequence) {
    if (acked_sequence != NULL)
        *acked_sequence = 0U;
    terminal_state *terminal = find_terminal(terminal_id);
    if (terminal == NULL)
        return EDGE_TERMINAL_INPUT_ERROR;
    if (terminal->pending_input_size == 0U)
        return EDGE_TERMINAL_INPUT_IDLE;
    while (terminal->pending_input_offset < terminal->pending_input_size) {
        const ssize_t size = write(
            terminal->master, terminal->pending_input + terminal->pending_input_offset,
            terminal->pending_input_size - terminal->pending_input_offset);
        if (size > 0) {
            terminal->pending_input_offset += (size_t)size;
            continue;
        }
        if (size < 0 && errno == EINTR)
            continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return EDGE_TERMINAL_INPUT_PENDING;
        return EDGE_TERMINAL_INPUT_ERROR;
    }
    terminal->last_input_sequence = terminal->pending_input_sequence;
    terminal->pending_input_size = 0U;
    terminal->pending_input_offset = 0U;
    terminal->pending_input_sequence = 0U;
    if (acked_sequence != NULL)
        *acked_sequence = terminal->last_input_sequence;
    return EDGE_TERMINAL_INPUT_ACKED;
}

edge_terminal_input_result edge_terminal_write(
    const iot_edge_v1_TerminalData *request, uint64_t *acked_sequence) {
    if (acked_sequence != NULL)
        *acked_sequence = 0U;
    if (request == NULL)
        return EDGE_TERMINAL_INPUT_ERROR;
    terminal_state *terminal = terminal_from_field(&request->terminal_id);
    if (terminal == NULL || request->data.size == 0U ||
        request->data.size > sizeof(terminal->pending_input) ||
        request->sequence == 0U)
        return EDGE_TERMINAL_INPUT_ERROR;
    if (terminal->pending_input_size != 0U)
        return EDGE_TERMINAL_INPUT_ERROR;
    if (terminal->last_input_sequence == UINT64_MAX ||
        request->sequence != terminal->last_input_sequence + 1U)
        return EDGE_TERMINAL_INPUT_ERROR;
    memcpy(terminal->pending_input, request->data.bytes, request->data.size);
    terminal->pending_input_size = request->data.size;
    terminal->pending_input_offset = 0U;
    terminal->pending_input_sequence = request->sequence;
    return edge_terminal_flush(terminal->id, acked_sequence);
}

bool edge_terminal_resize(const iot_edge_v1_TerminalResize *request) {
    if (request == NULL)
        return false;
    terminal_state *terminal = terminal_from_field(&request->terminal_id);
    if (terminal == NULL || request->columns < 20U || request->columns > 500U ||
        request->rows < 5U || request->rows > 200U)
        return false;
    const struct winsize size = {
        .ws_row = (unsigned short)request->rows,
        .ws_col = (unsigned short)request->columns,
    };
    return ioctl(terminal->master, TIOCSWINSZ, &size) == 0;
}

void edge_terminal_close(const uint8_t terminal_id[16]) {
    terminal_state *terminal = find_terminal(terminal_id);
    if (terminal == NULL)
        return;
    close(terminal->master);
    terminate_and_reap(terminal->child);
    memset(terminal, 0, sizeof(*terminal));
}

#ifdef EDGENODE_TERMINAL_TEST
bool edge_terminal_test_attach(int master, const uint8_t terminal_id[16]) {
    if (master < 0 || terminal_id == NULL || find_terminal(terminal_id) != NULL)
        return false;
    terminal_state *terminal = available_terminal();
    if (terminal == NULL)
        return false;
    memset(terminal, 0, sizeof(*terminal));
    terminal->active = true;
    terminal->master = master;
    terminal->child = -1;
    memcpy(terminal->id, terminal_id, sizeof(terminal->id));
    return true;
}
#endif

ssize_t edge_terminal_read(const uint8_t terminal_id[16], uint8_t *output, size_t capacity,
                           bool *closed, int32_t *exit_code) {
    if (closed != NULL)
        *closed = false;
    terminal_state *terminal = find_terminal(terminal_id);
    if (terminal == NULL || output == NULL || capacity == 0U)
        return 0;
    const ssize_t size = read(terminal->master, output, capacity);
    if (size > 0)
        return size;
    int status = 0;
    const pid_t result = waitpid(terminal->child, &status, WNOHANG);
    if (result == terminal->child || (size == 0) ||
        (size < 0 && errno != EAGAIN && errno != EINTR)) {
        if (closed != NULL)
            *closed = true;
        if (exit_code != NULL)
            *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        edge_terminal_close(terminal->id);
    }
    return 0;
}
