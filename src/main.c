#include "app.h"
#include "gfx.h"
#include <pspdebug.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility.h>
#include <stdio.h>
#include <string.h>
PSP_MODULE_INFO("PocketSend", 0, 0, 2);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(12288);
volatile int running = 1;
static int exit_callback(int a, int b, void *c) {
  (void)a;
  (void)b;
  (void)c;
  running = 0;
  return 0;
}
static int callbacks(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  int cb = sceKernelCreateCallback("Exit", exit_callback, NULL);
  sceKernelRegisterExitCallback(cb);
  sceKernelSleepThreadCB();
  return 0;
}
int main(int argc, char **argv) {
  gfx_init();
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
  int thread = sceKernelCreateThread("Callbacks", callbacks, 0x11, 4096,
                                     PSP_THREAD_ATTR_USER, NULL);
  if (thread >= 0)
    sceKernelStartThread(thread, 0, NULL);
  if (argc > 0 && argv[0] && !strncmp(argv[0], "ms0:/", 5) &&
      strlen(argv[0]) < sizeof app_dir) {
    strcpy(app_dir, argv[0]);
    char *slash = strrchr(app_dir, '/');
    if (slash)
      *slash = 0;
  }
  load_settings();
  save_settings();
  if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0 ||
      sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0) {
    message("Network modules unavailable",
            "Restart the PSP and launch under ARK-5.");
    goto finish;
  }
  if (sceNetInit(256 * 1024, 42, 4096, 42, 4096) < 0 || sceNetInetInit() < 0 ||
      sceNetApctlInit(0x8000, 48) < 0) {
    message("Network initialization failed", "Restart the app and try again.");
    goto finish;
  }
  if (!network_connect())
    goto cleanup;
  if (!network_start()) {
    message("Cannot start LocalSend service",
            "TCP port 53317 could not be opened.");
    goto cleanup;
  }
  while (running) {
    gfx_home(own_ip, settings.last, peer_count, network_pending(), now_ms());
    network_poll();
    unsigned k = keys();
    if (k & PSP_CTRL_CIRCLE)
      network_cancel_pending();
    if (k & PSP_CTRL_START)
      break;
    if (k & PSP_CTRL_CROSS)
      send_menu();
    if (k & PSP_CTRL_TRIANGLE)
      manual_peer();
    if (k & PSP_CTRL_SQUARE)
      settings_menu();
    pause_frame();
  }
  network_stop();
cleanup:
  sceNetApctlTerm();
  sceNetInetTerm();
  sceNetTerm();
finish:
  save_settings();
  sceKernelExitGame();
  return 0;
}
