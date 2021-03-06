#include "system.hpp"

#include <cstdlib>
#include <csignal>
#include <cctype>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>

#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <system_error>
#include <map>

// Needed to implement mlock2 as long as it's not in glibc
#include <sys/syscall.h>
#include <asm-generic/mman.h>

#include <sched.h>

bool FATAL = false;

// 0 means let C++ decide
uint nr_threads = 0;

uint signal_counter;
std::atomic<uint> signal_generation;
bool MEMORY_REPORT = false;
bool change_locale = true;

thread_local ssize_t allocated_ = 0;
thread_local ssize_t mmapped_   = 0;
thread_local ssize_t mmaps_     = 0;
thread_local ssize_t mlocked_   = 0;
thread_local ssize_t mlocks_    = 0;
std::atomic<ssize_t> total_allocated_;
std::atomic<ssize_t> total_mmapped_;
std::atomic<ssize_t> total_mmaps_;
std::atomic<ssize_t> total_mlocked_;
std::atomic<ssize_t> total_mlocks_;

std::string const PID{std::to_string(getpid())};
std::string HOSTNAME;
std::string CPUS;
std::string const VCS_COMMIT{STRINGIFY(COMMIT)};
std::string const VCS_COMMIT_TIME{STRINGIFY(COMMIT_TIME)};

size_t PAGE_SIZE;
size_t PAGE_SIZE1;
size_t SYSTEM_MEMORY;
size_t SYSTEM_SWAP;
uint NR_CPU;

struct separator: std::numpunct<char> {
  protected:
    virtual string_type do_grouping() const
        { return "\003"; } // groups of 3
};

std::locale my_locale{std::locale{""}, new separator()};

void imbue(std::ostream& os) {
    os << std::fixed << std::setprecision(3);
    if (change_locale) os.imbue(my_locale);
}

// Linux specific
size_t get_memory(bool set_base_mem) {
    static size_t base_mem = 0;

    size_t mem = 0;
    size_t dummy;
    std::ifstream statm;
    statm.open("/proc/self/statm");
    statm >> dummy >> dummy >> dummy >> dummy >> dummy >> mem;
    mem *= PAGE_SIZE;
    if (set_base_mem) {
        base_mem = mem;
        // std::cout << "Base mem=" << mem / 1000000 << " MB\n";
    } else mem -= base_mem;
    return mem;
}

// Linux specific
void get_cpu_string() COLD;
void get_cpu_string() {
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (!fp) throw_errno("Could not open '/proc/cpuinfo'");
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    uint nr_cpu = 0;

    static char const MODEL_NAME[] = "model name";
    std::map<std::string, uint> cpus;
    while ((nread = getline(&line, &len, fp)) != -1) {
        char const* ptr = line;
        while (isspace(*ptr)) ++ptr;
        if (memcmp(ptr, MODEL_NAME, sizeof(MODEL_NAME)-1)) continue;
        ptr += sizeof(MODEL_NAME)-1;
        while (isspace(*ptr)) ++ptr;
        if (*ptr != ':') continue;
        ++ptr;
        while (isspace(*ptr)) ++ptr;
        char* end = line+nread;
        while (end > ptr && isspace(end[-1])) --end;
        if (end <= ptr) continue;
        *end = '\0';
        ++cpus[std::string{ptr, static_cast<size_t>(end-ptr)}];
        ++nr_cpu;
    }
    free(line);
    fclose(fp);

    CPUS.clear();
    if (nr_cpu == NR_CPU) {
        for (auto& entry: cpus) {
            if (!CPUS.empty()) CPUS.append("<br />");
            CPUS.append(std::to_string(entry.second));
            CPUS.append(" x ");
            CPUS.append(entry.first);
        }
    } else if (cpus.size() == 1) {
        for (auto& entry: cpus) {
            CPUS.append(std::to_string(NR_CPU));
            CPUS.append(" x ");
            CPUS.append(entry.first);
        }
    } else
        CPUS.append(std::to_string(NR_CPU));
}

void rm_file(std::string const& file) {
    unlink(file.c_str());
}

