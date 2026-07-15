# GFANC FxNLMS — offline + realtime builds
#
#   make          → 编译离线版 main.exe
#   make realtime → 编译实时版 gfanc_realtime.exe
#   make all      → 两个都编译
#   make clean    → 清理编译产物

CC       = gcc
CFLAGS   = -O2 -Iinclude
CFLAGS_RT = -O2 -Iinclude -D_WIN32_WINNT=0x0601
LDFLAGS  = -lm
LDFLAGS_RT = -lm -lole32

MODULES = src/scene_controller.c src/fxnlms_mimo.c \
          src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c

.PHONY: all realtime clean

all: main.exe

realtime: gfanc_realtime.exe

main.exe: main.c $(MODULES)
	$(CC) $(CFLAGS) main.c $(MODULES) $(LDFLAGS) -o $@

gfanc_realtime.exe: main_realtime.c src/wasapi_io.c $(MODULES)
	$(CC) $(CFLAGS_RT) main_realtime.c src/wasapi_io.c $(MODULES) $(LDFLAGS_RT) -o $@

clean:
	rm -f main.exe gfanc_realtime.exe anti_out.wav error_out.wav
