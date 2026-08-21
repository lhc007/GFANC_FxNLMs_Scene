# SceneZone ANC — offline + realtime + calibration builds
#
#   make            → 离线版 main.exe (Windows 下另含 realtime/calibrate)
#   make realtime   → 编译实时版 scenezone_realtime.exe (Windows 专属)
#   make calibrate  → 编译反馈路径校准程序 calibrate_feedback.exe (Windows 专属)
#   make all        → 全部编译
#   make clean      → 清理编译产物
#
#   Linux (WSL): 实时/校准版依赖 Windows API, 阶段 1-A 移植前不构建
ifeq ($(OS),Windows_NT)
ALL_TARGETS = main.exe realtime calibrate
else
ALL_TARGETS = main.exe
endif

CC       = gcc
CFLAGS   = -O2 -Iinclude
CFLAGS_RT = -O2 -Iinclude -D_WIN32_WINNT=0x0601
LDFLAGS  = -lm
LDFLAGS_RT = -lm -lole32

MODULES = src/scene_controller.c src/fxnlms_mimo.c \
          src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c \
          src/howling_detect.c src/ocg.c
RT_MODULES = src/sec_online.c src/pa_loader.c   # 仅实时版需要 (与 README 编译命令一致)
CAL_MODULES = src/fir_filter.c src/binary_loader.c src/pa_loader.c

.PHONY: all realtime calibrate clean

all: $(ALL_TARGETS)

main.exe: main.c $(MODULES)
	$(CC) $(CFLAGS) main.c $(MODULES) $(LDFLAGS) -o $@

scenezone_realtime.exe: main_realtime.c $(MODULES) $(RT_MODULES)
	$(CC) $(CFLAGS_RT) main_realtime.c $(MODULES) $(RT_MODULES) $(LDFLAGS_RT) -o $@
realtime: scenezone_realtime.exe

calibrate_feedback.exe: src/calibrate_feedback.c $(CAL_MODULES)
	$(CC) $(CFLAGS_RT) src/calibrate_feedback.c $(CAL_MODULES) $(LDFLAGS_RT) -o $@
calibrate: calibrate_feedback.exe

clean:
	rm -f main.exe scenezone_realtime.exe calibrate_feedback.exe anti_out.wav error_out.wav
