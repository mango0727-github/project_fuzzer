#ifndef FUZZER_INTERNAL_H
#define FUZZER_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Project-wide constants

#define MAX_OBJECTS 4096
#define COVERAGE_MAP_SIZE (1 << 16)
#define MAX_FILE_SIZE (1 << 20)

#define MAX_DICTIONARY_ENTRIES 1500
#define INITIAL_SEED_CAPACITY 100

#define DEFAULT_DICT_PATH "pdf.dict"
#define DEFAULT_SEED_DIR "seeds"
#define DEFAULT_WORK_DIR "/dev/shm/temp_fuzzer_workdir"
#define DEFAULT_INTERESTING_DIR "/dev/shm/temp_fuzzer_workdir/interesting"
#define DEFAULT_OUTPUT_PATH "/dev/shm/temp_fuzzer_workdir/Real_World_Fuzzer_Result.txt"
#define DEFAULT_TEMP_INPUT_PATH "/dev/shm/temp_fuzzer_workdir/temp_fuzz_input.pdf"

// Shared types
typedef struct seed_struct {
  unsigned char *data;
  size_t size;
} seed_struct;

// External symbols from target/shim/trace code
extern int call_targetMain_cpp(int argc, char **argv);

extern void *chunk_map[];
extern int chunk_count;
extern void __real_free(void *ptr);

extern FILE *file_map[];
extern int file_count;
extern int __real_fclose(FILE *stream);

extern void setup_global_snapshot(void);
extern void *global_data_start;
extern void *global_data_backup;
extern size_t global_data_size;

extern char *start_address;
extern char *end_address;
extern int in_target;

// Shared runtime state
extern jmp_buf init_state;
extern int last_target_exit_status;
extern int last_target_exited_via_exit;

// Shared coverage state
extern uint8_t coverage_map[COVERAGE_MAP_SIZE];
extern uint8_t *coverage_map_tmp;

// Shared corpus state
extern seed_struct *seed_struct_array;
extern int struct_count;
extern int struct_array_size;
extern unsigned long long interesting_cnt;

// Shared dictionary state

extern char *dictionary[MAX_DICTIONARY_ENTRIES];
extern int dict_size;

// Cross-module function declarations
void load_dict(const char *file_name);
size_t fix_pdf_xref(unsigned char *input, size_t cur, size_t max_size);
size_t pick_seed_and_mutate(unsigned char *input);

int call_targetMain_reentrant(const char *target, const char *file_path);

int is_new_coverage(void);

void add_valid_seed_struct_to_array(const unsigned char *input,
                                    size_t input_size);
void load_seeds_from_files(const char *dirname, const char *target_path);

#ifdef __cplusplus
}
#endif

#endif
