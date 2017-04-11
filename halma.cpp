#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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
// pure means does not modify any (non const) global memory.
# define PURE		__attribute__((__pure__))
// const means does not read/modify any (non const) global memory.
# define FUNCTIONAL	__attribute__((__const__))
#else // __GNUC__
# define RESTRICT
# define NOINLINE
# define LIKELY(x)	(x)
# define UNLIKELY(x)	(x)
# define HOT
# define COLD
# define PURE
# define FUNCTIONAL
#endif // __GNUC__

using namespace std;

bool const STATISTICS = false;
bool const SYMMETRY = true;
bool const MEMCHECK = false;
bool const CLOSED_LOOP = false;
bool const PASS = false;
// For the moment it is always allowed to jump the same man multiple times
// bool const DOUBLE_CROSS = true;
int  const BALANCE  = -1;
// 0 means let C++ decide

#define CHECK   1
#define VERBOSE	0

int const X = 9;
int const Y = 9;
int const MOVES = 6;
int const ARMY = 10;

// ARMY < 32
using BalanceMask = uint32_t;
int const BALANCE_BITS = std::numeric_limits<BalanceMask>::digits;
BalanceMask BALANCE_FULL = ~ static_cast<BalanceMask>(0);
constexpr BalanceMask make_balance_mask(int min, int max) {
    return
        min < 0 ? make_balance_mask(0, max) :
        max >= BALANCE_BITS ? make_balance_mask(min, BALANCE_BITS-1) :
        min > max ? 0 :
        BALANCE_FULL << min & BALANCE_FULL >> (BALANCE_BITS-1 - max);
}
int balance = -1;
int balance_delay = 0;
int balance_min, balance_max;

// There is no fundamental limit. Just make up *SOME* bound
uint const THREADS_MAX = 256;
uint nr_threads = 0;

const char letters[] = "abcdefghijklmnopqrstuvwxyz";

uint64_t SEED = 123456789;

using Sec      = chrono::seconds;

string HOSTNAME;

uint64_t const murmur_multiplier = UINT64_C(0xc6a4a7935bd1e995);

inline uint64_t murmur_mix(uint64_t v) {
    v *= murmur_multiplier;
    return v ^ (v >> 47);
}

inline uint64_t hash64(uint64_t v) {
    return murmur_mix(murmur_mix(v));
    // return murmur_mix(murmur_mix(murmur_mix(v)));
    // return XXHash64::hash(reinterpret_cast<void const *>(&v), sizeof(v), SEED);
}

NOINLINE string get_memory(bool set_base_mem = false);
size_t PAGE_SIZE;
// Linux specific
string get_memory(bool set_base_mem) {
    stringstream out;

    static size_t base_mem = 0;

    size_t mem = 0;
    std::ifstream statm;
    statm.open("/proc/self/statm");
    statm >> mem;
    mem *= PAGE_SIZE;
    if (set_base_mem) base_mem = mem;
    else mem -= base_mem;
    if (mem >= 1000000) out << "(" << setw(5) << mem / 1000000  << " MB)";
    return out.str();
}

using Norm = uint8_t;
using Parity = uint8_t;
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
Color operator~(Color color) {
    return RED+BLUE-color;
}

inline string svg_color(Color color) {
    switch (color) {
        case EMPTY: return "grey";
        case BLUE:  return "turquoise";
        case RED:   return "red";
        default:    return "black";
    }
}

inline string font_color(Color color) {
    switch (color) {
        case EMPTY: return "grey";
        case BLUE:  return "blue";
        case RED:   return "red";
        default:    return "black";
    }
}

class Coord;
using Diff = Coord;
using Moves = array<Diff, MOVES>;

using CoordVal = int16_t;
class Coord {
  private:
    static int const ROW  = 2*X;
  public:
    static int const SIZE = (Y+1)*ROW+X+2;
    static int const MAX  = (Y-1)*ROW+X-1;

    static inline Moves const& moves();

    Coord() {}
    Coord(Coord const& from, Diff const& diff) : pos_{static_cast<CoordVal>(from.pos_ + diff.pos_)} {}
    Coord(int x, int y) : pos_{static_cast<CoordVal>(y*ROW+x)} { }
    int x() const PURE { return (pos_+(X+(Y-1)*ROW)) % ROW - X; }
    int y() const PURE { return (pos_+(X+(Y-1)*ROW)) / ROW - (Y-1); }
    inline Parity parity() const PURE;
    string str()  const PURE { return letters[x()] + to_string(y()+1); }
    int pos()     const PURE { return pos_; }
    uint index()  const PURE { return OFFSET+ pos_; }
    uint index2() const PURE { return MAX+pos_; }
    void check(int line) const {
        int x_ = x();
        int y_ = y();
        if (x_ < 0) throw(logic_error("x negative at line " + to_string(line)));
        if (x_ >= X) throw(logic_error("x too large at line " + to_string(line)));
        if (y_ < 0) throw(logic_error("y negative at line " + to_string(line)));
        if (y_ >= Y) throw(logic_error("y too large at line " + to_string(line)));
    }
    // Mirror over SW-NE diagonal
    Coord mirror() const PURE {
        Coord result;
        result.pos_ = MAX - pos_;
        return result;
    }
    // Mirror over NW-SE diagonal
    inline Coord symmetric() const PURE;

    void svg(ostream& os, Color color, uint scale) const;
  private:
    static uint const OFFSET = ROW+1;
    CoordVal pos_;

    friend inline ostream& operator<<(ostream& os, Coord const& pos) {
    os << setw(2) << static_cast<int>(pos.x()) << "," << setw(3) << static_cast<int>(pos.y());
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
    friend bool operator==(Coord const& l, Coord const& r) {
        return l.pos_ == r.pos_;
    }
    friend bool operator!=(Coord const& l, Coord const& r) {
        return l.pos_ != r.pos_;
    }
};

void Coord::svg(ostream& os, Color color, uint scale) const {
    os << "      <circle cx='" << (x()+1) * scale << "' cy='" << (y()+1) * scale<< "' r='" << static_cast<uint>(scale * 0.35) << "' fill='" << svg_color(color) << "' />\n";
}

class ArmyE;
// Army as a set of Coord
class Army: public array<Coord, ARMY> {
  public:
    Army() {}
    uint64_t hash() const PURE {
        return XXHash64::hash(reinterpret_cast<void const*>(&(*this)[0]), sizeof(Coord) * ARMY, SEED);
    }
    inline void check(int line) const;
    inline Army symmetric() const PURE {
        Army result;
        transform(begin(), end(), result.begin(), [](Coord const&pos){ return pos.symmetric(); });
        sort(result.begin(), result.end());
        return result;
    }
    void print(ostream& os) const {
        for (auto const& pos: *this)
            os << pos << "\n";
    }
    Army& operator=(Army const& army) {
        std::copy(army.begin(), army.end(), begin());
        return *this;
    }
    inline Army& operator=(ArmyE const& army);

