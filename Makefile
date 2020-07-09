OS=$(shell uname -s)
ifneq ("$(wildcard .git)","")
CFLAGS?=-g -Wall
VERSION := $(shell git describe --dirty --tags | sed 's/^v//')
CURR_VERSION := $(shell sed 's/^\#define VERSION "\(.*\)"$$/\1/' version.h 2>/dev/null)
else
BASE_VERSION := $(shell cat base_version)
endif
ifeq ($(OS),Linux)
PREFIX?=/usr
MANDIR?=$(PREFIX)/share/man
LDLIBS?=-pthread -lutil
INSTGROUP?=root
else
PREFIX?=/usr/local
MANDIR?=$(PREFIX)/man
INSTGROUP?=wheel
ifneq ($(OS),Darwin)
LDLIBS?=-lpthread -lutil
endif
endif
DESTDIR?= ""
BINDIR?=$(PREFIX)/bin
SHAREDIR=$(PREFIX)/share/kplex
LANGS=$(shell ls msg)
LOCALES=$(LANGS:%=%.cat)

objects=kplex.o fileio.o serial.o bcast.o tcp.o options.o error.o lookup.o mcast.o gofree.o udp.o

all: version kplex

.PHONY: version
version:
	@if [ "$(VERSION)" != "$(CURR_VERSION)" ]; then \
	echo '#define VERSION "'$(VERSION)'"' > version.h; \
	fi

kplex: $(objects)
	$(CC) -o kplex $(objects) $(LDLIBS)

tcp.o: tcp.h
gofree.o: tcp.h
$(objects): kplex.h
kplex.o: kplex.c kplex_mods.h version.h
	$(CC) -c -DSHAREDIR=\"$(SHAREDIR)\" kplex.c

version.h:
	@echo '#define VERSION "'$(BASE_VERSION)'"' > version.h

%.cat: msg/%
	gencat $@ $<

$(LOCALES):

install: kplex $(LOCALES)
	test -d $(DESTDIR)$(BINDIR)  || install -d -g $(INSTGROUP) -o root -m 755 $(DESTDIR)$(BINDIR)
	install -g $(INSTGROUP) -o root -m 755 kplex $(DESTDIR)$(BINDIR)/kplex
	test -d $(DESTDIR)$(MANDIR)/man1 || install -d -g $(INSTGROUP) -o root -m 755 $(DESTDIR)$(MANDIR)/man1
	gzip -c kplex.1 > $(DESTDIR)$(MANDIR)/man1/kplex.1.gz
	test -d "$(DESTDIR)$(SHAREDIR)" || install -d -g $(INSTGROUP) -o root -m 755 $(DESTDIR)$(SHAREDIR)
	test -d "$(DESTDIR)$(SHAREDIR)/locale" || install -d -g $(INSTGROUP) -o root -m 755 $(DESTDIR)$(SHAREDIR/)locale
	test -d "$(DESTDIR)$(SHAREDIR)/locale" || install -d -m 755 $(DESTDIR)$(SHAREDIR)/locale
	for l in $(LANGS); do test -d $(DESTDIR)$(SHAREDIR)/locale/$$l || mkdir $(DESTDIR)$(SHAREDIR)/locale/$$l ; mv $${l}.cat $(DESTDIR)$(SHAREDIR)/locale/$$l/kplex.cat; done

uninstall:
	-rm -f $(DESTDIR)$(BINDIR)/kplex
	-rm -f $(DESTDIR)$(MANDIR)/man1/kplex.1.gz

clean:
	-rm -f kplex $(objects)

.PHONY: release
release:
	sudo ./release
