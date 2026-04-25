#include "fuzzer_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int init_runtime_state(void) {
  if (!start_address || !end_address || end_address <= start_address) {
    fprintf(stderr, "sanitizer counter section not initialized\n");
    return -1;
  }

  setup_global_snapshot();

  fprintf(stderr, "counter_size=%zu, global_data_size=%zu\n",
          (size_t)(end_address - start_address), global_data_size);

  if (global_data_size > 0 && (!global_data_start || !global_data_backup)) {
    fprintf(stderr, "global snapshot metadata inconsistent\n");
    return -1;
  }

  return 0;
}

static void print_trial_header(int trial, int total_trials) {
  printf("\n#########################################\n");
  printf("############### Trial %d/%d ###############\n", trial + 1,
         total_trials);
  printf("#########################################\n");
}

static unsigned long long count_covered_edges(void) {
  unsigned long long edges = 0;

  for (int i = 0; i < COVERAGE_MAP_SIZE; i++) {
    if (coverage_map[i] > 0) {
      edges++;
    }
  }

  return edges;
}

static void write_progress_report(FILE *stream,
                                  unsigned long long num_testcases,
                                  long elapsed, int seed_count,
                                  unsigned long long edges, double throughput) {
  fprintf(stream, "--------------------------------------------------\n");
  fprintf(stream, "| %-25s |  %5llu  cases         |\n",
          "# of test cases generated", num_testcases);
  fprintf(stream, "| %-25s |  %5ld seconds        |\n", "Time elapsed",
          elapsed);
  fprintf(stream, "| %-25s |  %5d                |\n", "# of struct in array",
          seed_count);
  fprintf(stream, "| %-25s |  %5llu                |\n", "Total edges covered",
          edges);
  fprintf(stream, "| %-25s |  %5.2f exec/s        |\n", "Throughput",
          throughput);
  fprintf(stream, "--------------------------------------------------\n");
}

static void write_trial_summary(FILE *stream, long final_elapsed,
                                unsigned long long num_testcases) {
  fprintf(stream, "\nResult: No crash found\n");
  fprintf(stream, "Time elapsed: %ld\n", final_elapsed);
  fprintf(stream, "Test cases generated: %llu\n", num_testcases);
  fprintf(stream, "Trial ends\n");
  fprintf(stream, "------------------------------------------\n");
}

static int save_interesting_seed(const unsigned char *data, size_t size,
                                 unsigned long long index) {
  char save_path[256];

  snprintf(save_path, sizeof(save_path),
           DEFAULT_INTERESTING_DIR "/interesting_%llu.pdf", index);

  FILE *seed_file = fopen(save_path, "wb");
  if (!seed_file) {
    return -1;
  }

  fwrite(data, 1, size, seed_file);
  fclose(seed_file);
  return 0;
}

static void cleanup_main_resources(FILE *out, int fd, unsigned char *map_ptr) {
  if (map_ptr != MAP_FAILED) {
    munmap(map_ptr, MAX_FILE_SIZE);
  }

  if (coverage_map_tmp != NULL && coverage_map_tmp != MAP_FAILED) {
    munmap(coverage_map_tmp, COVERAGE_MAP_SIZE);
    coverage_map_tmp = NULL;
  }

  if (fd >= 0) {
    close(fd);
  }

  if (out) {
    fclose(out);
  }
}

