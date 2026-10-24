#include "app.h"
#include "gfx.h"
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <psputils.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
Settings settings;
char app_dir[PATH_CAP] = "ms0:/PSP/GAME/PocketSend";
static unsigned previous;
static uint64_t next_repeat, last_progress;
static SceKernelUtilsMt19937Context rng;
uint64_t now_ms(void) { return sceKernelGetSystemTimeWide() / 1000; }
void random_id(char out[65]) {
  static int initialized;
  if (!initialized) {
    sceKernelUtilsMt19937Init(&rng, (unsigned)sceKernelGetSystemTimeWide());
    initialized = 1;
  }
  for (int i = 0; i < 8; i++)
    sprintf(out + i * 8, "%08x", (unsigned)sceKernelUtilsMt19937UInt(&rng));
}
void pause_frame(void) {
  gfx_present();
  scePowerTick(PSP_POWER_TICK_SUSPEND);
  if (!settings.dim)
    scePowerTick(PSP_POWER_TICK_DISPLAY);
  sceKernelDelayThreadCB(16000);
}
unsigned keys(void) {
  gfx_present();
  SceCtrlData p;
  sceCtrlPeekBufferPositive(&p, 1);
  unsigned b = p.Buttons, pressed = b & ~previous;
  if (b) {
    scePowerTick(PSP_POWER_TICK_DISPLAY);
    uint64_t t = now_ms();
    if (b != previous)
      next_repeat = t + 400;
    else if (t >= next_repeat) {
      pressed |=
          b & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
      next_repeat = t + 130;
    }
  }
  previous = b;
  return pressed;
}

void line(int row, const char *fmt, ...) {
  char b[512];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  if (row >= 0 && row < 6)
    gfx_text(20, 58 + row * 26, b, 0, row == 0 ? INK : MUTED, 440);
}
void screen(const char *title) { gfx_screen(title); }

int confirm(const char *title, const char *detail) {
  screen(title);
  gfx_round(18, 67, 444, 142, 10, PANEL);
  gfx_wrap(32, 82, detail, 1, INK, 416, 4);
  gfx_hint("X  Confirm                 O  Cancel",
           "Your existing file stays until transfer completes.");
  while (running) {
    unsigned k = keys();
    if (k & PSP_CTRL_CROSS)
      return 1;
    if (k & PSP_CTRL_CIRCLE)
      return 0;
    pause_frame();
  }
  return 0;
}
void message(const char *title, const char *detail) {
  screen(title);
  gfx_round(18, 67, 444, 142, 10, PANEL);
  gfx_wrap(32, 82, detail, 1, INK, 416, 4);
  gfx_hint("X / O  Back", "");
  while (running) {
    if (keys() & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE))
      break;
    pause_frame();
  }
}

int keyboard(const char *title, char *out, unsigned cap) {
  const char *chars =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-:";
  int sel = 0, len = (int)strlen(out), total = (int)strlen(chars);
  while (running) {
    screen(title);
    gfx_round(18, 55, 444, 30, 6, PANEL);
    const char *visible = out;
    while (*visible && gfx_width(visible, 1) > 402)
      visible++;
    gfx_text(28, 58, visible, 1, MINT, 404);
    for (int i = 0; i < total; i++) {
      int x = 18 + (i % 14) * 32, y = 92 + (i / 14) * 25;
      gfx_round(x, y, 28, 22, 4, i == sel ? SELECTED : PANEL);
      char c[2] = {chars[i], 0};
      gfx_text(x + 7, y, c, 1, i == sel ? MINT : INK, 24);
      if (chars[i] == ' ')
        gfx_rect(x + 8, y + 17, 12, 2, MUTED);
    }
    gfx_hint("X  Type     Square  Delete     Start  Done",
             "D-pad  Move                     O  Cancel");
    unsigned k = keys();
    if (k & PSP_CTRL_CIRCLE)
      return 0;
    if (k & PSP_CTRL_START)
      return 1;
    if (k & PSP_CTRL_LEFT)
      sel = (sel + total - 1) % total;
    if (k & PSP_CTRL_RIGHT)
      sel = (sel + 1) % total;
    if (k & PSP_CTRL_UP)
      sel = (sel + total - 14) % total;
    if (k & PSP_CTRL_DOWN)
      sel = (sel + 14) % total;
    if ((k & PSP_CTRL_SQUARE) && len)
      out[--len] = 0;
    if ((k & PSP_CTRL_CROSS) && (unsigned)len + 1 < cap) {
      out[len++] = chars[sel];
      out[len] = 0;
    }
    pause_frame();
  }
  return 0;
}

