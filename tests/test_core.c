#include "core.h"
#include "sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int tests;
#define CHECK(x)                                                               \
  do {                                                                         \
    tests++;                                                                   \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static int parse(const char *s) {
  cJSON *j = cJSON_Parse(s);
  TransferFile f[MAX_FILES];
  int n;
  char error[128];
  int r = parse_files(j, f, &n, error);
  cJSON_Delete(j);
  return r;
}
int main(void) {
  CHECK(safe_name("photo.jpg"));
  CHECK(safe_name("save 1.sav"));
  CHECK(!safe_name("../photo.jpg"));
  CHECK(!safe_name("a/b"));
  CHECK(!safe_name("a\\b"));
  CHECK(!safe_name("ms0:file"));
  CHECK(!safe_name(".."));
  CHECK(!safe_name("."));
  CHECK(!safe_name(""));
  CHECK(!safe_name("a\n"));
  CHECK(!safe_name("a."));
  CHECK(!safe_name("a "));
  CHECK(safe_folder("ms0:/"));
  CHECK(safe_folder("ms0:/MUSIC"));
  CHECK(safe_folder("ms0:/PSP/SAVEDATA/"));
  CHECK(!safe_folder("flash0:/"));
  CHECK(!safe_folder("ms0:/../flash0"));
  CHECK(!safe_folder("ms0:/a//b"));
  CHECK(!safe_folder("ms0:/a/./b"));
  CHECK(parse(
      "{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\",\"size\":0}}}"));
  CHECK(parse("{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\",\"size\":"
              "2147483647}}}"));
  CHECK(!parse("{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\","
               "\"size\":2147483648}}}"));
  CHECK(!parse(
      "{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\",\"size\":-1}}}"));
  CHECK(!parse("{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\","
               "\"size\":0.5}}}"));
  CHECK(!parse("{\"files\":{\"0\":{\"id\":\"wrong\",\"fileName\":\"a.txt\","
               "\"size\":0}}}"));
  CHECK(!parse("{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"../"
               "a.txt\",\"size\":0}}}"));
  CHECK(!parse("{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\","
               "\"size\":0,\"sha256\":\"bad\"}}}"));
  CHECK(!parse("{\"files\":{}}"));
  CHECK(!parse("{}"));
  CHECK(
      !parse("{\"files\":{\"0\":{\"id\":\"0\",\"fileName\":\"a.txt\",\"size\":"
             "0},\"1\":{\"id\":\"1\",\"fileName\":\"A.TXT\",\"size\":0}}}"));
  cJSON *j = cJSON_CreateObject(), *files = cJSON_AddObjectToObject(j, "files");
  for (int i = 0; i < 33; i++) {
    char id[16];
    sprintf(id, "%d", i);
    cJSON *f = cJSON_AddObjectToObject(files, id);
    cJSON_AddStringToObject(f, "id", id);
    cJSON_AddStringToObject(f, "fileName", id);
    cJSON_AddNumberToObject(f, "size", 1);
  }
  char *s = cJSON_PrintUnformatted(j);
  CHECK(!parse(s));
  free(s);
  cJSON_Delete(j);
  char b[128];
  CHECK(query_value("/upload?sessionId=abc&fileId=a%2Fb%20c&token=x", "fileId",
                    b, sizeof b) &&
        !strcmp(b, "a/b c"));
  CHECK(!query_value("/upload?fileId=%00", "fileId", b, sizeof b));
  CHECK(!query_value("/upload?fileId=%GG", "fileId", b, sizeof b));
  CHECK(!query_value("/upload?fileId=%2", "fileId", b, sizeof b));
  CHECK(!query_value("/upload?fileId=12345", "fileId", b, 5));
  CHECK(!query_value("/upload?token=abc", "fileId", b, sizeof b));
  CHECK(url_encode("a/b c+?", b, sizeof b) && !strcmp(b, "a%2Fb%20c%2B%3F"));
  CHECK(!url_encode("/", b, 3));
  CHECK(!strcmp(suggest_folder("TEST.MP3"), "ms0:/MUSIC"));
  CHECK(!strcmp(suggest_folder("x.mp4"), "ms0:/VIDEO"));
  CHECK(!strcmp(suggest_folder("save.bin"), "ms0:/DOWNLOADS"));
  SHA256_CTX ctx;
  unsigned char digest[32];
  char hex[65];
  sha256_init(&ctx);
  sha256_update(&ctx, (const unsigned char *)"abc", 3);
  sha256_final(&ctx, digest);
  for (int i = 0; i < 32; i++)
    sprintf(hex + i * 2, "%02x", digest[i]);
  CHECK(!strcmp(
      hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  sha256_init(&ctx);
  for (int i = 0; i < 1000000; i++)
    sha256_update(&ctx, (const unsigned char *)"a", 1);
  sha256_final(&ctx, digest);
  for (int i = 0; i < 32; i++)
    sprintf(hex + i * 2, "%02x", digest[i]);
  CHECK(!strcmp(
      hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  printf("PASS: %d checks (paths, metadata, size limits, queries, SHA-256)\n",
         tests);
  return 0;
}
