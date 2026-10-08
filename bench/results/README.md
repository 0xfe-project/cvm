# bench/results/

One dated record per benchmark run. A file appears here only when
something was actually measured — a run that built the third-party
collectors and had no scenarios to run writes nothing, because a directory
whose stated purpose is "every file here is a result" is worth more than a
directory of empty ones.

Every record carries the three things a number needs to mean anything, per
`BENCH.md`:

- **when** it ran, to the second
- **where** it ran: platform, architecture, CPU model and core count
- **how** it ran: the optimization level and full flag set for every
  backend, the repetition count, and the collector settings that are not
  ours to choose — Go's `GOGC`, bdwgc's heap parameters, the `mixed-sizes`
  seed

Without those, a number from a different machine or a different flag set is
indistinguishable from a regression.

The format is whatever `bench/harness/run.c` writes; until the harness
exists there is nothing here to describe. What will not change is the three
items above.
