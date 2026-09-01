#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

int main(void) {
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
