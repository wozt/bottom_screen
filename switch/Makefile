#---------------------------------------------------------------------------------
# The Switch client.
#
# Built with devkitA64 and libnx, against the same bs_protocol.h,
# bs_net.c and bs_decoder.c the desktop client uses -- the wire format
# and the framing cannot drift between the two because there is only one
# copy of them.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      :=  bottom_screen_switch
BUILD       :=  build
# The parent directory holds the shared C. Listing it as a source
# directory would sweep in the server and the encoder as well, so the
# three files that are wanted are named in SOURCES_EXTRA below.
SOURCES     :=  source
DATA        :=  data
INCLUDES    :=  source ..

APP_TITLE   :=  Bottom Screen
APP_AUTHOR  :=  wozt
APP_VERSION :=  0.2.0

#---------------------------------------------------------------------------------
ARCH    :=  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  :=  -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  +=  $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS :=  -g $(ARCH)
LDFLAGS =   -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# SDL2 on this platform sits on Mesa/EGL, which is C++ -- hence -lstdc++,
# without which the link fails on operator new and delete. SDL2_ttf pulls
# freetype, which pulls harfbuzz, which pulls freetype back, so freetype
# appears on both sides of it.
#
# libavcodec is here for h264_nvtegra, the console's own video block. It
# drags in dav1d and vpx whether or not those formats are ever decoded:
# the static library was built with them and the linker wants them.
LIBS    :=  -lSDL2_ttf -lfreetype -lharfbuzz -lfreetype -lbz2 -lpng -lz \
            -lavcodec -lavutil -lswresample -ldav1d -lvpx -lopus -logg -lz \
            -lSDL2 -lEGL -lstdc++ -lglapi -ldrm_nouveau \
            -lnx -lpthread -lm

LIBDIRS :=  $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
                    $(CURDIR)/..

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))) \
                bs_net.c bs_decoder.c
CPPFILES    :=
SFILES      :=
BINFILES    :=

export LD   :=  $(CC)
export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   :=  $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
    jsons := $(wildcard *.json)
    ifneq (,$(findstring $(TARGET).json,$(jsons)))
        export APP_JSON := $(TOPDIR)/$(TARGET).json
    else
        ifneq (,$(findstring config.json,$(jsons)))
            export APP_JSON := $(TOPDIR)/config.json
        endif
    endif
else
    export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.jpg)
    ifneq (,$(findstring $(TARGET).jpg,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).jpg
    else
        ifneq (,$(findstring icon.jpg,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.jpg
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
    export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
    export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS :=  $(OFILES:.o=.d)

all :   $(OUTPUT).nro

ifeq ($(strip $(APP_JSON)),)
$(OUTPUT).nro   :   $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro   :   $(OUTPUT).elf $(APP_JSON)
endif

$(OUTPUT).elf   :   $(OFILES)

$(OFILES_SRC)   : $(HFILES_BIN)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
