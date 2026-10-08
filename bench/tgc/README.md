# bench/tgc/

A 459-line tiny conservative collector (`gc/tgc.c`), and `backend_tgc.c`,
which is ours.

## Three metrics added by this project

Upstream tgc reports only table occupancy — `nitems` and `nslots` — which
cannot answer the two questions the benchmark asks of every backend: how many
allocations happened, and how long the longest collection stopped the
program. Both are counted in the vendored copy now:

| field | where | meaning |
|---|---|---|
| `bench_allocations` | `tgc_add`, on success | allocations that happened |
| `bench_collections` | `tgc_run` | collections performed |
| `bench_pause_max_ns` | around `tgc_run` | longest mark + sweep |

They are additive; nothing in tgc's own behaviour reads them. They live in
the vendored copy because a tgc reporting different numbers than upstream
would make this a comparison of two collectors. Those three fields are the
entire difference, and `gc/tgc.h` says so at the declaration.

## What it is

| | |
|---|---|
| collector | tgc, a floor rather than a competitor |
| marking | stop-the-world, **conservative**, single-threaded |
| roots | conservative — `setjmp` plus a stack walk from the current frame to the stack bottom |
| moves objects | no |
| allocator | `malloc` per object, into an open-addressed hash table |
| built | `-O1`, two files |

## What it has to do that the others do not

**A real stack address for the stack bottom.** `tgc_start` wants the address
of a stack variable; it walks memory from a local in its own marker up to
that address. A `static` here — which lives in `.bss`, nowhere near the
stack — makes it walk off the end of the stack and fault. That was this
backend's first bug.

**No typed allocation.** Every object is scanned conservatively, so
`alloc_typed` is the same call as `alloc`. Not a shortcut: it is what the
collector does, and the difference between it and a descriptor-driven
allocation is part of what is being measured.

## What it reports

| column | source |
|---|---|
| `allocations` | `bench_allocations` |
| `collections` | `bench_collections` |
| `pause_max_ns` | `bench_pause_max_ns` |
| `live_objects` | `nitems` — live slots in the hash table |
| `heap_bytes` | `nslots × sizeof(tgc_ptr_t)` — the table, not the objects |

## Modes

All three. Auto mode is real rather than nominal: `tgc_add` calls `tgc_run`
itself once the item count passes a threshold the last sweep set, so tgc
collects without being asked. An earlier version of this backend claimed tgc
had no automatic policy and reported auto mode unavailable; that was wrong —
the call is at `gc/tgc.c:369` and it is why the churn rows show more than a
million collections.

## Known gap: it cannot mark read-heavy

**Stack overflow in its own marker**, and it is arithmetic rather than a
race. `tgc_mark_ptr` recurses once per pointer field:

```c
for (k = 0; k < gc->items[i].size/sizeof(void*); k++) {
    tgc_mark_ptr(gc, ((void**)gc->items[i].ptr)[k]);
}
```

read-heavy's graph is a chain 200,000 nodes long, so marking one end nests
200,000 frames deep. At roughly 56 bytes a frame that is about 11 MiB
against an 8 MiB stack, and the fault lands on the guard page — which is why
the diagnosis was "segfault in the stack walk" for a while before the
recursion was noticed.

This is a property of the collector this row exists to compare against, not
of the benchmark, and "fixing" it would mean rewriting that collector into
something else. So the row is not produced, and `scripts/bench.mjs` lists it
under `KNOWN_GAPS` with this explanation rather than reporting a bare
failure — the difference between a limitation and a broken harness is one a
reader should not have to work out.
