#include "fuzzer_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

jmp_buf init_state;
int last_target_exit_status = 0;
int last_target_exited_via_exit = 0;

static void cleanup_target_state(void) {
  for (int i = 0; i < chunk_count; i++) {
    if (chunk_map[i]) {
      __real_free(chunk_map[i]);
      chunk_map[i] = NULL;
    }
  }
  chunk_count = 0;

  for (int i = 0; i < file_count; i++) {
    if (file_map[i]) {
      __real_fclose(file_map[i]);
      file_map[i] = NULL;
    }
  }
  file_count = 0;

  if (global_data_size > 0 && global_data_start && global_data_backup) {
    memcpy(global_data_start, global_data_backup, global_data_size);
  }
}

int call_targetMain_reentrant(const char *target, const char *file_path) {

  if (!start_address || !end_address || end_address <= start_address) {
    return -1;
  }

  size_t cov_size = (size_t)(end_address - start_address);

  if (cov_size > COVERAGE_MAP_SIZE) {
    fprintf(stderr, "coverage section too large: %zu > %d\n", cov_size,
            COVERAGE_MAP_SIZE);
    return -1;
  }

  last_target_exit_status = 0;
  last_target_exited_via_exit = 0;

  memset(start_address, 0, cov_size);
  memset(coverage_map_tmp, 0, COVERAGE_MAP_SIZE);

  int status = 0;

  if (setjmp(init_state) == 0) {
    in_target = 1;

    char *args[] = {(char *)target, (char *)"-q", (char *)file_path,
                    (char *)"/dev/null", NULL};

    status = call_targetMain_cpp(4, args);
    in_target = 0;

    memcpy(coverage_map_tmp, start_address, cov_size);
  } else {
    in_target = 0;
    memcpy(coverage_map_tmp, start_address, cov_size);
    status = -1000 - last_target_exit_status;
  }

  cleanup_target_state();
  memset(start_address, 0, cov_size);

  return status;
}
