#include <cmath>
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

bool const CHECK = false;
bool const VERBOSE = false;
bool const STATISTICS = false;
bool const SYMMETRY = true;
bool const MEMCHECK = false;

int const X_MAX = 16;
int const Y_MAX = 16;

int const X = 8;
int const Y = 8;
int const MOVES = 8;
int const ARMY = 10;
bool const CLOSED_LOOP = false;
bool const PASS = false;

uint64_t SEED = 123456789;

using Sec      = chrono::seconds;

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
    int x() const PURE { return (pos_+(X+(Y-1)*ROW)) % ROW - X; }
    int y() const PURE { return (pos_+(X+(Y-1)*ROW)) / ROW - (Y-1); }
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
    friend bool operator!=(Coord const& l, Coord const& r) {
        return l.pos_ != r.pos_;
    }
};
struct Move {
    Coord from, to;
    Move mirror() const PURE {
        return Move{from.mirror(), to.mirror()};
    }
};

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
        // if (pos_ < 0) throw(logic_error("Negative pos_"));
        if (pos_ < 0) abort();
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
    uint8_t map(Coord const& pos) {
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
    ArmyId size() const PURE {
        return used1_ - 1;
    }
    ArmyId max_size() const PURE {
        return size_;
    }
    ArmyId capacity() const PURE {
        return limit_;
    }
    Army const& at(ArmyId i) const PURE { return armies_[i]; }
    inline ArmyId insert(Army  const& value);
    inline ArmyId insert(ArmyE const& value);
    ArmyId find(Army const& value) const PURE;
    Army const* begin() const PURE { return &armies_[1]; }
    Army const* end()   const PURE { return &armies_[used1_]; }

    // Non copyable
    ArmySet(ArmySet const&) = delete;
    ArmySet& operator=(ArmySet const&) = delete;

  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(ceil(0.7*factor)); }
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

// Board as two Armies
class Board {
  public:
    Board() {}
    Board(Army const& blue, Army const& red): blue_{blue}, red_{red} {}
    void move(Move const& move_);
    void move(Move const& move_, bool blue_to_move);
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
  private:
    Army blue_, red_;

    friend bool operator==(Board const& l, Board const& r) {
        return l.blue() == r.blue() && l.red() == r.red();
    }
};

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

    bool insert(ArmyId to_move, int symmetry) {
        if (CHECK) {
            if (to_move <= 0) throw(logic_error("to_move <= 0"));
            if (to_move >= ARMY_HIGHBIT) throw(logic_error("to_move is too large"));
        }
        return insert(to_move | (symmetry < 0 ? ARMY_HIGHBIT : 0));
    }
    bool find(ArmyId to_move, int symmetry) const PURE {
        if (CHECK) {
            if (to_move <= 0) throw(logic_error("to_move <= 0"));
            if (to_move >= ARMY_HIGHBIT) throw(logic_error("to_move is too large"));
        }
        return find(to_move | (symmetry < 0 ? ARMY_HIGHBIT : 0));
    }

    static ArmyId split(ArmyId value, ArmyId& moving) {
        moving   = value & ARMY_MASK;
        // cout << "Split: Value=" << hex << value << ", index=" << moving << ", symmetry=" << (value & ARMY_HIGHBIT) << dec << "\n";
        return value & ARMY_HIGHBIT;
    }
    static size_t nr_armies() PURE { return nr_armies_; }

  private:
    static ArmyId constexpr FACTOR(ArmyId factor=1) { return static_cast<ArmyId>(ceil(0.7*factor)); }
    NOINLINE void resize();

    ArmyId* begin() PURE { return &armies_[0]; }
    ArmyId* end()   PURE { return &armies_[allocated()]; }

    bool insert(ArmyId id) {
        return _insert(id, false);
    }
    inline bool insert_new(ArmyId id) {
        return _insert(id, true);
    }
    inline bool _insert(ArmyId id, bool is_resize);
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

