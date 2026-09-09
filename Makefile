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
LAUNCHER_PKGS := gtk+-3.0

SERVER_CFLAGS := $(shell pkg-config --cflags $(SERVER_PKGS))
SERVER_LIBS   := $(shell pkg-config --libs   $(SERVER_PKGS)) -lpthread
CLIENT_CFLAGS := $(shell pkg-config --cflags $(CLIENT_PKGS))
CLIENT_LIBS   := $(shell pkg-config --libs   $(CLIENT_PKGS))
LAUNCHER_CFLAGS := $(shell pkg-config --cflags $(LAUNCHER_PKGS))
LAUNCHER_LIBS   := $(shell pkg-config --libs   $(LAUNCHER_PKGS))

SERVER_SRC := bottom_screen_server.c bs_server.c bs_encoder.c bs_audio.c \
              bs_net.c bs_mailbox.c bs_ws.c testpattern.c
CLIENT_SRC := bottom_screen_client.c bs_decoder.c bs_net.c
SMOKE_SRC  := tests/smoke_client.c bs_decoder.c bs_net.c
MERGE_SRC  := tests/input_merge.c bs_server.c bs_encoder.c bs_audio.c \
              bs_mailbox.c bs_net.c bs_ws.c
FLIP_SRC   := tests/resize_flip.c bs_mailbox.c bs_net.c

BINARIES := bottom_screen_server bottom_screen_client launcher/bs_launcher

all: $(BINARIES)

# The page is compiled in, so there is no file to install and no path
# to get wrong relative to wherever an emulator was launched from.
# The order matters only for the scripts, and index.html is what lists
# them; here it is just the set of files the server carries.
WEB_FILES := web/index.html web/app.css \
             web/main.js web/protocol.js web/audio.js web/video.js \
             web/touch.js web/pad.js web/settings.js web/servers.js \
             web/prompt.js web/connect.js

web_page.h: $(WEB_FILES) scripts/embed_page.py
	python3 scripts/embed_page.py $(WEB_FILES) web_page.h

bottom_screen_server: $(SERVER_SRC) bs_server.h bs_encoder.h bs_net.h \
                      bs_protocol.h bs_source.h bs_mailbox.h bs_ws.h web_page.h
	$(CC) $(CFLAGS) $(SERVER_CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS) $(SERVER_LIBS) -lm

launcher/bs_launcher: launcher/bs_launcher.c bs_protocol.h
	$(CC) $(CFLAGS) $(LAUNCHER_CFLAGS) -o $@ launcher/bs_launcher.c $(LDFLAGS) $(LAUNCHER_LIBS)

bottom_screen_client: $(CLIENT_SRC) bs_decoder.h bs_net.h bs_protocol.h
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ $(CLIENT_SRC) $(LDFLAGS) $(CLIENT_LIBS)

# Headless end-to-end check: connects, decodes real frames, sends input
# back. No display, so it runs over SSH and in a script.
# Sanitized on purpose: the bug this guards against is an out-of-bounds
# read that a plain build would happily perform and pass.
tests/resize_flip: $(FLIP_SRC) bs_mailbox.h bs_source.h
	$(CC) $(CFLAGS) $(SERVER_CFLAGS) -fsanitize=address,undefined -g \
	  -o $@ $(FLIP_SRC) $(LDFLAGS) -lpthread -lm

tests/input_merge: $(MERGE_SRC) bs_server.h bs_mailbox.h bs_protocol.h web_page.h
	$(CC) $(CFLAGS) $(SERVER_CFLAGS) -o $@ $(MERGE_SRC) $(LDFLAGS) $(SERVER_LIBS) -lm

tests/smoke_client: $(SMOKE_SRC) bs_decoder.h bs_net.h bs_protocol.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libavcodec libavutil) -o $@ \
	  $(SMOKE_SRC) $(LDFLAGS) $(shell pkg-config --libs libavcodec libavutil)

# Every test, not one of them.
#
# This target used to build the others and run only the smoke test, so
# it passed while nothing exercised the button merging, the size
# renegotiation or the multi-client path -- and a stale binary left over
# from an earlier build could sit there reporting success.
# Built only where opus is: it is the console client's dependency, not
# this tree's, and its absence is a reason to skip that one test rather
# than to fail the build.
SWITCH_TEST := $(shell pkg-config --exists opus && echo tests/switch_client)

# The console client's own stream.c, compiled for this machine. It has no
# libnx in it, so the half of the homebrew that talks to the server can
# be driven here -- which is the only test that client ever gets.
tests/switch_client: tests/switch_client.c switch/source/stream.c \
                     bs_decoder.c bs_net.c bs_protocol.h
	$(CC) $(CFLAGS) -Iswitch/source $(shell pkg-config --cflags opus) \
	  -o $@ tests/switch_client.c switch/source/stream.c bs_decoder.c bs_net.c \
	  $(shell pkg-config --libs opus) -lavcodec -lavutil -lpthread

test: bottom_screen_server tests/smoke_client tests/input_merge tests/resize_flip \
      $(SWITCH_TEST)
	./tests/run_smoke.sh
	./tests/run_multiclient.sh
	./tests/run_receive_size.sh
	./tests/run_top_screen.sh
	./tests/run_web_files.sh
	./tests/run_prompt.sh
	./tests/run_switch_client.sh
	./tests/input_merge
	./tests/resize_flip
	./tests/run_patches_fresh.sh
	./tests/run_recipes.sh
	./tests/run_recipe_drift.sh

clean:
	rm -f $(BINARIES) tests/smoke_client tests/input_merge tests/resize_flip \
	      tests/switch_client web_page.h

.PHONY: all clean test
