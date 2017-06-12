#define SLOW 1
#define ONCE 1
#include "halma.hpp"

#include <fstream>

#include <random>

uint X = 0;
uint Y = 0;
uint RULES = 6;
uint ARMY = 10;

size_t Element::SIZE;

Align ARMY_MASK;
Align ARMY_MASK_NOT;
Align NIBBLE_LEFT;
Align NIBBLE_RIGHT;
uint ARMY_ALIGNED;
uint ARMY_PADDING;
uint ARMY64_DOWN;

uint ARMY_SUBSET_BITS  = 4;
uint ARMY_SUBSETS      = 1 << ARMY_SUBSET_BITS;
uint ARMY_SUBSETS_MASK = ARMY_SUBSETS-1;

int balance = -1;
int balance_delay = 0;
int balance_min, balance_max;

int example = 0;
int verbose_move = 0;
bool prune_slide = false;
bool prune_jump  = false;

bool statistics = false;
bool hash_statistics = false;
bool verbose = false;
bool attempt = true;
char const* sample_subset_red = nullptr;
char const* red_file = nullptr;

// Flags without specific meaning. Used in experiments
bool testq = false;
int  testQ = 0;

#include "pdqsort.h"
template <class RandomAccessIterator>
inline void lib_sort(RandomAccessIterator first, RandomAccessIterator last) ALWAYS_INLINE;
template <class RandomAccessIterator>
void lib_sort(RandomAccessIterator first, RandomAccessIterator last) {
    // std::sort(first, last);
    pdqsort_branchless(first, last);
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

class Diff {
  public:
    Diff() {}
    Diff(int dx, int dy): dx_{dx}, dy_{dy} {}
    int dx() const PURE { return dx_; }
    int dy() const PURE { return dy_; }
  private:
    int dx_;
    int dy_;

    friend inline bool operator<(Diff const& l, Diff const& r) {
        if (l.dy_ < r.dy_) return true;
        if (l.dy_ > r.dy_) return false;
        return l.dx_ < r.dx_;
    }
};

inline ostream& operator<<(ostream& os, Diff const& diff) {
    os << "[" << setw(2) << diff.dx() << "," << setw(2) << diff.dy() << "]";
    return os;
}

Align AlignFill(uint8_t byte) PURE;
Align AlignFill(uint8_t byte) {
    Align val;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(&val);
    std::fill(ptr, ptr + sizeof(val), byte);
    return val;
}

inline bool heuristics() {
    return balance >= 0 || prune_slide || prune_jump;
}

std::random_device rnd;
template <class T>
T random(T range) {
    std::uniform_int_distribution<T> dist(0, range-1);
    return dist(rnd);
}

template <class T>
T gcd(T small, T big) {
    while (small) {
        T r = big % small;
        big = small;
        small = r;
    }
    return big;
}

ArmyId random_coprime(ArmyId range) {
    std::uniform_int_distribution<ArmyId> dist(1, range-1);
    while (true) {
        ArmyId i = dist(rnd);
        if (gcd(i, range) == 1) return i;
    }
}

Align Coord::ArmyMask() {
    Align result;
    Coord* ptr = reinterpret_cast<Coord *>(&result);
    uint i = 0;
    uint n = ARMY % ALIGNSIZE;
    while (i < n)         ptr[i++] = Coord{0};
    while (i < ALIGNSIZE) ptr[i++] = Coord::MAX();
    return result;
}

void Coord::print(ostream& os, Align align) {
    Coord* ptr = reinterpret_cast<Coord *>(&align);
    os << hex << "[ ";
    for (uint i=0; i<ALIGNSIZE; ++i) os << setw(2) << static_cast<uint>(ptr[i]._pos()) << " ";
    os << "]" << dec;
}

void Coord::svg(ostream& os, Color color, uint scale) const {
    os << "      <circle cx='" << (x()+1) * scale << "' cy='" << (y()+1) * scale<< "' r='" << static_cast<uint>(scale * 0.35) << "' fill='" << svg_color(color) << "' />\n";
}

ostream& operator<<(ostream& os, Army const& army) {
    for (auto const pos: army)
        os << pos << "\n";
    return os;
}

void Army::check(const char* file, int line) const {
    for (uint i=0; i<ARMY; ++i) (*this)[i].check(file, line);
    for (uint i=0; i<ARMY-1; ++i)
        if ((*this)[i] >= (*this)[i+1]) {
            cerr << static_cast<void const *>(this) << "\n" << *this;
            throw_logic("Army out of order", file, line);
        }
    if (DO_ALIGN)
        for (uint i=ARMY; i < ARMY+ARMY_PADDING; ++i)
            if ((*this)[i] != Coord::MAX())
                throw_logic("Badly terminated army", file, line);
}

void Army::sort(Coord* RESTRICT base) {
    lib_sort(base, base+ARMY);
}

void Army::_import_symmetric(Coord const* RESTRICT from, Coord* RESTRICT to) {
    // logger << "Before:\n" << ArmyZconst{*from};
    std::array<uint8_t,MAX_Y> count;
    std::array<std::array<Coord, MAX_ARMY>, MAX_Y> work;
    std::memset(count.begin(), 0, sizeof(count));

    for (uint i=0; i<ARMY; ++i, ++from) {
        Coord symmetric = from->symmetric();
        work[symmetric.y()][count[symmetric.y()]++] = symmetric;
        // logger << "Assign " << symmetric << " to " << symmetric.y() << " count " << count[symmetric.y()] << "\n";
    }
    auto y = Y;
    for (uint i=0; i<y; ++i)
        for (uint j=0; j<count[i]; ++j)
            *to++ = work[i][j];
    // logger << "After:\n" << ArmyZconst{*(to-ARMY)} << flush;
}

void ArmyPos::check(char const* file, int line) const {
    for (uint i=0; i<ARMY; ++i) at(i).check(file, line);
    for (uint i=0; i<ARMY-1; ++i)
        if (at(i) >= at(i+1)) {
            cerr << static_cast<void const *>(this) << "\n" << *this;
            throw_logic("ArmyPos out of order", file, line);
        }
    if (at(-1) != Coord::MIN())
        throw_logic("ArmyPos wrong bottom", file, line);
    if (DO_ALIGN)
        for (uint i=ARMY; i < ARMY+ARMY_PADDING; ++i) {
            if (at(i) != Coord::MAX())
                throw_logic("Badly terminated army", file, line);
        }
    else if (at(ARMY) != Coord::MAX())
        throw_logic("ArmyPos wrong top", file, line);
    if (pos_ < 0 || pos_ >= static_cast<int>(ARMY))
        throw_logic("ArmyPos position " + to_string(pos_) +
                          " out of range", file, line);
}

ostream& operator<<(ostream& os, ArmyPos const& army) {
    os << "pos=" << army.pos_ << "\n";
    for (int i=-1; i<=static_cast<int>(ARMY); ++i)
        os << "  " << setw(3) << i << ": " << army[i] << "\n";
    return os;
}

void ArmyZconst::check(char const* file, int line) const {
    Coord const* army = &base_;
    for (uint i=0; i<ARMY; ++i) army[i].check(file, line);
    for (uint i=0; i<ARMY-1; ++i)
        if (army[i] >= army[i+1]) {
            cerr << static_cast<void const *>(army) << "\n" << *this;
            throw_logic("ArmyZ out of order", file, line);
        }
}

ostream& operator<<(ostream& os, ArmyZconst const& armyZ) {
    Coord const* army = &armyZ.base_;
    for (uint i=0; i<ARMY; ++i)
        os << army[i] << "\n";
    return os;
}

void Statistics::_largest_subset_size(BoardSetBlue const& boards) {
    for (auto const& subset: boards)
        subset_size(subset.size());
}

void StatisticsE::print(ostream& os) const {
    if (statistics || hash_statistics)
        os << "\t" << time_string() << "\n";

    if (statistics) {
        os << "\tLargest subset: " << largest_subset() << "\n";
        os << "\tLargest army resize overflow: " << overflow_max() << "\n";

        if (ARMYSET_CACHE) {
            auto army_tries = armyset_tries() + armyset_cache_hits();
            os << "\tArmy front cache ratio ";
            if (army_tries)
                os << setw(3) << armyset_cache_hits()*100/ army_tries << "%";
            else
                os << "----";
            os << "\t" << armyset_cache_hits() << " / " << army_tries << "\n";
        }
        os << "\tCached army   allocs: ";
        if (armyset_allocs())
            os << setw(3) << armyset_allocs_cached()*100 / armyset_allocs() << "%";
        else
            os << "----";
        os << "\t" << armyset_allocs_cached() << " / " << armyset_allocs() << "\n";

        os << "\tCached army deallocs: ";
        if (armyset_deallocs())
            os << setw(3) << armyset_deallocs_cached()*100 / armyset_deallocs() << "%";
        else
            os << "----";
        os << "\t" << armyset_deallocs_cached() << " / " << armyset_deallocs() << "\n";

        os << "\tArmy inserts:  ";
        if (armyset_tries())
            os << setw(3) << armyset_size()*100 / armyset_tries() << "%";
        else
            os << "----";
        os << "\t" << armyset_size() << " / " << armyset_tries() << "\n";

        if (boardset_uniques())
            os << "\tUnique input boards: " << boardset_uniques() << "\n";
        os << "\tBoard inserts: ";
        if (boardset_tries())
            os << setw(3) << boardset_size()*100 / boardset_tries() << "%";
        else
            os << "----";
        os << "\t" << boardset_size() << " / " << boardset_tries() << "\n";

        os << "\tBlue on red base edge: ";
        if (boardset_size())
            os << setw(3) << edges()*100 / boardset_size() << "%";
        else
            os << "----";
        os << "\t" << edges() << " / " << boardset_size() << "\n";

        os << "\tMemory: " << allocated()/ 1000000  << " plain + " << mmapped()/1000000 << " mmapped (" << mmaps() << " mmaps) = " << (allocated() + mmapped()) / 1000000 << " MB\n";
        os << "\tMlocks: " << mlocked()/ 1000000  << " MB in " << mlocks() << " ranges\n";
    }
    if (hash_statistics) {
        os << "\tArmy immediate:";
        if (armyset_tries())
            os << setw(3) << armyset_immediate() * 100 / armyset_tries() << "%";
        else
            os << "----";
        os << "\t" << armyset_immediate() << " / " << armyset_tries() << "\n";

        os << "\tArmy probes:  ";
        auto probes = armyset_tries();
        // probes -= armyset_immediate();
        if (probes)
            os << setw(4) << 1 + armyset_probes() * 100 / probes / 100.;
        else
            os << "----";
        os << "\t" << armyset_probes() << " / " << probes << " +1" << "\n";

        os << "\tBoard immediate:";
        if (boardset_tries())
            os << setw(3) << boardset_immediate() * 100 / boardset_tries() << "%";
        else
            os << "----";
        os << "\t" << boardset_immediate() << " / " << boardset_tries() << "\n";

        os << "\tBoard probes:  ";
        probes = boardset_tries();
        // probes -= boardset_immediate();
        if (probes)
            os << setw(4) << 1 + boardset_probes() * 100 / probes / 100.;
        else
            os << "----";
        os << "\t" << boardset_probes() << " / " << probes << " +1" << "\n";
    }

    os << setw(6) << duration() << " s, set " << setw(2) << available_moves()-1 << "," << setw(10) << boardset_size() << " boards/" << setw(9) << armyset_size() << " armies " << setw(7);
    os << boardset_size()/(blue_armies_size() ? blue_armies_size() : 1);
    os << " (" << setw(6) << (allocated()+mmapped()) / 1000000 << "/" << setw(6) << memory() / 1000000 << " MB)\n";
}

Move::Move(Army const& army_from, Army const& army_to): from{-1,-1}, to{-1, -1} {
    ArmyPos const fromE{army_from};
    ArmyPos const toE  {army_to};

    uint i = 0;
    uint j = 0;
    int diffs = 0;
    while (i < ARMY && j < ARMY) {
        if (fromE[i] == toE[j]) {
            ++i;
            ++j;
        } else if (fromE[i] < toE[j]) {
            from = fromE[i];
            ++i;
            ++diffs;
        } else {
            to = toE[j];
            ++j;
            ++diffs;
        }
    }
    if (i < ARMY) {
        from = fromE[i];
        diffs += ARMY-i;
    }
    if (j < ARMY) {
        to = toE[j];
        diffs += ARMY-j;
    }
    if (diffs > 2)
        throw_logic("Multimove");
    if (diffs == 1)
        throw_logic("Move going nowhere");
}

Move::Move(Army const& army_from, Army const& army_to, int& diffs): from{-1,-1}, to{-1, -1} {
    ArmyPos const fromE{army_from};
    ArmyPos const toE  {army_to};

    uint i = 0;
    uint j = 0;
    diffs = 0;
    while (i < ARMY && j < ARMY) {
        if (fromE[i] == toE[j]) {
            ++i;
            ++j;
        } else if (fromE[i] < toE[j]) {
            from = fromE[i];
            ++i;
            ++diffs;
        } else {
            to = toE[j];
            ++j;
            ++diffs;
        }
    }
    diffs += ARMY-i;
    diffs += ARMY-j;
}

void Board::svg(ostream& os, uint scale, uint margin) const {
    os << "      <path d='";
    for (uint x=0; x<=X; ++x) {
        os << "M " << margin + x * scale << " " << margin << " ";
        os << "L " << margin + x * scale << " " << margin + Y * scale << " ";
    }
    for (uint y=0; y<=X; ++y) {
        os << "M " << margin             << " " << margin + y * scale << " ";
        os << "L " << margin + X * scale << " " << margin + y * scale << " ";
    }
    os << "'\n            stroke='black' />\n";
    for (auto const pos: blue())
        pos.svg(os, BLUE, scale);
    for (auto const pos: red())
        pos.svg(os, RED,  scale);
}

bool BoardSubsetBlue::find(ArmyId red_id) const {
    return std::binary_search(begin(), end(), red_id);
}

void BoardSubsetBlue::resize() {
    auto old_size = allocated();
    ArmyId new_size = old_size*2;
    remallocate(armies_, old_size, new_size);
    allocated_ = new_size;
}

void BoardSubsetBlue::sort() {
    lib_sort(begin(), end());
}

void BoardSubsetBlue::sort_compress() {
    lib_sort(begin(), end());
    ArmyId red_value_previous = 0;
    ArmyId* to = begin();
    for (ArmyId red_value: *this) {
        if (red_value == red_value_previous) continue;
        *to++ = red_value_previous = red_value;
    }
    left_ = to - begin();
    munneeded(to, allocated() - size());
}

ArmyId BoardSubsetBlue::example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubset");

    ArmyId red_id;
    symmetry = split(armies_[0], red_id);
    return red_id;
}

ArmyId BoardSubsetBlue::random_example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubset");

    ArmyId red_id;
    symmetry = split(armies_[random(size())], red_id);
    return red_id;
}

void BoardSubsetBlue::print(ostream& os) const {
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

ArmyId BoardSubsetRed::example(BoardSubsetRedBuilder const& builder, ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubsetRed");

    ArmyId red_value = builder.read1(reinterpret_cast<size_t>(armies_));

    ArmyId red_id;
    symmetry = split(red_value, red_id);
    return red_id;
}

ArmyId BoardSubsetRed::example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubsetRed");

    ArmyId red_id;
    symmetry = split(armies_[0], red_id);
    return red_id;
}

ArmyId BoardSubsetRed::random_example(BoardSubsetRedBuilder const& builder, ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubsetRed");

    ArmyId red_value = builder.read1(reinterpret_cast<size_t>(armies_) + random(size()));

    ArmyId red_id;
    symmetry = split(red_value, red_id);
    return red_id;
}

ArmyId BoardSubsetRed::random_example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubsetRed");

    ArmyId red_id;
    symmetry = split(armies_[random(size())], red_id);
    return red_id;
}

bool BoardSubsetRed::_find(ArmyId red_value) const {
    if (!armies_) return false;
    return any_of(armies_, armies_+size(),
               [red_value](ArmyId value) { return value == red_value; });
}

