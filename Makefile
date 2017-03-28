# SANITIZE=-fsanitize=undefined
SANITIZE=
CXXFLAGS  = -Wall -Ofast -march=native -fstrict-aliasing -std=c++11 -g -pthread $(SANITIZE)
LDFLAGS = -g $(SANITIZE)
CXX := ccache $(CXX)

all: halma

# halma.o : halma.hpp

halma: halma.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LOADLIBES) $(LDLIBS) -o $@

clean:
	rm -f *.o halma core
