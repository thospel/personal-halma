/*
  Compile using something like:
    g++ -Wall -O3 -march=native -std=c++11 -s halma.cpp -o halma
*/

// Todo:
//   Variations:
//     - Allow pass
//     - Allow loop to self (effective pass)
//     - Allow leaving target area again
#include <array>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <unordered_set>

#include <pcg/pcg_random.hpp>

using namespace std;

#ifdef __GNUC__
# define NOINLINE	__attribute__((__noinline__))
#else // __GNUC__
# define NOINLINE
#endif // __GNUC__

pcg64 rng(0);

int const X = 9;
int const Y = 9;
int const RULE = 6;
int const ARMY = 10;
bool const CLOSED_LOOP = false;

unsigned int target = 30;

enum Color : uint8_t { EMPTY, BLUE, RED, COLORS };

// Special behavior for ++Colors
Color& operator++(Color& c) {
    c = static_cast<Color>(static_cast<int8_t>(c) + 1);
    return c;
}

struct Coord {
    Coord() {};
    Coord(int _x, int _y) : x(_x), y(_y) {};
    int8_t x,y;
};

struct Board;
struct Board {
    static vector<Coord> edge_base_red;
    static array<array<uint8_t, 2*X-1>, 2*Y-1> norm_;
    static array<array<uint8_t, 2*X-1>, 2*Y-1> distance_;
    static array<array<uint8_t, X>, Y> base_packed;
    static array<array<Color, X>, Y> base;
    static array<Coord, ARMY> base_blue;
    static array<Coord, ARMY> base_red;
    static array<Coord, RULE> moves_;
    static array<array<uint8_t, X>, Y> distance_base_red;
    static array<array<array<uint64_t, X>, Y>, COLORS> zobrist_;
    static uint64_t zobrist_red;
    static uint8_t infinity;
    static void init_static();
    static void fill_norm();
    static void show_norm();
    static void fill_base();
    static void show_base();
    static void fill_zobrist();
    static void show_zobrist();
    static uint8_t distance(Coord const from, Coord const to);

    Board(bool red_to_move = false);
    void insert(Coord const from, Coord const to);
    void _insert(Coord const from, Coord const to);
    void show();
    void update_lower_bound();
    uint8_t lower_bound() const { return lower_bound_; }
    void moves();

    uint64_t hash;
    Board* parent_;
    uint8_t distance_army_blue_base_red;
    uint8_t distance_army_blue_army_red;
    uint8_t unpacked;
    uint8_t lower_bound_;
    uint8_t ply_ = 0;
    array<Coord, ARMY> blue;
    array<Coord, ARMY> red;
    array<array<Color, X>, Y> board;
    bool red_to_move_;
};

inline bool operator==(Board const&lhs, Board const&rhs) {
    if (lhs.red_to_move_ != rhs.red_to_move_) return false;
    return lhs.board == rhs.board;
}

struct Hash {
    size_t operator() (Board const&board) const { return board.hash; }
};

unordered_set<Board, Hash> boards_seen;
vector<vector<Board*>> queue;

vector<Coord> Board::edge_base_red;
array<array<uint8_t, 2*X-1>, 2*Y-1> Board::norm_;
array<array<uint8_t, 2*X-1>, 2*Y-1> Board::distance_;
array<array<uint8_t, X>, Y> Board::distance_base_red;
array<Coord, RULE> Board::moves_;
array<array<uint8_t, X>, Y> Board::base_packed;
array<array<Color, X>, Y> Board::base;
array<Coord, ARMY> Board::base_blue;
array<Coord, ARMY> Board::base_red;
array<array<array<uint64_t, X>, Y>, COLORS> Board::zobrist_;
uint64_t Board::zobrist_red;
uint8_t Board::infinity;

inline
uint8_t Board::distance(Coord const from, Coord const to) {
    int x = to.x - from.x;
    int y = to.y - from.y;
    return distance_[Y-1+y][X-1+x];
}

