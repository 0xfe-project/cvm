# Exceptions: the contract with the collector

## Open items — to be resolved before any of this is implemented

Deliberately first. These are the parts of this document that are *not*
settled, and a reader should meet them before the design rather than after
it. Everything below this section is the design; this section is what is
still missing from it.

### Four gaps found in review

| # | the gap | what it costs if left as it is |
|---|---|---|
| **1** | `pending_exception` is described as "a root", but that is a claim about code: `cvm_mark_roots` walks the frame chain and the global root table, and **neither reaches a field of the TLS context**. Nothing marks it today. | The exception in flight is collected between the throw and the catch. A5, and the loudest of the four. |
| **2** | Nothing says **when `pending_exception` is cleared**. | If nothing clears it, the last exception a thread threw is retained for the thread's lifetime — one object per thread, and a test will not see it. Belongs at the landing pad, where `exception_slot` is filled, and should be asserted empty on the next throw. |
| **3** | The frame array is **fixed-size per thread** (`frames`, "sized for the recursion"), and nothing says what happens at the bound. | A pointer chain was bounded by the C stack, which a guard page turns into a fault. An array has no guard page: the bound is either checked or it is a silent overwrite. Also unquantified: capacity × `sizeof(CvmGcFrame)` × threads, per thread. |
| **4** | B2 states the fix as "zero the slots, then link", which leaves the frontend an obligation it can forget. | `cvm_frame_push` already receives `roots` and `num_slots` — **it can zero them itself**, which makes the correct order the only order. This is the same argument the comment above `cvm_frame_push` makes about obligations that "look like boilerplate": the slot zeroing is one of those, and it is currently the frontend's. |

### Two of the four resolve by naming a mechanism, not by adding a field

Gaps 1 and 2 are about `pending_exception`: nothing marks it, and nothing says
who clears it. Both dissolve once the mechanism is chosen, because **the field
only exists for one of the three.**

- **With `setjmp`, the throw writes straight into `handler->exception_slot`,
  which points into the catcher's frame.** That frame is at or below the depth
  being restored to, so it survives the unwind, and the frame chain already
  covers it — it is a root the day it is written. No TLS field is needed, and
  gap 2 goes with it: the slot is an ordinary frame slot that dies with its
  frame, so there is nothing to clear.

  It also makes B1's ordering rule trivially satisfied. The rule exists because
  of the window between "the throwing frames stop being roots" and "something
  else holds the exception"; writing into a surviving frame closes that window
  at the write, rather than requiring that no poll happen after it.

- **With the explicit return protocol there is no immediate dispatch.** The
  error propagates through ordinary returns and each level checks a flag, so
  the exception needs a home that outlives every frame it passes through — the
  TLS slot — and then gaps 1 and 2 are real and both matter.

So the design has two ways to hold an exception and the document should not
carry both without saying why. `pending_exception` is the explicit protocol's;
`exception_slot` is `setjmp`'s; the setjmp path needs no TLS field and no
clearing rule.

### The frame array must hand out indices, not pointers

