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

bool const CHECK = false;
bool const VERBOSE = false;
bool const SLIDES = false;

int const X_MAX = 16;
int const Y_MAX = 16;

int const X = 9;
int const Y = 9;
int const MOVES = 6;
int const ARMY = 10;
int const NR_MOVES = 30;
bool const CLOSED_LOOP = false;
bool const PASS = false;

uint64_t SEED = 123456789;

using Norm = uint8_t;
using Nbits = uint;
int const NBITS = std::numeric_limits<Nbits>::digits;
Nbits const NLEFT = static_cast<Nbits>(1) << (NBITS-1);
enum Color : uint8_t { EMPTY, BLUE, RED, COLORS };
Color operator+(Color from, int value) {
    return static_cast<Color>(static_cast<int>(from) + value);
}
Color operator-(Color from, int value) {
    return static_cast<Color>(static_cast<int>(from) - value);
}

class Coord;
using Diff = Coord;

using CoordVal = int16_t;
class Coord {
  private:
    static int const ROW  = 2*X;
  public:
    static int const SIZE = (Y+1)*ROW+X+2;
    static int const MAX  = (Y-1)*ROW+X-1;

    Coord() {}
    Coord(Coord const& from, Diff const& diff) : pos_{static_cast<CoordVal>(from.pos_ + diff.pos_)} {}
    Coord(int x, int y) : pos_{static_cast<CoordVal>(y*ROW+x)} { }
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
    Coord mirror() const {
        Coord result;
        result.pos_ = MAX - pos_;
        return result;
    }
    static Coord const INVALID;
  private:
    static uint const OFFSET = ROW+1;
    CoordVal pos_;

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
Coord const Coord::INVALID{-1, -1};
struct Move {
    Coord from, to;
    Move mirror() const {
        return Move{from.mirror(), to.mirror()};
    }
};

class ArmyE;
// Army as a set of Coord
class Army: public array<Coord, ARMY> {
  public:
    Army() {}
    uint64_t hash() const {
        return XXHash64::hash(reinterpret_cast<void const*>(&(*this)[0]), sizeof(Coord) * ARMY, SEED);
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
    uint64_t hash() const {
        return XXHash64::hash(reinterpret_cast<void const*>(&at(0)), sizeof(Coord) * ARMY, SEED);
    }
    void copy(Army const& army) {
        for (int i=0; i<ARMY; ++i)
            at(i) = army[i];
    }
    inline void check(int line) const;
};

bool operator==(ArmyE const& l, Army const& r) {
    for (int i=0; i<ARMY; ++i)
        if (l.at(i) != r[i]) return false;
    return true;
}

bool operator==(Army const& l, ArmyE const& r) {
    for (int i=0; i<ARMY; ++i)
        if (l[i] != r.at(i)) return false;
    return true;
}

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

void ArmyE::check(int line) const {
    for (int i=0; i<ARMY; ++i) at(i).check(line);
    for (int i=-1; i<ARMY; ++i)
        if (at(i) >= at(i+1)) {
            cerr << *this;
            throw(logic_error("Army out of order at line " + to_string(line)));
        }
}

class ArmySet {
  public:
    using Index = uint32_t;

    ArmySet(Index size = 2);
    ~ArmySet();
    void clear(Index size = 2);
    Index size() const {
        return used1_ - 1;
    }
    Index max_size() const {
        return size_;
    }
    Index capacity() const {
        return limit_;
    }
    Army const& at(Index i) const { return armies_[i]; }
    Index insert(Army const& value, Index hash, bool is_new = false);
    Index insert(Army const& value, bool is_new = false) {
        return insert(value, value.hash(), is_new);
    }
    Index insert(ArmyE const& value, Index hash, bool is_new = false);
    Index insert(ArmyE const& value, bool is_new = false) {
        return insert(value, value.hash(), is_new);
    }
    Index find(Army const& value, Index hash) const;
    Index find(Army const& value) const {
        return find(value, value.hash());
    }
    Army const* begin() const { return &armies_[1]; }
    Army const* end()   const { return &armies_[used1_]; }
  private:
    static Index constexpr FACTOR(Index factor=1) { return static_cast<Index>(0.7*factor); }
    void resize();