    friend bool operator==(Army const& l, Army const& r) {
        for (int i=0; i<ARMY; ++i)
            if (l[i] != r[i]) return false;
        return true;
    }
};

int cmp(Army const& left, Army const& right) {
    if (!SYMMETRY) return 0;
    return memcmp(reinterpret_cast<void const *>(&left[0]),
                  reinterpret_cast<void const *>(&right[0]),
                  sizeof(Coord) * ARMY);
}

// Army as a set of Coord
class ArmyE: public array<Coord, ARMY+2> {
  public:
    ArmyE() {
        at(-1)   = Coord{-1,-1};
        at(ARMY) = Coord{ X, Y};
    }
    explicit ArmyE(Army const& army) : ArmyE{} { *this = army; }
    // Coord operator[](ssize_t) = delete;
    Coord& at(int i) { return (*this)[i+1]; }
    Coord const& at(int i) const FUNCTIONAL { return (*this)[i+1]; }
    uint64_t hash() const PURE {
        return XXHash64::hash(reinterpret_cast<void const*>(&at(0)), sizeof(Coord) * ARMY, SEED);
    }
    inline void check(int line) const;
    inline ArmyE symmetric() const PURE {
        ArmyE result;
        transform(begin(), end(), result.begin(), [](Coord const&pos){ return pos.symmetric(); });
        sort(result.begin(), result.end());
        return result;
    }
    ArmyE& operator=(ArmyE const& army) {
        std::copy(army.begin(), army.end(), begin());
        return *this;
    }
    ArmyE& operator=(Army const& army) {
        std::copy(army.begin(), army.end(), begin());
        return *this;
    }
    Coord* begin() { return &at(0); }
    Coord* end  () { return &at(ARMY); }
    Coord const* begin() const FUNCTIONAL { return &at(0); }
    Coord const* end  () const FUNCTIONAL { return &at(ARMY); }
};

int cmp(ArmyE const& left, ArmyE const& right) {
    if (!SYMMETRY) return 0;
    return memcmp(reinterpret_cast<void const *>(&left.at(0)),
                  reinterpret_cast<void const *>(&right.at(0)),
                  sizeof(Coord) * ARMY);
}

Army& Army::operator=(ArmyE const& army) {
    std::copy(army.begin(), army.end(), begin());
    return *this;
}

class ArmyPos: public ArmyE {
  public:
    void copy(Army const& army, int pos) {
        std::copy(army.begin(), army.end(), begin());
        pos_ = pos;
    }
    void store(Coord const& val) {
        if (val > at(pos_+1)) {
            do {
                at(pos_) = at(pos_+1);
                // cout << "Set pos_ > " << pos_ << at(pos_) << "\n";
                ++pos_;
            } while (val > at(pos_+1));
        } else if (val < at(pos_-1)) {
            do {
                at(pos_) = at(pos_-1);
                // cout << "Set pos_ < " << pos_ << at(pos_) << "\n";
                --pos_;
            } while (val < at(pos_-1));
        }
        if (pos_ < 0) throw(logic_error("Negative pos_"));
        if (pos_ >= ARMY) throw(logic_error("Excessive pos_"));
        at(pos_) = val;
    }
  private:
    int pos_;
};

class ArmyMapper {
  public:
    ArmyMapper(Army const& army_symmetric) {
        for (uint i=0; i<ARMY; ++i)
            mapper_[army_symmetric[i].symmetric().pos()] = i;
    }
    uint8_t map(Coord const& pos) const PURE {
        return mapper_[pos.pos()];
    }
  private:
    array<uint8_t, Coord::MAX+1> mapper_;
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

inline ostream& operator<<(ostream& os, Army const& army) {
    army.print(os);
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
    for (auto const& pos: *this) pos.check(line);
    for (int i=-1; i<ARMY; ++i)
        if (at(i) >= at(i+1)) {
            cerr << *this;
            throw(logic_error("Army out of order at line " + to_string(line)));
        }
}

class SetStatistics {
  public:
    inline void stats_update(uint64_t offset) {
        if (!STATISTICS) return;
        if (offset) {
            ++hits_;
            tries_ += offset;
        } else
            ++misses_;
    }
    void stats_reset() {
        if (!STATISTICS) return;
        misses_ = hits_ = tries_ = 0;
    }
    NOINLINE void show_stats(ostream& os) const;
    void show_stats() const {
        show_stats(cout);
    }
  private:
    uint64_t hits_   = 0;
    uint64_t misses_ = 0;
    uint64_t tries_  = 0;
};

void SetStatistics::show_stats(ostream& os) const {
    if (!STATISTICS) return;
    if (hits_ == 0 && misses_ == 0) {
        os << "Not used\n";
        return;
    }
    os << "misses: " << misses_ << " (" << 100. * misses_ / (hits_+misses_) << "%)\n";
    os << "hits:   " << hits_ << " (" << 100. * hits_ / (hits_+misses_) << "%)\n";
    if (hits_)
        os << "Average retries: " << 1. * tries_ / hits_ << "\n";
}

using ArmyId = uint32_t;
STATIC const int ARMY_BITS = std::numeric_limits<ArmyId>::digits;
STATIC const ArmyId ARMY_HIGHBIT = static_cast<ArmyId>(1) << (ARMY_BITS-1);
STATIC const ArmyId ARMY_MASK = ARMY_HIGHBIT-1;

class ArmySet: public SetStatistics {
  public:
    ArmySet(ArmyId size = 1);
    ~ArmySet();
    void clear(ArmyId size = 1);
    void drop_hash() {
        delete [] values_;
        values_ = nullptr;
    }
    ArmyId size() const PURE { return used1_ - 1; }
    ArmyId max_size() const PURE {
        return size_;
    }
    ArmyId capacity() const PURE {
        return limit_;
    }
#if CHECK
    Army const& at(ArmyId i) const {
        if (i >= used1_) throw(logic_error("Army id " + to_string(i) + " out of range of set"));
        return armies_[i];
    }
#else  // CHECK
    Army const& at(ArmyId i) const PURE { return armies_[i]; }
#endif // CHECK
    inline ArmyId insert(Army  const& value);
    inline ArmyId insert(ArmyE const& value);
    ArmyId find(Army const& value) const PURE;
    Army const* begin() const PURE { return &armies_[1]; }
    Army const* end()   const PURE { return &armies_[used1_]; }

    void print(ostream& os) const;
    // Non copyable
    ArmySet(ArmySet const&) = delete;
    ArmySet& operator=(ArmySet const&) = delete;

  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.7*factor); }
    NOINLINE void resize();
    inline ArmyId _insert(Army  const& value, ArmyId hash, bool is_resize);
    inline ArmyId _insert(ArmyE const& value, ArmyId hash, bool is_resize);

    mutex exclude_;
    ArmyId size_;
    ArmyId mask_;
    ArmyId used1_;
    ArmyId limit_;
    ArmyId* values_;
    Army* armies_;
};

inline ostream& operator<<(ostream& os, ArmySet const& set) {
    set.print(os);
    return os;
}

struct Move {
    Move() {}
    Move(Coord const& from_, Coord const& to_): from{from_}, to{to_} {}
    Move(Army const& army_from, Army const& army_to);
    Move(Army const& army_from, Army const& army_to, int& diff);

    Move mirror() const PURE {
        return Move{from.mirror(), to.mirror()};
    }

    Coord from, to;
};

Move::Move(Army const& army_from, Army const& army_to): from{-1,-1}, to{-1, -1} {
    ArmyE const fromE{army_from};
    ArmyE const toE  {army_to};

    int i = 0;
    int j = 0;
    int diffs = 0;
    while (i < ARMY || j < ARMY) {
        if (fromE.at(i) == toE.at(j)) {
            ++i;
            ++j;
        } else if (fromE.at(i) < toE.at(j)) {
            from = fromE.at(i);
            ++i;
            ++diffs;
        } else {
            to = toE.at(j);
            ++j;
            ++diffs;
        }
    }
    if (diffs > 2)
        throw(logic_error("Multimove"));
    if (diffs == 1)
        throw(logic_error("Move going nowhere"));
}

Move::Move(Army const& army_from, Army const& army_to, int& diffs): from{-1,-1}, to{-1, -1} {
    ArmyE const fromE{army_from};
    ArmyE const toE  {army_to};

    int i = 0;
    int j = 0;
    diffs = 0;
    while (i < ARMY || j < ARMY) {
        if (fromE.at(i) == toE.at(j)) {
            ++i;
            ++j;
        } else if (fromE.at(i) < toE.at(j)) {
            from = fromE.at(i);
            ++i;
            ++diffs;
        } else {
            to = toE.at(j);
            ++j;
            ++diffs;
        }
    }
}

// Board as two Armies
class FullMove;
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    void move(Move const& move_);
    void move(Move const& move_, bool blue_to_move);
    inline void move(FullMove const& move_);
    inline void move(FullMove const& move_, bool blue_to_move);
    Army& blue() { return blue_; }
    Army& red()  { return red_; }
    Army const& blue() const FUNCTIONAL { return blue_; }
    Army const& red()  const FUNCTIONAL { return red_; }
    void check(int line) const {
        blue_.check(line);
        red_.check(line);
    }
    int min_moves(bool blue_to_move) const PURE;
    int min_moves() const PURE {
        return min(min_moves(true), min_moves(false));
    }
    inline Board symmetric() const PURE {
        return Board{blue().symmetric(), red().symmetric()};
    }
    void svg(ostream& os, uint scale, uint marging) const;

  private:
    Army blue_, red_;

    friend bool operator==(Board const& l, Board const& r) {
        return l.blue() == r.blue() && l.red() == r.red();
    }
};

void Board::svg(ostream& os, uint scale, uint margin) const {
    os << "      <path d='";
    for (int x=0; x<=X; ++x) {
        os << "M " << margin + x * scale << " " << margin << " ";
        os << "L " << margin + x * scale << " " << margin + Y * scale << " ";
    }
    for (int y=0; y<=X; ++y) {
        os << "M " << margin             << " " << margin + y * scale << " ";
        os << "L " << margin + X * scale << " " << margin + y * scale << " ";
    }
    os << "'           stroke='black' />\n";
    for (Coord const& pos: blue())
        pos.svg(os, BLUE, scale);
    for (Coord const& pos: red())
        pos.svg(os, RED,  scale);
}

class BoardSubSet {
  public:
    ArmyId allocated() const PURE { return mask_+1; }
    ArmyId capacity()  const PURE { return FACTOR(allocated()); }
    ArmyId size()      const PURE { return capacity() - left_; }
    void create(ArmyId size = 1);
    void destroy() {
        // cout << "Destroy BoardSubSet " << static_cast<void const*>(armies_) << "\n";
        if (MEMCHECK) nr_armies_ -= allocated();
        delete[] armies_;
    }
    ArmyId const* begin() const PURE { return &armies_[0]; }
    ArmyId const* end()   const PURE { return &armies_[allocated()]; }

