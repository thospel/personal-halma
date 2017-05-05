#include <cstdio>
#include <cstdlib>

#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

#include "xxhash64.h"

#include "system.hpp"

using namespace std;

#if __AVX__
# define M256 1
#elif __SSE2__
# define M128 1
#endif

#if   M256
using Align = __m256i;
#elif M128
using Align = __m128i;
#else
using Align = uint64_t;
#endif
// Each section here must define:
// ALOAD:  Aligned load (alignment which the compiler gives to the type)
// ULOAD:  Unaligned load
// ASTORE: Aligned store (alignment which the compiler gives to the type)
// USTORE: Unaligned store
inline Align ALOAD(Align const& pos) PURE;
inline void ASTORE(Align& pos, Align val);
inline Align ULOAD(Align const& pos) PURE;
inline void USTORE(Align& pos, Align val);
inline bool IS_ZERO(Align val) FUNCTIONAL;
inline Align SHIFT_RIGHT(Align val, int imm8) FUNCTIONAL;
inline Align ZERO() FUNCTIONAL;
Align ZERO() {
    Align tmp;
    return tmp ^ tmp;
}
#if M256
// Current gcc gives only 16-byte alignment in std containers
// (But is ok on the stack),
// but we need __m256i
Align ALOAD(Align const& pos) { return _mm256_lddqu_si256(&pos); }
void ASTORE(Align& pos, Align val) { _mm256_storeu_si256(&pos, val); }
Align ULOAD(Align const& pos) { return _mm256_lddqu_si256(&pos); }
void USTORE(Align& pos, Align val) { _mm256_storeu_si256(&pos, val); }
bool IS_ZERO(Align val) { return _mm256_testz_si256(val, val); }
// Using a >> imm shifts in sign bits. We want zeros
Align SHIFT_RIGHT(Align val, int imm8) { return _mm256_srli_epi64(val, imm8); }
#elif M128
Align ALOAD(Align const& pos) { return pos; }
void ASTORE(Align& pos, Align val) { pos = val; }
Align ULOAD(Align const& pos) { return _mm_lddqu_si128(&pos); }
void USTORE(Align& pos, Align val) { _mm_storeu_si128(&pos, val); }
bool IS_ZERO(Align val) { return _mm_testz_si128(val, val); }
Align SHIFT_RIGHT(Align val, int imm8) { return _mm_srli_epi64(val, imm8); }
#else
Align ALOAD(Align const& pos) { return pos; }
void ASTORE(Align& pos, Align val) { pos = val; }
// Assumes unaligned 64-bit access is allowed on this architecture
Align ULOAD(Align const& pos) { return pos; }
void USTORE(Align& pos, Align val) { pos = val; }
bool IS_ZERO(Align val) { return !val; }
Align SHIFT_RIGHT(Align val, int imm8) { return val >> imm8; }
#endif

extern Align ARMY_MASK;
extern Align ARMY_MASK_NOT;
extern Align NIBBLE_LEFT;
extern Align NIBBLE_RIGHT;

extern bool statistics;
extern bool hash_statistics;
extern bool verbose;
extern bool testq;
extern int  testQ;

#define STATISTICS	(SLOW && UNLIKELY(statistics))
#define HASH_STATISTICS (SLOW && UNLIKELY(hash_statistics))

bool const SYMMETRY = true;
bool const CLOSED_LOOP = false;
bool const PASS = false;
// For the moment it is always allowed to jump the same man multiple times
// bool const DOUBLE_CROSS = true;
bool const BALANCE  = true;

bool const MLOCK = true;

#define CHECK   0
#define RED_BUILDER 1
#define ARMYSET_SPARSE 1

extern uint X;
extern uint Y;
extern uint RULES;
extern uint ARMY;
extern uint ARMY_ALIGNED;
extern uint ARMY_PADDING;
extern uint ARMY64_DOWN;

uint const MAX_X     = 16;
uint const MAX_Y     = 16;
uint const MAX_RULES = 8;
uint const MIN_MAX_ARMY = 21;

constexpr uint LOG2(size_t value) {
    return value <= 1 ? 0 : 1+LOG2(value / 2);
}
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
extern int example;

// There is no fundamental limit. Just make up *SOME* bound
uint const THREADS_MAX = 256;

const char letters[] = "abcdefghijklmnopqrstuvwxyz";

uint64_t const SEED = 123456789;

using Sec      = chrono::seconds;

uint64_t const murmur_multiplier = UINT64_C(0xc6a4a7935bd1e995);

inline uint64_t murmur_mix(uint64_t v) FUNCTIONAL;
uint64_t murmur_mix(uint64_t v) {
    v *= murmur_multiplier;
    return v ^ (v >> 47);
}

inline uint64_t hash64(uint64_t v) FUNCTIONAL;
uint64_t hash64(uint64_t v) {
    // return murmur_mix(murmur_mix(v));
    // return murmur_mix(murmur_mix(murmur_mix(v)));
    // return XXHash64::hash(reinterpret_cast<void const *>(&v), sizeof(v), SEED);
    return v * 7;
}

using Norm = uint8_t;
using Nbits = uint;
int const NBITS = std::numeric_limits<Nbits>::digits;
Nbits const NLEFT = static_cast<Nbits>(1) << (NBITS-1);
enum Color : uint8_t { EMPTY = 0x49, BLUE = 0x05, RED = 0x04, COLORS= 0x08 };

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

class Coords;
class Coord;
using Offsets = array<Coord, 8>;

class Coord {
    friend class Coords;
  public:
    using value_type = uint8_t;

    static int const SIZE = MAX_X*MAX_Y;
    static inline Coord MIN();
    static inline Coord MAX();
    static Align ArmyMask() PURE;
    static void print(ostream& os, Align align);
    static inline void print(Align align) {
        print(cout, align);
    }

    inline Coord() {}
    inline Coord(uint x, uint y): pos_{static_cast<value_type>(y*MAX_X+x)} {}
    inline Coord( int x,  int y): pos_{static_cast<value_type>(y*MAX_X+x)} {}
    // Mirror over NW-SE diagonal
    inline Coord symmetric() const PURE {
        // This will compile to a rol instruction on x86
        return Coord(pos_ << 4 | pos_ >> 4);
    }
    inline Parity parity() const PURE;
    inline uint8_t base_blue() const PURE;
    inline uint8_t base_red() const PURE;
    inline uint8_t edge_red() const PURE;
    inline Nbits Ndistance_base_red() const PURE;
    inline Coords slide_targets() const PURE;
    inline Coords jumpees() const PURE;
    inline Coords jump_targets() const PURE;
    inline uint8_t nr_slide_jumps_red() const PURE;
    inline Offsets const& slide_jumps_red() const PURE;

    inline uint x() const PURE { return pos_ % MAX_X; }
    inline uint y() const PURE { return pos_ / MAX_X; }
    string str() const PURE { return letters[x()] + to_string(y()+1); }
    void check(const char* file, int line) const {
        uint x_ = x();
        uint y_ = y();
        if (x_ >= X) throw_logic("x too large", file, line);
        if (y_ >= Y) throw_logic("y too large", file, line);
    }

    void svg(ostream& os, Color color, uint scale) const;
    value_type _pos() const PURE { return pos_; }
  private:
    explicit inline Coord(value_type pos): pos_{pos} {}

    value_type pos_;

    friend inline bool operator<(Coord const& l, Coord const& r) {
        return l.pos_ < r.pos_;
    }
    friend inline bool operator>(Coord const& l, Coord const& r) {
        return l.pos_ > r.pos_;
    }
    friend inline bool operator>=(Coord const& l, Coord const& r) {
        return l.pos_ >= r.pos_;
    }
    friend inline bool operator==(Coord const& l, Coord const& r) {
        return l.pos_ == r.pos_;
    }
    friend inline bool operator!=(Coord const& l, Coord const& r) {
        return l.pos_ != r.pos_;
    }
};

Coord Coord::MIN() {
    return Coord{std::numeric_limits<value_type>::min()};
}
Coord Coord::MAX() {
    return Coord{std::numeric_limits<value_type>::max()};
}

inline ostream& operator<<(ostream& os, Coord const& pos) ALWAYS_INLINE;
ostream& operator<<(ostream& os, Coord const& pos) {
    os << setw(2) << pos.x() << "," << setw(3) << pos.y();
    return os;
}

class Coords {
  public:
    inline Coord current() {
        return Coord{static_cast<Coord::value_type>(coords_)};
    }
    void next() { coords_ >>= 8; }
    void set(Offsets& targets) {
        coords_ = 0;
        for (int i=7; i>=0; --i)
            coords_ = coords_ << 8 | targets[i]._pos();
    }
  private:
    uint64_t coords_;
};

// Should be a multiple of sizeof(Coord)
uint const ALIGNSIZE = sizeof(Align) / sizeof(Coord);
bool const DO_ALIGN  = ALIGNSIZE != 1;
uint const MAX_ARMY_ALIGNED = ((MIN_MAX_ARMY+DO_ALIGN)+ALIGNSIZE-1)/ALIGNSIZE;
bool const SINGLE_ALIGN = MAX_ARMY_ALIGNED == 1;
uint const MAX_ARMY = MAX_ARMY_ALIGNED * ALIGNSIZE;

inline uint64_t army_hash(Coord const* base) {
    return XXHash64::hash(reinterpret_cast<void const*>(base),
                          sizeof(*base) * ARMY,
                          SEED);
}

using ArmyId = uint32_t;
ArmyId const SYMMETRIC = 1;

class Tables;
struct Move;
class ArmyPos;
class ArmySetDense;
class ArmySetSparse;
#if ARMYSET_SPARSE
using ArmySet = ArmySetSparse;
#else // ARMYSET_SPARSE
using ArmySet = ArmySetDense;
#endif // ARMYSET_SPARSE
class ArmyZconst;
// Army as a set of Coord
class alignas(Align) Army {
    // Allow tables to build red and blue armies
    friend Tables;
    // Allow ArmyPos to set the before and after elements
    friend ArmyPos;
  public:
    Army() {}
    inline Army(ArmySet const& armies, ArmyId id, ArmyId symmetry = 0);
    explicit inline Army(ArmyZconst army, ArmyId symmetry = 0);
    explicit inline Army(Army const& army, ArmyId symmetry = 0) {
        _import(army.begin(), begin(), symmetry);
    }

    inline uint64_t hash() const PURE {
        return army_hash(begin());
    }
    NOINLINE void check(const char* file, int line) const;
    inline int symmetry() const;
    inline Army& operator=(Army const& army) {
        _import(army.begin(), begin());
        return *this;
    }
    inline Army& operator=(ArmyPos const& army);

