# MIMO GFANC C Inference Engine
# Usage: make && ./main.exe road_noise-15.wav

CC      = gcc
CFLAGS  = -Wall -O2 -march=native -ffast-math -Iinclude
LDFLAGS = -lm

SRCS = src/binary_loader.c
OBJS = $(SRCS:src/%.c=build/%.o)

.PHONY: all clean export

all: export build/main.exe

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/main.exe: main.c $(OBJS) | build
	$(CC) $(CFLAGS) main.c $(OBJS) $(LDFLAGS) -o $@

export:
	python export/export_bin.py

clean:
	rm -rf build main.exe anti_out.wav error_out.wav
