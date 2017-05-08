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
# define UNUSED          __attribute__((unused))
# define BUILTIN_CONSTANT(x) __builtin_constant_p(x)
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
# define UNUSED
# define BUILTIN_CONSTANT(x) true
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

size_t const MMAP_THRESHOLD  = 128 * 1024;
// size_t const MMAP_THRESHOLD  = 4096;
size_t const MMAP_THRESHOLD1 = MMAP_THRESHOLD - 1;
size_t const MMAP_THRESHOLD_MASK = ~MMAP_THRESHOLD1;
inline size_t MMAP_THRESHOLD_ROUND(size_t size) FUNCTIONAL;
size_t MMAP_THRESHOLD_ROUND(size_t size) {
    return (size + MMAP_THRESHOLD1) & MMAP_THRESHOLD_MASK;
}

bool const CLEAR = true;

int const ALLOC_LOCK     = 1;
int const ALLOC_POPULATE = 2;

extern bool FATAL;

extern uint nr_threads;
extern thread_local uint tid;
extern std::atomic<uint> signal_generation;
extern thread_local ssize_t allocated_;

extern size_t PAGE_SIZE;
extern size_t PAGE_SIZE1;
extern uint64_t PID;
extern std::string HOSTNAME;
extern std::string const VCS_COMMIT;
extern std::string const VCS_COMMIT_TIME;

[[noreturn]] void throw_errno(std::string const& text);
[[noreturn]] void throw_errno(int err, std::string const& text);
[[noreturn]] void throw_logic(char const* text);
[[noreturn]] void throw_logic(char const*, const char* file, int line);
[[noreturn]] void throw_logic(std::string const& text);
[[noreturn]] void throw_logic(std::string const& text, const char* file, int line);

inline size_t PAGE_ROUND(size_t size) FUNCTIONAL;
size_t PAGE_ROUND(size_t size) {
    return (size + PAGE_SIZE1) & ~PAGE_SIZE1;
}

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
ssize_t total_mlocked() PURE;
ssize_t total_mlocks() PURE;
void update_allocated();
void init_system();

inline bool main_thread() PURE;
bool main_thread() {
    return tid == 0;
}

inline uint popcount64(uint64_t value) FUNCTIONAL;
uint popcount64(uint64_t value) {
    // __builtin_popcountll generates the same assembly as _mm_popcnt_u64
    // if _mm_popcnt_u64 is available so the define doesn't really matter.
    // However it seems older gcc's were not as good, so this just makes sure
#if __POPCNT__
    return _mm_popcnt_u64(value);
#else
    return __builtin_popcountll(value);
#endif
}

inline uint clz64(uint64_t value) FUNCTIONAL;
uint clz64(uint64_t value) {
    return __builtin_clzll(value);
}

inline uint ctz64(uint64_t value) FUNCTIONAL;
uint ctz64(uint64_t value) {
    return __builtin_ctzll(value);
}

inline bool use_mmap(size_t size) {
    return size >= MMAP_THRESHOLD;
    // return false;
}

void _mlock(void* ptr, size_t length);
void _munlock(void* ptr, size_t length);
void* _mallocate(size_t new_size) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
void* _mallocate(size_t new_size, int flags) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
void* _cmallocate(size_t new_size) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
void* _cmallocate(size_t new_size, int flags) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
void* _remallocate(void* old_ptr, size_t old_size, size_t new_size) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void* _remallocate_partial(void* old_ptr, size_t old_size, size_t new_size, size_t keep) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void* _remallocate(void* old_ptr, size_t old_size, size_t new_size, int flags) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void* _remallocate_partial(void* old_ptr, size_t old_size, size_t new_size, size_t keep, int flags) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void* _recmallocate(void* old_ptr, size_t old_size, size_t new_size) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void* _recmallocate(void* old_ptr, size_t old_size, size_t new_size, int flags) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
// void* _cremallocate(void* old_ptr, size_t old_size, size_t new_size) MALLOC ALLOC_SIZE(3) NONNULL RETURNS_NONNULL WARN_UNUSED;
void _demallocate(void *ptr, size_t old_size) NONNULL;
void _demallocate(void *ptr, size_t old_size, int flags) NONNULL;

template<class T>
void memlock(T* ptr, size_t size) {
    size *= sizeof(T);
    // We assume only complete ranges are locked so size also implies mmmapped
    if (use_mmap(size)) _mlock(ptr, size);
}

template<class T>
void memunlock(T* ptr, size_t size) {
    size *= sizeof(T);
    // We assume only complete ranges are unlocked so size also implies mmmapped
    if (use_mmap(size)) _munlock(ptr, size);
}

template<class T>
inline T* mallocate(size_t new_size) {
    return static_cast<T*>(_mallocate(new_size * sizeof(T)));
}