    bool insert(ArmyId red_id, int symmetry) {
        if (CHECK) {
            if (red_id <= 0)
                throw(logic_error("red_id <= 0"));
            if (red_id >= ARMY_HIGHBIT)
                throw(logic_error("red_id is too large"));
        }
        ArmyId value = red_id | (symmetry < 0 ? ARMY_HIGHBIT : 0);
        bool result = insert(value);
        return result;
    }
    bool find(ArmyId red_id, int symmetry) const PURE {
        if (CHECK) {
            if (red_id <= 0)
                throw(logic_error("red_id <= 0"));
            if (red_id >= ARMY_HIGHBIT)
                throw(logic_error("red_id is too large"));
        }
        return find(red_id | (symmetry < 0 ? ARMY_HIGHBIT : 0));
    }

    static ArmyId split(ArmyId value, ArmyId& red_id) {
        red_id = value & ARMY_MASK;
        // cout << "Split: Value=" << hex << value << ", red id=" << red_id << ", symmetry=" << (value & ARMY_HIGHBIT) << dec << "\n";
        return value & ARMY_HIGHBIT;
    }
    static size_t nr_armies() PURE { return nr_armies_; }
    void print(ostream& os) const;
    void print() const { print(cout); }
  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.7*factor); }
    NOINLINE void resize();

    ArmyId* begin() PURE { return &armies_[0]; }
    ArmyId* end()   PURE { return &armies_[allocated()]; }

    bool insert(ArmyId id) {
        return _insert(id, false);
    }
    inline bool insert_new(ArmyId id) {
        return _insert(id, true);
    }
    inline bool _insert(ArmyId red_id, bool is_resize);
    bool find(ArmyId id) const PURE;

    ArmyId mask_;
    ArmyId left_;
    ArmyId* armies_;

    static size_t nr_armies_;
};
size_t BoardSubSet::nr_armies_ = 0;

void BoardSubSet::create(ArmyId size) {
    mask_ = size-1;
    left_ = FACTOR(size);
    armies_ = new ArmyId[size];
    // cout << "Create BoardSubSet " << static_cast<void const*>(armies_) << ": size " << size << ", " << left_ << " left\n";
    fill(begin(), end(), 0);
    if (MEMCHECK) nr_armies_ += allocated();
}

