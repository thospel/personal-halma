#include <cstdlib>
#include <cstdint>

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <limits>

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

using Norm = uint8_t;
enum Color : uint8_t { EMPTY, BLUE, RED, COLORS };

class Coord;
using Diff = Coord;

class Coord {
  private:
    static int const ROW  = 2*X;
  public:
    static int const SIZE = (Y+1)*ROW+X+2;
    static int const MAX  = (Y-1)*ROW+X-1;

    Coord() {}
    Coord(Coord const& from, Diff const& diff) : pos_{static_cast<int16_t>(from.pos_ + diff.pos_)} {}
    Coord(int x, int y) : pos_{static_cast<int16_t>(y*ROW+x)} { }
    uint x() const { return (pos_+(X+(Y-1)*ROW)) % ROW - X; }
    uint y() const { return (pos_+(X+(Y-1)*ROW)) / ROW - (Y-1); }
    int pos()     const { return pos_; }
    uint index()  const { return OFFSET+ pos_; }
    uint index2() const { return MAX+pos_; }
  private:
    static uint const OFFSET = ROW+1;
    int16_t pos_;
};
inline ostream& operator<<(ostream& os, Coord const& pos) {
    os << setw(3) << static_cast<int>(pos.x()) << " " << setw(3) << static_cast<int>(pos.y());
    return os;
};

// Army as a set of Coord
class Army: public array<Coord, ARMY> {
  public:
    Army() {}
};

inline ostream& operator<<(ostream& os, Army const& army) {
    for (int i=0; i<ARMY; ++i)
        os << army[i] << "\n";
    return os;
}

// Board as two Army
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    void make_moves(bool red_to_move = false) const;
    Army& blue() { return blue_; }
    Army& red()  { return red_; }
    Army const& blue() const { return blue_; }
    Army const& red()  const { return red_; }
  private:
    Army blue_, red_;
};

class Image {
  public:
    inline Image() {
        clear();
    }
    inline explicit Image(Board const& board);
    void print(ostream& os) const;
    inline void clear();
    Color get(Coord const& pos) const { return board_[pos.index()]; }
    Color get(int x, int y) const { return get(Coord{x,y}); }
    void  set(Coord const& pos, Color c) { board_[pos.index()] = c; }
    void  set(int x, int y, Color c) { set(Coord{x,y}, c); }
  private:
    array<Color, Coord::SIZE> board_;
};

Image::Image(Board const& board) : Image{} {
    for (auto const& pos: board.blue())
        set(pos, BLUE);
    for (auto const& pos: board.red())
        set(pos, RED);
}

void Image::print(ostream& os) const {
    os << "+";
    for (int x=0; x < X; ++x) os << "--";
    os << "+\n";

    for (int y=0; y < Y; ++y) {
        os << "|";
        for (int x=0; x < X; ++x) {
            auto c = get(x, y);
            os << (c == EMPTY ? ". " :
                   c == RED   ? "X " :
                   c == BLUE  ? "O " :
                   "? ");
        }
        os << "|\n";
    }

    os << "+";
    for (int x=0; x < X; ++x) os << "--";
    os << "+\n";
}

inline ostream& operator<<(ostream& os, Image const& image) {
    image.print(os);
    return os;
}

using Moves = array<Diff, MOVES>;

