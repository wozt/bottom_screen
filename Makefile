# bottom_screen_server / bottom_screen_client
#
# capture2cloud builds from a single gcc line in its README. Two binaries
# that share three of their objects is where that stops being convenient,
# so this is a small Makefile rather than a longer command to paste.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -I.
LDFLAGS ?=

SERVER_PKGS := libavcodec libavutil libswscale libswresample
CLIENT_PKGS := sdl2 libavcodec libavutil

SERVER_CFLAGS := $(shell pkg-config --cflags $(SERVER_PKGS))
SERVER_LIBS   := $(shell pkg-config --libs   $(SERVER_PKGS)) -lpthread
CLIENT_CFLAGS := $(shell pkg-config --cflags $(CLIENT_PKGS))
CLIENT_LIBS   := $(shell pkg-config --libs   $(CLIENT_PKGS))

SERVER_SRC := bottom_screen_server.c bs_server.c bs_encoder.c bs_audio.c \
              bs_net.c bs_mailbox.c testpattern.c
CLIENT_SRC := bottom_screen_client.c bs_decoder.c bs_net.c
SMOKE_SRC  := tests/smoke_client.c bs_decoder.c bs_net.c

BINARIES := bottom_screen_server bottom_screen_client

all: $(BINARIES)

bottom_screen_server: $(SERVER_SRC) bs_server.h bs_encoder.h bs_net.h \
                      bs_protocol.h bs_source.h bs_mailbox.h
	$(CC) $(CFLAGS) $(SERVER_CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS) $(SERVER_LIBS) -lm

bottom_screen_client: $(CLIENT_SRC) bs_decoder.h bs_net.h bs_protocol.h
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ $(CLIENT_SRC) $(LDFLAGS) $(CLIENT_LIBS)

# Headless end-to-end check: connects, decodes real frames, sends input
# back. No display, so it runs over SSH and in a script.
tests/smoke_client: $(SMOKE_SRC) bs_decoder.h bs_net.h bs_protocol.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libavcodec libavutil) -o $@ \
	  $(SMOKE_SRC) $(LDFLAGS) $(shell pkg-config --libs libavcodec libavutil)

test: bottom_screen_server tests/smoke_client
	./tests/run_smoke.sh

clean:
	rm -f $(BINARIES) tests/smoke_client

.PHONY: all clean test
