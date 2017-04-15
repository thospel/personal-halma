# SANITIZE=-fsanitize=address -fsanitize=leak -fsanitize=undefined
# SANITIZE= -fsanitize=thread
SANITIZE=

CXXFLAGS  = -Wall -Winline --param inline-unit-growth=200 --param large-function-growth=1000 --param max-inline-insns-single=800 -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -ffast-math -fno-signed-zeros -fno-trapping-math -Ofast -march=native -fstrict-aliasing $(CC_MALLOC) -std=c++11 -g -pthread $(SANITIZE)
LDFLAGS = -g $(SANITIZE)
CXX := ccache $(CXX)

# CXXFLAGS := $(CXXFLAGS) -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
# LDLIBS := $(LDLIBS) -l:libtcmalloc_minimal.so.4

CXXFLAGS := $(CXXFLAGS) -DCOMMIT=`git rev-parse HEAD`

all: halma

halma.o: halma.cpp moves.cpp Makefile

halma: halma.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LOADLIBES) $(LDLIBS) -o $@

.PHONY: clean
clean:
	rm -f *.o halma core
