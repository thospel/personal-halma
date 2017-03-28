#include <cstdlib>
#include <cstdint>

#include <algorithm>
#include <array>
#include <iomanip>
#include <iostream>
#include <limits>

#include "xxhash64.h"

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

bool const CHECK = true;

int const X_MAX = 16;
int const Y_MAX = 16;

int const X = 9;
int const Y = 9;
int const MOVES = 6;
int const ARMY = 10;
bool const CLOSED_LOOP = false;
bool const PASS = false;

uint64_t SEED = 123456789;

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
    int x() const { return (pos_+(X+(Y-1)*ROW)) % ROW - X; }
    int y() const { return (pos_+(X+(Y-1)*ROW)) / ROW - (Y-1); }
    int pos()     const { return pos_; }
    uint index()  const { return OFFSET+ pos_; }
    uint index2() const { return MAX+pos_; }
    void check(int line) const {
        int x_ = x();
        int y_ = y();
        if (x_ < 0) throw(logic_error("x negative at line " + to_string(line)));
        if (x_ >= X) throw(logic_error("x too large at line " + to_string(line)));
        if (y_ < 0) throw(logic_error("y negative at line " + to_string(line)));
        if (y_ >= Y) throw(logic_error("y too large at line " + to_string(line)));
    }
  private:
    static uint const OFFSET = ROW+1;
    int16_t pos_;

    friend inline ostream& operator<<(ostream& os, Coord const& pos) {
    os << setw(3) << static_cast<int>(pos.x()) << " " << setw(3) << static_cast<int>(pos.y());
    return os;
}

    friend bool operator<(Coord const& l, Coord const& r) {
        // cout << "operator<(" << l << ", " << r << ") [" << l.pos_ << ", " << r.pos_ << "] = " << (l.pos_ < r.pos_ ? "true" : "false") << "\n";
        return l.pos_ < r.pos_;
    }
    friend bool operator>(Coord const& l, Coord const& r) {
        // cout << "operator>(" << l << ", " << r << ") [" << l.pos_ << ", " << r.pos_ << "] = " << (l.pos_ > r.pos_ ? "true" : "false")  << "\n";
        return l.pos_ > r.pos_;
    }
    friend bool operator>=(Coord const& l, Coord const& r) {
        return l.pos_ >= r.pos_;
    }
    friend bool operator!=(Coord const& l, Coord const& r) {
        return l.pos_ != r.pos_;
    }
};
class ArmyE;
// Army as a set of Coord
class Army: public array<Coord, ARMY> {
  public:
    Army() {}
    uint64_t hash() const {
        return XXHash64::hash(reinterpret_cast<void const*>(this), sizeof(Army), SEED);
    }
    void invalidate() {
        (*this)[0] = Coord{-1, -1};
    }
    bool valid() const {
        return (*this)[0] >= Coord{0, 0};
    }
    inline void check(int line) const;
    inline void copy(ArmyE const army);
    friend bool operator==(Army const& l, Army const& r) {
        for (int i=0; i<ARMY; ++i)
            if (l[i] != r[i]) return false;
        return true;
    }
    void printE(ostream& os) {
        for (int i=-1; i<=ARMY; ++i)
            os << (*this)[i] << "\n";
    }
    void printE() {
        printE(cout);
    }
};

// Army as a set of Coord
class ArmyE: public array<Coord, ARMY+2> {
  public:
    ArmyE() {
        at(-1)   = Coord{-1,-1};
        at(ARMY) = Coord{ X, Y};
    }
    // Coord operator[](ssize_t) = delete;
    Coord& at(int i) { return (*this)[i+1]; }
    Coord const& at(int i) const { return (*this)[i+1]; }
    void copy(Army const& army) {
        for (int i=0; i<ARMY; ++i)
            at(i) = army[i];
    }
};

void Army::copy(ArmyE const army) {
    for (int i=0; i<ARMY; ++i)
        (*this)[i] = army.at(i);
}

inline ostream& operator<<(ostream& os, Army const& army) {
    for (int i=0; i<ARMY; ++i)
        os << army[i] << "\n";
    return os;
}

inline ostream& operator<<(ostream& os, ArmyE const& army) {
    for (int i=-1; i<=ARMY; ++i)
        os << army.at(i) << "\n";
    return os;
}

void Army::check(int line) const {
    for (int i=0; i<ARMY; ++i) (*this)[i].check(line);
    for (int i=0; i<ARMY-1; ++i)
        if ((*this)[i] >= (*this)[i+1]) {
            cerr << *this;
            throw(logic_error("Army out of order at line " + to_string(line)));
        }
}

class BoardSet;
// Board as two Armies
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    void make_moves(BoardSet& set, bool red_to_move = false) const;
    Army& blue() { return blue_; }
    Army& red()  { return red_; }
    Army const& blue() const { return blue_; }
    Army const& red()  const { return red_; }
    uint64_t hash() const {
        return blue_.hash() ^ red_.hash();
    }
    void invalidate() {
        blue_.invalidate();
    }
    bool valid() const {
        return blue_.valid();
    }
    void check(int line) const {
        blue_.check(line);
        red_.check(line);
    }
  private:
    Army blue_, red_;

    friend bool operator==(Board const& l, Board const& r) {
        return l.blue() == r.blue() && l.red() == r.red();
    }
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
    sort(moves_.begin(), moves_.end());
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
    sort(blue.begin(), blue.end());
    sort(red.begin(),  red.end());

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

