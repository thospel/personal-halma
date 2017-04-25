#include <ctime>
#include <cstdint>

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

extern bool FATAL;

extern uint nr_threads;
extern thread_local uint tid;
extern std::atomic<uint> signal_generation;
extern thread_local uint signal_generation_seen;
extern thread_local ssize_t allocated_;
extern std::atomic<ssize_t> total_allocated_;

extern uint64_t PID;
extern std::string HOSTNAME;
extern std::string const VCS_COMMIT;
extern std::string const VCS_COMMIT_TIME;

[[noreturn]] extern void throw_errno(std::string text);
[[noreturn]] extern void throw_logic(char const* text);
[[noreturn]] extern void throw_logic(char const*, const char* file, int line);
[[noreturn]] extern void throw_logic(std::string text);
[[noreturn]] extern void throw_logic(std::string text, const char* file, int line);

extern void rm_file(std::string const& file);
extern std::string time_string(time_t time);
extern std::string time_string();
extern time_t now();
extern size_t get_memory(bool set_base_mem = false);
extern void set_signals();
extern bool is_terminated();
extern void* _mmap(size_t length) MALLOC ALLOC_SIZE(1) RETURNS_NONNULL WARN_UNUSED;
extern void _munmap(void* ptr, size_t length) NONNULL;
// Strictly speaking _mremap shouldn't get the MALLOC property, but we will never
// store pointers in the mmapped regions
extern void* _mremap(void* old_ptr, size_t old_length, size_t new_length) MALLOC ALLOC_SIZE(2) RETURNS_NONNULL WARN_UNUSED;

extern void init_system();

template<class T>
inline T* mmap(size_t size) {
    return static_cast<T *>(_mmap(size * sizeof(T)));
}
template<class T>
inline void munmap(T* ptr, size_t size) {
    _munmap(static_cast<void *>(ptr), size * sizeof(T));
}
template<class T>
inline void mremap(T* old_ptr, size_t old_size, size_t new_size) {
    return static_cast<T *>(_mremap(static_cast<void *>(old_ptr), old_size * sizeof(T), new_size * sizeof(T)));
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
