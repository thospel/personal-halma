#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <streambuf>
#include <thread>
#include <vector>

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

#define CAT(x, y) _CAT(x,y)
#define _CAT(x, y)	x ## y

#define STRINGIFY(x) _STRINGIFY(x)
#define _STRINGIFY(x) #x

using namespace std;

bool const STATISTICS = true;
bool const SET_STATISTICS = false;
bool const SYMMETRY = true;
bool const MEMCHECK = false;
bool const CLOSED_LOOP = false;
bool const PASS = false;
// For the moment it is always allowed to jump the same man multiple times
// bool const DOUBLE_CROSS = true;
bool const BALANCE  = true;

#define CHECK   0
#define VERBOSE	0
bool const LOCK_DEBUG = false;

int const X = 9;
int const Y = 9;
int const RULES = 6;
int const ARMY = 10;

// ARMY < 32
using BalanceMask = uint32_t;
int const BALANCE_BITS = std::numeric_limits<BalanceMask>::digits;
BalanceMask const BALANCE_FULL = ~ static_cast<BalanceMask>(0);
constexpr BalanceMask make_balance_mask(int min, int max) {
    return
        min < 0 ? make_balance_mask(0, max) :
        max >= BALANCE_BITS ? make_balance_mask(min, BALANCE_BITS-1) :
        min > max ? 0 :
        BALANCE_FULL << min & BALANCE_FULL >> (BALANCE_BITS-1 - max);
}
extern int balance;
extern int balance_delay;
extern int balance_min, balance_max;

extern bool prune_slide;
extern bool prune_jump;
extern bool example;

// There is no fundamental limit. Just make up *SOME* bound
uint const THREADS_MAX = 256;
extern uint nr_threads;
extern thread_local uint tid;
extern atomic<uint> tids;

const char letters[] = "abcdefghijklmnopqrstuvwxyz";

uint64_t const SEED = 123456789;

using Sec      = chrono::seconds;

extern string HOSTNAME;

uint64_t const murmur_multiplier = UINT64_C(0xc6a4a7935bd1e995);

inline uint64_t murmur_mix(uint64_t v) FUNCTIONAL;
uint64_t murmur_mix(uint64_t v) {
    v *= murmur_multiplier;
    return v ^ (v >> 47);
}

inline uint64_t hash64(uint64_t v) FUNCTIONAL;
uint64_t hash64(uint64_t v) {
    return murmur_mix(murmur_mix(v));
    // return murmur_mix(murmur_mix(murmur_mix(v)));
    // return XXHash64::hash(reinterpret_cast<void const *>(&v), sizeof(v), SEED);
}

class LogBuffer: public std::streambuf {
  public:
    size_t const BLOCK = 80;

    LogBuffer();
    ~LogBuffer() {
        sync();
    }
  protected:
    int sync();
    int overflow(int ch);
  private:
    string prefix_;
    std::vector<char> buffer_;
};

class LogStream: public std::ostream {
  public:
    LogStream(): std::ostream{&buffer_} {}
  private:
    LogBuffer buffer_;
};

extern thread_local LogStream logger;