template<class T>
inline void mallocate(T*& ptr, size_t new_size) {
    ptr = mallocate<T>(new_size);
}

template<class T>
inline T* mallocate(size_t new_size, int flags) {
    return static_cast<T*>(_mallocate(new_size * sizeof(T), flags));
}

template<class T>
inline void mallocate(T*& ptr, size_t new_size, int flags) {
    ptr = mallocate<T>(new_size, flags);
}

template<class T>
inline T* cmallocate(size_t new_size) {
    return static_cast<T*>(_cmallocate(new_size * sizeof(T)));
}

template<class T>
inline void cmallocate(T*& ptr, size_t new_size) {
    ptr = cmallocate<T>(new_size);
}

template<class T>
inline T* cmallocate(size_t new_size, int flags) {
    return static_cast<T*>(_cmallocate(new_size * sizeof(T), flags));
}

template<class T>
inline void cmallocate(T*& ptr, size_t new_size, int flags) {
    ptr = cmallocate<T>(new_size, flags);
}

template<class T>
inline void remallocate(T*& old_ptr, size_t old_size, size_t new_size) {
    old_ptr = static_cast<T*>(_remallocate(old_ptr, old_size * sizeof(T), new_size * sizeof(T)));
}

template<class T>
inline void remallocate(T*& old_ptr, size_t old_size, size_t new_size, int flags) {
    old_ptr = static_cast<T*>(_remallocate(old_ptr, old_size * sizeof(T), new_size * sizeof(T), flags));
}

// Only the newly added range is zerod, the old range remains
template<class T>
inline void recmallocate(T*& old_ptr, size_t old_size, size_t new_size) {
    old_ptr = static_cast<T*>(_recmallocate(old_ptr, old_size * sizeof(T), new_size * sizeof(T)));
}

// Only the newly added range is zerod, the old range remains
template<class T>
inline void recmallocate(T*& old_ptr, size_t old_size, size_t new_size, int flags) {
    old_ptr = static_cast<T*>(_recmallocate(old_ptr, old_size * sizeof(T), new_size * sizeof(T), flags));
}

template<class T>
inline T* remallocate_partial(T*& old_ptr, size_t old_size, size_t new_size, size_t keep) {
    T* new_ptr = static_cast<T*>(_remallocate_partial(old_ptr, old_size * sizeof(T), new_size * sizeof(T), keep * sizeof(T)));
    old_ptr = nullptr;
    return new_ptr;
}

template<class T>
inline T* remallocate_partial(T*& old_ptr, size_t old_size, size_t new_size, size_t keep, int flags) {
    T* new_ptr = static_cast<T*>(_remallocate_partial(old_ptr, old_size * sizeof(T), new_size * sizeof(T), keep * sizeof(T), flags));
    old_ptr = nullptr;
    return new_ptr;
}

// Zero the whole new size
template<class T>
inline void cremallocate(T*& old_ptr, size_t old_size, size_t new_size) {
    _demallocate(old_ptr, old_size * sizeof(T));
    old_ptr = nullptr;
    old_ptr = static_cast<T*>(_cmallocate(new_size * sizeof(T)));
}

template<class T>
inline void cremallocate(T*& old_ptr, size_t old_size, size_t new_size, int flags) {
    _demallocate(old_ptr, old_size * sizeof(T), flags);
    old_ptr = nullptr;
    old_ptr = static_cast<T*>(_cmallocate(new_size * sizeof(T), flags));
}

template<class T>
inline void demallocate(T* old_ptr, size_t old_size) {
    _demallocate(old_ptr, old_size * sizeof(T));
}

template<class T>
inline void demallocate(T* old_ptr, size_t old_size, int flags) {
    _demallocate(old_ptr, old_size * sizeof(T), flags);
}

template<class T>
inline T* allocate(size_t new_size, int flags=0) {
    new_size *= sizeof(T);
    auto new_ptr = reinterpret_cast<T*>(new char[new_size]);
    allocated_ += new_size;
    return new_ptr;
}

template<class T>
inline void allocate(T*& ptr, size_t new_size, int flags=0) {
    ptr = allocate<T>(new_size, flags);
}

template<class T>
inline T* callocate(size_t new_size, int flags=0) {
    new_size *= sizeof(T);
    auto new_ptr = reinterpret_cast<T*>(new char[new_size]);
    std::memset(new_ptr, 0, new_size);
    allocated_ += new_size;
    return new_ptr;
}

template<class T>
inline void callocate(T*& ptr, size_t new_size, int flags=0) {
    ptr = callocate<T>(new_size, flags);
}

template<class T>
inline void deallocate(T* old_ptr, size_t old_size, int flags=0) ALWAYS_INLINE;
template<class T>
void deallocate(T* old_ptr, size_t old_size, int flags) {
    delete [] reinterpret_cast<char*>(old_ptr);
    allocated_ -= old_size * sizeof(T);
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
