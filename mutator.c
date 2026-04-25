#define _GNU_SOURCE
#include "fuzzer_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *dictionary[MAX_DICTIONARY_ENTRIES];
int dict_size = 0;

void load_dict(const char *file_name) {
  FILE *file = fopen(file_name, "r");
  if (!file) {
    fprintf(stderr, "dictionary open failed: %s\n", file_name);
    return;
  }

  char line[1024];

  while (fgets(line, sizeof(line), file)) {
    if (dict_size >= MAX_DICTIONARY_ENTRIES) {
      fprintf(stderr, "dictionary capacity reached\n");
      break;
    }

    if (line[0] == '#' || strlen(line) < 3) {
      continue;
    }

    const char *start = strchr(line, '"');
    char *end = strrchr(line, '"');

    if (start && end && end > start) {
      *end = '\0';
      dictionary[dict_size] = strdup(start + 1);
      if (dictionary[dict_size]) {
        dict_size++;
      }
    }
  }

  fclose(file);
}

size_t fix_pdf_xref(unsigned char *input, size_t cur, size_t max_size) {
  if (cur < 20) {
    return cur;
  }

  char root_ref[32] = "1 0 R";
  const unsigned char *root_ptr = memmem(input, cur, "/Root", 5);
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
    const unsigned char *obj_ptr = memmem(search_ptr, remain, " 0 obj", 6);
    if (!obj_ptr) {
      break;
    }

    const unsigned char *num_ptr = obj_ptr - 1;
    while (num_ptr > input && isspace(*num_ptr)) {
      num_ptr--;
    }
    while (num_ptr > input && isdigit(*num_ptr)) {
      num_ptr--;
    }
    if (!isdigit(*num_ptr)) {
      num_ptr++;
    }

    int obj_id = atoi((char *)num_ptr);
    if (obj_id > 0 && obj_id < MAX_OBJECTS) {
      obj_offsets[obj_id] = (int)(num_ptr - input);
      if (obj_id > max_obj_id) {
        max_obj_id = obj_id;
      }
    }

    size_t step = (size_t)(obj_ptr - search_ptr) + 6;
    search_ptr += step;
    remain -= step;
  }

  if (max_obj_id == 0) {
    return cur;
  }

  int startxref_offset = (int)cur;
  char buf[256];
  int len;

  len = snprintf(buf, sizeof(buf), "\nxref\n0 %d\n0000000000 65535 f \n",
                 max_obj_id + 1);
  if (cur + (size_t)len > max_size) {
    return (size_t)startxref_offset;
  }
  memcpy(input + cur, buf, (size_t)len);
  cur += (size_t)len;

  for (int i = 1; i <= max_obj_id; i++) {
    if (obj_offsets[i] > 0) {
      len = snprintf(buf, sizeof(buf), "%010d 00000 n \n", obj_offsets[i]);
    } else {
      len = snprintf(buf, sizeof(buf), "0000000000 00000 f \n");
    }

    if (cur + (size_t)len > max_size) {
      return (size_t)startxref_offset;
    }
    memcpy(input + cur, buf, (size_t)len);
    cur += (size_t)len;
  }

  len = snprintf(buf, sizeof(buf),
                 "trailer\n<< /Size %d /Root %s >>\nstartxref\n%d\n%%EOF\n",
                 max_obj_id + 1, root_ref, startxref_offset);
  if (cur + (size_t)len > max_size) {
    return (size_t)startxref_offset;
  }
  memcpy(input + cur, buf, (size_t)len);
  cur += (size_t)len;

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
  const seed_struct *a = &seed_struct_array[idx_a];
  size_t cur = a->size;

  if (cur > 0) {
    memcpy(input, a->data, cur);
  } else {
    return 0;
  }

  if (struct_count >= 2 && (rand() % 100) < 30) {
    int idx_b = rand() % struct_count;
    seed_struct *b = &seed_struct_array[idx_b];

    unsigned char *obj_a_start = memmem(input, cur, " obj", 4);
    const unsigned char *obj_a_end =
        obj_a_start ? memmem(obj_a_start, cur - (size_t)(obj_a_start - input),
                             "endobj", 6)
                    : NULL;
    unsigned char *obj_b_start = memmem(b->data, b->size, " obj", 4);
    const unsigned char *obj_b_end =
        obj_b_start
            ? memmem(obj_b_start, b->size - (size_t)(obj_b_start - b->data),
                     "endobj", 6)
            : NULL;

    if (obj_a_start && obj_a_end && obj_b_start && obj_b_end) {
      obj_a_start += 4;
      obj_b_start += 4;

      size_t len_a = (size_t)(obj_a_end - obj_a_start);
      size_t len_b = (size_t)(obj_b_end - obj_b_start);

      if (len_b > 0 && cur - len_a + len_b < MAX_FILE_SIZE) {
        memmove(obj_a_start + len_b, obj_a_start + len_a,
                cur - (size_t)(obj_a_end - input));
        memcpy(obj_a_start, obj_b_start, len_b);
        cur = cur - len_a + len_b;
      }
    }
  }

  if (cur == 0) {
    return 0;
  }

  if (dict_size > 0 && (rand() % 100) < 30) {
    const char *kw = dictionary[rand() % dict_size];
    size_t kw_len = strlen(kw);

    if (kw_len > 0 && cur + kw_len < MAX_FILE_SIZE) {
      size_t pos = (size_t)rand() % cur;
      memmove(input + pos + kw_len, input + pos, cur - pos);
      memcpy(input + pos, kw, kw_len);
      cur += kw_len;
    }
  }

  if ((rand() % 100) < 50) {
    const char *danger_keys[] = {
        "/Length", "/Size",   "/Count",   "/Columns",   "/Predictor", "/W",
        "/W2",     "/Ascent", "/Descent", "/CapHeight", "/First",     "/N"};
    int num_keys = (int)(sizeof(danger_keys) / sizeof(danger_keys[0]));

    for (int i = 0; i < 3; i++) {
      const char *key = danger_keys[rand() % num_keys];
      const unsigned char *ptr = memmem(input, cur, key, strlen(key));

      if (ptr) {
        size_t pos = (size_t)(ptr - input) + strlen(key);
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
    const unsigned char *stream_end = memmem(input, cur, "endstream", 9);

    if (stream_start && stream_end && stream_start < stream_end) {
      unsigned char *payload_start = stream_start + 6;
      while (payload_start < stream_end &&
             (*payload_start == '\r' || *payload_start == '\n')) {
        payload_start++;
      }

      if (stream_end > payload_start) {
        size_t stream_len = (size_t)(stream_end - payload_start);

        int mutations = 1 + (int)(stream_len / 50);
        if (mutations > 30) {
          mutations = 30;
        }

        for (int m = 0; m < mutations; m++) {
          size_t pos = (size_t)rand() % stream_len;
          if (rand() % 2 == 0) {
            payload_start[pos] ^= (unsigned char)(1u << (rand() % 8));
          } else {
            payload_start[pos] = (unsigned char)(rand() & 0xFF);
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
