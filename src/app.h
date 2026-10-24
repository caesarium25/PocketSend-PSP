#ifndef POCKET_APP_H
#define POCKET_APP_H
#include "core.h"
#include <pspctrl.h>
#include <pspkernel.h>
#include <stdint.h>
#define PORT 53317
#define MAX_PEERS 16
typedef struct {
  char ip[16], alias[64];
  int port, https;
  uint64_t seen;
} Peer;
typedef struct {
  char last[PATH_CAP], favorites[8][PATH_CAP], fingerprint[65];
  int favorite_count, dim;
} Settings;
extern Settings settings;
extern volatile int running;
extern char own_ip[16];
extern char app_dir[PATH_CAP];
uint64_t now_ms(void);
unsigned keys(void);
void screen(const char *title);
void line(int row, const char *fmt, ...);
void pause_frame(void);
int confirm(const char *title, const char *detail);
void message(const char *title, const char *detail);
int keyboard(const char *title, char *out, unsigned cap);
int browse(int send_mode, char *folder, TransferFile *files, int *count);
void progress(const char *name, uint64_t done, uint64_t total, uint64_t start,
              int index, int count, uint64_t batch_done, uint64_t batch_total);
void load_settings(void);
void save_settings(void);
void settings_menu(void);
uint64_t free_space(void);
void random_id(char out[65]);
int network_connect(void);
int network_start(void);
void network_stop(void);
void network_poll(void);
int network_pending(void);
void network_cancel_pending(void);
void send_menu(void);
void manual_peer(void);
extern Peer peers[MAX_PEERS];
extern int peer_count;
#endif