bool BoardSubSet::find(ArmyId value) const {
    ArmyId pos = hash64(value) & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        auto& v = armies_[pos];
        if (v == 0) return false;
        if (v == value) {
            // cout << "Found duplicate " << hash << "\n";
            return true;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

bool BoardSubSet::_insert(ArmyId value, bool is_resize) {
    // cout << "Insert " << value << "\n";
    if (left_ == 0) resize();
    ArmyId pos = hash64(value) & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << mask_+1 << "\n";
        auto& v = armies_[pos];
        if (v == 0) {
            // if (!is_resize) stats_update(offset);
            v = value;
            --left_;
            // cout << "Found empty\n";
            return true;
        }
        if (!is_resize && v == value) {
            // cout << "Found duplicate\n";
            // if (!is_resize) stats_update(offset);
            return false;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

void BoardSubSet::resize() {
    auto old_armies = armies_;
    auto old_size = mask_+1;
    ArmyId size = old_size*2;
    armies_ = new ArmyId[size];
    // cout << "Resize BoardSubSet " << static_cast<void const *>(old_armies) << " -> " << static_cast<void const *>(armies_) << ": " << size << "\n";
    if (MEMCHECK) nr_armies_ -= allocated();
    mask_ = size-1;
    if (MEMCHECK) nr_armies_ += allocated();
    left_ = FACTOR(size);
    fill(begin(), end(), 0);
    for (ArmyId i = 0; i < old_size; ++i)
        if (old_armies[i] != 0) insert_new(old_armies[i]);
    delete [] old_armies;
}

void BoardSubSet::print(ostream& os) const {
    for (ArmyId value: *this) {
        if (value) {
            ArmyId red_id;
            auto symmetry = split(value, red_id);
            os << " " << red_id << (symmetry ? "-" : "+");
        } else {
            os << " x";
        }
    }
}

class BoardSet {
    friend class BoardSubSetRef;
  public:
    BoardSet(bool keep = false, ArmyId size = 1);
    ~BoardSet() {
        for (auto& subset: *this)
            subset.destroy();
        // cout << "Destroy BoardSet " << static_cast<void const*>(subsets_) << "\n";
        delete [] subsets_;
    }
    ArmyId subsets() const PURE { return top_ - from(); }
    ArmyId size() const PURE { return size_; }
    void clear(ArmyId size = 1);
    BoardSubSet const&  at(ArmyId id) const PURE { return subsets_[id]; }
    BoardSubSet const& cat(ArmyId id) const PURE { return subsets_[id]; }
    BoardSubSet const* begin() const PURE { return &subsets_[from()]; }
    BoardSubSet const* end()   const PURE { return &subsets_[top_]; }
    ArmyId back_id() const PURE { return top_-1; }
    bool insert(ArmyId blue_id, ArmyId red_id, int symmetry) {
        if (CHECK) {
            if (blue_id <= 0)
                throw(logic_error("red_id <= 0"));
            if (blue_id >= ARMY_HIGHBIT)
                throw(logic_error("opponent is too large"));
        }
        lock_guard<mutex> lock{exclude_};
        bool result = grow_at(blue_id).insert(red_id, symmetry);
        size_ += result;
        return result;
    }
    bool insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red);
    bool insert(Board const& board, ArmySet& armies_to_move, ArmySet& armies_opponent, int nr_moves) {
        int blue_to_move = nr_moves & 1;
        return blue_to_move ?
            insert(board, armies_to_move, armies_opponent) :
            insert(board, armies_opponent, armies_to_move);
    }
    bool find(ArmyId blue_id, ArmyId red_id, int symmetry) const PURE {
        if (CHECK) {
            if (blue_id <= 0)
                throw(logic_error("blue_id <= 0"));
            if (blue_id >= ARMY_HIGHBIT)
                throw(logic_error("blue_id is too large"));
        }
        if (blue_id >= top_) return false;
        return cat(blue_id).find(red_id, symmetry);
    }
    bool find(Board const& board, ArmySet const& armies_blue, ArmySet const& armies_red) const PURE;
    bool find(Board const& board, ArmySet& armies_to_move, ArmySet& armies_opponent, int nr_moves) const PURE {
        int blue_to_move = nr_moves & 1;
        return blue_to_move ?
            find(board, armies_to_move, armies_opponent) :
            find(board, armies_opponent, armies_to_move);
    }
    bool solve(ArmyId solution_id, Army const& solution) {
        lock_guard<mutex> lock{exclude_};
        if (solution_id_) return false;
        solution_id_ = solution_id;
        solution_ = solution;
        return true;
    }
    ArmyId solution_id() const PURE { return solution_id_; }
    Army const& solution() const PURE { return solution_; }
    // Non copyable
    BoardSet(BoardSet const&) = delete;
    BoardSet& operator=(BoardSet const&) = delete;
    void print(ostream& os) const;
  private:
    ArmyId capacity() const PURE { return capacity_-1; }
    ArmyId next() {
        lock_guard<mutex> lock{exclude_};
        return from_ < top_ ? from_++ : 0;
    }
    ArmyId from() const PURE { return keep_ ? 1 : from_; }
    BoardSubSet&  at(ArmyId id) PURE { return subsets_[id]; }
    BoardSubSet& grow_at(ArmyId id) {
        if (id >= top_) {
            // Only in the multithreaded case id can be different from top_
            // if (id != top_) throw(logic_error("Cannot grow more than 1"));
            while (id >= capacity_) resize();
            while (id >= top_)
                subsets_[top_++].create();
        }
        return subsets_[id];
    }
    NOINLINE void resize() {
        auto old_subsets = subsets_;
        subsets_ = new BoardSubSet[capacity_*2];
        capacity_ *= 2;
        // cout << "Resize BoardSet " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity_ << "\n";
        copy(&old_subsets[from()], &old_subsets[top_], &subsets_[1]);
        if (!keep_) {
            top_ -= from_ - 1;
            from_ = 1;
        }
        delete [] old_subsets;
    }
    BoardSubSet* begin() PURE { return &subsets_[from()]; }
    BoardSubSet* end()   PURE { return &subsets_[top_]; }

    mutex exclude_;
    Army solution_;
    ArmyId solution_id_;
    ArmyId size_;
    ArmyId capacity_;
    ArmyId from_;
    ArmyId top_;
    BoardSubSet* subsets_;
    bool const keep_;
};

inline ostream& operator<<(ostream& os, BoardSet const& set) {
    set.print(os);
    return os;
}

class BoardSubSetRef {
  public:
    BoardSubSetRef(BoardSet& set): BoardSubSetRef{set, set.next()} {}
    ~BoardSubSetRef() { if (id_ && !keep_) subset_.destroy(); }
    ArmyId id() const PURE { return id_; }
    BoardSubSet const& armies() const PURE { return subset_; }

    BoardSubSetRef(BoardSubSetRef const&) = delete;
    BoardSubSetRef& operator=(BoardSubSetRef const&) = delete;
    void keep() { id_ = 0; }
  private:
    BoardSubSetRef(BoardSet& set, ArmyId id): subset_{set.at(id)}, id_{id}, keep_{set.keep_} {}
    BoardSubSet& subset_;
    ArmyId id_;
    bool const keep_;
};

BoardSet::BoardSet(bool keep, ArmyId size): solution_id_{keep}, size_{0}, capacity_{size+1}, from_{1}, top_{1}, keep_{keep} {
    subsets_ = new BoardSubSet[capacity_];
    // cout << "Create BoardSet " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
}

void BoardSet::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    from_ = top_ = 1;
    size_ = 0;
    solution_id_ = keep_;
    if (true) {
        auto old_subsets = subsets_;
        ++size;
        subsets_ = new BoardSubSet[size];
        delete[] old_subsets;
        capacity_ = size;
    }
}

void BoardSet::print(ostream& os) const {
    os << "-----\n";
    for (ArmyId i = from(); i < top_; ++i) {
        os << " Blue id " << i << ":";
        cat(i).print(os);
        os << "\n";
    }
    os << "-----\n";
}

class Image {
  public:
    inline Image() {
        clear();
    }
    inline Image(Army const& blue, Army const& red);
    inline Image(ArmyE const& blue, Army const& red);
    inline Image(Army const& blue, ArmyE const& red);
    inline Image(Army const& army, Color color);
    inline Image(ArmyE const& army, Color color);
    inline explicit Image(Board const& board): Image{board.blue(), board.red()} {}
    inline explicit Image(Army  const& blue): Image{blue, BLUE} {}
    inline explicit Image(ArmyE const& blue): Image{blue, BLUE} {}
    void print(ostream& os) const;
    inline void clear();
    Color get(Coord const& pos) const PURE { return board_[pos.index()]; }
    Color get(int x, int y) const PURE { return get(Coord{x,y}); }
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
    for (auto const& pos: blue)
        set(pos, BLUE);
    for (auto const& pos: red)
        set(pos, RED);
}

Image::Image(Army const& army, Color color) : Image{} {
    for (auto const& pos: army)
        set(pos, color);
}

Image::Image(ArmyE const& army, Color color) : Image{} {
    for (auto const& pos: army)
        set(pos, color);
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

void ArmySet::print(ostream& os) const {
    os << "[";
    for (ArmyId i=0; i < max_size(); ++i)
        os << " " << values_[i];
    os << " ] (" << static_cast<void const *>(this) << ")\n";
    for (ArmyId i=1; i < used1_; ++i) {
        os << "Army " << i << "\n" << Image{armies_[i]};
    }
}

class FullMove: public vector<Coord> {
  public:
    FullMove() {}
    FullMove(char const* str);
    FullMove(string const& str) : FullMove{str.c_str()} {}
    FullMove(Board const& from, Board const& to, Color color=COLORS);
    string str() const PURE;
    Coord from() const PURE;
    Coord to()   const PURE;
    Move move() const PURE;
  private:
    void move_expand(Board const& board_from, Board const& board_to, Move const& move);
};

FullMove::FullMove(char const* str): FullMove{} {
    auto ptr = str;
    if (!ptr[0]) return;
    while (true) {
        char ch = *ptr++;
        if (ch < letters[0] || ch >= letters[X]) break;
        int x = ch - 'a';
        int y = 0;
        while (true) {
            ch = *ptr++;
            if (ch < '0' || ch > '9') break;
            y = y * 10 + ch - '0';
            if (y > Y) break;
        }
        --y;
        if (y < 0 || y >= Y) break;
        emplace_back(x, y);
        if (!ch) return;
        if (ch != '-') break;
    }
    throw(logic_error("Could not parse full move '" + string(str) + "'"));
}

inline ostream& operator<<(ostream& os, FullMove const& move) {
    os << move.str();
    return os;
}

Coord FullMove::from() const {
    if (size() == 0) throw(logic_error("Empty full_move"));
    return (*this)[0];
}

Coord FullMove::to()   const {
    if (size() == 0) throw(logic_error("Empty full_move"));
    return (*this)[size()-1];
}

Move FullMove::move() const {
    if (size() == 0) throw(logic_error("Empty full_move"));
    return Move{(*this)[0], (*this)[size()-1]};
}

void FullMove::move_expand(Board const& board_from, Board const& board_to, Move const& move) {
    emplace_back(move.from);
    if (move.from.parity() != move.to.parity()) {
        // Must be a slide.
        emplace_back(move.to);
        // Check though
        for (auto step: Coord::moves())
            if (move.to == Coord{move.from, step}) return;
        throw(logic_error("Move is not a slide but has different parity"));
    }

    // Must be a jump
    Image image{board_from};
    if (CLOSED_LOOP) image.set(move.from, EMPTY);
    array<Coord, ARMY*2*MOVES+(1+MOVES)> reachable;
    array<int, ARMY*2*MOVES+(1+MOVES)> previous;
    reachable[0] = move.from;
    int nr_reachable = 1;
    for (int i=0; i < nr_reachable; ++i) {
        for (auto direction: Coord::moves()) {
            Coord jumpee{reachable[i], direction};
            if (image.get(jumpee) != RED && image.get(jumpee) != BLUE) continue;
            Coord target{jumpee, direction};
            if (image.get(target) != EMPTY) continue;
            image.set(target, COLORS);
            previous [nr_reachable] = i;
            reachable[nr_reachable] = target;
            if (target == move.to) {
                array<Coord, ARMY*2*MOVES+(1+MOVES)> trace;
                int t = 0;
                while (nr_reachable) {
                    trace[t++] = reachable[nr_reachable];
                    nr_reachable = previous[nr_reachable];
                }
                while (t > 0) emplace_back(trace[--t]);
                return;
            }
            ++nr_reachable;
        }
    }
    throw(logic_error("Move is a not a jump but has the same parity"));
}

FullMove::FullMove(Board const& board_from, Board const& board_to, Color color) : FullMove{} {
    bool blue_diff = board_from.blue() !=  board_to.blue();
    bool red_diff  = board_from.red()  !=  board_to.red();
    if (blue_diff && red_diff) throw(logic_error("Both players move"));

    if (blue_diff) {
        Move move{board_from.blue(), board_to.blue()};
        move_expand(board_from, board_to, move);
        return;
    }
    if (red_diff) {
        Move move{board_from.red(), board_to.red()};
        move_expand(board_from, board_to, move);
        return;
    }
    if (PASS) return;
    if (!CLOSED_LOOP) throw(logic_error("Invalid null move"));
    // CLOSED LOOP also needs a color hint
    throw(logic_error("closed loop analysis not implemented yet"));
}

string FullMove::str() const {
    string result;
    for (auto const &move: *this) {
        result += move.str();
        result += "-";
    }
    if (result.size()) result.pop_back();
    return result;
}

class FullMoves: public vector<FullMove> {
  public:
    FullMoves() {}
    template<class T>
    FullMoves(std::initializer_list<T> l): FullMoves{} {
        reserve(l.size());
        for (auto const& elem: l)
            emplace_back(elem);
    }
    void c_print(ostream& os, string const& indent) {
        os << indent << "{\n";
        for (auto const& move: *this)
            os << indent << "    \"" << move << "\",\n";
        os << indent << "}";
    }
    void c_print(ostream& os) {
        c_print(os, "");
    }
};

inline ostream& operator<<(ostream& os, FullMoves const& moves) {
    if (moves.empty()) throw(logic_error("Empty full move"));
    os << moves[0];
    for (size_t i=1; i<moves.size(); ++i)
        os << ", " << moves[i];
    return os;
}

class Svg {
  public:
    static uint const SCALE = 20;

    static string const solution_file() FUNCTIONAL {
        return string("solutions/halma-X") + to_string(X) + "Y" + to_string(Y) + "Army" + to_string(ARMY) + "Rule" + to_string(MOVES) + ".html";
    }
    Svg(uint scale = SCALE) : scale_{scale}, margin_{scale/2} {}
    void parameters(uint x, uint y, uint army, uint rule);
    void game(vector<Board> const& boards);
    void board(Board const& board) { board.svg(out_, scale_, margin_); }
    void move(FullMove const& move);
    void html(FullMoves const& full_moves);
    void html_header(uint nr_moves);
    void html_footer();
    void header();
    void footer();
    string str() const PURE { return out_.str(); }

  private:
    uint scale_;
    uint margin_;
    stringstream out_;
};

inline ostream& operator<<(ostream& os, Svg const& svg) {
    os << svg.str();
    return os;
}

template<class T>
class BoardTable {
  public:
    T&       operator[](Coord const& pos)       PURE { return data_[pos.pos()];}
    T const& operator[](Coord const& pos) const PURE { return data_[pos.pos()];}
    void fill(T value) { std::fill(data_.begin(), data_.end(), value); }
    void set(Army const& army, T const& value) {
        for (auto const& pos: army)
            (*this)[pos] = value;
    }
    void set(ArmyE const& army, T const& value) {
        for (auto const& pos: army)
            (*this)[pos] = value;
    }
  private:
    array<T, Coord::MAX+1> data_;
};

using ParityCount = array<int, 4>;
class Tables {
  public:
    Tables();
    uint min_moves() const PURE { return min_moves_; }
    inline Norm norm(Coord const& left, Coord const& right) const PURE {
        return norm_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance(Coord const& left, Coord const& right) const PURE {
        return distance_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance_base_red(Coord const& pos) const PURE {
        return distance_base_red_[pos];
    }
    inline Nbits Ndistance(Coord const& left, Coord const& right) const PURE {
        return NLEFT >> distance(left, right);
    }
    inline Nbits Ndistance_base_red(Coord const& pos) const PURE {
        return NLEFT >> distance_base_red(pos);
    }
    inline uint8_t base_red(Coord const& pos) const PURE {
        return base_red_[pos];
    }
    inline uint8_t edge_red(Coord const& pos) const PURE {
        return edge_red_[pos];
    }
    inline Parity parity(Coord const& pos) const PURE {
        return parity_[pos];
    }
    inline Coord symmetric(Coord const& pos) const PURE {
        return symmetric_[pos];
    }
    // The folowing methods in Tables really only PURE. However they are only
    // ever applied to the constant tables so the access global memory that
    // never changes making them effectively FUNCTIONAL
    inline ParityCount const& parity_count() const FUNCTIONAL {
        return parity_count_;
    }
    Norm infinity() const FUNCTIONAL { return infinity_; }
    Moves const& moves() const FUNCTIONAL { return moves_; }
    Board const& start() const FUNCTIONAL { return start_; }
    Image const& start_image() const FUNCTIONAL { return start_image_; }
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
    void print_parity(ostream& os) const;
    void print_parity() const {
        print_parity(cout);
    }
    void print_symmetric(ostream& os) const;
    void print_symmetric() const {
        print_symmetric(cout);
    }
    void print_parity_count(ostream& os) const;
    void print_parity_count() const {
        print_parity_count(cout);
    }
  private:
    ParityCount parity_count_;
    uint min_moves_;
    Moves moves_;
    Norm infinity_;
    BoardTable<Coord> symmetric_;
    array<Norm, 2*Coord::MAX+1> norm_;
    array<Norm, 2*Coord::MAX+1> distance_;
    BoardTable<Norm> distance_base_red_;
    BoardTable<uint8_t> base_red_;
    BoardTable<uint8_t> edge_red_;
    BoardTable<Parity> parity_;
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
    base_red_.fill(0);
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
            base_red_[red[i]] = 1;
            ++i;
            --x;
            ++y;
        }
        d++;
    }
    sort(blue.begin(), blue.end());
    sort(red.begin(),  red.end());
    for (auto const& pos: blue)
        if (base_red_[pos]) throw(logic_error("Red and blue overlap"));

    for (int y=0; y < Y; ++y) {
        start_image_.set(-1, y, COLORS);
        start_image_.set( X, y, COLORS);
        Norm d = infinity_;
        Parity y_parity = y%2*2;
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            start_image_.set(pos, EMPTY);
            for (int i=0; i<ARMY; ++i) {
                Norm d1 = norm(pos, red[i]);
                if (d1 < d) d = d1;
            }
            distance_base_red_[pos] = d > 2 ? d-2 : 0;
            edge_red_[pos] = d == 1;
            parity_[pos] = y_parity + x % 2;
            symmetric_[pos] = Coord(y, x);
        }
    }
    for (int x=-1; x <= X; ++x) {
        start_image_.set(x, -1, COLORS);
        start_image_.set(x,  Y, COLORS);
    }

    fill(parity_count_.begin(), parity_count_.end(), 0);
    for (auto const& r: red)
        ++parity_count_[parity(r)];

    min_moves_ = start_.min_moves();
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

void Tables::print_parity(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(parity(pos));
        }
        os << "\n";
    }
}

void Tables::print_symmetric(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << "|" << symmetric(pos);
        }
        os << "\n";
    }
}

