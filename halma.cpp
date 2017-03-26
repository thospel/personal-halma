#include <cstdlib>
#include <cstdint>

#include <algorithm>
#include <array>
#include <string>
#include <iomanip>
#include <iostream>

// #define STATIC static
#define STATIC

#ifdef __GNUC__
# define RESTRICT __restrict__
# define NOINLINE	__attribute__((__noinline__))
# define LIKELY(x)	__builtin_expect(!!(x),true)
# define UNLIKELY(x)	__builtin_expect(!!(x),false)
# define HOT		__attribute__((__hot__))
# define COLD		__attribute__((__cold__))
#else // __GNUC__
# define RESTRICT
# define NOINLINE
# define LIKELY(x)	(x)
# define UNLIKELY(x)	(x)
# define HOT
# define COLD
#endif // __GNUC__

using namespace std;

int const X_MAX = 16;
int const Y_MAX = 16;

int const X = 9;
int const Y = 9;
int const MOVES = 6;
int const ARMY = 10;
bool const CLOSED_LOOP = false;
bool const PASS = false;

enum Color : uint8_t { EMPTY, BLUE, RED, COLORS };

class Coord;
class CoordE;
// Coordinate as an offset: y*X+x
class CoordZ {
  public:
    static int const SIZE = X*Y;
    CoordZ() {}
    explicit inline CoordZ(Coord const pos);
    explicit inline CoordZ(CoordE const pos);
    uint x() const { return pos_ % X; }
    uint y() const { return pos_ / X; }
    uint index() const { return pos_; }
  private:
    uint8_t pos_;
};

class CoordE {
  private:
    static int const ROW  = X+1;
  public:
    static int const SIZE = (Y+1)*ROW+X+2;
    static inline int offset(Coord const pos);

    CoordE() {}
    CoordE(int x, int y) : pos_{y*ROW+x} { }
    explicit inline CoordE(Coord const pos);
    explicit inline CoordE(CoordZ const pos);
    inline CoordE(CoordE from, int offset) : pos_{from.pos_ + offset} {}
    uint x() const { return pos_ % ROW; }
    uint y() const { return pos_ / ROW; }
    uint index() const { return OFFSET+ pos_; }
  private:
    static uint const OFFSET = ROW+1;
    int pos_;
};

class Army;
// Army as a set of CoordZ
class ArmyZ: public array<CoordZ, ARMY> {
  public:
    ArmyZ() {}
    explicit inline ArmyZ(Army const& army);
};

class ArmyE: public array<CoordE, ARMY> {
  public:
    ArmyE() {}
    explicit inline ArmyE(ArmyZ const& army) {
        for (int i=0; i < ARMY; ++i)
            (*this)[i] = CoordE{army[i]};
    }
};

class Board;
// Board as two ArmyZ
class BoardZ {
  public:
    BoardZ() {}
    explicit inline BoardZ(Board const& board);
    void make_moves(bool red_to_move = false) const;
    ArmyZ const& blue() const { return blue_; }
    ArmyZ const& red()  const { return red_; }
  private:
    ArmyZ blue_, red_;
};

class Coord {
  public:
    Coord() {};
    Coord( int x,  int y)  : x_(x), y_(y) {};
    Coord(uint x, uint y)  : x_(x), y_(y) {};
    explicit Coord(CoordZ pos) : Coord{pos.y(), pos.x()} {}
    int8_t x() const { return x_; }
    int8_t y() const { return y_; }
  private:
    int8_t x_,y_;
};

inline ostream& operator<<(ostream& os, Coord pos) {
    os << setw(3) << static_cast<int>(pos.x()) << " " << setw(3) << static_cast<int>(pos.y());
    return os;
};

// Army as a set of Coord
class Army: public array<Coord, ARMY> {
  public:
    Army() {}
    explicit Army(ArmyZ const& army) : Army{} {
        for (int i=0; i<ARMY; ++i) (*this)[i] = Coord{army[i]};
    }
};

class BoardTables;

class Board {
  public:
    Board() {}
    explicit Board(BoardZ const& board): blue_{board.blue()}, red_{board.red()} {}
    Army const& blue() const { return blue_; }
    Army const& red()  const { return red_; }
    inline void print(ostream& os) const;
  private:
    friend BoardTables;
    Army blue_, red_;
};

