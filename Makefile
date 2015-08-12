
LDFLAGS+= -lprocps -lstdc++ -s
CXXFLAGS+= -std=c++11

default: all

all: pgcputrack

clean:
	rm -f *.o pgcputrack

pgcputrack: pgcputrack.o

