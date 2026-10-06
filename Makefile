#---------------------------------------------------------------------------------
# Nextendo Overlay — player counts for the Nextendo Network, from the Ultrahand
# (or Tesla) overlay menu.
#
# An .ovl is a libnx homebrew NRO with a different extension, loaded by the
# nx-ovlloader sysmodule. There is no Atmosphère-specific build step: we link
# against switch.specs and convert the ELF with elf2nro.
#
# Built on libultrahand (the maintained fork of libtesla, and what Ultrahand
# Overlay itself is built with), so the result is a first-class Ultrahand
# overlay. That matters because Ultrahand hides overlays it considers
# unsupported; the `ULTR` trailer appended below is how it recognises its own.
#
# Local build (inside the devkitPro MSYS2 shell):
#   pacman -S --needed switch-dev switch-curl switch-zlib switch-minizip switch-mbedtls
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
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#
# NO_ICON: overlays are never listed by hbmenu, so no icon is needed.
# NO_NACP: skip the .nacp as well; it is only read by the homebrew menu.
#---------------------------------------------------------------------------------
APP_TITLE	:=	Nextendo
APP_AUTHOR	:=	Nextendo Overlay
APP_VERSION	:=	1.0.0

TARGET		:=	nextendo-ovl
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include source

NO_ICON		:=	1
NO_NACP		:=	1

# Pulls libultrahand's sources and headers into SOURCES / INCLUDES.
include $(TOPDIR)/libs/libultrahand/ultrahand.mk

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
# NOTE: -mtp=soft is deliberately omitted. It is a devkitA64-specific default
# that upstream aarch64-none-elf-gcc rejects, and devkitA64 does not need it.
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections -fdata-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

# -fno-rtti is deliberately NOT used: the vendored libtesla uses dynamic_cast
# (its List implementation), so RTTI has to stay enabled. -fno-exceptions is
# safe because nothing here relies on exception handling.
CXXFLAGS	:= $(CFLAGS) -fno-exceptions -std=c++20

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections

# libcurl is built by devkitPro with its TLS backend pointed at libnx's own
# `ssl` service (CURLSSLBACKEND_LIBNX), so HTTPS needs no CA bundle of our own.
# The remaining libraries are what libultrahand requires.
LIBS	:= -lcurl -lz -lminizip -lmbedtls -lmbedx509 -lmbedcrypto -lnx

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

.PHONY: $(BUILD) clean all test dist

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
# Kept out of `all` so a cross-build never depends on a host compiler.
#---------------------------------------------------------------------------------
test:
	@$(MAKE) --no-print-directory -C tests run

# A zip laid out exactly as it should be copied onto the SD card root:
#   switch/.overlays/nextendo-ovl.ovl
# Built here rather than externally so the archive entries use forward slashes,
# which the console's extractor and every desktop tool handle correctly.
dist: all
	@rm -rf out
	@mkdir -p out/switch/.overlays
	@cp $(TARGET).ovl out/switch/.overlays/
	@rm -f $(TARGET)-sd.zip
	@cd out && zip -qr ../$(TARGET)-sd.zip switch
	@echo "built ... $(TARGET)-sd.zip"

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).ovl

# An overlay is an NRO with the .ovl extension. The trailing 'ULTR' marker is
# appended on purpose: it is the signature Ultrahand looks for to treat the
# binary as its own, and without it Ultrahand may hide the overlay entirely.
$(OUTPUT).ovl		:	$(OUTPUT).elf
	@elf2nro $< $@ $(NROFLAGS)
	@printf 'ULTR' >> $@
	@echo "built ... $(notdir $(OUTPUT).ovl) (Ultrahand signature appended)"

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