// Board as a double array of Colors
class Image {
  public:
    Image() {
        clear();
    }
    explicit Image(Board const& board);
    void print(ostream& os) const;
    void clear() {
        for (auto& row: board_)
            for (auto& pos: row)
                pos = EMPTY;
    }
    Color get(int x, int y) const { return board_[y][x]; }
    Color get(Coord pos)    const { return get(pos.x(), pos.y()); }
    void set(int x, int y, Color c) { board_[y][x] = c; }
    void set(Coord pos,    Color c) { set(pos.x(), pos.y(), c); }
    Color mirror_get(int x, int y) const { return board_[Y-1-y][X-1-x]; }
    void  mirror_set(int x, int y, Color c) { board_[Y-1-y][X-1-x] = c; }
  private:
    array<array<Color, X>, Y> board_;
};

using MovesE = array<int, MOVES>;

class BoardTables {
  public:
    BoardTables();
    void print_moves(ostream& os) const;
    void print_norm(ostream& os) const;
    void print_distance(ostream& os) const;
    void print_distance_base_red(ostream& os) const;
    void print_is_base_red(ostream& os) const;
    void print_coordZ(ostream& os) const;
    void print_coordE(ostream& os) const;
    uint8_t infinity() const { return infinity_; }
    Board  const& start () const { return start_; }
    BoardZ const& startZ() const { return startZ_; }
    uint8_t distance(Coord const from, Coord const to) const {
        int x = to.x() - from.x();
        int y = to.y() - from.y();
        return distance_[Y-1+y][X-1+x];
    }
    inline MovesE const& movesE() const { return movesE_; }
    inline CoordE coordE_from(CoordZ pos) const {
        return coordE_from_[pos.index()];
    }
    inline CoordZ coordZ_from(CoordE pos) const {
        return coordZ_from_[pos.index()];
    }
  private:
    uint8_t infinity_;
    array<array<uint8_t, 2*X-1>, 2*Y-1> norm_;
    array<array<uint8_t, 2*X-1>, 2*Y-1> distance_;
    array<array<uint8_t, X>, Y> distance_base_red_;
    array<array<uint8_t, X>, Y> is_base_red_;
    array<Coord, MOVES> moves_;
    MovesE movesE_;
    array<CoordE, CoordZ::SIZE> coordE_from_;
    array<CoordZ, CoordE::SIZE> coordZ_from_;
    Board  start_;
    BoardZ startZ_;
};

static BoardTables const board_tables;

CoordZ::CoordZ(Coord const pos) {
    pos_ = pos.y() * X + pos.x();
}

CoordZ::CoordZ(CoordE const pos) {
    *this = board_tables.coordZ_from(pos);
}

ArmyZ::ArmyZ(Army const& army) : ArmyZ{} {
    for (int i=0; i<ARMY; ++i) (*this)[i] = CoordZ{army[i]};
}

int CoordE::offset(Coord const pos) { 
    return pos.y() * ROW + pos.x();
}

CoordE::CoordE(Coord const pos) : pos_{ pos.y() * ROW + pos.x()} {}

CoordE::CoordE(CoordZ const pos) {
    *this = board_tables.coordE_from(pos);
}

BoardZ::BoardZ(Board const& board) : blue_{board.blue()}, red_{board.red()} {}

void Board::print(ostream& os) const {
    Image const board{*this};
    board.print(os);
}

inline ostream& operator<<(ostream& os, Board const& board) {
    board.print(os);
    return os;
}

// Board with borders
class BoardE {
  public:
    BoardE() {
        clear();
    }
    explicit BoardE(Board const& board) : BoardE{} {
        fill(board);
    }
    inline BoardE(ArmyE const&blue, ArmyE const&red);
    Color get(CoordE pos) const { return board_[pos.index()]; }
    void  set(CoordE pos, Color c) { board_[pos.index()] = c; }
    void clear();
    void print(ostream& os) const;
    void fill(Color color, Army const& army);
    void fill(Board const& board);
  private:
    array<Color, CoordE::SIZE> board_;
};

BoardE::BoardE(ArmyE const&blue, ArmyE const&red) : BoardE{} {
    for (auto soldier: blue)
        set(soldier, BLUE);
    for (auto soldier: red)
        set(soldier, RED);
}

void BoardE::fill(Color color, Army const& army) {
    for (auto soldier: army) {
        CoordE pos{soldier};
        set(pos, color);
    }
}

