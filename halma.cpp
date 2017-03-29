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
bool const CLOSED_LOOP = false;
bool const PASS = false;

uint64_t SEED = 123456789;

using Norm = uint8_t;
inline Norm mask(Norm value) {
    return value & (static_cast<Norm>(-1) << 1);
}
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
    Coord mirror() const {
        Coord result;
        result.pos_ = MAX - pos_;
        return result;
    }
    static Coord const INVALID;
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
        return XXHash64::hash(reinterpret_cast<void const*>(this), sizeof(Army), SEED);
    }
    void invalidate() {
        (*this)[0] = Coord::INVALID;
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

template <class T>
class Set {
  public:
    Set(uint64_t size = 2);
    ~Set();
    void clear(uint64_t size = 2);
    uint64_t size() const {
        return limit_ - left_;
    }
    uint64_t max_size() const {
        return size_;
    }
    T* insert(T const& value, uint64_t hash, bool is_new = false);
    T* insert(T const& value, bool is_new = false) {
        return insert(value, value.hash(), is_new);
    }
    bool find(T const& value, uint64_t hash) const;
    bool find(T const& value) {
        return find(value, value.hash());
    }
    T const* begin() const { return &values_[0]; }
    T const* end()   const { return &values_[size_]; }
  private:
    static uint64_t constexpr FACTOR(uint64_t factor=1) { return static_cast<uint64_t>(0.7*factor); }
    void resize();

    uint64_t size_;
    uint64_t mask_;
    uint64_t left_;
    uint64_t limit_;
    T* values_;
};

class Board;
using BoardSet = Set<Board>;
// Board as two Armies
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    uint make_moves(BoardSet& set, int available_moves) const;
    void move(Move const& move_);
    void move(Move const& move_, bool blue_to_move);
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

template <class T>
Set<T>::Set(uint64_t size) : size_{size}, mask_{size-1}, left_{FACTOR(size)}, limit_{FACTOR(size)} {
    values_ = new T[size];
    for (uint64_t i=0; i<size; ++i) values_[i].invalidate();
}

template <class T>
Set<T>::~Set() {
    delete [] values_;
}

template <class T>
void Set<T>::clear(uint64_t size) {
    auto old_values = values_;
    values_ = new Board[size];
    delete [] old_values;
    for (uint64_t i=0; i<size; ++i) values_[i].invalidate();
    size_ = size;
    mask_ = size-1;
    limit_ = left_ = FACTOR(size);
}

template <class T>
T* Set<T>::insert(T const& value, uint64_t hash, bool is_new) {
    // cout << "Insert\n";
    if (left_ == 0) resize();
    uint64_t pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        T& v = values_[pos];
        if (!v.valid()) {
            v = value;
            --left_;
            // cout << "Found empty\n";
            return &v;
        }
        if (!is_new && v == value) {
            // cout << "Found duplicate " << hash << "\n";
            return nullptr;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

template <class T>
bool Set<T>::find(T const& board, uint64_t hash) const {
    uint64_t pos = hash & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        T& b = values_[pos];
        if (!b.valid()) return false;
        if (b == board) return true;
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

template <class T>
void Set<T>::resize() {
    auto old_values = values_;
    auto old_size = size_;
    values_ = new T[2*size_];
    size_ *= 2;
    // cout << "Resize: " << size_ << "\n";
    mask_ = size_-1;
    left_ = limit_ = FACTOR(size_);
    for (uint64_t i = 0; i < size_; ++i) values_[i].invalidate();
    for (uint64_t i = 0; i < old_size; ++i) {
        if (!old_values[i].valid()) continue;
        insert(old_values[i], true);
    }
    delete [] old_values;
}

void Image::clear() {
    board_ = tables.start_image().board_;
}

uint Board::make_moves(BoardSet& set, int available_moves) const {
    if (VERBOSE) cout << "From: " << available_moves << "\n" << *this;

    uint8_t blue_to_move = available_moves & 1;
    auto& army  = blue_to_move ? blue() : red();
    auto& opponent_army  = blue_to_move ? red() : blue();
    Board board;
    auto& board_army = blue_to_move ? board.blue() : board.red();
    Norm distance_red;
    Coord critical_red  = Coord::INVALID;
    Coord critical_army = Coord::INVALID;
    Nbits Ndistance_army = 0;
    int off_base_from = ARMY;
    if (blue_to_move) {
        distance_red = tables.infinity();
        for (auto const& b: blue()) {
            if (tables.base_red(b)) 
                --off_base_from;
            else {
                auto d = tables.distance_base_red(b);
                if (d < distance_red) {
                    distance_red = d;
                    critical_red = b;
                }
            }
            Nbits Ndistance = 0;
            for (auto const& r: red())
                Ndistance |= tables.Ndistance(b, r);
            if (Ndistance > Ndistance_army) {
                Ndistance_army = Ndistance;
                critical_army = b;
            }
        }
    } else {
        for (auto const& r: red()) {
            Nbits Ndistance = 0;
            for (auto const& b: blue())
                Ndistance |= tables.Ndistance(r, b);
            if (Ndistance > Ndistance_army) {
                Ndistance_army = Ndistance;
                critical_army = r;
            }
        }

        Nbits Ndistance_red = NLEFT >> tables.infinity();
        for (auto& b: blue()) {
            if (tables.base_red(b))
                --off_base_from;
            else
                Ndistance_red |= tables.Ndistance_base_red(b);
        }
        distance_red = __builtin_clz(Ndistance_red);
    }
    Norm distance_army = __builtin_clz(Ndistance_army);

    if (blue_to_move)
        board.red() = opponent_army;
    else
        board.blue() = opponent_army;
    auto opponent_hash = opponent_army.hash();

    TypeCount type_count = tables.type_count();
    int edge_count = 0;
    int slides = 0;
    if (SLIDES) {
        for (auto const& b: blue()) {
            --type_count[tables.type(b)];
            edge_count += tables.edge_red(b);
        }
        for (auto tc: type_count)
            slides += max(tc, 0);
    }
    
    if (VERBOSE) {
        if (SLIDES) 
            cout << "Slides >= " << slides << ", red edge count " << edge_count << "\n";
        cout << "Distance army=" << static_cast<int>(distance_army) << ", critical=" << critical_army << "\n";
        cout << "Distance red =" << static_cast<int>(distance_red)  << ", critical=" << critical_red  << "\n";
        cout << "Off base=" << static_cast<uint>(off_base_from) << "\n";
        distance_red *= 2;
    }
    int needed_moves = min(distance_red, mask(distance_army+blue_to_move))+2*off_base_from-blue_to_move;
    if (VERBOSE)
        cout << "Needed moves=" << static_cast<int>(needed_moves) << "\n";
    if (needed_moves > available_moves) {
        if (VERBOSE)
            cout << "Late prune " << needed_moves << " > " << available_moves << "\n";
        return 1;
    }
    --available_moves;

    Image image{*this};
    ArmyE armyE;
    int off_base = off_base_from;

    for (int a=0; a<ARMY; ++a) {
        armyE.copy(army);
        int pos = a;

        auto const soldier = army[a];
        image.set(soldier, EMPTY);
        if (blue_to_move) off_base = off_base_from + tables.base_red(soldier);

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
            Nbits Ndistance = Ndistance_army;
            for (auto const&o: opponent_army)
                Ndistance |= tables.Ndistance(val, o);
            Norm d_army = __builtin_clz(Ndistance);
            if (blue_to_move) {
                if (tables.base_red(val)) {
                    int off = off_base - 1;
                    if (off == 0) throw(logic_error("Solution!"));
                    needed_moves = min(distance_red, mask(d_army))+2*off;
                } else {
                    needed_moves = min(min(distance_red, static_cast<Norm>(2*tables.distance_base_red(val))), mask(d_army))+2*off_base;
                }
            } else {
                needed_moves = min(distance_red, mask(d_army+1))+2*off_base-1;
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
            board_army.copy(armyE);
            if (CHECK) board.check(__LINE__);
            auto hash = board_army.hash() ^ opponent_hash;
            // cout << "Hash: " << hash << "\n";
            if (set.insert(board, hash)) {
                if (VERBOSE) cout << board;
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
    BoardSet set;
    cout << board;
    int nr_moves = 30;
    for (auto& move: game30) {
        board.make_moves(set, nr_moves);
        cout << "===============================\n";
        board.move(move.mirror());
        if (set.find(board)) {
            cout << "Good\n";
        } else {
            cout << "Bad\n";
        }
        // cout << board;
        --nr_moves;
    }
}

void my_main(int argc, char const* const* argv) {
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

    BoardSet set[2];
    set[0].insert(start);
    cout << "Set 0 done\n";
    for (int nr_moves = 29, i=0; nr_moves>0; --nr_moves, ++i) {
        auto& from_set = set[ i    % (sizeof(set)/sizeof(*set))];
        auto& to_set   = set[(i+1) % (sizeof(set)/sizeof(*set))];
        to_set.clear();
        uint64_t late = 0;
        for (auto& board: from_set) {
            if (!board.valid()) continue;
            if (CHECK) board.check(__LINE__);
            late += board.make_moves(to_set, nr_moves);
        }
        cout << "Set " << nr_moves << " done, size=" << to_set.size() << ", late prune=" << late << "\n";
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
