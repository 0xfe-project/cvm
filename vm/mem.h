/*
 * vm/mem.h
 *
 * The four memory primitives the compiler is entitled to call.
 *
 * In freestanding C the compiler may emit a call to memcpy, memmove,
 * memset or memcmp for something as ordinary as a struct assignment or an
 * array initialiser. A libc-free translation unit has to provide them, or
 * it fails to link for a reason that has nothing to do with anything the
 * programmer wrote. (This was not theoretical: the collection driver's
 * struct assignment produced a memcpy reference the moment it was added.)
 *
 * Visibility note, which matters more than the code:
 *
 * These definitions are hidden. vm.so must not export memcpy, because a
 * process that loads vm.so early would then have every libc memcpy call
 * in the process bind to ours — including calls from libraries that have
 * nothing to do with the collector. Hidden visibility keeps the symbol
 * internal to vm.so, so vm's own compiler-generated calls use it and
 * everything else keeps using libc.
 *
 * The signatures match the standard ones deliberately: the compiler emits
 * calls to these names with these argument types.
 */

#ifndef CVM_MEM_H
#define CVM_MEM_H

#include "base/types.h"

#if defined(__GNUC__) || defined(__clang__)
#define CVM_HIDDEN __attribute__((visibility("hidden")))
#else
#define CVM_HIDDEN
#endif

CVM_HIDDEN void* memcpy(void* restrict dst, void const* restrict src, usize n);
CVM_HIDDEN void* memmove(void* dst, void const* src, usize n);
CVM_HIDDEN void* memset(void* dst, int value, usize n);
CVM_HIDDEN int memcmp(void const* a, void const* b, usize n);

#endif /* CVM_MEM_H */
