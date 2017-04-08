# SANITIZE=-fsanitize=address -fsanitize=leak -fsanitize=undefined
# SANITIZE= -fsanitize=thread
SANITIZE=

CXXFLAGS  = -Wall -Winline --param inline-unit-growth=200 --param large-function-growth=1000 -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -ffast-math -fno-signed-zeros -fno-trapping-math -Ofast -march=native -fstrict-aliasing $(CC_MALLOC) -std=c++11 -g -pthread $(SANITIZE)
LDFLAGS = -g $(SANITIZE)
CXX := ccache $(CXX)

# CXXFLAGS := $(CXXFLAGS) -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
# LDLIBS := $(LDLIBS) -l:libtcmalloc_minimal.so.4

all: halma

halma.o: halma.cpp moves.cpp Makefile

halma: halma.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LOADLIBES) $(LDLIBS) -o $@

clean:
	rm -f *.o halma core
