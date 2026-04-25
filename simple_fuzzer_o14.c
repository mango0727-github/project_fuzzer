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

#define MAX_OBJECTS 4096

#define COVERAGE_MAP_SIZE (1 << 16)
#define MAX_FILE_SIZE (1 << 20)

extern int call_targetMain_cpp(int argc, char **argv);
extern void *chunk_map[];
extern int chunk_count;
extern void __real_free(void *ptr);

extern void setup_global_snapshot(void);
extern void *global_data_start;
extern void *global_data_backup;
extern size_t global_data_size;

static uint8_t coverage_map[COVERAGE_MAP_SIZE];

jmp_buf init_state;

int *shared_exec_result = NULL;

uint8_t *coverage_map_tmp;

unsigned long long edges = 0;

typedef struct seed_struct {
  unsigned char *data;
  size_t size;
} seed_struct;

seed_struct *seed_struct_array = NULL;
static int struct_count = 0;
static int struct_array_size = 0;
unsigned long long interesting_cnt = 0;

char *dictionary[1500];
int dict_size = 0;

pid_t child_pid = 0;

void handle_timeout(int sig) {
  if (child_pid > 0) {
    kill(child_pid, SIGKILL);
  }
}

int last_target_exit_status = 0;
int last_target_exited_via_exit = 0;

void load_dict(const char *file_name) {
  FILE *file = fopen(file_name, "r");
  if (!file) {
    printf("File open failed");
    return;
  }

  char line[1024];

  while (fgets(line, sizeof(line), file)) {
    if (dict_size >= 1500) {
      printf("the length of a line exceeds the buffer\n");
      break;
    }

    if (line[0] == '#' || strlen(line) < 3)
      continue;

    char *start = strchr(line, '"');
    char *end = strrchr(line, '"');

    if (start && end && end > start) {
      *end = '\0';
      dictionary[dict_size] = strdup(start + 1);
      dict_size++;
    }
  }
  fclose(file);
}

size_t fix_pdf_xref(unsigned char *input, size_t cur, size_t max_size) {
  if (cur < 20)
    return cur;

  char root_ref[32] = "1 0 R";
  unsigned char *root_ptr = memmem(input, cur, "/Root", 5);
  if (root_ptr) {
    int root_id = 1;
    if (sscanf((char *)root_ptr, "/Root %d", &root_id) == 1) {
      snprintf(root_ref, sizeof(root_ref), "%d 0 R", root_id);
    }
  }

  int obj_offsets[MAX_OBJECTS] = {0};
  int max_obj_id = 0;

  unsigned char *search_ptr = input;
  size_t remain = cur;

  while (remain > 6) {
    unsigned char *obj_ptr = memmem(search_ptr, remain, " 0 obj", 6);
    if (!obj_ptr)
      break;

    unsigned char *num_ptr = obj_ptr - 1;
    while (num_ptr > input && isspace(*num_ptr))
      num_ptr--;
    while (num_ptr > input && isdigit(*num_ptr))
      num_ptr--;
    if (!isdigit(*num_ptr))
      num_ptr++;

    int obj_id = atoi((char *)num_ptr);
    if (obj_id > 0 && obj_id < MAX_OBJECTS) {
      obj_offsets[obj_id] = (int)(num_ptr - input);
      if (obj_id > max_obj_id)
        max_obj_id = obj_id;
    }

    size_t step = (obj_ptr - search_ptr) + 6;
    search_ptr += step;
    remain -= step;
  }

  if (max_obj_id == 0)
    return cur;

  int startxref_offset = cur;
  char buf[256];
  int len;

  len = snprintf(buf, sizeof(buf), "\nxref\n0 %d\n0000000000 65535 f \n",
                 max_obj_id + 1);
  if (cur + len > max_size)
    return startxref_offset;
  memcpy(input + cur, buf, len);
  cur += len;

  for (int i = 1; i <= max_obj_id; i++) {
    if (obj_offsets[i] > 0) {
      len = snprintf(buf, sizeof(buf), "%010d 00000 n \n", obj_offsets[i]);
    } else {
      len = snprintf(buf, sizeof(buf), "0000000000 00000 f \n");
    }
    if (cur + len > max_size)
      return startxref_offset;
    memcpy(input + cur, buf, len);
    cur += len;
  }

  len = snprintf(buf, sizeof(buf),
                 "trailer\n<< /Size %d /Root %s >>\nstartxref\n%d\n%%EOF\n",
                 max_obj_id + 1, root_ref, startxref_offset);
  if (cur + len > max_size)
    return startxref_offset;
  memcpy(input + cur, buf, len);
  cur += len;

  return cur;
}