Gap 3 asks what happens at the bound. The bound now has an answer that did not
exist when this was written — `rt_mem_realloc` (see the two commits before
this section's) means the array can grow — and growing introduces a constraint
worth stating before anyone implements it.

**Generated code must never hold a pointer into the frame array across a
push.** `frame = &ctx->frames[ctx->frame_depth++]` hands out a pointer, and a
`realloc` during a deeper push moves the array and invalidates every pointer
the outer frames are holding. Holding the index is safe under growth, because
the index is what the array is addressed by.

That is already the shape of the design — the chain is an index, the handler
stores depths — but the obvious spelling of the push works against it, which is
why it is written down rather than implied.

The capacity question still wants a number: `40 bytes × depth × threads`. At a
4210-frame depth and a hundred threads it is 16 MiB, which is not nothing. A
checked bound as a backstop plus growth as the normal path is the shape that
survives a frontend that recurses deeply, and the check is the part that turns
"silent overwrite" into a fault.

### Why anyone would add the thing that arms the pitfall below

The `SS_ONSTACK` pitfall is an avoidance by absence, and absences do not hold
on their own. The reason to expect someone to add `sigaltstack` is concrete:
the trap mechanism parks a thread by faulting on a protected page, and a thread
whose C stack is nearly exhausted faults *again* inside the handler, on that
same exhausted stack. An alternate signal stack is the standard remedy for
exactly that, and it is the thing that would arm the bug.

Which is to say: the warning is not about a hypothetical. It is about the first
person to see a double fault in a trap handler.

### One thing the mechanism table must not be read as

The three mechanisms in "How control reaches a landing pad" are not three
equal choices with different prices. `setjmp` **loses a window**: it saves the
registers as of the try's entry, and the landing pad needs them as of the
throw, so every value live across a try has to be in memory. The explicit
return protocol and CFI do not have that correspondence problem. The table
says this, but a reader skimming the table will read "cost when nothing
throws: none" and not the paragraph below it.

### A platform pitfall, observed in the field

Found while reading wabt's exception runtime (`wasm2c/wasm-rt.h`), which
reaches the same mechanism through the same libc calls:

> On macOS XNU, there is a bug where nested `sigsetjmp` and `siglongjmp`
> across threads that have an allocated alternate signal stack (`SS_ONSTACK`)
> will erroneously cause the kernel to preserve the `SS_ONSTACK` flag in the
> thread state.
>
> — and the two reports that established it:
> <https://github.com/WebAssembly/wabt/issues/2654>,
> <https://github.com/golang/go/issues/44501>

**The interaction matters here and it is specific to this collector.** cvm
stops the world two ways, and one of them is a signal: the trap page faults
and the handler parks the thread. A thread whose `SS_ONSTACK` flag is stuck
set will be handled wrongly on its next fault — the kernel believes it is
already on the alternate stack.

**It does not apply today**, because `rt/pal/posix/trap.c` registers its
handler with `SA_SIGINFO | SA_RESTART` and installs no alternate stack. That
is an avoidance by absence rather than a decision, which is why it is written
down: adding `sigaltstack` or `SA_ONSTACK` is a normal thing to do for signal
robustness, and it would silently arm this. Anyone who does it should read
those two issues first and decide deliberately.


This is not a document about how to implement try/catch. It is about the
places where an unwinding mechanism and a precise collector touch, what each
one assumes about the other, and how to keep either from corrupting the other.

Two systems that both own "where the program is" will disagree eventually.
The disagreement is always the same shape: **the collector believes something
is reachable that the unwinder believes is dead, or the reverse.** Either
direction is a bug, and only one of them is loud.

## The collector's assumptions

Everything below is a consequence of these five. If a change to the exception
path violates one, the failure is listed with it.

| # | the collector assumes | violated by | symptom |
|---|---|---|---|
| A1 | every thread's frame chain is walkable, start to end | a frame node left pointing at reclaimed memory | **wild pointer read, crash** |
| A2 | a frame's `roots[0..num_slots)` are readable and each holds NULL, a heap payload pointer, or a value the bounds check rejects | a frame linked before its roots are written | false retention from a stale stack value |
| A3 | the chain contains exactly the live frames — no dead ones | an unwind that shortens the chain without withdrawing the frames' root slots | leak: live-looking garbage keeps objects alive |
| A4 | a thread recorded as managed will reach a safepoint | an unwind that loses the managed/unmanaged depth | **the world never stops: every thread hangs, not just this one** |
| A5 | every object the program can still reach is in a root, or reachable from one | an exception moved off a stack after that stack stopped being roots | **collection frees the exception; the catch reads reclaimed memory** |

A5 is the one that looks correct in every test until a collection happens to
land in its window.

## The state

Per thread, in the TLS context the collector already walks:

```c
/* frames become an index, not a pointer chain */
CvmGcFrame* frames;          /* a per-thread array, sized for the recursion */
u32         frame_depth;     /* how many are live; an unwind is a store here */

CvmHandler* handlers;        /* a per-thread array */
u32         handler_depth;

opaque_mut  pending_exception;   /* a root. See B1 below. */
```

Frames become an index rather than a linked list of pointers because **an
unwind must not be able to leave a dangling `prev`.** With a pointer chain,
unwinding past a frame leaves its node holding a pointer to memory that the C
stack has reclaimed (A1), and the collector walking the chain afterward reads
whatever is there — which is a crash, not a leak. With an index, the frames
above `frame_depth` are unreachable to the collector by construction: it
walks `frames[0..frame_depth)` and stops.

That is the whole argument. It is not that the frames are alive; it is that
**the collector cannot reach the ones that are not.**

The handler:

```c
typedef struct CvmHandler {
    u32 kind;                    /* CATCH | FINALLY | both */
    u32 frame_depth;             /* what to restore */
    u32 unmanaged_depth;         /* what to restore */
    opaque_mut* exception_slot;  /* where the exception lands. A root */
    void* landing_pad;           /* where control resumes */
    unsigned char jump[CVM_HANDLER_JUMP_SIZE];  /* setjmp storage, via rt/ */
} CvmHandler;
```

**The collector does not walk the handler chain.** It does not need to: the
`exception_slot` points at a slot in a frame, and the frame chain already
covers that. The handler chain exists for the unwinder alone.

## The contact points

Each one is a place where a change on one side can break the other.

### B1 — the exception must be rooted before anything else changes

The order is not negotiable:

```c
ctx->pending_exception = thrown;          /* now a root */
ctx->frame_depth = handler->frame_depth;  /* the throwing frames stop being roots */
ctx->unmanaged_depth = handler->unmanaged_depth;
restore_handler_chain(handler);
rt_longjmp(handler->jump, 1);
```

Swapping the first two lines opens a window where the throwing frames are no
longer roots and nothing else holds the exception (A5). Nothing can run in
that window — there is no safepoint poll and no allocation between them — and
that is exactly why the ordering has to be written down and asserted rather
than left to whoever edits it next. A function that allocates between the two
would reintroduce it silently.

**Invariant:** no safepoint poll, no allocation, and no call that can reach
either, between the store and the two restores.

### B2 — a frame must never be linked before its roots are valid

This is not exception-specific and it is already latent in the current code:
`cvm_frame_push` links the frame, and the generated code zeroes the slots
*after* it. Between those, a collection on another thread would walk slots
holding whatever the C stack had there.

The bounds check in the root walk is what makes this survivable rather than a
crash (A2) — a value outside the heap is rejected — but a stale stack value
that happens to land inside the heap is a false retention, and that is a leak
with no diagnostic.

The fix is ordering: zero the slots, then link. The exception work should not
proceed without it, because exception paths push and pop frames far more
often than straight-line code does.

### B3 — the collector must not need to know about unwinding

A stopped thread's C stack is in some intermediate state during an unwind.
**The collector never reads it.** It reads `frame_depth` and the slots below
it. So a thread that is halfway through an unwind is indistinguishable, to the
collector, from a thread that has simply returned from the functions above its
handler — which is exactly what it is.

This is a property of precise roots worth naming, because the alternative is
worse: a conservative collector would scan the C stack, see the frames the
unwinder has abandoned, and keep everything they mention alive. **A precise
collector is easier to combine with unwinding, not harder.**

The corollary is a requirement on the unwinder: **it must not poll inside its
window.** If it polled, the collector could stop the thread between the store
and the restores — a state that is consistent (B1) but must not be *allowed*
to occur while the handler chain is half-restored. Since the window contains
no function entry and no allocation, it does not poll. Stated as an invariant,
not as an observation.

### B4 — the managed/unmanaged depth is the dangerous one

Losing `frame_depth` is a GC imprecision. Losing `unmanaged_depth` is a hang
for the whole process: the thread is recorded as unmanaged, so the collector
stops waiting for it, so the world never stops, so **every** thread stops.
One thread's bug, everyone's deadlock.

So the restore is mandatory and must be in the same store sequence as
`frame_depth` (B1). And a handler may only be registered while managed: a
throw cannot unwind out of foreign code, because the C between has no landing
pad and would be skipped without running anything.

### B5 — the unwinder must not allocate before B1's first line

An unwinder that builds a stack trace, or allocates an exception wrapper, does
so **after** the exception is rooted. Before that point the only reference to
the exception is on a C stack that is about to stop being roots, and an
allocation is a collection point (A5).

### B6 — a handler inside the heap needs its pointers in a descriptor

A `try` that spans an `await` or a `yield` cannot keep its handler on the C
stack: the frame it belonged to is in a suspended state machine, in a heap
object. So a `CvmHandler` may live inside a heap object, and then
`exception_slot` is a pointer field of that object.

**The collector finds it the same way it finds any other pointer field:**
through the object's type descriptor. So the descriptor of any object that
embeds a handler must list `exception_slot` among its pointer fields. Forget
it and the exception the generator is holding is collected while the generator
is suspended — a bug that only shows up under a collection during a
suspension.

Stated as a rule: **a handler is either in a frame, in which case the frame
chain covers it, or in a heap object, in which case that object's descriptor
must cover it. There is no third case and no case where nothing covers it.**

### B7 — the finalizer thread runs user code

Finalizers run on their own managed thread. An exception escaping one must not
unwind into the runtime: the runtime's frames have no landing pad, and the
locks it holds (the finalizer queue, the registry) would be skipped. The
finalizer loop registers a handler at its top; an exception reaching it is a
runtime error reported and contained, not propagated.

### B8 — detach must retire the thread's handlers

A thread that exits with handlers registered leaves `handlers[0..depth)`
referring to frames that are gone. Nothing reads them (the chain is
per-thread), but a thread that exits *while another thread is stopped and
reading its context* is the same hazard as any other detach — it is handled by
the existing rule that a thread leaves the registry before its node is freed.
Worth stating so that the handler arrays are freed in the same place the node
is.

## How control reaches a landing pad

The contract above does not depend on the mechanism. A handler holds a
`landing_pad` address and an opaque `jump` area; how the first is reached and
what the second is for are the frontend's business. That is deliberate, and it
is what makes the table below a choice rather than an architecture.

| mechanism | cost when nothing throws | cost at a handler | registers correct? |
|---|---|---|---|
| `setjmp` / `longjmp` | none | ~20 registers saved, once per function that has a handler | **no — see below** |
| explicit return protocol | a branch per call | none | yes: unwinding is ordinary returning, so `cvm_frame_pop` runs and the chain is never inconsistent |
| CFI-driven (C++'s model) | none | none | yes |

### The register save has a time point, and setjmp's is the wrong one

This is the constraint that decides the whole question, and it is easy to
state wrongly. It is not "setjmp is slower". It is:

- **`setjmp` saves the register state as of the `setjmp` call** — that is, as
  of the entry to the try.
- **The landing pad needs the state as of the throw** — as of the call that
  led to it, in the frame that will catch.

If the try body modifies a value that lives in a callee-saved register, the
`longjmp` restores the value from *before* the modification. So the value the
catch sees is not the value the program had.

The C standard's rule — that a non-`volatile` local modified between `setjmp`
and `longjmp` has an indeterminate value — is a direct statement of this, not
a language quirk. **Every value live across a try therefore has to be in
memory**, because memory is the only place the two time points agree.

**CFI does not have the problem because it saves at the other time point:**
"where each register was spilled when this frame called the next". That is
exactly what the landing pad needs, so nothing has to be forced into memory.
C++ is not cleverer here; it uses a mechanism that does not lose that window.

So the honest shape of the choice is not "a table is a later optimisation".
The table fixes a *correspondence*, and the price of not having it is paid in
the quality of the generated code, on the path where nothing throws.

### What that costs, and how to make it explicit

With `setjmp`, the C compiler decides which values must go to memory: every
value live across the try, judged conservatively per function. **A JavaScript
try commonly spans a large region** — `try { a lot of code } catch` — so this
is the main path, not a corner.

There is a way to make the set to a set the frontend chose rather than one a
compiler guessed:

```c
/* try { A } catch (e) { B }, where A modifies x and y */
static void try_body_1(TryCtx *c) { /* A, touching only c->x and c->y */ }

/* caller */
struct { T x, y; } crossing = {x, y};
if (rt_jump_set(&h.jump)) goto landing;
try_body_1(&crossing);
x = crossing.x;  y = crossing.y;
```

Compiling the try body as its own function turns "which values cross the try"
into arguments at a call, which the frontend knows exactly. Values that do not
cross stay in registers, unaffected; values that do, go through memory because
the frontend put them there rather than because the C compiler could not keep
them anywhere else.

It does not fix the time point — the crossing values still go through memory.
It makes the price visible and proportional instead of blanket.

### Where the mechanism lives

`setjmp` is a libc function and `vm/` is freestanding, so it is reached
through `rt/`, which is the layer allowed to use libc:

```c
typedef struct RtJump { unsigned char opaque[RT_JUMP_SIZE]; } RtJump;
i32  rt_jump_set(RtJump* env);   /* 0 the first time, nonzero after a jump */
void rt_jump_to(RtJump* env);
```

**Use `sigsetjmp(env, 0)`, not `setjmp`.** Whether `setjmp` saves the signal
mask is implementation-defined, and where it does, it is a `sigprocmask`
system call — per handler, for a mask that cannot have changed inside a try.
`sigsetjmp(env, 0)` says not to, on every platform that has it.

**One `setjmp` per function that has a handler**, not one per try and not one
per execution of a try: a single `RtJump` can be jumped to repeatedly, and a
function with no handler registers none and saves nothing. That is what keeps
the cost proportional to the number of functions that can catch, which in most
programs is a small fraction of them.

## What this design refuses to do

**No fibers.** A stackful coroutine would mean a suspended frame is still on
*some* stack, and unwinding would have to reason about two stacks with
different rules. Stackless coroutines put the suspended frame in a heap
object, where it is covered by the same descriptor rule as everything else
(B6). The complexity is not avoided, it is moved somewhere the collector
already knows how to handle.

**No throwing out of foreign code.** A handler may only be registered while
managed, so a throw always has a landing pad in the frames it unwinds. A
frontend that calls into foreign code brackets the call with a handler that
converts an escaping exception into an error return.

**No pretending the register save is free.** The spill it forces is a real
cost on the path where nothing throws, and the mitigation above makes it
visible rather than removing it. A design that claimed otherwise would be
choosing the mechanism by how the table reads.

**No `finally` in the runtime.** `finally` runs on every exit path — normal,
throw, return, break, continue — and expressing that is the frontend's job. It
compiles to a cleanup routine and a landing pad; the runtime only provides the
landing pad. A runtime that tried to own `finally` would have to know the
frontend's control flow.

## The invariants, as assertions

Each of these is a one-line check that can be live in debug builds, which is
where they earn their keep — an exception bug that only appears under
concurrent load is not one to hunt by reading.

1. `cvm_throw` asserts the handler chain is non-empty, or takes the uncaught
   path. No unwinding to a handler that does not exist.
2. `cvm_throw` asserts the target handler's `unmanaged_depth` is one the
   current thread is actually at or below, so an unwind never crosses a
   foreign frame.
3. Between B1's store and its restores, a debug counter is set and both the
   safepoint poll and `cvm_alloc` assert it is clear. This is the window made
   visible instead of remembered.
4. After every unwind, `frame_depth` and `unmanaged_depth` are checked against
   the handler's saved values.
5. A thread's `frame_depth` is asserted nonzero-but-bounded at every
   collection, which catches an over-restore as well as an under-restore.

## The tests worth writing

Ordered by what they would catch, which is not the order they are easy to
write:

| test | catches |
|---|---|
| throw with a collection forced at every allocation point along the unwind | B1 and B5 — the exception's window |
| throw, then collect, then read the exception in the catch | A5 |
| throw with the collector already waiting for this thread to park | B3 |
| an exception escaping a finalizer | B7 |
| throw inside an `unmanaged` region, with a handler below it | B4 — the hang |
| a handler inside a suspended generator, with a collection during the suspension | B6 |
| a thread that exits with handlers registered | B8 |
| a value modified inside a try and read in the catch, with the try body large enough that the compiler must choose | the register time point — the one thing `setjmp` gets wrong on its own |

The first one is the whole design in one test: it walks the unwind window
looking for the collection point that was not accounted for.
