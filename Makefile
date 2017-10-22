OS=$(shell uname -s)
ifneq ("$(wildcard .git)","")
CFLAGS?=-g -Wall
VERSION := $(shell git describe --dirty --tags | sed 's/^v//')
CURR_VERSION := $(shell sed 's/^\#define VERSION "\(.*\)"$$/\1/' version.h 2>/dev/null)
else
BASE_VERSION := $(shell cat base_version)
endif
ifeq ($(OS),Linux)
DESTDIR?=/usr
LDLIBS?=-pthread -lutil
INSTGROUP?=root
else
DESTDIR?=/usr/local
MANDIR?=$(DESTDIR)/man
INSTGROUP?=wheel
ifneq ($(OS),Darwin)
LDLIBS?=-lpthread -lutil
endif
endif
BINDIR?=$(DESTDIR)/bin
MANDIR?=$(DESTDIR)/share/man

objects=kplex.o fileio.o serial.o bcast.o tcp.o options.o error.o lookup.o mcast.o gofree.o udp.o

all: version kplex

.PHONY: version
version:
	@if [ "$(VERSION)" != "$(CURR_VERSION)" ]; then \
	echo '#define VERSION "'$(VERSION)'"' > version.h; \
	fi

kplex: $(objects)
	$(CC) -o kplex $(objects) $(LDFLAGS) $(LDLIBS)

tcp.o: tcp.h
gofree.o: tcp.h
$(objects): kplex.h
kplex.o: kplex_mods.h version.h

version.h:
	@echo '#define VERSION "'$(BASE_VERSION)'"' > version.h

install: kplex
	test -d "$(BINDIR)"  || install -d -g $(INSTGROUP) -o root -m 755 $(BINDIR)
	install -g $(INSTGROUP) -o root -m 755 kplex $(BINDIR)/kplex
	test -d $(MANDIR)/man1 && gzip -c kplex.1 > $(MANDIR)/man1/kplex.1.gz

uninstall:
	-rm -f $(BINDIR)/kplex
	-rm -f $(MANDIR)/man1/kplex.1.gz

clean:
	-rm -f kplex $(objects)

.PHONY: release
release:
	sudo ./release