class BoardSet {
    friend class BoardSubSetRef;
  public:
    BoardSet(ArmyId size = 1, bool keep = false);
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
    bool insert(ArmyId to_move, ArmyId opponent, int symmetry) {
        if (CHECK) {
            if (opponent <= 0)
                throw(logic_error("opponent <= 0"));
            if (opponent >= ARMY_HIGHBIT)
                throw(logic_error("opponent is too large"));
        }
        lock_guard<mutex> lock{exclude_};
        bool result = grow_at(opponent).insert(to_move, symmetry);
        size_ += result;
        return result;
    }
    bool insert(Board const& board, ArmySet& army, ArmySet& opponent, int nr_moves);
    bool find(ArmyId to_move, ArmyId opponent, int symmetry) const PURE {
        if (CHECK) {
            if (opponent <= 0)
                throw(logic_error("opponent <= 0"));
            if (opponent >= ARMY_HIGHBIT)
                throw(logic_error("opponent is too large"));
        }
        return cat(opponent).find(to_move, symmetry);
    }
    bool find(Board const& board, ArmySet const& army, ArmySet const& opponent, int nr_moves) const PURE;
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

class BoardSubSetRef {
  public:
    BoardSubSetRef(BoardSet& set): BoardSubSetRef{set, set.next()} {}
    ~BoardSubSetRef() { if (id_) subset_.destroy(); }
    ArmyId id() const PURE { return id_; }
    BoardSubSet const& armies() const PURE { return subset_; }