BoardSubsetRedBuilder::BoardSubsetRedBuilder(uint t, ArmyId size) :
    filename_{red_file ? red_file + to_string(t) : string{}},
    army_list_{nullptr},
    army_mmap_{nullptr},
    army_list_size_{size},
    free_{0},
    from_{0},
    write_end_{BLOCK},
    real_allocated_{size},
    fd_{-1} {
    if (red_file) {
        offset_ = 0;
        fd_ = OpenReadWrite(filename_);
        if (write_end_ < left_) left_ = write_end_;
    }

    mask_ = size-1;
    cmallocate(armies_, size);
    // This allocates and locks more than needed for !red_file
    // For now keep the logic consistent
    // (we will probably always do red_file for large boardsets anyways)
    mallocate(army_list_, army_list_size_);
    left_ = FACTOR(size);
    // logger << "Create BoardSubsetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << size << "), list " << static_cast<void const *>(army_list_) << " (size " << left_ << ") filename '" << filename_ << "'\n" << flush;
}

BoardSubsetRedBuilder::~BoardSubsetRedBuilder() {
    if (armies_)
        demallocate(armies_, real_allocated_);
    if (fd_ >= 0) {
        if (army_mmap_) FdUnmap(army_mmap_, offset_ / sizeof(army_mmap_[0]));
        Close(fd_, filename_);
    }
    if (army_list_)
        demallocate(army_list_, army_list_size_);
    // logger << "Destroy BoardSubsetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << real_allocated_ << "), list " << static_cast<void const *>(army_list) << " (size " << FACTOR(real_allocated_) << ")\n" << flush;
}

void BoardSubsetRedBuilder::flush() {
    demallocate(armies_, real_allocated_);
    armies_ = nullptr;

    auto write_base = write_end_ - BLOCK;
    if (free_ > write_base) {
        size_t size = free_ - write_base;
        // logger << "list_size=" << army_list_size_ << ", free=" << free_ << ", write_end=" << write_end_ << ", write_base=" << write_base << ", size=" << size << ", PAGE_ROUND(size)=" << PAGE_ROUND(size * sizeof(army_list_[0])) << endl;
        size *= sizeof(army_list_[0]);
        Write(fd_, &army_list_[write_base], size, filename_);
        size_t extra = PAGE_ROUND(size) - size;
        if (extra > 0)
            Extend(fd_, offset_ * sizeof(army_list_[0]), extra, filename_);
    }
    demallocate(army_list_, army_list_size_);
    army_list_ = nullptr;
}

ArmyId BoardSubsetRedBuilder::read1(ArmyId pos) const {
    if (!offset_) throw_logic("Lookup in empty mmap");
    size_t offset = pos * sizeof(ArmyId);
    ArmyId result;
    Read(fd_, &result, offset, sizeof(result), filename_);
    return result;
}

void BoardSubsetRedBuilder::mmap() {
    if (offset_ && !army_mmap_) FdMap(army_mmap_, fd_, offset_);
}

void BoardSubsetRedBuilder::munmap() {
    if (offset_) {
        FdUnmap(army_mmap_, offset_);
        army_mmap_ = nullptr;
    }
}

void BoardSubsetRedBuilder::resize() {
    ArmyId free = free_;

    if (red_file && free == write_end_ ) {
        if (false) {
            logger << "File write\n";
            for (ArmyId i=write_end_ - BLOCK; i < write_end_; ++i)
                logger << "File[" << i << "]=" << army_list_[i] << "\n";
            logger << "--- wrote " << BLOCK << endl;
        }
        Write(fd_, &army_list_[write_end_ - BLOCK], BLOCK_BYTES, filename_);
        write_end_ += BLOCK;
    }

    if (free == army_list_size_) {
        auto old_size = army_list_size_;
        auto new_size = old_size * 2;
        auto list = army_list_;
        remallocate(army_list_, old_size, new_size);
        if (false)
            logger << "Resize BoardSubsetRedBuilder list " << static_cast<void const *>(list) << " (size " << old_size << ") -> " << static_cast<void const *>(army_list_) << " (size " << new_size << ")\n" << std::flush;
        army_list_size_ = new_size;
    }

    if (size() >= capacity()) {
        auto old_allocated = allocated();
        auto new_allocated = old_allocated*2;
        if (new_allocated > real_allocated_) {
            auto armies = armies_;
            cremallocate(armies_, real_allocated_, new_allocated);
            if (false)
                logger << "Resize BoardSubsetRedBuilder hash " << static_cast<void const *>(armies) << " (size " << real_allocated_ << ") -> " << static_cast<void const *>(armies_) << " (size " << new_allocated << ")\n" << std::flush;
            real_allocated_ = new_allocated;
        } else
            std::memset(begin(), 0, new_allocated * sizeof(armies_[0]));

        ArmyId mask = new_allocated-1;
        mask_ = mask;
        auto armies = armies_;
        auto army_list = army_list_;
        for (ArmyId i = from_; i < free; ++i) {
            auto value = army_list[i];
            // cout << "Insert " << value << "\n";
            ArmyId pos = hash64(value) & mask;
            ArmyId offset = 0;
            while (armies[pos]) {
                // cout << "Try " << pos << " of " << mask+1 << "\n";
                ++offset;
                pos = (pos + offset) & mask;
            }
            armies[pos] = value;
            // cout << "Found empty\n";
        }
    }
    left_ = min(army_list_size_, from_ + capacity());
    if (red_file && write_end_ < left_) left_ = write_end_;
}

size_t BoardSubsetRedBuilder::memory_report(ostream& os, string const& prefix) const {
    size_t sz = 0;
    os << prefix << "armies: ";
    if (armies_) {
        os << "ArmyId[" << real_allocated_ << "] (" << real_allocated_ * sizeof(armies_[0]) << " bytes)\n";
        sz += real_allocated_ * sizeof(armies_[0]);
    } else  os << "nullptr\n";

    os << prefix << "army list: ";
    if (army_list_) {
        os << "ArmyId[" << army_list_size_ << "] (" << army_list_size_ * sizeof(army_list_[0]) << " bytes)\n";
        sz += army_list_size_ * sizeof(army_list_[0]);
    } else os << "nullptr\n";
    return sz;
}

BoardSetBase::BoardSetBase(bool keep, ArmyId size): size_{0}, solution_id_{0}, capacity_{size}, from_{1}, top_{1}, keep_{keep} {
}

void BoardSetBase::clear() {
    from_ = top_ = 1;
    size_ = 0;
    solution_id_ = 0;
}

BoardSetBlue::BoardSetBlue(bool keep, ArmyId size) :
    BoardSetBase{keep, size} {
    cmallocate(subsets_, capacity());
    --subsets_;
    // cout << "Create BoardSetBlue " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
}

BoardSetBlue::~BoardSetBlue() {
    for (auto& subset: *this)
        subset.destroy();
    // cout << "Destroy BoardSetBlue " << static_cast<void const*>(subsets_) << "\n";
    ++subsets_;
    demallocate(subsets_, capacity());
}

void BoardSetBlue::resize() {
    auto subsets = subsets_ + 1;
    recmallocate(subsets, capacity(), capacity()*2);
    capacity_ *= 2;
    subsets_ = subsets - 1;
    // logger << "Resize BoardSetBlue " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity() << "\n" << flush;
    if (!keep_ && from_ != 1) throw_logic("Resize of partial BoardSetBase");
}

void BoardSetBlue::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    BoardSetBase::clear();
    ++subsets_;
    cremallocate(subsets_, capacity(), size);
    --subsets_;
    capacity_ = size;
}

void BoardSetBlue::print(ostream& os) const {
    os << "-----\n";
    for (ArmyId i = from(); i < top_; ++i) {
        os << " Blue id " << i << ":";
        cat(i).print(os);
        os << "\n";
    }
    os << "-----\n";
}

void BoardSetRed::grow(ArmyId size) {
    if (size < top_) return;
    // No exponential resize since grow() is assumed to be called only once
    // and immediately sets the correct size
    ++subsets_;
    recmallocate(subsets_, capacity(), size);
    capacity_ = size;
    --subsets_;
    top_ = size + 1;
    // No need to zero the skipped sets because subsets_ is
    // created with zeros
}

BoardSetRed::BoardSetRed(bool keep, ArmyId size) :
    BoardSetBase{keep, size} {
    cmallocate(subsets_, capacity());
    --subsets_;
    // cout << "Create BoardSetRed " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
    top_ = 1;
}

BoardSetRed::~BoardSetRed() {
    if (!red_file)
        for (auto& subset: *this)
            subset.destroy();
    // cout << "Destroy BoardSetRed " << static_cast<void const*>(subsets_) << "\n";
    ++subsets_;
    demallocate(subsets_, capacity());
}

uint BoardSetRed::pre_write(uint n) {
    builders_.reserve(n);
    for (uint t=0; t<n; ++t)
        builders_.emplace_back(make_unique<BoardSubsetRedBuilder>(t));
    return n;
}

void BoardSetRed::post_write() {
    if (red_file) {
        for (auto& builder: builders_)
            builder->flush();
    } else
        builders_.clear();
}

void BoardSetRed::pre_read() {
    if (red_file) {
        for (auto& builder: builders_)
            builder->mmap();
    }
}

void BoardSetRed::post_read() {
    if (red_file)
        for (auto& builder: builders_)
            builder->munmap();
}

void BoardSetRed::resize() {
    auto subsets = subsets_ + 1;
    recmallocate(subsets, capacity(), capacity()*2);
    capacity_ *= 2;
    subsets_ = subsets - 1;
    // logger << "Resize BoardSetRed " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity() << "\n" << flush;
    if (!keep_ && from_ != 1) throw_logic("Resize of partial BoardSetBase");
}

void BoardSetRed::clear(ArmyId size) {
    builders_.clear();
    if (!red_file)
        for (auto& subset: *this)
            subset.destroy();
    BoardSetBase::clear();
    ++subsets_;
    cremallocate(subsets_, capacity(), size);
    --subsets_;
    capacity_ = size;
    top_ = size+1;
}

void BoardSetRed::print(ostream& os) const {
    os << "-----\n";
    for (ArmyId i = from(); i < top_; ++i) {
        os << " Blue id " << i << ":";
        cat(i).print(os);
        os << "\n";
    }
    os << "-----\n";
}

string Image::_str() const {
    string result;
    result.reserve((2*X+3)*(Y+2));
    result += "+";
    for (uint x=0; x < X; ++x) result += "--";
    result += "+\n";

    for (uint y=0; y < Y; ++y) {
        result += "|";
        for (uint x=0; x < X; ++x) {
            auto c = get(x, y);
            result += (c == EMPTY ? ". " :
                       c == RED   ? "X " :
                       c == BLUE  ? "O " :
                       "? ");
        }
        result += "|\n";
    }

    result += "+";
    for (uint x=0; x < X; ++x) result += "--";
    result += "+\n";

    return result;
}

string Image::str() const {
    return _str();
}

string Image::str(Coord from, Coord to, Color color) {
    if (get(from) != color) abort();
    set(from, EMPTY);
    if (get(to) != EMPTY) abort();
    set(to, color);
    auto string = _str();
    set(to, EMPTY);
    set(from, color);
    return string;
}

void Image::check(const char* file, int line) const {
    uint blue = 0;
    uint red  = 0;
    for (uint y=0; y<Y; ++y)
        for (uint x=0; x<X; ++x) {
            Coord const pos{y, x};
            switch(get(pos)) {
                case BLUE:
                  ++blue;
                  break;
                case RED:
                  ++red;
                  break;
                case EMPTY:
                  break;
                default:
                  throw_logic("\n" + str() + "Unexpected Image color", file, line);
            }
        }
    if (blue != ARMY)
        throw_logic("\n" + str() + "Unexpected Image BLUE count", file, line);
    if (red  != ARMY)
        throw_logic("\n" + str() + "Unexpected Image RED count", file, line);
}

void ArmySetDense::_init(size_t size) {
    if (size < MIN_SIZE())
        throw_logic("ArmySetDense clear size too small");

    mask_ = size-1;
    used_ = 0;

    armies_size_ = size * ARMY;
    while (armies_size_ < ARMY+ARMY_PADDING) armies_size_ *= 2;
    size_t alimit = (armies_size_-ARMY_PADDING)/ARMY - 1;
    if (alimit >= ARMYID_HIGHBIT)
        throw(overflow_error("ArmySetDense size too large"));
    limit_ = min(FACTOR(size), static_cast<ArmyId>(alimit));

    mallocate(armies_, armies_size_, memory_flags_);
    cmallocate(values_, size, memory_flags_);
    // logger << "New value  " << static_cast<void const *>(values_) << "\n";
    // logger << "New armies " << static_cast<void const *>(armies_) << "\n";
}

ArmySetDense::ArmySetDense(bool lock, size_t size):
    armies_{nullptr},
    values_{nullptr},
    memory_flags_{MLOCK ? lock * ALLOC_LOCK : 0} {
        _init(size);
    }

ArmySetDense::~ArmySetDense() {
    if (armies_) demallocate(armies_, armies_size_, memory_flags_);
    if (values_) demallocate(values_, allocated(), memory_flags_);
}

void ArmySetDense::clear(size_t size) {
    demallocate(armies_, armies_size_, memory_flags_);
    if (values_) demallocate(values_, allocated(), memory_flags_);
    _init(size);
}

ArmyId ArmySetDense::find(Army const& army) const {
    if (true || CHECK) {
        if (UNLIKELY(!values_)) throw_logic("dropped hash ArmySetDense::find");
    }
    ArmyId const mask = mask_;
    ArmyId pos = army.hash() & mask;
    auto values = values_;
    ArmyId offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values[pos];
        if (i == 0) return 0;
        if (army == cat(i)) return i;
        ++offset;
        pos = (pos + offset) & mask;
    }
}

ArmyId ArmySetDense::find(ArmyPos const& army) const {
    if (true || CHECK) {
        if (UNLIKELY(!values_)) throw_logic("dropped hash ArmySetDense::find");
    }
    ArmyId const mask = mask_;
    ArmyId pos = army.hash() & mask;
    auto values = values_;
    ArmyId offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values[pos];
        if (i == 0) return 0;
        if (army == cat(i)) return i;
        ++offset;
        pos = (pos + offset) & mask;
    }
}

void ArmySetDense::resize() {
    ArmyId values_limit = FACTOR(allocated());
    ArmyId armies_limit = (armies_size_-ARMY_PADDING)/ARMY - 1;

    if (used_ >= armies_limit) {
        size_t new_size = armies_size_ * 2;
        // logger << "Resize ArmySetDense armies: new size=" << new_size/ARMY << endl;
        size_t alimit = (new_size-ARMY_PADDING)/ARMY - 1;
        // Only applies to the red armyset really. But the blue armyset is
        // always so much smaller than red that it doesn't matter
        if (alimit >= ARMYID_HIGHBIT)
            throw(overflow_error("ArmyId grew too large"));
        remallocate(armies_, armies_size_, new_size, memory_flags_);
        armies_size_ = new_size;
        armies_limit = alimit;
    }

    if (used_ >= values_limit) {
        size_t old_size = allocated();
        size_t new_size = old_size * 2;
        // logger << "Resize ArmySetDense values: new size=" << new_size << "\n" << flush;
        if (new_size-1 > ARMYID_MAX)
            throw(overflow_error("Army hash grew too large"));

        cremallocate(values_, old_size, new_size, memory_flags_);
        mask_ = new_size-1;
        auto values = values_;
        auto mask   = mask_;
        auto used   = used_;
        auto armies = armies_;
        for (ArmyId i = 1; i <= used; ++i) {
            armies += ARMY;
            ArmyId hash = army_hash(armies);
            ArmyId pos = hash & mask;
            ArmyId offset = 0;
            while (values[pos]) {
                ++offset;
                pos = (pos + offset) & mask;
            }
            values[pos] = i;
        }
        values_limit = FACTOR(allocated());
    }
    limit_ = min(values_limit, armies_limit);
    // logger << "Resize done\n" << flush;
}

void ArmySetDense::print(ostream& os) const {
    os << "[";
    if (values_) {
        for (size_t i=0; i < allocated(); ++i)
            os << " " << values_[i];
        } else os << " D";
    os << " ] (" << static_cast<void const *>(this) << ")\n";
    for (size_t i=1; i <= used_; ++i) {
        os << "Army " << i << "\n" << Image{cat(i)};
    }
}

