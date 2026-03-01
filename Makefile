#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment.")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

#---------------------------------------------------------------------------------
TARGET          :=  WiiUStreamDeck
BUILD           :=  build
SOURCES         :=  source
DATA            :=
export CONTENT  :=  content

APP_NAME        :=  Stream Deck
APP_SHORTNAME   :=  StreamDeck
APP_AUTHOR      :=  LocalStreamDeck
export APP_ICON :=  $(TOPDIR)/icon.png
#---------------------------------------------------------------------------------
CFLAGS   := -O2 -Wall $(MACHDEP)
CXXFLAGS := $(CFLAGS) -std=c++17
ASFLAGS  := $(MACHDEP)
LDFLAGS   = $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS     := -lcurl -lmbedtls -lmbedcrypto -lmbedx509 -lSDL2_ttf -lSDL2 -lfreetype -lharfbuzz -lpng16 -lbz2 -lzstd -lbrotlidec -lbrotlicommon -lz -lwut -lm

LIBDIRS  := $(WUT_ROOT) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD   :=  $(CXX)

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_BIN) $(OFILES_SRC)

# *** THE FIX: use CPPFLAGS, not INCLUDE ***
export CPPFLAGS :=  \
    -I$(WUT_ROOT)/include \
    -I$(PORTLIBS_PATH)/wiiu/include \
    -I$(PORTLIBS_PATH)/ppc/include \
    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  \
    -L$(WUT_ROOT)/lib \
    -L$(PORTLIBS_PATH)/wiiu/lib \
    -L$(PORTLIBS_PATH)/ppc/lib

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).rpx $(TARGET).elf $(TARGET).wuhb

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).wuhb

$(OUTPUT).wuhb   : $(OUTPUT).rpx
$(OUTPUT).rpx    : $(OUTPUT).elf
$(OUTPUT).elf    : $(OFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
