#pragma once

#if defined(__x86_64__) || defined(_M_X64)
#define KIMIX_ARCH_X86_64
#elif defined(__aarch64__)
#define KIMIX_ARCH_ARM64
#else
#error Unsupported architecture
#endif

#if defined(KIMIX_ARCH_X86_64)

#include <immintrin.h>
#include <cassert>

#define KIMIX_INTRIN_PAUSE() _mm_pause()

#elif defined(KIMIX_ARCH_ARM64)

#include <arm_neon.h>

#define KIMIX_INTRIN_PAUSE() asm volatile("isb")

#else

#include <thread>
#define KIMIX_INTRIN_PAUSE() std::this_thread::yield()

#endif

////////////// assume
#ifdef NDEBUG         // assume only enabled in non-debug mode.
#if defined(__clang__)// Clang
#define KIMIX_ASSUME(x) (__builtin_assume(x))
#elif defined(_MSC_VER)// MSVC
#define KIMIX_ASSUME(x) (__assume(x))
#else// GCC
#define KIMIX_ASSUME(x)                        \
    do {                                       \
        if (!(x)) { __builtin_unreachable(); } \
    } while (false)
#endif
#else
#define KIMIX_ASSUME(expression) assert(expression)
#endif

// debug trap
#if defined(_MSC_VER)
#define KIMIX_DEBUG_TRAP() __debugbreak()
#else
#if defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
#define KIMIX_DEBUG_TRAP() __builtin_debugtrap()
#elif defined(__has_builtin) && __has_builtin(__builtin_break)
#define KIMIX_DEBUG_TRAP() __builtin_break()
#else
#include <csignal>
#define KIMIX_DEBUG_TRAP() std::raise(SIGTRAP)
#endif
#endif
