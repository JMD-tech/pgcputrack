
LDFLAGS+= -lprocps -lstdc++ -lrt -g
CXXFLAGS+= -std=c++11

default: all

all: pgcputrack showpgcpuse

clean:
	rm -f *.o pgcputrack

pgcputrack: pgcputrack.o

showpgcpuse: showpgcpuse.o