using Norm = uint8_t;
using Nbits = uint;
int const NBITS = std::numeric_limits<Nbits>::digits;
Nbits const NLEFT = static_cast<Nbits>(1) << (NBITS-1);
enum Color : uint8_t { EMPTY, BLUE, RED, COLORS };
inline Color operator+(Color from, int value) {
    return static_cast<Color>(static_cast<int>(from) + value);
}
inline Color operator-(Color from, int value) {
    return static_cast<Color>(static_cast<int>(from) - value);
}
inline Color operator~(Color color) {
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

using Parity = uint8_t;
using ParityCount = array<int, 4>;

inline ostream& operator<<(ostream& os, ParityCount const& parity_count) {
    os << "[ ";
    for (auto p: parity_count) os << p << " ";
    os << "]";
    return os;
}

inline int min_slides(ParityCount const& parity_count) PURE;
int min_slides(ParityCount const& parity_count) {
    int slides = 0;
    for (auto pc: parity_count) slides += max(pc, 0);
    return slides;
}

template<class T>
class Pair {
  public:
    inline Pair() {}
    inline Pair(T const& normal, T const& symmetric):
        normal_{normal}, symmetric_{symmetric} {}
    inline Pair(T const& normal) : Pair{normal, normal.symmetric()} {}
    inline T const& normal() const FUNCTIONAL {
        return normal_;
    }
    inline T const& symmetric() const FUNCTIONAL {
        return symmetric_;
    }
    template<class X>
    void fill(X const& value) {
        std::fill(normal_.begin(), normal_.end(), value);
        std::fill(symmetric_.begin(), symmetric_.end(), value);
    }
    inline T& _normal() FUNCTIONAL {
        return normal_;
    }
    inline T& _symmetric() FUNCTIONAL {
        return symmetric_;
    }
  private:
    T normal_, symmetric_;
};

class Coord;
using CoordPair  = Pair<Coord>;
using ParityPair = Pair<Parity>;

class CoordZ {
  public:
    using value_type = uint8_t;

    static int const SIZE = X*Y;
    static inline CoordZ MIN();
    static inline CoordZ MAX();

    inline CoordZ() {}
    inline CoordZ(int x, int y): pos_{static_cast<value_type>(y*X+x)} {}
    explicit inline CoordZ(Coord const& pos);
    inline CoordPair const& coord_pair() const PURE;
    // Mirror over SW-NE diagonal
    CoordZ mirror() const PURE { return CoordZ{static_cast<value_type>((SIZE-1) - pos_)}; }
    // Mirror over NW-SE diagonal
    inline CoordZ symmetric() const PURE;
    inline Parity parity() const PURE;
    inline uint x() const PURE { return pos_ % X; }
    inline uint y() const PURE { return pos_ / X; }
    string str() const PURE { return letters[x()] + to_string(y()+1); }
    void check(int line) const {
        int x_ = x();
        int y_ = y();
        if (x_ < 0) throw(logic_error("x negative at line " + to_string(line)));
        if (x_ >= X) throw(logic_error("x too large at line " + to_string(line)));
        if (y_ < 0) throw(logic_error("y negative at line " + to_string(line)));
        if (y_ >= Y) throw(logic_error("y too large at line " + to_string(line)));
    }

    void svg(ostream& os, Color color, uint scale) const;
    value_type _pos() const PURE { return pos_; }
  private:
    explicit inline CoordZ(value_type pos): pos_{pos} {}

    value_type pos_;

    friend inline bool operator<(CoordZ const& l, CoordZ const& r) {
        return l.pos_ < r.pos_;
    }
    friend inline bool operator>(CoordZ const& l, CoordZ const& r) {
        return l.pos_ > r.pos_;
    }
    friend inline bool operator>=(CoordZ const& l, CoordZ const& r) {
        return l.pos_ >= r.pos_;
    }
    friend inline bool operator==(CoordZ const& l, CoordZ const& r) {
        return l.pos_ == r.pos_;
    }
    friend inline bool operator!=(CoordZ const& l, CoordZ const& r) {
        return l.pos_ != r.pos_;
    }
};

CoordZ CoordZ::MIN() {
    return CoordZ{std::numeric_limits<value_type>::min()};
}
CoordZ CoordZ::MAX() {
    return CoordZ{std::numeric_limits<value_type>::max()};
}

inline ostream& operator<<(ostream& os, CoordZ const& pos) {
    os << setw(2) << pos.x() << "," << setw(3) << pos.y();
    return os;
}

template<class T>
class BoardZTable {
  public:
    T&       operator[](CoordZ const& pos)       PURE { return data_[pos._pos()];}
    T const& operator[](CoordZ const& pos) const PURE { return data_[pos._pos()];}
    void fill(T value) { std::fill(data_.begin(), data_.end(), value); }
  private:
    array<T, CoordZ::SIZE> data_;
};

using Diff = Coord;
using Rules = array<Diff, RULES>;
using CoordVal = int16_t;
class Coord {
  private:
    static int const ROW  = 2*X;
  public:
    static int const SIZE = (Y+1)*ROW+X+2;
    static int const MAX  = (Y-1)*ROW+X-1;

    static inline Rules const& directions();

    Coord() {}
    explicit inline Coord(CoordZ const& pos);
    Coord(Coord const& from, Diff const& diff) : pos_{static_cast<CoordVal>(from.pos_ + diff.pos_)} {}
    Coord(int x, int y) : pos_{static_cast<CoordVal>(y*ROW+x)} { }
    int x() const PURE { return (pos_+(X+(Y-1)*ROW)) % ROW - X; }
    int y() const PURE { return (pos_+(X+(Y-1)*ROW)) / ROW - (Y-1); }
    inline Parity parity() const PURE;
    inline uint8_t base_blue() const PURE;
    inline uint8_t base_red() const PURE;
    inline uint8_t edge_red() const PURE;
    inline Norm distance_base_red() const PURE;
    inline Nbits Ndistance_base_red() const PURE;

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
    // Mirror over NW-SE diagonal
    inline Coord symmetric() const PURE;

    friend inline bool operator<(Coord const& l, Coord const& r) {
        return l.pos_ < r.pos_;
    }
    friend inline bool operator==(Coord const& l, Coord const& r) {
        return l.pos_ == r.pos_;
    }
    friend inline bool operator>=(Coord const& l, Coord const& r) {
        return l.pos_ >= r.pos_;
    }
  private:
    static uint const OFFSET = ROW+1;
    CoordVal pos_;
};

inline ostream& operator<<(ostream& os, Coord const& pos) {
    os << setw(2) << static_cast<int>(pos.x()) << "," << setw(3) << static_cast<int>(pos.y());
    return os;
}

class ArmyZE;
// ArmyZ as a set of CoordZ
class ArmyZ: public array<CoordZ, ARMY> {
  public:
    ArmyZ() {}
    uint64_t hash() const PURE {
        return XXHash64::hash(reinterpret_cast<void const*>(&(*this)[0]), sizeof((*this)[0]) * ARMY, SEED);
    }
    NOINLINE ArmyZ symmetric() const PURE {
        ArmyZ result;
        transform(begin(), end(), result.begin(), [](CoordZ const& pos){ return pos.symmetric(); });
        sort(result.begin(), result.end());
        return result;
    }

    NOINLINE void check(int line) const;
    inline int symmetry() const;
    ArmyZ& operator=(ArmyZ const& army) {
        std::copy(army.begin(), army.end(), begin());
        return *this;
    }
    inline ArmyZ& operator=(ArmyZE const& army);

    friend bool operator==(ArmyZ const& l, ArmyZ const& r) {
        for (int i=0; i<ARMY; ++i)
            if (l[i] != r[i]) return false;
        return true;
    }
};

ostream& operator<<(ostream& os, ArmyZ const& army);

inline int cmp(ArmyZ const& left, ArmyZ const& right) {
    if (!SYMMETRY || X != Y) return 0;
    return memcmp(reinterpret_cast<void const *>(&left[0]),
                  reinterpret_cast<void const *>(&right[0]),
                  sizeof(left[0]) * ARMY);
}

int ArmyZ::symmetry() const {
    if (!SYMMETRY || X != Y) return 0;
    BoardZTable<uint8_t> test;
    for (auto const& pos: *this)
        test[pos] = 0;
    for (auto const& pos: *this)
        test[pos.symmetric()] = 1;
    uint8_t sum = ARMY;
    for (auto const& pos: *this)
        sum -= test[pos];
    return sum ? 1 : 0;
}

// ArmyZ as a set of CoordZ
class ArmyZE: public array<CoordZ, ARMY+2> {
  public:
    ArmyZE() {
        at(-1)   = CoordZ::MIN();
        at(ARMY) = CoordZ::MAX();
    }
    explicit ArmyZE(ArmyZ const& army) : ArmyZE{} { *this = army; }
    // Coord operator[](ssize_t) = delete;
    CoordZ& at(int i) { return (*this)[i+1]; }
    CoordZ const& at(int i) const FUNCTIONAL { return (*this)[i+1]; }
    uint64_t hash() const PURE {
        return XXHash64::hash(reinterpret_cast<void const*>(&at(0)), sizeof((*this)[0]) * ARMY, SEED);
    }
    NOINLINE void check(int line) const;
    ArmyZE& operator=(ArmyZE const& army) {
        std::copy(army.begin(), army.end(), begin());
        return *this;
    }
    ArmyZE& operator=(ArmyZ const& army) {
        std::copy(army.begin(), army.end(), begin());
        return *this;
    }
    CoordZ* begin() FUNCTIONAL { return &at(0); }
    CoordZ* end  () FUNCTIONAL { return &at(ARMY); }
    CoordZ const* begin() const FUNCTIONAL { return &at(0); }
    CoordZ const* end  () const FUNCTIONAL { return &at(ARMY); }
};

ArmyZ& ArmyZ::operator=(ArmyZE const& army) {
    std::copy(army.begin(), army.end(), begin());
    return *this;
}

inline int cmp(ArmyZE const& left, ArmyZE const& right) {
    if (!SYMMETRY || X != Y) return 0;
    return memcmp(reinterpret_cast<void const *>(&left.at(0)),
                  reinterpret_cast<void const *>(&right.at(0)),
                  sizeof(left[0]) * ARMY);
}

inline bool operator==(ArmyZE const& l, ArmyZ const& r) {
    for (int i=0; i<ARMY; ++i)
        if (l.at(i) != r[i]) return false;
    return true;
}

inline bool operator==(ArmyZ const& l, ArmyZE const& r) {
    for (int i=0; i<ARMY; ++i)
        if (l[i] != r.at(i)) return false;
    return true;
}

ostream& operator<<(ostream& os, ArmyZE const& army);

class ArmyZPos: public ArmyZE {
  public:
    void copy(ArmyZ const& army, int pos) {
        std::copy(army.begin(), army.end(), begin());
        pos_ = pos;
    }
    void store(CoordZ const& val) {
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

class Army: public array<Coord, ARMY> {
  public:
    Army() {}
    explicit inline Army(ArmyZ const& army): Army{} {
        for (int i=0; i<ARMY; ++i)
            (*this)[i] = Coord{army[i]};
    }
    NOINLINE void check(int line) const;
};

ostream& operator<<(ostream& os, Army const& army);

class ArmyPair {
  public:
    NOINLINE explicit ArmyPair(ArmyZ const& army);
    inline ArmyPair(ArmyZ const& army, ArmyZ const& army_symmetric) :
        normal_{army},
        symmetric_{army_symmetric} { }
    Army const& normal()    const FUNCTIONAL { return normal_; }
    Army const& symmetric() const FUNCTIONAL { return symmetric_; }
    inline void check(int line) const {
        normal()   .check(line);
        symmetric().check(line);
    }
    int symmetry() const {
        if (!SYMMETRY || X != Y) return 0;
        return normal_ == symmetric_ ? 0 : 1;
    }
  private:
    Army normal_, symmetric_;
};

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
  private:
    array<T, Coord::MAX+1> data_;
};

class ArmyMapper {
    friend class ArmyMapperPair;
  public:
    ArmyMapper(ArmyZ const& army_symmetric) {
        for (uint i=0; i<ARMY; ++i)
            mapper_[Coord{army_symmetric[i].symmetric()}] = i;
    }
    ArmyMapper(Army const& army_symmetric) {
        for (uint i=0; i<ARMY; ++i)
            mapper_[army_symmetric[i].symmetric()] = i;
    }
    uint8_t map(Coord const& pos) const PURE {
        return mapper_[pos];
    }
  private:
    ArmyMapper() {}
    BoardTable<uint8_t> mapper_;
};

class ArmyMapperPair {
  public:
    explicit inline ArmyMapperPair(ArmyPair const& army_pair):
        normal_{army_pair.symmetric()},
        symmetric_{army_pair.normal()} {}
    ArmyMapper const& normal()    const FUNCTIONAL { return normal_; }
    ArmyMapper const& symmetric() const FUNCTIONAL { return symmetric_; }
  private:
    ArmyMapper normal_, symmetric_;
};

class Statistics {
  public:
    using Counter = uint64_t;
    Statistics() {
        clear();
    }
    void clear() {
        late_prunes_ = 0;
        boardset_tries_ = 0;
    }
    inline void late_prune()   { if (STATISTICS) ++late_prunes_; }
    inline void boardset_try() { if (STATISTICS) ++boardset_tries_; }
    Counter late_prunes() const PURE { return late_prunes_; }
    Counter boardset_tries() const PURE { return boardset_tries_; }

    Statistics& operator+=(Statistics const& statistics) {
        if (STATISTICS) {
            late_prunes_    += statistics.late_prunes();
            boardset_tries_ += statistics.boardset_tries();
        }
        return *this;
    }

  private:
    Counter late_prunes_;
    Counter boardset_tries_;
};

class SetStatistics {
  public:
    SetStatistics(): hits_{0}, misses_{0}, tries_{0} {}
    inline void stats_update(uint64_t offset) {
        if (!SET_STATISTICS) return;
        if (offset) {
            ++hits_;
            tries_ += offset;
        } else
            ++misses_;
    }
    void stats_reset() {
        if (!SET_STATISTICS) return;
        misses_ = hits_ = tries_ = 0;
    }
    NOINLINE void show_stats(ostream& os) const;
    void show_stats() const {
        show_stats(cout);
    }

  private:
    atomic<uint64_t> hits_;
    atomic<uint64_t> misses_;
    atomic<uint64_t> tries_;
};

using ArmyId = uint32_t;
STATIC const int ARMY_BITS = std::numeric_limits<ArmyId>::digits;
STATIC const ArmyId ARMY_HIGHBIT = static_cast<ArmyId>(1) << (ARMY_BITS-1);
STATIC const ArmyId ARMY_MASK = ARMY_HIGHBIT-1;

class ArmyZSet: public SetStatistics {
  public:
    static ArmyId const INITIAL_SIZE = 32;
    ArmyZSet(ArmyId size = INITIAL_SIZE);
    ~ArmyZSet();
    void clear(ArmyId size = INITIAL_SIZE);
    void drop_hash() {
        delete [] values_;
        // logger << "Drop hash values " << static_cast<void const *>(values_) << "\n" << flush;
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
    ArmyZ const& at(ArmyId i) const {
        if (i >= used1_) throw(logic_error("ArmyZ id " + to_string(i) + " out of range of set"));
        return armies_[i];
    }
#else  // CHECK
    ArmyZ const& at(ArmyId i) const PURE { return armies_[i]; }
#endif // CHECK
    inline ArmyId insert(ArmyZ  const& value);
    inline ArmyId insert(ArmyZE const& value);
    ArmyId find(ArmyZ  const& value) const PURE;
    ArmyId find(ArmyZE const& value) const PURE;
    ArmyZ const* begin() const PURE { return &armies_[1]; }
    ArmyZ const* end()   const PURE { return &armies_[used1_]; }

    void print(ostream& os) const;
    // Non copyable
    ArmyZSet(ArmyZSet const&) = delete;
    ArmyZSet& operator=(ArmyZSet const&) = delete;

  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.7*factor); }
    NOINLINE void resize();

    mutex exclude_;
    ArmyId size_;
    ArmyId mask_;
    ArmyId used1_;
    ArmyId limit_;
    ArmyId* values_;
    ArmyZ* armies_;
};

inline ostream& operator<<(ostream& os, ArmyZSet const& set) {
    set.print(os);
    return os;
}

struct Move {
    Move() {}
    Move(CoordZ const& from_, CoordZ const& to_): from{from_}, to{to_} {}
    Move(ArmyZ const& army_from, ArmyZ const& army_to);
    Move(ArmyZ const& army_from, ArmyZ const& army_to, int& diff);

    Move mirror() const PURE {
        return Move{from.mirror(), to.mirror()};
    }

    CoordZ from, to;
};

// Board as two Armies
class FullMove;
class Image;
class Board {
  public:
    Board() {}
    Board(ArmyZ const& blue, ArmyZ const& red): blue_{blue}, red_{red} {}
    void do_move(Move const& move_);
    void do_move(Move const& move_, bool blue_to_move);
    inline void do_move(FullMove const& move_);
    inline void do_move(FullMove const& move_, bool blue_to_move);
    ArmyZ& blue() { return blue_; }
    ArmyZ& red()  { return red_; }
    ArmyZ const& blue() const FUNCTIONAL { return blue_; }
    ArmyZ const& red()  const FUNCTIONAL { return red_; }
    inline void check(int line) const {
        blue_.check(line);
        red_ .check(line);
    }
    int min_nr_moves(bool blue_to_move) const PURE;
    int min_nr_moves() const PURE {
        return min(min_nr_moves(true), min_nr_moves(false));
    }
    void svg(ostream& os, uint scale, uint marging) const;

  private:
    ArmyZ blue_, red_;

    friend bool operator==(Board const& l, Board const& r) {
        return l.blue() == r.blue() && l.red() == r.red();
    }
};

class BoardSubSet {
  public:
    ArmyId allocated() const PURE { return mask_+1; }
    ArmyId capacity()  const PURE { return FACTOR(allocated()); }
    ArmyId size()      const PURE { return capacity() - left_; }
    bool empty() const PURE { return size() == 0; }
    void zero() {
        armies_ = nullptr;
        mask_ = 0;
        left_ = 0;
    }
    void create(ArmyId size = 1);
    void destroy() {
        // cout << "Destroy BoardSubSet " << static_cast<void const*>(armies_) << "\n";
        if (armies_) delete[] armies_;
    }
    ArmyId const* begin() const PURE { return &armies_[0]; }
    ArmyId const* end()   const PURE { return &armies_[allocated()]; }

    inline bool insert(ArmyId red_id, int symmetry) {
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
    ArmyId example(ArmyId& symmetry) const;
    void print(ostream& os) const;
    void print() const { print(cout); }
  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.7*factor); }
    NOINLINE void resize();

    ArmyId* begin() PURE { return &armies_[0]; }
    ArmyId* end()   PURE { return &armies_[allocated()]; }

    inline bool insert(ArmyId red_id);
    bool find(ArmyId id) const PURE;

    ArmyId* armies_;
    ArmyId mask_;
    ArmyId left_;
};

