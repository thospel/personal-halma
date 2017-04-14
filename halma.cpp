#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
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

bool prune_slide = false;
bool prune_jump  = false;
#define STRINGIFY(x) _STRINGIFY(x)
#define _STRINGIFY(x) #x
string VCS_COMMIT{STRINGIFY(COMMIT)};

// There is no fundamental limit. Just make up *SOME* bound
uint const THREADS_MAX = 256;
// 0 means let C++ decide
uint nr_threads = 0;

const char letters[] = "abcdefghijklmnopqrstuvwxyz";

uint64_t SEED = 123456789;

using Sec      = chrono::seconds;

string HOSTNAME;

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

thread_local uint tid;
atomic<uint> tids;
inline uint thread_id();
uint thread_id() {
    return tid;
}

inline string thread_name();
string thread_name() {
    return to_string(thread_id());
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
    if (mem >= 1000000) out << "(" << setw(6) << mem / 1000000  << " MB)";
    return out.str();
}

static mutex mutex_out_;
class LogBuffer: public std::streambuf {
  public:
    size_t const BLOCK = 80;

    LogBuffer(): prefix_{"Thread " + thread_name() + ": "} {
        buffer_.resize(BLOCK);
        setp(&buffer_[0], &buffer_[BLOCK]);
    }
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

int LogBuffer::overflow(int ch) {
    if (ch == EOF) return ch;
    int offset = pptr() - pbase();
    auto size = buffer_.size() * 2;
    buffer_.resize(size);
    setp(&buffer_[0], &buffer_[size]);
    pbump(offset+1);
    buffer_[offset] = ch;
    return ch;
}

int LogBuffer::sync() {
    int size = pbase() - pptr();
    if (size) {
        {
            std::lock_guard<std::mutex> lock{mutex_out_};
            cout << prefix_;
            cout.write(pbase(), pptr() - pbase());
        }
        pbump(size);
    }
    return 0;
}

class LogStream: public std::ostream {
  public:
    LogStream(): std::ostream{&buffer_} {}
  private:
    LogBuffer buffer_;
};

thread_local LogStream logger;

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
    static const CoordZ MIN;
    static const CoordZ MAX;

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

inline ostream& operator<<(ostream& os, CoordZ const& pos) {
    os << setw(2) << pos.x() << "," << setw(3) << pos.y();
    return os;
}

const CoordZ CoordZ::MIN = CoordZ{std::numeric_limits<value_type>::min()};
const CoordZ CoordZ::MAX = CoordZ{std::numeric_limits<value_type>::max()};

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

void CoordZ::svg(ostream& os, Color color, uint scale) const {
    os << "      <circle cx='" << (x()+1) * scale << "' cy='" << (y()+1) * scale<< "' r='" << static_cast<uint>(scale * 0.35) << "' fill='" << svg_color(color) << "' />\n";
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
    int symmetry() const;
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

ostream& operator<<(ostream& os, ArmyZ const& army) {
    for (auto const& pos: army)
        os << pos << "\n";
    return os;
}

int cmp(ArmyZ const& left, ArmyZ const& right) {
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

void ArmyZ::check(int line) const {
    // for (int i=0; i<ARMY; ++i) (*this)[i].check(line);
    for (int i=0; i<ARMY-1; ++i)
        if ((*this)[i] >= (*this)[i+1]) {
            cerr << *this;
            throw(logic_error("ArmyZ out of order at line " + to_string(line)));
        }
}

// ArmyZ as a set of CoordZ
class ArmyZE: public array<CoordZ, ARMY+2> {
  public:
    ArmyZE() {
        at(-1)   = CoordZ::MIN;
        at(ARMY) = CoordZ::MAX;
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

int cmp(ArmyZE const& left, ArmyZE const& right) {
    if (!SYMMETRY || X != Y) return 0;
    return memcmp(reinterpret_cast<void const *>(&left.at(0)),
                  reinterpret_cast<void const *>(&right.at(0)),
                  sizeof(left[0]) * ARMY);
}

bool operator==(ArmyZE const& l, ArmyZ const& r) {
    for (int i=0; i<ARMY; ++i)
        if (l.at(i) != r[i]) return false;
    return true;
}

bool operator==(ArmyZ const& l, ArmyZE const& r) {
    for (int i=0; i<ARMY; ++i)
        if (l[i] != r.at(i)) return false;
    return true;
}

ostream& operator<<(ostream& os, ArmyZE const& army) {
    for (int i=-1; i<=ARMY; ++i)
        os << army.at(i) << "\n";
    return os;
}

void ArmyZE::check(int line) const {
    for (int i=0; i<ARMY; ++i) at(i).check(line);
    for (int i=0; i<ARMY-1; ++i)
        if (at(i) >= at(i+1)) {
            cerr << *this;
            throw(logic_error("ArmyZE out of order at line " + to_string(line)));
        }
    if (at(-1) != CoordZ::MIN)
        throw(logic_error("ArmyZE wrong bottom at line " + to_string(line)));
    if (at(ARMY) != CoordZ::MAX)
        throw(logic_error("ArmyZE wrong top at line " + to_string(line)));
}

ArmyZ& ArmyZ::operator=(ArmyZE const& army) {
    std::copy(army.begin(), army.end(), begin());
    return *this;
}

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

ostream& operator<<(ostream& os, Army const& army) {
    for (auto const& pos: army)
        os << pos << "\n";
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

ArmyPair::ArmyPair(ArmyZ const& army) {
    for (int i=0; i < ARMY; ++i) {
        auto& p = army[i].coord_pair();
        normal_   [i] = p.normal();
        symmetric_[i] = p.symmetric();
    }
    sort(symmetric_.begin(), symmetric_.end());
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

class SetStatistics {
  public:
    SetStatistics(): hits_{0}, misses_{0}, tries_{0} {}
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
    atomic<uint64_t> hits_;
    atomic<uint64_t> misses_;
    atomic<uint64_t> tries_;
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

class ReadWriteLock {
  public:
    void lock_read() {
        unique_lock<mutex> lock{m};
        if (LOCK_DEBUG)
            logger << "lock_read()\n" << flush;
        cv.wait(lock, [this]{ return !writer; });
        ++readers;
        if (LOCK_DEBUG)
            logger << "Got read lock()\n" << flush;
    }
    void unlock_read() {
        unique_lock<mutex> lock{m};
        if (LOCK_DEBUG)
            logger << "unlock_read()\n" << flush;
        if (--readers == 0) cv.notify_one();
        if (LOCK_DEBUG)
            logger << "Dropped read lock()\n" << flush;
    }
    void lock_write() {
        unique_lock<mutex> lock{m};
        if (LOCK_DEBUG)
            logger << "Lock_write()\n" << flush;
        cv.wait(lock, [this]{ return readers == 0 && !writer; });
        writer = true;
        if (LOCK_DEBUG)
            logger << "Got write lock()\n" << flush;
    }
    void unlock_write() {
        unique_lock<mutex> lock{m};
        if (LOCK_DEBUG)
            logger << "Unlock_write()\n" << flush;
        writer = false;
        cv.notify_all();
        if (LOCK_DEBUG)
            logger << "Dropped write lock()\n" << flush;
    }
    void lock_upgrade() {
        unique_lock<mutex> lock{m};
        if (LOCK_DEBUG)
            logger << "Lock_upgrade()\n" << flush;
        --readers;
        cv.wait(lock, [this]{ return readers == 0 && !writer; });
        writer = true;
        if (LOCK_DEBUG)
            logger << "Upgraded lock()\n" << flush;
    }
    void lock_downgrade() {
        unique_lock<mutex> lock{m};
        if (LOCK_DEBUG)
            logger << "Lock_downgrade()\n" << flush;
        writer = false;
        ++readers;
        cv.notify_all();
        if (LOCK_DEBUG)
            logger << "Downgraded lock()\n" << flush;
    }
  private:
    condition_variable cv;
    mutex m;
    uint readers = 0;
    bool writer = false;
};

using ArmyId = uint32_t;
STATIC const int ARMY_BITS = std::numeric_limits<ArmyId>::digits;
STATIC const ArmyId ARMY_HIGHBIT = static_cast<ArmyId>(1) << (ARMY_BITS-1);

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
    size_t max_size() const PURE {
        return static_cast<size_t>(mask_) + 1;
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
    static ArmyId constexpr FACTOR(size_t factor=1) {
        return static_cast<ArmyId>(0.7*factor);
    }
    NOINLINE void resize();

    mutex exclude_;
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

Move::Move(ArmyZ const& army_from, ArmyZ const& army_to): from{-1,-1}, to{-1, -1} {
    ArmyZE const fromE{army_from};
    ArmyZE const toE  {army_to};

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

Move::Move(ArmyZ const& army_from, ArmyZ const& army_to, int& diffs): from{-1,-1}, to{-1, -1} {
    ArmyZE const fromE{army_from};
    ArmyZE const toE  {army_to};

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
    for (auto const& pos: blue())
        pos.svg(os, BLUE, scale);
    for (auto const& pos: red())
        pos.svg(os, RED,  scale);
}

class BoardSubSet {
  public:
    size_t allocated() const PURE { return static_cast<size_t>(mask_)+1; }
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

    inline bool insert(ArmyId red_id, ArmyId hash);
    inline bool find(ArmyId red_id, ArmyId hash) const PURE;

    static size_t nr_armies() PURE { return nr_armies_; }
    void print(ostream& os) const;
    void print() const { print(cout); }
  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(0.7*factor); }
    NOINLINE void resize();

    ArmyId* begin() PURE { return &armies_[0]; }
    ArmyId* end()   PURE { return &armies_[allocated()]; }

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

bool BoardSubSet::find(ArmyId red_id, ArmyId hash) const {
    if (CHECK && red_id <= 0)
        throw(logic_error("red_id <= 0"));
    auto mask = mask_;
    ArmyId pos = hash & mask;
    uint offset = 0;
    auto armies = armies_;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        auto& id = armies[pos];
        if (id == 0) return false;
        if (id == red_id) {
            // cout << "Found duplicate " << hash << "\n";
            return true;
        }
        ++offset;
        pos = (pos + offset) & mask;
    }
}

bool BoardSubSet::insert(ArmyId red_id, ArmyId hash) {
    // logger << "Insert " << red_id << "\n" << flush;
    if (CHECK && red_id <= 0)
        throw(logic_error("red_id <= 0"));
    if (left_ == 0) resize();
    auto mask = mask_;
    ArmyId pos = hash & mask;
    uint offset = 0;
    auto armies = armies_;
    while (true) {
        // logger << "Try " << pos << " of " << allocated() << "\n" << flush;
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

void BoardSubSet::resize() {
    auto old_armies = armies_;
    auto old_size = mask_+1;
    ArmyId size = old_size*2;
    auto new_armies = new ArmyId[size];
    armies_ = new_armies;
    // logger << "Resize BoardSubSet " << static_cast<void const *>(old_armies) << " -> " << static_cast<void const *>(armies_) << ": " << size << "\n" << flush;

    if (MEMCHECK) nr_armies_ -= allocated();
    auto mask = size-1;
    mask_ = size-1;
    if (MEMCHECK) nr_armies_ += allocated();
    left_ += FACTOR(size) - FACTOR(old_size);
    fill(begin(), end(), 0);
    for (ArmyId i = 0; i < old_size; ++i) {
        auto const& value = old_armies[i];
        if (value == 0) continue;
        // cout << "Insert " << value << "\n";
        ArmyId pos = hash64(value) & mask;
        uint offset = 0;
        while (new_armies[pos]) {
            // cout << "Try " << pos << " of " << mask+1 << "\n";
            ++offset;
            pos = (pos + offset) & mask;
        }
        new_armies[pos] = value;
        // cout << "Found empty\n";
    }
    delete [] old_armies;
}

void BoardSubSet::print(ostream& os) const {
    for (ArmyId red_id: *this)
        if (red_id) {
            ArmyId red_id;
            os << " " << red_id;
        } else {
            os << " x";
        }
}

class BoardSet {
    friend class BoardSubSetRef;
  public:
    static ArmyId const INITIAL_SIZE = 32;
    static ArmyId split(ArmyId blue_value, bool& symmetry) {
        symmetry = blue_value & 1;
        ArmyId blue_id = blue_value >> 1;
        // cout << "Split: Value=" << hex << blue_value << ", blue id=" << blue_id << ", symmetry=" << symmetry+0 << dec << "\n";
        return blue_id;
    }
    BoardSet(bool keep = false, ArmyId size = INITIAL_SIZE);
    ~BoardSet() {
        for (auto& subset: *this)
            subset.destroy();
        // cout << "Destroy BoardSet " << static_cast<void const*>(subsets_) << "\n";
        delete [] subsets_;
    }
    ArmyId subsets() const PURE { return top_ - from(); }
    ArmyId size() const PURE { return size_; }
    void clear(ArmyId size = INITIAL_SIZE);
    BoardSubSet const* begin() const PURE { return &subsets_[from()]; }
    BoardSubSet const* end()   const PURE { return &subsets_[top_]; }
    ArmyId back_id() const PURE { return top_-1; }
    BoardSubSet const&  at(ArmyId blue_id, int symmetry) const PURE { 
        ArmyId blue_value = blue_id << 1 | (symmetry < 0);
        return cat(blue_value);
    }
    inline bool insert(ArmyId blue_id, ArmyId red_id, int symmetry) {
        ArmyId red_hash = hash64(red_id);
        if (CHECK) {
            if (blue_id <= 0)
                throw(logic_error("red_id <= 0"));
            if (blue_id >= ARMY_HIGHBIT)
                throw(logic_error("opponent is too large"));
        }
        blue_id = blue_id << 1 | (symmetry < 0);

        lock_guard<mutex> lock{exclude_};

        if (blue_id >= top_) {
            while (blue_id >= capacity_) resize();
            while (blue_id >= top_)
                subsets_[top_++].create();
        }
        bool result = subsets_[blue_id].insert(red_id, red_hash);
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
    bool find(ArmyId blue_id, ArmyId red_id, int symmetry) const PURE {
        ArmyId red_hash = hash64(red_id);
        if (CHECK) {
            if (blue_id <= 0)
                throw(logic_error("blue_id <= 0"));
            if (blue_id >= ARMY_HIGHBIT)
                throw(logic_error("blue_id is too large"));
        }
        ArmyId blue_value = blue_id << 1 | (symmetry < 0);
        if (blue_value >= top_) return false;
        return cat(blue_value).find(red_id, red_hash);
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
    // Non copyable
    BoardSet(BoardSet const&) = delete;
    BoardSet& operator=(BoardSet const&) = delete;
    void print(ostream& os) const;
  private:
    static ArmyId const FROM = 2; // 1 << 1 | 0
    ArmyId capacity() const PURE { return capacity_-1; }
    ArmyId next() {
        lock_guard<mutex> lock{exclude_};
        return from_ < top_ ? from_++ : 0;
    }
    ArmyId from() const PURE { return keep_ ? FROM : from_; }
    BoardSubSet&  at(ArmyId id) PURE { return subsets_[id]; }
    BoardSubSet const&  at(ArmyId id) const PURE { return subsets_[id]; }
    BoardSubSet const& cat(ArmyId id) const PURE { return subsets_[id]; }
    NOINLINE void resize() {
        auto old_subsets = subsets_;
        subsets_ = new BoardSubSet[capacity_*2];
        capacity_ *= 2;
        // logger << "Resize BoardSet " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity_ << "\n" << flush;
        copy(&old_subsets[from()], &old_subsets[top_], &subsets_[FROM]);
        if (!keep_) {
            top_ -= from_ - FROM;
            from_ = FROM;
        }
        delete [] old_subsets;
    }
    BoardSubSet* begin() PURE { return &subsets_[from()]; }
    BoardSubSet* end()   PURE { return &subsets_[top_]; }

    mutex exclude_;
    ArmyZ solution_;
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

BoardSet::BoardSet(bool keep, ArmyId size): solution_id_{keep}, size_{0}, capacity_{size+1}, from_{FROM}, top_{FROM}, keep_{keep} {
    subsets_ = new BoardSubSet[capacity_];
    // cout << "Create BoardSet " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
}

void BoardSet::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    from_ = top_ = FROM;
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
    for (ArmyId blue_value = from(); blue_value < top_; ++blue_value) {
        bool symmetry;
        ArmyId blue_id = split(blue_value, symmetry);
        os << " Blue id " << blue_id << (symmetry ? "-" : "+") << ":";
        cat(blue_id).print(os);
        os << "\n";
    }
    os << "-----\n";
}

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

string Image::str() const {
    string result;
    result.reserve((2*X+3)*(Y+2));
    result += "+";
    for (int x=0; x < X; ++x) result += "--";
    result += "+\n";

    for (int y=0; y < Y; ++y) {
        result += "|";
        for (int x=0; x < X; ++x) {
            auto c = get(x, y);
            result += (c == EMPTY ? ". " :
                   c == RED   ? "X " :
                   c == BLUE  ? "O " :
                   "? ");
        }
        result += "|\n";
    }

    result += "+";
    for (int x=0; x < X; ++x) result += "--";
    result += "+\n";

    return result;
}

inline ostream& operator<<(ostream& os, Image const& image) {
    os << image.str();
    return os;
}

inline ostream& operator<<(ostream& os, Board const& board) {
    os << Image{board};
    return os;
}

void ArmyZSet::print(ostream& os) const {
    os << "[";
    for (ArmyId i=0; i < max_size(); ++i)
        os << " " << values_[i];
    os << " ] (" << static_cast<void const *>(this) << ")\n";
    for (ArmyId i=1; i < used1_; ++i) {
        os << "Army " << i << "\n" << Image{armies_[i]};
    }
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

CoordZ FullMove::from() const {
    if (size() == 0) throw(logic_error("Empty full_move"));
    return (*this)[0];
}

CoordZ FullMove::to()   const {
    if (size() == 0) throw(logic_error("Empty full_move"));
    return (*this)[size()-1];
}

Move FullMove::move() const {
    if (size() == 0) throw(logic_error("Empty full_move"));
    return Move{(*this)[0], (*this)[size()-1]};
}

void FullMove::move_expand(Board const& board_from, Board const& board_to, Move const& move) {
    emplace_back(move.from);
    Coord from{move.from};
    Coord to  {move.to};
    if (move.from.parity() != move.to.parity()) {
        // Must be a slide. Check though
        emplace_back(move.to);
        for (auto const& step: Coord::directions())
            if (to == Coord{from, step}) return;
        throw(logic_error("Move is not a slide but has different parity"));
    }

    // Must be a jump
    Image image{board_from};
    if (CLOSED_LOOP) image.set(move.from, EMPTY);
    array<Coord, ARMY*2*RULES+(1+RULES)> reachable;
    array<int, ARMY*2*RULES+(1+RULES)> previous;
    reachable[0] = from;
    int nr_reachable = 1;
    for (int i=0; i < nr_reachable; ++i) {
        for (auto direction: Coord::directions()) {
            Coord jumpee{reachable[i], direction};
            if (image.get(jumpee) != RED && image.get(jumpee) != BLUE) continue;
            Coord target{jumpee, direction};
            if (image.get(target) != EMPTY) continue;
            image.set(target, COLORS);
            previous [nr_reachable] = i;
            reachable[nr_reachable] = target;
            if (target == to) {
                array<Coord, ARMY*2*RULES+(1+RULES)> trace;
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
    Rules const& directions() const FUNCTIONAL { return directions_; }
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
    Board start_;
    Image start_image_;
};

Tables::Tables() {
    fill(norm_.begin(), norm_.end(), std::numeric_limits<Norm>::max());
    int rule = 0;
    infinity_ = 0;
    for (int y=1-Y; y < Y; ++y) {
        int ay = abs(y);
        for (int x=1-X; x < X; ++x) {
            int ax = abs(x);
            auto diff = Diff{x,y};
            Norm n =
                RULES == 8 ? max(ax, ay) :
                RULES == 6 ? (ax+ay+abs(x+y))/2 :
                RULES == 4 ? ax+ay :
                throw(logic_error("Unknown rule " + to_string(RULES)));
            norm_[diff.index2()] = n;
            distance_[diff.index2()] = n <= 2 ? 0 : n-2;
            infinity_ = max(infinity_, n);
            if (n == 1) {
                if (rule >= RULES) throw(logic_error("too many moves"));
                directions_[rule++] = diff;
            }
        }
    }
    if (rule < RULES) throw(logic_error("too few directions"));
    sort(directions_.begin(), directions_.end());
    if (infinity_ >= NBITS)
        throw(logic_error("Max distance does not fit in Nbits"));
    ++infinity_;

    // Fill base
    base_blue_.fill(0);
    base_red_ .fill(0);
    Army blue, red;
    auto& blueZ = start_.blue();
    auto& redZ  = start_.red();
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
            blueZ[i] = CoordZ{x, y};
            blue [i] = Coord {x, y};
            redZ[i]  = CoordZ{X-1-x, Y-1-y};
            red [i]  = Coord {X-1-x, Y-1-y};
            base_blue_[blue[i]] = 1;
            base_red_ [red [i]] = 1;
            ++i;
            --x;
            ++y;
        }
        d++;
    }
    sort(blueZ.begin(), blueZ.end());
    sort(redZ.begin(),  redZ.end());
    for (auto const& pos: blue)
        if (base_red_[pos]) throw(logic_error("Red and blue overlap"));

    for (int y=0; y < Y; ++y) {
        start_image_.set(-1, y, COLORS);
        start_image_.set( X, y, COLORS);
        Norm d = infinity_;
        Parity y_parity = y%2;
        for (int x=0; x < X; ++x) {
            auto pos  = Coord {x, y};
            auto posZ = CoordZ{x, y};
            coordZ_[pos] = posZ;
            symmetricZ_[posZ] = CoordZ{y, x};
            symmetric_ [pos ] = Coord {y, x};
            coord_pair_[posZ] = CoordPair{pos, Coord{y, x}};
            start_image_.set(pos, EMPTY);
            for (int i=0; i<ARMY; ++i) {
                Norm d1 = norm(pos, red[i]);
                if (d1 < d) d = d1;
            }
            distance_base_red_[pos] = d > 2 ? d-2 : 0;
            edge_red_[pos] = d == 1;
            Parity x_parity = x % 2;
            parity_pair_ [pos]  = ParityPair(
                2*y_parity + x_parity,
                2*x_parity + y_parity);
            parity_pairZ_[posZ] = ParityPair(
                2*y_parity + x_parity,
                2*x_parity + y_parity);
        }
    }
    for (int x=-1; x <= X; ++x) {
        start_image_.set(x, -1, COLORS);
        start_image_.set(x,  Y, COLORS);
    }

    parity_count_.fill(0);
    for (auto const& r: red)
        ++parity_count_[r.parity()];

    min_nr_moves_ = start_.min_nr_moves();
}

void Tables::print_directions(ostream& os) const {
    for (int i=0; i<RULES; ++i)
        os << directions_[i] << "\n";
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

void Tables::print_base_blue(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(base_blue(pos));
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

void Tables::print_parity_symmetric(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(parity_symmetric(pos));
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

void Tables::print_blue_parity_count(ostream& os) const {
    for (auto c: parity_count())
        os << " " << c;
    os << "\n";
}

void Tables::print_red_parity_count(ostream& os) const {
    for (auto c: parity_count())
        os << " " << c;
    os << "\n";
}

STATIC Tables const tables;

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

ArmyZSet::ArmyZSet(ArmyId size) : mask_{size-1}, used1_{1}, limit_{FACTOR(size)} {
    if (limit_ >= ARMY_HIGHBIT)
        throw(overflow_error("ArmyZSet size too large"));
    if (used1_ > limit_)
        throw(logic_error("ArmyZSet initial size too small"));
    values_ = new ArmyId[size];
    for (ArmyId i=0; i<size; ++i) values_[i] = 0;
    armies_ = new ArmyZ[limit_+1];
    // logger << "Create armies " << static_cast<void const *>(armies_) << "\n";
    // logger << "Create values " << static_cast<void const *>(values_) << "\n";
    // logger << flush;
}

ArmyZSet::~ArmyZSet() {
    delete [] armies_;
    // logger << "Destroy armies " << static_cast<void const *>(armies_) << "\n";
    if (values_) {
        // logger << "Destroy values " << static_cast<void const *>(values_) << "\n";
        delete [] values_;
    }
    // logger << flush;
}

void ArmyZSet::clear(ArmyId size) {
    ArmyId new_limit = FACTOR(size);
    if (new_limit >= ARMY_HIGHBIT)
        throw(overflow_error("ArmyZ size grew too large"));
    if (1 > limit_)
        throw(logic_error("ArmyZSet clear size too small"));
    // logger << "Clear\n";
    auto new_values = new ArmyId[size];
    // logger << "New value " << static_cast<void const *>(new_values) << "\n";
    auto new_armies = new ArmyZ[new_limit+1];
    // logger << "New armies " << static_cast<void const *>(new_armies) << "\n";
    if (values_) {
        delete [] values_;
        // logger << "delete old values " << static_cast<void const *>(values_) << "\n";
    }
    values_ = new_values;
    delete [] armies_;
    // logger << "delete old armies " << static_cast<void const *>(armies_) << "\n";
    // logger << flush;
    armies_ = new_armies;
    for (ArmyId i=0; i<size; ++i) values_[i] = 0;
    mask_ = size-1;
    limit_ = new_limit;
    used1_ = 1;
    stats_reset();
}

ArmyId ArmyZSet::find(ArmyZ const& army) const {
    ArmyId const mask = mask_;
    ArmyId pos = army.hash() & mask;
    auto values = values_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values[pos];
        if (i == 0) return 0;
        if (armies_[i] == army) return i;
        ++offset;
        pos = (pos + offset) & mask;
    }
}

ArmyId ArmyZSet::find(ArmyZE const& army) const {
    ArmyId const mask = mask_;
    ArmyId pos = army.hash() & mask;
    auto values = values_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values[pos];
        if (i == 0) return 0;
        if (armies_[i] == army) return i;
        ++offset;
        pos = (pos + offset) & mask;
    }
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

void ArmyZSet::resize() {
    delete [] values_;
    values_ = nullptr;

    if (mask_ >= ARMY_HIGHBIT)
        throw(overflow_error("ArmyZSet size grew too large"));
    size_t size = max_size() * 2;
    // logger << "Resize ArmyZSet: new size=" << size << "\n" << flush;
    auto new_values = new ArmyId[size];
    for (size_t i = 0; i < size; ++i) new_values[i] = 0;
    values_ = new_values;

    auto limit = FACTOR(size);
    auto new_armies = new ArmyZ[limit+1];
    auto mask = size-1;
    mask_   = mask;
    limit_  = limit;
    auto old_armies = armies_;
    auto used1 = used1_;
    for (ArmyId i = 1; i < used1; ++i) {
        ArmyZ const& army = old_armies[i];
        ArmyId hash = army.hash();
        ArmyId pos = hash & mask;
        ArmyId offset = 0;
        while (new_values[pos]) {
            ++offset;
            pos = (pos + offset) & mask;
        }
        new_values[pos] = i;
        new_armies[i] = army;
    }
    delete [] armies_;
    armies_ = new_armies;
    // logger << "Resize done\n" << flush;
}

bool BoardSet::insert(Board const& board, ArmyZSet& armies_blue, ArmyZSet& armies_red) {
    ArmyZ const& blue = board.blue();
    auto blue_symmetric = blue.symmetric();
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.insert(blue_symmetry >= 0 ? blue : blue_symmetric);

    ArmyZ const& red  = board.red();
    auto red_symmetric = red.symmetric();
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.insert(red_symmetry >= 0 ? red : red_symmetric);

    int symmetry = blue_symmetry * red_symmetry;
    return insert(blue_id, red_id, symmetry);
}

bool BoardSet::find(Board const& board, ArmyZSet const& armies_blue, ArmyZSet const& armies_red) const {
    ArmyZ const& blue = board.blue();
    auto blue_symmetric = blue.symmetric();
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.find(blue_symmetry >= 0 ? blue : blue_symmetric);
    if (blue_id == 0) return false;

    ArmyZ const& red = board.red();
    auto red_symmetric = red.symmetric();
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.find(red_symmetry >= 0 ? red : red_symmetric);
    if (red_id == 0) return false;

    int symmetry = blue_symmetry * red_symmetry;
    return find(blue_id, red_id, symmetry);
}

void Image::clear() {
    image_ = tables.start_image().image_;
}

int Board::min_nr_moves(bool blue_to_move) const {
    blue_to_move = blue_to_move ? true : false;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = NLEFT >> tables.infinity();
    int off_base_from = 0;
    ParityCount parity_count_from = tables.parity_count();
    int edge_count_from = 0;

    Army blue_{blue()};
    Army red_ {red()};
    for (auto const& b: blue_) {
        --parity_count_from[b.parity()];
        if (b.base_red()) continue;
        ++off_base_from;
        edge_count_from += b.edge_red();
        Ndistance_red |= b.Ndistance_base_red();
        for (auto const& r: red_)
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

void Board::do_move(Move const& move_, bool blue_to_move) {
    auto& army = blue_to_move ? blue() : red();
    auto pos = equal_range(army.begin(), army.end(), move_.from);
    if (pos.first == pos.second)
        throw(logic_error("Move not found"));
    *pos.first = move_.to;
    sort(army.begin(), army.end());
}

void Board::do_move(Move const& move_) {
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

void Board::do_move(FullMove const& move_, bool blue_to_move) {
    do_move(move_.move(), blue_to_move);
}

void Board::do_move(FullMove const& move_) {
    do_move(move_.move());
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
        "      <tr><th align='left'>Bound</th><td> &ge; " << tables.min_nr_moves() << " moves</td></tr>\n"
        "      <tr><th align='left'>Heuristics</th><td>";
    bool heuristics = false;
    if (balance >= 0) {
        if (heuristics) out_ << "<br />\n";
        heuristics = true;
        out_ << "balance=" << balance << ", delay=" << balance_delay;
    }
    if (prune_slide) {
        if (heuristics) out_ << "<br />\n";
        heuristics = true;
        out_ << "prune slides";
    }
    if (prune_jump) {
        if (heuristics) out_ << "<br />\n";
        heuristics = true;
        out_ << "prune jumps";
    }
    if (!heuristics)
        out_ << "None\n";
    out_ <<
        "</td>\n"
        "      <tr><th align='left'>Threads</th><td>" << nr_threads << "</td></tr>\n"
        "      <tr><th align='left'>Commit</th><td>" << VCS_COMMIT << "</td></tr>\n"
        "      <tr><th align='left'>Host</th><td>" << HOSTNAME << "</td></tr>\n"
        "    </table>\n";
}

void Svg::move(FullMove const& move) {
    out_ << "      <polyline points='";
    for (auto const& pos: move) {
        out_ << pos.x() * scale_ + scale_/2 + margin_ << "," << pos.y() * scale_ + scale_/2 + margin_ << " ";
    }
    out_ << "' stroke='black' stroke-width='" << scale_/10 << "' fill='none' marker-end='url(#arrowhead)' />\n";
}

void Svg::game(vector<Board> const& boards) {
    parameters(X, Y, ARMY, RULES);
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
                         ArmyZSet const& moving_armies,
                         ArmyZSet const& opponent_armies,
                         ArmyZSet& moved_armies,
                         int nr_moves,
                         int solution_moves,
                         BoardTable<uint8_t> const& red_backtrack,
                         BoardTable<uint8_t> const& red_backtrack_symmetric) {
    auto start = chrono::steady_clock::now();

    tids = 0;
    vector<future<uint64_t>> results;
    uint64_t late;
    int blue_to_move = nr_moves & 1;
    if (blue_to_move) {
        if (solution_moves > 0) {
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
        if (solution_moves > 0) {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, thread_red_moves_backtrack,
                      ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      ref(red_backtrack), ref(red_backtrack_symmetric),
                      solution_moves, nr_moves));
            late = thread_red_moves_backtrack
                (boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 red_backtrack, red_backtrack_symmetric,
                 solution_moves, nr_moves);
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
               ArmyZSet const& moving_armies,
               ArmyZSet const& opponent_armies,
               ArmyZSet& moved_armies,
               int nr_moves) {
    return _make_all_moves(boards_from, boards_to,
                           moving_armies, opponent_armies, moved_armies,
                           nr_moves, 0, *dummy_backtrack, *dummy_backtrack);
}

NOINLINE uint64_t
make_all_moves_backtrack(BoardSet& boards_from,
                         BoardSet& boards_to,
                         ArmyZSet const& moving_armies,
                         ArmyZSet const& opponent_armies,
                         ArmyZSet& moved_armies,
                         int nr_moves, int solution_moves,
                         BoardTable<uint8_t> const& red_backtrack,
                         BoardTable<uint8_t> const& red_backtrack_symmetric) {
    return _make_all_moves(boards_from, boards_to,
                           moving_armies, opponent_armies, moved_armies,
                           nr_moves, solution_moves, red_backtrack, red_backtrack_symmetric);
}

void play(bool print_moves=false) {
    auto board = tables.start();
    Board previous_board;
    if (print_moves) previous_board = board;

    FullMoves moves;
    FullMoves game;
    if (X == 4 && Y == 4 && ARMY == 6 && RULES == 8) {
        game = FullMoves{
            "c4-a4",
            "c1-d1",
            "d4-c4",
            "b2-d4",
            "c4-b3",
            "a2-c4",
            "b4-b2",
            "a3-b4",
            "d3-c2",
            "b1-d3",
            "c3-a3",
            "a1-c3",
            "d2-c1",
            "d1-d2",
        };
    } else if (X == 9 && Y == 9 && ARMY == 10 && RULES == 6) {
        // The classic 30 solution
        // Used to check the code, especially the pruning
        game = FullMoves{
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
    } else {
        throw(logic_error("No hardcoded replay game"));
    }

    int nr_moves = game.size();
    for (auto& move: game) {
        cout << board;
        // cout << board.symmetric();

        BoardSet board_set[2];
        ArmyZSet  army_set[3];
        board_set[0].insert(board, army_set[0], army_set[1], nr_moves);

        make_all_moves(board_set[0], board_set[1],
                       army_set[0], army_set[1], army_set[2], nr_moves);

        cout << "===============================\n";
        --nr_moves;
        board.do_move(move);
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

int solve(Board const& board, int nr_moves, ArmyZ& red_army) {
    auto start_solve = chrono::steady_clock::now();
    array<BoardSet, 2> board_set;
    array<ArmyZSet, 3>  army_set;
    board_set[0].insert(board, army_set[0], army_set[1], nr_moves);
    cout << setw(14) << "set " << setw(2) << nr_moves << " done (" << HOSTNAME;
    bool heuristics = false;
    if (balance >= 0) {
        heuristics = true;
        cout << ", balance=" << balance << ", delay=" << balance_delay;
    }
    if (prune_slide) {
        heuristics = true;
        cout << ", prune slides";
    }
    if (prune_jump) {
        heuristics = true;
        cout << ", prune jumps";
    }
    if (!heuristics)
        cout << ", no heuristics";
    cout << ")" << endl;

    ArmyId red_id = 0;
    int i;
    for (i=0; nr_moves>0; --nr_moves, ++i) {
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
            return -1;
        }
        if (boards_to.solution_id()) {
            red_id = boards_to.solution_id();
            red_army = boards_to.solution();
            if (nr_moves > 1)
                cout << "Unexpected early solution. Bailing out\n";
            ++i;
            break;
        }
    }
    auto stop_solve = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
    cout << setw(6) << duration << " s, solved" << endl;

    if (red_id == 0) throw(logic_error("Solved without solution"));
    return i;
}

void backtrack(Board const& board, int nr_moves, int solution_moves,
               ArmyZ const& last_red_army) {
    cout << "Start backtracking\n";

    auto start_solve = chrono::steady_clock::now();
    vector<unique_ptr<BoardSet>> board_set;
    board_set.reserve(nr_moves+1);
    vector<unique_ptr<ArmyZSet>>  army_set;
    army_set.reserve(nr_moves+2);

    board_set.emplace_back(new BoardSet(true));
    army_set.emplace_back(new ArmyZSet);
    army_set.emplace_back(new ArmyZSet);
    board_set[0]->insert(board, *army_set[0], *army_set[1], nr_moves);

    ArmyPair const last_army{last_red_army};
    BoardTable<uint8_t> red_backtrack{};
    red_backtrack.fill(0);
    red_backtrack.set(last_army.normal(), 2);
    BoardTable<uint8_t> red_backtrack_symmetric{};
    red_backtrack_symmetric.fill(0);
    red_backtrack_symmetric.set(last_army.symmetric(), 2);
    if (VERBOSE) {
        cout << "red_backtrack:\n";
        for (int y=0; y<Y; ++y) {
            for (int x=0; x<X; ++x)
                cout << " " << static_cast<uint>(red_backtrack[Coord{x, y}]);
            cout << "\n";
        }
        cout << "red_backtrack_symmetric:\n";
        for (int y=0; y<Y; ++y) {
            for (int x=0; x<X; ++x)
                cout << " " << static_cast<uint>(red_backtrack_symmetric[Coord{x, y}]);
            cout << "\n";
        }
    }

    cout << setw(14) << "set " << setw(2) << nr_moves << " done" << endl;
    for (int i=0; solution_moves>0; --nr_moves, --solution_moves, ++i) {
        board_set.emplace_back(new BoardSet(true));
        auto& boards_from = *board_set[i];
        auto& boards_to   = *board_set[i+1];

        army_set.emplace_back(new ArmyZSet);
        auto const& moving_armies   = *army_set[i];
        auto const& opponent_armies = *army_set[i+1];
        auto& moved_armies        = *army_set[i+2];

        make_all_moves_backtrack
            (boards_from, boards_to,
             moving_armies, opponent_armies, moved_armies,
             nr_moves, solution_moves, red_backtrack, red_backtrack_symmetric);

        if (boards_to.size() == 0)
            throw(logic_error("No solution while backtracking"));
    }
    auto stop_solve = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
    cout << setw(6) << duration << " s, backtrack tables built" << endl;

    // Do some sanity checking
    BoardSet const& final_board_set = *board_set.back();
    ArmyZSet const& final_army_set = *army_set.back();

    ArmyId blue_id;
    if (nr_moves == solution_moves) {
        // There should be only 1 blue army completely on the red base
        if (final_army_set.size() != 1)
            throw(logic_error("More than 1 final blue army"));
        ArmyId blue_value = final_board_set.back_id();
        bool symmetry;
        blue_id = BoardSet::split(blue_value, symmetry);
        if (blue_id != 1)
            throw(logic_error("Unexpected blue army id " + to_string(blue_id)));
        if (symmetry)
            throw(logic_error("Unexpected flipped blue army"));
        // There should be only 1 final board
        if (final_board_set.size() != 1)
            throw(logic_error("More than 1 solution while backtracking"));
    } else {
        ArmyZ blue_army = tables.start().red();
        blue_id = final_army_set.find(blue_army);
        if (blue_id == 0)
            throw(logic_error("Could not find final blue army"));
    }

    ArmyZ blueZ = final_army_set.at(blue_id);
    ArmyZ blueSymmetricZ = blueZ.symmetric();
    int blue_symmetry = cmp(blueZ, blueSymmetricZ);
    // And it was stored without flip
    if (blue_symmetry < 0)
        throw(logic_error("Unexpected blue army skewed"));

    BoardSubSet const& final_subset = final_board_set.at(blue_id, blue_symmetry);
    if (final_subset.size() != 1)
        throw(logic_error("More than 1 final red army"));
    ArmyId red_id = 0;
    for (ArmyId value: final_subset)
        if (value != 0) {
            red_id = value;
            break;
        }
    if (red_id == 0)
        throw(logic_error("Could not find final red army"));
    // Backtracking forced the final red army to be last_red_army
    // So there can only be 1 final red army and it therefore has army id 1
    if (red_id != 1)
        throw(logic_error("Unexpected red army id " +to_string(red_id)));

    ArmyZ redZ  = last_red_army;

    // It's probably more useful to generate a FullMove sequence
    // instead of a board sequence. Punt for now. --Note

    vector<Board> boards;
    // Reserve nr_moves+1 boards
    size_t board_pos = board_set.size();
    boards.resize(board_pos);
    boards[--board_pos] = Board{blueZ, redZ};

    if (false) {
        cout << "Initial\n";
        cout << "Blue: " << blue_id << ", Red: " << red_id << ", blue symmetry=" << blue_symmetry << "\n";
        cout << boards[board_pos];
    }

    board_set.pop_back();
    army_set.pop_back();
    army_set.pop_back();

    ArmyZ redSymmetricZ  = redZ.symmetric();
    int red_symmetry  = cmp(redZ , redSymmetricZ);

    Image image{blueZ, redZ};
    ArmyZPos armyE, armySymmetricE;
    for (int blue_to_move = 1;
         board_set.size() != 0;
         blue_to_move = 1-blue_to_move, board_set.pop_back(), army_set.pop_back()) {
        // cout << "Current image:\n" << image;
        BoardSet const& back_boards   = *board_set.back();
        ArmyZSet const& back_armies   = *army_set.back();

        ArmyZ const& armyZ          = blue_to_move ? blueZ          : redZ;
        ArmyZ const& armySymmetricZ = blue_to_move ? blueSymmetricZ : redSymmetricZ;
        ArmyMapper const& mapper{armySymmetricZ};
        Army army{armyZ};
        for (int a=0; a<ARMY; ++a) {
            auto const soldier = army[a];
            image.set(soldier, EMPTY);
            armyE         .copy(armyZ         , a);
            armySymmetricE.copy(armySymmetricZ, mapper.map(soldier));
            array<Coord, ARMY*2*RULES+(1+RULES)> reachable;

            // Jumps
            reachable[0] = soldier;
            int nr_reachable = 1;
            if (!CLOSED_LOOP) image.set(soldier, COLORS);
            for (int i=0; i < nr_reachable; ++i) {
                for (auto move: Coord::directions()) {
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

            // Slides
            for (auto move: Coord::directions()) {
                Coord target{soldier, move};
                if (image.get(target) != EMPTY) continue;
                reachable[nr_reachable++] = target;
            }

            for (int i=1; i < nr_reachable; ++i) {
                auto const val = reachable[i];
                CoordZ valZ{val};
                armyE         .store(valZ);
                armySymmetricE.store(valZ.symmetric());
                if (blue_to_move) {
                    blue_symmetry = cmp(armyE, armySymmetricE);
                    blue_id = back_armies.find(blue_symmetry >= 0 ? armyE : armySymmetricE);
                    if (blue_id == 0) continue;
                    int symmetry = blue_symmetry * red_symmetry;
                    auto board_id = back_boards.find(blue_id, red_id, symmetry);
                    if (board_id == 0) continue;
                    image.set(val,     BLUE);
                    blueZ          = armyE;
                    blueSymmetricZ = armySymmetricE;
                } else {
                    red_symmetry = cmp(armyE, armySymmetricE);
                    red_id = back_armies.find(red_symmetry >= 0 ? armyE : armySymmetricE);
                    if (red_id == 0) continue;
                    int symmetry = red_symmetry * blue_symmetry;
                    auto board_id = back_boards.find(blue_id, red_id, symmetry);
                    if (board_id == 0) continue;
                    image.set(val,     RED);
                    redZ          = armyE;
                    redSymmetricZ = armySymmetricE;
                }
                goto MOVE_DONE;
            }
            image.set(soldier, blue_to_move ? BLUE : RED);
        }
        cerr << "Blue:\n" << blueZ;
        cerr << "Red:\n" << redZ;
        cerr << "Failure board:\n" << image;
        throw(logic_error("Could not identify backtracking move"));
      MOVE_DONE:
        boards[--board_pos] = Board{blueZ, redZ};
    }
    // cout << "Final image:\n" << image;

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
    GetOpt options("b:B:t:sjp", argv);
    long long int val;
    bool replay = false;
    while (options.next())
        switch (options.option()) {
            case 'b': balance       = atoi(options.arg()); break;
            case 'B': balance_delay = atoi(options.arg()); break;
            case 'p': replay = true; break;
            case 's': prune_slide = true; break;
            case 'j': prune_jump  = true; break;
            case 't':
              val = atoll(options.arg());
              if (val < 0)
                  throw(range_error("Number of threads cannot be negative"));
              if (val > THREADS_MAX)
                  throw(range_error("Too many threads"));
              nr_threads = val;
              break;
            default:
              cerr << "usage: " << argv[0] << " [-t threads] [-b balance] [-B balance_delay] [-s] [-j] [-p]\n";
              exit(EXIT_FAILURE);
        }
    balance_min = ARMY     / 4 - balance;
    balance_max = (ARMY+3) / 4 + balance;
    if (nr_threads == 0) nr_threads = thread::hardware_concurrency();

    cout << "Commit: " << VCS_COMMIT << "\n";
    auto start_board = tables.start();
    if (false) {
        cout << "Infinity: " << static_cast<uint>(tables.infinity()) << "\n";
        tables.print_directions();
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
        cout << "Parity Symmetric:\n";
        tables.print_parity_symmetric();
        cout << "Symmetric:\n";
        tables.print_symmetric();
        cout << "Blue Base parity count:\n";
        tables.print_blue_parity_count();
        cout << "Red Base parity count:\n";
        tables.print_red_parity_count();
    }

    int needed_moves = tables.min_nr_moves();
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

    ArmyZ red_army;
    int solution_moves = solve(start_board, nr_moves, red_army);
    if (solution_moves < 0) return;
    backtrack(start_board, nr_moves, solution_moves, red_army);
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