void Tables::print_parity_count(ostream& os) const {
    for (auto c: parity_count_)
        os << " " << c;
    os << "\n";
}

STATIC Tables const tables;

Moves const& Coord::moves() {
    return tables.moves();
}

Coord Coord::symmetric() const {
    return tables.symmetric(*this);
}

Parity Coord::parity() const {
    return tables.parity(*this);
}

ArmySet::ArmySet(ArmyId size) : size_{size}, mask_{size-1}, used1_{1}, limit_{FACTOR(size)} {
    if (limit_ >= ARMY_HIGHBIT)
        throw(overflow_error("Army size too large"));
    values_ = new ArmyId[size];
    for (ArmyId i=0; i<size; ++i) values_[i] = 0;
    armies_ = new Army[limit_+1];
}

ArmySet::~ArmySet() {
    delete [] armies_;
    if (values_) delete [] values_;
}

void ArmySet::clear(ArmyId size) {
    stats_reset();
    ArmyId new_limit = FACTOR(size);
    if (new_limit >= ARMY_HIGHBIT)
        throw(overflow_error("Army size grew too large"));
    auto new_values = new ArmyId[size];
    auto new_armies = new Army[new_limit+1];
    if (values_) delete [] values_;
    values_ = new_values;
    delete [] armies_;
    armies_ = new_armies;
    for (ArmyId i=0; i<size; ++i) values_[i] = 0;
    size_ = size;
    mask_ = size-1;
    limit_ = new_limit;
    used1_ = 1;
}

ArmyId ArmySet::insert(Army  const& value) {
    // cout << "Insert:\n" << Image{value};
    // Take hash calculation out of the mutex
    ArmyId hash = value.hash();
    lock_guard<mutex> lock{exclude_};
    ArmyId i = _insert(value, hash, false);
    // cout << "i=" << i << "\n\n";
    return i;
}
ArmyId ArmySet::insert(ArmyE const& value) {
    // cout << "Insert:\n" << Image{value};
    // Take hash calculation out of the mutex
    ArmyId hash = value.hash();
    lock_guard<mutex> lock{exclude_};
    ArmyId i = _insert(value, hash, false);
    // cout << "i=" << i << "\n\n";
    return i;
}

