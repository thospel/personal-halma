#define SLOW 0
#include "halma.hpp"

#include <csignal>

#include <sys/types.h>
#include <unistd.h>

#include <random>

int balance = -1;
int balance_delay = 0;
int balance_min, balance_max;

bool prune_slide = false;
bool prune_jump  = false;
int example = 0;

bool statistics = false;
bool hash_statistics = false;
bool verbose = false;
bool attempt = true;
char const* sample_subset_red = nullptr;

string HOSTNAME;
// 0 means let C++ decide
uint nr_threads = 0;
string VCS_COMMIT{STRINGIFY(COMMIT)};
string VCS_COMMIT_TIME{STRINGIFY(COMMIT_TIME)};

int signal_counter = 1;
atomic<uint> signal_generation;
thread_local uint signal_generation_seen;
thread_local ssize_t allocated_;
atomic<ssize_t> total_allocated_;

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

inline bool heuristics() {
    return balance >= 0 || prune_slide || prune_jump;
}

inline bool is_terminated() {
    return UNLIKELY(signal_counter % 2 == 0);
}

inline string _time_string(time_t time) {
    struct tm tm;

    if (!localtime_r(&time, &tm))
        throw(system_error(errno, system_category(),
                           "Could not convert time to localtime"));
    char buffer[80];
    if (!strftime(buffer, sizeof(buffer), "%F %T %z", &tm))
        throw(logic_error("strtime buffer too short"));
    return string{buffer};
}

string time_string(time_t time) {
    return _time_string(time);
}

inline time_t now() {
    time_t tm = time(nullptr);
    if (tm == static_cast<time_t>(-1))
        throw(system_error(errno, system_category(), "Could not get time"));
    return tm;
}

string time_string() {
    return _time_string(now());
}

ssize_t total_allocated() {
    if (tid) throw(logic_error("Use of total_allocated inside a thread"));
    total_allocated_ += allocated_;
    allocated_ = 0;
    return total_allocated_;
}

size_t PAGE_SIZE;
// Linux specific
size_t get_memory(bool set_base_mem) {
    static size_t base_mem = 0;

    size_t mem = 0;
    std::ifstream statm;
    statm.open("/proc/self/statm");
    statm >> mem;
    mem *= PAGE_SIZE;
    if (set_base_mem) {
        base_mem = mem;
        // cout << "Base mem=" << mem / 1000000 << " MB\n";
    } else mem -= base_mem;
    return mem;
}

thread_local uint tid;
inline uint thread_id();
uint thread_id() {
    return tid;
}

inline string thread_name();
string thread_name() {
    return to_string(thread_id());
}

LogBuffer::LogBuffer(): prefix_{nr_threads == 1 ? "" : "Thread " + thread_name() + ": "} {
    buffer_.resize(BLOCK);
    setp(&buffer_[0], &buffer_[BLOCK]);
}

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

static mutex mutex_out_;

int LogBuffer::sync() {
    int size = pbase() - pptr();
    if (size) {
        {
            std::lock_guard<std::mutex> lock{mutex_out_};
            cout << prefix_;
            cout.write(pbase(), pptr() - pbase());
            cout.flush();
        }
        pbump(size);
    }
    return 0;
}

thread_local LogStream logger;

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

void CoordZ::svg(ostream& os, Color color, uint scale) const {
    os << "      <circle cx='" << (x()+1) * scale << "' cy='" << (y()+1) * scale<< "' r='" << static_cast<uint>(scale * 0.35) << "' fill='" << svg_color(color) << "' />\n";
}

ostream& operator<<(ostream& os, ArmyZ const& army) {
    for (auto const& pos: army)
        os << pos << "\n";
    return os;
}

void ArmyZ::check(int line) const {
    // for (int i=0; i<ARMY; ++i) (*this)[i].check(line);
    for (int i=0; i<ARMY-1; ++i)
        if ((*this)[i] >= (*this)[i+1]) {
            cerr << *this;
            throw(logic_error("ArmyZ out of order at line " + to_string(line)));
        }
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
    if (at(-1) != CoordZ::MIN())
        throw(logic_error("ArmyZE wrong bottom at line " + to_string(line)));
    if (at(ARMY) != CoordZ::MAX())
        throw(logic_error("ArmyZE wrong top at line " + to_string(line)));
}

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