size_t pick_seed_and_mutate(unsigned char *input) {
  if (struct_count == 0) {
    size_t len_rand = (size_t)(rand() % 256);
    for (size_t j = 0; j < len_rand; j++) {
      input[j] = (unsigned char)(rand() & 0xFF);
    }
    return len_rand;
  }

  int idx_a = rand() % struct_count;
  seed_struct *A = &seed_struct_array[idx_a];
  size_t cur = A->size;
  if (cur > 0) {
    memcpy(input, A->data, cur);
  } else {
    return 0;
  }

  if (struct_count >= 2 && (rand() % 100) < 30) {
    int idx_b = rand() % struct_count;
    seed_struct *B = &seed_struct_array[idx_b];
    unsigned char *obj_a_start = memmem(input, cur, " obj", 4);
    unsigned char *obj_a_end =
        obj_a_start
            ? memmem(obj_a_start, cur - (obj_a_start - input), "endobj", 6)
            : NULL;
    unsigned char *obj_b_start = memmem(B->data, B->size, " obj", 4);
    unsigned char *obj_b_end =
        obj_b_start ? memmem(obj_b_start, B->size - (obj_b_start - B->data),
                             "endobj", 6)
                    : NULL;

    if (obj_a_start && obj_a_end && obj_b_start && obj_b_end) {
      obj_a_start += 4;
      obj_b_start += 4;

      size_t len_a = obj_a_end - obj_a_start;
      size_t len_b = obj_b_end - obj_b_start;

      if (len_b > 0 && cur - len_a + len_b < MAX_FILE_SIZE) {

        memmove(obj_a_start + len_b, obj_a_start + len_a,
                cur - (obj_a_end - input));
        memcpy(obj_a_start, obj_b_start, len_b);
        cur = cur - len_a + len_b;
      }
    }
  }

  if (cur == 0) {
    return 0;
  }

  if (dict_size > 0 && (rand() % 100) < 30) {
    char *kw = dictionary[rand() % dict_size];
    size_t kw_len = strlen(kw);

    if (kw_len > 0 && cur + kw_len < MAX_FILE_SIZE) {
      size_t pos = (size_t)(rand() % cur);
      memmove(input + pos + kw_len, input + pos, cur - pos);
      memcpy(input + pos, kw, kw_len);
      cur += kw_len;
    }
  }

  if ((rand() % 100) < 50) {
    const char *danger_keys[] = {
        "/Length", "/Size",   "/Count",   "/Columns",   "/Predictor", "/W",
        "/W2",     "/Ascent", "/Descent", "/CapHeight", "/First",     "/N"};
    int num_keys = sizeof(danger_keys) / sizeof(danger_keys[0]);

    for (int i = 0; i < 3; i++) {
      const char *key = danger_keys[rand() % num_keys];
      unsigned char *ptr = memmem(input, cur, key, strlen(key));

      if (ptr) {
        size_t pos = (ptr - input) + strlen(key);
        while (pos < cur && !isdigit(input[pos]) && input[pos] != '-') {
          pos++;
        }

        if (pos < cur && isdigit(input[pos])) {
          size_t num_len = 0;
          while (pos + num_len < cur && isdigit(input[pos + num_len])) {
            num_len++;
          }

          if (num_len > 0) {
            int attack_type = rand() % 4;
            for (size_t j = 0; j < num_len; j++) {
              if (attack_type == 0) {
                input[pos + j] = '9';
              } else if (attack_type == 1) {
                input[pos + j] = '0';
              } else if (attack_type == 2 && j == 0) {
                input[pos + j] = '-';
              } else {
                input[pos + j] = (unsigned char)('0' + (rand() % 10));
              }
            }
          }
        }
      }
    }
  }

  if ((rand() % 100) < 60) {
    unsigned char *stream_start = memmem(input, cur, "stream", 6);
    unsigned char *stream_end = memmem(input, cur, "endstream", 9);

    if (stream_start && stream_end && stream_start < stream_end) {
      stream_start += 6;
      while (stream_start < stream_end &&
             (*stream_start == '\r' || *stream_start == '\n')) {
        stream_start++;
      }

      size_t stream_len = stream_end - stream_start;

      if (stream_len > 0) {
        int mutations = 1 + (stream_len / 50);
        if (mutations > 30) {
          mutations = 30;
        }

        for (int m = 0; m < mutations; m++) {
          size_t pos = (size_t)(rand() % stream_len);
          if (rand() % 2 == 0) {
            stream_start[pos] ^= (unsigned char)(1u << (rand() % 8));
          } else {
            stream_start[pos] = (unsigned char)(rand() & 0xFF);
          }
        }
      }
    }
  }

  if (cur > 0) {
    cur = fix_pdf_xref(input, cur, MAX_FILE_SIZE);
  }

  return cur;
}