inline std::string _time_string(time_t time) {
    struct tm tm;

    if (!localtime_r(&time, &tm))
        throw_errno("Could not convert time to localtime");
    char buffer[80];
    if (!strftime(buffer, sizeof(buffer), "%F %T %z", &tm))
        throw_logic("strtime buffer too short");
    return std::string{buffer};
}

std::string time_string(time_t time) {
    return _time_string(time);
}

inline time_t _now() {
    time_t tm = time(nullptr);
    if (tm == static_cast<time_t>(-1)) throw_errno("Could not get time");
    return tm;
}

time_t now() {
    return _now();
}

std::string time_string() {
    return _time_string(_now());
}

thread_local uint tid = -1;
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

static std::mutex mutex_out_;

int LogBuffer::sync() {
    int size = pbase() - pptr();
    if (size) {
        {
            std::lock_guard<std::mutex> lock{mutex_out_};
            std::cout << prefix_;
            std::cout.write(pbase(), pptr() - pbase());
            std::cout.flush();
        }
        pbump(size);
    }
    return 0;
}

LogStream::LogStream(): std::ostream{&buffer_} {
    ::imbue(*this);
}

thread_local LogStream logger;

void throw_errno(int err, std::string const& text) {
    throw(std::system_error(err, std::system_category(), text));
}

void throw_errno(std::string const& text) {
    throw_errno(errno, text);
}

void throw_logic(char const* text, const char* file, int line) {
    throw_logic(std::string{text} + " at " + file + ":" + std::to_string(line));
}

void throw_logic(std::string const& text, const char* file, int line) {
    throw_logic(text + " at " + file + ":" + std::to_string(line));
}

void throw_logic(char const* text) {
    throw_logic(std::string{text});
}

void throw_logic(std::string const& text) {
    if (FATAL) {
        logger << text << std::endl;
        abort();
    }
    throw(std::logic_error(text));
}

bool is_terminated() {
    return signal_counter & 1;
}

void signal_handler(int signum) COLD;
void signal_handler(int signum) {
    switch(signum) {
        case SIGSYS:
          signal_counter += 2;
          signal_generation.store(signal_counter, std::memory_order_relaxed);
          break;
        case SIGUSR1:
          if (nr_threads > 1) --nr_threads;
          break;
        case SIGUSR2:
          ++nr_threads;
          break;
        case SIGINT:
        case SIGTERM:
          signal_counter = 1;
          signal_generation.store(signal_counter, std::memory_order_relaxed);
          break;
        case SIGSTKFLT:
          MEMORY_REPORT = !MEMORY_REPORT;
          break;
        default:
          // Impossible. Ignore
          break;
    }
}

void set_signals() {
    struct sigaction new_action;

    signal_generation = signal_counter = 0;

    new_action.sa_handler = signal_handler;
    sigfillset(&new_action.sa_mask);
    new_action.sa_flags = 0;

    if (sigaction(SIGSYS, &new_action, nullptr))
        throw_errno("Could not set SIGSYS handler");
    if (sigaction(SIGUSR1, &new_action, nullptr))
        throw_errno("Could not set SIGUSR1 handler");
    if (sigaction(SIGUSR2, &new_action, nullptr))
        throw_errno("Could not set SIGUSR2 handler");
    if (sigaction(SIGINT, &new_action, nullptr))
        throw_errno("Could not set SIGUNT handler");
    if (sigaction(SIGTERM, &new_action, nullptr))
        throw_errno("Could not set SIGTERM handler");
    if (sigaction(SIGSTKFLT, &new_action, nullptr))
        throw_errno("Could not set SIGTERM handler");
}

void raise_limit(int resource, rlim_t value) COLD;
void raise_limit(int resource, rlim_t value) {
    struct rlimit rlim;
    if (getrlimit(resource, &rlim))
        throw_errno("Could not getrlimit");
    if (value == rlim.rlim_cur) return;
    if (rlim.rlim_max != RLIM_INFINITY &&
        (value == RLIM_INFINITY || value > rlim.rlim_max)) {
        std::cerr << "Cannot raise resource " << resource << " from " << rlim.rlim_cur << " to " << value << ". Raise to hard limit " << rlim.rlim_max << " instead" << std::endl;
        rlim.rlim_cur = rlim.rlim_max;
    } else rlim.rlim_cur = value;
    if (setrlimit(resource, &rlim))
        throw_errno("Could not setrlimit");
    // std::cerr << "Resource " << resource << " raised to " << value << std::endl;
}

