#include "system.hpp"

#include <cstdlib>
#include <csignal>

#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>

#include <fstream>
#include <mutex>
#include <system_error>

bool FATAL = false;

int signal_counter = 1;
std::atomic<uint> signal_generation;
thread_local uint signal_generation_seen;
thread_local ssize_t allocated_;
std::atomic<ssize_t> total_allocated_;

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

void throw_errno(std::string text) {
    throw(std::system_error(errno, std::system_category(), text));
}

void throw_logic(char const* text, const char* file, int line) {
    throw_logic(std::string{text} + " at " + file + ":" + std::to_string(line));
}

void throw_logic(std::string text, const char* file, int line) {
    throw_logic(text + " at " + file + ":" + std::to_string(line));
}

void throw_logic(char const* text) {
    throw_logic(std::string{text});
}

void throw_logic(std::string text) {
    if (FATAL) {
        logger << text << std::flush;
        abort();
    }
    throw(std::logic_error(text));
}

bool is_terminated() {
    return UNLIKELY(signal_counter % 2 == 0);
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
          signal_counter = 0;
          signal_generation.store(signal_counter, std::memory_order_relaxed);
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

void* _mmap(size_t length) {
    void* ptr = mmap(nullptr, 
                     PAGE_ROUND(length), 
                     PROT_READ | PROT_WRITE, 
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) 
        throw_errno("Could not set mmap " + std::to_string(length) + " bytes");
    return ptr;
}

void _munmap(void* ptr, size_t length) {
    if (munmap(ptr, PAGE_ROUND(length)))
        throw_errno("Could not set munmap " + std::to_string(length) + " bytes");
}

void* _mremap(void* old_ptr, size_t old_length, size_t new_length) {
    void* new_ptr = mremap(old_ptr,
                           PAGE_ROUND(old_length),
                           PAGE_ROUND(new_length),
                           MREMAP_MAYMOVE);
    if (new_ptr == MAP_FAILED) 
        throw_errno("Could not mremap to " + std::to_string(new_length) + " bytes");
    return new_ptr;
}

void init_system() {
    tzset();

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