ArmyId ArmySet::_insert(Army const& value, ArmyId hash, bool is_resize) {
    if (used1_ > limit_) resize();
    ArmyId pos = hash & mask_;
    ArmyId offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values_[pos];
        if (i == 0) {
            values_[pos] = used1_;
            armies_[used1_] = value;
            ArmyId id = used1_++;
            if (!is_resize) {
                stats_update(offset);
                // cout << "Found empty\n";
            }
            return id;
        }
        if (!is_resize && armies_[i] == value) {
            // cout << "Found duplicate " << hash << "\n";
            if (!is_resize) stats_update(offset);
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

ArmyId ArmySet::_insert(ArmyE const& value, ArmyId hash, bool is_resize) {
    if (used1_ > limit_) resize();
    ArmyId pos = hash & mask_;
    ArmyId offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values_[pos];
        if (i == 0) {
            values_[pos] = used1_;
            armies_[used1_] = value;
            ArmyId id = used1_++;
            if (!is_resize) {
                stats_update(offset);
                // cout << "Found empty\n";
            }
            return id;
        }
        if (!is_resize && armies_[i] == value) {
            if (!is_resize) stats_update(offset);
            // cout << "Found duplicate " << hash << "\n";
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

ArmyId ArmySet::find(Army const& army) const {
    ArmyId pos = army.hash() & mask_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values_[pos];
        if (i == 0) return 0;
        Army& a = armies_[i];
        if (a == army) return i;
        ++offset;
        pos = (pos + offset) & mask_;
    }
}

void ArmySet::resize() {
    if (size_ >= ARMY_HIGHBIT)
        throw(overflow_error("Army size grew too large"));
    auto old_values = values_;
    auto old_armies = armies_;
    auto old_used1  = used1_;
    size_ *= 2;
    // cout << "Resize: " << size_ << "\n";
    values_ = new ArmyId[size_];
    limit_ = FACTOR(size_);
    armies_ = new Army[limit_+1];
    delete [] old_values;
    mask_ = size_-1;
    used1_ = 1;
    for (ArmyId i = 0; i < size_; ++i) values_[i] = 0;
    for (ArmyId i = 1; i < old_used1; ++i)
        _insert(old_armies[i], old_armies[i].hash(), true);
    delete [] old_armies;
}

bool BoardSet::insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red) {
    Army const& blue = board.blue();
    auto blue_symmetric = blue.symmetric();
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.insert(blue_symmetry >= 0 ? blue : blue_symmetric);

    Army const& red  = board.red();
    auto red_symmetric = red.symmetric();
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.insert(red_symmetry >= 0 ? red : red_symmetric);

    int symmetry = blue_symmetry * red_symmetry;
    return insert(blue_id, red_id, symmetry);
}

bool BoardSet::find(Board const& board, ArmySet const& armies_blue, ArmySet const& armies_red) const {
    Army const& blue = board.blue();
    auto blue_symmetric = blue.symmetric();
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.find(blue_symmetry >= 0 ? blue : blue_symmetric);
    if (blue_id == 0) return false;

    Army const& red = board.red();
    auto red_symmetric = red.symmetric();
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.find(red_symmetry >= 0 ? red : red_symmetric);
    if (red_id == 0) return false;

    int symmetry = blue_symmetry * red_symmetry;
    return find(blue_id, red_id, symmetry);
}

void Image::clear() {
    board_ = tables.start_image().board_;
}

int Board::min_moves(bool blue_to_move) const {
    blue_to_move = blue_to_move ? true : false;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = NLEFT >> tables.infinity();
    int off_base_from = 0;
    ParityCount parity_count_from = tables.parity_count();
    int edge_count_from = 0;
    for (auto const& b: blue()) {
        --parity_count_from[b.parity()];
        if (tables.base_red(b)) continue;
        ++off_base_from;
        edge_count_from += tables.edge_red(b);
        Ndistance_red |= tables.Ndistance_base_red(b);
        for (auto const& r: red())
            Ndistance_army |= tables.Ndistance(r, b);
    }
    int slides = 0;
    for (auto tc: parity_count_from)
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
    return needed_moves;
}

void Board::move(Move const& move_, bool blue_to_move) {
    auto& army = blue_to_move ? blue() : red();
    auto pos = equal_range(army.begin(), army.end(), move_.from);
    if (pos.first == pos.second)
        throw(logic_error("Move not found"));
    *pos.first = move_.to;
    sort(army.begin(), army.end());
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

void Board::move(FullMove const& move_, bool blue_to_move) {
    move(move_.move(), blue_to_move);
}

void Board::move(FullMove const& move_) {
    move(move_.move());
}

void Svg::html_header(uint nr_moves) {
    out_ <<
        "<html>\n"
        "  <body>\n"
        "   <h1>" << nr_moves << " moves</h1>\n";
}

void Svg::html_footer() {
    out_ <<
        "  </body>\n"
        "</html>\n";
}

void Svg::header() {
    out_ << "    <svg height='" << Y * scale_ + 2*margin_ << "' width='" << X * scale_ + 2*margin_ << "'>\n";
    uint h = scale_ * 0.10;
    uint w = scale_ * 0.15;
    out_ <<
        "      <defs>\n"
        "        <marker id='arrowhead' markerWidth='" << w << "' markerHeight='" << 2*h << "' \n"
        "        refX='" << w << "' refY='" << h << "' orient='auto'>\n"
        "          <polygon points='0 0, " << w << " " << h << ", 0 " << 2*h << "' />\n"
        "        </marker>\n"
        "      </defs>\n";
}

void Svg::footer() {
    out_ << "    </svg>\n";
}

void Svg::parameters(uint x, uint y, uint army, uint rule) {
    out_ <<
        "    <table>\n"
        "      <tr><th align='left'>X</th><td>" << x << "</td></tr>\n"
        "      <tr><th align='left'>Y</th><td>" << y << "</td></tr>\n"
        "      <tr><th align='left'>Army</th><td>" << army << "</td></tr>\n"
        "      <tr><th align='left'>Rule</th><td>" << rule << "-move</td></tr>\n"
        "      <tr><th align='left'>Bound</th><td> &ge; " << tables.min_moves() << " moves</td></tr>\n"
        "      <tr><th align='left'>Heuristics</th><td>";
    if (balance < 0)
        out_ << "None\n";
    else
        out_ << "Balance " << balance << ", delay " << balance_delay;
    out_ <<
        "</td>\n"
        "      <tr><th align='left'>Host</th><td>" << HOSTNAME << "</td></tr>\n"
        "      <tr><th align='left'>Threads</th><td>" << nr_threads << "</td></tr>\n"
        "    </table>\n";
}

void Svg::move(FullMove const& move) {
    out_ << "      <polyline points='";
    for (Coord const& pos: move) {
        out_ << pos.x() * scale_ + scale_/2 + margin_ << "," << pos.y() * scale_ + scale_/2 + margin_ << " ";
    }
    out_ << "' stroke='black' stroke-width='" << scale_/10 << "' fill='none' marker-end='url(#arrowhead)' />\n";
}

void Svg::game(vector<Board> const& boards) {
    parameters(X, Y, ARMY, MOVES);
    FullMoves full_moves;
    for (size_t i = 0; i < boards.size(); ++i) {
        header();
        auto const& board_from = boards[i];
        board(board_from);
        if (i < boards.size()-1) {
            auto const& board_to = boards[i+1];
            full_moves.emplace_back(board_from, board_to);
            move(full_moves.back());
        }
        footer();
    }
    html(full_moves);
}

void Svg::html(FullMoves const& full_moves) {
    int color_index = full_moves.size() % 2;
    string color_font[] = {
        "      <font color='" + font_color(RED ) + "'>",
        "      <font color='" + font_color(BLUE) + "'>",
    };
    string moves_string;
    for (auto const& full_move: full_moves) {
        moves_string += color_font[color_index];
        moves_string += full_move.str();
        moves_string += "</font>,\n";
        color_index = !color_index;
    }
    if (full_moves.size()) {
        moves_string.pop_back();
        moves_string.pop_back();
        out_ << "    <p>\n" << moves_string << "\n    </p>\n";
    }
}

// Handle commandline options.
// Simplified getopt for systems that don't have it in their library (Windows..)
class GetOpt {
  private:
    string const options;
    char const* const* argv;
    int nextchar = 0;
    int optind = 1;
    char ch = '?';
    char const* optarg = nullptr;

  public:
    int ind() const PURE { return optind; }
    char const* arg() const PURE { return optarg; }
    char const* next_arg() { return argv[optind++]; }
    char option() const PURE { return ch; }

    GetOpt(string const options_, char const* const* argv_) :
        options(options_), argv(argv_) {}
    char next() {
        while (1) {
            if (nextchar == 0) {
                if (!argv[optind] ||
                    argv[optind][0] != '-' ||
                    argv[optind][1] == 0) return ch = 0;
                if (argv[optind][1] == '-' && argv[optind][2] == 0) {
                    ++optind;
                    return ch = 0;
                }
                nextchar = 1;
            }
            ch = argv[optind][nextchar++];
            if (ch == 0) {
                ++optind;
                nextchar = 0;
                continue;
            }
            auto pos = options.find(ch);
            if (pos == string::npos) ch = '?';
            else if (options[pos+1] == ':') {
                if (argv[optind][nextchar]) {
                    optarg = &argv[optind][nextchar];
                } else {
                    optarg = argv[++optind];
                    if (!optarg) return ch = options[0] == ':' ? ':' : '?';
                }
                ++optind;
                nextchar = 0;
            }
            return ch;
        }
    }
};

// When this is used it should not be accessed.
// Make it point to bad memory to force mayhem if it is
BoardTable<uint8_t>const *dummy_backtrack = nullptr;

// gcc is unable to inline the huge bodies if done as functions
// Hack around it using the preprocessor
#define NAME	     thread_blue_moves
#define BACKTRACK    0
#define BLUE_TO_MOVE 1
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK
#undef NAME

#define NAME	     thread_red_moves
#define BACKTRACK    0
#define BLUE_TO_MOVE 0
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK
#undef NAME

#define NAME	     thread_blue_moves_backtrack
#define BACKTRACK    1
#define BLUE_TO_MOVE 1
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK
#undef NAME

#define NAME	     thread_red_moves_backtrack
#define BACKTRACK    1
#define BLUE_TO_MOVE 0
#include "moves.cpp"
#undef BLUE_TO_MOVE
#undef BACKTRACK
#undef NAME

uint64_t _make_all_moves(BoardSet& boards_from,
                         BoardSet& boards_to,
                         ArmySet const& moving_armies,
                         ArmySet const& opponent_armies,
                         ArmySet& moved_armies,
                         int nr_moves,
                         bool backtrack,
                         BoardTable<uint8_t> const& red_backtrack) {
    auto start = chrono::steady_clock::now();

    vector<future<uint64_t>> results;
    uint64_t late;
    int blue_to_move = nr_moves & 1;
    if (blue_to_move) {
        if (backtrack) {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, thread_blue_moves_backtrack,
                      ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      nr_moves));
            late = thread_blue_moves_backtrack
                (boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 nr_moves);
        } else {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, thread_blue_moves,
                      ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      nr_moves));
            late = thread_blue_moves
                (boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 nr_moves);
        }
    } else {
        if (backtrack) {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, thread_red_moves_backtrack,
                      ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      ref(red_backtrack), nr_moves));
            late = thread_red_moves_backtrack
                (boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 red_backtrack, nr_moves);
        } else {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, thread_red_moves,
                      ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      nr_moves));
            late = thread_red_moves
                (boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 nr_moves);
        }
    }
    for (auto& result: results) late += result.get();

    auto stop = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop-start).count();
    moved_armies.show_stats();
    // boards_to.show_stats();
    if (MEMCHECK) cout << "nr armies in subsets=" << BoardSubSet::nr_armies() << "\n";
    cout << setw(6) << duration << " s, set " << setw(2) << nr_moves-1 << " done," << setw(10) << boards_to.size() << " boards /" << setw(9) << moved_armies.size() << " armies " << setw(7);
    if (nr_moves % 2)
        cout << boards_to.size()/(moved_armies.size() ? moved_armies.size() : 1);
    else
        cout << boards_to.size()/(opponent_armies.size() ? opponent_armies.size() : 1);
    cout << " " << get_memory() << endl;

    return late;
}

