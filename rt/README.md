# rt/ — the platform layer

Everything `vm/` cannot do for itself: time, memory, threads, files, and the
mechanism that stops the world. This is the one layer allowed to use libc.

`vm/` compiles with `-ffreestanding -nostdinc`, so it cannot call a libc
function even by accident. What it needs instead is declared in `rt.h` and
implemented per platform under `pal/`.

## The ABI is narrow on purpose

Three things are true of every declaration in `rt.h`, and they are the reason
it is worth keeping small:

- **It uses only types from `base/`.** No `FILE`, no `pthread_t`, no `size_t`
  from a libc header. A libc type that leaked this far up would appear in
  `vm/`'s source, which is compiled without libc headers and would not build.
- **`vm/` includes this header and no other platform one.** Porting means
  writing `pal/` again, not editing the collector.
- **Paths and payloads are `str`** — a pointer and a length, never assumed
  NUL-terminated. An implementation that has to hand a path to a libc call
  does the conversion inside itself, where the libc is.

## What is in it

| group | examples | why the collector needs it |
|---|---|---|
| `mem` | `rt_mem_alloc`, `rt_mem_reserve`, `rt_mem_commit` | the heap is one large reservation, mapped rather than malloc'd: a heap whose blocks are individually allocated cannot be described by address arithmetic |
| `thread` | `rt_thread_create`, `rt_thread_join`, `rt_hardware_threads` | the GC worker pool, and the finalizer thread |
| `sync` | `rt_mutex_*`, `rt_cond_*`, `rt_rwlock_*`, `rt_sem_*` | the thread registry's lock, the sleeper the pool waits on, the coalescing lock |
| `trap` | `rt_trap_arm`, `rt_trap_disarm`, `rt_signal_*` | one of the two safepoint mechanisms: a protected page per thread, whose fault parks it. The other, polling, needs nothing from the platform |
| `time` | `rt_time_nanos`, `rt_time_millis` | collection timing, and the event log's timestamps |
| `io` `channel` | `rt_channel_open_file`, `rt_channel_write` | the log and the statistics CSV, both of which must be writable from `vm/` without it knowing what a file is |
| `env` `process` | `rt_env_get`, `rt_process_exit` | configuration by environment variable, and exiting with a status |

`base/` holds the headers `vm/` and `rt/` share — `types.h`, `math.h`,
`assert.h`, `format.h` — and because they are shared they must be libc-free
themselves. `base/assert.c` is out of line because a failing assertion should
not be inlined into the collector's fast path.

## The rule is enforced, not documented

`vm/` not depending on libc is checked by the build on every run:

- a probe including every public header is compiled freestanding, because a
  header that needs a libc type breaks the collector before any of it runs;
- every compiled `vm/` object is scanned for undefined symbols outside
  `cvm_`/`rt_`. This is the check that matters, and it is the one a
  hand-declared libc prototype — which `-nostdinc` alone would let through —
  runs into.

Platform exceptions are named individually rather than matched by pattern,
and printed when they apply. There is exactly one today: macOS reaches its
thread-local storage through `__tlv_bootstrap`.

## Implementing it for a new platform

Copy `pal/posix/` and replace the implementations. The ABI does not change,
and `vm/` is not touched. Two things are worth knowing before starting:

- **`rt_mem_reserve` is not `malloc`.** It reserves address space that is
  never committed until it is used, and the collector relies on the pages
  reading zero — the bytemap's encoding of "free" is zero, and a fresh
  reservation is relied on rather than re-cleared.
- **The trap mechanism needs a signal handler and a way to arm a page.** A
  platform without either can still run the collector: the polling safepoint
  is a compile-time alternative, and `tests/unit/test_trap.c` reports which
  one the build uses.

`pal/posix/` is where the current work is. It is POSIX-shaped, and nothing is
written for Windows yet: the trap trampoline has no branch for it, so that
build would poll, and the rest of `pal/` is unimplemented there.