ArmyPair::ArmyPair(ArmyZ const& army) {
    for (int i=0; i < ARMY; ++i) {
        auto& p = army[i].coord_pair();
        normal_   [i] = p.normal();
        symmetric_[i] = p.symmetric();
    }
    sort(symmetric_.begin(), symmetric_.end());
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

    os << setw(6) << duration() << " s, set " << setw(2) << available_moves()-1 << " done," << setw(10) << boardset_size() << " boards /" << setw(9) << armyset_size() << " armies " << setw(7);
    os << boardset_size()/(blue_armies_size() ? blue_armies_size() : 1);
    os << " (" << setw(6) << memory() / 1000000 << " MB)\n";
}

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
    os << "'\n            stroke='black' />\n";
    for (auto const& pos: blue())
        pos.svg(os, BLUE, scale);
    for (auto const& pos: red())
        pos.svg(os, RED,  scale);
}

void BoardSubSet::create(ArmyId size) {
    mask_ = size-1;
    left_ = FACTOR(size);
    armies_ = new ArmyId[size];
    allocated_ += size * sizeof(ArmyId);
    // logger << "Create BoardSubSet " << static_cast<void const*>(armies_) << ": size " << size << ", " << left_ << " left\n" << flush;
    fill(begin(), end(), 0);
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
    auto old_armies = armies_;
    auto old_size = allocated();
    ArmyId size = old_size*2;
    auto new_armies = new ArmyId[size];
    allocated_ += size * sizeof(ArmyId);
    armies_ = new_armies;
    // logger << "Resize BoardSubSet " << static_cast<void const *>(old_armies) << " -> " << static_cast<void const *>(armies_) << ": " << size << "\n" << flush;

    ArmyId mask = size-1;
    mask_ = mask;
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
    allocated_ -= old_size * sizeof(ArmyId);
}

void BoardSubSet::convert_red() {
    ArmyId sz = size();
    auto new_armies = new ArmyId[sz];
    allocated_ += sz * sizeof(ArmyId);
    for (ArmyId const& red_value: *this)
        if (red_value) *new_armies++ = red_value;
    delete [] armies_;
    allocated_ -= allocated() * sizeof(ArmyId);
    // logger << "Convert BoardSubSet " << static_cast<void const*>(armies_) << " (size " << allocated() << ") to red -> " << static_cast<void const*>(new_armies-sz) << " (size " << sz << ")\n" << flush;
    auto subset_red = BoardSubSetRed{new_armies - sz, sz};
    static_cast<BoardSubSetBase&>(*this) = static_cast<BoardSubSetBase&>(subset_red);
}

ArmyId BoardSubSet::example(ArmyId& symmetry) const {
    if (empty()) throw(logic_error("No red value in BoardSubSet"));

    auto armies = armies_;
    for (auto end = &armies[allocated()]; armies < end; ++armies)
        if (*armies) {
            ArmyId red_id;
            symmetry = split(*armies, red_id);
            return red_id;
        }
    throw(logic_error("No red value even though the BoardSubSet is not empty"));
}

ArmyId BoardSubSet::random_example(ArmyId& symmetry) const {
    if (empty()) throw(logic_error("No red value in BoardSubSet"));

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
    if (empty()) throw(logic_error("No red value in BoardSubSetRed"));

    ArmyId red_id;
    symmetry = split(armies_[0], red_id);
    return red_id;
}

ArmyId BoardSubSetRed::random_example(ArmyId& symmetry) const {
    if (empty()) throw(logic_error("No red value in BoardSubSetRed"));

    ArmyId red_id;
    symmetry = split(armies_[random(size())], red_id);
    return red_id;
}

BoardSubSetRedBuilder::BoardSubSetRedBuilder(ArmyId allocate) {
    real_allocated_ = allocate;
    mask_ = allocate-1;
    left_ = capacity();
    armies_ = new ArmyId[allocate];
    allocated_ += allocate * sizeof(ArmyId);
    fill(begin(), end(), 0);
    army_list_ = new ArmyId[left_];
    allocated_ += left_ * sizeof(ArmyId);
    // logger << "Create BoardSubSetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << allocate << "), list " << static_cast<void const *>(army_list_) << " (size " << left_ << ")\n" << flush;
}