    Index size_;
    Index mask_;
    Index used1_;
    Index limit_;
    Index* values_;
    Army* armies_;
};

// Board as two Armies
class BoardSet;
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    void move(Move const& move_);
    void move(Move const& move_, bool blue_to_move);
    Army& blue() { return blue_; }
    Army& red()  { return red_; }
    Army const& blue() const { return blue_; }
    Army const& red()  const { return red_; }
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

class BoardSet {
  public:
    using Value = uint64_t;
    using Index = uint64_t;

    static void split(Value value, ArmySet::Index& moving, ArmySet::Index& opponent) {
        moving   = value & std::numeric_limits<ArmySet::Index>::max();
        opponent = value >> std::numeric_limits<ArmySet::Index>::digits;
    }
    BoardSet(Index size = 2);
    ~BoardSet();
    void clear(Index size = 2);
    Index size() const {
        return limit_ - left_;
    }
    Index max_size() const {
        return size_;
    }
    Index capacity() const {
        return limit_;
    }
    bool insert(Value value, bool is_new = false);
    bool insert(ArmySet::Index to_move, ArmySet::Index opponent) {
        if (CHECK) {
            if (to_move == 0) throw(logic_error("to_move == 0"));
            if (opponent == 0) throw(logic_error("opponent == 0"));
        }
        Value value = static_cast<Value>(opponent) << std::numeric_limits<ArmySet::Index>::digits | to_move;
        return insert(value);
    }
    bool insert(Board const& board, ArmySet& army, ArmySet& opponent, int nr_moves);
    bool find(Value value) const;
    bool find(ArmySet::Index to_move, ArmySet::Index opponent) const {
        Value value = static_cast<Value>(opponent) << std::numeric_limits<ArmySet::Index>::digits | to_move;
        return find(value);
    }
    bool find(Board const& board, ArmySet const& army, ArmySet const& opponent, int nr_moves) const;
    Value const* begin() const { return &values_[0]; }
    Value const* end()   const { return &values_[size_]; }
  private:
    static Index constexpr FACTOR(Index factor=1) { return static_cast<Index>(0.7*factor); }
    void resize();