long read_value(char const* filename) COLD;
long read_value(char const* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) throw_errno("Could not open '" + std::string{filename} + "'");
    long value;
    if (!fscanf(fp, "%ld", &value)) {
        fclose(fp);
        throw_logic("Could not read value from '" + std::string{filename} + "'");
    }
    fclose(fp);
    return value;
}

void sched_batch() {
    struct sched_param param;
    param.sched_priority = 0;
    if (sched_setscheduler(0, SCHED_BATCH, &param))
        throw_errno("Could not sched_setscheduler");
}

uint64_t usage() {
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u))
        throw_errno("Could not getrusage");
    // return (UINT64_C(1000000)*u.ru_utime.tv_sec + u.ru_utime.tv_usec) + (UINT64_C(1000000)*u.ru_stime.tv_sec + u.ru_stime.tv_usec);
    return UINT64_C(1000000)*u.ru_utime.tv_sec + u.ru_utime.tv_usec;
}

void _madv_free(void* ptr, size_t length) {
    auto base = reinterpret_cast<size_t>(ptr);
    auto end = (base + length) & ~PAGE_SIZE1;
    base = (base + PAGE_SIZE1) & ~PAGE_SIZE1;
    if (base >= end) return;
    length = end - base;
    ptr = reinterpret_cast<void*>(base);
    if (madvise(ptr, length, MADV_FREE))
        throw_errno("Could not madvise MADV_FREE " + std::to_string(length) + " bytes");
}

void* _fd_mmap(Fd fd, size_t length) {
    size_t length_rounded = PAGE_ROUND(length);
    void* ptr = mmap(nullptr,
                     length_rounded,
                     PROT_READ,
                     MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED)
        throw_errno("Could not mmap " + std::to_string(length) + " bytes");
    //std::cout << "Mmap " << length << " bytes -> " << static_cast<void const *>(ptr) << "\n";
    return ptr;
}

void _fd_munmap(void *ptr, size_t length) {
    // std::cout << "Unmap " << length << " bytes\n";
    size_t length_rounded = PAGE_ROUND(length);
    if (munmap(ptr, length_rounded))
        throw_errno("Could not set munmap " + std::to_string(length) + " bytes");
}

inline int _mlock2(void const* addr, size_t length, int flags) ALWAYS_INLINE;
int _mlock2(void const* addr, size_t length, int flags) {
    // return mlock2(ptr, length, flags))
    return syscall(__NR_mlock2, addr, length, flags);
}

void _mlock(void* ptr, size_t length) {
    if (_mlock2(ptr, length, MLOCK_ONFAULT))
        throw_errno("Could not mlock " + std::to_string(length) + " bytes");
    // logger << "mlock(" << ptr << ", " << length << ")" << std::endl;
    mlocked_ += length;
    ++mlocks_;
}

void _munlock(void* ptr, size_t length) {
    if (munlock(ptr, length))
        throw_errno("Could not munlock " + std::to_string(length) + " bytes");
    // logger << "munlock(" << ptr << ", " << length << ")" << std::endl;
    mlocked_ -= length;
    --mlocks_;
}

