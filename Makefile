CC = gcc
CFLAGS ?= -Wall -Wextra -g

ifeq ($(OS),Windows_NT)
CLEAN_CMD = powershell -NoProfile -Command "if (Test-Path 'renamer') { Remove-Item -Force 'renamer' }; if (Test-Path 'renamer.exe') { Remove-Item -Force 'renamer.exe' }"
TEST_CMD = powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-basic.ps1
else
CLEAN_CMD = rm -f renamer renamer.exe
TEST_CMD = sh tests/run-basic.sh
endif

.PHONY: all clean test

all: renamer

renamer: main.c renamer.c renamer.h
	$(CC) $(CFLAGS) -o renamer main.c renamer.c

clean:
	$(CLEAN_CMD)

test: renamer
	$(TEST_CMD)
