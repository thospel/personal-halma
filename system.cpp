#include "system.hpp"

#include <cstdlib>
#include <csignal>

#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>

#include <fstream>
#include <mutex>
#include <system_error>

// Needed to implement mlock2 as long as it's not in glibc
#include <sys/syscall.h>
#include <asm-generic/mman.h>

bool FATAL = false;

uint signal_counter;
std::atomic<uint> signal_generation;
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

uint64_t PID;
std::string HOSTNAME;
std::string const VCS_COMMIT{STRINGIFY(COMMIT)};
std::string const VCS_COMMIT_TIME{STRINGIFY(COMMIT_TIME)};

size_t PAGE_SIZE;
size_t PAGE_SIZE1;
size_t PAGE_MASK;

inline size_t PAGE_ROUND(size_t size) FUNCTIONAL;
size_t PAGE_ROUND(size_t size) {
    return (size + PAGE_SIZE1) & PAGE_MASK;
}

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

thread_local uint tid;
inline uint thread_id();
uint thread_id() {
    return tid;
}

inline std::string thread_name();
std::string thread_name() {
    return std::to_string(thread_id());
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
}

inline int _mlock2(void const* addr, size_t length, int flags) ALWAYS_INLINE;
int _mlock2(void const* addr, size_t length, int flags) {
    // return mlock2(ptr, length, flags))
    return syscall(__NR_mlock2, addr, length, flags);
}

void _mlock(void* ptr, size_t length) {
    if (_mlock2(ptr, length, MLOCK_ONFAULT))
        throw_errno("Could not mlock " + std::to_string(length) + " bytes");
    mlocked_ += length;
    ++mlocks_;
}

void _munlock(void* ptr, size_t length) {
    if (munlock(ptr, length))
        throw_errno("Could not munlock " + std::to_string(length) + " bytes");
    mlocked_ -= length;
    --mlocks_;
}

inline void* _mmap(size_t length, int flags) ALWAYS_INLINE;
void* _mmap(size_t length, int flags) {
    size_t length_rounded = PAGE_ROUND(length);
    void* ptr = mmap(nullptr,
                     length_rounded,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // logger << "mmap(" << length << "[" << PAGE_ROUND(length) << "]) -> " << ptr << "\n" << std::flush;
    if (ptr == MAP_FAILED)
        throw_errno("Could not mmap " + std::to_string(length) + " bytes");
    if (flags & ALLOC_LOCK) {
        if (_mlock2(ptr, length_rounded, MLOCK_ONFAULT)) {
            auto err = errno;
            munmap(ptr, length_rounded);
            throw_errno(err, "Could not mlock2 " + std::to_string(length) + " bytes");
        }
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

void* _allocate(size_t new_size) {
    if (use_mmap(new_size)) return _mmap(new_size, 0);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    return ptr;
}

void* _allocate(size_t new_size, int flags) {
    if (use_mmap(new_size)) return _mmap(new_size, flags);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    return ptr;
}

void* _callocate(size_t new_size) {
    if (use_mmap(new_size)) return _mmap(new_size, 0);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    std::memset(ptr, 0, new_size);
    return ptr;
}

void* _callocate(size_t new_size, int flags) {
    if (use_mmap(new_size)) return _mmap(new_size, flags);
    void* ptr = new char[new_size];
    allocated_ += new_size;
    std::memset(ptr, 0, new_size);
    return ptr;
}

void* _reallocate(void* old_ptr, size_t old_size, size_t new_size) {
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

void* _reallocate_partial(void* old_ptr, size_t old_size, size_t new_size, size_t keep) {
    void* new_ptr;
    if (use_mmap(old_size)) {
        if (use_mmap(new_size))
            new_ptr = _mremap(old_ptr, old_size, new_size, 0);
        else {
            new_ptr = new char[new_size];
            allocated_ += new_size;
            std::memcpy(new_ptr, old_ptr, keep);
            _munmap(old_ptr, old_size, 0);
        }
    } else {
        if (use_mmap(new_size))
            new_ptr = _mmap(new_size, 0);
        else {
            new_ptr = new char[new_size];
            allocated_ += new_size;
        }
        std::memcpy(new_ptr, old_ptr, keep);
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
    return new_ptr;
}

void* _reallocate(void* old_ptr, size_t old_size, size_t new_size, int flags) {
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

void* _reallocate_partial(void* old_ptr, size_t old_size, size_t new_size, size_t keep, int flags) {
    void* new_ptr;
    if (use_mmap(old_size)) {
        if (use_mmap(new_size))
            new_ptr = _mremap(old_ptr, old_size, new_size, flags);
        else {
            new_ptr = new char[new_size];
            allocated_ += new_size;
            std::memcpy(new_ptr, old_ptr, keep);
            _munmap(old_ptr, old_size, flags);
        }
    } else {
        if (use_mmap(new_size))
            new_ptr = _mmap(new_size, flags);
        else {
            new_ptr = new char[new_size];
            allocated_ += new_size;
        }
        std::memcpy(new_ptr, old_ptr, keep);
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
    return new_ptr;
}

void _deallocate(void *old_ptr, size_t old_size) {
    if (use_mmap(old_size)) _munmap(old_ptr, old_size, 0);
    else {
        delete [] static_cast<char *>(old_ptr);
        allocated_ -= old_size;
    }
}

void _deallocate(void *old_ptr, size_t old_size, int flags) {
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
    PID = static_cast<uint64_t>(getpid());

    long tmp = sysconf(_SC_PAGE_SIZE);
    if (tmp == -1)
        throw_errno("Could not determine PAGE SIZE");
    PAGE_SIZE  = tmp;
    PAGE_SIZE1 = PAGE_SIZE-1;
    PAGE_MASK  = ~PAGE_SIZE1;
}
