#include "gfx.h"
#include <stdio.h>
#include <string.h>
#ifndef POCKET_HOST
#include <pspdisplay.h>
#endif
extern const uint32_t wallpaper_pixels[];
extern const unsigned char font_0[], font_1[], font_2[];
extern const unsigned char advance_0[], advance_1[], advance_2[];
uint32_t gfx_pixels[480 * 272] __attribute__((aligned(16)));
static int dirty;
static int front;
static const unsigned char *fonts[] = {font_0, font_1, font_2};
static const unsigned char *advances[] = {advance_0, advance_1, advance_2};
static unsigned glyph(unsigned c) {
  return c >= 32 && c <= 126 ? c - 32 : '?' - 32;
}
void gfx_init(void) {
#ifndef POCKET_HOST
  sceDisplaySetMode(0, 480, 272);
  sceDisplaySetFrameBuf((void *)0x44000000, 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
                        PSP_DISPLAY_SETBUF_IMMEDIATE);
#endif
}
void gfx_present(void) {
  if (!dirty)
    return;
#ifndef POCKET_HOST
  uint32_t *vram = (uint32_t *)(0x44000000 + (front ^ 1) * 512 * 272 * 4);
  for (int y = 0; y < 272; y++)
    memcpy(vram + y * 512, gfx_pixels + y * 480, 480 * 4);
  sceDisplayWaitVblankStart();
  sceDisplaySetFrameBuf(vram, 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
                        PSP_DISPLAY_SETBUF_IMMEDIATE);
  front ^= 1;
#else
  (void)front;
#endif
  dirty = 0;
}
void gfx_rect(int x, int y, int w, int h, uint32_t c) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > 480 ? 480 : x + w,
      y1 = y + h > 272 ? 272 : y + h;
  for (int j = y0; j < y1; j++)
    for (int i = x0; i < x1; i++)
      gfx_pixels[j * 480 + i] = c | 0xff000000;
  dirty = 1;
}
void gfx_round(int x, int y, int w, int h, int r, uint32_t c) {
  for (int j = 0; j < h; j++) {
    int inset = 0;
    int dy = j < r ? r - j - 1 : j >= h - r ? j - (h - r) : 0;
    if (j < r || j >= h - r) {
      while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r)
        inset++;
    }
    gfx_rect(x + inset, y + j, w - 2 * inset, 1, c);
  }
}
static void blend(int x, int y, uint32_t c, unsigned a) {
  if (x < 0 || x >= 480 || y < 0 || y >= 272 || !a)
    return;
  uint32_t bg = gfx_pixels[y * 480 + x], v = 0xff000000;
  for (int shift = 0; shift <= 16; shift += 8)
    v |= ((((c >> shift) & 255) * a + ((bg >> shift) & 255) * (255 - a)) / 255)
         << shift;
  gfx_pixels[y * 480 + x] = v;
}
int gfx_width(const char *s, int f) {
  int n = 0;
  for (; *s; s++)
    n += advances[f][glyph((unsigned char)*s)];
  return n;
}
static void text_clipped(int x, int y, const char *s, int f, uint32_t color,
                         int left, int right) {
  for (; *s && x < right; s++) {
    unsigned g = glyph((unsigned char)*s);
    for (int j = 0; j < 38; j++)
      for (int i = 0; i < 32; i++)
        if (x + i >= left && x + i < right)
          blend(x + i, y + j, color, fonts[f][g * 32 * 38 + j * 32 + i]);
    x += advances[f][g];
  }
  dirty = 1;
}
void gfx_text(int x, int y, const char *s, int f, uint32_t color, int width) {
  char b[1024];
  snprintf(b, sizeof b, "%s", s);
  int n = strlen(b);
  if (gfx_width(b, f) > width) {
    int dots = gfx_width("...", f);
    while (n && gfx_width(b, f) > width - dots)
      b[--n] = 0;
    if (width >= dots && n < (int)sizeof b - 4)
      strcat(b, "...");
  }
  text_clipped(x, y, b, f, color, x, x + width);
}
void gfx_marquee(int x, int y, const char *s, int f, uint32_t c, int w,
                 uint64_t time) {
  int overflow = gfx_width(s, f) - w;
  if (overflow <= 0) {
    gfx_text(x, y, s, f, c, w);
    return;
  }
  int travel = overflow * 35, cycle = travel * 2 + 2400;
  int phase = (int)(time % (uint64_t)cycle);
  int offset = phase < 1200            ? 0
               : phase < 1200 + travel ? (phase - 1200) / 35
               : phase < 2400 + travel
                   ? overflow
                   : overflow - (phase - 2400 - travel) / 35;
  text_clipped(x - offset, y, s, f, c, x, x + w);
}
void gfx_wrap(int x, int y, const char *s, int f, uint32_t c, int w,
              int max_lines) {
  char line[256];
  int n = 0, rows = 0;
  while (*s && rows < max_lines) {
    line[n++] = *s++;
    line[n] = 0;
    if (gfx_width(line, f) > w || n >= 250) {
      line[--n] = 0;
      s--;
      int split = n;
      while (split > 0 && line[split] != ' ')
        split--;
      if (split > 0) {
        s -= n - split;
        line[split] = 0;
      }
      gfx_text(x, y + rows * (f == 0 ? 23 : 27), line, f, c, w);
      rows++;
      n = 0;
      while (*s == ' ')
        s++;
    }
  }
  if (n && rows < max_lines)
    gfx_text(x, y + rows * (f == 0 ? 23 : 27), line, f, c, w);
}
static void logo(int x, int y) {
  gfx_rect(x, y + 4, 22, 4, MINT);
  for (int i = 0; i < 8; i++)
    gfx_rect(x + 17 + i, y + i, 1, 12 - i * 1, MINT);
  gfx_rect(x + 3, y + 20, 22, 4, INK);
  for (int i = 0; i < 8; i++)
    gfx_rect(x + 7 - i, y + 16 + i / 2, 1, 12 - i, INK);
}
void gfx_screen(const char *title) {
  memcpy(gfx_pixels, wallpaper_pixels, sizeof gfx_pixels);
  dirty = 1;
  gfx_rect(0, 0, 480, 48, 0x0020130b);
  logo(18, 9);
  gfx_text(57, 12, title, 1, INK, 355);
  gfx_round(430, 13, 33, 23, 6, 0x003b3020);
  gfx_text(437, 15, "PSP", 0, MUTED, 27);
  gfx_rect(18, 47, 444, 1, 0x004c3825);
}
void gfx_hint(const char *a, const char *b) {
  gfx_rect(0, 226, 480, 46, 0x0020130b);
  gfx_text(18, 228, a, 0, INK, 444);
  gfx_text(18, 250, b, 0, MUTED, 444);
}
void gfx_item(int row, const char *label, const char *tag, int selected,
              int icon, uint64_t time) {
  int y = 80 + row * 29;
  gfx_round(18, y, 444, 27, 6, selected ? SELECTED : PANEL);
  if (selected)
    gfx_rect(18, y + 6, 3, 15, MINT);
  if (icon == 1) {
    gfx_round(29, y + 7, 17, 13, 2, 0x009bd9ff);
    gfx_rect(29, y + 4, 8, 4, 0x009bd9ff);
  } else if (icon == 2) {
    gfx_round(31, y + 5, 12, 17, 2, MINT);
    gfx_rect(34, y + 9, 6, 2, PANEL);
  } else {
    gfx_round(33, y + 10, 6, 6, 3, selected ? MINT : MUTED);
  }
  int tagw = tag && *tag ? gfx_width(tag, 0) + 18 : 0;
  if (selected)
    gfx_marquee(55, y + 3, label, 0, INK, 392 - tagw, time);
  else
    gfx_text(55, y + 3, label, 0, INK, 392 - tagw);
  if (tagw)
    gfx_text(450 - tagw + 9, y + 3, tag, 0, selected ? MINT : MUTED, tagw - 9);
}
void gfx_home(const char *ip, const char *folder, int peers, int pending,
              uint64_t time) {
  gfx_screen("PocketSend");
  gfx_round(18, 60, 444, 100, 12, PANEL);
  gfx_round(32, 75, 8, 8, 4, MINT);
  gfx_text(49, 68, pending ? "Incoming batch" : "Ready to receive", 2, INK,
           385);
  char address[80];
  snprintf(address, sizeof address, "%s : 53317", ip);
  gfx_text(32, 108, address, 1, MINT, 410);
  char note[80];
  snprintf(note, sizeof note, "%d nearby device%s  /  HTTP mode", peers,
           peers == 1 ? "" : "s");
  gfx_text(32, 137, note, 0, MUTED, 410);
  gfx_round(18, 165, 444, 25, 6, PANEL);
  gfx_text(20, 169, "SAVE TO", 0, MUTED, 80);
  gfx_marquee(105, 168, folder, 0, INK, 355, time);
  gfx_round(18, 194, 215, 26, 7, SELECTED);
  gfx_text(31, 196, "X   Send files", 0, INK, 190);
  gfx_round(243, 194, 219, 26, 7, PANEL);
  gfx_text(256, 196, "Triangle   Add IP", 0, INK, 195);
  gfx_hint("Square  Settings                 Start  Exit",
           pending ? "O  Cancel incoming batch"
                   : "Accept transfers here from LocalSend.");
}
void gfx_progress(const char *name, double percent, double done, double total,
                  double speed, double eta, int index, int count,
                  double batch_done, double batch_total) {
  gfx_screen("Transferring");
  gfx_text(20, 57, name, 1, INK, 440);
  char b[120];
  snprintf(b, sizeof b, "%.0f%%", percent);
  gfx_text(20, 89, b, 2, MINT, 130);
  snprintf(b, sizeof b, "File %d of %d", index, count);
  gfx_text(300, 100, b, 0, MUTED, 160);
  gfx_round(20, 133, 440, 12, 6, PANEL);
  int fill = (int)(440 * percent / 100);
  if (fill > 440)
    fill = 440;
  if (fill > 0)
    gfx_round(20, 133, fill, 12, fill < 12 ? fill / 2 : 6, MINT);
  gfx_round(18, 151, 444, 66, 8, PANEL);
  snprintf(b, sizeof b, "%.2f / %.2f MiB", done, total);
  gfx_text(20, 157, b, 1, INK, 280);
  snprintf(b, sizeof b, "%.0f KiB/s", speed);
  gfx_text(325, 158, b, 0, MINT, 135);
  snprintf(b, sizeof b, "Batch %.1f / %.1f MiB", batch_done, batch_total);
  gfx_text(20, 191, b, 0, MUTED, 300);
  if (eta >= 0) {
    snprintf(b, sizeof b, "~%.0fs left", eta);
    gfx_text(335, 191, b, 0, MUTED, 125);
  }
  gfx_hint("O  Cancel transfer", "Keep both devices connected.");
}