    void do_move(Move const& move);
    inline bool _try_move(Move const& move);

    inline Coord const& operator[](ssize_t i) const PURE { return army_[i];}
    // The end() versions aren't *really* FUNCTIONAL but ARMY will never change
    inline Coord const*cbegin() const FUNCTIONAL { return &army_[0]; }
    inline Coord const*cend  () const FUNCTIONAL { return &army_[ARMY]; }
    inline Coord const* begin() const FUNCTIONAL { return &army_[0]; }
    inline Coord const* end  () const FUNCTIONAL { return &army_[ARMY]; }

    friend bool operator==(Army const& l, Army const& r) {
        Coord const* lb = l.begin();
        Coord const* rb = r.begin();
        if (DO_ALIGN) {
            Align const* RESTRICT left  = reinterpret_cast<Align const*>(lb);
            Align const* RESTRICT right = reinterpret_cast<Align const*>(rb);
            uint n = ALIGNEDS();
            Align accu = ZERO();
            for (uint i=0; i<n; ++i)
                accu |= left[i] ^ right[i];
            return IS_ZERO(accu);
        }
        uint64_t const* RESTRICT left  = reinterpret_cast<uint64_t const*>(lb);
        uint64_t const* RESTRICT right = reinterpret_cast<uint64_t const*>(rb);
        uint n = ARMY64_DOWN;
        uint i;
        for (i=0; i<n; ++i)
            if (left[i] != right[i]) return false;
        for (i *= sizeof(*left); i<ARMY; ++i)
            if (lb[i] != rb[i]) return false;
        return true;
    }
    friend bool operator!=(Army const& l, Army const& r) {
        return !operator==(l, r);
    }
    static inline int ALIGNEDS() FUNCTIONAL {
        return SINGLE_ALIGN ? MAX_ARMY_ALIGNED : ARMY_ALIGNED;
    }
  private:
    // Really only PURE but the value never changes
    static NOINLINE void sort(Coord* RESTRICT base);
    static inline void _import(Coord const* RESTRICT from, Coord* RESTRICT to, ArmyId symmetry = 0, bool terminate=false) {
        if (DO_ALIGN) {
            Align const* RESTRICT afrom = reinterpret_cast<Align const*>(from);
            Align      * RESTRICT ato   = reinterpret_cast<Align      *>(to);
            uint n = ALIGNEDS();
            if (terminate) {
                --n;
                if (symmetry) {
                    for (uint i=0; i<n; ++i) {
                        Align tmp = ULOAD(afrom[i]);
                        Align left  = tmp & NIBBLE_LEFT;
                        Align right = tmp & NIBBLE_RIGHT;
                        ato[i] = SHIFT_RIGHT(left, 4) | right << 4;
                    }
                    Align tmp = ARMY_MASK | ULOAD(afrom[n]);
                    Align left  = tmp & NIBBLE_LEFT;
                    Align right = tmp & NIBBLE_RIGHT;
                    ato[n] = SHIFT_RIGHT(left, 4) | right << 4;
                    sort(to);
                } else {
                    for (uint i=0; i<n; ++i)
                        ato[i] = ULOAD(afrom[i]);
                    ato[n] = ARMY_MASK | ULOAD(afrom[n]);
                }
            } else {
                if (symmetry) {
                    for (uint i=0; i<n; ++i) {
                        Align tmp = ALOAD(afrom[i]);
                        Align left  = tmp & NIBBLE_LEFT;
                        Align right = tmp & NIBBLE_RIGHT;
                        ato[i] = SHIFT_RIGHT(left, 4) | right << 4;
                    }
                    sort(to);
                } else
                    for (uint i=0; i<n; ++i)
                        ASTORE(ato[i], ALOAD(afrom[i]));
            }
        } else {
            if (symmetry) {
                transform(from, from+ARMY, to,
                          [](Coord const& pos) ALWAYS_INLINE { return pos.symmetric(); });
                sort(to);
            } else
                std::copy(from, from+ARMY, to);
            if (terminate) to[ARMY] = Coord::MAX();
        }
    }

    inline Coord& operator[](ssize_t i) PURE { return army_[i];}
    inline Coord      * begin()       FUNCTIONAL { return &army_[0]; }
    inline Coord      * end  ()       FUNCTIONAL { return &army_[ARMY]; }

    inline void sort() { sort(begin()); }

    array<Coord, MAX_ARMY> army_;
};

ostream& operator<<(ostream& os, Army const& army);

inline int cmp(Army const& left, Army const& right) {
    if (!SYMMETRY || X != Y) return 0;
    return memcmp(reinterpret_cast<void const *>(left.begin()),
                  reinterpret_cast<void const *>(right.begin()),
                  sizeof(left[0]) * ARMY);
}

class alignas(Align) ArmyPos {
  private:
    using Pos = int;

    static uint const SIZE_POS = (sizeof(Pos)+sizeof(Coord)-1) / sizeof(Coord);
    static uint const POS_PADDING = ALIGNSIZE - SIZE_POS % ALIGNSIZE;

  public:
    ArmyPos() {
        pos_ = 0;
        at(-1) = Coord::MIN();
        if (!DO_ALIGN) at(ARMY) = Coord::MAX();
    }
    ArmyPos(Army const& army) {
        pos_ = 0;
        at(-1)    = Coord::MIN();
        _copy(army);
        if (!DO_ALIGN) at(ARMY) = Coord::MAX();
    }
    inline uint64_t hash() const PURE {
        return army_hash(begin());
    }
    [[deprecated]] Coord const& operator[](ssize_t i) const PURE {
        return at(static_cast<int>(i));
    }
    [[deprecated]] Coord const& operator[]( size_t i) const PURE {
        return at(static_cast<uint>(i));
    }
    inline Coord const& operator[]( int i) const PURE { return at(i); }
    inline Coord const& operator[](uint i) const PURE { return at(i); }
    // The end() versions aren't *really* FUNCTIONAL but ARMY will never change
    inline Coord const*cbegin() const FUNCTIONAL { return &at(0); }
    inline Coord const*cend  () const FUNCTIONAL { return &at(ARMY); }
    inline Coord const* begin() const FUNCTIONAL { return &at(0); }
    inline Coord const* end  () const FUNCTIONAL { return &at(ARMY); }

    inline void copy(Army const& army, int pos) {
        pos_  = pos;
        _copy(army);
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
        if (pos_ < 0) throw_logic("Negative pos_");
        if (pos_ >= static_cast<int>(ARMY))
            throw_logic("Excessive pos_");
        at(pos_) = val;
    }
    void check(const char* file, int line) const;

  private:
    inline void _copy(Army const& army) {
        Army::_import(army.begin(), begin());
    }

    inline Coord& at(int i) FUNCTIONAL {
        return army_[POS_PADDING+i];
    }
    inline Coord const & at(int i) const FUNCTIONAL {
        return army_[POS_PADDING+i];
    }
    inline Coord& at(uint i) FUNCTIONAL {
        return army_[POS_PADDING+i];
    }
    inline Coord const & at(uint i) const FUNCTIONAL {
        return army_[POS_PADDING+i];
    }
    inline Coord* begin() FUNCTIONAL { return &at(0); }
    inline Coord* end  () FUNCTIONAL { return &at(ARMY); }

    Pos pos_;
    array<Coord, POS_PADDING+MAX_ARMY+!DO_ALIGN> army_;

    friend ostream& operator<<(ostream& os, ArmyPos const& army);
};

inline int cmp(ArmyPos const& left, ArmyPos const& right) {
    if (!SYMMETRY || X != Y) return 0;
    return memcmp(reinterpret_cast<void const *>(left.begin()),
                  reinterpret_cast<void const *>(right.begin()),
                  sizeof(left[0]) * ARMY);
}

Army& Army::operator=(ArmyPos const& army) {
    _import(army.begin(), begin());
    return *this;
}

class ArmyZ {
  public:
    explicit inline ArmyZ(Coord& base): base_{base} {}
    inline void append(Army    const& army) { append(army.begin()); }
    inline void append(ArmyPos const& army) { append(army.begin()); }
  private:
    inline void append(Coord const* RESTRICT army) {
        if (DO_ALIGN) {
            Align const* RESTRICT from = reinterpret_cast<Align const*>(army);
            Align      * RESTRICT to   = reinterpret_cast<Align      *>(&base_);
            uint n = Army::ALIGNEDS();
            for (uint i=0; i<n; ++i)
                USTORE(to[i], from[i]);
        } else {
            std::copy(army, army+ARMY, &base_);
        }
    }

    Coord& RESTRICT base_;
};

class ArmyZconst {
  public:
    explicit inline ArmyZconst(Coord const& base): base_{base} {}
    Coord const* begin() const PURE { return &base_; };
    Coord const* end()   const PURE { return &base_ + ARMY; };
  private:
    inline bool equal(Coord const* RESTRICT army) {
        if (!DO_ALIGN) return std::equal(army, army+ARMY, &base_);
        Align const* RESTRICT left  = reinterpret_cast<Align const*>(army);
        Align const* RESTRICT right = reinterpret_cast<Align const*>(&base_);
        uint n = Army::ALIGNEDS()-1;
        Align accu = (left[n] ^ ULOAD(right[n])) & ARMY_MASK_NOT;
        for (uint i = 0; i < n; ++i)
            accu |= left[i] ^ ULOAD(right[i]);
        return IS_ZERO(accu);
    }
    friend inline bool operator==(Army const& l, ArmyZconst r) {
        return r.equal(l.begin());
    }
    friend inline bool operator==(ArmyPos const& l, ArmyZconst r) {
        return r.equal(l.begin());
    }

    Coord const& RESTRICT base_;
};

template<class T>
class BoardTable {
  public:
    using Array = array<T, Coord::SIZE>;
    using iterator       = typename Array::iterator;
    using const_iterator = typename Array::const_iterator;

    T&       operator[](Coord const& pos)       PURE { return data_[pos._pos()];}
    T const& operator[](Coord const& pos) const PURE { return data_[pos._pos()];}
    inline iterator begin() { return data_.begin(); }
    inline const_iterator begin()  const { return data_.begin(); }
    inline const_iterator cbegin() const { return data_.begin(); }
    inline iterator end() { return data_.begin() + Y * MAX_X; }
    inline const_iterator end()  const { return data_.begin() + Y * MAX_X; }
    inline const_iterator cend() const { return data_.begin() + Y * MAX_X; }
    void fill(T value) { std::fill(begin(), end(), value); }
    void set(Army const& army, T value) {
        for (auto const& pos: army) (*this)[pos] = value;
    }
  private:
    Array data_;
};

