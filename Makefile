
LDFLAGS+= -s

default: all

all: pgcputrack

clean:
	rm -f *.o pgcputrack

pgcputrack: pgcputrack.o