void BoardE::fill(Board const& board) {
    fill(BLUE, board.blue());
    fill(RED,  board.red());
}

void BoardE::print(ostream& os) const {
    for (int y=0; y<Y+2; ++y) {
        for (int x=0; x<X+2; ++x) {
            auto c = board_[y*(X+1)+x];
            os << (c == EMPTY ? ". " :
                   c == RED   ? "X " :
                   c == BLUE  ? "O " :
                   "? ");
        }
        os << "\n";
    }
}

inline ostream& operator<<(ostream& os, BoardE const& board) {
    board.print(os);
    return os;
}

void BoardE::clear() {
    for (auto& pos: board_) pos = EMPTY;
    for (int x=-1; x<X; ++x) {
        set(CoordE{x, -1}, COLORS);
        set(CoordE{x+1,  Y}, COLORS);
    }
    for (int y=0; y<Y+1; ++y)
        set(CoordE{-1, y}, COLORS);
}

BoardTables::BoardTables() {
    for (int y=0; y<Y; ++y)
        for (int x=0; x<Y; ++x) {
            Coord pos{x, y};
            CoordE posE{pos};
            CoordZ posZ{pos};
            coordZ_from_[posE.index()] = posZ;
            coordE_from_[posZ.index()] = posE;
        }

    if (MOVES == 8) {
        for (int y=1-Y; y < Y; ++y) {
            int ay = abs(y);
            for (int x=1-X; x < X; ++x) {
                int ax = abs(x);
                norm_[Y-1+y][X-1+x] = max(ax, ay);
            }
        }
    } else if (MOVES == 6) {
        for (int y=1-Y; y < Y; ++y) {
            int ay = abs(y);
            for (int x=1-X; x < X; ++x) {
                int ax = abs(x);
                norm_[Y-1-y][X-1+x] = (ax+ay+abs(x-y))/2;
            }
        }
    } else if (MOVES == 4) {
        for (int y=1-Y; y < Y; ++y) {
            int ay = abs(y);
            for (int x=1-X; x < X; ++x) {
                int ax = abs(x);
                norm_[Y-1+y][X-1+x] = ax+ay;
            }
        }
    } else {
        throw(logic_error("Unknown move " + to_string(MOVES)));
    }

    infinity_ = 0;
    int move = 0;
    for (int y=1-Y; y < Y; ++y)
        for (int x=1-X; x < X; ++x) {
            auto n = norm_[Y-1+y][X-1+x];
            if (n == 1) {
                if (move >= MOVES) throw(logic_error("too many moves"));
                moves_[move] = {x, y};
                movesE_[move] = CoordE::offset(moves_[move]);
                ++move;
            }
            infinity_ = max(infinity_, n);
            distance_[Y-1+y][X-1+x] = n <= 2 ? 0 : n-2;
        }
    if (move < MOVES) throw(logic_error("too few moves"));
    ++infinity_;

    // Fill base
    Image image;
    for (auto&row: is_base_red_)
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
        // cout << "[" << x << ", " << y << "]\n";
        while (n--) {
            if (image.get(x, y) != EMPTY)
                throw(logic_error("Army overlap"));
            image.set(x, y, BLUE);
            start_.blue_[i] = { x, y };

            if (image.mirror_get(x, y) != EMPTY)
                throw(logic_error("Army overlap"));
            image.mirror_set(x, y, RED);
            start_.red_[i] = { X-1-x, Y-1-y };
            is_base_red_[Y-1-y][X-1-x] = 1;

            ++i;
            --x;
            ++y;
        }
        d++;
    }
    startZ_ = BoardZ{start_};

    for (int y=0; y<Y; ++y)
        for (int x=0; x<X; ++x) {
            uint8_t d = infinity();
            Coord from{x, y};
            for (auto const& to: start_.red())
                d = min(d, distance(from, to));
            distance_base_red_[y][x] = d;
        }
}

void BoardTables::print_moves(ostream& os) const {
    for (int i=0; i<MOVES; ++i)
        os << moves_[i] << " " << setw(3) << movesE_[i] << "\n";
}

void BoardTables::print_norm(ostream& os) const {
    for (auto& row: norm_) {
        for (int pos: row)
            os << setw(3) << pos;
        os << "\n";
    }
}