// On my system (notebook with skylake):
//   about 0.5 to 1   microsecond per mmap/munmap pair (2.5 us for 100G)
//   about 0.3 to 0.5 microsecond per mremap (4 us for 100G)
inline void* _mmap(size_t length, int flags) ALWAYS_INLINE;
void* _mmap(size_t length, int flags) {
    size_t length_rounded = PAGE_ROUND(length);
    void* ptr = mmap(nullptr,
                     length_rounded,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // logger << "mmap(" << length << "[" << PAGE_ROUND(length) << "], " << std::hex << flags << std::dec << ") -> " << ptr << "\n" << std::flush;
    if (ptr == MAP_FAILED)
        throw_errno("Could not mmap " + std::to_string(length) + " bytes");
    if (flags & ALLOC_LOCK) {
        if (_mlock2(ptr, length_rounded, MLOCK_ONFAULT)) {
            auto err = errno;
            munmap(ptr, length_rounded);
            throw_errno(err, "Could not mlock2 " + std::to_string(length) + " bytes");
        }
        // logger << "mlock(" << ptr << ", " << length << ")" << std::endl;
        mlocked_ += length;
        ++mlocks_;
    }
    mmapped_ += length;
    ++mmaps_;
    return ptr;
}

inline void _munmap(void* ptr, size_t length, int flags) ALWAYS_INLINE;
void _munmap(void* ptr, size_t length, int flags) {
    // logger << "munmap(" << ptr << ", " << length << "[" << PAGE_ROUND(length) << "])\n" << std::flush;
    if (munmap(ptr, PAGE_ROUND(length)))
        throw_errno("Could not set munmap " + std::to_string(length) + " bytes");
    mmapped_ -= length;
    --mmaps_;
    if (flags & ALLOC_LOCK) {
        mlocked_ -= length;
        --mlocks_;
    }
}

inline void* _mremap(void* old_ptr, size_t old_length, size_t new_length, int flags) ALWAYS_INLINE;
void* _mremap(void* old_ptr, size_t old_length, size_t new_length, int flags) {
    size_t old_length_rounded = PAGE_ROUND(old_length);
    size_t new_length_rounded = PAGE_ROUND(new_length);
    void* new_ptr = mremap(old_ptr,
                           old_length_rounded,
                           new_length_rounded,
                           MREMAP_MAYMOVE);
    // logger << "mremap(" << old_ptr << ", " << old_length << "[" << old_length_rounded << "], " << new_length << "[" << PAGE_ROUND(new_length) << "]) -> " << new_ptr << "\n" << std::flush;
    if (new_ptr == MAP_FAILED)
        throw_errno("Could not mremap to " + std::to_string(new_length) + " bytes");
    mmapped_ += new_length - old_length;
    if (flags & ALLOC_LOCK) {
        if (_mlock2(new_ptr, new_length_rounded, MLOCK_ONFAULT)) {
            auto err = errno;
            munlock(new_ptr, std::min(old_length_rounded, new_length_rounded));
            mlocked_ -= old_length;
            --mlocks_;
            if (old_ptr == new_ptr)
                throw_errno(err, "Could not mlock2 " + std::to_string(new_length) + " bytes");
            // If we throw after reallocate the caller pointers are fucked
            logger << "Failed mlock2 on reallocate" << std::endl;
        } else
            mlocked_ += new_length - old_length;
    }
    return new_ptr;
}

void* _mallocate(size_t new_size) {
    if (use_mmap(new_size)) return _mmap(new_size, 0);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    return ptr;
}

void* _mallocate(size_t new_size, int flags) {
    if (use_mmap(new_size)) return _mmap(new_size, flags);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    return ptr;
}

void* _cmallocate(size_t new_size) {
    if (use_mmap(new_size)) return _mmap(new_size, 0);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    std::memset(ptr, 0, new_size);
    return ptr;
}

void* _cmallocate(size_t new_size, int flags) {
    if (use_mmap(new_size)) return _mmap(new_size, flags);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    std::memset(ptr, 0, new_size);
    return ptr;
}

void* _remallocate(void* old_ptr, size_t old_size, size_t new_size) {
    void* new_ptr;
    if (use_mmap(old_size)) {
        if (use_mmap(new_size))
            new_ptr = _mremap(old_ptr, old_size, new_size, 0);
        else {
            new_ptr = new char[new_size];
            std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
            allocated_ += new_size;
            _munmap(old_ptr, old_size, 0);
        }
    } else {
        if (use_mmap(new_size))
            new_ptr = _mmap(new_size, 0);
        else {
            new_ptr = new char[new_size];
            allocated_ += new_size;
        }
        std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
    return new_ptr;
}

void* _remallocate(void* old_ptr, size_t old_size, size_t new_size, int flags) {
    void* new_ptr;
    if (use_mmap(old_size)) {
        if (use_mmap(new_size))
            new_ptr = _mremap(old_ptr, old_size, new_size, flags);
        else {
            new_ptr = new char[new_size];
            std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
            allocated_ += new_size;
            _munmap(old_ptr, old_size, flags);
        }
    } else {
        if (use_mmap(new_size))
            new_ptr = _mmap(new_size, flags);
        else {
            new_ptr = new char[new_size];
            allocated_ += new_size;
        }
        std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
    return new_ptr;
}

// Only the newly added range is zerod, the old range remains
void* _recmallocate(void* old_ptr, size_t old_size, size_t new_size) {
    void* new_ptr;

    if (use_mmap(new_size)) {
        if (use_mmap(old_size)) {
            new_ptr = _mremap(old_ptr, old_size, new_size, 0);
        } else {
            new_ptr = _mmap(new_size, 0);
            std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
            delete [] static_cast<char *>(old_ptr);
            allocated_ -= old_size;
        }
    } else {
        new_ptr = new char[new_size];
        if (new_size > old_size) {
            std::memcpy(new_ptr, old_ptr, old_size);
            std::memset(static_cast<char *>(new_ptr) + old_size, 0, new_size - old_size);
        } else {
            std::memcpy(new_ptr, old_ptr, new_size);
        }
        if (use_mmap(old_size)) {
            _munmap(old_ptr, old_size, 0);
        } else {
            delete [] static_cast<char *>(old_ptr);
            allocated_ -= old_size;
        }
        allocated_ += new_size;
    }

    return new_ptr;
}

// Only the newly added range is zerod, the old range remains
void* _recmallocate(void* old_ptr, size_t old_size, size_t new_size, int flags) {
    void* new_ptr;

    if (use_mmap(new_size)) {
        if (use_mmap(old_size)) {
            new_ptr = _mremap(old_ptr, old_size, new_size, flags);
        } else {
            new_ptr = _mmap(new_size, flags);
            std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
            delete [] static_cast<char *>(old_ptr);
            allocated_ -= old_size;
        }
    } else {
        new_ptr = new char[new_size];
        if (new_size > old_size) {
            std::memcpy(new_ptr, old_ptr, old_size);
            std::memset(static_cast<char *>(new_ptr) + old_size, 0, new_size - old_size);
        } else {
            std::memcpy(new_ptr, old_ptr, new_size);
        }
        if (use_mmap(old_size)) {
            _munmap(old_ptr, old_size, flags);
        } else {
            delete [] static_cast<char *>(old_ptr);
            allocated_ -= old_size;
        }
        allocated_ += new_size;
    }

    return new_ptr;
}

void _demallocate(void *old_ptr, size_t old_size) {
    if (use_mmap(old_size)) _munmap(old_ptr, old_size, 0);
    else {
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
}

void _demallocate(void *old_ptr, size_t old_size, int flags) {
    if (use_mmap(old_size)) _munmap(old_ptr, old_size, flags);
    else {
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
}

ssize_t total_allocated() {
    if (tid) throw_logic("Use of total_allocated inside a thread");
    total_allocated_ += allocated_;
    allocated_ = 0;
    return total_allocated_;
}

ssize_t total_mmapped() {
    if (tid) throw_logic("Use of total_mmapped inside a thread");
    total_mmapped_ += mmapped_;
    mmapped_ = 0;
    return total_mmapped_;
}

ssize_t total_mmaps() {
    if (tid) throw_logic("Use of total_mmaps inside a thread");
    total_mmaps_ += mmaps_;
    mmaps_ = 0;
    return total_mmaps_;
}

ssize_t total_mlocked() {
    if (tid) throw_logic("Use of total_mlocked inside a thread");
    total_mlocked_ += mlocked_;
    mlocked_ = 0;
    return total_mlocked_;
}

ssize_t total_mlocks() {
    if (tid) throw_logic("Use of total_mlocks inside a thread");
    total_mlocks_ += mlocks_;
    mlocks_ = 0;
    return total_mlocks_;
}

void update_allocated() {
    if (tid) {
        total_allocated_ += allocated_;
        total_mmapped_   += mmapped_;
        total_mmaps_     += mmaps_;
        total_mlocked_   += mlocked_;
        total_mlocks_    += mlocks_;
    }
}

Fd OpenReadWrite(std::string const& filename) {
    // std::cout << "Open " << filename << "\n";
    int fd = open(filename.c_str(), O_RDWR | O_TRUNC | O_CREAT, 0666);
    if (fd < 0)
        throw_errno("Could not open '" + filename + "' for write");
    return fd;
}

Fd OpenRead(std::string const& filename) {
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0)
        throw_errno("Could not open '" + filename + "' for read");
    return fd;
}

void Close(Fd fd, std::string const& filename) {
    // std::cout << "Close " << filename << "\n";
    if (close(fd))
        throw_errno("Could not close '" + filename + "'");
}

void Extend(Fd fd, size_t offset, size_t size, std::string const& filename) {
    if (posix_fallocate(fd, offset, size))
        throw_errno("Could not extend '" + filename + "' by " + std::to_string(size) + " bytes");
}

void Write(Fd fd, void const* buffer, size_t size, std::string const& filename) {
    while (true) {
        auto rc = write(fd, buffer, size);
        if (rc > 0) {
            // logger << "Write " << rc << " of " << size << " bytes" << std::endl;
            size -= rc;
            if (size == 0) return;
            logger << "Partial write " << rc << " bytes" << std::endl;
            buffer = static_cast<char const*>(buffer) + rc;
        } else if (rc == 0)
            throw_logic("Zero size write");
        else if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
            throw_errno("Could not write " + std::to_string(size) + "bytes to '" + filename + "'");
    }
}

void Read(Fd fd, void* buffer, size_t offset, size_t size, std::string const& filename) {
    while (true) {
        auto rc = pread(fd, buffer, size, offset);
        if (rc > 0) {
            size -= rc;
            if (size == 0) return;
            logger << "Partial read " << rc << " bytes" << std::endl;
            buffer = static_cast<char*>(buffer) + rc;
            offset -= rc;
        } else if (rc == 0)
            throw_logic("Unexpected EOF while reading from '" + filename + "'");
        else if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
            throw_errno("Could not read " + std::to_string(size) + "bytes from '" + filename + "'");
    }
}

void init_system() {
    tzset();

    tid = 0;
    total_allocated_ = 0;
    total_mmapped_   = 0;
    total_mmaps_     = 0;
    total_mlocked_   = 0;
    total_mlocks_    = 0;

    char hostname[100];
    hostname[sizeof(hostname)-1] = 0;
    int rc = gethostname(hostname, sizeof(hostname)-1);
    if (rc) throw_errno("Could not determine host name");
    HOSTNAME.assign(hostname);

    long tmp = sysconf(_SC_PAGE_SIZE);
    if (tmp == -1)
        throw_errno("Could not determine PAGE SIZE");
    PAGE_SIZE  = tmp;
    PAGE_SIZE1 = PAGE_SIZE-1;

    tmp = read_value("/proc/sys/vm/max_map_count");
    if (tmp < 65536)
        std::cerr << "Warning: Maximum number of mmapped areas is " << tmp << ". This may be too low for large problems. If you run out of mmaps either increase the value in /proc/sys/vm/max_map_count or recompile with a larger value for MMAP_THRESHOLD (currently " << MMAP_THRESHOLD << ")\n" << std::flush;

    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof(cs), &cs))
        throw_errno("Could not determine number of CPUs");
    NR_CPU = CPU_COUNT(&cs);
    get_cpu_string();

    struct sysinfo s_info;
    if (sysinfo(&s_info))
        throw_errno("Could not determine memory");
    SYSTEM_MEMORY = static_cast<size_t>(s_info.totalram ) * s_info.mem_unit;
    SYSTEM_SWAP   = static_cast<size_t>(s_info.totalswap) * s_info.mem_unit;

    raise_limit(RLIMIT_MEMLOCK, RLIM_INFINITY);
}