void ArmySetSparse::DataArena::SizeArena::init(uint i) {
    block_size_ = (i+1) * Element::SIZE + sizeof(GroupId);
    if (block_size_ >= MMAP_THRESHOLD)
        throw_logic("Block size is way too big");

    size_ = MMAP_THRESHOLD;
    mallocate(data_, size());
    free_ = 0;
    lower_bound_ = 0;
    cached_ = 0;
}

void ArmySetSparse::DataArena::SizeArena::free() {
    if (!data_) return;
    demallocate(data_, size());
    data_   = nullptr;
    cached_ = 0;
}

void ArmySetSparse::DataArena::SizeArena::expand() {
    size_t old_size = size();
    size_t new_size =
        max(MMAP_THRESHOLD_ROUND(old_size + old_size / FRACTION),
            old_size + MMAP_THRESHOLD);
    remallocate(data_, old_size, new_size);
    lower_bound_ = min(MMAP_THRESHOLD_ROUND(old_size - old_size / FRACTION),
                                            old_size - MMAP_THRESHOLD);
    // logger << "Expand " << index() << ": " << old_size << " -> " << new_size << "[" << lower_bound_ << ", " << free_ << ", " << new_size << "]" << endl;
    size_ = new_size;
}

void ArmySetSparse::DataArena::SizeArena::shrink() {
    size_t old_size = size();
    size_t new_size =
        max(MMAP_THRESHOLD_ROUND(lower_bound_ + lower_bound_ / FRACTION),
            lower_bound_ + MMAP_THRESHOLD);
    remallocate(data_, old_size, new_size);
    lower_bound_ =
        min(MMAP_THRESHOLD_ROUND(lower_bound_ - lower_bound_ / FRACTION),
            lower_bound_ - MMAP_THRESHOLD);
    // logger << "Shrink " << index() << ": " << old_size << " -> " << new_size << "[" << lower_bound_ << ", " << free_ << ", " << new_size << "]" << endl;
    size_ = new_size;
    if (old_size <= new_size) throw_logic("This is not a shrink");
}

void ArmySetSparse::DataArena::SizeArena::post_convert() {
    uint n = index() + 1;
    char const* RESTRICT from = data_;
    char const* RESTRICT end = _data(free_);
    ArmyId *to = reinterpret_cast<ArmyId *>(data_);
    while (from < end) {
        from += sizeof(GroupId);
        for (uint i=0; i<n; ++i, from+=Element::SIZE, ++to) {
            Element const& element = Element::element(from);
            *to = element.id();
        }
    }
    end = reinterpret_cast<char const*>(to);
    free_ = end - data_;
    remallocate(data_, size(), free_);
    size_ = free_;
}

auto ArmySetSparse::DataArena::SizeArena::allocate() -> DataId {
    // Directly use free_ without test if DATA_ARENA_SIZE is made 0
    if (cached()) return cache_[--cached_];
    auto old_free = free_;
    free_ += block_size_;
    if (free_+ARMY_PADDING > size()) expand();
    return old_free;
}

void ArmySetSparse::DataArena::SizeArena::deallocate(Group* groups, DataId data_id) {
    if (cached() < DATA_ARENA_SIZE) {
        cache_[cached_++] = data_id;
        group_id_at(data_id) = GROUP_ID_MAX;
    } else {
        free_ -= block_size_;
        if (data_id != free_) {
            GroupId top_group_id = group_id_at(free_);
            if (is_cached(top_group_id)) {
                for (uint i=0; i<cached(); ++i)
                    if (cache_[i] == free_) {
                        cache_[i] = data_id;
                        goto FOUND;
                    }
                throw_logic("Top is free but I don't know why");
              FOUND:
                group_id_at(data_id) = GROUP_ID_MAX;
            } else {
                groups[top_group_id].data_id() = data_id;
                char const* RESTRICT from = _data(free_);
                std::copy(from, from + block_size_, _data(data_id));
            }
        }
        if (free_ < lower_bound_) shrink();
    }
}

size_t ArmySetSparse::DataArena::SizeArena::check(char const* file, int line) const {
    if (UNLIKELY(!data_)) throw_logic("No data", file, line);
    if (UNLIKELY(!size_)) throw_logic("Data without size", file, line);
    if (UNLIKELY(size_ % MMAP_THRESHOLD))
        throw_logic("size_ is not a multipe of MMAP_THRESHOLD", file, line);
    if (UNLIKELY(free_ > size_)) throw_logic("Free beyond size", file, line);
    if (UNLIKELY(!block_size_)) throw_logic("Zero block_size_", file, line);
    size_t nr_groups = free_ / block_size_;
    if (UNLIKELY(free_ != nr_groups * block_size_))
        throw_logic("free_ is not a multiple of block_size_", file, line);
    if (UNLIKELY(free_ < lower_bound_)) throw_logic("Free below lower bound", file, line);
    if (UNLIKELY(lower_bound_ % MMAP_THRESHOLD))
        throw_logic("lower_bound_ is not a multipe of MMAP_THRESHOLD", file, line);
    if (UNLIKELY(cached() > DATA_ARENA_SIZE))
        throw_logic("Too many cached", file, line);

    for (uint i=0; i<cached(); ++i) {
        if (UNLIKELY(cache_[i] % block_size_))
            throw_logic("Cache is not a multiple of block_size_", file, line);
        if (UNLIKELY(cache_[i] >= free_))
            throw_logic("Cache[" + to_string(i) + "] = " + to_string(cache_[i]) + " above free " + to_string(free_), file, line);
        if (UNLIKELY(group_id_at(cache_[i]) != GROUP_ID_MAX))
            throw_logic(to_string(index()) + "+1 bits: cache[" + to_string(i) + "] = " + to_string(cache_[i]) + " has valid group id " + to_string(group_id_at(cache_[i])), file, line);
    }
    if (UNLIKELY(cached() > nr_groups))
        throw_logic("Too many deallocated", file, line);
    return nr_groups - cached();
}

void ArmySetSparse::DataArena::SizeArena::check_data(DataId data_id, GroupId group_id, ArmyId nr_elements, char const* file, int line) const {
    if (UNLIKELY(data_id >= free_))
        throw_logic("DataId " + to_string(data_id) + " beyond free " + to_string(free_), file, line);
    if (UNLIKELY(group_id_at(data_id) != group_id))
        throw_logic("Inconsistent Group Id backpointer", file, line);

    char const* ptr = data(data_id);
    ptr -= sizeof(GroupId);
    for (uint i=sizeof(GroupId); i < block_size_; i += Element::SIZE) {
        Element const& element = Element::element(&ptr[i]);
        if ((UNLIKELY(element.id() > nr_elements && nr_elements) ||
             UNLIKELY(element.id() == 0)))
            throw_logic("Element " + to_string(element.id()) + " is out of range [1.." + to_string(nr_elements) + "]", file, line);
        element.armyZ().check(file, line);
    }
}

size_t ArmySetSparse::DataArena::SizeArena::memory_report(ostream& os) const {
    if (data_) {
        os << size();
        return size();
    }
    os << "X";
    return 0;
}

void ArmySetSparse::DataArena::init() {
    for (uint i=0; i<GROUP_SIZE; ++i)
        cache_[i].init(i);
}

void ArmySetSparse::DataArena::free() {
    for (auto& cache: cache_)
        cache.free();
}

void ArmySetSparse::DataArena::post_convert() {
    for (auto& cache: cache_)
        cache.post_convert();
}

void ArmySetSparse::DataArena::append(Group* groups, GroupId group_id, uint pos, Element const& old_element) {
    // if (CHECK) old_element.armyZ().check(__FILE__, __LINE__);
    char const* RESTRICT old_e = reinterpret_cast<char const*>(&old_element);
    Group& group = groups[group_id];
    DataId new_data_id;
    if (group.bitmap()) {
        uint n = group.bits();
        new_data_id = cache_[n].allocate();
        cache_[n].group_id_at(new_data_id) = group_id;
        char* RESTRICT new_data = cache_[n].data(new_data_id);
        DataId old_data_id = group.data_id();
        char const* RESTRICT old_data = cache_[n-1].data(old_data_id);
        uint i = group.index(pos);
        std::copy(&old_data[0], &old_data[i * Element::SIZE], &new_data[0]);
        auto new_element = &new_data[i * Element::SIZE];
        std::copy(old_e, old_e + Element::SIZE, new_element);
        std::copy(&old_data[i * Element::SIZE], &old_data[n * Element::SIZE],
                  new_element + Element::SIZE);
        cache_[n-1].deallocate(groups, old_data_id);
    } else {
        new_data_id = cache_[0].allocate();
        cache_[0].group_id_at(new_data_id) = group_id;
        char* RESTRICT new_data = cache_[0].data(new_data_id);
        std::copy(old_e, old_e + Element::SIZE, new_data);
    }
    group.data_id() = new_data_id;
    group.set(pos);
}

void ArmySetSparse::DataArena::check(Group const* groups, GroupId n, ArmyId size, ArmyId overflowed, ArmyId nr_elements, char const* file, int line) const {
    if (UNLIKELY(!groups)) throw_logic("NULL groups");

    size_t nr_groups = 0;
    for (uint i=0; i<GROUP_SIZE; ++i)
        nr_groups += cache_[i].check(file, line);

    uint n_gr = 0;
    size_t sz = 0;
    for (GroupId g=0; g<n; ++g) {
        auto& group = groups[g];
        if (group.bitmap() == 0) continue;
        uint n = group.bits();
        cache_[n-1].check_data(group.data_id(), g, nr_elements, file, line);
        ++n_gr;
        sz += n;
    }
    if (UNLIKELY(n_gr != nr_groups))
        throw_logic("Unexpected number of groups", file, line);
    if (UNLIKELY(sz + overflowed != size))
        throw_logic("Unexpected number of elements: " + to_string(sz) + " + " + to_string(overflowed) += " != " + to_string(size), file, line);
    if (UNLIKELY(overflowed > size))
        throw_logic("Excessive overflow", file, line);
}

size_t ArmySetSparse::DataArena::memory_report(ostream& os) const {
    size_t sz = 0;
    uint i = 0;
    for (auto& c: cache_) {
        os << setw(3) << i << ": ";
        sz += c.memory_report(os);
        ++i;
    }

    return sz;
}

void ArmySetSparse::_init(size_t size) {
    size_t nr_groups = size / GROUP_SIZE;

    mask_ = nr_groups-1;
    left_ = FACTOR(size);

    cmallocate(groups_, nr_groups, memory_flags_);
    // logger << "New groups  " << static_cast<void const *>(groups_) << "\n";
    data_arena_.init();
    if (ARMYSET_CACHE) {
        cmallocate(cache_, size / CACHE_FRACTION * Element::SIZE+ARMY_PADDING);
        mask_cache_ = size / CACHE_FRACTION - 1;
    }
    mallocate(overflow_, GROUP_SIZE * Element::SIZE);
    overflow_size_ = GROUP_SIZE * Element::SIZE;
}

ArmySetSparse::ArmySetSparse():
    groups_{nullptr},
    overflow_{nullptr},
    cache_{nullptr},
    overflow_used_{0},
    overflow_size_{0},
    overflow_max_{0},
    memory_flags_{0} {}

ArmySetSparse::~ArmySetSparse() {
    if (groups_) {
        data_arena_.free();
        demallocate(groups_, nr_groups(), memory_flags_);
    }
    if (ARMYSET_CACHE && cache_)
        demallocate(cache_, allocated_cache() * Element::SIZE+ARMY_PADDING);
    if (overflow_)
        demallocate(overflow_, overflow_size_);
}

void ArmySetSparse::clear() {
    if (groups_) {
        data_arena_.free();
        demallocate(groups_, nr_groups(), memory_flags_);
        groups_ = nullptr;
    }
    if (ARMYSET_CACHE && cache_) {
        demallocate(cache_, allocated_cache() * Element::SIZE+ARMY_PADDING);
        cache_ = nullptr;
    }
    if (overflow_) {
        if (overflow_used_) throw_logic("overflow is not empty");
        demallocate(overflow_, overflow_size_);
        overflow_ = nullptr;
        overflow_size_ = 0;
        overflow_max_  = 0;
    }
}

ArmyId ArmySetSparse::find(ArmySet const& army_set, Army const& army, uint64_t hash) const {
    if (UNLIKELY(!groups_))
        throw_logic("ArmySetSparse::find without groups");
    GroupId const mask = mask_;
    Group const* groups = groups_;
    uint64_t offset = 0;
    while (true) {
        GroupId group_id = (hash >> GROUP_BITS) & mask;
        auto& group = groups[group_id];
        uint pos = hash & GROUP_MASK;
        // cout << "Try [" << group_id << "," << pos<< "] of " << allocated() << "\n";
        if (group.bit(pos) == 0) return 0;
        ArmyId i = data_arena_.converted_id(group, pos);
        if (army == army_set.cat(i)) return i;
        hash += ++offset;
    }
}

ArmyId ArmySetSparse::find(ArmySet const& army_set, ArmyPos const& army, uint64_t hash) const {
    if (UNLIKELY(!groups_))
        throw_logic("ArmySetSparse::find without groups");
    GroupId const mask = mask_;
    Group const* groups = groups_;
    uint64_t offset = 0;
    while (true) {
        GroupId group_id = (hash >> GROUP_BITS) & mask;
        auto& group = groups[group_id];
        uint pos = hash & GROUP_MASK;
        // cout << "Try [" << group_id << "," << pos<< "] of " << allocated() << "\n";
        if (group.bit(pos) == 0) return 0;
        ArmyId i = data_arena_.converted_id(group, pos);
        if (army == army_set.cat(i)) return i;
        hash += ++offset;
    }
}

