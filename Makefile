CXXFLAGS  = -Wall -O3 -march=native -fstrict-aliasing -std=c++11 -g -pthread
LDFLAGS = -g
CXX := ccache $(CXX)

all: halma

# halma.o : halma.hpp

halma: halma.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LOADLIBES) $(LDLIBS) -o $@

clean:
	rm -f *.o halma core
