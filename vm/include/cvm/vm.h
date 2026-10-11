/*
 * vm/include/cvm/vm.h
 *
 * The runtime's entry points, as seen by a program.
 *
 * Generated code includes cvm/gc.h for the allocation path and the frame
 * protocol. This header is for the program's own setup and teardown: the
 * part a frontend emits once around everything else.
 *
 * ---------------------------------------------------------------------
 * Shape of a program
 * ---------------------------------------------------------------------
 *
 *     #include "cvm/vm.h"
 *     #include "cvm/gc.h"
 *
 *     int main(void) {
 *         if (!cvm_init(NULL)) return 1;   // NULL means the defaults
 *
 *         ... generated code runs here, allocating and pushing frames ...
 *
 *         cvm_shutdown();
 *         return 0;
 *     }
 *
 * cvm_init registers the calling thread, which is what lets it allocate.
 * A thread that has not registered cannot allocate, and cvm_alloc_slow
 * stops the process rather than writing through a null context — a
 * mis-registered thread is a frontend bug, and it is better to say so than
 * to corrupt the heap.
 */

#ifndef CVM_VM_H
#define CVM_VM_H

#include "base/types.h"
#include "cvm/gc.h"
#include "rt.h"
#include "stats.h"

/*
 * Ask the runtime to size the worker pool itself. A named constant because
 * zero already means something else — no workers at all — and the two are
 * very different requests.
 */
#define CVM_GC_WORKERS_DEFAULT ((u32)~0u)

typedef struct CvmOptions {
    /*
     * Heap sizes in bytes. Zero means "choose": the minimum becomes
     * CVM_MIN_HEAP_SIZE and the maximum becomes as much as the machine
     * will give, capped by CVM_MAX_HEAP_SIZE.
     */
    usize min_heap_size;
    usize max_heap_size;

    /*
     * Concurrent GC workers. Zero means none, in which case marking and
     * sweeping happen on whichever thread triggered them;
     * CVM_GC_WORKERS_DEFAULT means "as many as this machine and heap
     * justify".
     */
    u32 gc_workers;

    /*
     * Whether marking runs alongside the program rather than inside the
     * stop-the-world window.
     *
     * Off (the default) is the collector this tree has always had: the world
     * stops, marking finishes, the world resumes. Its pause scales with the
     * live set.
     *
     * On splits the stop in two. The first stops the world only long enough
     * to snapshot the roots and hand marking to the workers; the program
     * runs while marking drains the grey set, with the write barrier marking
     * what stores publish and new allocations starting life marked; the
     * second stop is short -- rescan the roots (frames changed while the
     * program ran), drain what that finds, nullify weak references, and
     * prepare the sweep. The pause is the roots and the tail, not the heap.
     *
     * Correctness on this path leans on the barrier: a program that stores a
     * pointer field directly rather than through cvm_store_field is silently
     * wrong when this is on. See the contract on cvm_store_field in
     * cvm/gc.h.
     */
    bool concurrent_mark;

    /*
     * Whether to run finalizers on their own thread.
     *
     * Off means finalizers are only run by an explicit
     * cvm_finalizer_drain_now or at shutdown, which is useful for a
     * program that wants deterministic control and for tests. On is what
     * a real program wants.
     */
    bool finalizer_thread;

    /*
     * Where the event log goes, or NULL for no log. The buffer itself
     * lives in the heap and is flushed by cvm_shutdown.
     */
    RtChannel* stats_channel;

    /*
     * Log level name ("debug", "info", "warn", "error", "none"). An empty
     * str means "read the environment".
     */
    str log_level;
} CvmOptions;

/*
 * Bring the runtime up. `options` may be NULL for the defaults.
 *
 * Registers the calling thread. Returns false if the heap could not be
 * reserved, in which case nothing is usable and the caller should stop.
 */
bool cvm_init(CvmOptions const* options);

/*
 * Tear down. Runs any pending finalizers, so a clean exit does not leak
 * the resources the program was holding.
 */
void cvm_shutdown(void);

/*
 * Collect now, and finish the sweep before returning.
 *
 * Allocation-triggered collections deliberately leave the sweep running
 * alongside the program; this is the settled form, which is what a caller
 * asking for a collection explicitly is asking for.
 */
void cvm_collect(void);

/* ------------------------------------------------------------------ */
/* blocking calls, from managed code                                   */
/* ------------------------------------------------------------------ */

/*
 * Wait for a thread to finish, declaring that the wait is not managed
 * execution.
 *
 * This exists because the obvious call is unsafe, and the failure is a
 * deadlock rather than a crash. A thread blocked in a join is not executing
 * managed code and is not going to reach a safepoint, but a collector
 * cannot tell that from the outside: it sees a managed thread that is not
 * parked, and waits for it. Meanwhile the thread waits for the thread it is
 * joining, which may itself be the one collecting. Both wait forever.
 *
 * So the runtime brackets the wait with the unmanaged state, which is
 * exactly the declaration the FFI protocol asks of generated code around
 * any foreign call. Any other blocking operation called from managed code
 * needs the same treatment:
 *
 *     cvm_thread_enter_unmanaged();
 *     ... blocking call ...
 *     cvm_thread_leave_unmanaged();
 *
 * `rt_thread_join` is safe to call only when the caller is already
 * unmanaged, or when no other thread can be collecting. Prefer this.
 */
bool cvm_thread_join(RtThread thread);

/* ------------------------------------------------------------------ */
/* what the runtime is doing                                           */
/* ------------------------------------------------------------------ */

typedef struct CvmVmStats {
    u64 collections;
    u64 collection_ns;

    /*
     * The longest single stop-the-world window so far, in nanoseconds.
     *
     * Distinct from collection_ns / collections, which is an average and
     * therefore hides the thing a program with a deadline actually cares
     * about. One 40 ms pause and forty 1 ms pauses have the same total.
     */
    u64 pause_max_ns;

    /*
     * Live objects.
     *
     * Counted on demand rather than tracked, because a running count would
     * have to be updated on the allocation fast path -- the one place that
     * has to stay a few instructions -- and on the sweep, which is the only
     * thing that knows an object died.
     *
     * Counted means counted: the call walks one metadata byte per sixteen
     * bytes of the heap's *reservation*, not of its live set. A program with
     * a 512 MiB maximum pays 32 million byte reads per call, and the walk is
     * the dominant cost of cvm_vm_stats by a wide margin -- 57% of the
     * benchmark process's samples, measured -- even though nothing in it is
     * slow.
     *
     * So: a call per collection is nothing, a call per allocation is the
     * program, and a call from a timer is a decision about that reservation
     * size. The rest of the struct is a handful of loads.
     */
    u64 objects_live;

    /* Blocks the heap has brought into use, and how many of those the
     * allocator currently has free. */
    u32 blocks_total;
    u32 blocks_free;

    usize heap_size;
    usize heap_max;
    u32 threads_registered;
    u64 finalizers_run;
    u64 finalizers_dropped;
} CvmVmStats;

CvmVmStats cvm_vm_stats(void);

/* The heap, for a caller that needs it. Not part of the generated-code
 * ABI; generated code never sees a heap pointer. */
struct CvmHeap* cvm_vm_heap(void);

#endif /* CVM_VM_H */
