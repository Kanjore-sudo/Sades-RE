# =====================================================================
# SADES engine - build system
# =====================================================================
#
# Targets:
#   make            release build
#   make debug      debug build with ASan / UBSan
#   make clean      remove build artifacts
#   make install    copy binary to $(PREFIX)/bin  (default: ~/.local)
#   make uninstall
#
# Dependencies:
#   SDL2        - windowing / rendering
#   freetype2   - Japanese text rendering
#   iconv       - Shift-JIS conversion (in glibc on Linux; on macOS,
#                 BSD and Termux, link with -liconv)
#
# Tested on Android/Termux (aarch64) and Linux (x86_64, aarch64).
# =====================================================================

# ---- Compiler and flags ---------------------------------------------

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -Wno-unused-parameter
LDFLAGS ?=

# ---- Dependency detection -------------------------------------------
#
# We try pkg-config first; if it isn't available we fall back to
# common include paths. This keeps the Makefile portable across
# Debian/Ubuntu/Fedora/Arch/macOS/Homebrew/Termux.

PKG_CONFIG ?= pkg-config

ifeq ($(shell $(PKG_CONFIG) --exists sdl2 2>/dev/null && echo yes),yes)
  SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
  SDL_LIBS   := $(shell $(PKG_CONFIG) --libs   sdl2)
else
  SDL_CFLAGS := -I/usr/include/SDL2 -I$(PREFIX)/include/SDL2
  SDL_LIBS   := -lSDL2
endif

ifeq ($(shell $(PKG_CONFIG) --exists freetype2 2>/dev/null && echo yes),yes)
  FT_CFLAGS := $(shell $(PKG_CONFIG) --cflags freetype2)
  FT_LIBS   := $(shell $(PKG_CONFIG) --libs   freetype2)
else
  FT_CFLAGS := -I/usr/include/freetype2
  FT_LIBS   := -lfreetype
endif

# iconv: glibc provides the symbols directly in libc. Termux (Android
# userland), macOS and BSD ship them in a separate libiconv. The most
# portable check is to look for the library file itself.
#
# Note: on Termux, PREFIX is /data/data/com.termux/files/usr, so the
# file exists there. On glibc systems, PREFIX is usually /usr/local
# and libiconv.* does not exist.
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

ICONV_LIBS :=
ifneq ($(wildcard $(PREFIX)/lib/libiconv.*),)
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

  ICONV_LIBS := -liconv
else ifneq ($(wildcard /data/data/com.termux/files/usr/lib/libiconv.*),)
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

  ICONV_LIBS := -liconv
else ifneq ($(wildcard /usr/lib/libiconv.*),)
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

  ICONV_LIBS := -liconv
else ifneq ($(wildcard /usr/local/lib/libiconv.*),)
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

  ICONV_LIBS := -liconv
endif
UNAME_S := $(shell uname -s)

INCLUDES := $(SDL_CFLAGS) $(FT_CFLAGS)
LIBS     := $(SDL_LIBS) $(FT_LIBS) $(ICONV_LIBS) -lm

# ---- Sources and targets --------------------------------------------

SRCS   := main.c glz.c m00.c sce.c display.c font.c
OBJS   := $(SRCS:.c=.o)
TARGET := sades
HDRS   := glz.h m00.h sce.h display.h font.h


.PHONY: all debug clean install uninstall

# ---- Build rules -----------------------------------------------------

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Debug build: AddressSanitizer + UBSan.
DEBUG_FLAGS = -g -O0 -DDEBUG -fsanitize=address,undefined
debug:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) $(DEBUG_FLAGS)" LDFLAGS="$(LDFLAGS) $(DEBUG_FLAGS)"

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# ---- Helpers ---------------------------------------------------------

# Print the resolved configuration. Useful when the build fails on an
# unfamiliar platform; the output shows exactly which flags were used.
.PHONY: config
config:
	@echo "CC        = $(CC)"
	@echo "CFLAGS    = $(CFLAGS)"
	@echo "SDL_CFLAGS= $(SDL_CFLAGS)"
	@echo "SDL_LIBS  = $(SDL_LIBS)"
	@echo "FT_CFLAGS = $(FT_CFLAGS)"
	@echo "FT_LIBS   = $(FT_LIBS)"
	@echo "ICONV_LIBS= $(ICONV_LIBS)"
	@echo "INCLUDES  = $(INCLUDES)"
	@echo "LIBS      = $(LIBS)"
	@echo "UNAME_S   = $(UNAME_S)"