void ArmySetSparse::resize() {
    // if (CHECK) check(__FILE__, __LINE__);

    DataArena new_arena;
    new_arena.init();
    array<GroupBuilder, GROUP_BUILDERS> groups_low;
    array<GroupBuilder, GROUP_BUILDERS> groups_high;

    GroupId old_nr_groups = nr_groups();
    GroupId new_nr_groups = old_nr_groups * 2;
    remallocate(groups_, old_nr_groups, new_nr_groups, memory_flags_);
    Group* groups = groups_;
    GroupId mask = new_nr_groups-1;
    mask_ = mask;
    left_ +=
        + FACTOR(new_nr_groups * GROUP_SIZE)
        - FACTOR(old_nr_groups * GROUP_SIZE);
    //logger << "Resize ArmySetSparse: " << old_groups << " -> " << new_groups << ", new size=" << new_nr_groups * GROUP_SIZE << " (" << size() << " armies)" << endl;

    if (ARMYSET_CACHE) {
        lock_guard<mutex> lock{exclude_cache_};

        size_t old_size_cache = allocated_cache() * Element::SIZE;
        size_t new_size_cache = 2 * old_size_cache;
        remallocate(cache_, old_size_cache+ARMY_PADDING, new_size_cache+ARMY_PADDING);
        std::memcpy(cache_ + old_size_cache, cache_, old_size_cache);
        mask_cache_ = 2 * mask_cache_ + 1;
    }

    //print(logger, false);
    //logger << "Start\n" << flush;

    for (GroupId g_low = 0, g_high = old_nr_groups;
         g_low < old_nr_groups;
         ++g_low, ++g_high) {
        uint g = g_low % GROUP_BUILDERS;
        if (g_low >= GROUP_BUILDERS) {
            new_arena.copy(groups, g_low  - GROUP_BUILDERS, groups_low [g]);
            new_arena.copy(groups, g_high - GROUP_BUILDERS, groups_high[g]);
        }
        groups_low [g].clear();
        groups_high[g].clear();
        auto const& old_group = groups[g_low];
        uint n = old_group.bits();
        if (n == 0) continue;
        DataId old_data_id = old_group.data_id();
        char const* RESTRICT ptr = data_arena_.data(n-1, old_data_id);
        for (uint i=0; i<n; ++i, ptr += Element::SIZE) {
            auto& old_element = Element::element(ptr);
            uint64_t hash = old_element.hash() >> ARMY_SUBSET_BITS;
            // logger << "Rehashing " << old_element.id() << ", hash " << hex << hash << dec << "\n" << Image{old_element.armyZ()};
            ArmyId offset = 0;
            while (true) {
                uint pos = hash & GROUP_MASK;
                ArmyId group_id = (hash >> GROUP_BITS) & mask;
                // logger << "Try [" << group_id << "," << pos << "]\n";
                auto& group_builders = group_id < old_nr_groups ?
                                                  groups_low : groups_high;
                GroupId g = group_id < old_nr_groups ? g_low : g_high;
                g -= group_id;
                // g must be an unsigned type!
                if (g >= GROUP_BUILDERS) {
                    // logger << "Far move g=" << g << ", g_low=" << g_low << ", g_high=" << g_high << endl;
                    overflow(old_element);
                    break;
                } else {
                    uint gb = (g_low - g) % GROUP_BUILDERS;
                    GroupBuilder& group_builder = group_builders[gb];
                    if (!group_builder.bit(pos)) {
                        // logger << "Hit\n";
                        group_builder.copy(pos, old_element);
                        break;
                    }
                }
                hash += ++offset;
                // logger << "Miss\n";
            }
            // logger.flush();
        }
        // old_group._free_data();
    }
    overflow_mark();
    for (GroupId g_low = old_nr_groups < GROUP_BUILDERS ? 0 : old_nr_groups-GROUP_BUILDERS; g_low < old_nr_groups; ++g_low) {
        uint g = g_low % GROUP_BUILDERS;
        GroupId g_high = g_low + old_nr_groups;

        new_arena.copy(groups, g_low , groups_low [g]);
        new_arena.copy(groups, g_high, groups_high[g]);
    }

    // if (CHECK) new_arena.check(groups, new_nr_groups, size(), overflowed(), __FILE__, __LINE__);
    data_arena_.copy(new_arena);

    //print(logger, false);
    //logger << "Resize half done\n" << flush;
    // if (CHECK) data_arena_.check(groups_, new_nr_groups, size(), overflowed(), __FILE__, __LINE__);

    // logger << "overflow " << overflowed() << " / " << old_nr_groups << endl;
    while (_overflowed()) {
        Element const& old_element = overflow_pop();
        ArmyId hash = old_element.hash() >> ARMY_SUBSET_BITS;
        // logger << "Rehashing overflow " << old_element.id() << ", hash " << hex << hash << dec << "\n" << Image{old_element.armyZ()};
        ArmyId offset = 0;
        while (true) {
            ArmyId group_id = (hash >> GROUP_BITS) & mask;
            auto& new_group = groups[group_id];
            uint pos = hash & GROUP_MASK;
            // logger << "Try [" << group_id << "," << pos << "]\n";
            if (new_group.bit(pos) == 0) {
                // logger << "Hit\n";
                data_arena_.append(groups, group_id, pos, old_element);
                break;
            }
            hash += ++offset;
            // logger << "Miss\n";
        }
    }


    if (CHECK) check(0, __FILE__, __LINE__);
    //print(logger, false);
    //logger << "Resize done\n" << flush;
}

void ArmySetSparse::_convert_hash(uint unit, Coord* armies, ArmyId nr_elements, bool keep) {
    if (!BUILTIN_CONSTANT(keep)) throw_logic("keep must be a constant");
    // cout << *this;

    if (CHECK) check(nr_elements, __FILE__, __LINE__);

    if (overflow_) {
        demallocate(overflow_, overflow_size_);
        overflow_ = nullptr;
    }

    if (ARMYSET_CACHE && cache_) {
        demallocate(cache_, allocated_cache() * Element::SIZE+ARMY_PADDING);
        cache_ = nullptr;
    }

    GroupId n_groups = nr_groups();
    Group* groups = groups_;

    for (GroupId g=unit; g<n_groups; g += work_units()) {
        auto& group = groups[g];
        uint n = group.bits();
        // cout << "Converting group " << g << " with " << n << " elements\n";
        if (n == 0) continue;
        DataId old_data_id = group.data_id();
        char const* RESTRICT old_ptr = data_arena_.data(n-1, old_data_id);
        for (uint i=0; i<n; ++i, old_ptr += Element::SIZE) {
            auto& old_element = Element::element(old_ptr);
            auto armyZ = old_element.armyZ();
            if (CHECK && (UNLIKELY(old_element.id() > nr_elements) ||
                          UNLIKELY(old_element.id() == 0)))
                throw_logic("Element " + to_string(old_element.id()) + " is out of range [1.." + to_string(nr_elements) + "]");
            std::copy(armyZ.begin(), armyZ.end(), &armies[old_element.id() * static_cast<size_t>(ARMY)]);
        }
        if (keep) group.data_id() = data_arena_.converted_data_id(n, old_data_id);
    }
    if (keep) {
        unlock();
        data_arena_.post_convert();
    } else {
        demallocate(groups, n_groups, memory_flags_);
        groups_ = nullptr;
        unlock();
        data_arena_.free();
    }
}

size_t ArmySetSparse::overflow_max() const {
    return overflow_max_ / Element::SIZE;
}

void ArmySetSparse::print(ostream& os, bool show_boards, Coord const* armies) const {
    GroupId n_groups = nr_groups();
    Group* groups = groups_;
    for (GroupId g=0; g<n_groups; ++g) {
        os << "\n{";
        auto& group = groups[g];
        if (group.bitmap()) {
            uint n = group.bits();
            DataId data_id = group.data_id();
            if (armies) {
                ArmyId const* RESTRICT ptr = data_arena_.converted_data(n-1, data_id);
                for (uint i=0; i<GROUP_SIZE; ++i)
                    if (group.bit(i)) {
                        os << " " << *ptr++;
                    } else
                        os << " D";
            } else {
                char const* RESTRICT ptr = data_arena_.data(n-1, data_id);
                for (uint i=0; i<GROUP_SIZE; ++i)
                    if (group.bit(i)) {
                        Element const& element = Element::element(ptr);
                        os << " " << element.id();
                        ptr += Element::SIZE;
                    } else
                        os << " D";
            }
        }
        os << " }";
    }
    os << " (" << static_cast<void const *>(this) << ")\n";

    if (!show_boards) return;

    auto mask = mask_;
    for (GroupId g=0; g<n_groups; ++g) {
        auto& group = groups[g];
        if (group.bitmap()) {
            uint n = group.bits();
            DataId data_id = group.data_id();
            if (armies) {
                ArmyId const* RESTRICT ptr = data_arena_.converted_data(n-1, data_id);
                for (uint i=0; i<GROUP_SIZE; ++i)
                    if (group.bit(i)) {
                        ArmyId id = *ptr++;
                        ArmyZconst const army{armies[id * static_cast<size_t>(ARMY)]};
                        auto h = army.hash() >> ARMY_SUBSET_BITS;
                        os << "Army " << id << ", hash " << hex << h << dec << " [" << ((h >> GROUP_BITS) & mask) << "," << (h & GROUP_MASK) << "]\n" << Image{army};
                    }
            } else {
                char const* RESTRICT ptr = data_arena_.data(n-1, data_id);
                for (uint i=0; i<GROUP_SIZE; ++i)
                    if (group.bit(i)) {
                        Element const& element = Element::element(ptr);
                        auto h = element.hash() >> ARMY_SUBSET_BITS;
                        os << "Army " << element.id() << ", hash " << hex << h << dec << " [" << ((h >> GROUP_BITS) & mask) << "," << (h & GROUP_MASK) << "]\n" << Image{element.armyZ()};
                        ptr += Element::SIZE;
                    }
            }
        }
    }
}

void ArmySetSparse::check(ArmyId nr_elements, char const* file, int line) const {
    if (UNLIKELY(!groups_))
        throw_logic("No groups", file, line);
    if (UNLIKELY(!overflow_))
        throw_logic("No overflow space", file, line);
    if (UNLIKELY(!overflow_size_))
        throw_logic("Empty overflow space", file, line);
    if (UNLIKELY(overflow_size_ % Element::SIZE))
        throw_logic("Overflow space is not a multiple of Element::SIZE", file, line);
    if (UNLIKELY(overflow_used_ % Element::SIZE))
        throw_logic("Overflow space used is not a multiple of Element::SIZE", file, line);
    if (UNLIKELY(overflow_max_ > overflow_size_))
        throw_logic("Excessive max overflow", file, line);
    if (UNLIKELY(overflow_max_ % Element::SIZE))
        throw_logic("Max Overflow is not a multiple of Element::SIZE", file, line);
    data_arena_.check(groups_, nr_groups(), size(), 0, nr_elements, file, line);
}

size_t ArmySetSparse::memory_report(ostream& os, string const& prefix) const {
    size_t sz = 0;

    if (ARMYSET_CACHE) {
        os << prefix << "cache = ";
        if (cache_) {
            os << "Element[" << allocated_cache() << "] (" << allocated_cache() * Element::SIZE+ARMY_PADDING << " bytes)\n";
            sz += allocated_cache() * Element::SIZE+ARMY_PADDING;
        } else os << "nullptr\n";
    }

    os << prefix << "groups = ";
    if (groups_) {
        os << "Group[" << nr_groups() << "] (" << nr_groups() * sizeof(Group) << " bytes, " << (memory_flags_ & ALLOC_LOCK ? "locked" : "unlocked") << ")\n";
        sz += nr_groups() * sizeof(Group);

        size_t arena = 0;
        os << prefix << "data arena: char[]";
        arena += data_arena_.memory_report(os);
        os << " = " << arena << "\n";
        sz += arena;
    } else os << "nullptr\n";

    os << prefix << "overflow = ";
    if (overflow_) {
        os << "char[" << overflow_size_ << "]\n";
        sz += overflow_size_;
    } else os << "nullptr\n";

    return sz;
}

size_t ArmySubsets::memory_report(ostream& os, string const& prefix) const {
    size_t sz = 0;
    uint i=0;
    auto pre = prefix + "  ";
    for (auto& subset: subsets_) {
        os << prefix << " subset[" << setw(2) << i << "]:\n";
        sz += subset.memory_report(os, pre);
        ++i;
    }
    return sz;
}

ArmySet::ArmySet(bool lock):
    armies_{nullptr},
    memory_flags_{MLOCK ? lock * ALLOC_LOCK : 0} {
        init();
    }

ArmySet::~ArmySet() {
    if (armies_)
        demallocate(armies_+ARMY, size() * static_cast<size_t>(ARMY) + ARMY_PADDING);
}

void ArmySet::init() {
    size_ = 0;
    if (memory_flags_ & ALLOC_LOCK)
        for (auto& subset: subsets_) subset.lock();
    for (auto& subset: subsets_) subset._init();
}

void ArmySet::clear() {
    for (auto& subset: subsets_) subset.clear();
    if (armies_) {
        demallocate(armies_+ARMY, size() * static_cast<size_t>(ARMY) + ARMY_PADDING);
        armies_ = nullptr;
    }
    init();
}

void ArmySet::lock() {
    if (!MLOCK) return;
    if (memory_flags_ & ALLOC_LOCK) throw_logic("Already locked");
    memory_flags_ |= ALLOC_LOCK;
    if (!armies_)
        for (auto&subset: subsets_) subset.lock();
}

void ArmySet::unlock() {
    // Only unlock the groups_ spine (if it is actively being built)
    if (!MLOCK) return;
    if (!(memory_flags_ & ALLOC_LOCK)) throw_logic("Already unlocked");
    memory_flags_ &= ~ALLOC_LOCK;
    if (!armies_)
        for (auto& subset: subsets_) subset.unlock();
}

ArmyId ArmySet::find(Army    const& army) const {
    if (UNLIKELY(!armies_))
        throw_logic("ArmySet::find without armies");
    uint64_t hash = army.hash();
    return subsets_[hash & ARMY_SUBSETS_MASK].find(*this, army, hash >> ARMY_SUBSET_BITS);
}

ArmyId ArmySet::find(ArmyPos const& army) const {
    if (UNLIKELY(!armies_))
        throw_logic("ArmySet::find without armies");
    uint64_t hash = army.hash();
    return subsets_[hash & ARMY_SUBSETS_MASK].find(*this, army, hash >> ARMY_SUBSET_BITS);
}

void ArmySet::_convert_hash(atomic<uint>& todo, bool keep) {
    auto armies = armies_;
    auto nr_elements = size();
    while (true) {
        uint work = --todo;
        if (static_cast<int>(work) < 0) break;
        uint subset = work / ArmySubset::work_units();
        uint unit   = work % ArmySubset::work_units();
        subsets_[subset]._convert_hash(unit, armies, nr_elements, keep);
    }
    update_allocated();
}

void ArmySet::__convert_hash(uint thid, atomic<uint>* todo, bool keep) {
    tid = thid;
    if (keep) _convert_hash(*todo, true);
    else      _convert_hash(*todo, false);
}

void ArmySet::_convert_hash(bool keep) {
    if (!main_thread())
        throw_logic("_convert_hash should not be called from a subthread");
    if (armies_) throw_logic("Already converted ArmySetSparse");

    mallocate(armies_, size() * static_cast<size_t>(ARMY) + ARMY_PADDING);
    armies_ -= ARMY;

    uint n = work_units();
    atomic<uint> todo{n};
    vector<future<void>> results;
    if (nr_threads < n) n = nr_threads;
    for (uint t=1; t < n; ++t)
        results.emplace_back(async(launch::async, &ArmySet::__convert_hash, this, t, &todo, keep));
    __convert_hash(0, &todo, keep);

    for (auto& result: results) result.get();
}

void ArmySet::drop_hash() {
    _convert_hash(false);
}

void ArmySet::convert_hash() {
    _convert_hash(true);
}

size_t ArmySet::overflow_max() const  {
    size_t omax = 0;
    for (auto& subset: subsets_)
        if (subset.overflow_max() > omax) omax = subset.overflow_max();
    return omax;
}

void ArmySet::check(char const* file, int line) const {
    if (UNLIKELY(armies_))
        throw_logic("Check after convert not implememted (yet)", file, line);

    ArmyId nr_elements = size();
    for (auto& subset: subsets_) subset.check(nr_elements, file, line);
}

size_t ArmySet::memory_report(ostream& os, string const& prefix) const {
    size_t sz = 0;
    os << prefix << "armies = ";
    if (armies_) {
        os << "Coord[" << size() << " * " << ARMY << " + " << ARMY_PADDING << "] (" << size() * sizeof(Coord) * ARMY << " bytes)\n";
        sz += (size() * ARMY + ARMY_PADDING) * sizeof(Coord);
    } else os << "nullptr\n";

    sz += subsets_.memory_report(os, prefix);
    os << prefix << "ArmySet total memory = " << sz << " bytes\n";
    return sz;
}

ArmySetCache::ArmySetCache(ArmyId size) {
    cmallocate(cache_, size * static_cast<size_t>(Element::SIZE)+ARMY_PADDING, ALLOC_LOCK);
    mask_ = size - 1;
    factor_ = nr_threads * FACTOR;
    limit_ = size * factor_;
}

ArmySetCache::~ArmySetCache() {
    demallocate(cache_, allocated() * static_cast<size_t>(Element::SIZE)+ARMY_PADDING, ALLOC_LOCK);
    //logger << "Hit " << hit << ", miss " << miss;
    //auto sum = hit + miss;
    //if (sum) logger << ", ratio: " << hit * 100 / sum << "%";
    //logger << endl;
}

void ArmySetCache::resize() {
    size_t element_size = Element::SIZE;
    ArmyId size = allocated();
    size_t old_allocated = size * element_size;
    size_t new_allocated = old_allocated * 2;
    remallocate(cache_, old_allocated+ARMY_PADDING, new_allocated+ARMY_PADDING, ALLOC_LOCK);
    std::memcpy(cache_+old_allocated, cache_, old_allocated);
    size *= 2;
    mask_ = size-1;
    limit_ = size * factor_;
    //logger << "Resize to " << allocated() << endl;
}

