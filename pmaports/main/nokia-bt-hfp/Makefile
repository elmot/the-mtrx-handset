CC ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr

# abuild overrides CFLAGS/LDFLAGS; keep warnings, std, and section GC regardless
CFLAGS ?= -Os
override CFLAGS += -Wall -Wextra -Wshadow -std=gnu11 -ffunction-sections -fdata-sections
override LDFLAGS += -Wl,--gc-sections
PKGS := gio-2.0 gio-unix-2.0 alsa speexdsp

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))

SRCS := nokia_bt_hfp.c alsa_bridge.c
OBJS := $(SRCS:.c=.o)
TARGET := nokia-bt-hfp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(PKG_LIBS) -pthread

%.o: %.c alsa_bridge.h
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/$(TARGET)"

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
