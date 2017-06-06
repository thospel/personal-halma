# SANITIZE=-fsanitize=address -fsanitize=leak -fsanitize=undefined
# SANITIZE=-fsanitize=thread

# CC_MALLOC=-fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
# LIBS_MALLOC=-l:libtcmalloc_minimal.so.4

CXXFLAGS := -Wall -Wextra -Wformat=2 -Wfloat-equal -Wlogical-op -Wshift-overflow=2 -Wduplicated-cond -Wcast-qual -Wcast-align -Winline --param inline-unit-growth=200 --param large-function-growth=1000 --param max-inline-insns-single=800 -mno-vzeroupper -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -ffast-math -fno-signed-zeros -fno-trapping-math -Ofast -march=native -fstrict-aliasing $(CC_MALLOC) -std=c++14 -g -pthread $(SANITIZE)
# CXXFLAGS += -Wrestrict
# CXXFLAGS += -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC -D_FORTIFY_SOURCE=2
# CXXFLAGS += -D CHECK=1

LDFLAGS = -g -pthread $(SANITIZE)
CXX := ccache $(CXX)

LDLIBS += $(LIBS_MALLOC)

CXXFLAGS += -DCOMMIT="`git rev-parse HEAD`" -DCOMMIT_TIME="`git show -s --format=%ci HEAD`"

all: halma

halma.o fast_moves.o slow_moves.o: halma.hpp xxhash64.h system.hpp Makefile
halma.o: pdqsort.h
system.o: system.hpp Makefile

halma.o: halma.cpp
fast_moves.o: fast_moves.cpp all_moves.cpp moves.cpp
slow_moves.o: slow_moves.cpp all_moves.cpp moves.cpp
system.o: system.cpp git_time

halma: halma.o fast_moves.o slow_moves.o system.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LOADLIBES) $(LDLIBS) -o $@

git_time: FORCE
	@touch --date=@`git show -s --format=%ct HEAD` git_time

FORCE:

.PHONY: clean
clean:
	rm -f *.o halma core