void BoardTables::print_distance(ostream& os) const {
    for (auto& row: distance_) {
        for (int pos: row)
            os << setw(3) << pos;
        os << "\n";
    }
}

void BoardTables::print_distance_base_red(ostream& os) const {
    for (auto& row: distance_base_red_) {
        for (int pos: row)
            os << setw(3) << pos;
        os << "\n";
    }
}

void BoardTables::print_is_base_red(ostream& os) const {
    for (auto& row: is_base_red_) {
        for (int pos: row)
            os << setw(3) << pos;
        os << "\n";
    }
}

void BoardTables::print_coordZ(ostream& os) const {
    for (auto pos: coordZ_from_)
        os << " " << setw(4) << pos.index();
    os << "\n";
}
void BoardTables::print_coordE(ostream& os) const {
    for (auto pos: coordE_from_)
        os << " " << setw(4) << pos.index();
    os << "\n";
}

inline ostream& operator<<(ostream& os, Image const& image) {
    image.print(os);
    return os;
}

Image::Image(Board const& board) : Image{} {
    for (auto pos: board.blue())
        set(pos, BLUE);
    for (auto pos: board.red())
        set(pos, RED);
}

void Image::print(ostream& os) const {
    for (uint y=0; y<Y; ++y) {
        for (uint x=0; x<Y; ++x) {
            auto c = board_[y][x];
            os << (c == EMPTY ? ". " :
                   c == RED   ? "X " :
                   c == BLUE  ? "O " :
                   "? ");
        }
        os << "\n";
    }
}

void BoardZ::make_moves(bool red_to_move) const {
    ArmyE army_blue{blue()};
    ArmyE army_red {red()};
    Color color = red_to_move ? RED : BLUE;
    auto& armyE  = red_to_move ? army_red : army_blue;
    ArmyZ& armyZ = const_cast<ArmyZ&>(red_to_move ? red() : blue());
    red_to_move = !red_to_move;

    BoardE board{army_blue, army_red};

    for (int a=0; a<ARMY; ++a) {
        auto oldZ = armyZ[a];
        auto soldier = armyE[a];
        board.set(soldier, EMPTY);

        // Jumps
        array<CoordE, ARMY*2*MOVES+1> reachable;
        reachable[0] = soldier;
        int nr_reachable = 1;
        if (!CLOSED_LOOP) board.set(soldier, COLORS);
        for (int i=0; i < nr_reachable; ++i) {
            for (auto move: board_tables.movesE()) {
                CoordE jumpee{soldier, move};
                if (board.get(jumpee) != RED && board.get(jumpee) != BLUE) continue;
                CoordE target{jumpee, move};
                if (board.get(target) != EMPTY) continue;
                board.set(target, COLORS);
                reachable[nr_reachable++] = target;
            }
        }
        for (int i=CLOSED_LOOP; i < nr_reachable; ++i)
            board.set(reachable[i], EMPTY);
        for (int i=1; i < nr_reachable; ++i) {
            armyZ[a] = CoordZ{reachable[i]};
            cout << Board{*this} << "\n";
        }        

        // Step moves
        for (auto move: board_tables.movesE()) {
            CoordE target{soldier, move};
            if (board.get(target) != EMPTY) continue;
            armyZ[a] = CoordZ{target};
            cout << Board{*this} << "\n";
        }        

        board.set(soldier, color);
        armyZ[a] = oldZ;
    }
}

void my_main(int argc, char const* const* argv) {
    //Board board;
    //board.clear();
    //cout << board;
    cout << "Norm:\n";
    board_tables.print_norm(cout);
    cout << "Distance-2:\n";
    board_tables.print_distance(cout);
    cout << "Distance base red:\n";
    board_tables.print_distance_base_red(cout);
    cout << "Is base red:\n";
    board_tables.print_is_base_red(cout);
    cout << "Moves:\n";
    board_tables.print_moves(cout);
    cout << "Infinity: " << static_cast<uint>(board_tables.infinity()) << "\n";
    cout << "coordZ:\n";
    board_tables.print_coordZ(cout);
    cout << "coordE:\n";
    board_tables.print_coordE(cout);
    cout << "Start position:\n";
    cout << Board{board_tables.startZ()};

    BoardE boarde{board_tables.start()};
    cout << boarde;

    board_tables.startZ().make_moves();
}

int main(int argc, char** argv) {
    try {
        my_main(argc, argv);
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
