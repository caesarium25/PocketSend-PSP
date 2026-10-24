#include "core.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
int safe_name(const char *s) {
  if (!s || !*s || strlen(s) >= NAME_CAP || !strcmp(s, ".") || !strcmp(s, ".."))
    return 0;
  size_t n = strlen(s);
  if (s[n - 1] == '.' || s[n - 1] == ' ')
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p < 32 || *p == 127 || strchr("/\\:*?\"<>|", *p))
      return 0;
  return 1;
}
int safe_folder(const char *s) {
  if (!s || strncmp(s, "ms0:/", 5) || strlen(s) >= PATH_CAP - NAME_CAP - 80)
    return 0;
  char tmp[PATH_CAP];
  strcpy(tmp, s);
  char *p = tmp + 5;
  while (*p) {
    char *end = strchr(p, '/');
    if (end)
      *end = 0;
    if (!safe_name(p))
      return 0;
    if (!end)
      break;
    p = end + 1;
  }
  return 1;
}
static const char *str(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
int parse_files(const cJSON *root, TransferFile *files, int *count,
                char *error) {
  const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "files"), *v;
  *count = 0;
  if (!cJSON_IsObject(obj)) {
    strcpy(error, "Missing file list");
    return 0;
  }
  cJSON_ArrayForEach(v, obj) {
    if (*count >= MAX_FILES) {
      strcpy(error, "Maximum 32 files per batch");
      return 0;
    }
    const char *name = str(v, "fileName"), *id = str(v, "id"),
               *sha = str(v, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(v, "size");
    if (!safe_name(name) || !id || !*id || strlen(id) >= ID_CAP || !v->string ||
        strcmp(id, v->string)) {
      strcpy(error, "Invalid name/id; send flat files only");
      return 0;
    }
    if (!cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
        size->valuedouble < 0 || size->valuedouble > MAX_FILE_BYTES ||
        floor(size->valuedouble) != size->valuedouble) {
      strcpy(error, "File exceeds prototype 2 GiB limit");
      return 0;
    }
    if (sha) {
      if (strlen(sha) != 64) {
        strcpy(error, "Invalid SHA-256");
        return 0;
      }
      for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)sha[i])) {
          strcpy(error, "Invalid SHA-256");
          return 0;
        }
    }
    for (int i = 0; i < *count; i++)
      if (!strcmp(files[i].id, id) || !strcasecmp(files[i].name, name)) {
        strcpy(error, "Duplicate file name or id");
        return 0;
      }
    TransferFile *f = &files[(*count)++];
    memset(f, 0, sizeof(*f));
    strcpy(f->id, id);
    strcpy(f->name, name);
    f->size = (uint64_t)size->valuedouble;
    if (sha)
      strcpy(f->sha, sha);
  }
  if (!*count) {
    strcpy(error, "Empty batch");
    return 0;
  }
  return 1;
}
static const char *ext(const char *s) {
  const char *p = strrchr(s, '.');
  return p ? p : "";
}
const char *suggest_folder(const char *name) {
  const char *e = ext(name);
  if (!strcasecmp(e, ".mp3") || !strcasecmp(e, ".wav") ||
      !strcasecmp(e, ".aac"))
    return "ms0:/MUSIC";
  if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") ||
      !strcasecmp(e, ".png") || !strcasecmp(e, ".bmp"))
    return "ms0:/PICTURE";
  if (!strcasecmp(e, ".mp4"))
    return "ms0:/VIDEO";
  return "ms0:/DOWNLOADS";
}
const char *format_note(const char *name) {
  const char *e = ext(name);
  if (!strcasecmp(e, ".mp4") || !strcasecmp(e, ".aac") ||
      !strcasecmp(e, ".wav"))
    return "Playback depends on codec/profile.";
  if (!strcasecmp(e, ".mp3") || !strcasecmp(e, ".jpg") ||
      !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png") ||
      !strcasecmp(e, ".bmp"))
    return "Stored as-is; media is not converted.";
  return "File can be stored; PSP playback is not assured.";
}
static int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
int query_value(const char *url, const char *key, char *out, unsigned cap) {
  const char *p = strchr(url, '?');
  if (!p || !cap)
    return 0;
  p++;
  size_t k = strlen(key);
  while (*p) {
    const char *end = strchr(p, '&');
    if (!end)
      end = p + strlen(p);
    if ((size_t)(end - p) > k && !strncmp(p, key, k) && p[k] == '=') {
      p += k + 1;
      unsigned n = 0;
      while (p < end) {
        int c = (unsigned char)*p++;
        if (c == '%') {
          if (end - p < 2)
            return 0;
          int a = hex(p[0]), b = hex(p[1]);
          if (a < 0 || b < 0)
            return 0;
          c = 16 * a + b;
          p += 2;
        } else if (c == '+')
          c = ' ';
        if (!c || n + 1 >= cap)
          return 0;
        out[n++] = (char)c;
      }
      out[n] = 0;
      return 1;
    }
    p = *end ? end + 1 : end;
  }
  return 0;
}
int url_encode(const char *src, char *dst, unsigned cap) {
  unsigned n = 0;
  const char *h = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
    if (isalnum(*p) || strchr("-_.~", *p)) {
      if (n + 1 >= cap)
        return 0;
      dst[n++] = *p;
    } else {
      if (n + 3 >= cap)
        return 0;
      dst[n++] = '%';
      dst[n++] = h[*p >> 4];
      dst[n++] = h[*p & 15];
    }
  }
  if (n >= cap)
    return 0;
  dst[n] = 0;
  return 1;
}
