/*
 * bench/harness/roots.h
 *
 * How a scenario tells the collector what is alive.
 *
 * This is the one place where the backends genuinely cannot share code, and
 * the difference is the subject of the comparison rather than an
 * inconvenience to be hidden.
 *
 *   cvm   reads roots from a frame the caller pushes. A frontend generates
 *         the push at every function entry, so a scenario that wants to be
 *         measured the way generated code would be must do the same. This is
 *         the requirement in BENCH.md §2.3.
 *
 *   bdwgc scans the stack and registers it word by word. The roots only have
 *         to be *on* the stack; nothing registers them.
 *
 *   tgc   does the same, from a setjmp point to the stack bottom.
 *
 * So for the conservative collectors the frame macros declare an array and
 * make it escape. `bench_roots_keep` exists for that: it is defined in
 * another translation unit so that the compiler cannot conclude the array is
 * unread and delete the stores. Without it a -O1 build removes the array and
 * the collector frees everything it pointed at, which is a crash that looks
 * like a collector bug and is not one.
 */

#ifndef BENCH_ROOTS_H
#define BENCH_ROOTS_H

#include "collector.h"

/* Defined in run.c. An opaque sink: it does nothing, visibly. */
void bench_roots_keep(void *p);

#if defined(BENCH_BACKEND_CVM)

#include "cvm/gc.h"

typedef CvmFrameHandle bench_frame_t;

#define BENCH_FRAME_DECL(name, n)                                                      \
    opaque_mut name##_roots[(n)];                                                      \
    bench_frame_t name##_frame

#define BENCH_FRAME_PUSH(name, n)                                                      \
    do {                                                                               \
        cvm_frame_push(&name##_frame, name##_roots, (n));                              \
    } while (0)

#define BENCH_FRAME_POP(name) cvm_frame_pop(&name##_frame)

#define BENCH_ROOT(name, i) (name##_roots[i])

/*
 * A pointer field store.
 *
 * The backend's own store, chosen at compile time rather than through the
 * vtable, and for a measured reason: through the vtable it is an indirect call
 * that cannot be inlined, and write-heavy -- the scenario that exists to
 * measure pointer stores -- came out 144% slower, which is the call and not the
 * barrier. A macro expands to the backend's own code, exactly like the frame
 * macros above.
 *
 * It still belongs here rather than in a scenario: what a store costs is part
 * of what is being compared, and a scenario must not decide it.
 */
#define BENCH_STORE(obj, offset, value)                                                \
    cvm_store_field((obj), (offset), (value))

#else /* conservative: the slots only have to live on the stack */

typedef int bench_frame_t;

#define BENCH_FRAME_DECL(name, n) void *name##_roots[(n)]

#define BENCH_FRAME_PUSH(name, n)                                                      \
    do {                                                                               \
        for (unsigned bench_i = 0; bench_i < (n); bench_i++) {                          \
            name##_roots[bench_i] = NULL;                                              \
        }                                                                              \
        bench_roots_keep(name##_roots);                                                \
    } while (0)

#define BENCH_FRAME_POP(name) ((void)0)

#define BENCH_ROOT(name, i) (name##_roots[i])

/*
 * A pointer field store. A plain store, because neither of these collectors
 * has a marking phase that runs alongside the program -- this is the baseline
 * the other backend's barrier is measured against.
 */
#define BENCH_STORE(obj, offset, value)                                                \
    (*(void **)((char *)(obj) + (offset)) = (value))

#endif

#endif /* BENCH_ROOTS_H */
