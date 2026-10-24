#ifndef POCKET_GFX_H
#define POCKET_GFX_H
#include <stdint.h>
#define INK 0x00fff7ed
#define MUTED 0x00d9c5b1
#define MINT 0x00daf665
#define PANEL 0x00342617
#define SELECTED 0x00504420
void gfx_init(void);
void gfx_present(void);
void gfx_screen(const char *title);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_round(int x, int y, int w, int h, int radius, uint32_t color);
void gfx_text(int x, int y, const char *s, int font, uint32_t color, int width);
void gfx_marquee(int x, int y, const char *s, int font, uint32_t color,
                 int width, uint64_t time);
void gfx_wrap(int x, int y, const char *s, int font, uint32_t color, int width,
              int max_lines);
void gfx_hint(const char *a, const char *b);
void gfx_item(int row, const char *label, const char *tag, int selected,
              int icon, uint64_t time);
void gfx_home(const char *ip, const char *folder, int peers, int pending,
              uint64_t time);
void gfx_progress(const char *name, double percent, double done, double total,
                  double speed, double eta, int index, int count,
                  double batch_done, double batch_total);
int gfx_width(const char *s, int font);
extern uint32_t gfx_pixels[480 * 272];
#endif