void Board::fill_norm() {
    if (RULE == 8) {
        for (int y=1-Y; y < Y; ++y) {
            int ay = abs(y);
            for (int x=1-X; x < X; ++x) {
                int ax = abs(x);
                norm_[Y-1+y][X-1+x] = max(ax, ay);
            }
        }
    } else if (RULE == 6) {
        for (int y=1-Y; y < Y; ++y) {
            int ay = abs(y);
            for (int x=1-X; x < X; ++x) {
                int ax = abs(x);
                norm_[Y-1-y][X-1+x] = (ax+ay+abs(x-y))/2;
            }
        }
    } else if (RULE == 4) {
        for (int y=1-Y; y < Y; ++y) {
            int ay = abs(y);
            for (int x=1-X; x < X; ++x) {
                int ax = abs(x);
                norm_[Y-1+y][X-1+x] = ax+ay;
            }
        }
    } else {
        throw(logic_error("Unknown rule " + to_string(RULE)));
    }

    infinity = 0;
    int rule = 0;
    for (int y=1-Y; y < Y; ++y)
        for (int x=1-X; x < X; ++x) {
            auto n = norm_[Y-1+y][X-1+x];
            if (n == 1) {
                if (rule >= RULE) throw(logic_error("too many moves"));
                moves_[rule++] = {x, y};
            }
            infinity = max(infinity, n);
            distance_[Y-1+y][X-1+x] = n <= 2 ? 0 : n-2;
        }
    if (rule < RULE) throw(logic_error("too many rules"));
    ++infinity;
}

void Board::show_norm() {
    printf("Norm:\n");
    for (int y=1-Y; y < Y; ++y) {
        for (int x=1-X; x < X; ++x) {
            printf(" %2d", norm_[Y-1+y][X-1+x]);
        }
        printf("\n");
    }
    printf("Infinity=%d\n", infinity);
    printf("Moves:\n");
    for (auto const& move: moves_)
        printf(" %2d %2d\n", move.x, move.y);
}

void Board::fill_base() {
    for (auto& row:base)
        for (auto& pos: row)
            pos = EMPTY;

    for (auto& row:base_packed)
        for (auto& pos: row)
            pos = 0;

    int d = 0;
    int a = ARMY;
    int i = 0;
    while (a) {
        int n = min(d+1,a);
        a -= n;
        int x = d - (d+1-n)/2;
        int y = d-x;
        while (n--) {
            if (base[y][x] != EMPTY)
                throw(logic_error("Army overlap"));
            base[y][x] = BLUE;
            base_blue[i] = { x, y };

            if (base[Y-1-y][X-1-x] != EMPTY)
                throw(logic_error("Army overlap"));
            base[Y-1-y][X-1-x] = RED;
            base_packed[Y-1-y][X-1-x] = 1;
            base_red[i] = { X-1-x, Y-1-y };

            ++i;
            --x;
            ++y;
        }
        d++;
    }

    for (int y=0; y<Y; ++y)
        for (int x=0; x<X; ++x) {
            uint8_t d = infinity;
            Coord from{x, y};
            for (auto const&to: base_red)
                d = min(d, distance(from, to));
            distance_base_red[y][x] = d;
        }

    for (auto const&to: base_red)
        for (auto const&move: moves_) {
            auto x = to.x - move.x;
            if (x < 0 || x >= X) continue;
            auto y = to.y - move.y;
            if (y < 0 || y >= Y) continue;
            if (distance_base_red[y][x] == 0 && base[y][x] == EMPTY) {
                edge_base_red.push_back(to);
                break;
            }
        }
}

void Board::show_base() {
    printf("Base:\n");
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            printf(" %d", base[y][x]);
        }
        printf("\n");
    }
    printf("Distance to red base:\n");
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            printf(" %2d", distance_base_red[y][x]);
        }
        printf("\n");
    }
    printf("Edge of red base:\n");
    for (auto const&pos: edge_base_red)
        printf(" %2d %2d\n", pos.x, pos.y);
}

void Board::fill_zobrist() {
    zobrist_red = rng();
    for (Color c=BLUE; c<=RED;++c)
        for (int y=0; y < Y; ++y)
            for (int x=0; x < X; ++x)
                zobrist_[c][y][x] = rng();
}