int Army::symmetry() const {
    if (!SYMMETRY || X != Y) return 0;
    BoardTable<uint8_t> test;
    for (auto const& pos: *this)
        test[pos] = 0;
    for (auto const& pos: *this)
        test[pos.symmetric()] = 1;
    uint8_t sum = ARMY;
    for (auto const& pos: *this)
        sum -= test[pos];
    return sum ? 1 : 0;
}

class ArmyMapper {
  public:
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

class ArmyMapperPair {
  public:
    explicit inline ArmyMapperPair(Army const& normal, Army const& symmetric):
        normal_{symmetric},
        symmetric_{normal} {}
    ArmyMapper const& normal()    const FUNCTIONAL { return normal_; }
    ArmyMapper const& symmetric() const FUNCTIONAL { return symmetric_; }
  private:
    ArmyMapper normal_, symmetric_;
};

class BoardSet;
class Statistics {
  public:
    using Counter = uint64_t;
    Statistics() {
        clear();
    }
    void clear() {
        late_prunes_ = 0;
        edge_count_  = 0;
        largest_subset_ = 0;
        max_overflow_ = 0;

        armyset_size_ = 0;
        armyset_tries_ = 0;
        armyset_probes_ = 0;
        armyset_immediate_ = 0;
        armyset_allocs_ = 0;
        armyset_allocs_cached_ = 0;
        armyset_deallocs_ = 0;
        armyset_deallocs_cached_ = 0;

        boardset_size_ = 0;
        boardset_tries_ = 0;
        boardset_probes_ = 0;
        boardset_immediate_ = 0;
    }
    inline void late_prune()   {
        if (!STATISTICS) return;
        ++late_prunes_;
    }
    inline void edge(Counter add = 1)   {
        if (!STATISTICS) return;
        edge_count_ += add;
    }
    inline void subset_size(size_t size) {
        if (!STATISTICS) return;
        if (size > largest_subset_) largest_subset_ = size;
    }
    inline void overflow(size_t size) {
        if (!STATISTICS) return;
        if (size > max_overflow_) max_overflow_ = size;
    }
    inline void largest_subset_size(BoardSet const& boards) {
        if (!STATISTICS) return;
        _largest_subset_size(boards);
    }
    inline void armyset_size(Counter size) {
        armyset_tries_ += size;
        armyset_size_ = size;
    }
    inline void armyset_try() {
        if (STATISTICS || HASH_STATISTICS) ++armyset_tries_;
    }
    inline void armyset_untry(Counter del) {
        armyset_tries_ -= del;
    }
    inline void armyset_alloc() {
        if (!STATISTICS) return;
        ++armyset_allocs_;
    }
    inline void armyset_alloc_cached() {
        if (!STATISTICS) return;
        ++armyset_allocs_cached_;
    }
    inline void armyset_dealloc() {
        if (!STATISTICS) return;
        ++armyset_deallocs_;
    }
    inline void armyset_dealloc_cached() {
        if (!STATISTICS) return;
        ++armyset_deallocs_cached_;
    }
    inline void boardset_size(Counter size) {
        boardset_tries_ += size;
        boardset_size_ = size;
    }
    inline void boardset_try() {
        if (STATISTICS || HASH_STATISTICS) ++boardset_tries_;
    }
    inline void boardset_untry(Counter del) {
        boardset_tries_ -= del;
    }
    inline void armyset_probe(ArmyId probes) {
        if (!HASH_STATISTICS) return;
        armyset_probes_    += probes;
        armyset_immediate_ += probes == 0;
    }
    inline void boardset_probe(ArmyId probes) {
        if (!HASH_STATISTICS) return;
        boardset_probes_    += probes;
        boardset_immediate_ += probes == 0;
    }
    Counter late_prunes() const PURE { return late_prunes_; }
    Counter edges() const PURE { return edge_count_; }
    size_t  largest_subset() const PURE { return largest_subset_; }
    size_t  max_overflow() const PURE { return max_overflow_; }
    Counter armyset_size() const PURE { return armyset_size_; }
    Counter armyset_tries() const PURE { return armyset_tries_; }
    Counter armyset_immediate() const PURE { return armyset_immediate_; }
    Counter armyset_probes() const PURE { return armyset_probes_; }
    Counter armyset_allocs() const PURE { return armyset_allocs_; }
    Counter armyset_allocs_cached() const PURE { return armyset_allocs_cached_; }
    Counter armyset_deallocs() const PURE { return armyset_deallocs_; }
    Counter armyset_deallocs_cached() const PURE { return armyset_deallocs_cached_; }
    Counter boardset_size() const PURE { return boardset_size_; }
    Counter boardset_tries() const PURE { return boardset_tries_; }
    Counter boardset_immediate() const PURE { return boardset_immediate_; }
    Counter boardset_probes() const PURE { return boardset_probes_; }

    Statistics& operator+=(Statistics const& stats) {
        late_prunes_	  += stats.late_prunes();
        edge_count_	  += stats.edges();
        auto largest_subset = stats.largest_subset();
        if (largest_subset > largest_subset_) largest_subset_ = largest_subset;
        auto max_overflow   = stats.max_overflow();
        if (max_overflow > max_overflow_) max_overflow_ = max_overflow;

        armyset_immediate_  += stats.armyset_immediate_;
        armyset_probes_	  += stats.armyset_probes_;
        armyset_tries_	  += stats.armyset_tries();
        armyset_allocs_          += stats.armyset_allocs();
        armyset_allocs_cached_   += stats.armyset_allocs_cached();
        armyset_deallocs_        += stats.armyset_deallocs();
        armyset_deallocs_cached_ += stats.armyset_deallocs_cached();

        boardset_immediate_ += stats.boardset_immediate_;
        boardset_probes_    += stats.boardset_probes_;
        boardset_tries_	    += stats.boardset_tries();
        return *this;
    }

  private:
    void _largest_subset_size(BoardSet const& boards);

    Counter late_prunes_;
    Counter edge_count_;
    Counter armyset_size_;
    Counter armyset_tries_;
    Counter armyset_probes_;
    Counter armyset_immediate_;
    Counter armyset_allocs_;
    Counter armyset_allocs_cached_;
    Counter armyset_deallocs_;
    Counter armyset_deallocs_cached_;
    Counter boardset_size_;
    Counter boardset_tries_;
    Counter boardset_probes_;
    Counter boardset_immediate_;
    size_t  largest_subset_;
    size_t  max_overflow_;
};

STATIC const int ARMYID_BITS = std::numeric_limits<ArmyId>::digits;
STATIC const ArmyId ARMYID_HIGHBIT = static_cast<ArmyId>(1) << (ARMYID_BITS-1);
STATIC const ArmyId ARMYID_MASK = ARMYID_HIGHBIT-1;
STATIC const ArmyId ARMYID_MAX  = std::numeric_limits<ArmyId>::max();

class ArmySetDense {
  public:
    static size_t const INITIAL_SIZE = 32;

    ArmySetDense(bool lock = false, size_t size = INITIAL_SIZE);
    ~ArmySetDense();
    void clear(size_t size = INITIAL_SIZE);
    void lock() {
        if (!MLOCK) return;
        if (memory_flags_ & ALLOC_LOCK) throw_logic("Already locked");
        memory_flags_ |= ALLOC_LOCK;
        memlock(armies_, armies_size_);
        if (values_) memlock(values_, allocated());
    }
    void unlock() {
        if (!MLOCK) return;
        if (!(memory_flags_ & ALLOC_LOCK)) throw_logic("Already unlocked");
        memory_flags_ &= ~ALLOC_LOCK;
        memunlock(armies_, armies_size_);
        if (values_) memunlock(values_, allocated());
    }
    void drop_hash() {
        demallocate(values_, allocated(), memory_flags_);
        // logger << "Drop hash values " << static_cast<void const *>(values_) << "\n" << flush;
        values_ = nullptr;
    }
    inline void convert_hash() {}
    inline ArmyId size() const PURE { return used_; }
    inline size_t allocated() const PURE {
        return static_cast<size_t>(mask_)+1;
    }
    inline ArmyId capacity() const PURE {
        return limit_;
    }
#if CHECK
    ArmyZconst at(ArmyId i) const {
        if (UNLIKELY(i > used_))
            throw_logic("Army id " + to_string(i) + " out of range of set");
        return ArmyZconst{armies_[i * static_cast<size_t>(ARMY)]};
    }
#else  // CHECK
    inline ArmyZconst at(ArmyId i) const PURE {
        return ArmyZconst{armies_[i * static_cast<size_t>(ARMY)]};
    }
#endif // CHECK
    inline ArmyZconst cat(ArmyId i) const { return at(i); }
    inline ArmyId insert(Army const& army, Statistics& stats) RESTRICT;
    inline ArmyId insert(ArmyPos const& army, Statistics& stats) RESTRICT;
    ArmyId find(Army    const& army) const PURE;
    ArmyId find(ArmyPos const& army) const PURE;
    inline Coord const* begin() const PURE { return &armies_[ARMY]; }
    inline Coord const* end()   const PURE { return &armies_[used_*static_cast<size_t>(ARMY)+ARMY]; }

    void print(ostream& os) const;
    // Not copyable (avoid accidents)
    ArmySetDense(ArmySetDense const&) = delete;
    ArmySetDense& operator=(ArmySetDense const&) = delete;

  private:
    static ArmyId constexpr FACTOR(size_t size) { return static_cast<ArmyId>(0.7*size); }
    static size_t constexpr MIN_SIZE_GENERATOR(size_t v) { return FACTOR(2*v) >= 1 ? v : MIN_SIZE_GENERATOR(2*v); }
    static size_t constexpr MIN_SIZE() { return MIN_SIZE_GENERATOR(1); }

    inline void _init(size_t size);
    NOINLINE void resize() RESTRICT;

#if CHECK
    ArmyZ at(ArmyId i) {
        if (UNLIKELY(i > used_))
            throw_logic("Army id " + to_string(i) + " out of range of set");
        return ArmyZ{armies_[i * static_cast<size_t>(ARMY)]};
    }
#else  // CHECK
    inline ArmyZ at(ArmyId i) PURE {
        return ArmyZ{armies_[i * static_cast<size_t>(ARMY)]};
    }
#endif // CHECK

    Coord* armies_;
    ArmyId* values_;
    size_t armies_size_;
    // g++ sizeof mutex=40, alignof mutex = 8
    mutex exclude_;
    ArmyId mask_;
    ArmyId used_;
    ArmyId limit_;
    int    memory_flags_;
};

// This is essentially google sparse hash for variable size elements
class ArmySetSparse {
  private:
    static ArmyId const GROUP_SIZE = 64;
    static uint   const GROUP_BITS = LOG2(GROUP_SIZE);
    static uint   const GROUP_MASK = GROUP_SIZE - 1;
    static size_t const INITIAL_SIZE = GROUP_SIZE * 1;
    static uint   const GROUP_BUILDERS = 8;