class Tables {
  public:
    Tables();
    inline Norm norm (Coord const&left, Coord const&right) const {
        return norm_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance (Coord const& left, Coord const& right) const {
        return distance_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance_base_red(Coord const& pos) const {
        return distance_base_red_[pos.pos()];
    }
    Norm infinity() const { return infinity_; }
    Moves const& moves() const { return moves_; }
    Board const& start() const { return start_; }
    Image const& start_image() const { return start_image_; }
    void print_moves(ostream& os) const;
    void print_moves() const {
        print_moves(cout);
    }
    void print_distance_base_red(ostream& os) const;
    inline void print_distance_base_red() const {
        print_distance_base_red(cout);
    }
  private:
    Norm infinity_;
    array<Norm, 2*Coord::MAX+1> norm_;
    array<Norm, 2*Coord::MAX+1> distance_;
    array<Norm, Coord::MAX+1> distance_base_red_;
    Moves moves_;
    Board start_;
    Image start_image_;
};

Tables::Tables() {
    fill(norm_.begin(), norm_.end(), std::numeric_limits<Norm>::max());
    int move = 0;
    infinity_ = 0;
    for (int y=1-Y; y < Y; ++y) {
        int ay = abs(y);
        for (int x=1-X; x < X; ++x) {
            int ax = abs(x);
            auto diff = Diff{x,y};
            Norm n =
                MOVES == 8 ? max(ax, ay) :
                MOVES == 6 ? (ax+ay+abs(x+y))/2 :
                MOVES == 4 ? ax+ay :
                throw(logic_error("Unknown move " + to_string(MOVES)));
            norm_[diff.index2()] = n;
            distance_[diff.index2()] = n <= 2 ? 0 : n-2;
            infinity_ = max(infinity_, n);
            if (n == 1) {
                if (move >= MOVES) throw(logic_error("too many moves"));
                moves_[move++] = diff;
            }
        }
    }
    if (move < MOVES) throw(logic_error("too few moves"));
    ++infinity_;

    // Fill base
    auto& red  = start_.red();
    auto& blue = start_.blue();
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
            blue[i] = Coord{x, y};
            red[i]  = Coord{X-1-x, Y-1-y};
            ++i;
            --x;
            ++y;
        }
        d++;
    }

    for (int y=0; y < Y; ++y) {
        start_image_.set(-1, y, COLORS);
        start_image_.set( X, y, COLORS);
        Norm d = infinity_;
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            start_image_.set(pos, EMPTY);
            for (int i=0; i<ARMY; ++i) {
                Norm d1 = distance(pos, red[i]);
                if (d1 < d) d = d1;
            }
            distance_base_red_[pos.pos()] = d;
        }
    }
    for (int x=-1; x <= X; ++x) {
        start_image_.set(x, -1, COLORS);
        start_image_.set(x,  Y, COLORS);
    }
}

void Tables::print_moves(ostream& os) const {
    for (int i=0; i<MOVES; ++i)
        os << moves_[i] << "\n";
}

void Tables::print_distance_base_red(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << setw(3) << static_cast<uint>(distance_base_red(pos));
        }
        os << "\n";
    }
}

STATIC Tables const tables;

void Image::clear() {
    board_ = tables.start_image().board_;
}

void Board::make_moves(bool red_to_move) const {
    Army army_blue{blue()};
    Army army_red {red()};
    Color color = red_to_move ? RED : BLUE;
    auto& army  = red_to_move ? army_red : army_blue;
    red_to_move = !red_to_move;

    // Board board{army_blue, army_red};
    Image image{*this};

    for (int a=0; a<ARMY; ++a) {
        auto soldier = army[a];
        image.set(soldier, EMPTY);

        // Jumps
        array<Coord, ARMY*2*MOVES+1> reachable;
        reachable[0] = soldier;
        int nr_reachable = 1;
        if (!CLOSED_LOOP) image.set(soldier, COLORS);
        for (int i=0; i < nr_reachable; ++i) {
            for (auto move: tables.moves()) {
                Coord jumpee{soldier, move};
                if (image.get(jumpee) != RED && image.get(jumpee) != BLUE) continue;
                Coord target{jumpee, move};
                if (image.get(target) != EMPTY) continue;
                image.set(target, COLORS);
                reachable[nr_reachable++] = target;
            }
        }
        for (int i=CLOSED_LOOP; i < nr_reachable; ++i)
            image.set(reachable[i], EMPTY);
        for (int i=1; i < nr_reachable; ++i) {
            // armyZ[a] = CoordZ{reachable[i]};
            image.set(reachable[i], color);
            cout << image << "\n";
            image.set(reachable[i], EMPTY);
        }

        // Step moves
        for (auto move: tables.moves()) {
            Coord target{soldier, move};
            if (image.get(target) != EMPTY) continue;
            // armyZ[a] = CoordZ{target};
            image.set(target, color);
            cout << image << "\n";
            image.set(target, EMPTY);
        }

        image.set(soldier, color);
        army[a] = soldier;
    }
}

void my_main(int argc, char const* const* argv) {
    cout << "Infinity: " << static_cast<uint>(tables.infinity()) << "\n";
    tables.print_moves();
    auto start = tables.start();
    cout << "Red:\n";
    cout << start.red();
    cout << "Blue:\n";
    cout << start.blue();
    cout << "Base red distance:\n";
    tables.print_distance_base_red();

    Image image{start};
    cout << image;
    start.make_moves();
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