    BoardSubSetRef(BoardSubSetRef const&) = delete;
    BoardSubSetRef& operator=(BoardSubSetRef const&) = delete;
    void keep() { id_ = 0; }
  private:
    BoardSubSetRef(BoardSet& set, ArmyId id): subset_{set.at(id)}, id_{id} {}
    BoardSubSet& subset_;
    ArmyId id_;
};

BoardSet::BoardSet(ArmyId size, bool keep): solution_id_{0}, size_{0}, capacity_{size+1}, from_{1}, top_{1}, keep_{keep} {
    subsets_ = new BoardSubSet[capacity_];
    // cout << "Create BoardSet " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
}

void BoardSet::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    from_ = top_ = 1;
    size_ = 0;
    solution_id_ = 0;
    if (true) {
        auto old_subsets = subsets_;
        ++size;
        subsets_ = new BoardSubSet[size];
        delete[] old_subsets;
        capacity_ = size;
    }
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

using Moves = array<Diff, MOVES>;
using TypeCount = array<int, 4>;
class Tables {
  public:
    Tables();
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
    inline uint8_t type(Coord const& pos) const PURE {
        return type_[pos];
    }
    inline Coord symmetric(Coord const& pos) const PURE {
        return symmetric_[pos];
    }
    // The folowing methods in Tables really only PURE. However they are only
    // ever applied to the constant tables so the access global memory that
    // never changes making them effectively FUNCTIONAL
    inline TypeCount const& type_count() const FUNCTIONAL {
        return type_count_;
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
    void print_type(ostream& os) const;
    void print_type() const {
        print_type(cout);
    }
    void print_symmetric(ostream& os) const;
    void print_symmetric() const {
        print_symmetric(cout);
    }
    void print_type_count(ostream& os) const;
    void print_type_count() const {
        print_type_count(cout);
    }
  private:
    TypeCount type_count_;
    Moves moves_;
    Norm infinity_;
    BoardTable<Coord> symmetric_;
    array<Norm, 2*Coord::MAX+1> norm_;
    array<Norm, 2*Coord::MAX+1> distance_;
    BoardTable<Norm> distance_base_red_;
    BoardTable<uint8_t> base_red_;
    BoardTable<uint8_t> edge_red_;
    BoardTable<uint8_t> type_;
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
            distance_base_red_[pos] = d > 2 ? d-2 : 0;
            edge_red_[pos] = d == 1;
            type_[pos] = y_type + x % 2;
            symmetric_[pos] = Coord(y, x);
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

void Tables::print_symmetric(ostream& os) const {
    for (int y=0; y < Y; ++y) {
        for (int x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << "|" << symmetric(pos);
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

Coord Coord::symmetric() const {
    return tables.symmetric(*this);
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
    delete [] values_;
}

void ArmySet::clear(ArmyId size) {
    stats_reset();
    ArmyId new_limit = FACTOR(size);
    if (new_limit >= ARMY_HIGHBIT)
        throw(overflow_error("Army size grew too large"));
    auto new_values = new ArmyId[size];
    auto new_armies = new Army[new_limit+1];
    delete [] values_;
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
            if (!is_resize) stats_update(offset);
            values_[pos] = used1_;
            auto& v = armies_[used1_];
            v = value;
            // cout << "Found empty\n";
            return used1_++;
        }
        auto& v = armies_[i];
        if (!is_resize && v == value) {
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
            if (!is_resize) stats_update(offset);
            values_[pos] = used1_;
            auto& v = armies_[used1_];
            v = value;
            // cout << "Found empty\n";
            return used1_++;
        }
        auto& v = armies_[i];
        if (!is_resize && v == value) {
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

bool BoardSet::insert(Board const& board, ArmySet& army_set, ArmySet& opponent_set, int nr_moves) {
    uint8_t blue_to_move = nr_moves & 1;

    Army const& army = blue_to_move ? board.blue() : board.red();
    Army const& opponent = blue_to_move ? board.red() : board.blue();

    auto army_symmetric = army.symmetric();
    int army_symmetry = cmp(army, army_symmetric);
    auto moving_id = army_set.insert(army_symmetry >= 0 ? army : army_symmetric);

    auto opponent_symmetric = opponent.symmetric();
    int opponent_symmetry = cmp(opponent, opponent_symmetric);
    auto opponent_id = opponent_set.insert(opponent_symmetry >= 0 ? opponent : opponent_symmetric);

    int symmetry = army_symmetry * opponent_symmetry;
    return insert(moving_id, opponent_id, symmetry);
}

bool BoardSet::find(Board const& board, ArmySet const& army_set, ArmySet const& opponent_set, int nr_moves) const {
    uint8_t blue_to_move = nr_moves & 1;

    Army const& army = blue_to_move ? board.blue() : board.red();
    auto army_symmetric = army.symmetric();
    int army_symmetry = cmp(army, army_symmetric);
    auto moving_id = army_set.find(army_symmetry >= 0 ? army : army_symmetric);
    if (moving_id == 0) return false;

    Army const& opponent = blue_to_move ? board.red() : board.blue();
    auto opponent_symmetric = opponent.symmetric();
    int opponent_symmetry = cmp(opponent, opponent_symmetric);
    auto opponent_id = opponent_set.find(opponent_symmetry >= 0 ? opponent : opponent_symmetric);
    if (opponent_id == 0) return false;

    int symmetry = army_symmetry * opponent_symmetry;
    return find(moving_id, opponent_id, symmetry);
}

void Image::clear() {
    board_ = tables.start_image().board_;
}

int Board::min_moves(bool blue_to_move) const {
    blue_to_move = blue_to_move ? true : false;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = NLEFT >> tables.infinity();
    int off_base_from = 0;
    TypeCount type_count_from = tables.type_count();
    int edge_count_from = 0;
    for (auto const& b: blue()) {
        --type_count_from[tables.type(b)];
        if (tables.base_red(b)) continue;
        ++off_base_from;
        edge_count_from += tables.edge_red(b);
        Ndistance_red |= tables.Ndistance_base_red(b);
        for (auto const& r: red())
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

// The classic 30 solution
// Used to check the code, especially the pruning
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

// When this is used it should not be accessed.
// Make it point to bad memory to force mayhem if it is
BoardTable<uint8_t>const *dummy_demand = nullptr;

// gcc is unable to inline the huge body if it done as a function
// Hack around it using the preprocessor
NOINLINE
uint make_moves(Army const& army, Army const& army_symmetric,
                Army const& opponent_army, ArmyId opponent_id, int opponent_symmetry,
                BoardSet& board_set, ArmySet& army_set, int available_moves) {
#define red_demand (*dummy_demand)
#define demand     false
#include "moves.cpp"
#undef demand
#undef red_demand
}

NOINLINE
uint make_moves_backtrack(Army const& army, Army const& army_symmetric,
                          Army const& opponent_army, ArmyId opponent_id, int opponent_symmetry,
                          BoardSet& board_set, ArmySet& army_set, int available_moves, BoardTable<uint8_t> const& red_demand) {
#define demand     true
#include "moves.cpp"
#undef demand
}

static inline
uint64_t _thread_make_all_moves(BoardSet& from_board_set,
                                BoardSet& to_board_set,
                                ArmySet const& moving_army_set,
                                ArmySet const& opponent_army_set,
                                ArmySet& moved_army_set,
                                int nr_moves,
                                bool backtrack,
                                BoardTable<uint8_t> const& red_demand) {
    uint64_t late = 0;
    while (true) {
        BoardSubSetRef subset{from_board_set};

        ArmyId opponent_id = subset.id();
        if (opponent_id == 0) break;
        auto const& opponent_army = opponent_army_set.at(opponent_id);
        if (CHECK) opponent_army.check(__LINE__);
        auto const opponent_symmetric = opponent_army.symmetric();
        int opponent_symmetry = cmp(opponent_army, opponent_symmetric);

        BoardSubSet const& armies = subset.armies();
        for (auto const& value: armies) {
            if (value == 0) continue;
            ArmyId moving_id;
            auto symmetry = BoardSubSet::split(value, moving_id);
            auto const& army = moving_army_set.at(moving_id);
            if (CHECK) army.check(__LINE__);
            Army army_symmetric = army.symmetric();
            late += backtrack ?
                make_moves_backtrack
                (symmetry ? army_symmetric : army,
                 symmetry ? army : army_symmetric,
                 opponent_army, opponent_id, opponent_symmetry,
                 to_board_set, moved_army_set, nr_moves,
                 red_demand) :
                make_moves
                (symmetry ? army_symmetric : army,
                 symmetry ? army : army_symmetric,
                 opponent_army, opponent_id, opponent_symmetry,
                 to_board_set, moved_army_set, nr_moves);
        }
        if (backtrack) subset.keep();
    }
    return late;
}

static NOINLINE
uint64_t thread_make_all_moves(BoardSet& from_board_set,
                        BoardSet& to_board_set,
                        ArmySet const& moving_army_set,
                        ArmySet const& opponent_army_set,
                        ArmySet& moved_army_set,
                        int nr_moves) {
    return _thread_make_all_moves(from_board_set, to_board_set,
                            moving_army_set, opponent_army_set, moved_army_set,
                            nr_moves, false, *dummy_demand);
}

static NOINLINE
uint64_t thread_make_all_moves_backtrack(BoardSet& from_board_set,
                                   BoardSet& to_board_set,
                                   ArmySet const& moving_army_set,
                                   ArmySet const& opponent_army_set,
                                   ArmySet& moved_army_set,
                                   int nr_moves,
                                   BoardTable<uint8_t> const& red_demand) {
    return _thread_make_all_moves(from_board_set, to_board_set,
                            moving_army_set, opponent_army_set, moved_army_set,
                            nr_moves, true, red_demand);
}

uint64_t _make_all_moves(BoardSet& from_board_set,
                         BoardSet& to_board_set,
                         ArmySet const& moving_army_set,
                         ArmySet const& opponent_army_set,
                         ArmySet& moved_army_set,
                         int nr_moves,
                         bool demand,
                         BoardTable<uint8_t> const& red_demand,
                         uint nr_threads = 0) {
    auto start = chrono::steady_clock::now();

    if (nr_threads == 0) nr_threads = thread::hardware_concurrency();
    vector<future<uint64_t>> results;
    uint64_t late;
    if (demand) {
        for (uint i=1; i < nr_threads; ++i)
            results.emplace_back
                (async
                 (launch::async, thread_make_all_moves_backtrack,
                  ref(from_board_set), ref(to_board_set),
                  ref(moving_army_set), ref(opponent_army_set), ref(moved_army_set),
                  nr_moves, ref(red_demand)));
        late = thread_make_all_moves_backtrack
            (from_board_set, to_board_set,
             moving_army_set, opponent_army_set, moved_army_set,
             nr_moves, red_demand);
    } else {
        for (uint i=1; i < nr_threads; ++i)
            results.emplace_back
                (async
                 (launch::async, thread_make_all_moves,
                  ref(from_board_set), ref(to_board_set),
                  ref(moving_army_set), ref(opponent_army_set), ref(moved_army_set),
                  nr_moves));
        late = thread_make_all_moves
            (from_board_set, to_board_set,
             moving_army_set, opponent_army_set, moved_army_set,
             nr_moves);
    }
    for (auto& result: results) late += result.get();

    auto stop = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop-start).count();
    moved_army_set.show_stats();
    // to_board_set.show_stats();
    if (MEMCHECK) cout << "nr armies in subsets=" << BoardSubSet::nr_armies() << "\n";
    cout << setw(6) << duration << " s, set " << setw(2) << nr_moves-1 << " done," << setw(10) << to_board_set.size() << " boards /" << setw(9) << moved_army_set.size() << " armies =" << setw(6) << to_board_set.size()/(moved_army_set.size() ? moved_army_set.size() : 1) << " " << get_memory() << endl;

    return late;
}

NOINLINE uint64_t
make_all_moves(BoardSet& from_board_set,
               BoardSet& to_board_set,
               ArmySet const& moving_army_set,
               ArmySet const& opponent_army_set,
               ArmySet& moved_army_set,
               int nr_moves, uint nr_threads = 0) {
    return _make_all_moves(from_board_set, to_board_set,
                           moving_army_set, opponent_army_set, moved_army_set,
                           nr_moves, false, *dummy_demand, nr_threads);
}

NOINLINE uint64_t
make_all_moves_backtrack(BoardSet& from_board_set,
                         BoardSet& to_board_set,
                         ArmySet const& moving_army_set,
                         ArmySet const& opponent_army_set,
                         ArmySet& moved_army_set,
                         int nr_moves,
                         BoardTable<uint8_t> const& red_demand,
                         uint nr_threads = 0) {
    return _make_all_moves(from_board_set, to_board_set,
                           moving_army_set, opponent_army_set, moved_army_set,
                           nr_moves, true, red_demand, nr_threads);
}

void play() {
    auto board = tables.start();

    int nr_moves = 30;
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
        board.move(move.mirror());
        if (board_set[1].find(board, army_set[1], army_set[2], nr_moves)) {
            cout << "Good\n";
        } else {
            cout << "Bad\n";
        }
        // cout << board;
    }
}

bool solve(Board const& board, int nr_moves, Army& red_army) {
    auto start_solve = chrono::steady_clock::now();
    array<BoardSet, 2> board_set;
    array<ArmySet, 3>  army_set;
    board_set[0].insert(board, army_set[0], army_set[1], nr_moves);
    cout << setw(14) << "set " << nr_moves << " done" << endl;
    auto const& final_to_board_set   = board_set[nr_moves % 2];
    for (int i=0; nr_moves>0; --nr_moves, ++i) {
        auto& from_board_set = board_set[ i    % 2];
        auto& to_board_set   = board_set[(i+1) % 2];
        to_board_set.clear();
        auto const& moving_army_set   = army_set[ i    % 3];
        auto const& opponent_army_set = army_set[(i+1) % 3];
        auto& moved_army_set    = army_set[(i+2) % 3];
        moved_army_set.clear();
        // uint64_t late = 0;

        make_all_moves(from_board_set, to_board_set,
                       moving_army_set, opponent_army_set, moved_army_set,
                       nr_moves);

        if (to_board_set.size() == 0) {
            auto stop_solve = chrono::steady_clock::now();
            auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            cout << setw(6) << duration << " s, no solution" << endl;
            return false;
        }
    }
    auto stop_solve = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
    cout << setw(6) << duration << " s, solved" << endl;

    ArmyId red_id = final_to_board_set.solution_id();
    if (red_id == 0) throw(logic_error("Solved without solution"));
    red_army = final_to_board_set.solution();
    return true;
}

void backtrack(Board const& board, int nr_moves,
               Army const& red_army) {
    cout << "Start backtracking\n";

    auto start_solve = chrono::steady_clock::now();
    vector<unique_ptr<BoardSet>> board_set;
    board_set.reserve(nr_moves+1);
    vector<unique_ptr<ArmySet>>  army_set;
    army_set.reserve(nr_moves+2);

    board_set.emplace_back(new BoardSet(1, true));
    army_set.emplace_back(new ArmySet);
    army_set.emplace_back(new ArmySet);
    board_set[0]->insert(board, *army_set[0], *army_set[1], nr_moves);

    BoardTable<uint8_t> red_demand{};
    red_demand.fill(0);
    red_demand.set(red_army, 2);

    cout << setw(14) << "set " << nr_moves << " done" << endl;
    for (int i=0; nr_moves>0; --nr_moves, ++i) {
        auto& from_board_set = *board_set[i];
        board_set.emplace_back(new BoardSet(1, true));
        auto& to_board_set   = *board_set[i+1];
        auto const& moving_army_set   = *army_set[i];
        auto const& opponent_army_set = *army_set[i+1];
        army_set.emplace_back(new ArmySet);
        auto& moved_army_set    = *army_set[i+2];

        make_all_moves_backtrack
            (from_board_set, to_board_set,
             moving_army_set, opponent_army_set, moved_army_set,
             nr_moves, red_demand);

        if (to_board_set.size() == 0)
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
    auto symmetry = BoardSubSet::split(red_value, red_id);
    // Backtracking forced the final red army to be the target red_army
    // So there can only be 1 final red army and it therefore has id 1
    if (red_id != 1)
        throw(logic_error("Unexpected red army id"));
    // And it was stored without flip
    if (symmetry)
        throw(logic_error("Unexpected red army symmetry"));
    if (true) {
        cout << "Blue: " << blue_id << ", Red: " << red_id << ", symmetry=" << symmetry << "\n";
        cout << Image{final_army_set.at(blue_id)};
    }

    //BoardSet work_board_set[2];
    //BoardSet const* from = &final_board_set;
    //BoardSet* to         = &work_board_set[1];
    //{
    //}
}

void my_main(int argc, char const* const* argv) {
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
        cout << "Type:\n";
        tables.print_type();
        cout << "Symmetric:\n";
        tables.print_symmetric();
        cout << "Red Base type count:\n";
        tables.print_type_count();
    }
    cout << start_board;
    int needed_moves = start_board.min_moves();
    cout << "Minimum possible number of moves: " << needed_moves << "\n";
    int nr_moves = needed_moves;
    if (argc > 1) {
        nr_moves = atoi(argv[1]);
        if (nr_moves < needed_moves) {
            if (nr_moves <= 0)
                throw(range_error("Number of moves must be positive"));
            cout << "No solution in " << nr_moves << " moves\n";
            return;
        }
    }

    get_memory(true);

    if (false) {
        play();
        return;
    }
    Army red_army;
    if (!solve(start_board, nr_moves, red_army)) return;
    backtrack(start_board, nr_moves, red_army);
}

int main(int argc, char** argv) {
    try {
        long tmp = sysconf(_SC_PAGE_SIZE);
        if (tmp == -1)
            throw(system_error(errno, system_category(),
                               "Could not determine PAGE SIZE"));
        PAGE_SIZE = tmp;

        my_main(argc, argv);
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
