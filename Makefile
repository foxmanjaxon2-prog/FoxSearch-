#-------------------------------------------------------------------------------
# FoxSearch – 3DS Homebrew Browser
#-------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

TOPDIR      ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

APP_TITLE   := FoxSearch
APP_AUTHOR  := FoxSearch
APP_DESC    := Better 3DS Browser with JS

TARGET      := FoxSearch
BUILD       := build
SOURCES     := source source/quickjs
INCLUDES    := source source/quickjs
ROMFS       := romfs

ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS      := -Wall -O2 -mword-relocations -ffunction-sections \
               $(ARCH) $(INCLUDE) -D__3DS__ \
               -DCONFIG_BIGNUM=0 \
               -DCONFIG_WORKER=0 \
               -DCONFIG_VERSION='"2024-01-01"' \
               -Wno-sign-compare -Wno-unused-parameter \
               -Wno-missing-field-initializers \
               -Wno-implicit-function-declaration \
               -Wno-unused-but-set-variable \
               -Wno-type-limits

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS        := -lcitro2d -lcitro3d -lctru -lm

LIBDIRS     := $(CTRULIB) $(PORTLIBS)

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN     := $(addsuffix .o,$(BINFILES))
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/cia_assets/icon.png

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
    export _3DSXDEPS  += $(OUTPUT).smdh
endif

ifneq ($(ROMFS),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia setup

setup:
	@echo "Creating project directories..."
	@mkdir -p source/quickjs cia_assets romfs
	@echo "Cloning QuickJS..."
	@git clone --depth 1 https://github.com/bellard/quickjs.git source/quickjs
	@echo "Generating assets..."
	@python3 cia_assets/gen_assets.py
	@echo "Done. Run 'make cia' to build."

cia_assets/icon.png cia_assets/banner.png &:
	@python3 cia_assets/gen_assets.py

all: cia_assets/icon.png
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD): cia_assets/icon.png
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf \
	        cia_assets/icon.png cia_assets/banner.png \
	        cia_assets/icon.bin cia_assets/banner.bin

cia: cia_assets/icon.png cia_assets/banner.png
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@echo "Packing CIA..."
	@bash cia_assets/build_cia.sh

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf  : $(OFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