    using GroupId = ArmyId;

    class Element;
    class DataCache {
        class SizeCache {
            // Not copyable (avoid accidents)
            SizeCache(SizeCache const&) = delete;
            SizeCache& operator=(SizeCache const&) = delete;

          public:
            static uint   const DATA_CACHE_SIZE = 8;
            static void set_ARMY_size(uint i) {
                block_size_[i] = (i+1) * Element::SIZE;
            }

            SizeCache(): cached_{0} {}
            inline char* allocate(GroupId id, uint i) {
                // Directly use ::allocate without test if DATA_CACHE_SIZE is made 0
                if (DATA_CACHE_SIZE && cached_) return cache_[--cached_];
                return ::allocate<char>(block_size_[i]);
            }
            inline char* allocate(GroupId id, uint i, Statistics& stats) {
                stats.armyset_alloc();
                // Directly use ::allocate without test if DATA_CACHE_SIZE is made 0
                if (DATA_CACHE_SIZE && cached_) {
                    stats.armyset_alloc_cached();
                    return cache_[--cached_];
                }
                return ::allocate<char>(block_size_[i]);
            }
            void deallocate(uint i, char* data) {
                if (cached_ < DATA_CACHE_SIZE) cache_[cached_++] = data;
                else ::deallocate(data, block_size_[i]);
            }
            void deallocate(uint i, char* data, Statistics& stats) {
                stats.armyset_dealloc();
                if (cached_ < DATA_CACHE_SIZE) {
                    stats.armyset_dealloc_cached();
                    cache_[cached_++] = data;
                }
                else ::deallocate(data, block_size_[i]);
            }
            inline void free(uint i, bool zero);
          private:
            static array<uint, GROUP_SIZE> block_size_;

            array<char*, DATA_CACHE_SIZE> cache_;
            uint cached_;
            // uint block_size_;
            char* data_;
            size_t size_;
            size_t free_;
            size_t lower_bound_;
            size_t upper_bound_;
        };
      public:
        static void set_ARMY_size() {
            for (uint i=0; i<GROUP_SIZE; ++i)
                SizeCache::set_ARMY_size(i);
        }

        inline char* allocate(GroupId id, uint n) {
            return cache_[n-1].allocate(id, n-1);
        }
        inline char* allocate(GroupId id, uint n, Statistics& stats) {
            return cache_[n-1].allocate(id, n-1, stats);
        }
        void deallocate(uint n, char* data) {
            cache_[n-1].deallocate(n-1, data);
        }
        void deallocate(uint n, char* data, Statistics& stats) {
            cache_[n-1].deallocate(n-1, data, stats);
        }
        inline void free(bool zero);
        inline size_t _overflowed() const PURE {
            return overflow_used_;
        }
        inline size_t overflowed() const PURE {
            return _overflowed() / Element::SIZE;
        }
        inline size_t max_overflow() const PURE {
            return overflow_size_ / Element::SIZE;
        }
        inline Element const& overflow_pop() {
            overflow_used_ -= Element::SIZE;
            return Element::element(&overflow_[overflow_used_]);
        }
        inline void overflow(Element const& element) {
            if (overflow_used_ >= overflow_size_) {
                if (overflow_size_) {
                    remallocate(overflow_, overflow_size_, overflow_size_ * 2);
                    overflow_size_ *= 2;
                } else {
                    mallocate(overflow_, GROUP_SIZE * Element::SIZE);
                    overflow_size_ = GROUP_SIZE * Element::SIZE;
                }
            }
            char const* ptr = reinterpret_cast<char const*>(&element);
            std::copy(ptr, ptr+Element::SIZE, &overflow_[overflow_used_]);
            overflow_used_ += Element::SIZE;
        }
      private:
        char* overflow_;
        size_t overflow_used_;
        size_t overflow_size_;
        array<SizeCache, GROUP_SIZE> cache_;
    };

    class alignas(char) Element {
      public:
        static size_t SIZE;
        
        static Element& element(char* ptr, uint i = 0) {
            return *reinterpret_cast<Element *>(&ptr[i * SIZE]);
        }
        static Element const& element(char const* ptr, uint i = 0) {
            return *reinterpret_cast<Element const*>(&ptr[i * SIZE]);
        }
        inline ArmyId id() const PURE { return id_; }
        inline void id(ArmyId army_id) { id_ = army_id; }
        inline ArmyZconst armyZ() const PURE { return ArmyZconst{coord_[0]}; }
        inline uint64_t hash() const PURE {
            return army_hash(&coord_[0]);
        }

        Coord*       begin()       PURE { return &coord_[0]; }
        Coord const* begin() const PURE { return &coord_[0]; }
        Coord*       end()         PURE { return &coord_[ARMY]; }
        Coord const* end()   const PURE { return &coord_[ARMY]; }
      private:
        ArmyId id_;
        Coord  coord_[0];
    };
    class Bitmap {
        // Not copyable (avoid accidents)
        Bitmap(Bitmap const&) = delete;
        Bitmap& operator=(Bitmap const&) = delete;
      public:
        using bitmap_type = uint64_t;

        Bitmap() {}
        Bitmap(bitmap_type bitmap): bitmap_{bitmap} {}
        inline uint bits() const PURE {
            return popcount64(bitmap_);
        }
        inline uint drop_one() {
            uint i = ctz64(bitmap_);
            bitmap_ = bitmap_ & (UINT64_C(-2) << i);
            return i;
        }
        inline bool bit(uint n) const PURE {
            return (bitmap_ & (UINT64_C(1) << n)) != 0;
        }
        inline uint index(uint n) const PURE {
            return popcount64(bitmap_ & ((UINT64_C(1) << n)-1));
        }
        inline void set(uint n) {
            bitmap_ = bitmap_ | (UINT64_C(1) << n);
        }
        inline bitmap_type bitmap() const PURE { return bitmap_; }
      protected:
        bitmap_type bitmap_;
    };
    class GroupBuilder;
    class Group: public Bitmap {
      public:
        inline void _free_data() {
            deallocate(data_, bits() * Element::SIZE);
        }
        inline void _free_converted_data() {
            deallocate(data_, bits() * sizeof(ArmyId));
        }
        inline void _free_data(ArmyId* new_data) {
            deallocate(data_, bits() * Element::SIZE);
            data_ = reinterpret_cast<char *>(new_data);
        }
        inline void _free() {
            if (bitmap_) _free_data();
        }
        inline Element const& at(uint pos) const PURE {
            uint i = index(pos);
            return Element::element(data_, i);
        }
        inline ArmyId const& id(uint pos) const PURE {
            uint i = index(pos);
            return reinterpret_cast<ArmyId const*>(data_)[i];
        }
        inline char const* _data() const PURE { return data_; }
        inline void append(DataCache& cache, GroupId group_id, uint pos, ArmyId army_id, Coord const* army, Statistics& stats);
        inline void append(DataCache& cache, GroupId group_id, uint pos, Element const& old_element);
        inline void copy(DataCache& cache, GroupId id, GroupBuilder const& group_builder);
      protected:
        char* data_;
    };
    class GroupBuilder: public Bitmap {
      public:
        inline void clear() {
            bitmap_ = 0;
        }
        inline Element const& at(uint pos) const PURE {
            return Element::element(&data_[0], pos);
        }
        inline void copy(uint pos, Element const& element) {
            set(pos);
            char const* ptr = reinterpret_cast<char const*>(&element);
            std::copy(ptr, ptr+Element::SIZE, &data_[pos * Element::SIZE]);
        }
        inline void copy_to(uint pos, char* target) const {
            char const* ptr = &data_[pos * Element::SIZE];
            std::copy(ptr, ptr+Element::SIZE, target);
        }
      private:
        array<char, GROUP_SIZE * (MAX_ARMY * sizeof(Coord) + sizeof(Element))> data_;
    };
  public:
    static void set_ARMY_size() {
        Element::SIZE = ARMY * sizeof(Coord) + sizeof(Element);
        // cout << "sizeof(Element) = " << sizeof(Element) << ", Element::SIZE = " << Element::SIZE << "\n";
        DataCache::set_ARMY_size();
    }
    ArmySetSparse(bool lock = false, size_t size = INITIAL_SIZE);
    ~ArmySetSparse();
    void lock() {
        // Only lock the groups_ spine (if it is actively being built)
        if (!MLOCK) return;
        if (memory_flags_ & ALLOC_LOCK) throw_logic("Already locked");
        memory_flags_ |= ALLOC_LOCK;
        // logger << "Would lock " << groups_ << " " << armies_ << " g=" << nr_groups() << endl;
        if (groups_ && !armies_) memlock(groups_, nr_groups());
    }
    void unlock() {
        // Only unlock the groups_ spine (if it is actively being built)
        if (!MLOCK) return;
        if (!(memory_flags_ & ALLOC_LOCK)) throw_logic("Already unlocked");
        memory_flags_ &= ~ALLOC_LOCK;
        // logger << "Would unlock " << groups_ << " " << armies_ << " g=" << nr_groups() << endl;
        if (groups_ && !armies_) memunlock(groups_, nr_groups());
    }
    void clear(size_t size = INITIAL_SIZE);
    inline void drop_hash()    { _convert_hash(false); }
    inline void convert_hash() { _convert_hash(true);  }

    inline ArmyId size() const PURE { return size_; }
    inline size_t nr_groups() const PURE {
        return static_cast<size_t>(mask_)+1;
    }
    inline size_t allocated() const PURE {
        return nr_groups() * GROUP_SIZE;
    }
    inline size_t max_overflow() const PURE {
        return data_cache_.max_overflow();
    }
    inline ArmyId insert(Army    const& army, Statistics& stats) RESTRICT;
    inline ArmyId insert(ArmyPos const& army, Statistics& stats) RESTRICT;
    ArmyId find(Army const& army) const PURE;
    ArmyId find(ArmyPos const& army) const PURE;

#if CHECK
    ArmyZconst at(ArmyId i) const {
        if (UNLIKELY(i > size_))
            throw_logic("Army id " + to_string(i) + " out of range of set");
        if (UNLIKELY(!armies_)) throw_logic("No army list allocated");
        return ArmyZconst{armies_[i * static_cast<size_t>(ARMY)]};
    }
#else  // CHECK
    inline ArmyZconst at(ArmyId i) const PURE {
        return ArmyZconst{armies_[i * static_cast<size_t>(ARMY)]};
    }
#endif // CHECK
    inline ArmyZconst cat(ArmyId i) const { return at(i); }
    void print(ostream& os, bool show_boards = true) const;