NOINLINE uint64_t
make_all_moves(BoardSet& boards_from,
               BoardSet& boards_to,
               ArmySet const& moving_armies,
               ArmySet const& opponent_armies,
               ArmySet& moved_armies,
               int nr_moves) {
    return _make_all_moves(boards_from, boards_to,
                           moving_armies, opponent_armies, moved_armies,
                           nr_moves, false, *dummy_backtrack);
}

NOINLINE uint64_t
make_all_moves_backtrack(BoardSet& boards_from,
                         BoardSet& boards_to,
                         ArmySet const& moving_armies,
                         ArmySet const& opponent_armies,
                         ArmySet& moved_armies,
                         int nr_moves,
                         BoardTable<uint8_t> const& red_backtrack) {
    return _make_all_moves(boards_from, boards_to,
                           moving_armies, opponent_armies, moved_armies,
                           nr_moves, true, red_backtrack);
}

void play(bool print_moves=false) {
    auto board = tables.start();
    Board previous_board;
    if (print_moves) previous_board = board;

    FullMoves moves;

    // The classic 30 solution
    // Used to check the code, especially the pruning
    FullMoves game30 {
        "g8-f8",
            "b2-b4",
            "f9-f7",
            "a4-c4",
            "i7-g7-e7",
            "c1-c3-c5",
            "i8-g8-e8-e6",
            "b3-b5-d3",
            "e6-d6",
            "a1-c1-c3-e3",
            "f7-g6",
            "c4-c6-e6-e8-g8-i8",
            "i6-g8-e8-e6-c6-c4-e2-e4",
            "a2-a4-c4-c6-e6-e8-g8-i6",
            "i9-i7-g7-g5",
            "d1-b3-b5-d5-d7-f7-f9",
            "g5-f5",
            "e3-e5-g5-g7-i7-i9",
            "h8-h6-f6-f4-d4-d2-b2",
            "a3-c1-c3-e3-e5-g5-g7-i7",
            "g9-e9-g7-g5-e5-e3-c3",
            "b4-d2-d4-f4-f6-h6-h8",
            "h7-h6",
            "c2-c4-c6-e6-e8-g8",
            "h9-h7-h5-f7-d7-d5",
            "c5-e5-g5-g7-g9",
            "d5-d4",
            "d3-d5-d7-f7-h5-h7-h9",
            "e4-c4-c2-a2",
            "b1-b3-d3-d5-d7-f7-h5-h7",
            };

    int nr_moves = game30.size();
    for (auto& move: game30) {
        cout << board;
        // cout << board.symmetric();

        BoardSet board_set[2];
        ArmySet  army_set[3];
        board_set[0].insert(board, army_set[0], army_set[1], nr_moves);

        make_all_moves(board_set[0], board_set[1],
                       army_set[0], army_set[1], army_set[2], nr_moves);

        cout << "===============================\n";
        --nr_moves;
        board.move(move);
        if (board_set[1].find(board, army_set[1], army_set[2], nr_moves)) {
            cout << "Good\n";
        } else {
            cout << "Bad\n";
        }
        // cout << board;
        if (print_moves) {
            moves.emplace_back(previous_board, board);
            previous_board = board;
        }
    }
    if (print_moves) {
        moves.c_print(cout);
        cout << ";\n";
    }
}

bool solve(Board const& board, int nr_moves, Army& red_army) {
    auto start_solve = chrono::steady_clock::now();
    array<BoardSet, 2> board_set;
    array<ArmySet, 3>  army_set;
    board_set[0].insert(board, army_set[0], army_set[1], nr_moves);
    cout << setw(14) << "set " << nr_moves << " done (" << HOSTNAME << ", ";
    if (balance >= 0)
        cout << "balance=" << balance << ", delay=" << balance_delay;
    else
        cout << "no heuristics";
    cout << ")" << endl;

    ArmyId red_id = 0;
    for (int i=0; nr_moves>0; --nr_moves, ++i) {
        auto& boards_from = board_set[ i    % 2];
        auto& boards_to   = board_set[(i+1) % 2];
        boards_to.clear();
        auto const& moving_armies   = army_set[ i    % 3];
        auto const& opponent_armies = army_set[(i+1) % 3];
        auto& moved_armies    = army_set[(i+2) % 3];

        moved_armies.clear();
        make_all_moves(boards_from, boards_to,
                       moving_armies, opponent_armies, moved_armies,
                       nr_moves);
        moved_armies.drop_hash();

        if (boards_to.size() == 0) {
            auto stop_solve = chrono::steady_clock::now();
            auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            cout << setw(6) << duration << " s, no solution" << endl;
            return false;
        }
        if (boards_to.solution_id()) {
            red_id = boards_to.solution_id();
            red_army = boards_to.solution();
            --nr_moves;
            break;
        }
    }
    auto stop_solve = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
    cout << setw(6) << duration << " s, solved" << endl;
    if (nr_moves) cout << "Unexpected early solution. Bailing out\n";

    if (red_id == 0) throw(logic_error("Solved without solution"));
    return true;
}