static int exists_dir(const char *p) {
  SceIoStat st;
  return sceIoGetstat(p, &st) >= 0 && FIO_S_ISDIR(st.st_mode);
}
static void config_path(char *out, const char *suffix) {
  snprintf(out, PATH_CAP, "%.480s/%.24s", app_dir, suffix);
}
void load_settings(void) {
  memset(&settings, 0, sizeof settings);
  strcpy(settings.last, "ms0:/DOWNLOADS");
  settings.dim = 1;
  random_id(settings.fingerprint);
  char path[PATH_CAP];
  config_path(path, "settings.json");
  FILE *f = fopen(path, "rb");
  if (f) {
    char b[8192];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    cJSON *j = cJSON_Parse(b);
    if (j) {
      cJSON *v = cJSON_GetObjectItem(j, "last");
      if (cJSON_IsString(v) && safe_folder(v->valuestring))
        strcpy(settings.last, v->valuestring);
      v = cJSON_GetObjectItem(j, "fingerprint");
      if (cJSON_IsString(v) && strlen(v->valuestring) == 64)
        strcpy(settings.fingerprint, v->valuestring);
      v = cJSON_GetObjectItem(j, "dim");
      if (cJSON_IsBool(v))
        settings.dim = cJSON_IsTrue(v);
      cJSON *a = cJSON_GetObjectItem(j, "favorites");
      cJSON_ArrayForEach(v, a) if (cJSON_IsString(v) &&
                                   safe_folder(v->valuestring) &&
                                   settings.favorite_count < 8)
          strcpy(settings.favorites[settings.favorite_count++], v->valuestring);
      cJSON_Delete(j);
    }
  }
  if (!settings.favorite_count) {
    const char *defaults[] = {"ms0:/DOWNLOADS", "ms0:/MUSIC", "ms0:/PICTURE",
                              "ms0:/VIDEO"};
    for (int i = 0; i < 4; i++)
      strcpy(settings.favorites[settings.favorite_count++], defaults[i]);
  }
  sceIoMkdir("ms0:/DOWNLOADS", 0777);
  if (!exists_dir(settings.last))
    strcpy(settings.last, "ms0:/");
}
void save_settings(void) {
  cJSON *j = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(j, "favorites");
  cJSON_AddStringToObject(j, "last", settings.last);
  cJSON_AddStringToObject(j, "fingerprint", settings.fingerprint);
  cJSON_AddBoolToObject(j, "dim", settings.dim);
  for (int i = 0; i < settings.favorite_count; i++)
    cJSON_AddItemToArray(a, cJSON_CreateString(settings.favorites[i]));
  char *s = cJSON_PrintUnformatted(j), path[PATH_CAP], tmp[PATH_CAP];
  config_path(path, "settings.json");
  config_path(tmp, "settings.tmp");
  if (s) {
    FILE *f = fopen(tmp, "wb");
    if (f) {
      size_t n = strlen(s);
      int good = fwrite(s, 1, n, f) == n;
      if (fclose(f))
        good = 0;
      if (good) {
        sceIoRemove(path);
        sceIoRename(tmp, path);
      } else
        sceIoRemove(tmp);
    }
    free(s);
  }
  cJSON_Delete(j);
}
uint64_t free_space(void) {
  struct {
    unsigned max, free, max2, sector, cluster;
  } d;
  void *p = &d;
  memset(&d, 0, sizeof d);
  if (sceIoDevctl("ms0:", 0x02425818, &p, sizeof p, NULL, 0) < 0)
    return UINT64_MAX;
  return (uint64_t)d.free * d.sector * d.cluster;
}
typedef struct {
  char name[256];
  int dir;
  uint64_t size;
} Entry;
static int entry_cmp(const void *a, const void *b) {
  const Entry *x = a, *y = b;
  return x->dir != y->dir ? y->dir - x->dir : strcasecmp(x->name, y->name);
}
static int favorite_pick(char *folder) {
  int s = 0;
  while (running) {
    screen("Favorite folders");
    line(0, "Choose a saved destination");
    int top = s / 5 * 5;
    for (int i = top; i < settings.favorite_count && i < top + 5; i++)
      gfx_item(i - top, settings.favorites[i], "", i == s, 1, now_ms());
    gfx_hint("X  Open     O  Back     Square  Remove",
             "Up / Down  Browse favorites");
    unsigned k = keys();
    if (k & PSP_CTRL_CIRCLE)
      return 0;
    if (!settings.favorite_count)
      return 0;
    if (k & PSP_CTRL_UP)
      s = (s + settings.favorite_count - 1) % settings.favorite_count;
    if (k & PSP_CTRL_DOWN)
      s = (s + 1) % settings.favorite_count;
    if (k & PSP_CTRL_SQUARE) {
      for (int i = s; i < settings.favorite_count - 1; i++)
        strcpy(settings.favorites[i], settings.favorites[i + 1]);
      settings.favorite_count--;
      s = 0;
      save_settings();
    }
    if (k & PSP_CTRL_CROSS) {
      const char *p = settings.favorites[s];
      if (!exists_dir(p))
        sceIoMkdir(p, 0777);
      if (exists_dir(p)) {
        strcpy(folder, p);
        return 1;
      }
      message("Folder unavailable", "Create its parent folders first.");
    }
    pause_frame();
  }
  return 0;
}
int browse(int send_mode, char *folder, TransferFile *files, int *count) {
  Entry *entries = calloc(512, sizeof(Entry));
  if (!entries)
    return 0;
  int selected = 0, n = 0, reload = 1;
  *count = 0;
  while (running) {
    if (reload) {
      n = 0;
      selected = 0;
      SceUID d = sceIoDopen(folder);
      if (d < 0) {
        message("Cannot open folder", folder);
        free(entries);
        return 0;
      }
      SceIoDirent e;
      memset(&e, 0, sizeof e);
      while (n < 512 && sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..") &&
            (send_mode || FIO_S_ISDIR(e.d_stat.st_mode))) {
          snprintf(entries[n].name, sizeof entries[n].name, "%s", e.d_name);
          entries[n].dir = FIO_S_ISDIR(e.d_stat.st_mode);
          entries[n].size = e.d_stat.st_size;
          n++;
        }
        memset(&e, 0, sizeof e);
      }
      sceIoDclose(d);
      qsort(entries, n, sizeof(Entry), entry_cmp);
      reload = 0;
    }
    screen(send_mode ? "Select files to send" : "Choose destination");
    gfx_marquee(20, 54, folder, 0, MUTED, 335, now_ms());
    char page[48];
    snprintf(page, sizeof page, "%d / %d", n ? selected + 1 : 0, n);
    gfx_text(370, 54, page, 0, MUTED, 90);
    int top = selected / 5 * 5;
    for (int i = top; i < n && i < top + 5; i++) {
      int marked = 0;
      char full[PATH_CAP];
      snprintf(full, sizeof full, "%s%s%s", folder,
               folder[strlen(folder) - 1] == '/' ? "" : "/", entries[i].name);
      for (int j = 0; j < *count; j++)
        if (!strcmp(files[j].path, full))
          marked = 1;
      gfx_item(i - top, entries[i].name, marked ? "SELECTED" : "",
               i == selected, entries[i].dir ? 1 : 2, now_ms());
    }
    if (!n)
      line(2, "No matching files or folders.");
    char action[100];
    if (send_mode)
      snprintf(action, sizeof action,
               "X  Open / mark   Start  Send (%d)   O  Back", *count);
    else
      snprintf(action, sizeof action, "X  Open   Start  Use folder   O  Back");
    gfx_hint(action, "Triangle  Favorites     Square  More controls");
    unsigned k = keys();
    if (k & PSP_CTRL_SQUARE)
      message("Browser controls",
              "L: New folder. R: Add favorite. Select: Cancel. Up/Down: "
              "Scroll. Maximum 512 entries per folder.");
    if (k & PSP_CTRL_SELECT) {
      free(entries);
      return 0;
    }
    if (k & PSP_CTRL_UP && n)
      selected = (selected + n - 1) % n;
    if (k & PSP_CTRL_DOWN && n)
      selected = (selected + 1) % n;
    if (k & PSP_CTRL_CIRCLE) {
      if (strlen(folder) <= 5) {
        free(entries);
        return 0;
      }
      char *p = strrchr(folder, '/');
      if (p == folder + 4)
        p[1] = 0;
      else if (p)
        *p = 0;
      reload = 1;
    }
    if (k & PSP_CTRL_TRIANGLE) {
      if (favorite_pick(folder))
        reload = 1;
    }
    if (k & PSP_CTRL_RTRIGGER) {
      int found = 0;
      for (int i = 0; i < settings.favorite_count; i++)
        if (!strcmp(folder, settings.favorites[i]))
          found = 1;
      if (!found && settings.favorite_count < 8) {
        strcpy(settings.favorites[settings.favorite_count++], folder);
        save_settings();
        message("Favorite added", folder);
      } else
        message("Favorites", "Already saved, or all 8 slots are full.");
    }
    if (k & PSP_CTRL_LTRIGGER) {
      char name[NAME_CAP] = "", p[PATH_CAP];
      if (keyboard("New folder", name, sizeof name) && safe_name(name)) {
        int len = snprintf(p, sizeof p, "%s%s%s", folder,
                           folder[strlen(folder) - 1] == '/' ? "" : "/", name);
        if (len > 0 && len < (int)sizeof p && safe_folder(p) &&
            sceIoMkdir(p, 0777) >= 0)
          reload = 1;
        else
          message("Cannot create folder", "Check name and available space.");
      }
    }
    if ((k & PSP_CTRL_CROSS) && n) {
      Entry *e = &entries[selected];
      char p[PATH_CAP];
      int len = snprintf(p, sizeof p, "%s%s%s", folder,
                         folder[strlen(folder) - 1] == '/' ? "" : "/", e->name);
      if (len < 0 || len >= (int)sizeof p) {
        message("Path too long", "Choose a shorter folder path.");
        continue;
      }
      if (e->dir) {
        if (safe_folder(p)) {
          strcpy(folder, p);
          reload = 1;
        } else
          message("Unsupported folder", "Folder path is too long or invalid.");
      } else if (send_mode) {
        int at = -1;
        for (int i = 0; i < *count; i++)
          if (!strcmp(files[i].path, p))
            at = i;
        if (at >= 0) {
          memmove(files + at, files + at + 1, (--*count - at) * sizeof *files);
        } else if (*count < MAX_FILES && safe_name(e->name) &&
                   e->size <= MAX_FILE_BYTES) {
          int dup = 0;
          for (int i = 0; i < *count; i++)
            if (!strcasecmp(files[i].name, e->name))
              dup = 1;
          if (dup)
            message("Duplicate name", "Choose files with distinct names.");
          else {
            TransferFile *f = &files[(*count)++];
            memset(f, 0, sizeof *f);
            strcpy(f->path, p);
            strcpy(f->name, e->name);
            f->size = e->size;
          }
        } else
          message("Cannot select file",
                  "Maximum 32 files; each must be under 2 GiB.");
      }
    }
    if (k & PSP_CTRL_START) {
      if (!send_mode || *count) {
        strcpy(settings.last, folder);
        save_settings();
        free(entries);
        return 1;
      }
    }
    pause_frame();
  }
  free(entries);
  return 0;
}
void progress(const char *name, uint64_t done, uint64_t total, uint64_t start,
              int index, int count, uint64_t batch_done, uint64_t batch_total) {
  uint64_t t = now_ms();
  if (t - last_progress < 150 && done != total)
    return;
  last_progress = t;
  double sec = (t - start) / 1000.0, speed = sec > 0 ? done / sec : 0;
  gfx_progress(name, total ? 100.0 * done / total : 100, done / 1048576.0,
               total / 1048576.0, speed / 1024,
               speed > 0 ? (total - done) / speed : -1, index, count,
               batch_done / 1048576.0, batch_total / 1048576.0);
  gfx_present();
}
void settings_menu(void) {
  while (running) {
    screen("Settings");
    gfx_round(18, 64, 444, 62, 9, PANEL);
    gfx_text(30, 70, "Auto screen dimming", 1, INK, 330);
    gfx_text(30, 98, "Uses your PSP display timer", 0, MUTED, 375);
    gfx_text(399, 74, settings.dim ? "ON" : "OFF", 1, MINT, 52);
    gfx_text(20, 143, "DEFAULT DESTINATION", 0, MUTED, 440);
    gfx_marquee(20, 168, settings.last, 1, INK, 440, now_ms());
    gfx_hint("X  Toggle dimming     O  Save and back",
             "Triangle  Change destination");
    unsigned k = keys();
    if (k & PSP_CTRL_CIRCLE) {
      save_settings();
      return;
    }
    if (k & PSP_CTRL_CROSS)
      settings.dim = !settings.dim;
    if (k & PSP_CTRL_TRIANGLE) {
      int count = 0;
      char p[PATH_CAP];
      strcpy(p, settings.last);
      browse(0, p, NULL, &count);
    }
    pause_frame();
  }
}