FullMove::FullMove(char const* str): FullMove{} {
    auto ptr = str;
    if (!ptr[0]) return;
    while (true) {
        char ch = *ptr++;
        if (ch < letters[0] || ch >= letters[X]) break;
        uint x = ch - 'a';
        if (x >= X) break;
        uint y = 0;
        while (true) {
            ch = *ptr++;
            if (ch < '0' || ch > '9') break;
            y = y * 10 + ch - '0';
            if (y > Y) break;
        }
        --y;
        if (y >= Y) break;
        emplace_back(x, y);
        if (!ch) return;
        if (ch != '-') break;
    }
    throw_logic("Could not parse full move '" + string(str) + "'");
}

Coord FullMove::from() const {
    if (size() == 0) throw_logic("Empty full_move");
    return (*this)[0];
}

Coord FullMove::to()   const {
    if (size() == 0) throw_logic("Empty full_move");
    return (*this)[size()-1];
}

Move FullMove::move() const {
    if (size() == 0) throw_logic("Empty full_move");
    return Move{(*this)[0], (*this)[size()-1]};
}

void FullMove::move_expand(Board const& board, Move const& move) {
    emplace_back(move.from);
    if (move.from.parity() != move.to.parity()) {
        // Must be a slide. Check though
        emplace_back(move.to);
        auto slide_targets = move.from.slide_targets();
        for (uint r = 0; r < RULES; ++r, slide_targets.next())
            if (slide_targets.current() == move.to) return;
        throw_logic("Move is not a slide but has different parity");
    }

    // Must be a jump
    Image image{board};
    if (CLOSED_LOOP) image.set(move.from, EMPTY);
    array<Coord, MAX_X*MAX_Y/4+1> reachable;
    array<int,    MAX_X*MAX_Y/4+1> previous;
    reachable[0] = move.from;
    int nr_reachable = 1;
    for (int i=0; i < nr_reachable; ++i) {
        auto jumpees      = reachable[i].jumpees();
        auto jump_targets = reachable[i].jump_targets();
        for (uint r = 0; r < RULES; ++r, jumpees.next(), jump_targets.next()) {
            auto const jumpee = jumpees.current();
            auto const target = jump_targets.current();
            if (!image.jumpable(jumpee, target)) continue;
            image.set(target, COLORS);
            previous [nr_reachable] = i;
            reachable[nr_reachable] = target;
            if (target == move.to) {
                array<Coord, MAX_X*MAX_Y/4+1> trace;
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
    throw_logic("Move is a not a jump but has the same parity");
}

FullMove::FullMove(Board const& board_from, Board const& board_to) : FullMove{} {
    //bool blue_diff = board_from.blue() !=  board_to.blue();
    //bool red_diff  = board_from.red()  !=  board_to.red();
    // Temporary workaround until C++17 will support overallocated classes
    bool blue_diff = 0 != memcmp(&board_from.blue(), &board_to.blue(), sizeof(Coord) * ARMY);
    bool red_diff  = 0 != memcmp(&board_from.red(),  &board_to.red(), sizeof(Coord) * ARMY);
    if (blue_diff && red_diff) throw_logic("Both players move");

    if (blue_diff) {
        Move move{board_from.blue(), board_to.blue()};
        move_expand(board_from, move);
        return;
    }
    if (red_diff) {
        Move move{board_from.red(), board_to.red()};
        move_expand(board_from, move);
        return;
    }
    if (PASS) return;
    if (!CLOSED_LOOP) throw_logic("Invalid null move");
    // CLOSED LOOP also needs a color hint
    throw_logic("closed loop analysis not implemented yet");
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

void Tables::init() {
    array<array<Norm, 2*MAX_X-1>, 2*MAX_Y-1> norm;
    array<array<Norm, 2*MAX_X-1>, 2*MAX_Y-1> distance;
    array<Diff,MAX_RULES> directions;
    uint rule = 0;
    infinity_ = 0;
    for (uint y=0; y<2*Y-1; ++y) {
        int const dy = y - (Y-1);
        int const ay = abs(dy);
        for (uint x=0; x<2*X-1; ++x) {
            int const dx = x - (X-1);
            int const ax = abs(dx);
            Norm n =
                RULES == 8 ? max(ax, ay) :
                RULES == 6 ? (ax+ay+abs(dx+dy))/2 :
                RULES == 4 ? ax+ay :
                (throw_logic("Unknown rule " + to_string(RULES)), 1);
            norm[y][x] = n;
            distance[y][x] = n <= 2 ? 0 : n-2;
            infinity_ = max(infinity_, n);
            if (n == 1) {
                if (rule >= RULES) throw_logic("too many moves");
                directions[rule++] = Diff{dx, dy};
            }
        }
    }
    if (rule < RULES) throw_logic("too few directions");
    lib_sort(&directions[0], &directions[RULES]);
    if (false) {
        cout << "Rules:\n";
        for (uint r=0; r<RULES; ++r) cout << "  " << directions[r] << "\n";
    }
    ++infinity_;
    if (infinity_ >= NBITS)
        throw_logic("Max distance does not fit in Nbits");
    Ninfinity_ = NLEFT >> infinity_;

    // Set up distance tables
    for (uint y_slow = 0; y_slow < Y; ++y_slow) {
        auto y_top = y_slow + (Y-1);
        for (uint x_slow = 0; x_slow < X; ++x_slow) {
            auto& distances = distance_[Coord{x_slow, y_slow}];
            auto x_top = x_slow + (X-1);
            for (uint y=0; y<Y; ++y) {
                auto dy = y_top - y;
                for (uint x=0; x<X; ++x) {
                    auto dx = x_top - x;
                    distances[Coord{x, y}] = distance[dy][dx];
                }
            }
        }
    }

    // Set up move tables (slide and jump)
    for (uint y=0; y<Y; ++y) {
        for (uint x=0; x<X; ++x) {
            Coord pos{x, y};
            Offsets slide_targets, jumpees, jump_targets;
            int jump  = 0;
            int slide = 0;
            for (uint r=0; r<RULES; ++r) {
                int tx = x + directions[r].dx();
                if (tx < 0 || tx >= static_cast<int>(X)) continue;
                int ty = y + directions[r].dy();
                if (ty < 0 || ty >= static_cast<int>(Y)) continue;
                slide_targets[slide] = Coord{tx, ty};
                ++slide;

                int jx = tx + directions[r].dx();
                if (jx < 0 || jx >= static_cast<int>(X)) continue;
                int jy = ty + directions[r].dy();
                if (jy < 0 || jy >= static_cast<int>(Y)) continue;
                jumpees     [jump] = Coord{tx, ty};
                jump_targets[jump] = Coord{jx, jy};
                ++jump;
            }
            while (slide < 8) {
                slide_targets[slide] = pos;
                ++slide;
            }
            slide_targets_[pos].set(slide_targets);
            while (jump < 8) {
                jumpees     [jump] = pos;
                jump_targets[jump] = pos;
                ++jump;
            }
            jumpees_     [pos].set(jumpees);
            jump_targets_[pos].set(jump_targets);
        }
    }

    // Fill base
    base_blue_.zero();
    base_red_ .zero();
    auto& blue = start_.blue();
    auto& red  = start_.red();
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
            base_blue_[blue[i]] = 1;
            base_red_ [red [i]] = 1;
            ++i;
            --x;
            ++y;
        }
        d++;
    }
    lib_sort(blue.begin(), blue.end());
    lib_sort(red .begin(), red .end());
    if (DO_ALIGN)
        for (uint i=ARMY; i < ARMY+ARMY_PADDING; ++i) {
            blue[i] = Coord::MAX();
            red [i] = Coord::MAX();
        }

    for (auto const pos: blue) {
        if (base_red_[pos])
            throw_logic("Red and blue overlap");
        if (!base_blue_[pos.symmetric()])
            throw_logic("Asymmetric bases not supported (yet)");
    }

    // Set up edge, distance and parity tables
    for (uint y=0; y < Y; ++y) {
        Norm d = infinity_;
#if !__BMI2__
        Parity y_parity = y%2;
#endif // !__BMI2__
        for (uint x=0; x < X; ++x) {
            Coord const pos{x, y};
            for (uint i=0; i<ARMY; ++i) {
                uint dy = (Y-1)+y-red[i].y();
                uint dx = (X-1)+x-red[i].x();
                Norm d1 = norm[dy][dx];
                if (d1 < d) d = d1;
            }
            Ndistance_base_red_[pos] =
                d == 0 ? Ninfinity_ :
                d > 2  ? NLEFT >> (d-2) :
                NLEFT;
            edge_red_[pos] = d == 1;
#if !__BMI2__
            Parity x_parity = x % 2;
            parity_[pos]  = 2*y_parity + x_parity;
#endif // !__BMI2__
        }
    }

    parity_count_.fill(0);
    for (auto const r: red)
        ++parity_count_[parity(r)];

    deep_red_.fill(0);
    shallow_red_.fill(0);
    deepness_.fill(2);
    nr_deep_red_ = 0;
    uint shallow_red = ARMY;
    BoardTable<uint8_t> seen;
    seen.zero();
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            Coord const pos{x, y};
            nr_slide_jumps_red_[pos] = 0;
            if (base_red_[pos]) {
                deep_red_[pos] = 1;
                deepness_[pos] = 0;
                auto targets = jump_targets(pos);
                for (uint r = 0; r < RULES; ++r, targets.next()) {
                    auto const target = targets.current();
                    if (!base_red(target)) {
                        shallow_red_[pos] = 1;
                        deep_red_[pos] = 0;
                        deepness_[pos] = 2;
                        deep_red_base_[--shallow_red] = pos;
                        break;
                    }
                }
                if (shallow_red_[pos]) continue;

                targets = slide_targets(pos);
                for (uint r = 0; r < RULES; ++r, targets.next()) {
                    auto const target = targets.current();
                    if (!base_red(target)) {
                        shallow_red_[pos] = 1;
                        deep_red_[pos] = 0;
                        deepness_[pos] = 2;
                        deep_red_base_[--shallow_red] = pos;
                        break;
                    }
                }

                deep_red_base_[nr_deep_red_++] = pos;
            } else {
                auto const pos_parity = parity(pos);
                // We are outside the red base
                auto steps1 = slide_targets(pos);
                for (uint r = 0; r < RULES; ++r, steps1.next()) {
                    auto const target1 = steps1.current();
                    if (target1 == pos) break;
                    if (base_red_[target1]) continue;
                    auto steps2 = slide_targets(target1);
                    for (uint r = 0; r < RULES; ++r, steps2.next()) {
                        auto const target2 = steps2.current();
                        if (target2 == target1) break;
                        if (!base_red_[target2]) continue;
                        if (parity(target2) != pos_parity) continue;
                        if (seen[target2]) continue;
                        seen[target2] = 1;
                        if (nr_slide_jumps_red_[pos] >= RULES)
                            throw_logic("Too many slide_jumps_red");
                        slide_jumps_red_[pos][nr_slide_jumps_red_[pos]++] = target2;
                    }
                }
                for (uint i=0; i < nr_slide_jumps_red_[pos]; ++i)
                    seen[slide_jumps_red_[pos][i]] = 0;
            }
        }
    }
    if (nr_deep_red_ != shallow_red) throw_logic("Inconsistent deep_red_base");
    medium_red_ = ARMY;
    for (uint i=0; i<nr_deep_red_; ++i) {
        Coord pos = deep_red_base_[i];
        auto j_targets = jump_targets(pos);
        for (uint r = 0; r < RULES; ++r, j_targets.next()) {
            auto const target = j_targets.current();
            if (target == pos) break;
            if (deepness_[target] == 2) {
                deepness_[target] = 1;
                --medium_red_;
            }
        }
    }
    sort(&deep_red_base_[nr_deep_red_], &deep_red_base_[ARMY],
         [this](Coord left, Coord right) {
             return deepness_[left] > deepness_[right];
         });

    min_nr_moves_ = start_.min_nr_moves();
}

void Tables::print_Ndistance_base_red(ostream& os) const {
    os << hex;
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << setw(11) << Ndistance_base_red(pos);
        }
        os << "\n";
    }
    os << dec;
}

void Tables::print_base_red(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(base_red(pos));
        }
        os << "\n";
    }
}

void Tables::print_base_blue(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(base_blue(pos));
        }
        os << "\n";
    }
}

void Tables::print_edge_red(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(edge_red(pos));
        }
        os << "\n";
    }
}

void Tables::print_deep_red(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(deep_red(pos));
        }
        os << "\n";
    }
}

void Tables::print_shallow_red(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(shallow_red(pos));
        }
        os << "\n";
    }
}

void Tables::print_deepness(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(deepness(pos));
        }
        os << "\n";
    }
}

void Tables::print_parity(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(parity(pos));
        }
        os << "\n";
    }
}

void Tables::print_nr_slide_jumps_red(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            Coord const pos{x, y};
            os << " " << static_cast<uint>(nr_slide_jumps_red(pos));
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

Tables tables;

size_t BoardSubsetBlue::memory_report() const {
    if (armies_) return allocated();
    return 0;
}

void BoardSetBlue::insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red) {
    Statistics dummy_stats;

    Army const& blue = board.blue();
    Army const blue_symmetric{blue, SYMMETRIC};
    int blue_symmetry = cmp(blue, blue_symmetric);
    ArmyPos const blueE{blue_symmetry >= 0 ? blue : blue_symmetric};
    auto blue_id = armies_blue.insert(blueE, dummy_stats);

    Army const& red = board.red();
    Army const red_symmetric{red, SYMMETRIC};
    int red_symmetry = cmp(red, red_symmetric);
    ArmyPos const redE{red_symmetry >= 0 ? red : red_symmetric};
    auto red_id = armies_red.insert(redE, dummy_stats);

    int symmetry = blue_symmetry * red_symmetry;

    pre_write(1);
    insert(blue_id, red_id, symmetry, dummy_stats);
    post_write();
}

bool BoardSetBlue::find(Board const& board, ArmySet const& armies_blue, ArmySet const& armies_red) const {
    Army const& blue = board.blue();
    Army const blue_symmetric{blue, SYMMETRIC};
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.find(blue_symmetry >= 0 ? blue : blue_symmetric);
    if (blue_id == 0) return false;

    Army const& red = board.red();
    Army const red_symmetric{red, SYMMETRIC};
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.find(red_symmetry >= 0 ? red : red_symmetric);
    if (red_id == 0) return false;

    int symmetry = blue_symmetry * red_symmetry;
    return find(blue_id, red_id, symmetry);
}

Board BoardSetBlue::example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const {
    if (empty()) throw_logic("No board in BoardSet");
    for (ArmyId blue_id = from(); blue_id <= back_id(); ++blue_id) {
        auto const& subset = at(blue_id);
        if (subset.empty()) continue;
        ArmyId red_id, symmetry;
        red_id = subset.example(symmetry);
        ArmySet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmySet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        Army const blue{blue_armies, blue_id};
        Army const red {red_armies,  red_id, symmetry};
        return Board{blue, red};
    }
    throw_logic("No board even though BoardSetBlue is not empty");
}

Board BoardSetBlue::random_example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const {
    if (empty()) throw_logic("No board in BoardSet");

    ArmyId n = subsets();
    ArmyId step = n == 1 ? 0 : random_coprime(n);
    for (ArmyId i = random(n); true; i = (i + step) % n) {
        ArmyId blue_id = from() + i;
        auto const& subset = at(blue_id);
        if (subset.empty()) continue;
        ArmyId red_id, symmetry;
        red_id = subset.random_example(symmetry);
        ArmySet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmySet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        Army const blue{blue_armies, blue_id};
        Army const red {red_armies,  red_id, symmetry};
        return Board{blue, red};
    }
}

size_t BoardSetBlue::memory_report(ostream& os, string const& prefix) const {
    size_t sz = 0;
    os << prefix << " subsets = ";
    if (subsets_) {
        os << "BoardSubset[" << capacity() << "] (" << capacity() * sizeof(subsets_[0]) << " bytes)\n";
        sz += capacity() * sizeof(subsets_[0]);
        size_t total_subset_size = 0;
        for (auto& subset: *this)
            total_subset_size += subset.memory_report();
        os << prefix << " Total in subsets ArmyId[" << total_subset_size << "] (" << total_subset_size * sizeof(ArmyId) << " bytes)\n";
        sz += total_subset_size * sizeof(ArmyId);
    } else os << "nullptr\n";

    os << prefix << "BoardSetBlue total memory = " << sz << " bytes\n";
    return sz;
}

