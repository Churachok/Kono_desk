PKG_CONFIG = pkg-config
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

WLR_CFLAGS = $(shell $(PKG_CONFIG) --cflags wlroots-0.19)
WLR_LIBS = $(shell $(PKG_CONFIG) --libs wlroots-0.19)
IMG_CFLAGS = $(shell $(PKG_CONFIG) --cflags libpng)
IMG_LIBS = $(shell $(PKG_CONFIG) --libs libpng) -ljpeg

CFLAGS ?= -Wall -Wextra -g -O2
CFLAGS += $(WLR_CFLAGS) $(IMG_CFLAGS) -DWLR_USE_UNSTABLE -I. -Iprotocols
LDFLAGS = $(WLR_LIBS) $(IMG_LIBS) -lwayland-server -lxkbcommon -lm

SRC = main.c server.c output.c seat.c config.c wallpaper.c
OBJ = $(SRC:.c=.o)
TARGET = kono

all: $(TARGET)

protocols/wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) server-header $< $@

$(OBJ): protocols/wlr-layer-shell-unstable-v1-protocol.h server.h config.h

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET) protocols/wlr-layer-shell-unstable-v1-protocol.h

run: $(TARGET)
	./$(TARGET)

install-session: $(TARGET)
	chmod +x kono-session install-sddm.sh
	@echo "SDDM hides sessions whose TryExec is inside a private home directory."
	@echo "Install into /usr/local so the greeter can see Kono:"
	@echo "  sudo $(CURDIR)/install-sddm.sh"

.PHONY: all clean run install-session
