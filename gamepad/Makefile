# bottom_screen -> a real Wii U GamePad.
#
# Built on its own rather than from the top-level Makefile: it needs
# libdrc and a C++ compiler, and a machine without a Realtek adapter has
# no use for it. `make` at the top level stays buildable everywhere.
#
# libdrc and the modified x264 are carried in vendor/ because they are
# build outputs of two other projects and getting them is half the work.
# The sources they came from are wozt/libdrc and wozt/drc-x264; see
# docs/WIIU_GAMEPAD.md.

CXX      ?= c++
BS       := ..
VENDOR   := vendor

CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
CXXFLAGS += -I$(BS) -I$(VENDOR)/libdrc/include -I$(VENDOR)/x264/include
CXXFLAGS += $(shell pkg-config --cflags libavcodec libavutil libswscale opus)

# rpath, so it runs from here without anybody setting LD_LIBRARY_PATH.
# The two libraries live in this tree and nowhere a loader would look.
LDFLAGS  += -L$(VENDOR)/libdrc -L$(VENDOR)/x264/lib
LDFLAGS  += -Wl,-rpath,'$$ORIGIN/$(VENDOR)/libdrc'
LDFLAGS  += -Wl,-rpath,'$$ORIGIN/$(VENDOR)/x264/lib'
LDLIBS   += -ldrc -lx264
LDLIBS   += $(shell pkg-config --libs libavcodec libavutil libswscale opus)
LDLIBS   += -lpthread

# The decoder and the socket layer are the ones every other client here
# uses, compiled as C.
BS_SRC   := $(BS)/bs_decoder.c $(BS)/bs_net.c
BS_OBJ   := bs_decoder.o bs_net.o

all: bs_gamepad

bs_gamepad: bs_gamepad.o $(BS_OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)

bs_gamepad.o: bs_gamepad.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: $(BS)/%.c
	$(CC) -O2 -Wall -Wextra -I$(BS) $(shell pkg-config --cflags libavcodec libavutil) -c -o $@ $<

clean:
	rm -f bs_gamepad *.o

.PHONY: all clean