    // Not copyable (avoid accidents)
    ArmySetSparse(ArmySetSparse const&) = delete;
    ArmySetSparse& operator=(ArmySetSparse const&) = delete;

  private:
    static ArmyId constexpr FACTOR(size_t size) { return static_cast<ArmyId>(0.7*size); }

    inline void _init(size_t size);
    inline void free_groups();
    NOINLINE void resize() RESTRICT;
    void _convert_hash(bool keep = false);
    inline void __convert_hash(bool keep);

    Coord* armies_;
    Group* groups_;
    ArmyId* indices_;
    DataCache data_cache_;
    mutex exclude_;
    ArmyId size_;
    ArmyId left_;
    GroupId mask_;
    int    memory_flags_;
};

void ArmySetSparse::Group::copy(DataCache& cache, GroupId id, GroupBuilder const& group_builder) {
    bitmap_ = group_builder.bitmap();
    if (bitmap_ == 0) return;
    Bitmap b{bitmap_};
    uint n = b.bits();
    data_ = cache.allocate(id, n);
    char* ptr = data_;
    for (uint i=0; i < n; ++i, ptr += Element::SIZE) {
        uint j = b.drop_one();
        group_builder.copy_to(j, ptr);
    }
}

inline ostream& operator<<(ostream& os, ArmySet const& set) {
    set.print(os);
    return os;
}

Army::Army(ArmyZconst army, ArmyId symmetry) {
    _import(army.begin(), begin(), symmetry, true);
}

Army::Army(ArmySet const& armies, ArmyId id, ArmyId symmetry):
    Army{armies.cat(id), symmetry}
 {}

struct Move {
    Move() {}
    Move(Coord const& from_, Coord const& to_): from{from_}, to{to_} {}
    Move(Army const& army_from, Army const& army_to);
    Move(Army const& army_from, Army const& army_to, int& diff);

    Coord from, to;
};

// Board as two Armies
class FullMove;
class Image;
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    void do_move(Move const& move_);
    void do_move(Move const& move, bool blue_to_move) {
        auto& army = blue_to_move ? blue() : red();
        army.do_move(move);
    }
    inline void do_move(FullMove const& move_);
    inline void do_move(FullMove const& move_, bool blue_to_move);
    Army& blue() { return blue_; }
    Army& red()  { return red_; }
    Army const& blue() const FUNCTIONAL { return blue_; }
    Army const& red()  const FUNCTIONAL { return red_; }
    inline void check(const char* file, int line) const {
        blue_.check(file, line);
        red_ .check(file, line);
    }
    int min_nr_moves(bool blue_to_move) const PURE;
    int min_nr_moves() const PURE {
        return min(min_nr_moves(true), min_nr_moves(false));
    }
    void svg(ostream& os, uint scale, uint marging) const;

  private:
    Army blue_, red_;

    friend bool operator==(Board const& l, Board const& r) {
        return l.blue() == r.blue() && l.red() == r.red();
    }
};
using BoardList = vector<Board>;

class StatisticsE: public Statistics {
  public:
    StatisticsE(int available_moves, Counter opponent_armies_size):
        opponent_armies_size_{opponent_armies_size},
        available_moves_{available_moves} {}
    void start() { start_ = chrono::steady_clock::now(); }
    void stop () {
        stop_  = chrono::steady_clock::now();
        memory_ = get_memory();
        allocated_ = total_allocated();
        mmapped_ = total_mmapped();
        mmaps_ = total_mmaps();
        mlocked_ = total_mlocked();
        mlocks_ = total_mlocks();
    }
    size_t const memory()    const PURE { return memory_; }
    size_t const allocated() const PURE { return allocated_; }
    size_t const mmapped() const PURE { return mmapped_; }
    size_t const mmaps() const PURE { return mmaps_; }
    size_t const mlocked() const PURE { return mlocked_; }
    size_t const mlocks() const PURE { return mlocks_; }
    Sec::rep duration() const PURE {
        return chrono::duration_cast<Sec>(stop_-start_).count();
    }
    int available_moves() const PURE { return available_moves_; }
    bool blue_move() const PURE { return available_moves() & 1; }
    string css_color() const PURE { return blue_move() ? "blue" : "red"; }
    Counter blue_armies_size() const PURE {
        return blue_move() ? armyset_size() : opponent_armies_size_;
    }
    void print(ostream& os) const;
    void example_board(Board const& board) {
        example_board_ = board;
        example_ = true;
    }
    Board const& example_board() const FUNCTIONAL { return example_board_; }
    bool const example() const FUNCTIONAL { return example_; }
  private:
    size_t memory_;
    ssize_t allocated_;
    ssize_t mmapped_;
    ssize_t mmaps_;
    ssize_t mlocked_;
    ssize_t mlocks_;
    chrono::steady_clock::time_point start_, stop_;
    Counter opponent_armies_size_;
    int available_moves_;
    bool example_ = false;
    Board example_board_;
};

inline ostream& operator<<(ostream& os, StatisticsE const& stats) {
    stats.print(os);
    return os;
}

using StatisticsList = vector<StatisticsE>;

class BoardSubSetBase {
  public:
    static ArmyId split(ArmyId value, ArmyId& red_id) {
        red_id = value & ARMYID_MASK;
        // cout << "Split: Value=" << hex << value << ", red id=" << red_id << ", symmetry=" << (value & ARMYID_HIGHBIT) << dec << "\n";
        return value & ARMYID_HIGHBIT;
    }
    static inline constexpr ArmyId join(ArmyId id, bool flip) {
        return id | (flip ? ARMYID_HIGHBIT : 0);
    }
    ArmyId const* begin() const PURE { return &armies_[0]; }
  protected:
    ArmyId* begin() PURE { return &armies_[0]; }

    ArmyId* armies_;
    ArmyId mask_;
    ArmyId left_;
};

class BoardSubSet: public BoardSubSetBase {
  public:
    static ArmyId const INITIAL_SIZE = 4;

    ArmyId allocated() const PURE { return mask_+1; }
    ArmyId capacity()  const PURE { return FACTOR(allocated()); }
    ArmyId size()      const PURE { return capacity() - left_; }
    bool empty() const PURE { return size() == 0; }
    inline void create(ArmyId size = INITIAL_SIZE) {
        mask_ = size-1;
        left_ = FACTOR(size);
        cmallocate(armies_, size);
        // logger << "Create BoardSubSet " << static_cast<void const*>(armies_) << ": size " << size << ", " << left_ << " left\n" << flush;
    }
    inline void zero() {
        armies_ = nullptr;
        mask_ = 0;
        left_ = 0;
    }
    inline void destroy() {
        if (armies_) {
            demallocate(armies_, allocated());
            // logger << "Destroy BoardSubSet " << static_cast<void const*>(armies_) << ": size " << allocated() << "\n" << flush;
        }
    }
    ArmyId const* end()   const PURE { return &armies_[allocated()]; }

    inline bool insert(ArmyId red_id, int symmetry, Statistics& stats) {
        if (CHECK) {
            if (UNLIKELY(red_id <= 0))
                throw_logic("red_id <= 0");
            if (UNLIKELY(red_id >= ARMYID_HIGHBIT))
                throw_logic("red_id is too large");
        }
        ArmyId value = join(red_id, symmetry < 0);
        return insert(value, stats);
    }
    bool find(ArmyId red_id, int symmetry) const PURE {
        if (CHECK) {
            if (UNLIKELY(red_id <= 0))
                throw_logic("red_id <= 0");
            if (UNLIKELY(red_id >= ARMYID_HIGHBIT))
                throw_logic("red_id is too large");
        }
        return find(join(red_id, symmetry < 0));
    }
    ArmyId example(ArmyId& symmetry) const;
    ArmyId random_example(ArmyId& symmetry) const;
    void print(ostream& os) const;
    void print() const { print(cout); }

  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.7*factor); }
    NOINLINE void resize() RESTRICT;

    inline bool insert(ArmyId red_value, Statistics& stats);
    bool find(ArmyId id) const PURE;

    ArmyId* end()   PURE { return &armies_[allocated()]; }
};

