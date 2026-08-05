# GFANC FxNLMs — offline + realtime + calibration builds
#
#   make            → 编译离线版 main.exe
#   make realtime   → 编译实时版 gfanc_realtime.exe
#   make calibrate  → 编译反馈路径校准程序 calibrate_feedback.exe
#   make all        → 全部编译
#   make clean      → 清理编译产物
#   make test       → 运行黄金回归 + 单元测试 (R-31)
#   make test-accept → 接受当前输出为新的黄金基线

CC       = gcc
CFLAGS   = -O2 -Iinclude
CFLAGS_RT = -O2 -Iinclude -D_WIN32_WINNT=0x0601
LDFLAGS  = -lm
LDFLAGS_RT = -lm -lole32

MODULES = src/scene_controller.c src/fxnlms_mimo.c \
          src/fir_filter.c src/binary_loader.c src/cnn_m5_forward.c \
          src/howling_detect.c
RT_MODULES = src/sec_online.c src/pa_loader.c   # 仅实时版需要 (与 README 编译命令一致)
CAL_MODULES = src/fir_filter.c src/binary_loader.c src/pa_loader.c

.PHONY: all realtime calibrate clean test test-accept

all: main.exe realtime calibrate

main.exe: main.c $(MODULES)
	$(CC) $(CFLAGS) main.c $(MODULES) $(LDFLAGS) -o $@

gfanc_realtime.exe: main_realtime.c $(MODULES) $(RT_MODULES)
	$(CC) $(CFLAGS_RT) main_realtime.c $(MODULES) $(RT_MODULES) $(LDFLAGS_RT) -o $@
realtime: gfanc_realtime.exe

calibrate_feedback.exe: src/calibrate_feedback.c $(CAL_MODULES)
	$(CC) $(CFLAGS_RT) src/calibrate_feedback.c $(CAL_MODULES) $(LDFLAGS_RT) -o $@
calibrate: calibrate_feedback.exe

test:
	bash test/run_tests.sh

test-accept:
	bash test/run_tests.sh --accept

clean:
	rm -f main.exe gfanc_realtime.exe calibrate_feedback.exe anti_out.wav error_out.wav
	rm -f test/gen_test_wav.exe test/test_fir.exe test/test_signal.wav