    Index size_;
    Index mask_;
    Index left_;
    Index limit_;
    Value* values_;
};

class Image {
  public:
    inline Image() {
        clear();
    }
    inline Image(Army const& blue, Army const& red);
    inline Image(ArmyE const& blue, Army const& red);
    inline Image(Army const& blue, ArmyE const& red);
    inline explicit Image(Board const& board): Image{board.blue(), board.red()} {}
    void print(ostream& os) const;
    inline void clear();
    Color get(Coord const& pos) const { return board_[pos.index()]; }
    Color get(int x, int y) const { return get(Coord{x,y}); }
    void  set(Coord const& pos, Color c) { board_[pos.index()] = c; }
    void  set(int x, int y, Color c) { set(Coord{x,y}, c); }
  private:
    array<Color, Coord::SIZE> board_;
};

Image::Image(Army const& blue, Army const& red) : Image{} {
    for (auto const& pos: blue)
        set(pos, BLUE);
    for (auto const& pos: red)
        set(pos, RED);
}

Image::Image(ArmyE const& blue, Army const& red) : Image{} {
    for (int i=0; i<ARMY; ++i)
        set(blue.at(i), BLUE);
    for (auto const& pos: red)
        set(pos, RED);
}

Image::Image(Army const& blue, ArmyE const& red) : Image{} {
    for (auto const& pos: blue)
        set(pos, BLUE);
    for (int i=0; i<ARMY; ++i)
        set(red.at(i), RED);
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

inline ostream& operator<<(ostream& os, Board const& board) {
    Image{board}.print(os);
    return os;
}

using Moves = array<Diff, MOVES>;
using TypeCount = array<int, 4>;
class Tables {
  public:
    Tables();
    inline Norm norm(Coord const&left, Coord const&right) const {
        return norm_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance(Coord const& left, Coord const& right) const {
        return distance_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance_base_red(Coord const& pos) const {
        return distance_base_red_[pos.pos()];
    }
    inline Nbits Ndistance(Coord const& left, Coord const& right) const {
        return NLEFT >> distance(left, right);
    }
    inline Nbits Ndistance_base_red(Coord const& pos) const {
        return NLEFT >> distance_base_red(pos);
    }
    inline uint8_t base_red(Coord const& pos) const {
        return base_red_[pos.pos()];
    }
    inline uint8_t edge_red(Coord const& pos) const {
        return edge_red_[pos.pos()];
    }
    inline uint8_t type(Coord const& pos) const {
        return type_[pos.pos()];
    }
    inline TypeCount const& type_count() const {
        return type_count_;
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
    void print_base_red(ostream& os) const;
    void print_base_red() const {
        print_base_red(cout);
    }
    void print_edge_red(ostream& os) const;
    void print_edge_red() const {
        print_edge_red(cout);
    }
    void print_type(ostream& os) const;
    void print_type() const {
        print_type(cout);
    }
    void print_type_count(ostream& os) const;
    void print_type_count() const {
        print_type_count(cout);
    }
  private:
    TypeCount type_count_;
    Moves moves_;
    Norm infinity_;
    array<Norm, 2*Coord::MAX+1> norm_;
    array<Norm, 2*Coord::MAX+1> distance_;
    array<Norm, Coord::MAX+1> distance_base_red_;
    array<uint8_t, Coord::MAX+1> base_red_;
    array<uint8_t, Coord::MAX+1> edge_red_;
    array<uint8_t, Coord::MAX+1> type_;
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
    if (infinity_ >= NBITS)
        throw(logic_error("Max distance does not fit in Nbits"));
    ++infinity_;

    // Fill base
    fill(base_red_.begin(), base_red_.end(), 0);
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
            base_red_[red[i].pos()] = 1;
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
        uint8_t y_type = y%2*2;
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            start_image_.set(pos, EMPTY);
            for (int i=0; i<ARMY; ++i) {
                Norm d1 = norm(pos, red[i]);
                if (d1 < d) d = d1;
            }
            distance_base_red_[pos.pos()] = d > 2 ? d-2 : 0;
            edge_red_[pos.pos()] = d == 1;
            type_[pos.pos()] = y_type + x % 2;
        }
    }
    for (int x=-1; x <= X; ++x) {
        start_image_.set(x, -1, COLORS);
        start_image_.set(x,  Y, COLORS);
    }

    fill(type_count_.begin(), type_count_.end(), 0);
    for (auto const& r: red)
        ++type_count_[type(r)];
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

void Tables::print_base_red(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(base_red(pos));
        }
        os << "\n";
    }
}

void Tables::print_edge_red(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(edge_red(pos));
        }
        os << "\n";
    }
}

void Tables::print_type(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(type(pos));
        }
        os << "\n";
    }
}

void Tables::print_type_count(ostream& os) const {
    for (auto c: type_count_)
        os << " " << c;
    os << "\n";
}

STATIC Tables const tables;

ArmySet::ArmySet(Index size) : size_{size}, mask_{size-1}, used1_{1}, limit_{FACTOR(size)} {
    values_ = new Index[size];
    for (Index i=0; i<size; ++i) values_[i] = 0;
    armies_ = new Army[limit_+1];
}

ArmySet::~ArmySet() {
    delete [] armies_;
    delete [] values_;
}

void ArmySet::clear(Index size) {
    auto new_values = new Index[size];
    Index new_limit = FACTOR(size);
    auto new_armies = new Army[new_limit+1];
    delete [] values_;
    values_ = new_values;
    delete [] armies_;
    armies_ = new_armies;
    for (Index i=0; i<size; ++i) values_[i] = 0;
    size_ = size;
    mask_ = size-1;
    limit_ = new_limit;
    used1_ = 1;
}

