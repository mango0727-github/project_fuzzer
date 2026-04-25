// Analogy: trace-pc-guard works as a "guard" that marks which parts of the code
// have been executed. On the other hand, inline-8bit-counters work as a "guard
// with a counter" that not only marks execution but also counts how many times
// each part has been executed. Therefore, inline-8bit-counters provide more
// detailed coverage information compared to trace-pc-guard so that our fuzzer
// captures loop-related bugs (e.g., off-by-one overflows, size checks, or
// loop-carried memory corruption).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
char *start_address;
char *end_address;

void __sanitizer_cov_8bit_counters_init(char *start, char *end) {
  // [start,end) is the array of 8-bit counters created for the current DSO.
  // Capture this array in order to read/modify the counters.
  start_address = start;
  end_address = end;
}
