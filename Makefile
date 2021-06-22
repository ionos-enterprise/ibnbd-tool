# SPDX-License-Identifier: GPL-2.0-or-later
# Makefile for building rnbd

PREFIX ?= /usr/local
VERSION := 1.0.19
GITVER = $(shell cat ./.git/HEAD 2>/dev/null | sed -n 's/\(ref: refs\/heads\/\)\?\(.*\)/\2/p' | tr -d '\n')

CC = gcc
DEFINES = -DPACKAGE_VERSION='"$(VERSION)"' -DGIT_BRANCH='"$(GITVER)"'
CFLAGS = -fPIC -Wall -Werror -Wno-stringop-truncation -O2 -g -Iinclude $(DEFINES)
LIBS =

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

TARGETS_OBJ = rnbd.o
TARGETS = $(TARGETS_OBJ:.o=)

      rnbd_OBJ = levenshtein.o misc.o table.o rnbd-sysfs.o list.o

.PHONY: all
all: $(TARGETS)

dist: rnbd-$(VERSION).tar.xz rnbd-$(VERSION).tar.xz.asc

version:
	@echo $(VERSION)

%.asc: %
	gpg --armor --batch --detach-sign --yes --output $@ $^

%.tar.xz: $(DIST)
	tar -c --exclude-vcs --transform="s@^@$*/@" $^ | xz -cz9 > $@

install: all
	install -D -m 755 rnbd $(DESTDIR)$(PREFIX)/sbin/rnbd
	install -D -m 644 man/rnbd.8 $(DESTDIR)$(PREFIX)/share/man/man8/rnbd.8

$(TARGETS): $(OBJ)
	$(CC) -o $@ $@.o $($@_OBJ) $(LIBS)

ifneq ($(MAKECMDGOALS),clean)
# do not include for 'clean' goal. make won’t create *.d only to
# immediately remove them again.
-include $(OBJ:.o=.d)
endif

# Stolen from:
# https://www.gnu.org/software/make/manual/html_node/Automatic-Prerequisites.html
%.d: %.c
	@set -e; rm -f $@; \
	$(CC) -M $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

clean:
	rm -f *~ $(TARGETS) $(OBJ) $(OBJ:.o=.d)

.PHONY: all clean install version
