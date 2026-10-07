/*
 * rt/base/types.h
 *
 * Base types for the entire CVM project. Zero libc dependency.
 *
 * Rules (enforced everywhere in rt/ and vm/):
 *   1. No raw C scalar types: no int/long/short/char/double/float.
 *      Use i8/i16/i32/i64, u8/u16/u32/u64, f32/f64, usize, isize.
 *   2. No C strings. `char` does not exist here; text is u8 bytes.
 *      Text in flight is always (u8 const *ptr, usize len) -> Str.
 *      No NUL termination is ever assumed.
 *   3. const discipline: every pointer parameter is
 *        T const *        (read-only pointee)
 *        T const * const  (read-only pointee, read-only pointer)
 *      Mutable pointers are the explicit exception, not the default.
 *   4. Fixed-width only. No implementation-defined widths anywhere.
 *      usize/isize exist solely for pointer arithmetic and sizes.
 */

#ifndef RT_BASE_TYPES_H
#define RT_BASE_TYPES_H

/* Freestanding headers only: compiler-provided, not libc. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdalign.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef size_t usize;
typedef ptrdiff_t isize;

/* A word: the unit the GC thinks in. Pointer-sized, unsigned. */
typedef uintptr_t word;
typedef intptr_t sword;

/*
 * opaque: type-erased pointer whose pointee type is not known at
 * this layer. const is the default; the mutable flavor is explicit.
 * Raw `void *` should not appear in cvm interfaces.
 */
typedef void const* opaque;
typedef void* opaque_mut;

_Static_assert(sizeof(usize) == sizeof(void*), "usize must be pointer-sized");
_Static_assert(sizeof(word) == sizeof(void*), "word must be pointer-sized");

/*
 * str: the only sanctioned way to pass text around.
 * Not NUL-terminated. Never assume text beyond [ptr, ptr+len).
 */
typedef struct str {
    u8 const* ptr;
    usize len;
} str;

#define STR_LITERAL(s) ((str){.ptr = (u8 const*)(s), .len = sizeof(s) - 1})

/* Byte span for raw memory that is not text. */
typedef struct span {
    u8* ptr;
    usize len;
} span;

typedef struct span_const {
    u8 const* ptr;
    usize len;
} span_const;

/*
 * Branch hints and codegen controls. Pure compiler intrinsics,
 * no libc. Mirrors what refs/shared/GCTypes.h does but with
 * our naming.
 */
#if defined(__GNUC__) || defined(__clang__)
#define CVM_LIKELY(x) __builtin_expect(!!(x), 1)
#define CVM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CVM_INLINE static inline __attribute__((always_inline))
#define CVM_NOINLINE __attribute__((noinline))
#define CVM_NORETURN __attribute__((noreturn))
#define CVM_PREFETCH(p) __builtin_prefetch((p), 0, 3)
#else
#define CVM_LIKELY(x) (x)
#define CVM_UNLIKELY(x) (x)
#define CVM_INLINE static inline
#define CVM_NOINLINE
#define CVM_NORETURN
#define CVM_PREFETCH(p) ((void)0)
#endif

/* Alignment constants used across GC and allocators. */
#define CVM_WORD_BITS ((u32)(sizeof(word) * 8))
#define CVM_WORD_SIZE ((usize)sizeof(word))
#define CVM_ALIGN_WORD ((usize)sizeof(word))

CVM_INLINE usize cvm_align_up(usize n, usize align) {
    /* align must be a power of two */
    return (n + (align - 1)) & ~(align - 1);
}

CVM_INLINE bool cvm_is_aligned(usize n, usize align) {
    return (n & (align - 1)) == 0;
}

#endif /* RT_BASE_TYPES_H */
