# bench/go/

The seven scenarios rewritten in Go: `main.go` (the harness), `scenarios.go`,
`shapes.go`. One Go module, one binary.

## Why it is a rewrite and not a shared source

There is no way to link the C scenarios into a Go binary, so the workloads
are repeated here and the differences are written down where they occur. The
rule from `BENCH.md` §2.4 is that the *work* must be the same, not that the
source is. `shapes.go` asserts the sizes at init for exactly that reason: a
Go struct whose padding differed by eight bytes would put the two runs in
different size classes and quietly make the comparison meaningless, so the
binary refuses to start instead.

The allocation counts agree with the C side exactly — 4,000,000 and 6,291,432
— which is what says the two are running the same workload.

## What it does differently, and why

| | |
|---|---|
| one binary with `--scenario`, not one per scenario | the C side needs a binary per (scenario, backend) because it links a different collector into each. Go has one runtime and seven workloads, so seven near-identical binaries would be seven copies of one program |
| no optimization knob | Go has no `-O1`. Its own default optimizer runs, and `BENCH.md` §2.1 states that rather than pretending the two sides are aligned |
| alloc-only = `debug.SetGCPercent(-1)` | the exact statement "do not collect", which is stronger and more honest than a heap big enough that collection probably does not happen |
| scenario names use dashes | `churn-unary`, as the table in `BENCH.md` writes them. The C binaries are named after their files, so the runner asks each C binary for its scenario name rather than deriving one |

## What it reports

| column | source |
|---|---|
| `allocations` | counted in the scenario's own allocation helpers |
| `collections` | `runtime.MemStats.NumGC` |
| `pause_max_ns` | the longest entry in the `PauseNs` ring buffer — Go's own stop-the-world time. Its concurrent marking and sweeping are not in it, for the same reason they are not in cvm's |
| `live_objects` | `MemStats.HeapObjects` |
| `heap_bytes` | `MemStats.HeapSys` |

**Escape analysis is the hazard.** Go may allocate on the stack if it can
prove an object never escapes, and a scenario that quietly stopped allocating
would measure an empty loop — fast, believable and meaningless. Every
allocation goes through a helper that stores into a package-level sink first.
`go vet` reports no issues; the sinks are what keep the allocations real.

## Running it

Built and run by `scripts/bench.mjs` along with everything else, if a Go
toolchain is on `PATH`. Without one the Go rows are simply absent, and the
run says so rather than silently comparing three collectors.
