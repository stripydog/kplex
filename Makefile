OS=$(shell uname -s)
CFLAGS= -g -Wall
ifeq ($(OS),Linux)
LFLAGS=-pthread -lutil
endif

objects=kplex.o fileio.o serial.o bcast.o tcp.o options.o error.o lookup.o

kplex: $(objects)
	cc -o kplex $(objects) $(LFLAGS)

$(objects): kplex.h
kplex.o: kplex_mods.h

clean:
	rm -f kplex $(objects)
