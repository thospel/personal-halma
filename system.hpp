#include <ctime>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

// #define STATIC static
#define STATIC

#ifdef __GNUC__
# define RESTRICT	 __restrict__
# define NOINLINE	 __attribute__((__noinline__))
# define ALWAYS_INLINE	 __attribute__((always_inline))
# define LIKELY(x)	 __builtin_expect(!!(x),true)
# define UNLIKELY(x)	 __builtin_expect(!!(x),false)
# define HOT		 __attribute__((__hot__))
# define COLD		 __attribute__((__cold__))
// pure means does not modify any (non const) global memory.
# define PURE		 __attribute__((__pure__))
// const means does not read/modify any (non const) global memory.
# define FUNCTIONAL	 __attribute__((__const__))
# define ALLOC_SIZE(x)	 __attribute__((alloc_size(x)))
# define MALLOC		 __attribute__((malloc))
# define NONNULL         __attribute__((nonnull))
# define RETURNS_NONNULL __attribute__((returns_nonnull))
# define WARN_UNUSED     __attribute__((warn_unused_result))
#else // __GNUC__
# define RESTRICT
# define NOINLINE
# define ALWAYS_INLINE
# define LIKELY(x)	(x)
# define UNLIKELY(x)	(x)
# define HOT
# define COLD
# define PURE
# define FUNCTIONAL
# define ALLOC_SIZE(x)
# define MALLOC
# define NONNULL
# define RETURNS_NONNULL
# define WARN_UNUSED
#endif // __GNUC__

#define CAT(x, y) _CAT(x,y)
#define _CAT(x, y)	x ## y

#define STRINGIFY(x) _STRINGIFY(x)
#define _STRINGIFY(x) #x

#if defined(_MSC_VER)
     /* Microsoft C/C++-compatible compiler */
     #include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
     /* GCC-compatible compiler, targeting x86/x86-64 */
     #include <x86intrin.h>
#elif defined(__GNUC__) && defined(__ARM_NEON__)
     /* GCC-compatible compiler, targeting ARM with NEON */
     #include <arm_neon.h>
#elif defined(__GNUC__) && defined(__IWMMXT__)
     /* GCC-compatible compiler, targeting ARM with WMMX */
     #include <mmintrin.h>
#elif (defined(__GNUC__) || defined(__xlC__)) && (defined(__VEC__) || defined(__ALTIVEC__))
     /* XLC or GCC-compatible compiler, targeting PowerPC with VMX/VSX */
     #include <altivec.h>
#elif defined(__GNUC__) && defined(__SPE__)
     /* GCC-compatible compiler, targeting PowerPC with SPE */
     #include <spe.h>
#endif

size_t const MMAP_THRESHOLD = 128 * 1024;
bool const CLEAR = true;

extern bool FATAL;

extern uint nr_threads;
extern thread_local uint tid;
extern std::atomic<uint> signal_generation;
extern thread_local uint signal_generation_seen;

extern uint64_t PID;
extern std::string HOSTNAME;
extern std::string const VCS_COMMIT;
extern std::string const VCS_COMMIT_TIME;

[[noreturn]] void throw_errno(std::string text);
[[noreturn]] void throw_logic(char const* text);
[[noreturn]] void throw_logic(char const*, const char* file, int line);
[[noreturn]] void throw_logic(std::string text);
[[noreturn]] void throw_logic(std::string text, const char* file, int line);

void rm_file(std::string const& file);
std::string time_string(time_t time);
std::string time_string();
time_t now();
size_t get_memory(bool set_base_mem = false);
void set_signals();
bool is_terminated();
ssize_t total_allocated() PURE;
ssize_t total_mmapped() PURE;
ssize_t total_mmaps() PURE;
void update_allocated();
void init_system();

inline bool use_mmap(size_t size) {
    return size >= MMAP_THRESHOLD;
    // return false;
}

void* _allocate(size_t new_size) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
void* _callocate(size_t new_size) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
void* _reallocate(void* old_ptr, size_t old_size, size_t new_size) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void* _reallocate(void* old_ptr, size_t old_size, size_t new_size, size_t keep) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
// void* _creallocate(void* old_ptr, size_t old_size, size_t new_size) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void _deallocate(void *ptr, size_t old_size) NONNULL;

template<class T>
inline void allocate(T*& ptr, size_t new_size) {
    ptr = static_cast<T*>(_allocate(new_size * sizeof(T)));
}

template<class T>
inline T* allocate(size_t new_size) {
    return static_cast<T*>(_allocate(new_size * sizeof(T)));
}

template<class T>
inline void callocate(T*& ptr, size_t new_size) {
    ptr = static_cast<T*>(_callocate(new_size * sizeof(T)));
}

template<class T>
inline T* callocate(size_t new_size) {
    return static_cast<T*>(_callocate(new_size * sizeof(T)));
}

template<class T>
inline void reallocate(T*& old_ptr, size_t old_size, size_t new_size) {
    old_ptr = static_cast<T*>(old_ptr ? _reallocate(old_ptr, old_size * sizeof(T), new_size * sizeof(T)) : _allocate(new_size * sizeof(T)));
}

template<class T>
inline T* reallocate(T*& old_ptr, size_t old_size, size_t new_size, size_t keep) {
    T* new_ptr = static_cast<T*>(_reallocate(old_ptr, old_size * sizeof(T), new_size * sizeof(T), keep * sizeof(T)));
    old_ptr = nullptr;
    return new_ptr;
}

template<class T>
inline void creallocate(T*& old_ptr, size_t old_size, size_t new_size) {
    if (old_ptr) {
        _deallocate(old_ptr, old_size * sizeof(T));
        old_ptr = nullptr;
    }
    old_ptr = static_cast<T*>(_callocate(new_size * sizeof(T)));
}

template<class T>
inline void deallocate(T*& old_ptr, size_t old_size) {
    if (old_ptr) {
        _deallocate(old_ptr, old_size * sizeof(T));
        old_ptr = nullptr;
    }
}

template<class T>
inline void unallocate(T* old_ptr, size_t old_size) {
    _deallocate(old_ptr, old_size * sizeof(T));
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
    std::string prefix_;
    std::vector<char> buffer_;
};

class LogStream: public std::ostream {
  public:
    LogStream(): std::ostream{&buffer_} {}
  private:
    LogBuffer buffer_;
};

extern thread_local LogStream logger;