bool BoardSubsetRed::_insert(ArmyId red_value, Statistics& stats) {
    // cout << "Insert " << red_value << "\n";
    if (!main_thread())
        throw_logic("BoardSubsetRed::_insert should not be called from a subthread");
    if (armies_) throw_logic("Multiple single insert in BoardSubsetRed");

    stats.boardset_probe(0);
    ArmyId* new_list = mallocate<ArmyId>(1);
    new_list[0] = red_value;
    *this = BoardSubsetRed{new_list, 1};
    return true;
}

size_t BoardSubsetRed::memory_report() const {
    if (armies_) return size();
    return 0;
}

void BoardSubsetRed::print(ostream& os) const {
    ArmyId sz = size();
    for (ArmyId i=0; i < sz; ++i) {
        ArmyId red_id;
        auto symmetry = split(armies_[i], red_id);
        os << " " << red_id << (symmetry ? "-" : "+");
    }
}

bool BoardSetRed::_insert(ArmyId blue_id, ArmyId red_id, int symmetry, Statistics& stats) {
    if (CHECK) {
        if (UNLIKELY(blue_id <= 0))
            throw_logic("red_id <= 0");
    }

    if (UNLIKELY(blue_id >= top_))
        throw_logic("High blue id " + to_string(blue_id) + " >= " + to_string(top_) + ". BoardSubsetRed not properly presized");

    // No locking because each thread works on one blue id and the
    // subsets_ area is presized
    // lock_guard<mutex> lock{exclude_};

    bool result = at(blue_id)._insert(red_id, symmetry, stats);
    size_ += result;
    stats.boardset_size(size_);
    return result;
}

void BoardSetRed::insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red) {
    Statistics dummy_stats;

    Army const& blue = board.blue();
    Army const blue_symmetric{blue, SYMMETRIC};
    int blue_symmetry = cmp(blue, blue_symmetric);
    ArmyPos blueE{blue_symmetry >= 0 ? blue : blue_symmetric};
    auto blue_id = armies_blue.insert(blueE, dummy_stats);

    Army const& red = board.red();
    Army const red_symmetric{red, SYMMETRIC};
    int red_symmetry = cmp(red, red_symmetric);
    ArmyPos redE{red_symmetry >= 0 ? red : red_symmetric};
    auto red_id = armies_red.insert(redE, dummy_stats);

    int symmetry = blue_symmetry * red_symmetry;

    pre_write(1);
    BoardSubsetRedBuilder& subset_to = builder();
    subset_to.insert(red_id, symmetry, dummy_stats);
    if (STATISTICS) dummy_stats.subset_size(subset_to.size());
    insert(blue_id, subset_to);
    post_write();
}

bool BoardSetRed::find(Board const& board, ArmySet const& armies_blue, ArmySet const& armies_red) const {
    Army const& blue = board.blue();
    Army const blue_symmetric{blue, SYMMETRIC};
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.find(blue_symmetry >= 0 ? blue : blue_symmetric);
    if (blue_id == 0) return false;

    Army const& red = board.red();
    Army const red_symmetric{red, SYMMETRIC};
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.find(red_symmetry >= 0 ? red : red_symmetric);
    if (red_id == 0) return false;

    int symmetry = blue_symmetry * red_symmetry;
    return _find(blue_id, red_id, symmetry);
}

Board BoardSetRed::example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const {
    if (empty()) throw_logic("No board in BoardSet");
    for (ArmyId blue_id = from(); blue_id <= back_id(); ++blue_id) {
        auto const& subset_red = at(blue_id);
        if (subset_red.empty()) continue;
        ArmyId red_id, symmetry;
        if (red_file)
            red_id = subset_red.example(*builders_[subset_red.thread_id()], symmetry);
        else
            red_id = subset_red.example(symmetry);
        ArmySet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmySet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        Army const blue{blue_armies, blue_id};
        Army const red {red_armies,  red_id, symmetry};
        return Board{blue, red};
    }
    throw_logic("No board even though BoardSetBlue is not empty");
}

Board BoardSetRed::random_example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const {
    if (empty()) throw_logic("No board in BoardSet");

    ArmyId n = subsets();
    ArmyId step = n == 1 ? 0 : random_coprime(n);
    for (ArmyId i = random(n); true; i = (i + step) % n) {
        ArmyId blue_id = from() + i;
        auto const& subset_red = at(blue_id);
        if (subset_red.empty()) continue;
        ArmyId red_id, symmetry;
        if (red_file)
            red_id = subset_red.random_example(*builders_[subset_red.thread_id()], symmetry);
        else
            red_id = subset_red.random_example(symmetry);
        ArmySet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmySet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        Army const blue{blue_armies, blue_id};
        Army const red {red_armies,  red_id, symmetry};
        return Board{blue, red};
    }
}

size_t BoardSetRed::memory_report(ostream& os, string const& prefix) const {
    size_t sz = 0;
    os << prefix << "subsets = ";
    if (subsets_) {
        os << "BoardSubsetRed[" << capacity() << "] (" << capacity() * sizeof(subsets_[0]) << " bytes)\n";
        sz += capacity() * sizeof(subsets_[0]);

        if (red_file)
            os << prefix << " Total in subsets = 0 (index into fd mmap)\n";
        else {
            size_t total_subset_size = 0;
            for (auto& subset: *this)
                total_subset_size += subset.memory_report();
            os << prefix << " Total in subsets ArmyId[" << total_subset_size << "] (" << total_subset_size * sizeof(ArmyId) << " bytes)\n";
            sz += total_subset_size * sizeof(ArmyId);
        }
    } else os << "nullptr\n";

    uint i=0;
    auto pre = prefix + "  ";
    for (auto& builder: builders_) {
        os << prefix << " BoardSubsetRedBuilder[" << i << "]:";
        if (builder) {
            os << "\n";
            sz += builder->memory_report(os, pre);
        } else
            os << " ----\n";

        ++i;
    }

    os << prefix << "BoardSetRed total memory = " << sz << " bytes\n";
    return sz;
}

