TARGET = pocketsend
OBJS = src/main.o src/ui.o src/network.o src/core.o src/cJSON.o src/sha256.o src/gfx.o src/assets.o
CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu99 -DCJSON_NESTING_LIMIT=32
LIBS = -lpsppower -lm
BUILD_PRX = 1
PSP_FW_VERSION = 661
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PocketSend PSP
PSP_EBOOT_ICON = assets/ICON0.PNG
PSP_EBOOT_PIC1 = assets/PIC1.PNG
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
