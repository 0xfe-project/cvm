# bench/results/

One record per **state that was committed**, not per run.

The distinction matters and this directory got it wrong for a while. Every
`pnpm run bench` writes a dated file, so a day of optimization work left
twenty-two of them here — each one a real measurement, none of them a result
anyone would cite. Working data belongs in `bench/build/`, which is ignored;
this directory is for the runs that say what the tree currently does.

So: re-running on the same commit replaces the record for that commit rather
than adding to it. A commit that changes performance keeps its own record,
because that is the one worth comparing against.

Every record carries what a number needs to mean anything, per `BENCH.md`:

- **when** it ran, to the second
- **where** it ran: platform, architecture, CPU model and core count
- **how** it ran: the optimization level, the repetition count, and the
  collector settings that are not ours to choose — Go's `GOGC`, bdwgc's heap
  parameters, the `mixed-sizes` seed

Without those, a number from a different machine or a different flag set is
indistinguishable from a regression.

## Reading a row

Each row is one (scenario, backend, mode) and carries the median of K runs
with the min and max beside it. The spread is not decoration: a median with
no dispersion is a number pretending to be a measurement, and several of
these scenarios are small enough that three repetitions cannot tell a
regression from a scheduler hiccup. `pnpm run bench:quick` is for checking
that the harness works, not for comparing collectors.

`pause_max_ns` is each backend's own longest stop-the-world window, measured
by that backend. For cvm it is mark + nullify + sweep preparation, and not
the sweep, which continues after the world resumes. For bdwgc it is its
PRE_STOP_WORLD to POST_START_WORLD event pair, for the same reason. Go reports
the longest entry in its own pause ring buffer. They are comparable because
each is the interval during which the program could not run — but only just,
and a reader comparing them should know what is inside each.
