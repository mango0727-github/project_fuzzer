#include "fuzzer_internal.h"
#include <string.h>

uint8_t coverage_map[COVERAGE_MAP_SIZE];
uint8_t *coverage_map_tmp = NULL;

int is_new_coverage(void) {
  int new_covered = 0;

  for (int i = 0; i < COVERAGE_MAP_SIZE; i++) {
    int prev = coverage_map[i];
    int curr = coverage_map_tmp[i];

    if (prev < curr) {
      new_covered = 1;
      coverage_map[i] = coverage_map_tmp[i];
    }
  }

  return new_covered;
}
