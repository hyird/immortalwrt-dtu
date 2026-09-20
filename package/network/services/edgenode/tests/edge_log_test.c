#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

static int64_t clock_ms = 1000;
static bool clock_failed;

int edge_log_test_clock_gettime(clockid_t id, struct timespec *value) {
  (void)id;
  if (clock_failed)
    return -1;
  value->tv_sec = (time_t)(clock_ms / 1000);
  value->tv_nsec = (long)((clock_ms % 1000) * 1000000);
  return 0;
}

static void test_log_lease(void) {
  edge_log_init();
  assert(strcmp(edge_log_level(), "silent") == 0);
  assert(!edge_log_enabled("silent"));
  const char *levels[] = {"debug", "info", "warn", "error"};
  for (unsigned i = 0; i < 4; ++i) {
    assert(!edge_log_enabled(levels[i]));
    const int64_t start = clock_ms;
    assert(edge_log_set_level(levels[i]));
    assert(edge_log_enabled(levels[i]));
    assert(strcmp(edge_log_level(), levels[i]) == 0);
    clock_ms = start + 299999;
    assert(edge_log_enabled(levels[i]));
    clock_ms = start + 300000;
    assert(!edge_log_enabled("error"));
    assert(strcmp(edge_log_level(), "silent") == 0);
  }
  assert(edge_log_set_level("debug"));
  clock_ms += 200000;
  assert(edge_log_set_level("debug"));
  clock_ms += 100000;
  assert(edge_log_enabled("debug"));
  assert(!edge_log_set_level("slient"));
  assert(!edge_log_set_level(NULL));
  clock_ms += 200000;
  assert(!edge_log_enabled("error"));
  assert(edge_log_set_level("info"));
  assert(edge_log_set_level("silent"));
  assert(!edge_log_enabled("error"));
  assert(edge_log_set_level("warn"));
  for (int i = 1; i <= 300; ++i) {
    clock_ms += 1000;
    assert(edge_log_enabled("warn") == (i < 300));
  }
  assert(edge_log_set_level("warn"));
  edge_log_init();
  assert(strcmp(edge_log_level(), "silent") == 0);

  /* A worker forked before the setting changes sees the node-wide lease. */
  int ready[2], changed[2];
  assert(pipe(ready) == 0 && pipe(changed) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    char signal = 'r';
    assert(write(ready[1], &signal, 1) == 1);
    assert(read(changed[0], &signal, 1) == 1);
    clock_ms += 1000;
    assert(edge_log_enabled("debug"));
    clock_ms += 299000;
    assert(!edge_log_enabled("error"));
    _exit(0);
  }
  char signal;
  assert(read(ready[0], &signal, 1) == 1);
  assert(edge_log_set_level("debug"));
  assert(write(changed[1], "c", 1) == 1);
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(ready[0]); close(ready[1]); close(changed[0]); close(changed[1]);
  clock_failed = true;
  assert(!edge_log_enabled("error"));
  assert(!edge_log_set_level("debug"));
  clock_failed = false;
  edge_log_init();
}

int main(void) {
  test_log_lease();
  assert(setenv("EDGENODE_LOGREAD_COMMAND",
                "printf 'Tue Sep  1 21:00:00 2026 daemon.info first line\\nTue "
                "Sep  1 21:01:00 2026 daemon.err second line\\n'",
                1) == 0);

  iot_edge_v1_LogRequest request = iot_edge_v1_LogRequest_init_zero;
  request.limit = 2U;
  snprintf(request.source, sizeof(request.source), "%s", "system");
  iot_edge_v1_LogResult result = iot_edge_v1_LogResult_init_zero;
  edge_log_query(&request, &result);

  assert(result.success);
  assert(result.lines_count == 2U);
  assert(strcmp(result.lines[0].level, "error") == 0);
  assert(strcmp(result.lines[0].source, "system") == 0);
  assert(strstr(result.lines[0].message, "second line") != NULL);
  assert(strcmp(result.lines[1].level, "info") == 0);
  assert(strstr(result.lines[1].message, "first line") != NULL);
  assert(result.lines[0].time_ms > result.lines[1].time_ms);
  return 0;
}
