CFLAGS = -g -O2 -Wall -Wextra -std=c99 \
         $(shell pkg-config --cflags wayland-client wayland-egl egl glesv2 mpv)
LDLIBS = $(shell pkg-config --libs wayland-client wayland-egl egl glesv2 mpv)
LDLIBS += -lrt

WL_PROTOCOLS = protocols/layer-shell-protocol.o protocols/xdg-shell-protocol.o

waytop: main.o overlay.o player.o $(WL_PROTOCOLS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

main.o: main.c overlay.h player.h
overlay.o: overlay.c overlay.h protocols/layer-shell-client-protocol.h
player.o: player.c player.h

protocols/layer-shell-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner private-code $< $@

protocols/layer-shell-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner client-header $< $@

protocols/xdg-shell-protocol.c: protocols/xdg-shell.xml
	wayland-scanner private-code $< $@

protocols/xdg-shell-client-protocol.h: protocols/xdg-shell.xml
	wayland-scanner client-header $< $@

protocols/layer-shell-protocol.o: protocols/layer-shell-protocol.c \
                                  protocols/layer-shell-client-protocol.h

protocols/xdg-shell-protocol.o: protocols/xdg-shell-protocol.c \
                                protocols/xdg-shell-client-protocol.h

clean:
	rm -f *.o protocols/*.o waytop

distclean: clean
	rm -f protocols/layer-shell-protocol.c \
	      protocols/layer-shell-client-protocol.h \
	      protocols/xdg-shell-protocol.c \
	      protocols/xdg-shell-client-protocol.h

.PHONY: clean distclean
