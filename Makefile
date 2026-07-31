# -*- Mode: makefile-gmake -*-

.PHONY: clean all debug release

# Allow building against an oFono variant.
OFONO_PKG ?= ofono

#
# Required packages
#
# ofono.pc adds -export-symbols-regex linker option which doesn't work
# on all platforms.
#

LDPKGS = libofonobinderpluginext libgbinder-radio libgbinder libglibutil gobject-2.0 glib-2.0 libandroid-properties gio-2.0
PKGS = $(OFONO_PKG) $(LDPKGS)

#
# Default target
#

all: debug release

#
# Library name
#

NAME = mtkbinderpluginext
LIB_NAME = $(NAME)
LIB_SONAME = $(LIB_NAME).so
LIB = $(LIB_SONAME)
STATIC_LIB = $(NAME).a

#
# Sources
#

SRC = \
  mtk_ext.c \
  mtk_ims.c \
  mtk_ims_call.c \
  mtk_ims_sms.c \
  mtk_radio_ext.c \
  mtk_plugin.c \
  mtk_slot.c \
  nm_dbus.c \
  dbus_ext.c \
  binder_util.c

#
# Directories
#

SRC_DIR = src
BUILD_DIR = build
DEBUG_BUILD_DIR = $(BUILD_DIR)/debug
RELEASE_BUILD_DIR = $(BUILD_DIR)/release

#
# Tools and flags
#

CC = $(CROSS_COMPILE)gcc
LD = $(CC)
WARNINGS = -Wall
BASE_FLAGS = -fPIC -fvisibility=hidden
TRIPLET = $(shell $(CC) -dumpmachine)
BINDER_PLUGIN_INCLUDE_PATH = /usr/include/$(TRIPLET)/ofonobinderpluginext/
FULL_CFLAGS = $(BASE_FLAGS) $(CFLAGS) $(DEFINES) $(WARNINGS) -MMD -MP \
  $(shell pkg-config --cflags $(PKGS)) -I$(BINDER_PLUGIN_INCLUDE_PATH)
FULL_LDFLAGS = $(BASE_FLAGS) $(LDFLAGS) -shared \
  $(shell pkg-config --libs $(LDPKGS))
DEBUG_FLAGS = -g
RELEASE_FLAGS =

KEEP_SYMBOLS ?= 0
ifneq ($(KEEP_SYMBOLS),0)
RELEASE_FLAGS += -g
endif

DEBUG_LDFLAGS = $(FULL_LDFLAGS) $(DEBUG_FLAGS)
RELEASE_LDFLAGS = $(FULL_LDFLAGS) $(RELEASE_FLAGS)
DEBUG_CFLAGS = $(FULL_CFLAGS) $(DEBUG_FLAGS) -DDEBUG
RELEASE_CFLAGS = $(FULL_CFLAGS) $(RELEASE_FLAGS) -O2

#
# Files
#

DEBUG_OBJS = $(SRC:%.c=$(DEBUG_BUILD_DIR)/%.o)
RELEASE_OBJS = $(SRC:%.c=$(RELEASE_BUILD_DIR)/%.o)

DEBUG_SO = $(DEBUG_BUILD_DIR)/$(LIB)
RELEASE_SO = $(RELEASE_BUILD_DIR)/$(LIB)

#
# Dependencies
#

DEPS = $(DEBUG_OBJS:%.o=%.d) $(RELEASE_OBJS:%.o=%.d)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(DEPS)),)
-include $(DEPS)
endif
endif

$(DEBUG_OBJS) $(DEBUG_SO): | $(DEBUG_BUILD_DIR)
$(RELEASE_OBJS) $(RELEASE_SO): | $(RELEASE_BUILD_DIR)

#
# Rules
#

debug: $(DEBUG_SO)

release: $(RELEASE_SO)

clean:
	rm -f $(SRC_DIR)/*~ rpm/*~ *~
	rm -fr $(BUILD_DIR)

$(DEBUG_BUILD_DIR):
	mkdir -p $@

$(RELEASE_BUILD_DIR):
	mkdir -p $@

$(DEBUG_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(RELEASE_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(DEBUG_SO): $(DEBUG_OBJS)
	$(LD) $(DEBUG_OBJS) $(DEBUG_LDFLAGS) $(DEBUG_LIBS) -o $@

$(RELEASE_SO): $(RELEASE_OBJS)
	$(LD) $(RELEASE_OBJS) $(RELEASE_LDFLAGS) $(RELEASE_LIBS) -o $@

#
# Install
#

PLUGINDIR ?= $$(pkg-config $(OFONO_PKG) --variable=plugindir)
ABS_PLUGINDIR := $(shell echo /$(PLUGINDIR) | sed -r 's|/+|/|g')

INSTALL = install
INSTALL_PLUGIN_DIR = $(DESTDIR)$(ABS_PLUGINDIR)

install: $(INSTALL_PLUGIN_DIR)
	$(INSTALL) -m 644 $(RELEASE_SO) $(INSTALL_PLUGIN_DIR)

$(INSTALL_PLUGIN_DIR):
	$(INSTALL) -d $@
