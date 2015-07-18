OS=$(shell uname -s)
CFLAGS+= -g -Wall
BINDIR=/usr/local/bin
ifeq ($(OS),Linux)
LDLIBS+=-pthread -lutil
BINDIR=/usr/bin
INSTGROUP=root
else
INSTGROUP=wheel
ifneq ($(OS),Darwin)
LFLAGS=-lpthread -lutil
endif
endif

objects=kplex.o fileio.o serial.o bcast.o tcp.o options.o error.o lookup.o mcast.o gofree.o udp.o

kplex: $(objects)
	$(CC) -o kplex $(objects) $(LDFLAGS) $(LDLIBS)

tcp.o: tcp.h
gofree.o: tcp.h
$(objects): kplex.h
kplex.o: kplex_mods.h version.h

install:
	test -d "$(DESTDIR)/$(BINDIR)"  || install -d -g $(INSTGROUP) -o root -m 755 $(DESTDIR)/$(BINDIR)
	install -g $(INSTGROUP) -o root -m 755 kplex $(DESTDIR)/$(BINDIR)/kplex

uninstall:
	-rm -f $(DESTDIR)/$(BINDIR)/kplex

clean:
	rm -f kplex $(objects)

version.h:
	@echo "#define VERSION \""`git describe --abbrev=0 | sed 's/^v\(.*\)/\1-git/'`"\"" > version.h

.PHONY: release
release:
	sudo ./release
