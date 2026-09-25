#---------------------------------------------------------------------------------
# bottom_screen, running ON a Wii U console.
#
# Not to be confused with ../wiiu_gamepad, which runs on the PC and talks
# to a GamePad over a radio. This one is a .rpx for the console itself.
#
# Built with devkitPPC + wut:
#     sudo dkp-pacman -S wiiu-dev
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

TARGET		:=	bottomscreen_wiiu
BUILD		:=	build
SOURCES		:=	src
DATA		:=	data
# The parent directory holds bs_protocol.h and bs_le.h, shared with the
# server so the protocol cannot drift. Wii U serialises fields explicitly
# because PowerPC is big-endian.
INCLUDES	:=	src ..

# These become the channel's name, icon and splash on the Wii U menu.
# A .wuhb is an .rpx plus this metadata, and the homebrew_on_menu plugin
# is what puts one on the menu as a channel -- see README.md.
APP_NAME	:=	Bottom Screen
APP_SHORTNAME	:=	Bottom Screen
APP_AUTHOR	:=	wozt
APP_ICON	:=	$(TOPDIR)/../assets/icon-512.png

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
CFLAGS	:=	-g -Wall -Wextra -O2 -ffunction-sections \
			$(MACHDEP)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(MACHDEP)
LDFLAGS	=	-g $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

# SDL2, and the reason is not convenience.
#
# Measured on the console: once OSScreen has been up, GX2 renders
# nothing -- and the keyboard, the HOME overlay and an NV12 video
# texture all need GX2. So the program has to be GX2 from its first
# line, and this toolchain ships no shader assembler to get there by
# hand. SDL2's Wii U renderer IS GX2 and brings text and touch with it.
#
# SDL2_ttf pulls freetype, which pulls harfbuzz, which pulls freetype
# back -- hence freetype on both sides of it. The font itself is not
# shipped: it comes from the console's shared memory (see ui.c).
LIBS	:=	-lSDL2_ttf -lfreetype -lharfbuzz -lfreetype -lbrotlidec -lbrotlicommon -lpng -lbz2 -lz \
			-lSDL2 -lopus -lwut -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(WUT_ROOT)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add
# additional rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).rpx $(TARGET).elf $(TARGET).wuhb

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# The .wuhb as well as the .rpx: the .rpx is what wiiload sends over the
# network while developing, the .wuhb is what goes on the SD card and
# shows up as a channel.
all	:	$(OUTPUT).wuhb

$(OUTPUT).wuhb	:	$(OUTPUT).rpx
$(OUTPUT).rpx	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)


#---------------------------------------------------------------------------------
%.gsh.o %_gsh.h : %.gsh
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