void Board::show_zobrist() {
    printf("Zobrist:\n");
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            for (Color c=EMPTY; c<COLORS;++c)
                printf(" %016lx", zobrist_[c][y][x]);
            printf("\n");
        }
        printf("\n");
    }
}

Board::Board(bool red_to_move) : parent_(nullptr), red_to_move_(red_to_move) {
    board = base;
    blue = base_blue;
    red  = base_red;
    hash = red_to_move ? zobrist_red : 0;
    auto distance_blue_red = infinity;
    for (auto& pos : blue) {
        hash ^= zobrist_[BLUE][pos.y][pos.x];
        distance_blue_red = min(distance_blue_red, distance_base_red[pos.y][pos.x]);
    }
    for (auto& pos : red)
        hash ^= zobrist_[RED][pos.y][pos.x];

    distance_army_blue_base_red = distance_blue_red;
    distance_army_blue_army_red = distance_blue_red;

    unpacked = ARMY;

    update_lower_bound();
}

void Board::update_lower_bound() {
    if (unpacked == 0) throw(logic_error("Lower bound on finished game"));
    // blue moves without using the red army
    unsigned int blue_direct = distance_army_blue_base_red+unpacked;

    // blue moves jumping the red army
    if (distance_army_blue_base_red) {
        auto from_blue = distance_army_blue_army_red;
        if (red_to_move_ && from_blue > 0) --from_blue;
        unsigned int blue_indirect = (1+from_blue)/2;
        blue_direct = min(blue_direct, blue_indirect);
    }
    blue_direct += unpacked;
    blue_direct *=2;
    if (!red_to_move_) --blue_direct;
    lower_bound_ = blue_direct;
}

void Board::show() {
    printf("Board, ply %d (%s to move):\n",
           ply_, red_to_move_ ? "red" : "blue");
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            printf(" %d", board[y][x]);
        }
        printf("\n");
    }
    for (int i=0; i<ARMY; ++i)
        printf("Blue %2d: [%2d, %2d]\n", i, blue[i].x, blue[i].y);
    for (int i=0; i<ARMY; ++i)
        printf("Red  %2d: [%2d, %2d]\n", i, red [i].x, red [i].y);

    printf("Zobrist = %016lx\n", hash);
    printf("Distance from blue to red base: %d\n", distance_army_blue_base_red);
    printf("Distance from blue to red army: %d\n", distance_army_blue_army_red);
    printf("Targets still open: %d\n", unpacked);
    printf("Lower bound: %d\n", lower_bound());
    printf("\n");
}

void Board::init_static() {
    fill_norm();
     show_norm();
    fill_base();
     show_base();
    fill_zobrist();
    // show_zobrist();
}

inline
void Board::_insert(Coord const from, Coord const to) {
    if (red_to_move_) {
        // The previous move was by blue
        auto distance = distance_base_red[to.y][to.x];
        if (distance <= distance_army_blue_base_red)
            distance_army_blue_base_red = distance;
        else if (distance_base_red[from.y][from.x] == distance_army_blue_base_red) {
            // It could be we increased the distance
            distance = infinity;
            for (auto const& soldier: blue)
                distance = min(distance, distance_base_red[soldier.y][soldier.x]);
            distance_army_blue_base_red = distance;
        }
        unpacked = unpacked +
            base_packed[to.y][to.x] - base_packed[from.y][from.x];
    }

    auto& army  = red_to_move_ ? red : blue;
    auto d = infinity;
    for (auto const& soldier: army)
        d = min(d, distance(soldier, to));
    if (d <= distance_army_blue_army_red)
        distance_army_blue_army_red = d;
    else {
        // New distance isn't lower. Maybe we increased the old distance
        d = infinity;
        for (auto const& soldier: army)
            d = min(d, distance(soldier, from));
        if (d == distance_army_blue_army_red) {
            // The current move may indeed have changed the old distance. 
            // Do a full recalculation
            d = infinity;
            for (auto& from: blue)
                for (auto& to: red)
                    d = min(d, distance(from, to));
            distance_army_blue_army_red = d;
        }
    }

    update_lower_bound();
    unsigned int plies = ply_ + lower_bound_;
    if (queue.size() <= plies) queue.resize(plies+1);
    queue.at(plies).push_back(this);

    // show();
}

