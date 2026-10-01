#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
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

static void test_local_diagnostics_do_not_enter_log_api(void) {
  const char *path = "/tmp/edgenode-test-local-diagnostics/logs/acquisition.log";
  (void)unlink(path);
  for (unsigned index = 1U; index < 4U; ++index) {
    char rotated[120];
    snprintf(rotated, sizeof(rotated), "%s.%u", path, index);
    (void)unlink(rotated);
  }
  edge_log_init();
  assert(strcmp(edge_log_level(), "silent") == 0);
  const uint8_t platform[16] = {0x12U}, device[16] = {0x34U};
  edge_log_local_config(platform, device, 3U, 1U, 1000U, 300U);
  edge_log_local_io(platform, device, 3U, "read", 2U);
  edge_log_local_report(platform, device, 3U, EDGE_LOCAL_REPORT_ENQUEUE, 11U, 22U);
  edge_log_local_report(platform, device, 3U, EDGE_LOCAL_REPORT_QUEUED, 11U, 22U);
  edge_log_local_fault(platform, device, 3U, EDGE_LOCAL_FAULT_SAMPLE_INCOMPLETE, 7U);
  struct stat info;
  assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
  struct stat lock_info;
  assert(stat("/tmp/edgenode-test-local-diagnostics/logs/acquisition.lock", &lock_info) == 0 &&
         (lock_info.st_mode & 0777) == 0600);
  FILE *input = fopen(path, "r");
  assert(input != NULL);
  char lines[1024] = {0};
  const size_t read = fread(lines, 1U, sizeof(lines) - 1U, input);
  assert(read != 0U && fclose(input) == 0);
  assert(strstr(lines, "12000000") != NULL && strstr(lines, "34000000") != NULL);
  assert(strstr(lines, "\t3\tconfig-applied\t1000\t300\t1\n") != NULL);
  assert(strstr(lines, "\t3\tio-read\t2\t0\t0\n") != NULL);
  assert(strstr(lines, "\t3\treport-failed\t4\t11\t22\n") != NULL);
  assert(strstr(lines, "\t3\treport-queued\t0\t11\t22\n") != NULL);
  assert(strstr(lines, "\t3\tsample-incomplete\t7\t0\t0\n") != NULL);
  assert(strstr(lines, "data=") == NULL && strstr(lines, "endpoint=") == NULL);
  const off_t first_size = info.st_size;
  edge_log_local_fault(platform, device, 3U, EDGE_LOCAL_FAULT_SAMPLE_INCOMPLETE, 7U);
  assert(stat(path, &info) == 0 && info.st_size == first_size);
  clock_ms += 60000;
  edge_log_local_fault(platform, device, 3U, EDGE_LOCAL_FAULT_SAMPLE_INCOMPLETE, 7U);
  assert(stat(path, &info) == 0 && info.st_size > first_size);
  iot_edge_v1_LogRequest request = iot_edge_v1_LogRequest_init_zero;
  iot_edge_v1_LogResult result = iot_edge_v1_LogResult_init_zero;
  edge_log_query(&request, &result);
  assert(result.success && result.lines_count == 0U);
  for (unsigned i = 0U; i < 4000U; ++i)
    edge_log_local_io(platform, device, 3U, "not-a-real-operation secret", 2U);
  assert(stat(path, &info) == 0 && info.st_size <= 256 * 1024);
  assert(stat("/tmp/edgenode-test-local-diagnostics/logs/acquisition.log.1", &info) == 0 &&
         (info.st_mode & 0777) == 0600);
  input = fopen(path, "r");
  assert(input != NULL);
  memset(lines, 0, sizeof(lines));
  assert(fread(lines, 1U, sizeof(lines) - 1U, input) != 0U);
  fclose(input);
  assert(strstr(lines, "secret") == NULL);
  const pid_t child = fork();
  assert(child >= 0);
  for (unsigned index = 0U; index < 1200U; ++index)
    edge_log_local_io(platform, device, 3U, "read", 2U);
  if (child == 0)
    _exit(0);
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  input = fopen(path, "r");
  assert(input != NULL);
  while (fgets(lines, sizeof(lines), input) != NULL) {
    unsigned tabs = 0U;
    for (const char *cursor = lines; *cursor != '\0'; ++cursor)
      tabs += *cursor == '\t';
    assert(tabs == 7U && strchr(lines, '\n') != NULL);
  }
  assert(fclose(input) == 0);
  (void)unlink(path);
  for (unsigned index = 1U; index < 4U; ++index) {
    char rotated[120];
    snprintf(rotated, sizeof(rotated), "%s.%u", path, index);
    (void)unlink(rotated);
  }
}

int main(void) {
  test_log_lease();
  test_local_diagnostics_do_not_enter_log_api();
  return 0;
}
