# MIMO GFANC C — offline + realtime builds
#
#   make             → 编译离线版 main.exe
#   make realtime    → 编译实时版 gfanc_realtime.exe (需 WASAPI)
#   make all         → 两个都编译
#
#   ./main.exe <noise.wav>              # 离线降噪
#   ./gfanc_realtime.exe                # 实时降噪 (Ctrl+C 退出)

CC      = gcc
CFLAGS  = -Wall -O2 -march=native -ffast-math -Iinclude
LDFLAGS = -lm
LDFLAGS_RT = -lm -lole32

SRCS = src/binary_loader.c
OBJS = $(SRCS:src/%.c=build/%.o)

.PHONY: all clean export realtime

all: export build/main.exe

realtime: export build/gfanc_realtime.exe

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/main.exe: main.c $(OBJS) | build
	$(CC) $(CFLAGS) main.c $(OBJS) $(LDFLAGS) -o $@

build/gfanc_realtime.exe: main_realtime.c src/wasapi_io.c $(OBJS) | build
	$(CC) $(CFLAGS) main_realtime.c src/wasapi_io.c $(OBJS) $(LDFLAGS_RT) -o $@

export:
	python export/export_bin.py

clean:
	rm -rf build main.exe gfanc_realtime.exe anti_out.wav error_out.wav
