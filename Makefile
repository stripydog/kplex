LFLAGS=-pthread -lutil
objects=kplex.o fileio.o serial.o bcast.o tcp.o

kplex: $(objects)
	cc -o kplex $(LFLAGS) $(objects)

$(objects): kplex.h
kplex.o: kplex_mods.h

clean:
	rm -f kplex $(objects)
