/*
 * vm/include/cvm/intern.h
 *
 * A table of strings that are the same string.
 *
 * ---------------------------------------------------------------------
 * What it is for
 * ---------------------------------------------------------------------
 *
 * Interning turns "same text" into "same pointer". A comparison becomes a
 * pointer comparison, a hash is computed once per distinct text, and a table
 * keyed by a string can hash by the address it was given rather than by walking
 * it.
 *
 * ---------------------------------------------------------------------
 * What it is not
 * ---------------------------------------------------------------------
 *
 * quickjs's atom table, which is the obvious reference here, interns *forever*.
 * Every distinct string that was ever an atom stays in the table for the life of
 * the process, and in a long-running program that set is unbounded even though
 * the working set is not: a key that was used once, a name constructed for one
 * lookup, is resident for good.
 *
 * So this holds its keys *weakly*. A string is in the table for as long as
 * something else holds it, and when nothing does, the entry goes away. The
 * bound is the working set, not the history.
 *
 * ---------------------------------------------------------------------
 * What that costs: four rules, each of which is a leak if broken
 * ---------------------------------------------------------------------
 *
 * 1. A dead entry must be *deleted*, not merely noticed. Nulling the key inside
 *    the slot leaves the bucket marked occupied forever, so the table fills up
 *    with dead buckets, grows to make room for them, and never comes back.
 *    cvm_intern_prune does the deletion.
 *
 * 2. The bucket array is a GC object, so clearing a slot releases the wrapper
 *    object that was in it. The wrapper is what holds the key weakly: leave it
 *    referenced and each interned string costs a permanent wrapper.
 *
 * 3. The capacity must be able to come back down. Pruning empties buckets;
 *    without giving the memory back the array stays the size of the busiest
 *    moment the table ever had, which is a leak with a different shape.
 *    cvm_intern_prune does both, since it rebuilds the arrays either way.
 *
 * 4. Cleaning up cannot happen during the collection that killed the keys. The
 *    nullify phase runs with the world stopped and allocation forbidden, and
 *    deleting an entry can resize the table, which allocates. So the cleanup is
 *    a mutator-phase call: cvm_intern_prune, by whoever owns the table, after a
 *    collection. This is why it is an API and not a hook.
 */

#ifndef CVM_INTERN_H
#define CVM_INTERN_H

#include "base/types.h"
#include "cvm/hash.h"
#include "cvm/str.h"

/* A new table. Its bucket array appears on the first interned string. */
CvmHashMap* cvm_intern_new(void);

/*
 * The canonical string for `text`, which is `text` itself if it was not there.
 *
 * The result is the one to keep. It is the string that was interned first, which
 * may be a different object from the argument, and the argument is not kept
 * alive by this call: the table holds its keys weakly, so what keeps an interned
 * string alive is the caller holding it.
 *
 * Which also means the answer to "is this string interned" is only yes while
 * someone holds it — that is the design, not a gap: an interned string nobody
 * holds is nothing.
 */
CvmString* cvm_intern(CvmHashMap* t, CvmString* text);

/* The canonical string for `text`, or NULL if it is not interned. Does not
 * insert. */
CvmString* cvm_intern_lookup(CvmHashMap* t, CvmString* text);

/*
 * Remove every entry whose string has died, and report how many.
 *
 * Called by the mutator, after a collection: see rule 4 above.
 *
 * Dropping and resizing are one operation rather than two, because they are the
 * same work: an entry cannot be removed in place, since closing a deleted
 * entry's probe chain moves entries a scan has not reached yet behind it (see
 * the note on cvm_map_compact), so removing anything means rebuilding the
 * arrays — and rebuilding them is also when they get sized to fit. A separate
 * shrink would rebuild a second time to ask the same question.
 */
usize cvm_intern_prune(CvmHashMap* t);

/* Entries in the table, including any whose string has already died and has not
 * been pruned yet. */
usize cvm_intern_count(CvmHashMap* t);

#endif /* CVM_INTERN_H */