void backtrack(Board const& board, int nr_moves,
               Army const& last_red_army) {
    cout << "Start backtracking\n";

    auto start_solve = chrono::steady_clock::now();
    vector<unique_ptr<BoardSet>> board_set;
    board_set.reserve(nr_moves+1);
    vector<unique_ptr<ArmySet>>  army_set;
    army_set.reserve(nr_moves+2);

    board_set.emplace_back(new BoardSet(true));
    army_set.emplace_back(new ArmySet);
    army_set.emplace_back(new ArmySet);
    board_set[0]->insert(board, *army_set[0], *army_set[1], nr_moves);

    BoardTable<uint8_t> red_backtrack{};
    red_backtrack.fill(0);
    red_backtrack.set(last_red_army, 2);

    cout << setw(14) << "set " << nr_moves << " done" << endl;
    for (int i=0; nr_moves>0; --nr_moves, ++i) {
        board_set.emplace_back(new BoardSet(true));
        auto& boards_from = *board_set[i];
        auto& boards_to   = *board_set[i+1];

        army_set.emplace_back(new ArmySet);
        auto const& moving_armies   = *army_set[i];
        auto const& opponent_armies = *army_set[i+1];
        auto& moved_armies        = *army_set[i+2];

        make_all_moves_backtrack
            (boards_from, boards_to,
             moving_armies, opponent_armies, moved_armies,
             nr_moves, red_backtrack);

        if (boards_to.size() == 0)
            throw(logic_error("No solution while backtracking"));
    }
    auto stop_solve = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
    cout << setw(6) << duration << " s, backtrack tables built" << endl;

    // Do some sanity checking
    BoardSet const& final_board_set = *board_set.back();
    ArmySet const& final_army_set = *army_set.back();

    // There should be only 1 blue army completely on the red base
    if (final_army_set.size() != 1)
        throw(logic_error("More than 1 final blue army"));
    ArmyId blue_id = final_board_set.back_id();
    if (blue_id != 1)
        throw(logic_error("Unexpected blue army id"));

    // There should be only 1 final board
    if (final_board_set.size() != 1)
        throw(logic_error("More than 1 solution while backtracking"));
    BoardSubSet const& final_subset = final_board_set.cat(1);
    if (final_subset.size() != 1)
        throw(logic_error("More than 1 final red army"));
    ArmyId red_value = 0;
    for (ArmyId value: final_subset)
        if (value != 0) {
            red_value = value;
            break;
        }
    if (red_value == 0)
        throw(logic_error("Could not find final red army"));
    ArmyId red_id;
    bool skewed = BoardSubSet::split(red_value, red_id) != 0;
    // Backtracking forced the final red army to be last_red_army
    // So there can only be 1 final red army and it therefore has army id 1
    if (red_id != 1)
        throw(logic_error("Unexpected red army id"));
    // And it was stored without flip
    if (skewed)
        throw(logic_error("Unexpected red army skewed"));

    vector<Board> boards;
    // Reserve nr_moves+1 boards
    size_t board_pos = board_set.size();
    boards.resize(board_pos);
    boards[--board_pos] = Board{final_army_set.at(blue_id), last_red_army};

    if (false) {
        cout << "Initial\n";
        cout << "Blue: " << blue_id << ", Red: " << red_id << ", skewed=" << skewed << "\n";
        cout << boards[board_pos];
    }

    bool opponent_flipped  = false;
    bool blue_symmetry = true;
    bool red_symmetry  = false;
    board_set.pop_back();
    for (int nr_moves = 2*board_set.size()+3;
         board_set.size() != 0;
         --nr_moves, board_set.pop_back(), army_set.pop_back()) {
        BoardSet board_froms;
        // Set keep on board_tos to suppress solution printing
        // We don't care about the implied late free since it is tiny and scoped
        BoardSet board_tos{true};
        BoardSet const& moved_boards = *board_set[board_set.size()-1];
        ArmySet  to_armies;
        ArmySet const& moving_armies   = *army_set[army_set.size()-1];
        ArmySet const& opponent_armies = *army_set[army_set.size()-2];
        ArmySet const& moved_armies    = *army_set[army_set.size()-3];

        board_froms.insert(blue_id, red_id, skewed ? -1 : 0);
        int blue_to_move = nr_moves & 1;
        if (blue_to_move) {
            thread_blue_moves(board_froms, board_tos,
                              moving_armies, opponent_armies, to_armies,
                              nr_moves);
            while (true) {
                BoardSubSetRef subset{board_tos};
                ArmyId b_id = subset.id();
                if (b_id == 0) break;
                Army const& blue_army = to_armies.at(b_id);
                blue_id = moved_armies.find(blue_army);
                if (blue_id == 0) continue;
                BoardSubSet const& red_armies = subset.armies();
                for (auto const& red_value: red_armies) {
                    if (red_value == 0) continue;
                    ArmyId r_id;
                    skewed = BoardSubSet::split(red_value, r_id) != 0;
                    if (r_id != red_id) continue;
                    if (moved_boards.find(blue_id, red_id, skewed ? -1 : 0)) {

                        auto blue_army_symmetric = blue_army.symmetric();
                        blue_symmetry = blue_army == blue_army_symmetric;
                        --board_pos;
                        if (red_symmetry && !blue_symmetry) {
                            // If the opponent is symmetric we lost
                            // track of the oreientation and can
                            // only recover by trying both
                            try {
                                boards[board_pos] = Board{
                                    blue_army,
                                    boards[board_pos+1].red()};
                                FullMove{boards[board_pos+1], boards[board_pos], BLUE};
                                opponent_flipped = false;
                            } catch(exception& e) {
                                try {
                                    boards[board_pos] = Board{
                                        blue_army_symmetric,
                                        boards[board_pos+1].red()};
                                    FullMove{boards[board_pos+1], boards[board_pos], BLUE};
                                    opponent_flipped = true;
                                } catch(exception& e) {
                                    throw(logic_error("No orientation works"));
                                }
                            }
                        } else {
                            opponent_flipped ^= skewed;
                            boards[board_pos] = Board{
                                opponent_flipped ? blue_army_symmetric : blue_army,
                                boards[board_pos+1].red()};
                        }
                        if (false) {
                            cout << "Found blue:\n" << Image{blue_army, BLUE} << boards[board_pos] << "skewed=" << skewed << "\n";
                            FullMove{boards[board_pos+1], boards[board_pos], BLUE};
                        }
                        goto BLUE_DONE;
                    }
                }
            }
            throw(logic_error("Blue backtrack failure"));
          BLUE_DONE:;
        } else {
            thread_red_moves(board_froms, board_tos,
                             moving_armies, opponent_armies, to_armies,
                             nr_moves);
            BoardSubSet const& red_armies = board_tos.cat(blue_id);
            for (auto const& red_value: red_armies) {
                if (red_value == 0) continue;
                skewed = BoardSubSet::split(red_value, red_id) != 0;
                Army const& red_army = to_armies.at(red_id);
                red_id = moved_armies.find(red_army);
                if (red_id == 0) continue;
                if (moved_boards.find(blue_id, red_id, skewed ? -1 : 0)) {
                    auto red_army_symmetric = red_army.symmetric();
                    red_symmetry = red_army == red_army_symmetric;
                    --board_pos;
                    if (blue_symmetry && !red_symmetry) {
                        // If the opponent is symmetric we lost
                        // track of the oreientation and can
                        // only recover by trying both
                        try {
                            boards[board_pos] = Board{
                                boards[board_pos+1].blue(),
                                red_army};
                            FullMove{boards[board_pos+1], boards[board_pos], RED};
                            opponent_flipped = false;
                        } catch(exception& e) {
                            try {
                                boards[board_pos] = Board{
                                    boards[board_pos+1].blue(),
                                    red_army_symmetric};
                                FullMove{boards[board_pos+1], boards[board_pos], RED};
                                opponent_flipped = true;
                            } catch(exception& e) {
                                throw(logic_error("No orientation works"));
                            }
                        }
                    } else {
                        opponent_flipped ^= skewed;
                        boards[board_pos] = Board{
                            boards[board_pos+1].blue(),
                            opponent_flipped ? red_army_symmetric : red_army};
                    }
                    if (false) {
                        cout << "Found red:\n" << Image{red_army, RED} << boards[board_pos] << "skewed=" << skewed << "\n";
                        FullMove{boards[board_pos+1], boards[board_pos], RED};
                    }
                    goto RED_DONE;
                }
            }
            throw(logic_error("Blue backtrack failure"));
          RED_DONE:;
        }
    }
    for (size_t i = 0; i < boards.size(); ++i)
        cout << "Move " << i << "\n" << boards[i];

    Svg svg;
    svg.html_header(boards.size()-1);
    svg.game(boards);
    svg.html_footer();
    string const svg_file = Svg::solution_file();
    string const svg_file_tmp = svg_file + "." + HOSTNAME + "." + to_string(getpid()) + ".new";
    ofstream svg_fh;
    svg_fh.exceptions(ofstream::failbit | ofstream::badbit);
    try {
        svg_fh.open(svg_file_tmp, ofstream::out);
        svg_fh << svg;
        svg_fh.close();
        int rc = rename(svg_file_tmp.c_str(), svg_file.c_str());
        if (rc)
            throw(system_error(errno, system_category(),
                               "Could not rename '" + svg_file_tmp + "' to '" +
                               svg_file + "'"));
    } catch(exception& e) {
        unlink(svg_file_tmp.c_str());
        throw;
    }
}

void system_properties() {
    char hostname[100];
    hostname[sizeof(hostname)-1] = 0;
    int rc = gethostname(hostname, sizeof(hostname)-1);
    if (rc) throw(system_error(errno, system_category(),
                               "Could not determine host name"));
    HOSTNAME.assign(hostname);

    long tmp = sysconf(_SC_PAGE_SIZE);
    if (tmp == -1)
        throw(system_error(errno, system_category(),
                           "Could not determine PAGE SIZE"));
    PAGE_SIZE = tmp;
}

void my_main(int argc, char const* const* argv) {
    GetOpt options("b:B:t:p", argv);
    long long int val;
    bool replay = false;
    while (options.next())
        switch (options.option()) {
            case 'b': balance       = atoi(options.arg()); break;
            case 'B': balance_delay = atoi(options.arg()); break;
            case 'p': replay = true; break;
            case 't':
              val = atoll(options.arg());
              if (val < 0)
                  throw(range_error("Number of threads cannot be negative"));
              if (val > THREADS_MAX)
                  throw(range_error("Too many threads"));
              nr_threads = val;
              break;
            default:
              cerr << "usage: " << argv[0] << " [-t threads] [-b balance] [-B balance_delay]\n";
              exit(EXIT_FAILURE);
        }
    balance_min = ARMY     / 4 - balance;
    balance_max = (ARMY+3) / 4 + balance;
    if (nr_threads == 0) nr_threads = thread::hardware_concurrency();

    auto start_board = tables.start();
    if (false) {
        cout << "Infinity: " << static_cast<uint>(tables.infinity()) << "\n";
        tables.print_moves();
        cout << "Red:\n";
        cout << start_board.red();
        cout << "Blue:\n";
        cout << start_board.blue();
        cout << "Base red distance:\n";
        tables.print_distance_base_red();
        cout << "Base red:\n";
        tables.print_base_red();
        cout << "Edge red:\n";
        tables.print_edge_red();
        cout << "Parity:\n";
        tables.print_parity();
        cout << "Symmetric:\n";
        tables.print_symmetric();
        cout << "Red Base parity count:\n";
        tables.print_parity_count();
    }

    int needed_moves = tables.min_moves();
    cout << "Minimum possible number of moves: " << needed_moves << "\n";
    int nr_moves = needed_moves;
    auto arg = options.next_arg();
    if (arg) {
        nr_moves = atoi(arg);
        if (nr_moves < needed_moves) {
            if (nr_moves <= 0)
                throw(range_error("Number of moves must be positive"));
            cout << "No solution in " << nr_moves << " moves\n";
            return;
        }
    }

    get_memory(true);

    if (replay) {
        play();
        return;
    }

    Army red_army;
    if (!solve(start_board, nr_moves, red_army)) return;
    backtrack(start_board, nr_moves, red_army);
}

int main(int argc, char const* const* argv) {
    try {
        system_properties();

        my_main(argc, argv);
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
