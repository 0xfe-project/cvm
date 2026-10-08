/*
 * vm/include/cvm/json.h
 *
 * A JSON document, in the heap, and the two functions that move it to and from
 * text.
 *
 * ---------------------------------------------------------------------
 * The value model
 * ---------------------------------------------------------------------
 *
 * Six kinds, and each of them is a GC object of its own:
 *
 *     null    a singleton value, with no payload
 *     bool    one flag
 *     number  an f64
 *     string  a CvmString, so its units and its hash are the ones the rest of
 *             the runtime uses
 *     array   a CvmRefArray of values
 *     object  members in insertion order, with an index built on demand
 *
 * ---------------------------------------------------------------------
 * Why an object is not a hash map
 * ---------------------------------------------------------------------
 *
 * The obvious representation for a JSON object here is CvmHashMap, and it is
 * the wrong one: an open-addressed map has no order, so printing a document
 * would reorder its keys, and a document that goes out in a different order
 * from the one it came in is not round-tripping even though it is "equal".
 * Anything a human diffs, or a test compares as text, would churn.
 *
 * So members are a list, in the order they arrived, and lookup is served by an
 * index over that list: an array of member pointers sorted by the key's cached
 * hash, built on the first lookup after a change and then reused. Lookup is a
 * binary search on the hash followed by a comparison of the one or two keys that
 * share it. Insertion is a list append and invalidates the index, which is the
 * trade ljson makes and the reason its object layout survives a round trip.
 *
 * A repeated key is *not* two members: putting a key that is already there
 * replaces its value in place. JSON says nothing about duplicates and every
 * implementation that matters keeps the last, which this gets for free without
 * ever having a document with two answers to one question.
 *
 * ---------------------------------------------------------------------
 * Text
 * ---------------------------------------------------------------------
 *
 * Parsing is strict: the grammar is RFC 8259's, which means no comments, no
 * trailing commas, no single quotes, no NaN or Infinity, and no unquoted keys.
 * A document that is not JSON fails with the offset it failed at, because the
 * only useful thing to say about a syntax error is where it is.
 *
 * Strings are UTF-8, and a lone surrogate has exactly one spelling: `\uXXXX`.
 * Writing it as UTF-8 would be WTF-8, which this parser refuses, so a string that
 * holds one prints as the escape it came in as — and the round trip is exact in
 * both directions because of it.
 *
 * Nesting is bounded. A recursive descent parser has no other limit, and a
 * document of ten thousand open brackets is otherwise a stack overflow that
 * looks like a crash in someone else's code.
 */

#ifndef CVM_JSON_H
#define CVM_JSON_H

#include "base/list.h"
#include "base/types.h"
#include "cvm/array.h"
#include "cvm/builder.h"
#include "cvm/gc.h"
#include "cvm/str.h"

/* Deep enough for anything a person writes; shallow enough that the parser's
 * own frames cannot exhaust a small stack. */
#define CVM_JSON_MAX_DEPTH 128

typedef enum CvmJsonKind {
    CVM_JSON_NULL = 0,
    CVM_JSON_BOOL,
    CVM_JSON_NUMBER,
    CVM_JSON_STRING,
    CVM_JSON_ARRAY,
    CVM_JSON_OBJECT,
} CvmJsonKind;

typedef struct CvmJson CvmJson;
typedef struct CvmJsonMember CvmJsonMember;

/*
 * One member of an object: the link the list is made of, the key, the value, and
 * the key's hash.
 *
 * The links are traced like any other field, which is what makes an intrusive
 * list legal in the heap at all: the descriptor names them, so a member is
 * reachable through its neighbours and the whole chain survives a collection.
 * Forgetting them would leave an object whose first member is alive and whose
 * rest were swept.
 */
struct CvmJsonMember {
    CvmList link;
    CvmString* key;
    CvmJson* value;
    u32 hash; /* cached: cvm_string_hash(key) */
};

struct CvmJson {
    /* STRING */
    CvmString* string;
    /* ARRAY: the values, in order */
    CvmRefArray* array;
    /* OBJECT: the member list's sentinel, which lives here rather than being a
     * NULL head so that appending and iterating are the same code for an empty
     * object as for a full one. */
    CvmList members;
    /* OBJECT: the lookup index, built on demand; NULL when absent or stale. */
    CvmRefArray* index;
    /* NUMBER */
    f64 number;
    u32 kind; /* CvmJsonKind */
    /* ARRAY: element count. OBJECT: member count. */
    u32 size;
    /* BOOL */
    bool boolean;
    /* Whether `index` reflects the member list. */
    bool index_ready;
};

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

