CC ?= clang
PKG_CONFIG ?= pkg-config

TARGET := bin/apx-relayd
QEMU_PROXY := bin/qemu-i386-apxproxy
QEMU_PROXY_BUILDER := scripts/build-qemu-i386-apxproxy.sh
QEMU_PROXY_SRC := src/apx_qemu_usbfs_proxy.c

BIN_DIR := bin
BUILD_DIR := build
RELAY_SRC := src/apx-relayd.c
RELAY_HDR := src/apx_relay_protocol.h
OBJS := $(BUILD_DIR)/apx-relayd.o

QEMU_VERSION ?= 8.2.7
QEMU_URL ?= https://download.qemu.org/qemu-$(QEMU_VERSION).tar.xz
QEMU_ARCHIVE_DIR ?= qemu-$(QEMU_VERSION)

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_LIBS := -lusb-1.0
endif

CPPFLAGS += $(LIBUSB_CFLAGS)
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Wpedantic
LDLIBS += $(LIBUSB_LIBS)

.PHONY: all clean relay qemu-proxy

all: $(TARGET) $(QEMU_PROXY)

relay: $(TARGET)

qemu-proxy: $(QEMU_PROXY)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(QEMU_PROXY): $(QEMU_PROXY_SRC) $(QEMU_PROXY_BUILDER) | $(BIN_DIR)
	QEMU_VERSION="$(QEMU_VERSION)" QEMU_URL="$(QEMU_URL)" QEMU_ARCHIVE_DIR="$(QEMU_ARCHIVE_DIR)" OUTPUT="$@" $(QEMU_PROXY_BUILDER)

$(BIN_DIR):
	mkdir -p $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/apx-relayd.o: $(RELAY_SRC) $(RELAY_HDR) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $(RELAY_SRC)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET) $(QEMU_PROXY)
