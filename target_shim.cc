#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * target_shim.cc
 *
 * Responsibilities:
 *  - snapshot writable global section once at startup
 *  - track heap/file resources created during target execution
 *  - provide wrappers for malloc/calloc/realloc/free and fopen/fclose
 *  - redirect explicit exit() from target into longjmp-based recovery
 */

#define MAX_ALLOCS 10000
#define MAX_FILES 1000

// Shared state exposed to other translation units

void *global_data_start = NULL;
void *global_data_backup = NULL;
size_t global_data_size = 0;

void *chunk_map[MAX_ALLOCS];
int chunk_count = 0;

FILE *file_map[MAX_FILES];
int file_count = 0;

int in_target = 0;

// Real libc symbols resolved by linker wrapping

extern "C" void *__real_malloc(size_t size);
extern "C" void *__real_calloc(size_t nmemb, size_t size);
extern "C" void *__real_realloc(void *ptr, size_t size);
extern "C" void __real_free(void *ptr);

extern "C" FILE *__real_fopen(const char *filename, const char *mode);
extern "C" int __real_fclose(FILE *stream);

// Exit-recovery state from runtime.c

extern "C" {
extern jmp_buf init_state;
extern int last_target_exit_status;
extern int last_target_exited_via_exit;
}

// Internal helpers

static void remove_tracked_chunk(void *ptr) {
  if (!ptr) {
    return;
  }

  for (int i = 0; i < chunk_count; i++) {
    if (chunk_map[i] == ptr) {
      chunk_map[i] = chunk_map[--chunk_count];
      return;
    }
  }
}

static void track_chunk(void *ptr) {
  if (!in_target || !ptr) {
    return;
  }

  if (chunk_count < MAX_ALLOCS) {
    chunk_map[chunk_count++] = ptr;
  }
}

static void remove_tracked_file(FILE *stream) {
  if (!stream) {
    return;
  }

  for (int i = 0; i < file_count; i++) {
    if (file_map[i] == stream) {
      file_map[i] = file_map[--file_count];
      return;
    }
  }
}

static void track_file(FILE *stream) {
  if (!in_target || !stream) {
    return;
  }

  if (file_count < MAX_FILES) {
    file_map[file_count++] = stream;
  }
}

static void reset_global_snapshot_state(void) {
  if (global_data_backup) {
    __real_free(global_data_backup);
    global_data_backup = NULL;
  }

  global_data_start = NULL;
  global_data_size = 0;
}

// Global snapshot setup

extern "C" void setup_global_snapshot() {
  const char *addr_str = getenv("CLOSURE_GLOBAL_SECTION_ADDR");
  const char *size_str = getenv("CLOSURE_GLOBAL_SECTION_SIZE");

  reset_global_snapshot_state();

  if (!addr_str || !size_str) {
    return;
  }

  global_data_start = (void *)strtoull(addr_str, NULL, 16);
  global_data_size = (size_t)strtoull(size_str, NULL, 10);

  if (!global_data_start || global_data_size == 0) {
    global_data_start = NULL;
    global_data_size = 0;
    return;
  }

  global_data_backup = __real_malloc(global_data_size);
  if (!global_data_backup) {
    global_data_start = NULL;
    global_data_size = 0;
    return;
  }

  memcpy(global_data_backup, global_data_start, global_data_size);
}

// File wrappers

extern "C" FILE *__wrap_fopen(const char *filename, const char *mode) {
  FILE *stream = __real_fopen(filename, mode);
  track_file(stream);
  return stream;
}

extern "C" int __wrap_fclose(FILE *stream) {
  if (!stream) {
    return EOF;
  }

  if (in_target) {
    remove_tracked_file(stream);
  }

  return __real_fclose(stream);
}

// Heap wrappers

extern "C" void *__wrap_malloc(size_t size) {
  void *ptr = __real_malloc(size);
  track_chunk(ptr);
  return ptr;
}

extern "C" void *__wrap_calloc(size_t nmemb, size_t size) {
  void *ptr = __real_calloc(nmemb, size);
  track_chunk(ptr);
  return ptr;
}

extern "C" void *__wrap_realloc(void *ptr, size_t size) {
  if (!in_target) {
    return __real_realloc(ptr, size);
  }

  if (ptr) {
    remove_tracked_chunk(ptr);
  }

  void *new_ptr = __real_realloc(ptr, size);

  if (new_ptr && size > 0) {
    track_chunk(new_ptr);
  }

  return new_ptr;
}

extern "C" void __wrap_free(void *ptr) {
  if (!ptr) {
    return;
  }

  if (in_target) {
    remove_tracked_chunk(ptr);
  }

  __real_free(ptr);
}

// Target entry shim

int targetMain(int argc, char **argv);

extern "C" int call_targetMain_cpp(int argc, char **argv) {
  return targetMain(argc, argv);
}

// exit() redirection

extern "C" void __wrap_exit(int status) {
  last_target_exit_status = status;
  last_target_exited_via_exit = 1;
  longjmp(init_state, 1);
}