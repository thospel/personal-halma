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

halma.o: halma.hpp halma.cpp git_time Makefile
fast_moves.o: halma.hpp all_moves.cpp fast_moves.cpp moves.cpp Makefile
slow_moves.o: halma.hpp all_moves.cpp slow_moves.cpp moves.cpp Makefile

halma: halma.o fast_moves.o slow_moves.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LOADLIBES) $(LDLIBS) -o $@

git_time: FORCE
	@touch --date=@`git show -s --format=%ct HEAD` git_time

FORCE:

.PHONY: clean
clean:
	rm -f *.o halma core
