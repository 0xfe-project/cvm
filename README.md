# cvm: use C like LLVM

cvm is a runtime whose compilation target is C. A language frontend lowers
its program to C, and that C links against cvm's two shared objects: a
precise garbage collector (`vm`) and a platform layer (`rt`).

The reference for the collector's design is the Commix collector from
Scala Native, kept read-only under `refs/` (Apache 2.0, not part of this
repository's history).

[TEST.md](TEST.md) covers how the collector is tested: the liveness
oracles, the stress patterns, and what is borrowed from other collectors.

## Status

The collector works end to end: a program allocates, collections reclaim,
and `tests/e2e/test_smoke.c` runs a whole program through the public
headers. What is in place:

    base types, integer math, heap geometry, GC metadata, the bytemap
    the heap, the block allocator, the per-thread allocator, large objects
    grey packets, the precise marker, weak references, the sweeper
    the thread registry, the safepoint synchroniser, the GC worker pool
    external handles, the global root table, the finalizer thread
    the entry points, and a smoke test

What is not: Windows. The platform layer is POSIX-shaped; the trap
trampoline has no Windows branch, so that build would poll instead, and
nothing in `rt/pal/` is written for it yet. Evacuation is also absent —
sweeping reclaims lines and blocks and leaves the fragmentation that
opportunistic evacuation would address — and because nothing moves objects,
there is no pinning either. The flag for it was removed rather than left in
the header describing behaviour no code implements.

What is not, and is easy to mistake for present: parallel sweeping. The
worker pool exists and marks in parallel, and `pool.c` has a `CVM_GC_SWEEP`
case, but the collector never wakes workers into it — so sweeping is
single-threaded today. The case is reachable code that nothing reaches,
which is the shape of a feature that looks shipped.

## Two safepoint mechanisms

Which one a build uses is a compile-time choice, so that the poll can be a
load rather than a load and a branch:

  **polling**   the safepoint is a flag the thread checks. Portable, needs
                nothing from the platform, and is the baseline.

  **traps**     the safepoint is a load from a per-thread page the
                collector makes inaccessible. One load, no branch, and
                nothing for the compiler to reorder around. Needs a signal
                handler and a per-architecture trampoline.

Traps are used on POSIX and off under a sanitizer, which installs its own
`SIGSEGV` handling and does not expect a fault that is deliberately handled
and resumed. Both mechanisms are therefore exercised: traps in the plain
debug and release runs, polling under asan and tsan.

## Layout

```
rt/                the platform layer: the only place libc is allowed
  base/            headers shared with vm, therefore libc-free themselves
    types.h        fixed-width integers, str, span, opaque, u24
    math.h         log2, rounding
    assert.h       cvm_assert, out of line in assert.c
vm/                the collector: freestanding, no libc at all
  include/cvm/     the public ABI that generated code includes
    gc.h           object header, frame, context, allocation fast path
  gc/              internal implementation headers
    layout.h       block/line geometry and growth policy
    meta.h         BlockMeta, LineMeta, ObjectMeta
    bytemap.h      address -> ObjectMeta
    block_list.[ch] lock-free and local block stacks
tests/
  vendor/          greatest.h, vendored
  unit/            one binary per unit
scripts/           build, test, format and CI drivers (no Makefile)
```

## Building and testing

Requires a C compiler, `ccache`, and `clang-format`. A Node-compatible
runtime (node, bun, or deno) drives the scripts; there is no Makefile and
no shell build script.

```
pnpm run build        # compile, then run the freestanding audit
pnpm run test         # build and run tests
pnpm run fmt          # format in place;  fmt:check for CI
pnpm run ci           # the full matrix: format, build, debug/release/asan/tsan
```

Incremental compilation is delegated to `ccache`; the build script hands
every source to the compiler on every run and lets the cache sort it out.

## The two rules that shape everything

**vm does not depend on libc.** `vm/` compiles with `-ffreestanding
-nostdinc`; `rt/` is hosted, because it is where the syscalls live.
Headers under `rt/base/` are shared with `vm/`, so they must stay libc-free
too. This is enforced, not documented-and-hoped: every build compiles a
probe including every public header and scans every compiled `vm/` object
for undefined symbols outside `cvm_`/`rt_`. A hand-declared libc prototype
— which `-nostdinc` alone would let through — is caught by the object
scan.

**The collector is precise.** Roots come from an explicit frame chain
reached through TLS, and objects are scanned by walking a recursive type
descriptor. Nothing scans raw stack memory, so a pointer-shaped integer on
the stack cannot keep an object alive.

## Conventions

- Fixed-width integers only: `u8`..`u64`, `i8`..`i64`, `usize`, `isize`.
  No bare `int`, `long`, or `char`.
- No C strings. Text is `str`, a pointer plus a length; it is never
  assumed NUL-terminated.
- `const` by default. A read-only pointee is the normal case and the
  mutable one is spelled out. Type-erased pointers are `opaque` (read-only)
  and `opaque_mut`; raw `void *` does not appear in an interface.
- Primitives are lowercase (`u32`, `str`, `u24`); named types are
  capitalised (`CvmGcHeader`, `BlockMeta`).
- No bitfields in structures whose layout must be stable across compilers
  (MSVC packs them differently), and no structure whose size depends on
  the build configuration.

## Platform notes

- TLS is `__thread` with the `initial-exec` model: one segment-offset
  access, no runtime call. The consequence is that the shared object must
  be in the initial load set; `dlopen` of it is unsupported.
- `mold` is an ELF-only linker and cannot link Mach-O, so it is used on
  Linux only. macOS uses the default `ld64`.
- macOS refuses section alignment above 16 KiB, so a GC heap cannot be a
  static array; it must be mapped and rounded up by hand.