static int run_single_trial(FILE *out, const char *target,
                            const char *temp_input_path, int fd,
                            unsigned char *map_ptr, int trial, int total_trials,
                            int seedset_count_init) {
  unsigned long long num_testcases = 0;
  const unsigned seed = 1;
  const time_t start_time = time(NULL);

  print_trial_header(trial, total_trials);

  srand(seed);
  struct_count = seedset_count_init;
  memset(coverage_map, 0, sizeof(coverage_map));
  memset(coverage_map_tmp, 0, COVERAGE_MAP_SIZE);

  fprintf(out, "Trial: %d/%d\n", trial + 1, total_trials);
  fprintf(out, "Target: %s\n", target);
  fprintf(out, "Seed: %u\n", seed);

  while (difftime(time(NULL), start_time) < 86400) {
    if (ftruncate(fd, MAX_FILE_SIZE) == -1) {
      perror("ftruncate reset failed");
      return -1;
    }

    size_t curr_size = pick_seed_and_mutate(map_ptr);

    if (ftruncate(fd, (off_t)curr_size) == -1) {
      perror("ftruncate resize failed");
      return -1;
    }

    num_testcases++;

    int execution = call_targetMain_reentrant(target, temp_input_path);
    if (execution == -1) {
      fprintf(stderr, "target execution failed\n");
      return -1;
    }

    if (is_new_coverage()) {
      add_valid_seed_struct_to_array(map_ptr, curr_size);

      interesting_cnt++;
      if (save_interesting_seed(map_ptr, curr_size, interesting_cnt) != 0) {
        fprintf(stderr, "failed to save interesting seed %llu\n",
                interesting_cnt);
      }
    }

    if (num_testcases % 10000 == 0) {
      time_t elapsed = time(NULL) - start_time;
      unsigned long long edges = count_covered_edges();
      double throughput =
          (elapsed > 0) ? (double)num_testcases / (double)elapsed : 0.0;

      write_progress_report(out, num_testcases, (long)elapsed, struct_count,
                            edges, throughput);
      fflush(out);

      write_progress_report(stdout, num_testcases, (long)elapsed, struct_count,
                            edges, throughput);
    }
  }

  time_t final_elapsed = time(NULL) - start_time;
  write_trial_summary(out, (long)final_elapsed, num_testcases);
  fflush(out);

  return 0;
}

int main(int argc, char **argv) {
  FILE *out = NULL;
  int fd = -1;
  unsigned char *map_ptr = MAP_FAILED;

  if (argc != 3) {
    fprintf(stderr, "Usage: %s <target_path> <number_of_trials>\n", argv[0]);
    return 1;
  }

  const char *target = argv[1];
  const int total_trials = atoi(argv[2]);
  const char *temp_input_path = DEFAULT_TEMP_INPUT_PATH;

  if (total_trials <= 0) {
    fprintf(stderr, "number_of_trials must be positive\n");
    return 1;
  }

  printf("starting fuzzerMain with target\n");

  out = fopen(DEFAULT_OUTPUT_PATH, "w");
  if (!out) {
    perror("Failed to open output file");
    cleanup_main_resources(out, fd, map_ptr);
    return 1;
  }

  coverage_map_tmp = mmap(NULL, COVERAGE_MAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (coverage_map_tmp == MAP_FAILED) {
    perror("mmap coverage_map_tmp failed");
    cleanup_main_resources(out, fd, map_ptr);
    return 1;
  }

  if (init_runtime_state() != 0) {
    cleanup_main_resources(out, fd, map_ptr);
    return 1;
  }

  load_dict(DEFAULT_DICT_PATH);

  fd = open(temp_input_path, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    perror("Failed to create temporary file");
    cleanup_main_resources(out, fd, map_ptr);
    return 1;
  }

  if (ftruncate(fd, MAX_FILE_SIZE) == -1) {
    perror("Failed to set size of temporary file");
    cleanup_main_resources(out, fd, map_ptr);
    return 1;
  }

  map_ptr = (unsigned char *)mmap(NULL, MAX_FILE_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
  if (map_ptr == MAP_FAILED) {
    perror("mmap temp input failed");
    cleanup_main_resources(out, fd, map_ptr);
    return 1;
  }

  load_seeds_from_files(DEFAULT_SEED_DIR, target);
  const int seedset_count_init = struct_count;

  for (int trial = 0; trial < total_trials; trial++) {
    if (run_single_trial(out, target, temp_input_path, fd, map_ptr, trial,
                         total_trials, seedset_count_init) != 0) {
      cleanup_main_resources(out, fd, map_ptr);
      return 1;
    }
  }

  cleanup_main_resources(out, fd, map_ptr);
  return 0;
}