int Board::min_nr_moves(bool blue_to_move) const {
    blue_to_move = blue_to_move ? true : false;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = tables.Ninfinity();
    int off_base_from = 0;
    ParityCount parity_count_from = tables.parity_count();
    int edge_count_from = 0;

    for (auto const b: blue()) {
        --parity_count_from[b.parity()];
        if (b.base_red()) continue;
        ++off_base_from;
        edge_count_from += b.edge_red();
        Ndistance_red |= b.Ndistance_base_red();
        for (auto const r: red())
            Ndistance_army |= tables.Ndistance(r, b);
    }
    int slides = 0;
    for (auto tc: parity_count_from)
        slides += max(tc, 0);
    int distance_army = __builtin_clz(Ndistance_army);
    int distance_red  = __builtin_clz(Ndistance_red);

    if (verbose) {
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

void Army::do_move(Move const& move) {
    auto pos = equal_range(begin(), end(), move.from);
    if (pos.first == pos.second)
        throw_logic("Move not found");
    *pos.first = move.to;
    sort();
}

bool Army::_try_move(Move const& move) {
    auto pos = equal_range(begin(), end(), move.from);
    if (pos.first == pos.second) return false;
    *pos.first = move.to;
    sort();
    return true;
}

void Board::do_move(Move const& move) {
    if (blue()._try_move(move)) return;
    if (red ()._try_move(move)) return;
    throw_logic("Move not found");
}

string const Svg::file(string const& prefix, uint nr_moves) {
    return string(prefix + "/halma-X") + to_string(X) + "Y" + to_string(Y) + "Army" + to_string(ARMY) + "Rule" + to_string(RULES) + "_" + to_string(nr_moves) + ".html";
}

void Svg::html_header(uint nr_moves, int target_moves, bool terminated) {
    out_ <<
        "<html>\n"
        "  <head>\n"
        "    <style>\n"
        "      span.blue { color: blue; }\n"
        "      span.red  { color: red; }\n"
        "      .blue .available_moves { color: blue; }\n"
        "      .red  .available_moves { color: red; }\n"
        "      table,tr,td,th { border: 1px solid black; }\n"
        "      .red  td { border: 1px solid red; }\n"
        "      .blue td { border: 1px solid blue; }\n"
        "      .stats td { text-align: right; }\n"
        "      .parameters th { text-align: left; }\n"
        "    </style>\n"
        "  </head>\n"
        "  <body>\n";
    if (terminated)
        out_ << "    <h1>Warning: Terminated run</h1>\n";
    out_ <<
        "    <h1>" << nr_moves << " / " << target_moves << " moves</h1>\n";
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
        "        <marker id='arrowhead' markerWidth='" << w << "' markerHeight='" << 2*h << "'\n"
        "                refX='" << w << "' refY='" << h << "' orient='auto'>\n"
        "          <polygon points='0 0, " << w << " " << h << ", 0 " << 2*h << "' />\n"
        "        </marker>\n"
        "      </defs>\n";
}

void Svg::footer() {
    out_ << "    </svg>\n";
}

void Svg::parameters(time_t start_time, time_t stop_time) {
    out_ <<
        "    <table class='parameters'>\n"
        "      <tr class='x'><th>X</th><td>" << X << "</td></tr>\n"
        "      <tr class='y'><th>Y</th><td>" << Y << "</td></tr>\n"
        "      <tr class='army'><th>Army</th><td>" << ARMY << "</td></tr>\n"
        "      <tr class='rule'><th>Rule</th><td>" << RULES << "-move</td></tr>\n"
        "      <tr class='lower_bound'><th>Bound</th><td> &ge; " << tables.min_nr_moves() << " moves</td></tr>\n"
        "      <tr class='heuristics'><th>Heuristics</th><td>";
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
        "      <tr class='host'><th>Host</th><td>" << HOSTNAME << "</td></tr>\n"
        "      <tr class='cpus'><th>CPUs</th><td>" << NR_CPU << "</td></tr>\n"
        "      <tr class='threads'><th>Threads</th><td>" << nr_threads << "</td></tr>\n"
        "      <tr class='start_time'><th>Start</th><td>" << time_string(start_time) << "</td></tr>\n"
        "      <tr class='stop_time'><th>Stop</th><td>"  << time_string(stop_time) << "</td></tr>\n"
        "      <tr class='commit_id'><th>Commit id</th><td>" << VCS_COMMIT << "</td></tr>\n"
        "      <tr class='commit_time'><th>Commit time</th><td>" << VCS_COMMIT_TIME << "</td></tr>\n"
        "    </table>\n";
}

void Svg::move(FullMove const& move) {
    out_ << "      <polyline points='";
    for (auto const pos: move) {
        out_ << pos.x() * scale_ + scale_/2 + margin_ << "," << pos.y() * scale_ + scale_/2 + margin_ << " ";
    }
    out_ << "' stroke='black' stroke-width='" << scale_/10 << "' fill='none' marker-end='url(#arrowhead)' />\n";
}

void Svg::game(BoardList const& boards) {
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
    string const color_span[] = {
        "      <span class='red'>",
        "      <span class='blue'>",
    };
    string moves_string;
    for (auto const& full_move: full_moves) {
        moves_string += color_span[color_index];
        moves_string += full_move.str();
        moves_string += "</span>,\n";
        color_index = !color_index;
    }
    if (full_moves.size()) {
        moves_string.pop_back();
        moves_string.pop_back();
        out_ << "    <p>\n" << moves_string << "\n    </p>\n";
    }
}

void Svg::stats(string const& cls, StatisticsList const& stats_list) {
    char buffer[1024];
    buffer[sizeof(buffer)-1] = 0;
    out_ <<
        "    <table class='stats " << cls << "'>\n"
        "      <tr>\n"
        "        <th>Moves<br/>left</th>\n"
        "        <th>Seconds</th>\n"
        "        <th>Boards</th>\n"
        "        <th>Armies</th>\n"
        "        <th>Memory<br/>(MB)</th>\n"
        "        <th>Allocated<br/>(MB)</th>\n"
        "        <th>Mmapped<br/>(MB)</th>\n"
        "        <th>Mmaps</th>\n"
        "        <th>Mlocked<br/>(MB)</th>\n"
        "        <th>Mlocks</th>\n"
        "        <th>Boards per<br/>blue army</th>\n";
    if (statistics) {
        out_ <<
            "        <th>Largest<br/>subset</th>\n"
            "        <th>Late<br/>prunes</th>\n"
            "        <th>Late<br/>prune<br/>ratio</th>\n"
            "        <th>Army<br/>resize<br/>overflow</th>\n";
        if (ARMYSET_CACHE)
            out_ <<
                "        <th>Army<br/>front<br/>inserts</th>\n"
                "        <th>Army<br/>front<br/>ratio</th>\n";
        out_ <<
            "        <th>Army inserts</th>\n"
            "        <th>Army<br/>ratio</th>\n"
            "        <th>Board inserts</th>\n"
            "        <th>Board<br/>ratio</th>\n"
            "        <th>Red base edge</th>\n"
            "        <th>Edge<br/>ratio</th>\n";
    }
    if (hash_statistics) {
        out_ <<
            "        <th>Army immediate</th>\n"
            "        <th>Army<br/>immediate<br/>ratio</th>\n"
            "        <th>Army<br/>probes</th>\n"
            "        <th>Board immediate</th>\n"
            "        <th>Board<br/>immediate<br/>ratio</th>\n"
            "        <th>Board<br/>probes</th>\n";
    }
    out_ <<
        "      </tr>\n";
    Statistics::Counter old_boards = 1;
    size_t stats_size = stats_list.size();
    for (size_t i=0; i < stats_size; ++i) {
        auto const& st = stats_list[i];
        Statistics::Counter nr_boards = i+1 < stats_size && stats_list[i+1].boardset_uniques() ? stats_list[i+1].boardset_uniques() : st.boardset_size();
        out_ <<
            "      <tr class='" << st.css_color() << "'>\n"
            "        <td class='available_moves'>" << st.available_moves() << "</td>\n"
            "        <td class='duration'>" << st.duration() << "</td>\n"
            "        <td class='boards'>" << nr_boards << "</td>\n"
            "        <td class='armies'>" << st.armyset_size() << "</td>\n"
            "        <td class='memory'>" << st.memory()    / 1000000 << "</td>\n"
            "        <td class='allocated'>" << st.allocated() / 1000000 << "</td>\n"
            "        <td class='mmapped'>" << st.mmapped() / 1000000 << "</td>\n"
            "        <td class='mmaps'>" << st.mmaps() << "</td>\n"
            "        <td class='mlocked'>" << st.mlocked() / 1000000 << "</td>\n"
            "        <td class='mlocks'>" << st.mlocks() << "</td>\n"
            "        <td>" << st.boardset_size()/(st.blue_armies_size() ? st.blue_armies_size() : 1) << "</td>\n";
        if (statistics) {
            out_ <<
                "        <td>" << st.largest_subset() << "</td>\n"
                "        <td>" << st.late_prunes() << "</td>\n"
                "        <td>" << st.late_prunes() * 100 / old_boards << "%</td>\n"
                "        <td>" << st.overflow_max() << "</td>\n";
            if (ARMYSET_CACHE) {
                auto army_tries = st.armyset_tries() + st.armyset_cache_hits();
                out_ <<
                    "        <td>" << st.armyset_cache_hits() << " / " << army_tries << "</td>\n"
                    "        <td>";
                if (army_tries)
                    out_ << st.armyset_cache_hits()*100 / army_tries;
                out_ << "</td>\n";
            }
            out_ <<
                "        <td>" << st.armyset_size() << " / " << st.armyset_tries() << "</td>\n"
                "        <td>";
            if (st.armyset_tries())
                out_ << st.armyset_size()*100 / st.armyset_tries() << "%";

            out_ <<
                "</td>\n"
                "        <td>" << nr_boards << " / " << st.boardset_tries() << "</td>\n"
                "        <td>";
            if (st.boardset_tries())
                out_ << nr_boards*100 / st.boardset_tries() << "%";
            out_ << "</td>\n"
                "</td>\n"
                "        <td>" << st.edges() << " / " << st.boardset_size() << "</td>\n"
                "        <td>";
            if (st.boardset_size())
                out_ << st.edges()*100 / st.boardset_size() << "%";
            out_ << "</td>\n";
        }
        if (hash_statistics) {
            out_ <<
                "        <td>" << st.armyset_immediate() << " / " << st.armyset_tries() << "</td>\n";
            if (st.armyset_tries()) {
                snprintf(buffer, sizeof(buffer)-1,
                         "        <td>%.0f%%</td>\n"
                         "        <td>%.2f</td>\n",
                         st.armyset_immediate() * 100. / st.armyset_tries(),
                         1 + 1. * st.armyset_probes() / st.armyset_tries());
                out_ << &buffer[0];
            } else {
                out_ <<
                    "        <td></td>\n"
                    "        <td></td>\n";
            }
            out_ <<
                "        <td>" << st.boardset_immediate() << " / " << st.boardset_tries() << "</td>\n";
            if (st.boardset_tries()) {
                snprintf(buffer, sizeof(buffer)-1,
                         "        <td>%.0f%%</td>\n"
                         "        <td>%.2f</td>\n",
                         st.boardset_immediate() * 100. / st.boardset_tries(),
                         1 + 1. * st.boardset_probes() / st.boardset_tries());
                out_ << buffer;
            } else {
                out_ <<
                    "        <td></td>\n"
                    "        <td></td>\n";
            }
        }
        out_ <<
            "      </tr>\n";
        old_boards = st.boardset_size();
    }
    out_ << "    </table>\n";

    if (any_of(stats_list.begin(), stats_list.end(),
               [](StatisticsE const& st) { return st.example(); })) {
        out_ << "<h4>Sample boards</h4>\n";
        header();
        board(tables.start());
        footer();
    }

    for (auto const& st: stats_list)
        if (st.example()) {
            header();
            board(st.example_board());
            footer();
        }
}

void Svg::write(time_t start_time, time_t stop_time,
                int solution_moves, BoardList const& boards,
                StatisticsList const& stats_list_solve, Sec::rep solve_duration,
                StatisticsList const& stats_list_backtrack, Sec::rep backtrack_duration) {
    bool terminated = is_terminated();
    int target_moves = stats_list_solve[0].available_moves();
    if (solution_moves >= 0) {
        html_header(boards.size()-1, target_moves, terminated);
        parameters(start_time, stop_time);
        game(boards);
    } else {
        html_header(stats_list_solve.size()-1, target_moves, terminated);
        parameters(start_time, stop_time);
    }
    out_ << "<h3>Solve (" << solve_duration << " seconds)</h3>\n";
    stats("Solve", stats_list_solve);
    if (stats_list_backtrack.size()) {
        out_ << "<h3>Backtrack (" << backtrack_duration << " seconds)</h3>\n";
        stats("Backtrack", stats_list_backtrack);
    }
    html_footer();

    string const svg_file =
        attempt || terminated ? attempts_file(target_moves) :
        solution_moves >= 0 ? solution_file(boards.size()-1) :
        heuristics()        ? attempts_file(target_moves) :
        failures_file(target_moves);
    string const svg_file_tmp = svg_file + "." + HOSTNAME + ".new";
    ofstream svg_fh;
    svg_fh.exceptions(ofstream::failbit | ofstream::badbit);
    try {
        svg_fh.open(svg_file_tmp, ofstream::out);
        svg_fh << *this;
        svg_fh.close();
        int rc = rename(svg_file_tmp.c_str(), svg_file.c_str());
        if (rc)
            throw(system_error(errno, system_category(),
                               "Could not rename '" + svg_file_tmp + "' to '" +
                               svg_file + "'"));
    } catch(exception& e) {
        rm_file(svg_file_tmp);
        throw;
    }
}

size_t memory_report
(ostream& os,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet const& moved_armies,
 BoardSetRed const& boards_from,
 BoardSetBlue    const& boards_to) {
    size_t sz = 0;
    os << "moving armies (" << static_cast<void const*>(&moving_armies) << "):\n";
    sz += moving_armies.memory_report(os, " ");
    os << "opponent armies (" << static_cast<void const*>(&opponent_armies) << "):\n";
    sz += opponent_armies.memory_report(os, " ");
    os << "moved armies (" << static_cast<void const*>(&moved_armies) << "):\n";
    sz += moved_armies.memory_report(os, " ");

    os << "boards from (" << static_cast<void const*>(&boards_from) << "):\n";
    sz += boards_from.memory_report(os, " ");
    os << "boards to (" << static_cast<void const*>(&boards_to) << "):\n";
    sz += boards_to.memory_report(os, " ");

    os << "Total memory " << sz << " bytes\n";

    return sz;
}

size_t memory_report
(ostream& os,
 ArmySet const& moving_armies,
 ArmySet const& opponent_armies,
 ArmySet const& moved_armies,
 BoardSetBlue    const& boards_from,
 BoardSetRed const& boards_to) {
    size_t sz = 0;
    os << "moving armies (" << static_cast<void const*>(&moving_armies) << "):\n";
    sz += moving_armies.memory_report(os, " ");
    os << "opponent armies (" << static_cast<void const*>(&opponent_armies) << "):\n";
    sz += opponent_armies.memory_report(os, " ");
    os << "moved armies (" << static_cast<void const*>(&moved_armies) << "):\n";
    sz += moved_armies.memory_report(os, " ");

    os << "boards from (" << static_cast<void const*>(&boards_from) << "):\n";
    sz += boards_from.memory_report(os, " ");
    os << "boards to (" << static_cast<void const*>(&boards_to) << "):\n";
    sz += boards_to.memory_report(os, " ");

    os << "Total memory " << sz << " bytes\n";

    return sz;
}

void play(bool print_moves=false) COLD;
void play(bool print_moves) {
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
        throw_logic("No hardcoded replay game");
    }

    int nr_moves = game.size();
    for (auto& move: game) {
        cout << board;
        // cout << board.symmetric();

        ArmySet  army_set[3];
        bool blue_to_move = nr_moves & 1;
        if (blue_to_move) {
            BoardSetRed red_boards{false};
            red_boards.grow(1);
            red_boards.insert(board, army_set[0], army_set[1]);
            army_set[0].drop_hash();
            army_set[1].convert_hash();

            BoardSetBlue    blue_boards;
            auto stats = make_all_blue_moves
                (red_boards, blue_boards,
                 army_set[0], army_set[1], army_set[2],
                 nr_moves);
            --nr_moves;
            board.do_move(move);
            army_set[2].convert_hash();
            blue_boards.sort_compress();
            if (blue_boards.find(board, army_set[2], army_set[1])) {
                cout << "Good\n";
            } else {
                cout << "Bad\n";
            }
            cout << stats << "===============================\n";
        } else {
            BoardSetBlue    blue_boards;
            blue_boards.insert(board, army_set[1], army_set[0]);
            army_set[0].drop_hash();
            army_set[1].convert_hash();

            BoardSetRed red_boards{false};
            red_boards.grow(blue_boards.back_id());
            auto stats = make_all_red_moves
                (blue_boards, red_boards,
                 army_set[0], army_set[1], army_set[2],
                 nr_moves);
            --nr_moves;
            board.do_move(move);
            army_set[2].convert_hash();
            if (red_boards.find(board, army_set[1], army_set[2])) {
                cout << "Good\n";
            } else {
                cout << "Bad\n";
            }
            cout << stats << "===============================\n";
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

void save_largest_red(vector<ArmyId> const& largest_red) {
    string const file{sample_subset_red};
    string const file_tmp = file + "." + HOSTNAME + ".new";

    ofstream fh;
    fh.exceptions(ofstream::failbit | ofstream::badbit);
    try {
        fh.open(file_tmp, ofstream::out);
        fh << hex;
        for (ArmyId const& red_value: largest_red) {
            ArmyId red_id;
            bool symmetry = BoardSubsetBlue::split(red_value, red_id) != 0;

            fh << red_id << (symmetry ? "-" : "+") << "\n";
        }
        fh << dec;
        fh.close();
        int rc = rename(file_tmp.c_str(), file.c_str());
        if (rc)
            throw(system_error(errno, system_category(),
                               "Could not rename '" + file_tmp + "' to '" +
                               file + "'"));
    } catch(exception& e) {
        rm_file(file_tmp);
        throw;
    }
}

int solve(Board const& board, int nr_moves, Army& red_army,
          StatisticsList& stats_list, Sec::rep& duration) COLD;
int solve(Board const& board, int nr_moves, Army& red_army,
          StatisticsList& stats_list, Sec::rep& duration) {
    auto start_solve = chrono::steady_clock::now();
    cout << setw(14) << "set " << setw(2) << nr_moves << " (" << HOSTNAME;
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

    vector<ArmyId> largest_red;
    BoardSetBlue boards_blue;
    BoardSetRed  boards_red{red_file ? true : false};
    boards_red.grow(1);
    array<ArmySet, 3>  army_set;
    bool blue_to_move = nr_moves & 1;
    if (blue_to_move)
        boards_red.insert(board, army_set[0], army_set[1]);
    else
        boards_blue.insert(board, army_set[1], army_set[0]);
    army_set[0].drop_hash();
    army_set[1].drop_hash();

    ArmyId red_id = 0;
    int i;
    for (i=0; nr_moves>0; --nr_moves, ++i) {
        if (nr_moves == testQ) {
            cout << "User abort\n";
            return -1;
        }
        if (nr_moves == verbose_move) verbose = !verbose;

        auto& moving_armies         = army_set[ i    % 3];
        auto const& opponent_armies = army_set[(i+1) % 3];
        auto& moved_armies          = army_set[(i+2) % 3];

        bool blue_to_move = nr_moves & 1;
        if (blue_to_move) {
            lock_guard<ArmySet> lock{moved_armies};
            stats_list.emplace_back
                (make_all_blue_moves
                 (boards_red, boards_blue,
                  moving_armies, opponent_armies, moved_armies,
                  nr_moves));
            boards_red.clear(boards_blue.back_id());
        } else {
#if ARMYSET_SPARSE
            lock_guard<ArmySet> lock{moved_armies};
#endif // ARMYSET_SPARSE
            stats_list.emplace_back
                (make_all_red_moves
                 (boards_blue, boards_red,
                  moving_armies, opponent_armies, moved_armies,
                  nr_moves));
            boards_blue.clear();
        }
        // if (testQ) cout << moved_armies;
        if (is_terminated()) {
            auto stop_solve = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            return -1;
        }

        moving_armies.clear();
        moved_armies.drop_hash();

        if (sample_subset_red && !blue_to_move) {
            auto const& boards = boards_red;
            for (auto const& subset_red: boards) {
                if (subset_red.size() > largest_red.size())
                    largest_red.assign(subset_red.begin(), subset_red.end());
            }
        }

        // if (verbose) cout << moved_armies << boards_to;
        auto const& boards_to = blue_to_move ?
            static_cast<BoardSetBase const&>(boards_blue) :
            static_cast<BoardSetBase const&>(boards_red);
        auto& stats = stats_list.back();
        if (boards_to.size() == 0) {
            auto stop_solve = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            cout << stats << setw(6) << duration << " s, no solution" << endl;
            if (largest_red.size()) save_largest_red(largest_red);
            return -1;
        }
        if (example) {
            if (blue_to_move)
                stats.example_board
                    (example > 0 ?
                     boards_blue.example(opponent_armies, moved_armies, true) :
                     boards_blue.random_example(opponent_armies, moved_armies, true));
            else
                stats.example_board
                    (example > 0 ?
                     boards_red.example(opponent_armies, moved_armies, false) :
                     boards_red.random_example(opponent_armies, moved_armies, false));
            cout << stats.example_board();
        }
        cout << stats << flush;
        if (boards_to.solution_id()) {
            red_id = boards_to.solution_id();
            red_army = boards_to.solution();
            if (nr_moves > 1)
                cout << "Unexpected early solution. Bailing out" << endl;
            ++i;
            break;
        }
    }
    auto stop_solve = chrono::steady_clock::now();
    duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
    cout << setw(6) << duration << " s, solved" << endl;

    if (largest_red.size()) save_largest_red(largest_red);

    if (red_id == 0) throw_logic("Solved without solution");
    return i;
}

BoardSetPairs::BoardSetPairs(uint nr_moves): nr_moves_{nr_moves} {
    // We need nr_moves + 1 boards
    boardset_pairs_ = new BoardSetPair[nr_moves/2+1];
}

BoardSetPairs::~BoardSetPairs() {
    delete [] boardset_pairs_;
}

void backtrack(Board const& board, uint nr_moves, uint solution_moves,
               Army const& last_red_army,
               StatisticsList& stats_list, Sec::rep& duration,
               BoardList& boards) COLD;
void backtrack(Board const& board, uint nr_moves, uint solution_moves,
               Army const& last_red_army,
               StatisticsList& stats_list, Sec::rep& duration,
               BoardList& boards) {
    cout << "Start backtracking\n";

    if (solution_moves > nr_moves)
        throw_logic("solution_moves > nr_moves");
    if ((nr_moves - solution_moves) % 2)
        throw_logic("solution_moves - nr_moves must be even");

    // We don't support external storage for backtracking (yet)
    red_file = nullptr;

    auto start_backtrack = chrono::steady_clock::now();

    vector<unique_ptr<ArmySet>>  army_set;
    army_set.reserve(solution_moves+2);

    BoardSetPairs boardset_pairs{solution_moves};

    army_set.emplace_back(make_unique<ArmySet>());
    army_set.emplace_back(make_unique<ArmySet>());
    if (solution_moves & 1) {
        // BLUE does the first move
        boardset_pairs.red (solution_moves).grow(1);
        boardset_pairs.red (solution_moves).insert(board, *army_set[0], *army_set[1]);
    } else {
        // RED does the first move
        boardset_pairs.blue(solution_moves).insert(board, *army_set[1], *army_set[0]);
    }
    army_set[0]->convert_hash();
    army_set[1]->convert_hash();

    BoardTable<uint8_t> red_backtrack{};
    red_backtrack.zero();
    red_backtrack.set(last_red_army, 2);
    BoardTable<uint8_t> red_backtrack_symmetric{};
    red_backtrack_symmetric.zero();
    red_backtrack_symmetric.set(Army{last_red_army, SYMMETRIC}, 2);
    if (verbose) {
        cout << "red_backtrack:\n";
        for (uint y=0; y<Y; ++y) {
            for (uint x=0; x<X; ++x)
                cout << " " << static_cast<uint>(red_backtrack[Coord{x, y}]);
            cout << "\n";
        }
        cout << "red_backtrack_symmetric:\n";
        for (uint y=0; y<Y; ++y) {
            for (uint x=0; x<X; ++x)
                cout << " " << static_cast<uint>(red_backtrack_symmetric[Coord{x, y}]);
            cout << "\n";
        }
    }

    cout << setw(14) << "set " << setw(2) << nr_moves << endl;
    for (uint i=0; solution_moves>0; --nr_moves, --solution_moves, ++i) {
        auto& boards_blue = boardset_pairs.blue(solution_moves);
        auto& boards_red  = boardset_pairs.red (solution_moves);

        army_set.emplace_back(make_unique<ArmySet>());
        auto& moving_armies         = *army_set[i];
        auto const& opponent_armies = *army_set[i+1];
        auto& moved_armies          = *army_set[i+2];

        bool blue_to_move = solution_moves & 1;
        if (blue_to_move) {
            lock_guard<ArmySet> lock{moved_armies};
            stats_list.emplace_back
                (make_all_blue_moves_backtrack
                 (boards_red, boards_blue,
                  moving_armies, opponent_armies, moved_armies,
                  nr_moves));
        } else {
            boards_red.grow(boards_blue.back_id());
#if ARMYSET_SPARSE
            lock_guard<ArmySet> lock{moved_armies};
#endif // ARMYSET_SPARSE
            stats_list.emplace_back
                (make_all_red_moves_backtrack
                 (boards_blue, boards_red,
                  moving_armies, opponent_armies, moved_armies,
                  solution_moves, red_backtrack, red_backtrack_symmetric,
                  nr_moves));
        }

        if (is_terminated()) {
            auto stop_backtrack = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_backtrack-start_backtrack).count();
            return;
        }

        moved_armies.convert_hash();
        auto& stats = stats_list.back();
        if (blue_to_move) {
            if (boards_blue.size() == 0)
                throw_logic("No solution while backtracking");
            if (example) {
                stats.example_board
                    (example > 0 ?
                     boards_blue.example(opponent_armies, moved_armies, solution_moves & 1) :
                     boards_blue.random_example(opponent_armies, moved_armies, solution_moves & 1));
                cout << stats.example_board();
            }
        } else {
            if (boards_red.size() == 0)
                throw_logic("No solution while backtracking");
            if (example) {
                stats.example_board
                    (example > 0 ?
                     boards_red.example(opponent_armies, moved_armies, solution_moves & 1) :
                     boards_red.random_example(opponent_armies, moved_armies, solution_moves & 1));
                cout << stats.example_board();
            }
        }
        cout << stats << flush;
    }
    boardset_pairs.blue(1).sort_compress();

    auto stop_backtrack = chrono::steady_clock::now();
    duration = chrono::duration_cast<Sec>(stop_backtrack-start_backtrack).count();
    cout << setw(6) << duration << " s, backtrack tables built" << endl;

    // Do some sanity checking
    BoardSetBlue const& final_board_set = boardset_pairs.blue(1);
    ArmySet const& final_army_set = *army_set.back();

    ArmyId blue_id;
    if (nr_moves == solution_moves) {
        // There should be only 1 blue army completely on the red base
        if (final_army_set.size() != 1)
            throw_logic("More than 1 final blue army");
        blue_id = 1;
        // There should be only 1 final board
        if (final_board_set.back_id() != 1) {
            if (final_board_set.back_id() == 0)
                throw_logic("0 solution while backtracking");
            throw_logic("More than 1 solution while backtracking");
        }
    } else {
        Army const& blue_army = tables.start().red();
        blue_id = final_army_set.find(blue_army);
        if (blue_id == 0)
            throw_logic("Could not find final blue army");
    }

    BoardSubsetBlue const& final_subset = final_board_set.cat(blue_id);
    if (final_subset.size() != 1)
        throw_logic("More than 1 final red army");
    ArmyId red_value = 0;
    for (ArmyId value: final_subset)
        if (value != 0) {
            red_value = value;
            break;
        }
    if (red_value == 0)
        throw_logic("Could not find final red army");
    ArmyId red_id;
    bool skewed = BoardSubsetBlue::split(red_value, red_id) != 0;
    // Backtracking forced the final red army to be last_red_army
    // So there can only be 1 final red army and it therefore has army id 1
    if (red_id != 1)
        throw_logic("Unexpected red army id " +to_string(red_id));
    // And it was stored without flip
    if (skewed)
        throw_logic("Unexpected red army skewed");

    Army blue{final_army_set, blue_id};
    Army red{last_red_army};

    // It's probably more useful to generate a FullMove sequence
    // instead of a board sequence. Punt for now. --Note

    solution_moves = boardset_pairs.nr_moves();
    // Reserve solution_moves+1 boards
    boards.resize(solution_moves+1);
    boards[solution_moves] = Board{blue, red};

    if (false) {
        cout << "Initial\n";
        cout << "Blue: " << blue_id << ", Red: " << red_id << ", skewed=" << skewed << "\n";
        cout << boards[solution_moves];
    }

    army_set.pop_back();

    Army blueSymmetric{blue, SYMMETRIC};
    Army redSymmetric {red , SYMMETRIC};
    int blue_symmetry = cmp(blue, blueSymmetric);
    int red_symmetry  = cmp(red , redSymmetric);

    Image image{blue, red};
    ArmyPos armyE, armySymmetricE;
    for (uint i=1; i <= solution_moves; ++i) {
        if (is_terminated()) return;

        army_set.pop_back();
        ArmySet const& back_armies   = *army_set.back();

        auto& boards_blue = boardset_pairs.blue(i);
        auto& boards_red  = boardset_pairs.red (i);

        bool blue_to_move = i & 1;
        if (blue_to_move)
            boards_blue.clear(1);
        else
            boards_red.clear(1);

        Army const& army          = blue_to_move ? blue          : red;
        Army const& armySymmetric = blue_to_move ? blueSymmetric : redSymmetric;
        ArmyMapper const& mapper{armySymmetric};
        for (uint a=0; a<ARMY; ++a) {
            auto const soldier = army[a];
            armyE         .copy(army         , a);
            armySymmetricE.copy(armySymmetric, mapper.map(soldier));
            array<Coord, MAX_X*MAX_Y/4+1> reachable;

            // Jumps
            reachable[0] = soldier;
            int nr_reachable = 1;
            image.set(soldier, CLOSED_LOOP ? EMPTY : COLORS);
            for (int i=0; i < nr_reachable; ++i) {
                auto jumpees      = reachable[i].jumpees();
                auto jump_targets = reachable[i].jump_targets();
                for (uint r = 0; r < RULES; ++r, jumpees.next(), jump_targets.next()) {
                    auto const jumpee = jumpees.current();
                    auto const target = jump_targets.current();
                    if (!image.jumpable(jumpee, target)) continue;
                    image.set(target, COLORS);
                    reachable[nr_reachable++] = target;
                }
            }
            for (int i=1; i < nr_reachable; ++i)
                image.set(reachable[i], EMPTY);
            image.set(soldier, blue_to_move ? BLUE : RED);

            // Slides
            auto slide_targets = soldier.slide_targets();
            for (uint r = 0; r < RULES; ++r, slide_targets.next()) {
                auto const target = slide_targets.current();
                if (image.get(target) == EMPTY)
                    reachable[nr_reachable++] = target;
            }

            for (int i=1; i < nr_reachable; ++i) {
                auto const val = reachable[i];
                armyE         .store(val);
                armySymmetricE.store(val.symmetric());
                if (blue_to_move) {
                    blue_symmetry = cmp(armyE, armySymmetricE);
                    blue_id = back_armies.find(blue_symmetry >= 0 ? armyE : armySymmetricE);
                    if (blue_id == 0) continue;
                    int symmetry = blue_symmetry * red_symmetry;
                    auto board_id = boards_red._find(blue_id, red_id, symmetry);
                    if (board_id == 0) continue;
                    image.set(soldier, EMPTY);
                    image.set(val,     BLUE);
                    blue          = armyE;
                    blueSymmetric = armySymmetricE;
                } else {
                    red_symmetry = cmp(armyE, armySymmetricE);
                    red_id = back_armies.find(red_symmetry >= 0 ? armyE : armySymmetricE);
                    if (red_id == 0) continue;
                    int symmetry = red_symmetry * blue_symmetry;
                    auto board_id = boards_blue.find(blue_id, red_id, symmetry);
                    if (board_id == 0) continue;
                    image.set(soldier, EMPTY);
                    image.set(val,     RED);
                    red          = armyE;
                    redSymmetric = armySymmetricE;
                }
                goto MOVE_DONE;
            }
        }
        cerr << "Blue:\n" << blue;
        cerr << "Red:\n" << red;
        cerr << "Failure board:\n" << image;
        throw_logic("Could not identify backtracking move");
      MOVE_DONE:
        boards[solution_moves - i] = Board{blue, red};
    }
    // cout << "Final image:\n" << image;
}

void my_main(int argc, char const* const* argv) COLD;
void my_main(int UNUSED argc, char const* const* argv) {
    GetOpt options("b:B:t:IsHSjpqQ:eEFf:vV:R:Ax:y:r:a:T", argv);
    long long int val;
    bool replay = false;
    bool show_tables = false;
    bool batch = true;
    while (options.next())
        switch (options.option()) {
            case 'A': attempt     = false; break;
            case 'B': balance_delay = atoi(options.arg()); break;
            case 'E': example     = -1; break;
            case 'f': red_file = options.arg(); break;
            case 'F': FATAL       = true; break;
            case 'H': hash_statistics  = true; break;
            case 'Q': testQ       = atoi(options.arg()); break;
            case 'R': sample_subset_red = options.arg(); break;
            case 'S': statistics  = true; break;
            case 'T': show_tables = true; break;
            case 'b': balance     = atoi(options.arg()); break;
            case 'e': example     =  1; break;
            case 'j': prune_jump  = true; break;
            case 'p': replay      = true; break;
            case 'q': testq       = true; break;
            case 's': prune_slide = true; break;
            case 'v': verbose     = true; break;
            case 'V': verbose_move = atoi(options.arg()); break;
            case 't':
              val = atoll(options.arg());
              if (val < 0)
                  throw(range_error("Number of threads cannot be negative"));
              if (val > THREADS_MAX)
                  throw(range_error("Too many threads"));
              nr_threads = val;
              break;
            case 'r':
              val = atoll(options.arg());
              if (val == 4 || val == 6 || val == 8)
                  RULES = val;
              else
                  throw(range_error("Invalid ruleset" + to_string(val)));
              break;
            case 'x':
              val = atoll(options.arg());
              if (val < 1)
                  throw(range_error("X must be positive"));
              if (val > MAX_X)
                  throw(range_error("X must be <= " + to_string(MAX_X)));
              X = val;
              break;
            case 'y':
              val = atoll(options.arg());
              if (val < 1)
                  throw(range_error("Y must be positive"));
              if (val > MAX_X)
                  throw(range_error("Y must be <= " + to_string(MAX_Y)));
              Y = val;
              break;
            case 'a':
              val = atoll(options.arg());
              if (val < 1)
                  throw(range_error("ARMY must be positive"));
              if (val > MAX_ARMY-DO_ALIGN)
                  throw(range_error("ARMY must be <= " + to_string(MAX_ARMY-DO_ALIGN)));
              ARMY = val;
              break;
            case 'I':
              batch = false;
              break;
            default:
              cerr << "usage: " << argv[0] << " [-A] [-x size] [-y size] [-r ruleset] [-a soldiers] [-v] [-V verbose_move] [-I] [-t threads] [-b balance] [-B balance_delay] [-s] [-j] [-p] [-T] [-F] [-f path_prefix] [-e] [-H] [-S] [-R sample_red_file]\n";
              exit(EXIT_FAILURE);
        }
    if (batch) sched_batch();

    if (X == 0)
        if (Y == 0) X = Y = 9;
        else X = Y;
    else
        if (Y == 0) Y = X;

    // +DO_ALIGN to allow for a terminating Coord
    ARMY_ALIGNED = ((ARMY+DO_ALIGN)+ALIGNSIZE-1) / ALIGNSIZE;
    ARMY_MASK    = Coord::ArmyMask();
    ARMY_MASK_NOT= ~ARMY_MASK;
    ARMY_PADDING = DO_ALIGN ? ALIGNSIZE - ARMY % ALIGNSIZE : 0;
    ARMY64_DOWN  = ARMY / sizeof(uint64_t);
    NIBBLE_LEFT  = AlignFill(0xf0);
    NIBBLE_RIGHT = AlignFill(0x0f);
    ArmySetSparse::set_ARMY_size();

    balance_min = ARMY     / 4 - balance;
    balance_max = (ARMY+3) / 4 + balance;
    if (nr_threads == 0) nr_threads = thread::hardware_concurrency();

    tables.init();

    cout << "Time " << time_string() << "\n";
    cout << "Pid: " << PID << "\n";
    cout << "Commit: " << VCS_COMMIT << "\n";
    auto start_board = tables.start();
    if (show_tables) {
        // g++ sizeof mutex=40, alignof mutex = 8
        //cout << "Sizeof(mutex)   =" << setw(3) << sizeof(mutex) <<
        //    " algnment " << setw(2) << alignof(mutex) << "\n";
        cout << "Sizeof(Coord)   =" << setw(3) << sizeof(Coord) <<
            " algnment " << setw(2) << alignof(Coord) << "\n";
        cout << "Sizeof(Align)   =" << setw(3) << sizeof(Align) <<
            " algnment " << setw(2) << alignof(Align) << "\n";
        cout << "Sizeof(Army)    =" << setw(3) << sizeof(Army) <<
            " algnment " << setw(2) << alignof(Army) << "\n";
        cout << "Sizeof(ArmyPos) =" << setw(3) << sizeof(ArmyPos) <<
            " algnment " << setw(2) << alignof(ArmyPos) << "\n";
        cout << "Sizeof(Board)   =" << setw(3) << sizeof(Board) <<
            " algnment " << setw(2) << alignof(Board) << "\n";
        cout << "Sizeof(Image)   =" << setw(3) << sizeof(Image) <<
            " algnment " << setw(2) << alignof(Image) << "\n";
        cout << "DO_ALIGN:     " << (DO_ALIGN ? "true" : "false") << "\n";
        cout << "SINGLE_ALIGN: " << (SINGLE_ALIGN ? "true" : "false") << "\n";
        cout << "ALIGNSIZE:    " << ALIGNSIZE << "\n";
        cout << "ARMY_MASK:    "; Coord::print(ARMY_MASK); cout << "\n";
        cout << "ARMY_PADDING: " << ARMY_PADDING << "\n";
        cout << "MAX_ARMY_ALIGNED: " << MAX_ARMY_ALIGNED << "\n";
        cout << "ARMY_ALIGNED: " << ARMY_ALIGNED << "\n";
        cout << "MAX_ARMY:     " << MAX_ARMY << "\n";
        cout << "Infinity: " << static_cast<uint>(tables.infinity()) << "\n";
        cout << "Red:\n";
        cout << start_board.red();
        cout << "Blue:\n";
        cout << start_board.blue();
        cout << "Base red Ndistance:\n";
        tables.print_Ndistance_base_red();
        cout << "Base red:\n";
        tables.print_base_red();
        cout << "Edge red:\n";
        tables.print_edge_red();
        cout << "Deep red:\n";
        tables.print_deep_red();
        cout << "Shallow red:\n";
        tables.print_shallow_red();
        cout << "Deepness:\n";
        tables.print_deepness();
        cout << "Parity:\n";
        tables.print_parity();
        cout << "Blue Base parity count:\n";
        tables.print_blue_parity_count();
        cout << "Red Base parity count:\n";
        tables.print_red_parity_count();
        cout << "Number of red slides blue jumps to red base:\n";
        tables.print_nr_slide_jumps_red();
    }

    int needed_moves = tables.min_nr_moves();
    cout << "Minimum possible number of moves: " << needed_moves << "\n";
    if (show_tables) return;
    int nr_moves = needed_moves;
    auto arg = options.next_arg();
    if (arg) {
        nr_moves = atoi(arg);
        if (nr_moves < needed_moves) {
            if (nr_moves < 0)
                throw(range_error("Number of moves must be positive"));
            cout << start_board << "No solution in " << nr_moves << " moves\n";
            return;
        }
    }

    set_signals();
    // get_memory(true);

    if (replay) {
        play();
        return;
    }

    StatisticsList stats_list_solve, stats_list_backtrack;
    Sec::rep solve_duration, backtrack_duration;
    BoardList boards;

    if (is_terminated()) return;

    auto start_time = now();
    Army red_army;
    int solution_moves =
        solve(start_board, nr_moves, red_army, stats_list_solve, solve_duration);
    if (solution_moves >= 0)
        backtrack(start_board, nr_moves, solution_moves, red_army,
                  stats_list_backtrack, backtrack_duration, boards);

    auto stop_time = now();

    for (size_t i = 0; i < boards.size(); ++i)
        cout << "Move " << i << "\n" << boards[i];

    Svg svg;
    svg.write(start_time, stop_time, solution_moves, boards,
              stats_list_solve, solve_duration,
              stats_list_backtrack, backtrack_duration);
}

int main(int argc, char const* const* argv) COLD;
int main(int argc, char const* const* argv) {
    try {
        init_system();

        my_main(argc, argv);
        cout << "Final memory " << total_allocated() << "+" << total_mmapped() << "(" << total_mmaps() << "), mlocks " << total_mlocked() << "(" << total_mlocks() << ")\n";
        if (is_terminated())
            cout << "Terminated by signal" << endl;
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        cerr << "Final memory " << total_allocated() << "+" << total_mmapped() << "(" << total_mmaps() << ") " << total_mlocked() << "(" << total_mlocks() << ")\n";
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