ArmySet::Index ArmySet::insert(Army const& value, Index hash, bool is_new) {
    // cout << "Insert\n";
    if (used1_ > limit_) resize();
    Index pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Index i = values_[pos];
        if (i == 0) {
            values_[pos] = used1_;
            auto& v = armies_[used1_];
            v = value;
            // cout << "Found empty\n";
            return used1_++;
        }
        auto& v = armies_[i];
        if (!is_new && v == value) {
            // cout << "Found duplicate " << hash << "\n";
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

ArmySet::Index ArmySet::insert(ArmyE const& value, Index hash, bool is_new) {
    // cout << "Insert, used1=" << used1_ << "\n";
    if (used1_ > limit_) resize();
    Index pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Index i = values_[pos];
        if (i == 0) {
            values_[pos] = used1_;
            auto& v = armies_[used1_];
            v.copy(value);
            // cout << "Found empty\n";
            return used1_++;
        }
        auto& v = armies_[i];
        if (!is_new && v == value) {
            // cout << "Found duplicate " << hash << "\n";
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

ArmySet::Index ArmySet::find(Army const& army, Index hash) const {
    Index pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Index i = values_[pos];
        if (i == 0) return 0;
        Army& v = armies_[i];
        if (v == army) return i;
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

void ArmySet::resize() {
    auto old_values = values_;
    auto old_armies = armies_;
    auto old_used1  = used1_;
    size_ *= 2;
    // cout << "Resize: " << size_ << "\n";
    values_ = new Index[size_];
    limit_ = FACTOR(size_);
    armies_ = new Army[limit_+1];
    delete [] old_values;
    mask_ = size_-1;
    used1_ = 1;
    for (Index i = 0; i < size_; ++i) values_[i] = 0;
    for (Index i = 1; i < old_used1; ++i)
        insert(old_armies[i], true);
    delete [] old_armies;
}

BoardSet::BoardSet(Index size) : size_{size}, mask_{size-1}, left_{FACTOR(size)}, limit_{FACTOR(size)} {
    values_ = new Value[size];
    for (Index i=0; i<size; ++i) values_[i] = 0;
}

BoardSet::~BoardSet() {
    delete [] values_;
}

void BoardSet::clear(Index size) {
    auto old_values = values_;
    values_ = new Value[size];
    delete [] old_values;
    for (Index i=0; i<size; ++i) values_[i] = 0;
    size_ = size;
    mask_ = size-1;
    limit_ = left_ = FACTOR(size);
}

uint64_t const murmur_multiplier = UINT64_C(0xc6a4a7935bd1e995);

uint64_t murmur_mix(uint64_t v) {
    v *= murmur_multiplier;
    return v ^ (v >> 47);
}

bool BoardSet::insert(Value value, bool is_new) {
    // cout << "Insert\n";
    if (left_ == 0) resize();
    Index pos = murmur_mix(value) & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Value& v = values_[pos];
        if (v == 0) {
            v = value;
            --left_;
            // cout << "Found empty\n";
            return true;
        }
        if (!is_new && v == value) {
            // cout << "Found duplicate " << hash << "\n";
            return false;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

bool BoardSet::insert(Board const& board, ArmySet& army_set, ArmySet& opponent_set, int nr_moves) {
    uint8_t blue_to_move = nr_moves & 1;
    Army const& army = blue_to_move ? board.blue() : board.red();
    Army const& opponent_army = blue_to_move ? board.red() : board.blue();
    auto moving_index = army_set.insert(army);
    auto opponent_index = opponent_set.insert(opponent_army);
    return insert(moving_index, opponent_index);
}

bool BoardSet::find(Value value) const {
    Index pos = murmur_mix(value) & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        Value& v = values_[pos];
        if (v == 0) return false;
        if (v == value) {
            // cout << "Found duplicate " << hash << "\n";
            return true;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

bool BoardSet::find(Board const& board, ArmySet const& army_set, ArmySet const& opponent_set, int nr_moves) const {
    uint8_t blue_to_move = nr_moves & 1;

    Army const& army = blue_to_move ? board.blue() : board.red();
    auto moving_index = army_set.find(army);
    if (moving_index == 0) return false;

    Army const& opponent_army = blue_to_move ? board.red() : board.blue();
    auto opponent_index = opponent_set.find(opponent_army);
    if (opponent_index == 0) return false;

    return find(moving_index, opponent_index);
}

void BoardSet::resize() {
    auto old_values = values_;
    auto old_size = size_;
    values_ = new Value[2*size_];
    size_ *= 2;
    // cout << "Resize: " << size_ << "\n";
    mask_ = size_-1;
    left_ = limit_ = FACTOR(size_);
    for (Index i = 0; i < size_; ++i) values_[i] = 0;
    for (Index i = 0; i < old_size; ++i) {
        if (old_values[i] == 0) continue;
        insert(old_values[i], true);
    }
    delete [] old_values;
}

void Image::clear() {
    board_ = tables.start_image().board_;
}

uint make_moves(Army const& army, Army const& opponent_army, ArmySet::Index opponent_index, BoardSet& board_set, ArmySet& army_set, int available_moves) {

    uint8_t blue_to_move = available_moves & 1;
    Army const& blue = blue_to_move ? army : opponent_army;
    Army const& red  = blue_to_move ? opponent_army : army;
    Image image{blue, red};
    if (VERBOSE) cout << "From: " << available_moves << "\n" << image;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = NLEFT >> tables.infinity();
    int off_base_from = 0;
    TypeCount type_count_from = tables.type_count();
    int edge_count_from = 0;
    for (auto const& b: blue) {
        --type_count_from[tables.type(b)];
        if (tables.base_red(b)) continue;
        ++off_base_from;
        edge_count_from += tables.edge_red(b);
        Ndistance_red |= tables.Ndistance_base_red(b);
        for (auto const& r: red)
            Ndistance_army |= tables.Ndistance(r, b);
    }
    int slides = 0;
    for (auto tc: type_count_from)
        slides += max(tc, 0);
    int distance_army = __builtin_clz(Ndistance_army);
    int distance_red  = __builtin_clz(Ndistance_red);

    if (VERBOSE) {
        cout << "Slides >= " << slides << ", red edge count " << edge_count_from << "\n";
        cout << "Distance army=" << distance_army << "\n";
        cout << "Distance red =" << distance_red  << "\n";
        cout << "Off base=" << off_base_from << "\n";
    }
    int pre_moves = min((distance_army + blue_to_move) / 2, distance_red);
    int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
    int needed_moves = 2*blue_moves - blue_to_move;
    if (VERBOSE)
        cout << "Needed moves=" << static_cast<int>(needed_moves) << "\n";
    if (needed_moves > available_moves) {
        if (VERBOSE)
            cout << "Late prune " << needed_moves << " > " << available_moves << "\n";
        return 1;
    }
    --available_moves;

    ArmyE armyE;
    auto off_base   = off_base_from;
    auto type_count = type_count_from;
    auto edge_count = edge_count_from;

    for (int a=0; a<ARMY; ++a) {
        armyE.copy(army);
        int pos = a;

        auto const soldier = army[a];
        image.set(soldier, EMPTY);
        if (blue_to_move) {
            off_base = off_base_from;
            off_base += tables.base_red(soldier);
            type_count = type_count_from;
            ++type_count[tables.type(soldier)];
            edge_count = edge_count_from;
            edge_count -= tables.edge_red(soldier);
        }

        // Jumps
        array<Coord, ARMY*2*MOVES+(1+MOVES)> reachable;
        reachable[0] = soldier;
        int nr_reachable = 1;
        if (!CLOSED_LOOP) image.set(soldier, COLORS);
        for (int i=0; i < nr_reachable; ++i) {
            for (auto move: tables.moves()) {
                Coord jumpee{reachable[i], move};
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
                image.set(reachable[i], RED - blue_to_move);
                cout << image;
                image.set(reachable[i], EMPTY);
            }
            auto val = reachable[i];

            Nbits Ndistance_a = Ndistance_army;
            if (blue_to_move) {
                Nbits Ndistance_r = Ndistance_red;
                auto off    = off_base;
                auto type_c = type_count;
                auto edge_c = edge_count;

                --type_c[tables.type(val)];
                slides = 0;
                for (auto tc: type_c)
                    slides += max(tc, 0);
                if (tables.base_red(val)) {
                    --off;
                    if (off == 0) throw(logic_error("Solution!"));
                } else {
                    edge_c += tables.edge_red(val);
                    Ndistance_r |=  tables.Ndistance_base_red(val);
                    for (auto const& r: red)
                        Ndistance_a |= tables.Ndistance(val, r);
                }
                int distance_red  = __builtin_clz(Ndistance_red);
                int distance_army = __builtin_clz(Ndistance_a);
                int pre_moves = min(distance_army / 2, distance_red);
                int blue_moves = pre_moves + max(slides-pre_moves-edge_c, 0) + off;
                needed_moves = 2*blue_moves;
            } else {
                // We won't notice an increase in army distance, but these
                // are rare and will be discovered in the late prune
                for (auto const& b: blue) {
                    if (tables.base_red(b)) continue;
                    Ndistance_a |= tables.Ndistance(val, b);
                }
                int distance_army = __builtin_clz(Ndistance_a);
                int pre_moves = min((distance_army + 1) / 2, distance_red);
                int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
                needed_moves = 2*blue_moves - 1;
            }
            if (needed_moves > available_moves) {
                if (VERBOSE) {
                    cout << "Move " << soldier << " to " << val << "\n";
                    cout << "Prune " << needed_moves << " > " << available_moves << "\n";
                }
                continue;
            }

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
            if (CHECK) armyE.check(__LINE__);
            auto moved_index = army_set.insert(armyE);
            if (CHECK && moved_index == 0)
                throw(logic_error("Army Insert returns 0"));
            if (board_set.insert(opponent_index, moved_index)) {
                if (VERBOSE) {
                    if (blue_to_move)
                        cout << Image{armyE, opponent_army};
                    else
                        cout << Image{opponent_army, armyE};
                }
            }
        }

        image.set(soldier, RED - blue_to_move);
    }
    return 0;
}

void Board::move(Move const& move_) {
    auto pos = equal_range(blue().begin(), blue().end(), move_.from);
    if (pos.first != pos.second) {
        *pos.first = move_.to;
        sort(blue().begin(), blue().end());
        return;
    }
    pos = equal_range(red().begin(), red().end(), move_.from);
    if (pos.first != pos.second) {
        *pos.first = move_.to;
        sort(red().begin(), red().end());
        return;
    }
    throw(logic_error("Move not found"));
}

void Board::move(Move const& move_, bool blue_to_move) {
    auto& army = blue_to_move ? blue() : red();
    auto pos = equal_range(army.begin(), army.end(), move_.from);
    if (pos.first == pos.second)
        throw(logic_error("Move not found"));
    *pos.first = move_.to;
    sort(army.begin(), army.end());
}

Move const game30[] = {
    {{2,1}, {3,1}},
    {{7,7}, {7,5}},
    {{3,0}, {3,2}},
    {{8,5}, {6,5}},
    {{0,2}, {4,2}},
    {{6,8}, {6,4}},
    {{0,1}, {4,3}},
    {{7,6}, {5,6}},
    {{4,3}, {5,3}},
    {{8,8}, {4,6}},
    {{3,2}, {2,3}},
    {{6,5}, {0,1}},
    {{0,3}, {4,5}},
    {{8,7}, {0,3}},
    {{0,0}, {2,4}},
    {{5,8}, {3,0}},
    {{2,4}, {3,4}},
    {{4,6}, {0,0}},
    {{1,1}, {7,7}},
    {{8,6}, {0,2}},
    {{2,0}, {6,6}},
    {{7,5}, {1,1}},
    {{1,2}, {1,3}},
    {{6,7}, {2,1}},
    {{1,0}, {5,4}},
    {{6,4}, {2,0}},
    {{5,4}, {5,5}},
    {{5,6}, {1,0}},
    {{4,5}, {8,7}},
    {{7,8}, {1,2}},
};

void play() {
    auto board = tables.start();

    int nr_moves = 30;
    for (auto& move: game30) {
        cout << board;

        BoardSet board_set[2];
        ArmySet  army_set[3];
        board_set[0].insert(board, army_set[0], army_set[1], nr_moves);

        for (auto& board_value: board_set[0]) {
            if (!board_value) continue;
            ArmySet::Index moving_index, opponent_index;
            BoardSet::split(board_value, moving_index, opponent_index);
            auto const& army          = army_set[0].at(moving_index);
            auto const& opponent_army = army_set[1].at(opponent_index);
            if (CHECK) {
                army.check(__LINE__);
                opponent_army.check(__LINE__);
            }
            make_moves(army, opponent_army, opponent_index, board_set[1], army_set[2], nr_moves);
        }

        cout << "===============================\n";
        --nr_moves;
        board.move(move.mirror());
        if (board_set[1].find(board, army_set[1], army_set[2], nr_moves)) {
            cout << "Good\n";
        } else {
            cout << "Bad\n";
        }
        // cout << board;
    }
}

void my_main(int argc, char const* const* argv) {
    int nr_moves = NR_MOVES;
    if (argc > 1) {
        nr_moves = atoi(argv[1]);
        if (nr_moves <= 0)
            throw(range_error("Number of moves must be positive"));
    }
        
    auto start = tables.start();
    if (false) {
        cout << "Infinity: " << static_cast<uint>(tables.infinity()) << "\n";
        tables.print_moves();
        cout << "Red:\n";
        cout << start.red();
        cout << "Blue:\n";
        cout << start.blue();
        cout << "Base red distance:\n";
        tables.print_distance_base_red();
        cout << "Base red:\n";
        tables.print_base_red();
        cout << "Edge red:\n";
        tables.print_edge_red();
        cout << "Type:\n";
        tables.print_type();
        cout << "Red Base type count:\n";
        tables.print_type_count();
    }

    if (false) {
        play();
        return;
    }

    BoardSet board_set[2];
    ArmySet  army_set[3];
    board_set[0].insert(start, army_set[0], army_set[1], nr_moves);
    cout << "Starting set done\n";
    for (int i=0; nr_moves>0; --nr_moves, ++i) {
        auto& from_board_set = board_set[ i    % 2];
        auto& to_board_set   = board_set[(i+1) % 2];
        to_board_set.clear();
        auto& moving_army_set   = army_set[ i    % 3];
        auto& opponent_army_set = army_set[(i+1) % 3];
        auto& moved_army_set    = army_set[(i+2) % 3];
        moved_army_set.clear();
        uint64_t late = 0;
        for (auto& board_value: from_board_set) {
            if (board_value == 0) continue;
            ArmySet::Index moving_index, opponent_index;
            BoardSet::split(board_value, moving_index, opponent_index);
            // cout << "Value=" << hex << board_value << ", moving_index=" << moving_index << ", opponent_index=" << opponent_index << dec << "\n";
            auto const& army          = moving_army_set.  at(moving_index);
            auto const& opponent_army = opponent_army_set.at(opponent_index);
            if (CHECK) {
                army.check(__LINE__);
                opponent_army.check(__LINE__);
            }
            late += make_moves(army, opponent_army, opponent_index, to_board_set, moved_army_set, nr_moves);
        }
        cout << "Set " << nr_moves << " done, " << to_board_set.size() << " boards, " << moved_army_set.size() << " armies, late prune=" << late << endl;
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