bool BoardSubSet::insert(ArmyId red_id) {
    // cout << "Insert " << red_id << "\n";
    if (left_ == 0) resize();
    auto mask = mask_;
    ArmyId pos = hash64(red_id) & mask;
    uint offset = 0;
    auto armies = armies_;
    while (true) {
        // cout << "Try " << pos << " of " << mask+1 << "\n";
        auto& id = armies[pos];
        if (id == 0) {
            // stats_update(offset);
            id = red_id;
            --left_;
            // cout << "Found empty\n";
            return true;
        }
        if (id == red_id) {
            // cout << "Found duplicate\n";
            // stats_update(offset);
            return false;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

class BoardSet {
    friend class BoardSubSetRef;
  public:
    static ArmyId const INITIAL_SIZE = 32;
    BoardSet(bool keep = false, ArmyId size = INITIAL_SIZE);
    ~BoardSet() {
        for (auto& subset: *this)
            subset.destroy();
        // cout << "Destroy BoardSet " << static_cast<void const*>(subsets_) << "\n";
        delete [] subsets_;
    }
    ArmyId subsets() const PURE { return top_ - from(); }
    size_t size() const PURE { return size_; }
    bool empty() const PURE { return size() == 0; }
    void clear(ArmyId size = INITIAL_SIZE);
    BoardSubSet const&  at(ArmyId id) const PURE { return subsets_[id]; }
    BoardSubSet const& cat(ArmyId id) const PURE { return subsets_[id]; }
    BoardSubSet const* begin() const PURE { return &subsets_[from()]; }
    BoardSubSet const* end()   const PURE { return &subsets_[top_]; }
    ArmyId back_id() const PURE { return top_-1; }
    inline bool insert(ArmyId blue_id, ArmyId red_id, int symmetry) {
        if (CHECK) {
            if (blue_id <= 0)
                throw(logic_error("red_id <= 0"));
        }
        lock_guard<mutex> lock{exclude_};

        if (blue_id >= top_) {
            // Only in the multithreaded case blue_id can be different from top_
            // if (blue_id != top_) throw(logic_error("Cannot grow more than 1"));
            while (blue_id >= capacity_) resize();
            while (blue_id >= top_)
                subsets_[top_++].create();
        }
        bool result = subsets_[blue_id].insert(red_id, symmetry);
        size_ += result;
        return result;
    }
    bool insert(Board const& board, ArmyZSet& armies_blue, ArmyZSet& armies_red);
    bool insert(Board const& board, ArmyZSet& armies_to_move, ArmyZSet& armies_opponent, int nr_moves) {
        int blue_to_move = nr_moves & 1;
        return blue_to_move ?
            insert(board, armies_to_move, armies_opponent) :
            insert(board, armies_opponent, armies_to_move);
    }
    void insert(ArmyId blue_id, BoardSubSet const& subset) {
        if (CHECK) {
            if (blue_id <= 0)
                throw(logic_error("red_id <= 0"));
        }
        lock_guard<mutex> lock{exclude_};

        if (blue_id >= top_) {
            // Only in the multithreaded case blue_id can be different from top_
            // if (blue_id != top_) throw(logic_error("Cannot grow more than 1"));
            while (blue_id >= capacity_) resize();
            while (blue_id > top_)
                subsets_[top_++].zero();
            ++top_;
        }
        subsets_[blue_id] = subset;
        size_ += subset.size();
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
    bool find(Board const& board, ArmyZSet const& armies_blue, ArmyZSet const& armies_red) const PURE;
    bool find(Board const& board, ArmyZSet& armies_to_move, ArmyZSet& armies_opponent, int nr_moves) const PURE {
        int blue_to_move = nr_moves & 1;
        return blue_to_move ?
            find(board, armies_to_move, armies_opponent) :
            find(board, armies_opponent, armies_to_move);
    }
    bool solve(ArmyId solution_id, ArmyZ const& solution) {
        lock_guard<mutex> lock{exclude_};
        if (solution_id_) return false;
        solution_id_ = solution_id;
        solution_ = solution;
        return true;
    }
    ArmyId solution_id() const PURE { return solution_id_; }
    ArmyZ const& solution() const PURE { return solution_; }
    NOINLINE Board example(ArmyZSet const& opponent_armies, ArmyZSet const& moved_armies, bool blue_moved) const PURE;
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
    NOINLINE void resize() {
        auto old_subsets = subsets_;
        subsets_ = new BoardSubSet[capacity_*2];
        capacity_ *= 2;
        // logger << "Resize BoardSet " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity_ << "\n" << flush;
        copy(&old_subsets[from()], &old_subsets[top_], &subsets_[1]);
        if (!keep_) {
            top_ -= from_ - 1;
            from_ = 1;
        }
        delete [] old_subsets;
    }
    BoardSubSet* begin() PURE { return &subsets_[from()]; }
    BoardSubSet* end()   PURE { return &subsets_[top_]; }

    size_t size_;
    mutex exclude_;
    ArmyZ solution_;
    ArmyId solution_id_;
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

class Image {
  public:
    inline Image() {
        clear();
    }
    inline Image(ArmyZ const& blue, ArmyZ const& red): Image{} {
        set(blue, BLUE);
        set(red,  RED);
    }
    inline Image(Army const& blue, Army const& red): Image{} {
        set(blue, BLUE);
        set(red,  RED);
    }
    inline explicit Image(Board const& board): Image{board.blue(), board.red()} {}
    inline explicit Image(ArmyZ const& army, Color color = BLUE): Image{} {
        set(army, color);
    }
    inline explicit Image(ArmyZE const& army, Color color = BLUE): Image{} {
        set(army, color);
    }
    inline explicit Image(Army const& army, Color color = BLUE): Image{} {
        set(army, color);
    }
    inline void clear();
    inline Color get(Coord const& pos) const PURE { return image_[pos.index()]; }
    inline Color get(int x, int y) const PURE { return get(Coord{x,y}); }
    inline void  set(Coord const& pos, Color c) { image_[pos.index()] = c; }
    inline void  set(CoordZ const& pos, Color c) { image_[Coord{pos}.index()] = c; }
    inline void  set(int x, int y, Color c) { set(Coord{x,y}, c); }
    inline void  set(ArmyZ const& army, Color c) {
        for (auto const& pos: army)
            set(pos, c);
    }
    inline void  set(ArmyZE const& army, Color c) {
        for (auto const& pos: army)
            set(pos, c);
    }
    inline void  set(Army const& army, Color c) {
        for (auto const& pos: army)
            set(pos, c);
    }
    NOINLINE string str() const PURE;
    Image& operator=(Image const& image) {
        std::copy(image.begin(), image.end(), begin());
        return *this;
    }
  private:
    Color* begin() FUNCTIONAL { return &image_[0]; }
    Color* end  () FUNCTIONAL { return &image_[0]; }
    Color const* begin() const FUNCTIONAL { return &image_[Coord::SIZE]; }
    Color const* end  () const FUNCTIONAL { return &image_[Coord::SIZE]; }

    array<Color, Coord::SIZE> image_;
};

inline ostream& operator<<(ostream& os, Image const& image) {
    os << image.str();
    return os;
}

inline ostream& operator<<(ostream& os, Board const& board) {
    os << Image{board};
    return os;
}

class FullMove: public vector<CoordZ> {
  public:
    FullMove() {}
    FullMove(char const* str);
    FullMove(string const& str) : FullMove{str.c_str()} {}
    FullMove(Board const& from, Board const& to, Color color=COLORS);
    string str() const PURE;
    CoordZ from() const PURE;
    CoordZ to()   const PURE;
    Move move() const PURE;

  private:
    void move_expand(Board const& board_from, Board const& board_to, Move const& move);
};

inline ostream& operator<<(ostream& os, FullMove const& move) {
    os << move.str();
    return os;
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
        return string("solutions/halma-X") + to_string(X) + "Y" + to_string(Y) + "Army" + to_string(ARMY) + "Rule" + to_string(RULES) + ".html";
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

class Tables {
  public:
    Tables();
    uint min_nr_moves() const PURE { return min_nr_moves_; }
    inline Norm norm(Coord const& left, Coord const& right) const PURE {
        return norm_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Norm distance(Coord const& left, Coord const& right) const PURE {
        return distance_[right.pos() - left.pos() + Coord::MAX];
    }
    inline Nbits Ndistance(Coord const& left, Coord const& right) const PURE {
        return NLEFT >> distance(left, right);
    }
    inline Norm distance_base_red(Coord const& pos) const PURE {
        return distance_base_red_[pos];
    }
    inline Nbits Ndistance_base_red(Coord const& pos) const PURE {
        return NLEFT >> distance_base_red(pos);
    }
    inline uint8_t base_blue(Coord const& pos) const PURE {
        return base_blue_[pos];
    }
    inline uint8_t base_red(Coord const& pos) const PURE {
        return base_red_[pos];
    }
    inline uint8_t edge_red(Coord const& pos) const PURE {
        return edge_red_[pos];
    }
    inline ParityPair const& parity_pair(Coord const& pos) const PURE {
        return parity_pair_[pos];
    }
    inline Parity parity(Coord const& pos) const PURE {
        return parity_pair(pos).normal();
    }
    inline Parity parity_symmetric(Coord const& pos) const PURE {
        return parity_pair(pos).symmetric();
    }
    inline ParityPair const& parity_pair(CoordZ const& pos) const PURE {
        return parity_pairZ_[pos];
    }
    inline Parity parity(CoordZ const& pos) const PURE {
        return parity_pair(pos).normal();
    }
    inline Parity parity_symmetric(CoordZ const& pos) const PURE {
        return parity_pair(pos).symmetric();
    }
    inline Coord symmetric(Coord const& pos) const PURE {
        return symmetric_[pos];
    }
    inline CoordZ symmetric(CoordZ const& pos) const PURE {
        return symmetricZ_[pos];
    }
    inline CoordZ coord(Coord const& pos) const PURE {
        return coordZ_[pos];
    }
    inline CoordPair const& coord_pair(CoordZ const& pos) const PURE {
        return coord_pair_[pos];
    }
    inline Coord coord(CoordZ const& pos) const PURE {
        return coord_pair(pos).normal();
    }
    // The folowing methods in Tables really only PURE. However they are only
    // ever applied to the constant tables so the access global memory that
    // never changes making them effectively FUNCTIONAL
    inline ParityCount const& parity_count() const FUNCTIONAL {
        return parity_count_;
    }
    Norm infinity() const FUNCTIONAL { return infinity_; }
    inline Rules const& directions() const FUNCTIONAL { return directions_; }
    inline Army const& army_red()  const FUNCTIONAL { return army_red_; }
    Board const& start() const FUNCTIONAL { return start_; }
    Image const& start_image() const FUNCTIONAL { return start_image_; }

    void print_directions(ostream& os) const;
    void print_directions() const {
        print_directions(cout);
    }
    void print_distance_base_red(ostream& os) const;
    inline void print_distance_base_red() const {
        print_distance_base_red(cout);
    }
    void print_base_blue(ostream& os) const;
    void print_base_blue() const {
        print_base_blue(cout);
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
    void print_parity_symmetric(ostream& os) const;
    void print_parity_symmetric() const {
        print_parity_symmetric(cout);
    }
    void print_symmetric(ostream& os) const;
    void print_symmetric() const {
        print_symmetric(cout);
    }
    void print_blue_parity_count(ostream& os) const;
    void print_blue_parity_count() const {
        print_blue_parity_count(cout);
    }
    void print_red_parity_count(ostream& os) const;
    void print_red_parity_count() const {
        print_red_parity_count(cout);
    }
  private:
    ParityCount parity_count_;
    uint min_nr_moves_;
    Rules directions_;
    Norm infinity_;
    BoardTable<Coord> symmetric_;
    array<Norm, 2*Coord::MAX+1> norm_;
    array<Norm, 2*Coord::MAX+1> distance_;
    BoardTable<Norm> distance_base_red_;
    BoardTable<uint8_t> base_blue_;
    BoardTable<uint8_t> base_red_;
    BoardTable<uint8_t> edge_red_;
    BoardTable <ParityPair> parity_pair_;
    BoardZTable<ParityPair> parity_pairZ_;
    BoardTable<CoordZ> coordZ_;
    BoardZTable<CoordZ> symmetricZ_;
    BoardZTable<CoordPair> coord_pair_;
    Army army_red_;
    Board start_;
    Image start_image_;
};

extern Tables const tables;

CoordZ CoordZ::symmetric() const {
    return tables.symmetric(*this);
}

CoordPair const& CoordZ::coord_pair() const {
    return tables.coord_pair(*this);
}

CoordZ::CoordZ(Coord const& pos) : CoordZ{tables.coord(pos)} {}
Coord::Coord (CoordZ const& pos) : Coord {tables.coord(pos)} {}

Rules const& Coord::directions() {
    return tables.directions();
}

Coord Coord::symmetric() const {
    return tables.symmetric(*this);
}

Parity CoordZ::parity() const {
    return tables.parity(*this);
}

Parity Coord::parity() const {
    return tables.parity(*this);
}

uint8_t Coord::base_blue() const {
    return tables.base_blue(*this);
}

uint8_t Coord::base_red() const {
    return tables.base_red(*this);
}

uint8_t Coord::edge_red() const {
    return tables.edge_red(*this);
}

Norm Coord::distance_base_red() const {
    return tables.distance_base_red(*this);
}

Nbits Coord::Ndistance_base_red() const {
    return tables.Ndistance_base_red(*this);
}

ArmyId ArmyZSet::insert(ArmyZ  const& value) {
    // cout << "Insert:\n" << Image{value};
    // Leave hash calculation out of the mutex
    ArmyId hash = value.hash();
    lock_guard<mutex> lock{exclude_};
    // logger << "used1_ = " << used1_ << ", limit = " << limit_ << "\n" << flush;
    if (used1_ > limit_) resize();
    ArmyId const mask = mask_;
    ArmyId pos = hash & mask;
    ArmyId offset = 0;
    auto values = values_;
    while (true) {
        // logger << "Try " << pos << " of " << size_ << "\n" << flush;
        ArmyId i = values[pos];
        if (i == 0) {
            ArmyId id = used1_++;
            // logger << "Found empty, assign id " << id << "\n" + Image{value}.str() << flush;
            values[pos] = id;
            armies_[id] = value;
            stats_update(offset);
            return id;
        }
        if (armies_[i] == value) {
            stats_update(offset);
            // cout << "Found duplicate " << hash << "\n";
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

ArmyId ArmyZSet::insert(ArmyZE const& value) {
    // cout << "Insert:\n" << Image{value};
    // Leave hash calculation out of the mutex
    ArmyId hash = value.hash();
    lock_guard<mutex> lock{exclude_};
    // logger << "used1_ = " << used1_ << ", limit = " << limit_ << "\n" << flush;
    if (used1_ > limit_) resize();
    ArmyId const mask = mask_;
    ArmyId pos = hash & mask;
    ArmyId offset = 0;
    auto values = values_;
    while (true) {
        // logger << "Try " << pos << " of " << size_ << "\n" << flush;
        ArmyId i = values[pos];
        if (i == 0) {
            ArmyId id = used1_++;
            // logger << "Found empty, assign id " << id << "\n" + Image{value}.str() << flush;
            values[pos] = id;
            armies_[id] = value;
            stats_update(offset);
            return id;
        }
        if (armies_[i] == value) {
            stats_update(offset);
            // cout << "Found duplicate " << hash << "\n";
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

void Image::clear() {
    image_ = tables.start_image().image_;
}

void Board::do_move(FullMove const& move_, bool blue_to_move) {
    do_move(move_.move(), blue_to_move);
}

void Board::do_move(FullMove const& move_) {
    do_move(move_.move());
}

extern string get_memory(bool set_base_mem = false);
extern void make_all_moves
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmyZSet const& moving_armies,
 ArmyZSet const& opponent_armies,
 ArmyZSet& moved_armies,
 int nr_moves);
extern void make_all_moves_backtrack
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmyZSet const& moving_armies,
 ArmyZSet const& opponent_armies,
 ArmyZSet& moved_armies,
 int solution_moves,
 BoardTable<uint8_t> const& red_backtrack,
 BoardTable<uint8_t> const& red_backtrack_symmetric,
 int nr_moves);