void BoardSubSetRedBuilder::resize() {
    auto old_allocated = allocated();
    ArmyId new_allocated = old_allocated*2;
    ArmyId *armies, *army_list;
    ArmyId nr_elems = size();
    if (new_allocated > real_allocated_) {
        armies = new ArmyId[new_allocated];
        allocated_ += new_allocated * sizeof(ArmyId);
        delete [] armies_;
        allocated_ -= real_allocated_ * sizeof(ArmyId);
        // logger << "Resize BoardSubSetRedBuilder hash " << static_cast<void const *>(armies_) << " (size " << real_allocated_ << ") -> " << static_cast<void const *>(armies) << " (size " << new_allocated << ")\n" << flush;
        armies_ = armies;

        ArmyId new_left = FACTOR(new_allocated);
        army_list = new ArmyId[new_left];
        allocated_ += new_left * sizeof(ArmyId);
        army_list_ -= nr_elems;
        std::copy(&army_list_[0], &army_list_[size()], army_list);
        delete [] army_list_;
        allocated_ -= FACTOR(real_allocated_) * sizeof(ArmyId);
        // logger << "Resize BoardSubSetRedBuilder list " << static_cast<void const *>(army_list_) << " (size " << FACTOR(real_allocated_) << ") -> " << static_cast<void const *>(army_list) << " (size " << new_left << ")\n" << flush;
        army_list_ = army_list + nr_elems;
        real_allocated_ = new_allocated;
    } else {
        armies = armies_;
        army_list = army_list_ - nr_elems;
    }
    ArmyId mask = new_allocated-1;
    mask_ = mask;
    left_ += FACTOR(new_allocated) - FACTOR(old_allocated);
    fill(begin(), end(), 0);

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

BoardSet::BoardSet(bool keep, ArmyId size): size_{0}, solution_id_{keep}, capacity_{size}, from_{1}, top_{1}, keep_{keep} {
    subsets_ = (new BoardSubSet[capacity_])-1;
    allocated_ += subsets_bytes();
    // cout << "Create BoardSet " << static_cast<void const*>(subsets_) << ": size " << capacity_ << "\n";
}

void BoardSet::clear(ArmyId size) {
    for (auto& subset: *this)
        subset.destroy();
    from_ = top_ = 1;
    size_ = 0;
    solution_id_ = keep_;
    auto old_subsets = subsets_+1;
    subsets_ = (new BoardSubSet[size])-1;
    delete [] old_subsets;
    allocated_ -= subsets_bytes();
    capacity_ = size;
    allocated_ += subsets_bytes();
}

void BoardSet::resize() {
    auto old_subsets = subsets_;
    subsets_ = (new BoardSubSet[capacity_*2])-1;
    // logger << "Resize BoardSet " << static_cast<void const *>(old_subsets) << " -> " << static_cast<void const *>(subsets_) << ": " << capacity_ << "\n" << flush;
    copy(&old_subsets[from()], &old_subsets[top_], &subsets_[1]);
    if (!keep_) {
        top_ -= from_ - 1;
        from_ = 1;
    }
    delete [] (old_subsets+1);
    allocated_ -= subsets_bytes();
    capacity_ *= 2;
    allocated_ += subsets_bytes();
}

void BoardSet::convert_red() {
    for (auto& subset: *this) subset.convert_red();
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

void ArmyZSet::print(ostream& os) const {
    os << "[";
    if (values_) {
        for (size_t i=0; i < allocated(); ++i)
            os << " " << values_[i];
        } else os << " deleted";
    os << " ] (" << static_cast<void const *>(this) << ")\n";
    for (size_t i=1; i <= used_; ++i) {
        os << "Army " << i << "\n" << Image{armies_[i]};
    }
}

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
    array<Coord, X*Y-2*ARMY+1> reachable;
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
    army_red_ = Army{red};

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

Tables const tables;

void ArmyZSet::_clear0() {
    delete [] armies_;
    allocated_ -= armies_bytes();
    // cout << "Destroy armies " << static_cast<void const *>(armies_) << "\n";
    // if (values_) cout << "Destroy values " << static_cast<void const *>(values_) << "\n";
    if (values_) {
        delete [] values_;
        allocated_ -= values_bytes();
    }
}

void ArmyZSet::_clear1(ArmyId size) {
    if (size > ARMY_HIGHBIT)
        throw(overflow_error("ArmyZSet size too large"));

    armies_size_ = size;
    mask_ = size-1;
    used_ = 0;
    limit_ = FACTOR(size);
    if (used_ >= limit_)
        throw(logic_error("ArmyZSet clear size too small"));

    armies_ = new ArmyZ [size];
    allocated_ += armies_bytes();
    values_ = new ArmyId[size];
    allocated_ += values_bytes();
    
    std::fill(&values_[0], &values_[size], 0);
    // logger << "New value  " << static_cast<void const *>(values_) << "\n";
    // logger << "New armies " << static_cast<void const *>(armies_) << "\n";
}

ArmyZSet::ArmyZSet(ArmyId size) : armies_{nullptr}, values_{nullptr} {
    _clear1(size);
}

ArmyZSet::~ArmyZSet() {
    _clear0();
}

void ArmyZSet::clear(ArmyId size) {
    // cout << "Clear\n";
    _clear0();
    _clear1(size);
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

void ArmyZSet::resize() {
    ArmyId values_limit = FACTOR(allocated());
    ArmyId armies_limit = armies_size_ - 1;

    if (used_ >= armies_limit) {
        size_t size = armies_size_ * 2;
        // logger << "Resize ArmyZSet armies: new size=" << size << endl;
        auto new_armies = new ArmyZ[size];
        // 0 is a dummy element we don't need to copy
        // But it's probably nicely aligned so the compiler can generate
        // some very fast copy code
        std::copy(&armies_[0], &armies_[armies_size_], &new_armies[0]);
        delete [] armies_;
        allocated_ -= armies_bytes();
        armies_ = new_armies;
        armies_size_ = size;
        armies_limit = size - 1;
        allocated_ += armies_bytes();
    }

    if (used_ >= values_limit) {
        size_t size = allocated() * 2;
        // logger << "Resize ArmyZSet values: new size=" << size << "\n" << flush;
        if (size > ARMY_HIGHBIT)
            throw(overflow_error("ArmyZ size grew too large"));

        delete [] values_;
        values_ = nullptr;
        allocated_ -= values_bytes();
        auto mask = size-1;
        mask_ = mask;
        auto values = new ArmyId[size];
        values_ = values;
        allocated_ += values_bytes();
        std::fill(&values[0], &values[size], 0);
        auto used = used_;
        auto armies = armies_;
        for (ArmyId i = 1; i <= used; ++i) {
            ArmyZ const& army = armies[i];
            ArmyId hash = army.hash();
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

bool BoardSet::insert(Board const& board, ArmyZSet& armies_blue, ArmyZSet& armies_red) {
    Statistics dummy_stats;

    ArmyZ const& blue = board.blue();
    auto blue_symmetric = blue.symmetric();
    int blue_symmetry = cmp(blue, blue_symmetric);
    auto blue_id = armies_blue.insert(blue_symmetry >= 0 ? blue : blue_symmetric, dummy_stats);

    ArmyZ const& red  = board.red();
    auto red_symmetric = red.symmetric();
    int red_symmetry = cmp(red, red_symmetric);
    auto red_id = armies_red.insert(red_symmetry >= 0 ? red : red_symmetric, dummy_stats);

    int symmetry = blue_symmetry * red_symmetry;
    return insert(blue_id, red_id, symmetry, dummy_stats);
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

Board BoardSet::example(ArmyZSet const& opponent_armies, ArmyZSet const& moved_armies, bool blue_moved) const {
    if (empty()) throw(logic_error("No board in BoardSet"));
    for (ArmyId blue_id = from(); blue_id <= back_id(); ++blue_id) {
        auto const& subset = at(blue_id);
        auto const& subset_red = subset.red();
        ArmyId red_id, symmetry;
        if (subset_red) {
            if (subset_red->empty()) continue;
            red_id = subset_red->example(symmetry);
        } else {
            if (subset.empty()) continue;
            red_id = subset.example(symmetry);
        }
        ArmyZSet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmyZSet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        ArmyZ const& blue = blue_armies.at(blue_id);
        ArmyZ const& red  = red_armies .at(red_id);
        return Board{blue, symmetry ? red.symmetric() : red};
    }
    throw(logic_error("No board even though BoardSet is not empty"));
}

Board BoardSet::random_example(ArmyZSet const& opponent_armies, ArmyZSet const& moved_armies, bool blue_moved) const {
    if (empty()) throw(logic_error("No board in BoardSet"));

    ArmyId n = subsets();
    ArmyId step = n == 1 ? 0 : random_coprime(n);
    for (ArmyId i = random(n); true; i = (i + step) % n) {
        ArmyId blue_id = from() + i;
        auto const& subset = at(blue_id);
        auto const& subset_red = subset.red();
        ArmyId red_id, symmetry;
        if (subset_red) {
            if (subset_red->empty()) continue;
            red_id = subset_red->random_example(symmetry);
        } else {
            if (subset.empty()) continue;
            red_id = subset.random_example(symmetry);
        }
        ArmyZSet const& blue_armies = blue_moved ? moved_armies : opponent_armies;
        ArmyZSet const& red_armies  = blue_moved ? opponent_armies : moved_armies;
        ArmyZ const& blue = blue_armies.at(blue_id);
        ArmyZ const& red  = red_armies .at(red_id);
        return Board{blue, symmetry ? red.symmetric() : red};
    }
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
            "        <td class='allocate'>" << st.allocated() / 1000000 << "</td>\n"
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
        unlink(svg_file_tmp.c_str());
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
        throw(logic_error("No hardcoded replay game"));
    }

    int nr_moves = game.size();
    for (auto& move: game) {
        cout << board;
        // cout << board.symmetric();

        BoardSet board_set[2];
        ArmyZSet  army_set[3];
        board_set[0].insert(board, army_set[0], army_set[1], nr_moves);

        auto stats = make_all_moves(board_set[0], board_set[1],
                                    army_set[0], army_set[1], army_set[2],
                                    nr_moves);
        cout << stats << "===============================\n";
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
        unlink(file_tmp.c_str());
        throw;
    }
}

int solve(Board const& board, int nr_moves, ArmyZ& red_army,
          StatisticsList& stats_list, Sec::rep& duration) {
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

    vector<ArmyId> largest_red;

    ArmyId red_id = 0;
    int i;
    for (i=0; nr_moves>0; --nr_moves, ++i) {
        auto& boards_from = board_set[ i    % 2];
        auto& boards_to   = board_set[(i+1) % 2];
        auto& moving_armies   = army_set[ i    % 3];
        auto const& opponent_armies = army_set[(i+1) % 3];
        auto& moved_armies    = army_set[(i+2) % 3];

        stats_list.emplace_back
            (make_all_moves(boards_from, boards_to,
                            moving_armies, opponent_armies, moved_armies,
                            nr_moves));
        moved_armies.drop_hash();
        moving_armies.clear();
        boards_from.clear();

        if (is_terminated()) {
            auto stop_solve = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            return -1;
        }

        if (sample_subset_red && nr_moves % 2 == 0)
            for (auto const& subset: static_cast<BoardSet const&>(boards_to)) {
                BoardSubSetRed const& subset_red = static_cast<BoardSubSetRed const&>(static_cast<BoardSubSetBase const&>(subset));
                if (subset_red.size() > largest_red.size())
                    largest_red.assign(subset_red.begin(), subset_red.end());
            }

        // if (verbose) cout << moved_armies << boards_to;
        auto& stats = stats_list.back();
        if (boards_to.size() == 0) {
            auto stop_solve = chrono::steady_clock::now();
            duration = chrono::duration_cast<Sec>(stop_solve-start_solve).count();
            cout << stats << setw(6) << duration << " s, no solution" << endl;
            if (largest_red.size()) save_largest_red(largest_red);
            return -1;
        }
        if (example) {
            stats.example_board
                (example > 0 ?
                 boards_to.example(opponent_armies, moved_armies, nr_moves & 1) :
                 boards_to.random_example(opponent_armies, moved_armies, nr_moves & 1));
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

    if (red_id == 0) throw(logic_error("Solved without solution"));
    return i;
}

void backtrack(Board const& board, int nr_moves, int solution_moves,
               ArmyZ const& last_red_army,
               StatisticsList& stats_list, Sec::rep& duration,
               BoardList& boards) {
    cout << "Start backtracking\n";

    auto start_backtrack = chrono::steady_clock::now();
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
    if (verbose) {
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

        stats_list.emplace_back
            (make_all_moves_backtrack
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
            throw(logic_error("No solution while backtracking"));
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
    ArmyZSet const& final_army_set = *army_set.back();

    ArmyId blue_id;
    if (nr_moves == solution_moves) {
        // There should be only 1 blue army completely on the red base
        if (final_army_set.size() != 1)
            throw(logic_error("More than 1 final blue army"));
        blue_id = final_board_set.back_id();
        if (blue_id != 1)
            throw(logic_error("Unexpected blue army id " + to_string(blue_id)));
        // There should be only 1 final board
        if (final_board_set.size() != 1)
            throw(logic_error("More than 1 solution while backtracking"));
    } else {
        ArmyZ blue_army = tables.start().red();
        blue_id = final_army_set.find(blue_army);
        if (blue_id == 0)
            throw(logic_error("Could not find final blue army"));
    }

    BoardSubSet const& final_subset = final_board_set.cat(blue_id);
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
        throw(logic_error("Unexpected red army id " +to_string(red_id)));
    // And it was stored without flip
    if (skewed)
        throw(logic_error("Unexpected red army skewed"));

    ArmyZ blueZ = final_army_set.at(blue_id);
    ArmyZ redZ  = last_red_army;

    // It's probably more useful to generate a FullMove sequence
    // instead of a board sequence. Punt for now. --Note

    // Reserve nr_moves+1 boards
    size_t board_pos = board_set.size();
    boards.resize(board_pos);
    boards[--board_pos] = Board{blueZ, redZ};

    if (false) {
        cout << "Initial\n";
        cout << "Blue: " << blue_id << ", Red: " << red_id << ", skewed=" << skewed << "\n";
        cout << boards[board_pos];
    }

    board_set.pop_back();
    army_set.pop_back();
    army_set.pop_back();

    ArmyZ blueSymmetricZ = blueZ.symmetric();
    ArmyZ redSymmetricZ  = redZ.symmetric();
    int blue_symmetry = cmp(blueZ, blueSymmetricZ);
    int red_symmetry  = cmp(redZ , redSymmetricZ);

    Image image{blueZ, redZ};
    ArmyZPos armyE, armySymmetricE;
    for (int blue_to_move = 1;
         board_set.size() != 0;
         blue_to_move = 1-blue_to_move, board_set.pop_back(), army_set.pop_back()) {
        if (is_terminated()) return;

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
            array<Coord, X*Y-2*ARMY+1> reachable;

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

void signal_handler(int signum) {
    switch(signum) {
        case SIGSYS:
          signal_counter += 2;
          signal_generation.store(signal_counter, memory_order_relaxed);
          break;
        case SIGUSR1:
          if (nr_threads > 1) --nr_threads;
          break;
        case SIGUSR2:
          ++nr_threads;
          break;
        case SIGINT:
        case SIGTERM:
          signal_counter = 0;
          signal_generation.store(signal_counter, memory_order_relaxed);
        default:
          // Impossible. Ignore
          break;
    }
}

void set_signals() {
    struct sigaction new_action;

    signal_generation = signal_counter;

    new_action.sa_handler = signal_handler;
    sigfillset(&new_action.sa_mask);
    new_action.sa_flags = 0;

    if (sigaction(SIGSYS, &new_action, nullptr))
        throw(system_error(errno, system_category(),
                           "Could not set SIGSYS handler"));
    if (sigaction(SIGUSR1, &new_action, nullptr))
        throw(system_error(errno, system_category(),
                           "Could not set SIGUSR1 handler"));
    if (sigaction(SIGUSR2, &new_action, nullptr))
        throw(system_error(errno, system_category(),
                           "Could not set SIGUSR2 handler"));
    if (sigaction(SIGINT, &new_action, nullptr))
        throw(system_error(errno, system_category(),
                           "Could not set SIGUNT handler"));
    if (sigaction(SIGTERM, &new_action, nullptr))
        throw(system_error(errno, system_category(),
                           "Could not set SIGTERM handler"));
}

void my_main(int argc, char const* const* argv) {
    GetOpt options("b:B:t:sHSjpeErvR:A", argv);
    long long int val;
    bool replay = false;
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
            case 'R': sample_subset_red = options.arg(); break;
            case 't':
              val = atoll(options.arg());
              if (val < 0)
                  throw(range_error("Number of threads cannot be negative"));
              if (val > THREADS_MAX)
                  throw(range_error("Too many threads"));
              nr_threads = val;
              break;
            default:
              cerr << "usage: " << argv[0] << " [-A] [-t threads] [-b balance] [-B balance_delay] [-s] [-j] [-p] [-e] [-H] [-S] [-R sample_red_file]\n";
              exit(EXIT_FAILURE);
        }
    balance_min = ARMY     / 4 - balance;
    balance_max = (ARMY+3) / 4 + balance;
    if (nr_threads == 0) nr_threads = thread::hardware_concurrency();

    cout << "Time " << time_string() << "\n";
    cout << "Pid: " << getpid() << "\n";
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
        cout << "Army red:\n" << tables.army_red();
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
    ArmyZ red_army;
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
        tid = 0;
        allocated_ = 0;
        total_allocated_ = 0;

        tzset();
        system_properties();

        my_main(argc, argv);
        cout << "Final memory " << total_allocated() << "\n";
        if (is_terminated())
            cout << "Terminated by signal" << endl;
    } catch(exception& e) {
        cerr << "Exception: " << e.what() << endl;
        cerr << "Final memory " << total_allocated() << "\n";
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}