CvmJson* cvm_json_null(void);
CvmJson* cvm_json_bool(bool value);
CvmJson* cvm_json_number(f64 value);
/* Takes a reference to `s`; the string is shared, not copied. */
CvmJson* cvm_json_string(CvmString* s);
CvmJson* cvm_json_array(void);
CvmJson* cvm_json_object(void);

/* ------------------------------------------------------------------ */
/* arrays                                                              */
/* ------------------------------------------------------------------ */

/*
 * Append a value. False only if the array could not grow.
 *
 * The result must be taken for objects too — see the note in cvm/array.h — so
 * this returns the array it appended to, which is the argument unless growth
 * happened.
 */
CvmJson* cvm_json_array_push(CvmJson* array, CvmJson* value);

CVM_INLINE usize cvm_json_array_length(CvmJson const* array) {
    return array->size;
}

/* The element at `index`. No bounds check: the caller has the length. */
CVM_INLINE CvmJson* cvm_json_array_at(CvmJson const* array, usize index) {
    cvm_assert(index < array->size);
    return (CvmJson*)cvm_ref_array_get(array->array, index);
}

/* ------------------------------------------------------------------ */
/* objects                                                             */
/* ------------------------------------------------------------------ */

/*
 * Set `key`, replacing the value if the key is already there, and return the
 * object (the argument: an object never grows in place, but the index moves).
 *
 * The key is shared, not copied, and hashed once here — which is where the
 * cached hash in CvmString pays for itself, since a document's keys are looked
 * up far more often than they are inserted.
 */
CvmJson* cvm_json_object_put(CvmJson* object, CvmString* key, CvmJson* value);

/* The value under `key`, or NULL. A JSON value can be null, so the question
 * "is it there" is cvm_json_object_has. */
CvmJson* cvm_json_object_get(CvmJson* object, CvmString const* key);

bool cvm_json_object_has(CvmJson* object, CvmString const* key);

CVM_INLINE usize cvm_json_object_size(CvmJson const* object) {
    return object->size;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

CVM_INLINE CvmJsonKind cvm_json_kind(CvmJson const* v) {
    return (CvmJsonKind)v->kind;
}

CVM_INLINE bool cvm_json_as_bool(CvmJson const* v) {
    cvm_assert(v->kind == (u32)CVM_JSON_BOOL);
    return v->boolean;
}

CVM_INLINE f64 cvm_json_as_number(CvmJson const* v) {
    cvm_assert(v->kind == (u32)CVM_JSON_NUMBER);
    return v->number;
}

CVM_INLINE CvmString* cvm_json_as_string(CvmJson const* v) {
    cvm_assert(v->kind == (u32)CVM_JSON_STRING);
    return v->string;
}

/*
 * Members in insertion order, which is the order the text had them in.
 *
 *     for (CvmJsonMember const* m = cvm_json_object_first(obj); m != NULL;
 *          m = cvm_json_object_next(obj, m)) { ... }
 *
 * The object is passed back in rather than being remembered by the member,
 * because the end of the list is the object's own sentinel: reaching it is how
 * the walk knows it is done, and a member does not know where its object is.
 */
CvmJsonMember const* cvm_json_object_first(CvmJson const* object);
CvmJsonMember const* cvm_json_object_next(CvmJson const* object,
                                          CvmJsonMember const* member);

CVM_INLINE CvmString* cvm_json_member_key(CvmJsonMember const* member) {
    return member->key;
}

CVM_INLINE CvmJson* cvm_json_member_value(CvmJsonMember const* member) {
    return member->value;
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */

/*
 * Parse `text`. NULL if it is not JSON, with `*error_offset` (may be NULL) set
 * to the offset of the offending byte — or to the length, when the document
 * simply ended early, which is the one case where "here" means "after here".
 */
CvmJson* cvm_json_parse(str text, usize* error_offset);

/*
 * Append `value` as text. False if the builder could not grow.
 *
 * A number that is not finite prints as `null`, because JSON has no spelling for
 * Infinity or NaN and every other option produces text that no parser accepts.
 */
bool cvm_json_print(CvmJson const* value, CvmStrBuf* out);

#endif /* CVM_JSON_H */
