OS=$(shell uname -s)
CFLAGS= -g -Wall
BINDIR=/usr/local/bin
ifneq ($(OS),Darwin)
ifeq ($(OS),Linux)
LFLAGS=-pthread -lutil
BINDIR=/usr/bin
else
LFLAGS=-lpthread -lutil
endif
endif

objects=kplex.o fileio.o serial.o bcast.o tcp.o options.o error.o lookup.o

kplex: $(objects)
	cc -o kplex $(objects) $(LFLAGS)

$(objects): kplex.h
kplex.o: kplex_mods.h

install:
	install -D -g root -o root -m 755 kplex $(DESTDIR)/$(BINDIR)/kplex

uninstall:
	-rm -f $(DESTDIR)/$(BINDIR)/kplex

clean:
	rm -f kplex $(objects)

.PHONY: release
release:
	sudo ./release
