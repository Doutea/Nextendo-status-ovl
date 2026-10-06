#---------------------------------------------------------------------------------
# Nextendo Overlay — player counts for the Nextendo Network, from the Tesla /
# Ultrahand overlay menu.
#
# An .ovl is a libnx homebrew NRO with a different extension, loaded by the
# nx-ovlloader sysmodule. There is no Atmosphère-specific build step: we link
# against switch.specs and convert the ELF with elf2nro.
#
# Built on WerWolv's libtesla rather than libultrahand on purpose: every overlay
# confirmed working on the target console (NX-FanControl, FPSLocker,
# Status-Monitor, EdiZon) is built this way, and a libultrahand build of this
# same overlay crashed the loader on launch (Atmosphère fatal 2345-0002, PC=0).
# Ultrahand is a drop-in Tesla replacement, so a plain libtesla overlay is listed
# and launched normally.
#
# Local build (inside the devkitPro MSYS2 shell):
#   pacman -S --needed switch-dev switch-curl switch-zlib
#   make
#
# The result is nextendo-ovl.ovl, to copy to sdmc:/switch/.overlays/.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# NACP is REQUIRED, not optional.
#
# Ultrahand's getOverlayInfo() reads the NACP resource elf2nro appends after the
# NRO and takes the overlay's display name and version from it. Without the NACP
# it returns ResultParseError, the menu loop does `if (result != ResultSuccess)
# continue;`, and the overlay never appears at all.
#
# An icon is not required: elf2nro falls back to libnx's default_icon.jpg.
#---------------------------------------------------------------------------------
APP_TITLE	:=	Nextendo
APP_AUTHOR	:=	Nextendo Overlay
APP_VERSION	:=	1.0.0

TARGET		:=	nextendo-ovl
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include source libs/libtesla/include

NO_ICON		:=	1

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
# Flags mirror an overlay that is known to work on the target console
# (NX-FanControl): same ARCH including -mtp=soft, and no -Wl,--gc-sections.
# -mtp=soft matters here: devkitA64 patches the thread-pointer model, and
# omitting it changes TLS addressing, which overlays are sensitive to because
# libnx stores its thread vars in TLS.
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

# -fno-rtti is deliberately NOT used: libtesla's List uses dynamic_cast, so RTTI
# has to stay enabled. -fno-exceptions is safe here.
CXXFLAGS	:= $(CFLAGS) -fno-exceptions -std=c++20

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# libcurl is built by devkitPro with its TLS backend pointed at libnx's own
# `ssl` service (CURLSSLBACKEND_LIBNX), so HTTPS needs no CA bundle of our own.
LIBS	:= -lcurl -lz -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
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
export OFILES	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

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

.PHONY: $(BUILD) clean all test

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf out $(TARGET)-sd.zip

#---------------------------------------------------------------------------------
# Host-side unit tests for the JSON parser (no devkitA64 needed).
#---------------------------------------------------------------------------------
test:
	@$(MAKE) --no-print-directory -C tests run

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).ovl

# An overlay is an NRO with the .ovl extension.
#
# The .nacp prerequisite is essential, not cosmetic: `--nacp=` is passed to
# elf2nro below, and listing it here is what triggers switch_rules' `%.nacp`
# rule. Without it elf2nro fails with "Failed to open input nacp!", and without
# the NACP inside the NRO the overlay is silently skipped by Ultrahand.
#
# No 'ULTR' trailer is appended: libultrahand-based overlays carry it, but this
# one is built on plain libtesla like the overlays known to work on the target
# console, none of which have it.
$(OUTPUT).ovl		:	$(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $(OUTPUT).ovl) (libtesla build, NACP embedded)"

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
