#include "halma.hpp"

// gcc is unable to inline the huge bodies if done as functions
// Hack around it using the preprocessor

#define BACKTRACK    0
#define BLUE_TO_MOVE 1
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK

#define BACKTRACK    0
#define BLUE_TO_MOVE 0
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK

#define BACKTRACK    1
#define BLUE_TO_MOVE 1
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK

#define BACKTRACK    1
#define BLUE_TO_MOVE 0
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK
