CC ?= gcc
PKG_CONFIG ?= pkg-config

# abuild overrides CFLAGS/LDFLAGS; keep warnings and std regardless
CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -Wshadow -std=gnu11
PKGS := gio-2.0 gio-unix-2.0 alsa speexdsp

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))

SRCS := nokia_bt_hfp.c alsa_bridge.c
OBJS := $(SRCS:.c=.o)
TARGET := nokia-bt-hfp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(PKG_LIBS) -pthread -lm

%.o: %.c alsa_bridge.h
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
