#include "app.h"
#include "gfx.h"
#include "sha256.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pspiofilemgr.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define BODY_CAP (64 * 1024)
#define CHUNK 32768
Peer peers[MAX_PEERS];
int peer_count;
char own_ip[16] = "0.0.0.0";
static int listener = -1, udp = -1, cancelled;
static uint64_t announced;
static struct {
  TransferFile files[MAX_FILES];
  int count, active, complete;
  char id[65], ip[16];
  uint64_t touched, total, done;
} session;
int network_pending(void) { return session.active; }
void network_cancel_pending(void) { session.active = 0; }
typedef struct {
  char method[12], url[2048];
  uint64_t length;
  int status, expect, chunked;
} Http;
static void nonblock(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
static int ready(int fd, int write, uint64_t timeout) {
  uint64_t start = now_ms();
  while (running) {
    if (keys() & PSP_CTRL_CIRCLE) {
      cancelled = 1;
      return 0;
    }
    if (now_ms() - start >= timeout)
      return 0;
    fd_set f;
    FD_ZERO(&f);
    FD_SET(fd, &f);
    struct timeval tv = {0, 50000};
    int r = select(fd + 1, write ? NULL : &f, write ? &f : NULL, NULL, &tv);
    if (r > 0)
      return 1;
    if (r < 0 && errno != EINTR)
      return 0;
    pause_frame();
  }
  return 0;
}
static int write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  while (len) {
    if (!ready(fd, 1, 30000))
      return 0;
    int n = send(fd, p, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (n <= 0)
      return 0;
    p += n;
    len -= n;
  }
  return 1;
}
static int read_some(int fd, void *buf, size_t len, uint64_t timeout) {
  for (;;) {
    if (!ready(fd, 0, timeout))
      return -1;
    int n = recv(fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    return n;
  }
}
static int read_exact(int fd, void *buf, size_t len, uint64_t timeout) {
  char *p = buf;
  while (len) {
    int n = read_some(fd, p, len, timeout);
    if (n <= 0)
      return 0;
    p += n;
    len -= n;
  }
  return 1;
}
static int headers(int fd, Http *h, uint64_t timeout) {
  char buf[8192];
  unsigned used = 0;
  memset(h, 0, sizeof *h);
  uint64_t deadline = now_ms() + timeout;
  while (used < sizeof buf - 1) {
    uint64_t t = now_ms();
    if (t >= deadline || !read_exact(fd, buf + used, 1, deadline - t))
      return 0;
    used++;
    if (used >= 4 && !memcmp(buf + used - 4, "\r\n\r\n", 4))
      break;
  }
  if (used >= sizeof buf - 1)
    return 0;
  buf[used] = 0;
  char *save = NULL, *ln = strtok_r(buf, "\r\n", &save);
  if (!ln)
    return 0;
  if (!strncmp(ln, "HTTP/1.", 7)) {
    if (sscanf(ln, "HTTP/%*s %d", &h->status) != 1)
      return 0;
  } else if (sscanf(ln, "%11s %2047s", h->method, h->url) != 2)
    return 0;
  int has_length = 0;
  while ((ln = strtok_r(NULL, "\r\n", &save))) {
    if (!strncasecmp(ln, "Content-Length:", 15)) {
      if (has_length++)
        return 0;
      char *p = ln + 15;
      while (*p == ' ')
        p++;
      if (!isdigit((unsigned char)*p))
        return 0;
      errno = 0;
      char *end;
      h->length = strtoull(p, &end, 10);
      while (*end == ' ')
        end++;
      if (errno || *end)
        return 0;
    }
    if (!strncasecmp(ln, "Transfer-Encoding:", 18)) {
      char *p = ln + 18;
      while (*p == ' ')
        p++;
      if (strcasecmp(p, "chunked") || h->chunked)
        return 0;
      h->chunked = 1;
    }
    if (!strncasecmp(ln, "Expect:", 7) && strstr(ln, "100-continue"))
      h->expect = 1;
  }
  return !(has_length && h->chunked);
}
typedef struct {
  int fd, chunked, done, need_crlf;
  uint64_t left;
} Body;
static int body_line(int fd, char *out, unsigned cap) {
  unsigned n = 0;
  uint64_t deadline = now_ms() + 15000;
  while (n + 1 < cap && now_ms() < deadline) {
    if (!read_exact(fd, out + n, 1, deadline - now_ms()))
      return 0;
    if (out[n++] == '\n') {
      if (n < 2 || out[n - 2] != '\r')
        return 0;
      out[n - 2] = 0;
      return 1;
    }
  }
  return 0;
}
static Body body_start(int fd, const Http *h) {
  Body b = {fd, h->chunked, 0, 0, h->chunked ? 0 : h->length};
  return b;
}
static int body_read(Body *b, void *out, unsigned cap) {
  if (b->done)
    return 0;
  if (b->chunked && !b->left) {
    if (b->need_crlf) {
      char crlf[2];
      if (!read_exact(b->fd, crlf, 2, 15000) || memcmp(crlf, "\r\n", 2))
        return -1;
    }
    char ln[256];
    if (!body_line(b->fd, ln, sizeof ln) || !isxdigit((unsigned char)ln[0]))
      return -1;
    errno = 0;
    char *end;
    b->left = strtoull(ln, &end, 16);
    if (errno || (*end && *end != ';') || b->left > MAX_FILE_BYTES)
      return -1;
    if (!b->left) {
      unsigned trailers = 0;
      do {
        if (!body_line(b->fd, ln, sizeof ln) || ++trailers > 32)
          return -1;
      } while (*ln);
      b->done = 1;
      return 0;
    }
    b->need_crlf = 1;
  }
  if (!b->left) {
    b->done = 1;
    return 0;
  }
  unsigned want = b->left > cap ? cap : (unsigned)b->left;
  int n = read_some(b->fd, out, want, 30000);
  if (n <= 0)
    return -1;
  b->left -= n;
  return n;
}
static cJSON *read_json(int fd, const Http *h) {
  if (!h->chunked && (!h->length || h->length > BODY_CAP))
    return NULL;
  char *buf = malloc(BODY_CAP + 1);
  if (!buf)
    return NULL;
  Body body = body_start(fd, h);
  unsigned used = 0;
  int n = 0;
  while (used < BODY_CAP &&
         (n = body_read(&body, buf + used, BODY_CAP - used)) > 0)
    used += n;
  char extra;
  int tail = n < 0 ? -1 : body_read(&body, &extra, 1);
  cJSON *j = NULL;
  if (tail == 0 && body.done) {
    buf[used] = 0;
    j = cJSON_ParseWithLengthOpts(buf, used + 1, NULL, 1);
  }
  free(buf);
  return j;
}
static int response(int fd, int code, const char *body) {
  char head[256];
  if (!body)
    body = "";
  size_t n = strlen(body);
  snprintf(
      head, sizeof head,
      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
      "%u\r\nConnection: close\r\n\r\n",
      code, code == 200 ? "OK" : "Error", (unsigned)n);
  return write_all(fd, head, strlen(head)) && write_all(fd, body, n);
}
static void json_response(int fd, int code, cJSON *j) {
  char *s = cJSON_PrintUnformatted(j);
  response(fd, s ? code : 500, s ? s : "{}");
  free(s);
}
static cJSON *info(void) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "alias", "PocketSend PSP");
  cJSON_AddStringToObject(j, "version", "2.1");
  cJSON_AddStringToObject(j, "deviceModel", "PSP-3000");
  cJSON_AddStringToObject(j, "deviceType", "mobile");
  cJSON_AddStringToObject(j, "fingerprint", settings.fingerprint);
  cJSON_AddNumberToObject(j, "port", PORT);
  cJSON_AddStringToObject(j, "protocol", "http");
  cJSON_AddBoolToObject(j, "download", 0);
  return j;
}
static const char *jsonstr(const cJSON *j, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
static void remember(const cJSON *j, const char *ip) {
  const char *fp = jsonstr(j, "fingerprint"), *alias = jsonstr(j, "alias"),
             *proto = jsonstr(j, "protocol");
  if (!alias || !strcmp(ip, own_ip) ||
      (fp && !strcmp(fp, settings.fingerprint)))
    return;
  const cJSON *port = cJSON_GetObjectItemCaseSensitive(j, "port");
  int p = cJSON_IsNumber(port) ? port->valueint : PORT;
  if (p < 1 || p > 65535)
    return;
  int at = -1;
  for (int i = 0; i < peer_count; i++)
    if (!strcmp(peers[i].ip, ip) && peers[i].port == p)
      at = i;
  if (at < 0) {
    if (peer_count >= MAX_PEERS)
      return;
    at = peer_count++;
  }
  snprintf(peers[at].ip, sizeof peers[at].ip, "%s", ip);
  snprintf(peers[at].alias, sizeof peers[at].alias, "%s", alias);
  peers[at].port = p;
  peers[at].https = !proto || strcmp(proto, "http");
  peers[at].seen = now_ms();
}
static void announce(int is_announce) {
  if (udp < 0)
    return;
  cJSON *j = info();
  cJSON_AddBoolToObject(j, "announce", is_announce);
  char *s = cJSON_PrintUnformatted(j);
  if (s) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(PORT);
    inet_aton("224.0.0.167", &dst.sin_addr);
    sendto(udp, s, strlen(s), 0, (struct sockaddr *)&dst, sizeof dst);
    free(s);
  }
  cJSON_Delete(j);
}
static int connect_to(const char *ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  nonblock(fd);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (!inet_aton(ip, &a.sin_addr)) {
    close(fd);
    return -1;
  }
  int r = connect(fd, (struct sockaddr *)&a, sizeof a);
  if (r < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
    close(fd);
    return -1;
  }
  if (r < 0) {
    if (!ready(fd, 1, 10000)) {
      close(fd);
      return -1;
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
      close(fd);
      return -1;
    }
  }
  return fd;
}
static int request_head(int fd, const Peer *p, const char *url,
                        uint64_t length) {
  char b[2600];
  int n = snprintf(b, sizeof b,
                   "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: "
                   "application/octet-stream\r\nContent-Length: "
                   "%llu\r\nConnection: close\r\n\r\n",
                   url, p->ip, p->port, (unsigned long long)length);
  return n > 0 && n < (int)sizeof b && write_all(fd, b, n);
}
static cJSON *request_json(const Peer *p, const char *url, cJSON *j,
                           int *status, uint64_t timeout) {
  *status = 0;
  char *s = cJSON_PrintUnformatted(j);
  if (!s)
    return NULL;
  int fd = connect_to(p->ip, p->port);
  cJSON *result = NULL;
  if (fd >= 0) {
    char head[2600];
    int n = snprintf(
        head, sizeof head,
        "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: "
        "application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
        url, p->ip, p->port, (unsigned)strlen(s));
    if (n > 0 && n < (int)sizeof head && write_all(fd, head, n) &&
        write_all(fd, s, strlen(s))) {
      Http h;
      if (headers(fd, &h, timeout)) {
        *status = h.status;
        result = read_json(fd, &h);
      }
    }
    close(fd);
  }
  free(s);
  return result;
}
int network_connect(void) {
  int ids[32], count = 0, sel = 0;
  char names[32][128];
  for (int i = 1; i <= 99 && count < 32; i++)
    if (sceUtilityCheckNetParam(i) == 0) {
      netData d;
      memset(&d, 0, sizeof d);
      if (sceUtilityGetNetParam(i, PSP_NETPARAM_NAME, &d) >= 0) {
        ids[count] = i;
        snprintf(names[count], 128, "%s", d.asString);
        count++;
      }
    }
  if (!count) {
    message("No saved Wi-Fi profile",
            "Save your phone hotspot in the PSP Network Settings first.");
    return 0;
  }
  while (running) {
    screen("Connect to Wi-Fi");
    line(0, "Select your saved hotspot profile");
    int top = sel / 5 * 5;
    for (int i = top; i < count && i < top + 5; i++)
      gfx_item(i - top, names[i], "", i == sel, 0, now_ms());
    gfx_hint("X  Connect                     O  Back",
             "Up / Down  Browse profiles");
    unsigned k = keys();
    if (k & PSP_CTRL_CIRCLE)
      return 0;
    if (k & PSP_CTRL_UP)
      sel = (sel + count - 1) % count;
    if (k & PSP_CTRL_DOWN)
      sel = (sel + 1) % count;
    if (k & PSP_CTRL_CROSS)
      break;
    pause_frame();
  }
  if (!running)
    return 0;
  int r = sceNetApctlConnect(ids[sel]);
  if (r < 0) {
    char b[80];
    snprintf(b, sizeof b, "Connection failed: 0x%08X", r);
    message("Wi-Fi", b);
    return 0;
  }
  uint64_t start = now_ms();
  while (running && now_ms() - start < 45000) {
    int state = 0;
    sceNetApctlGetState(&state);
    screen("Connecting");
    line(1, "%s", names[sel]);
    line(3, "Connecting to network (%d)...", state);
    gfx_hint("O  Cancel", "Keep the WLAN switch on.");
    if (state == 4) {
      union SceNetApctlInfo data;
      if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &data) >= 0) {
        snprintf(own_ip, sizeof own_ip, "%s", data.ip);
        return 1;
      }
    }
    if (keys() & PSP_CTRL_CIRCLE)
      break;
    pause_frame();
  }
  sceNetApctlDisconnect();
  message("Wi-Fi connection incomplete",
          "Check WLAN switch and hotspot. Try again.");
  return 0;
}
int network_start(void) {
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0)
    return 0;
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = INADDR_ANY;
  if (bind(listener, (struct sockaddr *)&a, sizeof a) < 0 ||
      listen(listener, 8) < 0) {
    close(listener);
    listener = -1;
    return 0;
  }
  nonblock(listener);
  udp = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp >= 0) {
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(udp, (struct sockaddr *)&a, sizeof a) < 0) {
      close(udp);
      udp = -1;
    } else {
      struct ip_mreq m;
      inet_aton("224.0.0.167", &m.imr_multiaddr);
      inet_aton(own_ip, &m.imr_interface);
      setsockopt(udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
      setsockopt(udp, IPPROTO_IP, IP_MULTICAST_IF, &m.imr_interface,
                 sizeof m.imr_interface);
      nonblock(udp);
    }
  }
  announce(1);
  announced = now_ms();
  return 1;
}
void network_stop(void) {
  if (listener >= 0)
    close(listener);
  if (udp >= 0)
    close(udp);
  listener = udp = -1;
  session.active = 0;
  sceNetApctlDisconnect();
}
static int accept_batch(int fd, const char *sender) {
  char folder[PATH_CAP];
  strcpy(folder, settings.last);
  int dummy = 0;
  uint64_t deadline = now_ms() + 120000;
  while (running) {
    char peek;
    if (now_ms() >= deadline || recv(fd, &peek, 1, MSG_PEEK) == 0)
      return 0;
    screen("Incoming files");
    line(0, "From: %s", sender);
    line(1, "%d files / %.2f MiB", session.count, session.total / 1048576.0);
    gfx_round(18, 111, 444, 76, 8, PANEL);
    gfx_text(30, 116, "SAVE TO", 0, MUTED, 400);
    gfx_marquee(30, 140, folder, 1, INK, 416, now_ms());
    gfx_round(18, 190, 444, 29, 6, PANEL);
    gfx_text(20, 194, session.files[0].name, 0, INK, 440);
    gfx_hint("X  Accept   O  Reject   Triangle  Folder",
             "Square  Suggest folder    R  Review files");
    unsigned k = keys();
    if (k & PSP_CTRL_RTRIGGER) {
      int item = 0;
      while (running) {
        screen("Review incoming files");
        line(0, "File %d of %d", item + 1, session.count);
        gfx_wrap(20, 92, session.files[item].name, 1, INK, 440, 3);
        gfx_wrap(20, 178, format_note(session.files[item].name), 0, MUTED, 440,
                 2);
        gfx_hint("Left / Right  Previous / next",
                 "O  Back to transfer request");
        unsigned review = keys();
        if (review & PSP_CTRL_CIRCLE)
          break;
        if (review & PSP_CTRL_LEFT)
          item = (item + session.count - 1) % session.count;
        if (review & PSP_CTRL_RIGHT)
          item = (item + 1) % session.count;
        pause_frame();
      }
    }
    if (k & PSP_CTRL_CIRCLE)
      return 0;
    if (k & PSP_CTRL_TRIANGLE)
      browse(0, folder, NULL, &dummy);
    if (k & PSP_CTRL_SQUARE) {
      const char *s = suggest_folder(session.files[0].name);
      int same = 1;
      for (int i = 1; i < session.count; i++)
        if (strcmp(s, suggest_folder(session.files[i].name)))
          same = 0;
      if (!same)
        s = "ms0:/DOWNLOADS";
      sceIoMkdir(s, 0777);
      strcpy(folder, s);
    }
    if (k & PSP_CTRL_CROSS) {
      uint64_t space = free_space();
      if (space == UINT64_MAX) {
        message("Cannot check free space",
                "Memory Stick free-space query failed. Transfer rejected.");
        return 0;
      }
      if (space < session.total + 65536) {
        message("Not enough free space",
                "Free space must hold the complete incoming batch.");
        return 0;
      }
      SceIoStat dir;
      if (sceIoGetstat(folder, &dir) < 0 || !FIO_S_ISDIR(dir.st_mode)) {
        message("Folder unavailable", "Choose an existing folder.");
        continue;
      }
      for (int i = 0; i < session.count; i++) {
        TransferFile *f = &session.files[i];
        int len =
            snprintf(f->path, sizeof f->path, "%s%s%s", folder,
                     folder[strlen(folder) - 1] == '/' ? "" : "/", f->name);
        if (len < 0 || len >= (int)sizeof f->path)
          return 0;
        SceIoStat st;
        if (sceIoGetstat(f->path, &st) >= 0) {
          if (FIO_S_ISDIR(st.st_mode)) {
            message("Name belongs to a folder", f->name);
            return 0;
          }
          if (!confirm("Replace existing file?", f->name))
            return 0;
          f->overwrite = 1;
        }
      }
      strcpy(settings.last, folder);
      save_settings();
      return 1;
    }
    pause_frame();
  }
  return 0;
}
static void handle_prepare(int fd, const Http *h, const char *ip) {
  if (session.active) {
    response(fd, 409, "{}");
    return;
  }
  if (h->expect)
    write_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
  cJSON *j = read_json(fd, h);
  char error[128];
  memset(&session, 0, sizeof session);
  if (!j || !parse_files(j, session.files, &session.count, error)) {
    response(fd, 400, "{}");
    cJSON_Delete(j);
    return;
  }
  const cJSON *inf = cJSON_GetObjectItemCaseSensitive(j, "info");
  const char *sender = jsonstr(inf, "alias");
  if (!sender)
    sender = ip;
  for (int i = 0; i < session.count; i++)
    session.total += session.files[i].size;
  if (!accept_batch(fd, sender)) {
    response(fd, 403, "{}");
    cJSON_Delete(j);
    return;
  }
  random_id(session.id);
  snprintf(session.ip, sizeof session.ip, "%s", ip);
  cJSON *reply = cJSON_CreateObject(),
        *f = cJSON_AddObjectToObject(reply, "files");
  cJSON_AddStringToObject(reply, "sessionId", session.id);
  for (int i = 0; i < session.count; i++) {
    random_id(session.files[i].token);
    cJSON_AddStringToObject(f, session.files[i].id, session.files[i].token);
  }
  session.active = 1;
  session.touched = now_ms();
  json_response(fd, 200, reply);
  cJSON_Delete(reply);
  cJSON_Delete(j);
}
static int match_session(const char *url, const char *ip) {
  char id[128];
  return session.active && !strcmp(ip, session.ip) &&
         query_value(url, "sessionId", id, sizeof id) &&
         !strcmp(id, session.id);
}
static void handle_upload(int fd, const Http *h, const char *ip) {
  char id[ID_CAP], token[128];
  if (!match_session(h->url, ip) ||
      !query_value(h->url, "fileId", id, sizeof id) ||
      !query_value(h->url, "token", token, sizeof token)) {
    response(fd, 403, "{}");
    return;
  }
  TransferFile *f = NULL;
  int index = 0;
  for (int i = 0; i < session.count; i++)
    if (!strcmp(session.files[i].id, id)) {
      f = &session.files[i];
      index = i;
      break;
    }
  if (!f || strcmp(f->token, token) || f->done) {
    response(fd, 403, "{}");
    return;
  }
  if (!h->chunked && h->length != f->size) {
    response(fd, 400, "{}");
    return;
  }
  char temp[PATH_CAP + 80], backup[PATH_CAP + 80];
  snprintf(temp, sizeof temp, "%s.%s.part", f->path, f->token);
  snprintf(backup, sizeof backup, "%s.%s.backup", f->path, f->token);
  SceUID out = sceIoOpen(temp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0666);
  if (out < 0) {
    response(fd, 500, "{}");
    return;
  }
  if (h->expect)
    write_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
  unsigned char *buf = malloc(CHUNK);
  uint64_t done = 0, start = now_ms();
  int good = buf != NULL;
  SHA256_CTX sha;
  sha256_init(&sha);
  Body body = body_start(fd, h);
  while (good && done < f->size) {
    size_t want = f->size - done > CHUNK ? CHUNK : (size_t)(f->size - done);
    int n = body_read(&body, buf, want);
    if (n <= 0) {
      good = 0;
      break;
    }
    int off = 0;
    while (off < n) {
      int written = sceIoWrite(out, buf + off, n - off);
      if (written <= 0) {
        good = 0;
        break;
      }
      off += written;
    }
    if (!good)
      break;
    sha256_update(&sha, buf, n);
    done += n;
    session.touched = now_ms();
    progress(f->name, done, f->size, start, index + 1, session.count,
             session.done + done, session.total);
  }
  if (good) {
    char extra;
    if (body_read(&body, &extra, 1) != 0)
      good = 0;
  }
  free(buf);
  if (sceIoClose(out) < 0)
    good = 0;
  unsigned char digest[32];
  char hex[65];
  sha256_final(&sha, digest);
  for (int i = 0; i < 32; i++)
    sprintf(hex + i * 2, "%02x", digest[i]);
  int code = 500;
  if (good && f->sha[0] && strcasecmp(hex, f->sha)) {
    good = 0;
    code = 422;
  }
  int moved = 0;
  if (good) {
    SceIoStat st;
    if (sceIoGetstat(f->path, &st) >= 0) {
      if (!f->overwrite || sceIoGetstat(backup, &st) >= 0 ||
          sceIoRename(f->path, backup) < 0)
        good = 0;
      else
        moved = 1;
    }
    if (good && sceIoRename(temp, f->path) < 0)
      good = 0;
    if (!good && moved)
      sceIoRename(backup, f->path);
    if (good && moved)
      sceIoRemove(backup);
  }
  if (!good) {
    sceIoRemove(temp);
    response(fd, cancelled ? 409 : code, "{}");
    session.active = 0;
    message("Transfer stopped",
            code == 422
                ? "Checksum mismatch. Original file retained."
                : "Incomplete file removed. Check connection and storage.");
    return;
  }
  f->done = 1;
  session.done += done;
  session.complete++;
  session.touched = now_ms();
  response(fd, 200, "");
  if (session.complete == session.count) {
    session.active = 0;
    message("Transfer complete",
            "All accepted files saved to the selected folder.");
  }
}
static void serve(int fd, const char *ip) {
  Http h;
  cancelled = 0;
  if (!headers(fd, &h, 15000))
    return;
  char route[2048];
  strcpy(route, h.url);
  char *q = strchr(route, '?');
  if (q)
    *q = 0;
  if (!strcmp(h.method, "GET") && !strcmp(route, "/api/localsend/v2/info")) {
    cJSON *j = info();
    json_response(fd, 200, j);
    cJSON_Delete(j);
  } else if (strcmp(h.method, "POST"))
    response(fd, 405, "{}");
  else if (!strcmp(route, "/api/localsend/v2/register")) {
    cJSON *j = read_json(fd, &h);
    if (j) {
      remember(j, ip);
      cJSON *reply = info();
      json_response(fd, 200, reply);
      cJSON_Delete(reply);
    } else
      response(fd, 400, "{}");
    cJSON_Delete(j);
  } else if (!strcmp(route, "/api/localsend/v2/prepare-upload"))
    handle_prepare(fd, &h, ip);
  else if (!strcmp(route, "/api/localsend/v2/upload"))
    handle_upload(fd, &h, ip);
  else if (!strcmp(route, "/api/localsend/v2/cancel")) {
    if (match_session(h.url, ip)) {
      session.active = 0;
      response(fd, 200, "");
    } else
      response(fd, 403, "{}");
  } else
    response(fd, 404, "{}");
}
void network_poll(void) {
  if (session.active && now_ms() - session.touched > 300000)
    session.active = 0;
  if (udp >= 0) {
    char b[4096];
    struct sockaddr_in a;
    socklen_t size = sizeof a;
    int n = recvfrom(udp, b, sizeof b - 1, 0, (struct sockaddr *)&a, &size);
    if (n > 0) {
      b[n] = 0;
      cJSON *j = cJSON_Parse(b);
      if (j) {
        remember(j, inet_ntoa(a.sin_addr));
        const char *fp = jsonstr(j, "fingerprint");
        if (cJSON_IsTrue(cJSON_GetObjectItem(j, "announce")) && fp &&
            strcmp(fp, settings.fingerprint))
          announce(0);
        cJSON_Delete(j);
      }
    }
  }
  if (now_ms() - announced > 10000) {
    announce(1);
    announced = now_ms();
  }
  struct sockaddr_in a;
  socklen_t size = sizeof a;
  int fd = accept(listener, (struct sockaddr *)&a, &size);
  if (fd >= 0) {
    nonblock(fd);
    char ip[16];
    snprintf(ip, sizeof ip, "%s", inet_ntoa(a.sin_addr));
    serve(fd, ip);
    close(fd);
  }
}
void manual_peer(void) {
  char ip[32] = "", port[8] = "53317";
  if (!keyboard("Device IPv4 address", ip, sizeof ip))
    return;
  struct in_addr a;
  if (!inet_aton(ip, &a)) {
    message("Invalid IPv4 address", "Example: 192.168.43.1");
    return;
  }
  if (!keyboard("LocalSend port", port, sizeof port))
    return;
  char *end;
  long p = strtol(port, &end, 10);
  if (*end || p < 1 || p > 65535) {
    message("Invalid port", "Use 53317 unless changed in LocalSend.");
    return;
  }
  Peer peer;
  memset(&peer, 0, sizeof peer);
  snprintf(peer.ip, sizeof peer.ip, "%s", inet_ntoa(a));
  peer.port = p;
  cancelled = 0;
  screen("Connecting by IP");
  line(1, "%s:%d", peer.ip, peer.port);
  gfx_hint("O  Cancel", "Connecting to device...");
  cJSON *j = info();
  int status;
  cJSON *reply =
      request_json(&peer, "/api/localsend/v2/register", j, &status, 15000);
  cJSON_Delete(j);
  if (status == 200 && reply) {
    /* Register responses may omit these fields. The transport just used
       and manually entered port remain authoritative in that case. */
    if (!cJSON_GetObjectItemCaseSensitive(reply, "protocol"))
      cJSON_AddStringToObject(reply, "protocol", "http");
    if (!cJSON_GetObjectItemCaseSensitive(reply, "port"))
      cJSON_AddNumberToObject(reply, "port", peer.port);
    remember(reply, peer.ip);
    message("Device added",
            jsonstr(reply, "alias") ? jsonstr(reply, "alias") : peer.ip);
  } else
    message("Could not connect",
            "Check IP, hotspot isolation, and disable LocalSend encryption.");
  cJSON_Delete(reply);
}
static void cancel_remote(const Peer *peer, const char *id) {
  char encoded[384], url[512];
  if (!url_encode(id, encoded, sizeof encoded))
    return;
  snprintf(url, sizeof url, "/api/localsend/v2/cancel?sessionId=%s", encoded);
  cJSON *j = cJSON_CreateObject();
  int status;
  cJSON *r = request_json(peer, url, j, &status, 3000);
  cJSON_Delete(r);
  cJSON_Delete(j);
}
void send_menu(void) {
  if (session.active) {
    message("Incoming batch active",
            "Finish or cancel the incoming batch first.");
    return;
  }
  int sel = 0;
  Peer peer;
  while (running) {
    screen("Choose receiver");
    line(0, "Your PSP: %s:%d", own_ip, PORT);
    int top = sel / 5 * 5;
    for (int i = top; i < peer_count && i < top + 5; i++)
      gfx_item(i - top, peers[i].alias, peers[i].https ? "HTTPS" : peers[i].ip,
               i == sel, 0, now_ms());
    if (!peer_count) {
      line(2, "No devices found yet.");
      line(3, "Open LocalSend or add an IP.");
    }
    gfx_hint("X  Select   Triangle  Add IP   O  Back",
             "Up / Down  Browse devices");
    network_poll();
    unsigned k = keys();
    if (k & PSP_CTRL_CIRCLE)
      return;
    if (k & PSP_CTRL_TRIANGLE)
      manual_peer();
    if (k & PSP_CTRL_UP && peer_count)
      sel = (sel + peer_count - 1) % peer_count;
    if (k & PSP_CTRL_DOWN && peer_count)
      sel = (sel + 1) % peer_count;
    if (k & PSP_CTRL_CROSS && peer_count) {
      peer = peers[sel];
      if (peer.https) {
        message(
            "Encryption not supported in this build",
            "Disable encryption in receiver LocalSend settings, then retry.");
        continue;
      }
      break;
    }
    pause_frame();
  }
  if (!running)
    return;
  TransferFile *files = calloc(MAX_FILES, sizeof *files);
  if (!files)
    return;
  int count = 0;
  char folder[PATH_CAP];
  strcpy(folder, settings.last);
  if (!browse(1, folder, files, &count)) {
    free(files);
    return;
  }
  cJSON *root = cJSON_CreateObject(),
        *list = cJSON_AddObjectToObject(root, "files");
  cJSON_AddItemToObject(root, "info", info());
  uint64_t total = 0;
  for (int i = 0; i < count; i++) {
    snprintf(files[i].id, sizeof files[i].id, "%d", i);
    cJSON *f = cJSON_AddObjectToObject(list, files[i].id);
    cJSON_AddStringToObject(f, "id", files[i].id);
    cJSON_AddStringToObject(f, "fileName", files[i].name);
    cJSON_AddNumberToObject(f, "size", files[i].size);
    cJSON_AddStringToObject(f, "fileType", "application/octet-stream");
    total += files[i].size;
  }
  screen("Waiting for receiver");
  line(0, "Accept the transfer on:");
  line(2, "%s", peer.alias);
  gfx_hint("O  Cancel", "Waiting up to 120 seconds");
  cancelled = 0;
  int status;
  cJSON *reply = request_json(&peer, "/api/localsend/v2/prepare-upload", root,
                              &status, 120000);
  if (status == 401) {
    char pin[32] = "";
    cJSON_Delete(reply);
    reply = NULL;
    if (keyboard("Receiver PIN", pin, sizeof pin)) {
      char encoded[96], url[160];
      url_encode(pin, encoded, sizeof encoded);
      snprintf(url, sizeof url, "/api/localsend/v2/prepare-upload?pin=%s",
               encoded);
      reply = request_json(&peer, url, root, &status, 120000);
    }
  }
  cJSON_Delete(root);
  const char *sid = jsonstr(reply, "sessionId");
  char session_id[128] = "";
  if (status != 200 || !sid || !*sid || strlen(sid) >= sizeof session_id) {
    cJSON_Delete(reply);
    free(files);
    message("Transfer not accepted",
            status == 403 ? "Receiver declined."
            : status == 401
                ? "Receiver PIN was not accepted."
                : "Check receiver, encryption setting, and connection.");
    return;
  }
  strcpy(session_id, sid);
  cJSON *tokens = cJSON_GetObjectItemCaseSensitive(reply, "files");
  int accepted = 0;
  for (int i = 0; i < count; i++) {
    const char *token = jsonstr(tokens, files[i].id);
    if (token && *token && strlen(token) < sizeof files[i].token) {
      strcpy(files[i].token, token);
      accepted++;
    }
  }
  cJSON_Delete(reply);
  if (!accepted) {
    free(files);
    message("Nothing accepted", "Receiver did not accept any files.");
    return;
  }
  uint64_t batch_done = 0;
  total = 0;
  for (int i = 0; i < count; i++)
    if (files[i].token[0])
      total += files[i].size;
  int ok = 1, index = 0;
  unsigned char *buf = malloc(CHUNK);
  if (!buf)
    ok = 0;
  for (int i = 0; i < count && ok; i++) {
    TransferFile *f = &files[i];
    if (!f->token[0])
      continue;
    index++;
    char si[384], fi[384], tk[256], url[1400];
    if (!url_encode(session_id, si, sizeof si) ||
        !url_encode(f->id, fi, sizeof fi) ||
        !url_encode(f->token, tk, sizeof tk)) {
      ok = 0;
      break;
    }
    snprintf(url, sizeof url,
             "/api/localsend/v2/upload?sessionId=%s&fileId=%s&token=%s", si, fi,
             tk);
    SceUID in = sceIoOpen(f->path, PSP_O_RDONLY, 0);
    if (in < 0) {
      ok = 0;
      break;
    }
    SceIoStat st;
    if (sceIoGetstat(f->path, &st) < 0 || (uint64_t)st.st_size != f->size) {
      sceIoClose(in);
      ok = 0;
      break;
    }
    int fd = connect_to(peer.ip, peer.port);
    if (fd < 0) {
      sceIoClose(in);
      ok = 0;
      break;
    }
    if (!request_head(fd, &peer, url, f->size))
      ok = 0;
    uint64_t done = 0, start = now_ms();
    while (ok && done < f->size) {
      unsigned want =
          f->size - done > CHUNK ? CHUNK : (unsigned)(f->size - done);
      int n = sceIoRead(in, buf, want);
      if (n <= 0 || !write_all(fd, buf, n)) {
        ok = 0;
        break;
      }
      done += n;
      progress(f->name, done, f->size, start, index, accepted,
               batch_done + done, total);
    }
    sceIoClose(in);
    Http h;
    if (ok && (!headers(fd, &h, 30000) || h.status != 200))
      ok = 0;
    close(fd);
    if (ok)
      batch_done += done;
  }
  free(buf);
  free(files);
  if (!ok) {
    cancel_remote(&peer, session_id);
    message("Sending stopped",
            "Connection, cancellation, or file error. Retry the batch.");
  } else
    message("Sending complete",
            accepted < count
                ? "Receiver accepted only part of the selected batch."
                : "All selected files delivered to the receiver.");
}
