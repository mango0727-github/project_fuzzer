#include "fuzzer_internal.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

seed_struct *seed_struct_array = NULL;
int struct_count = 0;
int struct_array_size = 0;
unsigned long long interesting_cnt = 0;

static int read_file_to_buffer(const char *filepath, unsigned char **buffer_out,
                               size_t *size_out) {
  FILE *file = fopen(filepath, "rb");
  if (!file) {
    perror("Failed to open seed file");
    return -1;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }

  long end = ftell(file);
  if (end < 0) {
    fclose(file);
    return -1;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return -1;
  }

  size_t file_size = (size_t)end;
  unsigned char *buffer = (unsigned char *)malloc(file_size);
  if (!buffer) {
    fclose(file);
    return -1;
  }

  size_t nread = fread(buffer, 1, file_size, file);
  fclose(file);

  if (nread != file_size) {
    free(buffer);
    return -1;
  }

  *buffer_out = buffer;
  *size_out = file_size;
  return 0;
}

static void save_interesting_seed(const unsigned char *data, size_t size) {
  char save_path[256];
  snprintf(save_path, sizeof(save_path),
           DEFAULT_INTERESTING_DIR "/interesting_%llu.pdf", interesting_cnt);

  FILE *seed_file = fopen(save_path, "wb");
  if (!seed_file) {
    return;
  }

  fwrite(data, 1, size, seed_file);
  fclose(seed_file);
}

void add_valid_seed_struct_to_array(const unsigned char *input,
                                    size_t input_size) {
  if (struct_count >= struct_array_size) {
    int new_capacity = (struct_array_size == 0) ? INITIAL_SEED_CAPACITY
                                                : struct_array_size * 2;

    seed_struct *new_ptr = (seed_struct *)realloc(
        seed_struct_array, (size_t)new_capacity * sizeof(seed_struct));
    if (!new_ptr) {
      fprintf(stderr, "[warn] seed array realloc failed\n");
      return;
    }

    seed_struct_array = new_ptr;
    struct_array_size = new_capacity;
  }

  seed_struct_array[struct_count].data = (unsigned char *)malloc(input_size);
  if (!seed_struct_array[struct_count].data) {
    fprintf(stderr, "[warn] seed buffer alloc failed\n");
    return;
  }

  memcpy(seed_struct_array[struct_count].data, input, input_size);
  seed_struct_array[struct_count].size = input_size;
  struct_count++;
}

void load_seeds_from_files(const char *dirname, const char *target_path) {
  DIR *dir = opendir(dirname);
  if (!dir) {
    perror("opendir failed");
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    printf("Found file: %s\n", entry->d_name);

    if (!strstr(entry->d_name, ".pdf")) {
      continue;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", dirname, entry->d_name);

    unsigned char *file_contents_ptr = NULL;
    size_t file_size = 0;
    if (read_file_to_buffer(filepath, &file_contents_ptr, &file_size) != 0) {
      continue;
    }

    int init_exec_result = call_targetMain_reentrant(target_path, filepath);

    if (init_exec_result != -1 && is_new_coverage()) {
      add_valid_seed_struct_to_array(file_contents_ptr, file_size);

      interesting_cnt++;
      save_interesting_seed(file_contents_ptr, file_size);
    }

    free(file_contents_ptr);
  }

  closedir(dir);
}
