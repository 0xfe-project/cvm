/*
 * vm/include/cvm/atom.h
 *
 * A table of strings that are the same string, shared between threads.
 *
 * ---------------------------------------------------------------------
 * Why this is not just cvm_intern
 * ---------------------------------------------------------------------
 *
 * An intern table (cvm/intern.h) is a hash map with weak keys, and it is
 * single-threaded. This is the same table cut into shards, one lock per shard, so
 * that threads working on different text do not contend — and so that a lookup
 * does not take a lock at all.
 *
 * The three pieces:
 *
 *   - the shard. A shard is one intern table and one mutex. A string's content
 *     hash picks the shard, and everything that writes takes that shard's lock.
 *
 *   - the read. Both operations take the shard's lock today, and the plan is that
 *     the lookup will not: see the note in vm/atom.c for why it does not yet, and
 *     for the two shapes the fix can take. The kernel is already prepared for it —
 *     the occupancy bitmap and the key slots are published atomically, and a probe
 *     takes one snapshot of the arrays — so what is missing is the lifetime
 *     argument, not the mechanism.
 *
 *   - the insert, which does take the lock, and looks again after it has it. That
 *     second look is what keeps two entries for one text from existing: a lookup
 *     that missed a concurrent insert arrives at the insert path, and the insert
 *     path re-checks under the lock.
 *
 * ---------------------------------------------------------------------
 * The four rules from the intern table still apply
 * ---------------------------------------------------------------------
 *
 * Every shard is a weak-keyed table, so the four rules in cvm/intern.h hold per
 * shard: a dead entry must be deleted and not merely noticed, the bucket array is
 * a GC object so clearing a slot releases the wrapper, the capacity must be able
 * to come back down, and the cleanup cannot run during the collection that killed
 * the keys. The last one is why pruning is a call the table's owner makes after a
 * collection (cvm_atom_prune) rather than a hook inside the collector.
 *
 * ---------------------------------------------------------------------
 * What is deliberately absent
 * ---------------------------------------------------------------------
 *
 * A global table. The runtime does not have one yet, and a table nobody asks for
 * is a table nobody prunes: an atom table that is never pruned is quickjs's, which
 * interns forever. When something needs a process-wide table, it creates one and
 * owns its pruning, which is the same contract the intern table has.
 */

#ifndef CVM_ATOM_H
#define CVM_ATOM_H

#include "base/types.h"
#include "cvm/str.h"

typedef struct CvmAtomTable CvmAtomTable;

/* Enough shards that a handful of threads rarely land on the same one. A power
 * of two, which is what lets the shard be a bit mask. */
#define CVM_ATOM_SHARDS_DEFAULT ((usize)8u)

/*
 * A table with `shards` shards, which must be a power of two. Each shard's bucket
 * array appears when that shard is first written to, so an idle shard costs a
 * table entry and nothing else.
 */
CvmAtomTable* cvm_atom_table_new(usize shards);

/*
 * The canonical string for `text`: `text` itself if the table had not seen it, the
 * string it saw first otherwise.
 *
 * Takes the shard's lock, and takes it again after finding the entry, which is
 * what makes "one entry per text" hold when several threads ask for the same text
 * at once. The result is the one to keep: the table holds its strings weakly, so
 * what keeps one alive is a caller holding it. The argument must be rooted by the
 * caller across this call, which allocates.
 */
CvmString* cvm_atom_intern(CvmAtomTable* t, CvmString* text);

/*
 * The canonical string for `text`, or NULL.
 *
 * A string this returns is one that was in the table when the lookup ran; the
 * caller has to root it before its next allocation, like any other value that
 * comes back from a call. Does not insert.
 */
CvmString* cvm_atom_lookup(CvmAtomTable const* t, CvmString* text);

/*
 * Drop every entry whose string has died, in every shard, and report how many.
 *
 * Called by whoever owns the table, after a collection: the collector cannot do
 * this itself, because it runs with allocation forbidden and this rebuilds bucket
 * arrays. Each shard's prune is the intern table's, which drops the dead entries
 * and sizes the arrays for what is left in one operation.
 */
usize cvm_atom_prune(CvmAtomTable* t);

/* Entries across the shards, including ones whose string has died and has not
 * been pruned. Takes each shard's lock in turn, so the answer is a snapshot and
 * not a sum that races with the writers. */
usize cvm_atom_count(CvmAtomTable* t);

usize cvm_atom_shards(CvmAtomTable const* t);

/* Buckets across the shards. What a table costs, as opposed to what it holds:
 * a table that has been busy and then pruned has to come back down to the buckets
 * its entries need, and this is the number that says whether it did. */
usize cvm_atom_buckets(CvmAtomTable* t);

#endif /* CVM_ATOM_H */
