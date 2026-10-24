#ifndef POCKET_CORE_H
#define POCKET_CORE_H
#include "cJSON.h"
#include <stdint.h>
#define MAX_FILES 32
#define PATH_CAP 512
#define NAME_CAP 160
#define ID_CAP 128
#define MAX_FILE_BYTES 2147483647ULL
typedef struct {
  char id[ID_CAP], name[NAME_CAP], token[65], sha[65];
  char path[PATH_CAP];
  uint64_t size;
  int overwrite, done;
} TransferFile;
int safe_name(const char *s);
int safe_folder(const char *s);
int parse_files(const cJSON *root, TransferFile *files, int *count,
                char *error);
const char *suggest_folder(const char *name);
const char *format_note(const char *name);
int query_value(const char *url, const char *key, char *out, unsigned cap);
int url_encode(const char *src, char *dst, unsigned cap);
#endif
