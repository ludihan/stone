PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin

CC ?= cc
CFLAGS := -std=c23 -Wall -Wextra -Wpedantic -Werror \
          -Wshadow -Wconversion -Wsign-conversion \
          -Wformat=2 -Wwrite-strings -Wnull-dereference \
          -Wstrict-prototypes -Wold-style-definition \
          -Wmissing-prototypes -Wmissing-declarations \
          -O2
LDFLAGS :=

stone: stone.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: stone
	install -d $(BINDIR)
	install -m 755 stone $(BINDIR)

uninstall:
	rm -f $(BINDIR)/stone

clean:
	rm -f stone

.PHONY: install uninstall clean