NOINLINE
void Board::insert(Coord const from, Coord const to) {
    auto result = boards_seen.insert(*this);
    if (!result.second) return;

    // The board is unseen and was indeed inserted
    // Update the mutable elements in the new copy that are currently wrong
    Board& board = const_cast<Board&>(*result.first);
    board.parent_ = this;
    board._insert(from, to);
}

void Board::moves() {
    auto& army  = red_to_move_ ? red : blue;
    auto const& zobrist = red_to_move_ ? zobrist_[RED] : zobrist_[BLUE];
    Color color = red_to_move_ ? RED : BLUE;

    ++ply_;
    hash ^= zobrist_red;
    red_to_move_ = !red_to_move_;

    for (auto& soldier: army) {
        hash ^= zobrist[soldier.y][soldier.x];
        auto hash_old = hash;
        board[soldier.y][soldier.x] = EMPTY;

        // Jumps
        array<Coord, ARMY*2*RULE+1> reachable;
        reachable[0] = soldier;
        if (!CLOSED_LOOP) board[soldier.y][soldier.x] = COLORS;
        int nr_reachable = 1;
        for (int i=0; i < nr_reachable; ++i) {
            for (auto const& move: moves_) {
                auto x = reachable[i].x + 2*move.x;
                if (x < 0 || x >= X) continue;
                auto y = reachable[i].y + 2*move.y;
                if (y < 0 || y >= Y) continue;
                if (board[y][x] != EMPTY) continue;
                auto x0 = reachable[i].x + move.x;
                auto y0 = reachable[i].y + move.y;
                if (board[y0][x0] != BLUE && board[y0][x0] != RED) continue;
                board[y][x] = COLORS;
                reachable[nr_reachable++] = {x, y};
            }
        }
        for (int i=CLOSED_LOOP; i < nr_reachable; ++i)
            board[reachable[i].y][reachable[i].x] = EMPTY;
        for (int i=1; i < nr_reachable; ++i) {
            auto x = reachable[i].x;
            auto y = reachable[i].y;
            board[y][x] = color;
            hash ^= zobrist[y][x];
            soldier.x = x;
            soldier.y = y;

            insert(reachable[0], soldier);

            // soldier = reachable[0];
            hash = hash_old;
            board[y][x] = EMPTY;
        }
        soldier = reachable[0];

        // Step moves
        for (auto const& move: moves_) {
            auto x = soldier.x + move.x;
            if (x < 0 || x >= X) continue;
            auto y = soldier.y + move.y;
            if (y < 0 || y >= Y) continue;
            if (board[y][x] != EMPTY) continue;
            board[y][x] = color;
            hash ^= zobrist[y][x];
            soldier.x = x;
            soldier.y = y;

            insert(reachable[0], soldier);

            soldier = reachable[0];
            hash = hash_old;
            board[y][x] = EMPTY;
        }

        board[soldier.y][soldier.x] = color;
        hash ^= zobrist[soldier.y][soldier.x];
    }

    red_to_move_ = !red_to_move_;
    hash ^= zobrist_red;
    --ply_;
}

void my_main(int argc, char const* const* argv) {

    Board::init_static();
    Board empty_board(true);
    auto result = boards_seen.insert(empty_board);
    if (!result.second)
        throw(logic_error("Could not insert initial board into empty sety"));
    Board& board = const_cast<Board&>(*result.first);
    board.show();
    unsigned int plies = board.lower_bound();
    queue.resize(plies+1);
    queue.at(plies).push_back(&board);
    board.show();

    while (plies < queue.size()) {
        if (queue.at(plies).size() == 0) {
            printf("No solution of %d plies\n", plies);
            ++plies;
            continue;
        }
        vector<Board*> work;
        work.swap(queue.at(plies));
        for (auto bptr: work) {
            Board& board = *bptr;
            board.moves();
        }
    }
}

int main(int argc, char const* const* argv) {
    try {
        my_main(argc, argv);
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
