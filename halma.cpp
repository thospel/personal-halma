#define SLOW 0
#define ONCE 1
#include "halma.hpp"

#include <fstream>

#include <random>

uint X = 0;
uint Y = 0;
uint RULES = 6;
uint ARMY = 10;

Align ARMY_MASK;
Align ARMY_MASK_NOT;
Align NIBBLE_LEFT;
Align NIBBLE_RIGHT;
uint ARMY_ALIGNED;
uint ARMY_PADDING;
uint ARMY64_DOWN;

int balance = -1;
int balance_delay = 0;
int balance_min, balance_max;

int example = 0;
bool prune_slide = false;
bool prune_jump  = false;

bool statistics = false;
bool hash_statistics = false;
bool verbose = false;
bool attempt = true;
char const* sample_subset_red = nullptr;

// 0 means let C++ decide
uint nr_threads = 0;

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
    for (auto const& pos: army)
        os << pos << "\n";
    return os;
}

void Army::check(const char* file, int line) const {
    // for (int i=0; i<ARMY; ++i) (*this)[i].check(file, line);
    for (uint i=0; i<ARMY-1; ++i)
        if ((*this)[i] >= (*this)[i+1]) {
            cerr << *this;
            throw_logic("Army out of order", file, line);
        }
    if (DO_ALIGN)
        for (uint i=ARMY; i < ARMY+ARMY_PADDING; ++i)
            if ((*this)[i] != Coord::MAX())
                throw_logic("Badly terminated army", file, line);
}

void Army::sort(Coord* RESTRICT base) {
    std::sort(base, base+ARMY);
}