bool BoardSubSet::insert(ArmyId red_value, Statistics& stats) {
    // logger << "Insert " << red_value << "\n";
    if (left_ == 0) resize();
    auto mask = mask_;
    ArmyId pos = hash64(red_value) & mask;
    ArmyId offset = 0;
    auto armies = armies_;
    while (true) {
        // cout << "Try " << pos << " of " << mask+1 << "\n";
        auto& rv = armies[pos];
        if (rv == 0) {
            stats.boardset_probe(offset);
            rv = red_value;
            --left_;
            // cout << "Found empty\n";
            return true;
        }
        if (rv == red_value) {
            stats.boardset_probe(offset);
            stats.boardset_try();
            // cout << "Found duplicate\n";
            return false;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

class BoardSubSetRed: public BoardSubSetBase {
  public:
    inline BoardSubSetRed(ArmyId* list, ArmyId size) {
        armies_ = list;
        left_   = size;
        mask_   = ARMYID_MAX;
    }
    inline void zero() {
        armies_ = nullptr;
        mask_ = 0;
        left_ = 0;
    }
    inline void destroy() {
        if (armies_) {
            demallocate(armies_, size());
            // logger << "Destroy BoardSubSetRed " << static_cast<void const*>(armies_) << ": size " << size() << "\n" << flush;
        }
    }
    ArmyId size()       const PURE { return left_; }
    bool empty() const PURE { return size() == 0; }
    ArmyId const* end() const PURE { return &armies_[size()]; }
    ArmyId example(ArmyId& symmetry) const;
    ArmyId random_example(ArmyId& symmetry) const;
    inline bool _insert(ArmyId red_id, int symmetry, Statistics& stats) {
        if (CHECK) {
            if (UNLIKELY(red_id <= 0))
                throw_logic("red_id <= 0");
            if (UNLIKELY(red_id >= ARMYID_HIGHBIT))
                throw_logic("red_id is too large");
        }
        ArmyId value = join(red_id, symmetry < 0);
        return _insert(value, stats);
    }
    bool _find(ArmyId red_id, int symmetry) const PURE {
        if (CHECK) {
            if (UNLIKELY(red_id <= 0))
                throw_logic("red_id <= 0");
            if (UNLIKELY(red_id >= ARMYID_HIGHBIT))
                throw_logic("red_id is too large");
        }
        return _find(join(red_id, symmetry < 0));
    }
  private:
    bool _insert(ArmyId red_value, Statistics& stats);
    bool _find(ArmyId red_value) const PURE;
};

class BoardSubSetRedBuilder: public BoardSubSetBase {
  public:
    static ArmyId const INITIAL_SIZE = 32;

    BoardSubSetRedBuilder(ArmyId allocate = INITIAL_SIZE);
    ~BoardSubSetRedBuilder() {
        demallocate(armies_, real_allocated_, ALLOC_LOCK);
        army_list_ -= size();
        demallocate(army_list_, FACTOR(real_allocated_), ALLOC_LOCK);
        // logger << "Destroy BoardSubSetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << real_allocated_ << "), list " << static_cast<void const *>(army_list) << " (size " << FACTOR(real_allocated_) << ")\n" << flush;
    }
    ArmyId allocated() const PURE { return mask_+1; }
    ArmyId capacity()  const PURE { return FACTOR(allocated()); }
    ArmyId size()      const PURE { return capacity() - left_; }
    inline bool insert(ArmyId red_id, int symmetry, Statistics& stats) {
        if (CHECK) {
            if (UNLIKELY(red_id <= 0))
                throw_logic("red_id <= 0");
            if (UNLIKELY(red_id >= ARMYID_HIGHBIT))
                throw_logic("red_id is too large");
        }
        ArmyId value = join(red_id, symmetry < 0);
        return insert(value, stats);
    }
    inline BoardSubSetRed extract(ArmyId allocated = INITIAL_SIZE) {
        ArmyId sz = size();
        ArmyId* new_list = mallocate<ArmyId>(sz);
        // logger << "Extract BoardSubSetRed " << static_cast<void const*>(new_list) << ": size " << sz << "\n" << flush;
        ArmyId* old_list = army_list_ - sz;
        std::copy(&old_list[0], &old_list[sz], new_list);

        army_list_ = old_list;
        mask_ = allocated-1;
        left_ = capacity();
        std::fill(begin(), end(), 0);

        return BoardSubSetRed{new_list, sz};
    }

  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.5*factor); }

    ArmyId const* end()   const PURE { return &armies_[allocated()]; }
    ArmyId      * end()         PURE { return &armies_[allocated()]; }
    inline bool insert(ArmyId red_value, Statistics& stats);
    NOINLINE void resize() RESTRICT;

    ArmyId* army_list_;
    ArmyId real_allocated_;
};

bool BoardSubSetRedBuilder::insert(ArmyId red_value, Statistics& stats) {
    // logger << "Insert " << red_value << " into BoardSubSetRedBuilder\n";
    if (left_ == 0) resize();
    auto mask = mask_;
    ArmyId pos = hash64(red_value) & mask;
    ArmyId offset = 0;
    auto armies = armies_;
    while (true) {
        // cout << "Try " << pos << " of " << mask+1 << "\n";
        auto& rv = armies[pos];
        if (rv == 0) {
            stats.boardset_probe(offset);
            rv = red_value;
            *army_list_++ = red_value;
            --left_;
            // cout << "Found empty\n";
            return true;
        }
        if (rv == red_value) {
            stats.boardset_probe(offset);
            stats.boardset_try();
            // cout << "Found duplicate\n";
            return false;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

class BoardSetBase {
    friend class BoardSubSetRefBase;
  public:
    static ArmyId const INITIAL_SIZE = 32;
    BoardSetBase(bool keep = false, ArmyId size = INITIAL_SIZE);
    ~BoardSetBase() {
        ++subsets_;
        demallocate(subsets_, capacity());
    }
    ArmyId subsets() const PURE { return top_ - from(); }
    size_t size() const PURE { return size_; }
    bool empty() const PURE { return size() == 0; }
    ArmyId back_id() const PURE { return top_-1; }
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
    BoardSetBase(BoardSetBase const&) = delete;
    BoardSetBase& operator=(BoardSetBase const&) = delete;
  protected:
    ArmyId capacity() const PURE { return capacity_; }
    ArmyId next() {
        lock_guard<mutex> lock{exclude_};
        return from_ < top_ ? from_++ : 0;
    }
    ArmyId from() const PURE { return keep_ ? 1 : from_; }
    NOINLINE void resize() RESTRICT;
    void down_size(ArmyId size) { size_ -= size; }
    inline void clear(ArmyId size = INITIAL_SIZE);

    size_t size_;
    mutex exclude_;
    Army solution_;
    ArmyId solution_id_;
    ArmyId capacity_;
    ArmyId from_;
    ArmyId top_;
    BoardSubSetBase* subsets_;
    bool const keep_;
};

class BoardSet: public BoardSetBase {
    friend class BoardSubSetRef;
  public:
    BoardSet(bool keep = false, ArmyId size = INITIAL_SIZE) :
        BoardSetBase{keep, size} {}
    ~BoardSet() {
        for (auto& subset: *this)
            subset.destroy();
        // cout << "Destroy BoardSet " << static_cast<void const*>(subsets_) << "\n";
    }
    void clear(ArmyId size = INITIAL_SIZE);
    inline BoardSubSet const& cat(ArmyId id) const PURE {
        return static_cast<BoardSubSet const&>(subsets_[id]);
    }
    inline BoardSubSet const& at(ArmyId id) const PURE { return cat(id); }
    inline BoardSubSet const* begin() const PURE { return &cat(from()); }
    inline BoardSubSet const* end()   const PURE { return &cat(top_); }

    inline bool insert(ArmyId blue_id, ArmyId red_id, int symmetry, Statistics& stats) {
        if (CHECK) {
            if (UNLIKELY(blue_id <= 0))
                throw_logic("red_id <= 0");
        }
        lock_guard<mutex> lock{exclude_};

        if (blue_id >= top_) {
            // Only in the multithreaded case blue_id can be different from top_
            // if (blue_id != top_) throw_logic("Cannot grow more than 1");
            while (blue_id > capacity_) resize();
            while (blue_id >= top_) at(top_++).create();
        }
        bool result = at(blue_id).insert(red_id, symmetry, stats);
        size_ += result;
        return result;
    }
    void insert(ArmyId blue_id, BoardSubSet const& subset) {
        if (CHECK) {
            if (UNLIKELY(blue_id <= 0))
                throw_logic("red_id <= 0");
        }
        lock_guard<mutex> lock{exclude_};

        if (blue_id >= top_) {
            // Only in the multithreaded case blue_id can be different from top_
            // if (blue_id != top_) throw_logic("Cannot grow more than 1");
            while (blue_id > capacity_) resize();
            // This is only called when doing red moves and all updates
            // will be done as an overwrite. So use zero() instead of create()
            while (blue_id > top_) at(top_++).zero();
            ++top_;
        }
        at(blue_id) = subset;
        size_ += subset.size();
    }
    bool insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red);
    bool insert(Board const& board, ArmySet& armies_to_move, ArmySet& armies_opponent, int nr_moves) {
        int blue_to_move = nr_moves & 1;
        bool result = blue_to_move ?
            insert(board, armies_to_move, armies_opponent) :
            insert(board, armies_opponent, armies_to_move);
        return result;
    }
    bool find(ArmyId blue_id, ArmyId red_id, int symmetry) const PURE {
        if (CHECK) {
            if (UNLIKELY(blue_id <= 0))
                throw_logic("blue_id <= 0");
            if (UNLIKELY(blue_id >= ARMYID_HIGHBIT))
                throw_logic("blue_id is too large");
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
    NOINLINE Board example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const PURE;
    NOINLINE Board random_example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const PURE;
    void print(ostream& os) const;
  private:
    inline BoardSubSet& at(ArmyId id) PURE {
        return static_cast<BoardSubSet&>(subsets_[id]);
    }
    BoardSubSet* begin() PURE { return &at(from()); }
    BoardSubSet* end()   PURE { return &at(top_); }
};

class BoardSetRed: public BoardSetBase {
    friend class BoardSubSetRedRef;
  public:
    BoardSetRed(bool keep = false, ArmyId size = INITIAL_SIZE) :
        BoardSetBase{keep, size} {}
    ~BoardSetRed() {
        for (auto& subset: *this)
            subset.destroy();
        // cout << "Destroy BoardSet " << static_cast<void const*>(subsets_) << "\n";
    }
    void clear(ArmyId size = INITIAL_SIZE);
    inline BoardSubSetRed const& cat(ArmyId id) const PURE {
        return static_cast<BoardSubSetRed const&>(subsets_[id]);
    }
    inline BoardSubSetRed const& at(ArmyId id) const PURE { return cat(id); }
    inline BoardSubSetRed const* begin() const PURE { return &cat(from()); }
    inline BoardSubSetRed const* end()   const PURE { return &cat(top_); }
    void insert(ArmyId blue_id, BoardSubSetRedBuilder& builder) {
        if (CHECK) {
            if (UNLIKELY(blue_id <= 0))
                throw_logic("red_id <= 0");
        }
        BoardSubSetRed subset_red = builder.extract();

        lock_guard<mutex> lock{exclude_};
        if (blue_id >= top_) {
            // Only in the multithreaded case can blue_id be different from top_
            // if (blue_id != top_) throw_logic("Cannot grow more than 1");
            while (blue_id > capacity_) resize();
            while (blue_id > top_) at(top_++).zero();
            ++top_;
        }
        at(blue_id) = subset_red;
        size_ += subset_red.size();
    }
    bool insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red);
    bool find(Board const& board, ArmySet const& armies_blue, ArmySet const& armies_red) const PURE;
    NOINLINE Board example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const PURE;
    NOINLINE Board random_example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const PURE;
  private:
    // Inefficient for core use, only meant for simple initialization
    bool _insert(ArmyId blue_id, ArmyId red_id, int symmetry, Statistics& stats);
    bool _find(ArmyId blue_id, ArmyId red_id, int symmetry) const PURE {
        if (CHECK) {
            if (UNLIKELY(blue_id <= 0))
                throw_logic("blue_id <= 0");
            if (UNLIKELY(blue_id >= ARMYID_HIGHBIT))
                throw_logic("blue_id is too large");
        }
        if (blue_id >= top_) return false;
        return cat(blue_id)._find(red_id, symmetry);
    }
    inline BoardSubSetRed& at(ArmyId id) PURE {
        return static_cast<BoardSubSetRed&>(subsets_[id]);
    }
    BoardSubSetRed* begin() PURE { return &at(from()); }
    BoardSubSetRed* end()   PURE { return &at(top_); }
};

inline ostream& operator<<(ostream& os, BoardSet const& set) {
    set.print(os);
    return os;
}

class BoardSubSetRefBase {
  public:
    ArmyId id() const PURE { return id_; }

    BoardSubSetRefBase(BoardSubSetRefBase const&) = delete;
    BoardSubSetRefBase& operator=(BoardSubSetRefBase const&) = delete;
    void keep() { id_ = 0; }
  protected:
    static void down_size(BoardSetBase& set, ArmyId size) { set.down_size(size); }

    BoardSubSetRefBase(BoardSubSetBase& subset, ArmyId id, bool keep): subset_{subset}, id_{id}, keep_{keep} {}
    BoardSubSetBase& subset_;
    ArmyId id_;
    bool const keep_;
};

class BoardSubSetRef: public BoardSubSetRefBase {
  public:
    ~BoardSubSetRef() { if (id_ && !keep_) _armies().destroy(); }
    BoardSubSetRef(BoardSet& set, ArmyId id): BoardSubSetRefBase{set.at(id), id, set.keep_} {
    if (id) down_size(set, armies().size());
}
    BoardSubSetRef(BoardSet& set): BoardSubSetRef{set, set.next()} {}
    BoardSubSet const& armies() const PURE {
        return static_cast<BoardSubSet const&>(subset_);
    }
  private:
    BoardSubSet& _armies() PURE {
        return static_cast<BoardSubSet&>(subset_);
    }
};

class BoardSubSetRedRef: public BoardSubSetRefBase {
  public:
    ~BoardSubSetRedRef() { if (id_ && !keep_) _armies().destroy(); }
    BoardSubSetRedRef(BoardSetRed& set, ArmyId id): BoardSubSetRefBase{set.at(id), id, set.keep_} {
    if (id) down_size(set, armies().size());
}
    BoardSubSetRedRef(BoardSetRed& set): BoardSubSetRedRef{set, set.next()} { }
    BoardSubSetRed const& armies() const PURE {
        return static_cast<BoardSubSetRed const&>(subset_);
    }
  private:
    BoardSubSetRed& _armies() PURE {
        return static_cast<BoardSubSetRed&>(subset_);
    }
};

class Image {
  public:
    inline Image() {
        clear();
    }
    inline Image(Army const& blue, Army const& red): Image{} {
        set(blue, BLUE);
        set(red,  RED);
    }
    inline explicit Image(Board const& board): Image{board.blue(), board.red()} {}
    inline explicit Image(Army const& army, Color color = BLUE): Image{} {
        set(army, color);
    }
    inline explicit Image(ArmyPos const& army, Color color = BLUE): Image{} {
        set(army, color);
    }
    inline explicit Image(ArmyZconst army, Color color = BLUE): Image{} {
        set(army, color);
    }
    inline void clear() { image_.fill(EMPTY); }
    inline Color get(Coord const& pos) const PURE { return image_[pos]; }
    inline Color get(int x, int y) const PURE { return get(Coord{x,y}); }
    inline void  set(Coord const& pos, Color c) { image_[pos] = c; }
    inline void  set(int x, int y, Color c) { set(Coord{x,y}, c); }
    inline void  set(ArmyZconst army, Color c) {
        for (auto const& pos: army) set(pos, c);
    }
    inline void  set(Army const& army, Color c) {
        for (auto const& pos: army) set(pos, c);
    }
    inline void  set(ArmyPos const& army, Color c) {
        for (auto const& pos: army) set(pos, c);
    }
    inline bool jumpable(Coord const& jumpee, Coord const& target) const PURE {
        return (get(target) >> get(jumpee)) != 0;
    }
    inline bool blue_jumpable(Coord const& jumpee, Coord const& target) const PURE {
        return ((get(target) << 2) & get(jumpee)) != 0;
    }
    inline bool red_jumpable(Coord const& jumpee, Coord const& target) const PURE {
        return ((get(target) + 3) & get(jumpee) & 4) != 0;
    }
    NOINLINE string str() const PURE;
    NOINLINE string str(Coord from, Coord to, Color c);
    Image& operator=(Image const& image) {
        std::copy(image.begin(), image.end(), begin());
        return *this;
    }
    void check(const char* file, int line) const;
  private:
    Color* begin() FUNCTIONAL { return image_.begin(); }
    Color* end  () PURE       { return image_.end(); }
    Color const* begin() const FUNCTIONAL { return image_.begin(); }
    Color const* end  () const FUNCTIONAL { return image_.end(); }
    inline string _str() const PURE;

    BoardTable<Color> image_;
};

inline ostream& operator<<(ostream& os, Image const& image) {
    os << image.str();
    return os;
}

inline ostream& operator<<(ostream& os, Board const& board) {
    os << Image{board};
    return os;
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
    if (moves.empty()) throw_logic("Empty full move");
    os << moves[0];
    for (size_t i=1; i<moves.size(); ++i)
        os << ", " << moves[i];
    return os;
}

class Svg {
  public:
    static uint const SCALE = 20;

    static string const solution_file(uint nr_moves) FUNCTIONAL {
        return file("solutions", nr_moves);
    }
    static string const attempts_file(uint nr_moves) FUNCTIONAL {
        return file("attempts", nr_moves);
    }
    static string const failures_file(uint nr_moves) FUNCTIONAL {
        return file("failures", nr_moves);
    }
    Svg(uint scale = SCALE) : scale_{scale}, margin_{scale/2} {}
    void write(time_t start_time, time_t stop_time,
               int solution_moves, BoardList const& boards,
               StatisticsList const& stats_list_solve, Sec::rep solve_duration,
               StatisticsList const& stats_list_backtrack, Sec::rep backtrack_duration);
    void parameters(time_t start_time, time_t stop_time);
    void game(BoardList const& boards);
    void stats(string const& cls, StatisticsList const& stats_list);
    void board(Board const& board) { board.svg(out_, scale_, margin_); }
    void move(FullMove const& move);
    void html(FullMoves const& full_moves);
    void html_header(uint nr_moves, int tatget_moves, bool terminated = false);
    void html_footer();
    void header();
    void footer();
    string str() const PURE { return out_.str(); }

  private:
    static string const file(string const& prefix, uint nr_moves) FUNCTIONAL;

    uint scale_;
    uint margin_;
    stringstream out_;
};

inline ostream& operator<<(ostream& os, Svg const& svg) {
    os << svg.str();
    return os;
}

class ThreadData {
  public:
    inline ThreadData(): signal_generation_{signal_generation.load(memory_order_relaxed)} {
    }
    inline ~ThreadData() {
        update_allocated();
    }
    inline bool signalled() {
        uint signal_gen = signal_generation.load(memory_order_relaxed);
        if (UNLIKELY(signal_generation_ != signal_gen)) {
            signal_generation_ = signal_gen;
            return true;
        }
        return false;
    }
    bool is_terminated() {
        return UNLIKELY(signal_generation_ & 1);
    }
  private:
    uint signal_generation_;

};

class Tables {
  public:
    Tables() {}
    void init();
    // The folowing methods in Tables are really only PURE. However they should
    // only ever be applied to the constant "tables" so they access
    // global memory that never changes making them effectively FUNCTIONAL
    uint min_nr_moves() const FUNCTIONAL { return min_nr_moves_; }
    // Use the less often changing coordinate as "slow"
    // This will keep more of the data in the L1 cache
    inline Norm distance(Coord const& slow, Coord const& fast) const FUNCTIONAL {
        return distance_[slow][fast];
    }
    inline Nbits Ndistance(Coord const& slow, Coord const& fast) const FUNCTIONAL {
        return NLEFT >> distance(slow, fast);
    }
    inline Nbits Ndistance_base_red(Coord const& pos) const FUNCTIONAL {
        return Ndistance_base_red_[pos];
    }
    inline uint8_t base_blue(Coord const& pos) const FUNCTIONAL {
        return base_blue_[pos];
    }
    inline uint8_t base_red(Coord const& pos) const FUNCTIONAL {
        return base_red_[pos];
    }
    inline uint8_t edge_red(Coord const& pos) const FUNCTIONAL {
        return edge_red_[pos];
    }
#if __BMI2__
    inline Parity parity(Coord const& pos) const PURE {
        // The intrinsics version seems to have the exact same speed as a
        // table fetch. But it should lower the pressure one the L1 cache a bit
        return _pext_u32(pos._pos(), 0x11);
    }
#else // __BMI2__
    inline Parity parity(Coord const& pos) const FUNCTIONAL {
        return parity_[pos];
    }
#endif // !__BMI2__
    inline Coords slide_targets(Coord const& pos) const FUNCTIONAL {
        return slide_targets_[pos];
    }
    inline Coords jumpees(Coord const& pos) const FUNCTIONAL {
        return jumpees_[pos];
    }
    inline Coords jump_targets(Coord const& pos) const FUNCTIONAL {
        return jump_targets_[pos];
    }
    inline uint8_t nr_slide_jumps_red(Coord const& pos) const FUNCTIONAL {
        return nr_slide_jumps_red_[pos];
    }
    inline Offsets const& slide_jumps_red(Coord const& pos) const FUNCTIONAL {
        return slide_jumps_red_[pos];
    }
    inline ParityCount const& parity_count() const FUNCTIONAL {
        return parity_count_;
    }
    inline Army const& army_red()  const FUNCTIONAL { return start().red(); }
    Norm   infinity() const FUNCTIONAL { return  infinity_; }
    Nbits Ninfinity() const FUNCTIONAL { return Ninfinity_; }
    Board const& start() const FUNCTIONAL { return start_; }

    void print_directions(ostream& os) const;
    void print_directions() const {
        print_directions(cout);
    }
    void print_Ndistance_base_red(ostream& os) const;
    inline void print_Ndistance_base_red() const {
        print_Ndistance_base_red(cout);
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
    void print_blue_parity_count(ostream& os) const;
    void print_blue_parity_count() const {
        print_blue_parity_count(cout);
    }
    void print_red_parity_count(ostream& os) const;
    void print_red_parity_count() const {
        print_red_parity_count(cout);
    }
    void print_nr_slide_jumps_red(ostream& os) const;
    void print_nr_slide_jumps_red() const {
        print_nr_slide_jumps_red(cout);
    }
  private:
    BoardTable<Coords>  slide_targets_;
    BoardTable<Coords>  jumpees_;
    BoardTable<Coords>  jump_targets_;
    BoardTable<Nbits>   Ndistance_base_red_;
    BoardTable<uint8_t> base_blue_;
    BoardTable<uint8_t> base_red_;
    BoardTable<uint8_t> edge_red_;
    BoardTable<uint8_t> nr_slide_jumps_red_;
    BoardTable<Offsets> slide_jumps_red_;
#if !__BMI2__
    BoardTable<Parity> parity_;
#endif // !__BMI2__
    BoardTable<BoardTable<Norm>> distance_;
    ParityCount parity_count_;
    Norm   infinity_;
    Nbits Ninfinity_;
    uint min_nr_moves_;
    Board start_;
};

extern Tables tables;

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

Nbits Coord::Ndistance_base_red() const {
    return tables.Ndistance_base_red(*this);
}

Coords Coord::slide_targets() const {
    return tables.slide_targets(*this);
}

Coords Coord::jumpees() const {
    return tables.jumpees(*this);
}

Coords Coord::jump_targets() const {
    return tables.jump_targets(*this);
}

uint8_t Coord::nr_slide_jumps_red() const {
    return tables.nr_slide_jumps_red(*this);
}
Offsets const& Coord::slide_jumps_red() const {
    return tables.slide_jumps_red(*this);
}

ArmyId ArmySetDense::insert(Army const& army, Statistics& stats) RESTRICT {
    // logger << "Insert:\n" << Image{army};
    // Leave hash calculation out of the mutex
    ArmyId hash = army.hash();
    lock_guard<mutex> lock{exclude_};
    // logger << "used_ = " << used_ << ", limit = " << limit_ << "\n" << flush;
    if (used_ >= limit_) resize();
    ArmyId const mask = mask_;
    ArmyId pos = hash & mask;
    ArmyId offset = 0;
    auto values = values_;
    while (true) {
        // logger << "Try " << pos << " of " << allocated() << "\n" << flush;
        ArmyId i = values[pos];
        if (i == 0) {
            stats.armyset_probe(offset);
            ArmyId id = ++used_;
            // logger << "Found empty, assign id " << id << "\n" << Image{army} << flush;
            values[pos] = id;
            at(id).append(army);
            return id;
        }
        if (army == cat(i)) {
            stats.armyset_probe(offset);
            stats.armyset_try();
            // logger << "Found duplicate " << hash << "\n" << flush;
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

ArmyId ArmySetDense::insert(ArmyPos const& army, Statistics& stats) RESTRICT {
    // logger << "Insert:\n" << Image{army};
    // Leave hash calculation out of the mutex
    ArmyId hash = army.hash();
    lock_guard<mutex> lock{exclude_};
    // logger << "used_ = " << used_ << ", limit = " << limit_ << "\n" << flush;
    if (used_ >= limit_) resize();
    ArmyId const mask = mask_;
    ArmyId pos = hash & mask;
    ArmyId offset = 0;
    auto values = values_;
    while (true) {
        // logger << "Try " << pos << " of " << allocated() << "\n" << flush;
        ArmyId i = values[pos];
        if (i == 0) {
            stats.armyset_probe(offset);
            ArmyId id = ++used_;
            // logger << "Found empty, assign id " << id << "\n" << Image{army} << flush;
            values[pos] = id;
            at(id).append(army);
            return id;
        }
        if (army == cat(i)) {
            stats.armyset_probe(offset);
            stats.armyset_try();
            // logger << "Found duplicate " << hash << "\n" << flush;
            return i;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

void ArmySetSparse::Group::append(DataCache& cache, GroupId group_id, uint pos, ArmyId army_id, Coord const* army, Statistics& stats) {
    if (bitmap_) {
        uint i = index(pos);
        uint n = bits();
        auto new_data = cache.allocate(group_id, n+1, stats);
        std::copy(&data_[0], &data_[i * Element::SIZE], &new_data[0]);
        auto new_element = &new_data[i * Element::SIZE];
        auto& element = Element::element(new_element);
        element.id(army_id);
        std::copy(army, army+ARMY, element.begin());
        std::copy(&data_[i * Element::SIZE], &data_[n * Element::SIZE],
                  new_element + Element::SIZE);
        cache.deallocate(n, data_, stats);
        data_ = new_data;
    } else {
        data_ = cache.allocate(group_id, 1, stats);
        auto& element = Element::element(data_);
        element.id(army_id);
        std::copy(army, army+ARMY, element.begin());
    }
    set(pos);
}

void ArmySetSparse::Group::append(DataCache& cache, GroupId group_id, uint pos, Element const& old_element) {
    char const* old_e = static_cast<char const*>(static_cast<void const *>(&old_element));
    if (bitmap_) {
        uint i = index(pos);
        uint n = bits();
        auto new_data = cache.allocate(group_id, n+1);
        std::copy(&data_[0], &data_[i * Element::SIZE], &new_data[0]);
        auto new_element = &new_data[i * Element::SIZE];
        std::copy(old_e, old_e + Element::SIZE, new_element);
        std::copy(&data_[i * Element::SIZE], &data_[n * Element::SIZE],
                  new_element + Element::SIZE);
        cache.deallocate(n, data_);
        data_ = new_data;
    } else {
        data_ = cache.allocate(group_id, 1);
        std::copy(old_e, old_e + Element::SIZE, data_);
    }
    set(pos);
}

ArmyId ArmySetSparse::insert(Army const& army, Statistics& stats) RESTRICT {
    // logger << "Insert:\n" << Image{army};
    // Leave hash calculation out of the mutex
    uint64_t hash = army.hash();
    lock_guard<mutex> lock{exclude_};
    // logger << "left_ = " << left_ << endl;
    if (left_ == 0) resize();
    GroupId const mask = mask_;
    Group* groups = groups_;
    uint64_t offset = 0;
    while (true) {
        GroupId group_id = (hash >> GROUP_BITS) & mask;
        auto& group = groups[group_id];
        uint pos = hash & GROUP_MASK;
        // logger << "Try [" << group_id << "," << pos<< "] of " << allocated() << "\n" << flush;
        if (group.bit(pos) == 0) {
            stats.armyset_probe(offset);
            --left_;
            ArmyId army_id = ++size_;
            // logger << "Found empty, assign id " << army_id << "\n" << Image{army} << flush;
            group.append(data_cache_, group_id, pos, army_id, army.begin(), stats);
            return army_id;
        }
        Element const& element = group.at(pos);
        if (army == element.armyZ()) {
            stats.armyset_probe(offset);
            stats.armyset_try();
            // logger << "Found duplicate " << hash << "\n" << flush;
            return element.id();
        }
        hash += ++offset;
    }
}

ArmyId ArmySetSparse::insert(ArmyPos const& army, Statistics& stats) RESTRICT {
    // logger << "Insert:\n" << Image{army};
    // Leave hash calculation out of the mutex
    uint64_t hash = army.hash();
    lock_guard<mutex> lock{exclude_};
    // logger << "left_ = " << left_ << endl;
    if (left_ == 0) resize();
    GroupId const mask = mask_;
    Group* groups = groups_;
    uint64_t offset = 0;
    while (true) {
        GroupId group_id = (hash >> GROUP_BITS) & mask;
        auto& group = groups[group_id];
        uint pos = hash & GROUP_MASK;
        // logger << "Try [" << group_id << "," << pos<< "] of " << allocated() << "\n" << flush;
        if (group.bit(pos) == 0) {
            stats.armyset_probe(offset);
            --left_;
            ArmyId army_id = ++size_;
            // logger << "Found empty, assign id " << army_id << "\n" << Image{army} << flush;
            group.append(data_cache_, group_id, pos, army_id, army.begin(), stats);
            return army_id;
        }
        Element const& element = group.at(pos);
        if (army == element.armyZ()) {
            stats.armyset_probe(offset);
            stats.armyset_try();
            // logger << "Found duplicate " << hash << "\n" << flush;
            return element.id();
        }
        hash += ++offset;
    }
}

void Board::do_move(FullMove const& move_, bool blue_to_move) {
    do_move(move_.move(), blue_to_move);
}

void Board::do_move(FullMove const& move_) {
    do_move(move_.move());
}

StatisticsE make_all_blue_moves_slow
(BoardSetRed& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves);

StatisticsE make_all_blue_moves_fast
(BoardSetRed& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves);

inline StatisticsE make_all_blue_moves
(BoardSetRed& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves) {
    return verbose || statistics || hash_statistics ?
        make_all_blue_moves_slow(boards_from, boards_to,
                                moving_armies, opponent_armies, moved_armies,
                                nr_moves) :
        make_all_blue_moves_fast(boards_from, boards_to,
                                moving_armies, opponent_armies, moved_armies,
                                nr_moves);
}

StatisticsE make_all_red_moves_slow
(BoardSet& boards_from,
 BoardSetRed& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves);

StatisticsE make_all_red_moves_fast
(BoardSet& boards_from,
 BoardSetRed& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves);

inline StatisticsE make_all_red_moves
(BoardSet& boards_from,
 BoardSetRed& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves) {
    return verbose || statistics || hash_statistics ?
        make_all_red_moves_slow(boards_from, boards_to,
                                moving_armies, opponent_armies, moved_armies,
                                nr_moves) :
        make_all_red_moves_fast(boards_from, boards_to,
                                moving_armies, opponent_armies, moved_armies,
                                nr_moves);
}

StatisticsE make_all_blue_moves_backtrack_slow
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves);

StatisticsE make_all_blue_moves_backtrack_fast
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves);

inline StatisticsE make_all_blue_moves_backtrack
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int nr_moves) {
    return verbose || statistics || hash_statistics ?
        make_all_blue_moves_backtrack_slow
        (boards_from, boards_to,
         moving_armies, opponent_armies, moved_armies,
         nr_moves) :
        make_all_blue_moves_backtrack_fast
        (boards_from, boards_to,
         moving_armies, opponent_armies, moved_armies,
         nr_moves);
}

StatisticsE make_all_red_moves_backtrack_slow
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int solution_moves,
 BoardTable<uint8_t> const& red_backtrack,
 BoardTable<uint8_t> const& red_backtrack_symmetric,
 int nr_moves);

StatisticsE make_all_red_moves_backtrack_fast
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int solution_moves,
 BoardTable<uint8_t> const& red_backtrack,
 BoardTable<uint8_t> const& red_backtrack_symmetric,
 int nr_moves);

inline StatisticsE make_all_red_moves_backtrack
(BoardSet& boards_from,
 BoardSet& boards_to,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet& moved_armies,
 int solution_moves,
 BoardTable<uint8_t> const& red_backtrack,
 BoardTable<uint8_t> const& red_backtrack_symmetric,
 int nr_moves) {
    return verbose || statistics || hash_statistics ?
        make_all_red_moves_backtrack_slow
        (boards_from, boards_to,
         moving_armies, opponent_armies, moved_armies,
         solution_moves, red_backtrack, red_backtrack_symmetric,
         nr_moves) :
        make_all_red_moves_backtrack_fast
        (boards_from, boards_to,
         moving_armies, opponent_armies, moved_armies,
         solution_moves, red_backtrack, red_backtrack_symmetric,
         nr_moves);
}
