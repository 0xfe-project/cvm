# Exceptions: the contract with the collector

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

The first one is the whole design in one test: it walks the unwind window
looking for the collection point that was not accounted for.