void ArmyPos::check(char const* file, int line) const {
    for (uint i=0; i<ARMY; ++i) at(i).check(file, line);
    for (uint i=0; i<ARMY-1; ++i)
        if (at(i) >= at(i+1)) {
            cerr << *this;
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

void StatisticsE::print(ostream& os) const {
    if (statistics) {
        os << "\tArmy inserts:  ";
        if (armyset_tries())
            os << setw(3) << armyset_size()*100 / armyset_tries() << "%";
        else
            os << "----";
        os << "\t" << armyset_size() << " / " << armyset_tries() << " " << "\n";

        os << "\tBoard inserts: ";
        if (boardset_tries())
            os << setw(3) << boardset_size()*100 / boardset_tries() << "%";
        else
            os << "----";
        os << "\t" << boardset_size() << " / " << boardset_tries() << " " << "\n";

        os << "\tBlue on red base edge: ";
        if (boardset_size())
            os << setw(3) << edges()*100 / boardset_size() << "%";
        else
            os << "----";
        os << "\t" << edges() << " / " << boardset_size() << " " << "\n";

        os << "\tMemory: " << allocated()/ 1000000  << " plain + " << mmapped()/1000000 << " mmapped (" << mmaps() << " mmaps) = " << (allocated() + mmapped()) / 1000000 << " MB\n";
    }
    if (hash_statistics) {
        os << "\tArmy immediate:";
        if (armyset_tries())
            os << setw(3) << armyset_immediate() * 100 / armyset_tries() << "%";
        else
            os << "----";
        os << "\t" << armyset_immediate() << " / " << armyset_tries() << " " << "\n";

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
        os << "\t" << boardset_immediate() << " / " << boardset_tries() << " " << "\n";

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
    while (i < ARMY || j < ARMY) {
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
    while (i < ARMY || j < ARMY) {
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
    for (auto const& pos: blue())
        pos.svg(os, BLUE, scale);
    for (auto const& pos: red())
        pos.svg(os, RED,  scale);
}

bool BoardSubSet::find(ArmyId red_id) const {
    auto mask = mask_;
    ArmyId pos = hash64(red_id) & mask;
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

void BoardSubSet::resize() {
    auto old_size = allocated();
    ArmyId new_size = old_size*2;
    auto new_armies = callocate<ArmyId>(new_size);
    auto old_armies = armies_;
    armies_ = new_armies;
    // logger << "Resize BoardSubSet " << static_cast<void const *>(old_armies) << " -> " << static_cast<void const *>(new_armies) << ": " << new_size << "\n" << flush;
    ArmyId mask = new_size-1;
    mask_ = mask;
    left_ += FACTOR(new_size) - FACTOR(old_size);
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
    unallocate(old_armies, old_size);
}

ArmyId BoardSubSet::example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubSet");

    auto armies = armies_;
    for (auto end = &armies[allocated()]; armies < end; ++armies)
        if (*armies) {
            ArmyId red_id;
            symmetry = split(*armies, red_id);
            return red_id;
        }
    throw_logic("No red value even though the BoardSubSet is not empty");
}

ArmyId BoardSubSet::random_example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubSet");

    auto armies = armies_;
    ArmyId n = allocated();
    ArmyId i = random(n);
    ArmyId mask = n-1;
    ArmyId offset = 0;

    while (armies[i] == 0) i = (i + ++offset) & mask;
    ArmyId red_id;
    symmetry = split(armies[i], red_id);
    return red_id;
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

ArmyId BoardSubSetRed::example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubSetRed");

    ArmyId red_id;
    symmetry = split(armies_[0], red_id);
    return red_id;
}

ArmyId BoardSubSetRed::random_example(ArmyId& symmetry) const {
    if (empty()) throw_logic("No red value in BoardSubSetRed");

    ArmyId red_id;
    symmetry = split(armies_[random(size())], red_id);
    return red_id;
}

bool BoardSubSetRed::_find(ArmyId red_value) const {
    if (!armies_) return false;
    return any_of(armies_, armies_+size(),
               [red_value](ArmyId value) { return value == red_value; });
}

BoardSubSetRedBuilder::BoardSubSetRedBuilder(ArmyId size): army_list_{nullptr} {
    real_allocated_ = size;
    mask_ = size-1;
    left_ = capacity();
    callocate(armies_, size);
    allocate(army_list_, left_);
    // logger << "Create BoardSubSetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << size << "), list " << static_cast<void const *>(army_list_) << " (size " << left_ << ")\n" << flush;
}

void BoardSubSetRedBuilder::resize() {
    auto old_allocated = allocated();
    ArmyId new_allocated = old_allocated*2;
    ArmyId mask = new_allocated-1;
    ArmyId *armies, *army_list;
    ArmyId nr_elems = size();
    if (new_allocated > real_allocated_) {
        creallocate(armies_, real_allocated_, new_allocated);
        // logger << "Resize BoardSubSetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << real_allocated_ << ") -> " << static_cast<void const *>(armies) << " (size " << new_allocated << ")\n" << flush;
        armies = armies_;

        ArmyId new_left = FACTOR(new_allocated);
        army_list_ -= nr_elems;
        army_list = reallocate(army_list_, FACTOR(real_allocated_), new_left, nr_elems);
        // logger << "Resize BoardSubSetRedBuilder list " << static_cast<void const *>(army_list_) << " (size " << FACTOR(real_allocated_) << ") -> " << static_cast<void const *>(army_list) << " (size " << new_left << ")\n" << flush;
        army_list_ = army_list + nr_elems;
        real_allocated_ = new_allocated;
        mask_ = mask;
    } else {
        armies = armies_;
        army_list = army_list_ - nr_elems;
        mask_ = mask;
        fill(begin(), end(), 0);
    }
    left_ += FACTOR(new_allocated) - FACTOR(old_allocated);

    for (ArmyId i = 0; i < nr_elems; ++i) {
        auto value = army_list[i];
        // cout << "Insert " << value << "\n";
        ArmyId pos = hash64(value) & mask;
        uint offset = 0;
        while (armies[pos]) {
            // cout << "Try " << pos << " of " << mask+1 << "\n";
            ++offset;
            pos = (pos + offset) & mask;
        }
        armies[pos] = value;
        // cout << "Found empty\n";
    }
}

BoardSetBase::BoardSetBase(bool keep, ArmyId size): size_{0}, solution_id_{keep}, capacity_{size}, from_{1}, top_{1}, keep_{keep} {
    allocate(subsets_, capacity());
    --subsets_;
    // cout << "Create BoardSet " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
}

void BoardSetBase::clear(ArmyId size) {
    from_ = top_ = 1;
    size_ = 0;
    solution_id_ = keep_;
    ++subsets_;
    reallocate(subsets_, capacity(), size);
    --subsets_;
    capacity_ = size;
}

void BoardSetBase::resize() {
    auto old_subsets = subsets_;
    subsets_ = allocate<BoardSubSet>(capacity()*2) - 1;
    // logger << "Resize BoardSet " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity() << "\n" << flush;
    std::copy(&old_subsets[from()], &old_subsets[top_], &subsets_[1]);
    if (!keep_) {
        top_ -= from_ - 1;
        from_ = 1;
    }
    ++old_subsets;
    deallocate(old_subsets, capacity());
    capacity_ *= 2;
}

void BoardSet::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    BoardSetBase::clear(size);
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

void BoardSetRed::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    BoardSetBase::clear(size);
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

void ArmySet::print(ostream& os) const {
    os << "[";
    if (values_) {
        for (size_t i=0; i < allocated(); ++i)
            os << " " << values_[i];
        } else os << " deleted";
    os << " ] (" << static_cast<void const *>(this) << ")\n";
    for (size_t i=1; i <= used_; ++i) {
        os << "Army " << i << "\n" << Image{cat(i)};
    }
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

void FullMove::move_expand(Board const& board_from, Board const& board_to, Move const& move) {
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
    Image image{board_from};
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

FullMove::FullMove(Board const& board_from, Board const& board_to, Color color) : FullMove{} {
    bool blue_diff = board_from.blue() !=  board_to.blue();
    bool red_diff  = board_from.red()  !=  board_to.red();
    if (blue_diff && red_diff) throw_logic("Both players move");

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
    sort(&directions[0], &directions[RULES]);
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
            array<Coord, 8> slide_targets, jumpees, jump_targets;
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
    base_blue_.fill(0);
    base_red_ .fill(0);
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
    sort(blue.begin(), blue.end());
    sort(red.begin(),  red.end());
    for (auto const& pos: blue)
        if (base_red_[pos]) throw_logic("Red and blue overlap");

    for (uint y=0; y < Y; ++y) {
        Norm d = infinity_;
#if !__BMI2__
        Parity y_parity = y%2;
#endif // !__BMI2__
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
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
    for (auto const& r: red)
        ++parity_count_[parity(r)];

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

void Tables::print_parity(ostream& os) const {
    for (uint y=0; y < Y; ++y) {
        for (uint x=0; x < X; ++x) {
            auto pos = Coord{x, y};
            os << " " << static_cast<uint>(parity(pos));
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

void ArmySet::_init(size_t size) {
    if (size < MIN_SIZE())
        throw_logic("ArmySet clear size too small");

    mask_ = size-1;
    used_ = 0;

    armies_size_ = size * ARMY;
    while (armies_size_ < ARMY+ARMY_PADDING) armies_size_ *= 2;
    size_t alimit = (armies_size_-ARMY_PADDING)/ARMY - 1;
    if (alimit >= ARMYID_HIGHBIT)
        throw(overflow_error("ArmySet size too large"));
    limit_ = min(FACTOR(size), static_cast<ArmyId>(alimit));

    allocate(armies_, armies_size_);
    callocate(values_, size);
    // logger << "New value  " << static_cast<void const *>(values_) << "\n";
    // logger << "New armies " << static_cast<void const *>(armies_) << "\n";
}

ArmyId ArmySet::find(Army const& army) const {
    ArmyId const mask = mask_;
    ArmyId pos = army.hash() & mask;
    auto values = values_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values[pos];
        if (i == 0) return 0;
        if (army == cat(i)) return i;
        ++offset;
        pos = (pos + offset) & mask;
    }
}

ArmyId ArmySet::find(ArmyPos const& army) const {
    ArmyId const mask = mask_;
    ArmyId pos = army.hash() & mask;
    auto values = values_;
    uint offset = 0;
    while (true) {
        // cout << "Try " << pos << " of " << size_ << "\n";
        ArmyId i = values[pos];
        if (i == 0) return 0;
        if (army == cat(i)) return i;
        ++offset;
        pos = (pos + offset) & mask;
    }
}

void ArmySet::resize() {
    ArmyId values_limit = FACTOR(allocated());
    ArmyId armies_limit = (armies_size_-ARMY_PADDING)/ARMY - 1;

    if (used_ >= armies_limit) {
        size_t new_size = armies_size_ * 2;
        // logger << "Resize ArmySet armies: new size=" << new_size/ARMY << endl;
        size_t alimit = (new_size-ARMY_PADDING)/ARMY - 1;
        // Only applies to the red armyset really. But the blue armyset is
        // always so much smaller than red that it doesn't matter
        if (alimit >= ARMYID_HIGHBIT)
            throw(overflow_error("ArmyId grew too large"));
        reallocate(armies_, armies_size_, new_size);
        armies_size_ = new_size;
        armies_limit = alimit;
    }

    if (used_ >= values_limit) {
        size_t old_size = allocated();
        size_t new_size = old_size * 2;
        // logger << "Resize ArmySet values: new size=" << new_size << "\n" << flush;
        if (new_size-1 > ARMYID_MAX)
            throw(overflow_error("Army hash grew too large"));

        creallocate(values_, old_size, new_size);
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

bool BoardSet::insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red) {
    Statistics dummy_stats;

    Army const& blue = board.blue();
    Army const blue_symmetric{blue, SYMMETRIC};
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.insert(blue_symmetry >= 0 ? blue : blue_symmetric, dummy_stats);

    Army const& red = board.red();
    Army const red_symmetric{red, SYMMETRIC};
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.insert(red_symmetry >= 0 ? red : red_symmetric, dummy_stats);

    int symmetry = blue_symmetry * red_symmetry;
    return insert(blue_id, red_id, symmetry, dummy_stats);
}

bool BoardSet::find(Board const& board, ArmySet const& armies_blue, ArmySet const& armies_red) const {
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

Board BoardSet::example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const {
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
    throw_logic("No board even though BoardSet is not empty");
}

Board BoardSet::random_example(ArmySet const& opponent_armies, ArmySet const& moved_armies, bool blue_moved) const {
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

bool BoardSubSetRed::_insert(ArmyId red_value, Statistics& stats) {
    // cout << "Insert " << red_value << "\n";
    if (armies_) throw_logic("Multiple single insert in BoardSubSetRed");
    ArmyId* new_list = allocate<ArmyId>(1);
    new_list[0] = red_value;
    *this = BoardSubSetRed{new_list, 1};
    return true;
}

bool BoardSetRed::_insert(ArmyId blue_id, ArmyId red_id, int symmetry, Statistics& stats) {
    if (CHECK) {
        if (blue_id <= 0)
            throw_logic("red_id <= 0");
    }
    lock_guard<mutex> lock{exclude_};

    if (blue_id >= top_) {
        // Only in the multithreaded case blue_id can be different from top_
        // if (blue_id != top_) throw_logic("Cannot grow more than 1");
        while (blue_id > capacity_) resize();
        while (blue_id >= top_) at(top_++).zero();
    }
    bool result = at(blue_id)._insert(red_id, symmetry, stats);
    size_ += result;
    return result;
}

bool BoardSetRed::insert(Board const& board, ArmySet& armies_blue, ArmySet& armies_red) {
    Statistics dummy_stats;

    Army const& blue = board.blue();
    Army const blue_symmetric{blue, SYMMETRIC};
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.insert(blue_symmetry >= 0 ? blue : blue_symmetric, dummy_stats);

    Army const& red = board.red();
    Army const red_symmetric{red, SYMMETRIC};
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.insert(red_symmetry >= 0 ? red : red_symmetric, dummy_stats);

    int symmetry = blue_symmetry * red_symmetry;
    return _insert(blue_id, red_id, symmetry, dummy_stats);
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
        red_id = subset_red.example(symmetry);
        ArmySet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmySet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        Army const blue{blue_armies, blue_id};
        Army const red {red_armies,  red_id, symmetry};
        return Board{blue, red};
    }
    throw_logic("No board even though BoardSet is not empty");
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
        red_id = subset_red.random_example(symmetry);
        ArmySet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmySet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        Army const blue{blue_armies, blue_id};
        Army const red {red_armies,  red_id, symmetry};
        return Board{blue, red};
    }
}

int Board::min_nr_moves(bool blue_to_move) const {
    blue_to_move = blue_to_move ? true : false;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = tables.Ninfinity();
    int off_base_from = 0;
    ParityCount parity_count_from = tables.parity_count();
    int edge_count_from = 0;

    for (auto const& b: blue()) {
        --parity_count_from[b.parity()];
        if (b.base_red()) continue;
        ++off_base_from;
        edge_count_from += b.edge_red();
        Ndistance_red |= b.Ndistance_base_red();
        for (auto const& r: red())
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
        "        <marker id='arrowhead' markerWidth='" << w << "' markerHeight='" << 2*h << "' \n"
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
        "      <tr class='threads'><th>Threads</th><td>" << nr_threads << "</td></tr>\n"
        "      <tr class='start_time'><th>Start</th><td>" << time_string(start_time) << "</td></tr>\n"
        "      <tr class='stop_time'><th>Stop</th><td>"  << time_string(stop_time) << "</td></tr>\n"
        "      <tr class='commit_id'><th>Commit id</th><td>" << VCS_COMMIT << "</td></tr>\n"
        "      <tr class='commit_time'><th>Commit time</th><td>" << VCS_COMMIT_TIME << "</td></tr>\n"
        "    </table>\n";
}

void Svg::move(FullMove const& move) {
    out_ << "      <polyline points='";
    for (auto const& pos: move) {
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
        "        <th>Boards per<br/>blue army</th>\n";
    if (statistics) {
        out_ <<
            "        <th>Late<br/>prunes</th>\n"
            "        <th>Late<br/>prune<br/>ratio</th>\n"
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
    for (auto const& st: stats_list) {
        out_ <<
            "      <tr class='" << st.css_color() << "'>\n"
            "        <td class='available_moves'>" << st.available_moves() << "</td>\n"
            "        <td class='duration'>" << st.duration() << "</td>\n"
            "        <td class='boards'>" << st.boardset_size() << "</td>\n"
            "        <td class='armies'>" << st.armyset_size() << "</td>\n"
            "        <td class='memory'>" << st.memory()    / 1000000 << "</td>\n"
            "        <td class='allocated'>" << st.allocated() / 1000000 << "</td>\n"
            "        <td class='mmapped'>" << st.mmapped() / 1000000 << "</td>\n"
            "        <td class='mmaps'>" << st.mmaps() << "</td>\n"
            "        <td>" << st.boardset_size()/(st.blue_armies_size() ? st.blue_armies_size() : 1) << "</td>\n";
        if (statistics) {
            out_ <<
                "        <td>" << st.late_prunes() << "</td>\n"
                "        <td>" << st.late_prunes() * 100 / old_boards << "%</td>\n"
                "        <td>" << st.armyset_size() << " / " << st.armyset_tries() << "</td>\n"
                "        <td>";
            if (st.armyset_tries())
                out_ << st.armyset_size()*100 / st.armyset_tries() << "%";
            out_ <<
                "</td>\n"
                "        <td>" << st.boardset_size() << " / " << st.boardset_tries() << "</td>\n"
                "        <td>";
            if (st.boardset_tries())
                out_ << st.boardset_size()*100 / st.boardset_tries() << "%";
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
        throw_logic("No hardcoded replay game");
    }

    int nr_moves = game.size();
    for (auto& move: game) {
        cout << board;
        // cout << board.symmetric();

        ArmySet  army_set[3];
        BoardSet    blue_boards;
        BoardSetRed red_boards;
        bool blue_to_move = nr_moves & 1;
        if (blue_to_move) {
            red_boards.insert(board, army_set[0], army_set[1]);

            auto stats = make_all_blue_moves
                (red_boards, blue_boards,
                 army_set[0], army_set[1], army_set[2],
                 nr_moves);
            cout << stats << "===============================\n";
            --nr_moves;
            board.do_move(move);
            if (blue_boards.find(board, army_set[1], army_set[2], nr_moves)) {
                cout << "Good\n";
            } else {
                cout << "Bad\n";
            }
        } else {
            blue_boards.insert(board, army_set[0], army_set[1], nr_moves);

            auto stats = make_all_red_moves
                (blue_boards, red_boards,
                 army_set[0], army_set[1], army_set[2],
                 nr_moves);
            cout << stats << "===============================\n";
            --nr_moves;
            board.do_move(move);
            if (red_boards.find(board, army_set[1], army_set[2])) {
                cout << "Good\n";
            } else {
                cout << "Bad\n";
            }
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
            bool symmetry = BoardSubSet::split(red_value, red_id) != 0;

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
    BoardSet    boards_blue;
    BoardSetRed boards_red;
    array<ArmySet, 3>  army_set;
    bool blue_to_move = nr_moves & 1;
    if (blue_to_move)
        boards_red.insert(board, army_set[0], army_set[1]);
    else
        boards_blue.insert(board, army_set[0], army_set[1], nr_moves);

    ArmyId red_id = 0;
    int i;
    for (i=0; nr_moves>0; --nr_moves, ++i) {
        auto& moving_armies         = army_set[ i    % 3];
        auto const& opponent_armies = army_set[(i+1) % 3];
        auto& moved_armies          = army_set[(i+2) % 3];

        bool blue_to_move = nr_moves & 1;
        if (blue_to_move) {
            stats_list.emplace_back
                (make_all_blue_moves
                 (boards_red, boards_blue,
                  moving_armies, opponent_armies, moved_armies,
                  nr_moves));
            boards_red.clear();
        } else {
            stats_list.emplace_back
                (make_all_red_moves
                 (boards_blue, boards_red,
                  moving_armies, opponent_armies, moved_armies,
                  nr_moves));
            boards_blue.clear();
        }
        moved_armies.drop_hash();
        moving_armies.clear();

        if (is_terminated()) {
            auto stop_solve = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            return -1;
        }

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

void backtrack(Board const& board, int nr_moves, int solution_moves,
               Army const& last_red_army,
               StatisticsList& stats_list, Sec::rep& duration,
               BoardList& boards) {
    cout << "Start backtracking\n";

    auto start_backtrack = chrono::steady_clock::now();
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
    BoardTable<uint8_t> red_backtrack_symmetric{};
    red_backtrack_symmetric.fill(0);
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
    for (int i=0; solution_moves>0; --nr_moves, --solution_moves, ++i) {
        board_set.emplace_back(new BoardSet(true));
        auto& boards_from = *board_set[i];
        auto& boards_to   = *board_set[i+1];

        army_set.emplace_back(new ArmySet);
        auto const& moving_armies   = *army_set[i];
        auto const& opponent_armies = *army_set[i+1];
        auto& moved_armies          = *army_set[i+2];

        bool blue_to_move = nr_moves & 1;
        stats_list.emplace_back
            (blue_to_move ?
             make_all_blue_moves_backtrack
             (boards_from, boards_to,
              moving_armies, opponent_armies, moved_armies,
              nr_moves) :
             make_all_red_moves_backtrack
             (boards_from, boards_to,
              moving_armies, opponent_armies, moved_armies,
              solution_moves, red_backtrack, red_backtrack_symmetric,
              nr_moves));

        if (is_terminated()) {
            auto stop_backtrack = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_backtrack-start_backtrack).count();
            return;
        }

        if (boards_to.size() == 0)
            throw_logic("No solution while backtracking");
        auto& stats = stats_list.back();
        if (example) {
            stats.example_board
                (example > 0 ?
                 boards_to.example(opponent_armies, moved_armies, nr_moves & 1) :
                 boards_to.random_example(opponent_armies, moved_armies, nr_moves & 1));
            cout << stats.example_board();
        }
        cout << stats << flush;
    }
    auto stop_backtrack = chrono::steady_clock::now();
    duration = chrono::duration_cast<Sec>(stop_backtrack-start_backtrack).count();
    cout << setw(6) << duration << " s, backtrack tables built" << endl;

    // Do some sanity checking
    BoardSet const& final_board_set = *board_set.back();
    ArmySet const& final_army_set = *army_set.back();

    ArmyId blue_id;
    if (nr_moves == solution_moves) {
        // There should be only 1 blue army completely on the red base
        if (final_army_set.size() != 1)
            throw_logic("More than 1 final blue army");
        blue_id = final_board_set.back_id();
        if (blue_id != 1)
            throw_logic("Unexpected blue army id " + to_string(blue_id));
        // There should be only 1 final board
        if (final_board_set.size() != 1)
            throw_logic("More than 1 solution while backtracking");
    } else {
        Army const& blue_army = tables.start().red();
        blue_id = final_army_set.find(blue_army);
        if (blue_id == 0)
            throw_logic("Could not find final blue army");
    }

    BoardSubSet const& final_subset = final_board_set.cat(blue_id);
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
    bool skewed = BoardSubSet::split(red_value, red_id) != 0;
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

    // Reserve nr_moves+1 boards
    size_t board_pos = board_set.size();
    boards.resize(board_pos);
    boards[--board_pos] = Board{blue, red};

    if (false) {
        cout << "Initial\n";
        cout << "Blue: " << blue_id << ", Red: " << red_id << ", skewed=" << skewed << "\n";
        cout << boards[board_pos];
    }

    board_set.pop_back();
    army_set.pop_back();
    army_set.pop_back();

    Army blueSymmetric{blue, SYMMETRIC};
    Army redSymmetric {red , SYMMETRIC};
    int blue_symmetry = cmp(blue, blueSymmetric);
    int red_symmetry  = cmp(red , redSymmetric);

    Image image{blue, red};
    ArmyPos armyE, armySymmetricE;
    for (int blue_to_move = 1;
         board_set.size() != 0;
         blue_to_move = 1-blue_to_move, board_set.pop_back(), army_set.pop_back()) {
        if (is_terminated()) return;

        // cout << "Current image:\n" << image;
        BoardSet const& back_boards   = *board_set.back();
        ArmySet const& back_armies   = *army_set.back();

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
                    auto board_id = back_boards.find(blue_id, red_id, symmetry);
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
                    auto board_id = back_boards.find(blue_id, red_id, symmetry);
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
        boards[--board_pos] = Board{blue, red};
    }
    // cout << "Final image:\n" << image;
}

void my_main(int argc, char const* const* argv) {
    GetOpt options("b:B:t:sHSjpeEFvR:Ax:y:r:a:T", argv);
    long long int val;
    bool replay = false;
    bool show_tables = false;
    while (options.next())
        switch (options.option()) {
            case 'b': balance       = atoi(options.arg()); break;
            case 'B': balance_delay = atoi(options.arg()); break;
            case 'p': replay = true; break;
            case 's': prune_slide = true; break;
            case 'H': hash_statistics  = true; break;
            case 'S': statistics  = true; break;
            case 'v': verbose     = true; break;
            case 'j': prune_jump  = true; break;
            case 'A': attempt     = false; break;
            case 'e': example     =  1; break;
            case 'E': example     = -1; break;
            case 'F': FATAL       = true; break;
            case 'T': show_tables = true; break;
            case 'R': sample_subset_red = options.arg(); break;
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
            default:
              cerr << "usage: " << argv[0] << " [-A] [-x size] [-y size] [-r ruleset] [-a soldiers] [-t threads] [-b balance] [-B balance_delay] [-s] [-j] [-p] [-e] [-H] [-S] [-R sample_red_file]\n";
              exit(EXIT_FAILURE);
        }

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

    balance_min = ARMY     / 4 - balance;
    balance_max = (ARMY+3) / 4 + balance;
    if (nr_threads == 0) nr_threads = thread::hardware_concurrency();

    tables.init();

    cout << "Time " << time_string() << "\n";
    cout << "Pid: " << PID << "\n";
    cout << "Commit: " << VCS_COMMIT << "\n";
    auto start_board = tables.start();
    if (show_tables) {
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
        cout << "Parity:\n";
        tables.print_parity();
        cout << "Blue Base parity count:\n";
        tables.print_blue_parity_count();
        cout << "Red Base parity count:\n";
        tables.print_red_parity_count();
    }

    int needed_moves = tables.min_nr_moves();
    cout << "Minimum possible number of moves: " << needed_moves << "\n";
    if (show_tables) return;
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

int main(int argc, char const* const* argv) {
    try {
        init_system();

        my_main(argc, argv);
        cout << "Final memory " << total_allocated() << "+" << total_mmapped() << "(" << total_mmaps() << ")\n";
        if (is_terminated())
            cout << "Terminated by signal" << endl;
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        cerr << "Final memory " << total_allocated() << "+" << total_mmapped() << "(" << total_mmaps() << ")\n";
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