static void cleanup_target_state(void) {
  extern void *chunk_map[];
  extern int chunk_count;
  extern void __real_free(void *ptr);

  extern FILE *file_map[];
  extern int file_count;
  extern int __real_fclose(FILE * stream);

  extern void *global_data_start;
  extern void *global_data_backup;
  extern size_t global_data_size;

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
  char *args[] = {(char *)target, (char *)"-q", (char *)file_path,
                  (char *)"/dev/null", NULL};

  extern char *start_address;
  extern char *end_address;
  extern int in_target;

  if (!start_address || !end_address || end_address <= start_address) {
    return -1;
  }

  size_t cov_size = (size_t)(end_address - start_address);

  if (cov_size > COVERAGE_MAP_SIZE) {
    fprintf(stderr, "[fatal] coverage section too large: %zu > %u\n", cov_size,
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

int is_new_coverage() {
  int new_covered = 0;
  for (int i = 0; i < COVERAGE_MAP_SIZE; i++) {
    int prev = coverage_map[i];
    int curr = coverage_map_tmp[i];

    if (prev < curr) {
      new_covered = 1;
      coverage_map[i] = curr;
    }
  }
  return new_covered;
}

void add_valid_seed_struct_to_array(unsigned char *input, size_t input_size) {
  if (struct_count >= struct_array_size) {
    if (struct_count == 0) {
      struct_array_size = 100;
      int new_struct_array_size = struct_array_size;

      seed_struct *new_struct_ptr = realloc(
          seed_struct_array, new_struct_array_size * sizeof(seed_struct));

      seed_struct_array = new_struct_ptr;
    }

    else {
      int new_struct_array_size = struct_array_size * 2;

      seed_struct *new_struct_ptr = realloc(
          seed_struct_array, new_struct_array_size * sizeof(seed_struct));

      seed_struct_array = new_struct_ptr;
      struct_array_size = new_struct_array_size;
    }
  }

  seed_struct_array[struct_count].data = (unsigned char *)malloc(input_size);

  memcpy(seed_struct_array[struct_count].data, input, input_size);
  seed_struct_array[struct_count].size = input_size;
  struct_count++;
}

void load_seeds_from_files(const char *dirname, const char *target_path) {
  DIR *dir;
  struct dirent *dir_struct;

  dir = opendir(dirname);
  if (dir == NULL) {
    perror("opendir failed");
    return;
  }

  while ((dir_struct = readdir(dir)) != NULL) {
    printf("Found file: %s\n", dir_struct->d_name);
    if (strstr(dir_struct->d_name, ".pdf") != NULL) {
      char filepath[512];
      snprintf(filepath, sizeof(filepath), "%s/%s", dirname,
               dir_struct->d_name);
      int init_crashed = 0;
      FILE *file = fopen(filepath, "rb");
      if (file == NULL) {
        perror("Failed to open seed file");
        continue;
      }

      if (file) {

        fseek(file, 0, SEEK_END);
        size_t file_size = ftell(file);
        fseek(file, 0, SEEK_SET);
        unsigned char *file_contents_ptr = (unsigned char *)malloc(file_size);

        fread(file_contents_ptr, 1, file_size, file);

        fclose(file);

        int init_exec_result = call_targetMain_reentrant(target_path, filepath);

        int newed = is_new_coverage();
        if (newed) {
          add_valid_seed_struct_to_array(file_contents_ptr, file_size);

          interesting_cnt++;
          char path_for_save[64];
          snprintf(path_for_save, sizeof(path_for_save),
                   "/dev/shm/trial14/interesting/interesting_%llu.txt",
                   interesting_cnt);
          FILE *seed_file = fopen(path_for_save, "w");
          if (seed_file) {
            fwrite(file_contents_ptr, 1, file_size, seed_file);
            fclose(seed_file);
          }
        }
        free(file_contents_ptr);
      }
    }
  }
  closedir(dir);
}

static int init_runtime_state(void) {
  extern char *start_address;
  extern char *end_address;

  if (!start_address || !end_address || end_address <= start_address) {
    fprintf(stderr, "[fatal] sanitizer counter section not initialized\n");
    return -1;
  }

  setup_global_snapshot();

  fprintf(stderr, "[init] counter_size=%zu, global_data_size=%zu\n",
          (size_t)(end_address - start_address), global_data_size);

  if (global_data_size > 0 && (!global_data_start || !global_data_backup)) {
    fprintf(stderr, "[fatal] global snapshot metadata inconsistent\n");
    return -1;
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    printf("Usage: %s <target_path> <number_of_trials>\n", argv[0]);
    return 1;
  }
  printf("starting fuzzerMain with target\n");

  int sigkilled_cnt = 0;
  int crash_cnt = 0;

  const char *target = argv[1];
  const int total_trials = atoi(argv[2]);
  char out_name[256];
  snprintf(out_name, sizeof(out_name),
           "/dev/shm/trial14/Real_World_Fuzzer_Result.txt");
  FILE *out = fopen(out_name, "w");
  if (!out) {
    perror("Failed to open output file");
    return 1;
  }
  coverage_map_tmp = mmap(NULL, COVERAGE_MAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  if (init_runtime_state() != 0) {
    fclose(out);
    return 1;
  }

  load_dict("pdf.dict");
  for (int s = 0; s < dict_size; s++) {
    printf("%s ", dictionary[s]);
  }

  char *temp_filename = "/dev/shm/trial14/temp_fuzz_input.pdf";
  int fd = open(temp_filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    perror("Failed to create temporary file");
    fclose(out);
    return 1;
  }

  if (ftruncate(fd, MAX_FILE_SIZE) == -1) {
    perror("Failed to set size of temporary file");
    close(fd);
    fclose(out);
    return 1;
  }

  unsigned char *map_ptr = (unsigned char *)mmap(
      NULL, MAX_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map_ptr == MAP_FAILED) {
    perror("mmap failed");
    close(fd);
    fclose(out);
    return 1;
  }

  load_seeds_from_files("seeds", target);
  int seedset_count_init = struct_count;

  for (int trial = 0; trial < total_trials; trial++) {
    printf("\n#########################################\n");
    printf("############### Trial %d/%d ###############\n", trial + 1,
           total_trials);
    printf("#########################################\n");
    unsigned long long num_testcases = 0;
    unsigned seed = 1;
    srand(seed);
    struct_count = seedset_count_init;
    int crash_found = 0;
    memset(coverage_map, 0, sizeof(coverage_map));
    memset(coverage_map_tmp, 0, COVERAGE_MAP_SIZE);

    fprintf(out, "Trial: %d/%d\n", trial + 1, total_trials);
    fprintf(out, "Target: %s\n", target);
    fprintf(out, "Seed: %u\n", seed);

    time_t start_time = time(NULL);
    while (difftime(time(NULL), start_time) < 86400) {
      ftruncate(fd, MAX_FILE_SIZE);
      size_t curr_size = pick_seed_and_mutate(map_ptr);
      ftruncate(fd, curr_size);
      num_testcases++;

      int execution = call_targetMain_reentrant(target, temp_filename);

      int is_new = is_new_coverage();
      if (is_new) {

        add_valid_seed_struct_to_array(map_ptr, curr_size);
        interesting_cnt++;
        char path_for_save[64];
        snprintf(path_for_save, sizeof(path_for_save),
                 "/dev/shm/trial14/interesting/interesting_%llu.txt",
                 interesting_cnt);
        FILE *seed_file = fopen(path_for_save, "w");
        if (seed_file) {
          fwrite(map_ptr, 1, curr_size, seed_file);
          fclose(seed_file);
        }
      }

      if (num_testcases % 10000 == 0) {
        edges = 0;
        for (int i = 0; i < COVERAGE_MAP_SIZE; i++) {
          if (coverage_map[i] > 0)
            edges++;
        }

        time_t elapsed = difftime(time(NULL), start_time);
        fprintf(out, "--------------------------------------------------\n");
        fprintf(out, "| %-25s |  %5lld  cases         |\n",
                "# of test cases generated", num_testcases);
        fprintf(out, "| %-25s |  %5ld seconds        |\n", "Time elaped",
                (long)elapsed);
        fprintf(out, "| %-25s |  %5d                |\n",
                "# of struct in array", struct_count);
        fprintf(out, "| %-25s |  %5llu                |\n",
                "Total edges covered", edges);
        fprintf(out, "| %-25s |  %5.2f exec/s        |\n", "Throughput",
                (double)num_testcases / elapsed);
        fprintf(out, "--------------------------------------------------\n");

        fflush(out);

        printf("--------------------------------------------------\n");
        printf("| %-25s |  %5lld  cases         |\n",
               "# of test cases generated", num_testcases);
        printf("| %-25s |  %5ld seconds        |\n", "Time elaped",
               (long)elapsed);
        printf("| %-25s |  %5d                |\n", "# of struct in array",
               struct_count);
        printf("| %-25s |  %5llu                |\n", "Total edges covered",
               edges);
        printf("| %-25s |  %5.2f exec/s        |\n", "Throughput",
               (double)num_testcases / elapsed);
        printf("--------------------------------------------------\n");
      }
    }

    time_t final_elapsed = difftime(time(NULL), start_time);
    if (!crash_found) {
      fprintf(out, "\nResult: No crash found\n");
      fprintf(out, "Time elapsed: %ld\n", (long)final_elapsed);
      fprintf(out, "Test cases generated: %llu\n", num_testcases);
      fprintf(out, "Trial ends\n");
      fprintf(out, "------------------------------------------\n");
    }
  }
  munmap(map_ptr, MAX_FILE_SIZE);
  close(fd);
  fclose(out);
  return 0;
}