class BoardSet {
  public:
    BoardSet(uint64_t size = 2);
    ~BoardSet();
    uint64_t size() const {
        return limit_ - left_;
    }
    uint64_t max_size() const {
        return size_;
    }
    Board* insert(Board const& board, uint64_t hash, bool is_new = false);
    Board* insert(Board const& board, bool is_new = false) {
        return insert(board, board.hash(), is_new);
    }
    bool find(Board const& board, uint64_t hash) const;
    bool find(Board const& board) {
        return find(board, board.hash());
    }
    Board const* begin() const { return &boards_[0]; }
    Board const* end()   const { return &boards_[size_]; }
  private:
    static uint64_t constexpr FACTOR(uint64_t factor=1) { return static_cast<uint64_t>(0.7*factor); }
    void resize();

    uint64_t size_;
    uint64_t mask_;
    uint64_t left_;
    uint64_t limit_;
    Board* boards_;
};

BoardSet::BoardSet(uint64_t size) : size_{size}, mask_{size-1}, left_{FACTOR(size)}, limit_{FACTOR(size)} {
    boards_ = new Board[size];
    for (uint64_t i=0; i<size; ++i) boards_[i].invalidate();
}

BoardSet::~BoardSet() {
    delete [] boards_;
}

Board* BoardSet::insert(Board const& board, uint64_t hash, bool is_new) {
    // cout << "Insert\n";
    if (left_ == 0) resize();
    uint64_t pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Board& b = boards_[pos];
        if (!b.valid()) {
            b = board;
            --left_;
            // cout << "Found empty\n";
            return &b;
        }
        if (!is_new && b == board) {
            // cout << "Found duplicate " << hash << "\n";
            return nullptr;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

bool BoardSet::find(Board const& board, uint64_t hash) const {
    uint64_t pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Board& b = boards_[pos];
        if (!b.valid()) return false;
        if (b == board) return true;
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

void BoardSet::resize() {
    auto old_boards = boards_;
    auto old_size = size_;
    boards_ = new Board[2*size_];
    size_ *= 2;
    cout << "Resize: " << size_ << "\n";
    mask_ = size_-1;
    left_ = limit_ = FACTOR(size_);
    for (uint64_t i = 0; i < size_; ++i) boards_[i].invalidate();
    for (uint64_t i = 0; i < old_size; ++i) {
        if (!old_boards[i].valid()) continue;
        insert(old_boards[i], true);
    }
    delete [] old_boards;
}

void Image::clear() {
    board_ = tables.start_image().board_;
}

void Board::make_moves(BoardSet& set, bool red_to_move) const {
    Color color = red_to_move ? RED : BLUE;
    auto& army  = red_to_move ? red() : blue();
    auto& opponent_army  = red_to_move ? blue() : red();
    Board board;
    auto& board_army = red_to_move ? board.red() : board.blue();
    if (red_to_move)
        board.blue() = opponent_army;
    else
        board.red() = opponent_army;
    auto opponent_hash = opponent_army.hash();

    Image image{*this};
    ArmyE armyE;

    for (int a=0; a<ARMY; ++a) {
        armyE.copy(army);
        int pos = a;

        auto soldier = army[a];
        image.set(soldier, EMPTY);

        // Jumps
        array<Coord, ARMY*2*MOVES+(1+MOVES)> reachable;
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

        // Step moves
        for (auto move: tables.moves()) {
            Coord target{soldier, move};
            if (image.get(target) != EMPTY) continue;
            reachable[nr_reachable++] = target;
        }

        for (int i=1; i < nr_reachable; ++i) {
            // armyZ[a] = CoordZ{reachable[i]};
            if (false) {
                image.set(reachable[i], color);
                cout << image;
                image.set(reachable[i], EMPTY);
            }
            auto val = reachable[i];
            if (val > armyE.at(pos+1)) {
                do {
                    armyE.at(pos) = armyE.at(pos+1);
                    // cout << "Set pos > " << pos << armyE.at(pos) << "\n";
                    ++pos;
                } while (val > armyE.at(pos+1));
            } else if (val < armyE.at(pos-1)) {
                do {
                    armyE.at(pos) = armyE.at(pos-1);
                    // cout << "Set pos < " << pos << armyE.at(pos) << "\n";
                    --pos;
                } while (val < armyE.at(pos-1));
            }
            // if (pos < 0) throw(logic_error("Negative pos"));
            if (pos < 0) abort();
            if (pos >= ARMY) throw(logic_error("Excessive pos"));
            armyE.at(pos) = val;
            // cout << "Final Set pos " << pos << armyE[pos] << "\n";
            // cout << armyE << "----------------\n";
            board_army.copy(armyE);
            if (CHECK) board.check(__LINE__);
            auto hash = board_army.hash() ^ opponent_hash;
            // cout << "Hash: " << hash << "\n";
            if (set.insert(board, hash))
                cout << Image{board};
        }

        image.set(soldier, color);
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

    BoardSet set[4];
    set[0].insert(start);
    cout << "Set 0 done\n";
    bool red_to_move = false;
    for (uint i=1; i<sizeof(set)/sizeof(*set); ++i) {
        auto& from_set = set[i-1];
        auto& to_set   = set[i];
        for (auto& board: from_set) {
            if (!board.valid()) continue;
            if (CHECK) board.check(__LINE__);
            // cout << "From:\n" << Image{board};
            board.make_moves(to_set, red_to_move);
        }
        red_to_move = !red_to_move;
        cout << "Set " << i << "  done\n";
    }
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
