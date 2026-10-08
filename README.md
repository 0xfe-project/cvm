# cvm — a runtime without a runtime

Use C like LLVM: a language frontend lowers its program to C, and that C
links against cvm. What cvm provides is the two things C does not — a precise
garbage collector and a platform layer — and nothing else.

No bytecode, no interpreter, no scheduler of its own, no idle daemon. The
program is compiled C; the runtime it runs on is C's. cvm is a library that
happens to be the part of a language runtime that cannot be generated.

```
frontend ──generates──▶ C ──links──▶ libvm (the collector) + librt (platform)
```

## Where to read what

| | |
|---|---|
| [vm/](vm/README.md) | the collector: the algorithm, how a collection runs, what the design is good at and what it pays, and how to call it |
| [rt/](rt/README.md) | the platform layer: the only place libc is allowed, and the ABI `vm/` calls down into |
| [bench/](bench/README.md) | four collectors measured on seven workloads, with the figures and what is missing from them |


## Two rules that shape everything

**`vm/` does not depend on libc.** It compiles with `-ffreestanding
-nostdinc`; `rt/` is hosted, because it is where the syscalls live. This is
checked on every build rather than documented and hoped for: a probe
including every public header is compiled freestanding, and every `vm/`
object is scanned for undefined symbols outside `cvm_`/`rt_`.

**The collector is precise.** Roots come from an explicit frame chain reached
through TLS, and an object is scanned by walking its type descriptor. Nothing
scans raw stack memory, so a pointer-shaped integer on the stack cannot keep
an object alive.

## Building

Requires a C compiler, `ccache` and `clang-format`; a Node-compatible runtime
(node, bun or deno) drives the scripts. There is no Makefile.

```
pnpm run build        compile, then run the freestanding audit
pnpm run test         build and run the tests
pnpm run ci           format, build, and the debug/release/asan/tsan matrix
pnpm run bench:all    measure the four collectors, then redraw the figures
```

The design reference is the Commix collector from Scala Native, kept read-only
under `refs/` (Apache 2.0, not part of this repository's history).
