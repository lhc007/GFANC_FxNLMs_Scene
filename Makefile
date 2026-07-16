# GFANC FxNLMs — offline + realtime + calibration builds
#
#   make              → 编译离线版 main.exe
#   make realtime     → 编译实时版 gfanc_realtime.exe
#   make calibrate    → 编译反馈路径校准程序 calibrate_feedback.exe
#   make calibrate-sec→ 编译次级路径校准程序 calibrate_secondary.exe (F-B 修复)
#   make all          → 全部编译
#   make clean        → 清理编译产物

CC       = gcc
CFLAGS   = -O2 -Iinclude
CFLAGS_RT = -O2 -Iinclude -D_WIN32_WINNT=0x0601
LDFLAGS  = -lm
LDFLAGS_RT = -lm -lole32

MODULES = src/scene_controller.c src/fxnlms_mimo.c \
          src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c \
          src/howling_detect.c
CAL_MODULES = src/fir_filter.c src/binary_loader.c

.PHONY: all realtime calibrate calibrate-sec clean

all: main.exe realtime calibrate calibrate-sec

main.exe: main.c $(MODULES)
	$(CC) $(CFLAGS) main.c $(MODULES) $(LDFLAGS) -o $@

gfanc_realtime.exe: main_realtime.c $(MODULES)
	$(CC) $(CFLAGS_RT) main_realtime.c $(MODULES) $(LDFLAGS_RT) -o $@
realtime: gfanc_realtime.exe

calibrate_feedback.exe: src/calibrate_feedback.c $(CAL_MODULES)
	$(CC) $(CFLAGS_RT) src/calibrate_feedback.c $(CAL_MODULES) $(LDFLAGS_RT) -o $@
calibrate: calibrate_feedback.exe

calibrate_secondary.exe: src/calibrate_secondary.c
	$(CC) $(CFLAGS_RT) src/calibrate_secondary.c $(LDFLAGS_RT) -o $@
calibrate-sec: calibrate_secondary.exe

clean:
	rm -f main.exe gfanc_realtime.exe calibrate_feedback.exe calibrate_secondary.exe anti_out.wav error_out.wav
