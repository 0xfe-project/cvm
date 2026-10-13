/*
 * vm/json.c
 *
 * JSON values, and the text that moves them. See include/cvm/json.h for the
 * value model, for why an object is a list with an index rather than a hash map,
 * and for what is deliberately strict.
 *
 * Two things run through this file and are worth naming before the code:
 *
 *   - every level of the parser holds a container across allocations, because
 *     parsing a child allocates. Each level therefore pushes its own frame, and
 *     the frame protocol is what makes a partially built document survivable.
 *
 *   - the object index is built on demand and invalidated by a change. Its sort
 *     uses off-heap scratch, and nothing in that path allocates in the heap,
 *     which is what makes holding raw member pointers in off-heap memory safe:
 *     no collection can run while it exists.
 */

#include "cvm/json.h"

#include "base/assert.h"
#include "base/types.h"
#include "base/utf8.h"
#include "cvm/array.h"
#include "cvm/builder.h"
#include "cvm/gc.h"
#include "cvm/str.h"
#include "mem.h"
#include "rt.h"
#include "str/dtoa.h"

/* ------------------------------------------------------------------ */
/* descriptors                                                         */
/* ------------------------------------------------------------------ */

/*
 * One value has three pointer fields that are ever a pointer, and a member list
 * whose links are two more. The links matter as much as the payload: a member's
 * neighbours are only reachable through them, so a descriptor that named just
 * `key` and `value` would keep the first member of an object alive and let the
 * rest be swept.
 */
static CvmPtrField const json_fields[] = {
    {.offset = offsetof(CvmJson, string)},
    {.offset = offsetof(CvmJson, array)},
    {.offset = offsetof(CvmJson, members) + offsetof(CvmList, prev)},
    {.offset = offsetof(CvmJson, members) + offsetof(CvmList, next)},
    {.offset = offsetof(CvmJson, index)},
};

static CvmTypeDescriptor const cvm_type_json = {
    .size = sizeof(CvmJson),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 5,
    .num_embed_fields = 0,
    .ptr_fields = json_fields,
    .embed_fields = NULL,
};

static CvmPtrField const member_fields[] = {
    {.offset = offsetof(CvmJsonMember, link) + offsetof(CvmList, prev)},
    {.offset = offsetof(CvmJsonMember, link) + offsetof(CvmList, next)},
    {.offset = offsetof(CvmJsonMember, key)},
    {.offset = offsetof(CvmJsonMember, value)},
};

static CvmTypeDescriptor const cvm_type_member = {
    .size = sizeof(CvmJsonMember),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 4,
    .num_embed_fields = 0,
    .ptr_fields = member_fields,
    .embed_fields = NULL,
};

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

static CvmJson* json_alloc(CvmJsonKind kind) {
    CvmJson* v = cvm_alloc(&cvm_type_json);
    v->kind = (u32)kind;
    /* An empty object's list is a sentinel that points at itself, so appending
     * and iterating need no special case for "no members yet". */
    cvm_list_init(&v->members);
    return v;
}

CvmJson* cvm_json_null(void) {
    return json_alloc(CVM_JSON_NULL);
}

CvmJson* cvm_json_bool(bool value) {
    CvmJson* v = json_alloc(CVM_JSON_BOOL);
    v->boolean = value;
    return v;
}

CvmJson* cvm_json_number(f64 value) {
    CvmJson* v = json_alloc(CVM_JSON_NUMBER);
    v->number = value;
    return v;
}

CvmJson* cvm_json_int(i64 value) {
    CvmJson* v = json_alloc(CVM_JSON_INT);
    v->integer = value;
    return v;
}

CvmJson* cvm_json_string(CvmString* s) {
    CvmJson* v = json_alloc(CVM_JSON_STRING);
    v->string = s;
    return v;
}

CvmJson* cvm_json_array(void) {
    CvmJson* v = json_alloc(CVM_JSON_ARRAY);
    v->array = cvm_ref_array_new(0);
    return v;
}

CvmJson* cvm_json_object(void) {
    return json_alloc(CVM_JSON_OBJECT);
}

/* ------------------------------------------------------------------ */
/* arrays                                                              */
/* ------------------------------------------------------------------ */

CvmJson* cvm_json_array_push(CvmJson* array, CvmJson* value) {
    cvm_assert(array->kind == (u32)CVM_JSON_ARRAY);

    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = array;
    f_roots[1] = value;

    CvmJson* a = (CvmJson*)f_roots[0];
    a->array = cvm_ref_array_push(a->array, f_roots[1]);
    a->size = (u32)cvm_ref_array_length(a->array);

    cvm_frame_pop(&f_frame);
    return a;
}

/* ------------------------------------------------------------------ */
/* the object index                                                    */
/* ------------------------------------------------------------------ */

CVM_INLINE u32 member_hash(CvmJson const* object, usize i) {
    return ((CvmJsonMember*)cvm_ref_array_get(object->index, i))->hash;
}

/*
 * Sort members by their key's hash.
 *
 * Bottom-up merge sort with off-heap scratch, rather than insertion sort, which
 * is the obvious choice for the small objects a hand-written document has and
 * the wrong one for the object a generated document can: quadratic on a hundred
 * thousand keys is a hang, and a parser is exactly where that input arrives.
 *
 * Nothing here allocates in the heap, so no collection can run while the
 * off-heap scratch holds member pointers the collector cannot see.
 */
static void merge_span(opaque_mut* dst, opaque_mut const* left, usize left_n,
                       opaque_mut const* right, usize right_n) {
    usize i = 0;
    usize j = 0;
    usize k = 0;
    while (i < left_n && j < right_n) {
        u32 a = ((CvmJsonMember*)left[i])->hash;
        u32 b = ((CvmJsonMember*)right[j])->hash;
        /* <= keeps equal hashes in insertion order, so a lookup that walks the
         * equal run walks the list's order. */
        dst[k++] = (a <= b) ? left[i++] : right[j++];
    }
    while (i < left_n) {
        dst[k++] = left[i++];
    }
    while (j < right_n) {
        dst[k++] = right[j++];
    }
}

static bool sort_index(CvmJson* object) {
    usize n = object->size;
    if (n < 2u) {
        return true;
    }

    opaque_mut* scratch = rt_mem_alloc(n * sizeof(opaque_mut));
    if (scratch == NULL) {
        return false;
    }

    opaque_mut* items = (opaque_mut*)object->index->items;
    for (usize width = 1u; width < n; width *= 2u) {
        for (usize i = 0; i < n; i += 2u * width) {
            usize left_n = width;
            if (i + left_n > n) {
                left_n = n - i;
            }
            usize right_n = width;
            if (i + width + right_n > n) {
                right_n = (i + width < n) ? n - i - width : 0u;
            }
            if (right_n == 0u) {
                for (usize k = i; k < i + left_n; k++) {
                    scratch[k] = items[k];
                }
                continue;
            }
            merge_span(scratch + i, items + i, left_n, items + i + width, right_n);
        }
        for (usize k = 0; k < n; k++) {
            items[k] = scratch[k];
        }
    }

    rt_mem_free(scratch);
    return true;
}

/*
 * Build the index from the member list, in list order.
 *
 * The caller has the object rooted: this allocates the index array and the
 * scratch.
 */
static bool object_build_index(CvmJson* object) {
    CvmRefArray* index = cvm_ref_array_new(object->size);
    object->index = index; /* rooted by the object from here on */

    usize i = 0;
    cvm_list_for_each_entry(m, CvmJsonMember, &object->members, link) {
        cvm_ref_array_set(index, i++, m);
    }
    cvm_assert(i == object->size);

    if (!sort_index(object)) {
        object->index = NULL;
        return false;
    }
    object->index_ready = true;
    return true;
}

static CvmJsonMember* index_find(CvmJson* object, CvmString const* key, u32 hash) {
    usize lo = 0;
    usize hi = object->size;

    /* First member whose hash is not less than the one wanted. */
    while (lo < hi) {
        usize mid = lo + (hi - lo) / 2u;
        if (member_hash(object, mid) < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    /* Then the run of equal hashes, compared by content: the hash only chose the
     * neighbourhood. */
    for (usize i = lo; i < object->size && member_hash(object, i) == hash; i++) {
        CvmJsonMember* m = (CvmJsonMember*)cvm_ref_array_get(object->index, i);
        if (cvm_string_equal(m->key, key)) {
            return m;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* objects                                                             */
/* ------------------------------------------------------------------ */

static CvmJsonMember* object_find(CvmJson* object, CvmString const* key) {
    if (object->size == 0u) {
        return NULL;
    }
    u32 hash = cvm_string_hash((CvmString*)key);
    if (!object->index_ready && !object_build_index(object)) {
        return NULL;
    }
    return index_find(object, key, hash);
}

/*
 * Append without checking for a duplicate: the parser's path, where every
 * member is new and the check is the cost that made a thousand-member object
 * take eleven milliseconds to parse.
 *
 * The duplicate is resolved when the object closes (object_dedup below), not
 * on the next insert: the find that would resolve it is what rebuilds the
 * index, and rebuilding per member is the O(n²) the bench found.
 */
static void object_append(CvmJson* object, CvmString* key, CvmJson* value) {
    CvmJsonMember* member = cvm_alloc(&cvm_type_member);
    member->key = key;
    member->value = value;
    member->hash = cvm_string_hash(key);
    cvm_list_add_tail(&member->link, &object->members);
    object->size++;
    object->index_ready = false;
}

/*
 * Resolve duplicate keys: keep the last occurrence, as the test and every
 * implementation that matters does. Called once, when the object's parse
 * closes, rather than per member.
 *
 * The walk is front to back along the member list, and for each member the
 * rest of the list is checked for the same key. The one that is found later
 * is the one that stays — "last wins" — and the one being walked is the one
 * that goes. The scan is O(n) when there are no duplicates, which is the
 * common case and the reason the index is not used here: the index is sorted
 * by hash, and the two members with one key are adjacent in it, but which of
 * them is the later insert is not something the sort knows.
 */
static void object_dedup(CvmJson* object) {
    if (object->size <= 1u) {
        return;
    }
    CvmJsonMember* m = (object->members.next == &object->members)
                           ? NULL
                           : cvm_list_entry(object->members.next, CvmJsonMember, link);
    while (m != NULL) {
        CvmJsonMember* next = NULL;
        CvmList* nl = m->link.next;
        if (nl != &object->members) {
            next = cvm_list_entry(nl, CvmJsonMember, link);
        }
        bool deleted = false;
        for (CvmJsonMember* later = next; later != NULL;) {
            if (later->hash == m->hash && cvm_string_equal(later->key, m->key)) {
                /* A later member has the same key: its value is the one that
                 * stays, and this member keeps its position. */
                m->value = later->value;
                cvm_list_del(&later->link);
                object->size--;
                /* The scan restarts from the same `m`: there may be more
                 * duplicates after the one just removed. */
                deleted = true;
                break;
            }
            CvmList* ll = later->link.next;
            later = (ll == &object->members) ? NULL
                                             : cvm_list_entry(ll, CvmJsonMember, link);
        }
        if (!deleted) {
            m = next;
        }
    }
    object->index_ready = false;
}

CvmJson* cvm_json_object_put(CvmJson* object, CvmString* key, CvmJson* value) {
    cvm_assert(object->kind == (u32)CVM_JSON_OBJECT);
    cvm_assert(key != NULL);

    CVM_FRAME_DECL(f, 3);
    cvm_frame_push(&f_frame, f_roots, 3);
    f_roots[0] = object;
    f_roots[1] = key;
    f_roots[2] = value;

    CvmJson* o = (CvmJson*)f_roots[0];

    CvmJsonMember* existing = object_find(o, (CvmString*)f_roots[1]);
    if (existing != NULL) {
        /*
         * Replaced in place, so a repeated key never becomes two members. The
         * index stays valid: the key did not change, and neither did its hash,
         * so the sorted order is still the right one.
         */
        existing->value = (CvmJson*)f_roots[2];
        cvm_frame_pop(&f_frame);
        return o;
    }

    CvmJsonMember* member = cvm_alloc(&cvm_type_member);
    member->key = (CvmString*)f_roots[1];
    member->value = (CvmJson*)f_roots[2];
    member->hash = cvm_string_hash(member->key);

    cvm_list_add_tail(&member->link, &o->members);
    o->size++;
    /* A new key can go anywhere in the sorted order, and finding where is the
     * same work as rebuilding: the next lookup does it once. */
    o->index_ready = false;

    cvm_frame_pop(&f_frame);
    return o;
}

CvmJson* cvm_json_object_get(CvmJson* object, CvmString const* key) {
    cvm_assert(object->kind == (u32)CVM_JSON_OBJECT);
    cvm_assert(key != NULL);

    /* The index may have to be built, which allocates, so the object is rooted
     * across the call even though this function only reads. */
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = object;
    f_roots[1] = (opaque_mut)(CvmString*)key;

    CvmJsonMember* found = object_find((CvmJson*)f_roots[0], (CvmString*)f_roots[1]);

    cvm_frame_pop(&f_frame);
    return found == NULL ? NULL : found->value;
}

bool cvm_json_object_has(CvmJson* object, CvmString const* key) {
    cvm_assert(object->kind == (u32)CVM_JSON_OBJECT);
    cvm_assert(key != NULL);

    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = object;
    f_roots[1] = (opaque_mut)(CvmString*)key;

    bool found = object_find((CvmJson*)f_roots[0], (CvmString*)f_roots[1]) != NULL;

    cvm_frame_pop(&f_frame);
    return found;
}

CvmJsonMember const* cvm_json_object_first(CvmJson const* object) {
    cvm_assert(object->kind == (u32)CVM_JSON_OBJECT);
    if (cvm_list_empty(&object->members)) {
        return NULL;
    }
    return cvm_list_entry(cvm_list_first((CvmList*)&object->members), CvmJsonMember,
                          link);
}

CvmJsonMember const* cvm_json_object_next(CvmJson const* object,
                                          CvmJsonMember const* member) {
    /* The list is circular, and its head is the object's sentinel — so the end
     * of the walk is the moment a member's successor *is* that sentinel, which
     * is a question only the object can answer. */
    CvmList* next = member->link.next;
    if (next == (CvmList*)&object->members) {
        return NULL;
    }
    return cvm_list_entry(next, CvmJsonMember, link);
}

/* ------------------------------------------------------------------ */
/* printing                                                            */
/* ------------------------------------------------------------------ */

static bool printable_cstr(CvmStrBuf* out, char const* text) {
    usize n = 0;
    while (text[n] != '\0') {
        n++;
    }
    str s = {.ptr = (u8 const*)text, .len = n};
    return cvm_str_buf_put_bytes(out, s);
}

static bool print_hex4(CvmStrBuf* out, u32 value) {
    static char const digits[] = "0123456789abcdef";
    if (!printable_cstr(out, "\\u")) {
        return false;
    }
    for (u32 shift = 12u;; shift -= 4u) {
        if (!cvm_str_buf_put_unit(out, (u32)(u8)digits[(value >> shift) & 0xFu])) {
            return false;
        }
        if (shift == 0u) {
            break;
        }
    }
    return true;
}

/*
 * A code point as UTF-8 bytes.
 *
 * Not cvm_str_buf_put_rune: a builder is a *string* buffer, whose units are 8 or
 * 16 bits of characters, and a JSON document is made of bytes. Feeding it a rune
 * of 0xE9 would append the single unit 0xE9 — one byte, and not UTF-8 — and a
 * rune above 0xFF would widen the whole builder, after which every later
 * put_bytes would be writing bytes into a buffer of 16-bit units.
 */
static bool put_utf8_rune(CvmStrBuf* out, rune r) {
    u8 encoded[CVM_UTF8_MAX_BYTES];
    u32 n = cvm_utf8_encode(r, encoded);
    if (n == 0u) {
        return false;
    }
    str bytes = {.ptr = encoded, .len = n};
    return cvm_str_buf_put_bytes(out, bytes);
}

static bool print_string(CvmString const* s, CvmStrBuf* out) {
    if (!cvm_str_buf_put_unit(out, (u32)'"')) {
        return false;
    }

    usize i = 0;
    while (i < s->length) {
        usize next = 0;
        rune r = cvm_string_code_point_at(s, i, &next);
        i = next;

        switch (r) {
        case (rune)'"':
            if (!printable_cstr(out, "\\\"")) {
                return false;
            }
            continue;
        case (rune)'\\':
            if (!printable_cstr(out, "\\\\")) {
                return false;
            }
            continue;
        case (rune)'\b':
            if (!printable_cstr(out, "\\b")) {
                return false;
            }
            continue;
        case (rune)'\f':
            if (!printable_cstr(out, "\\f")) {
                return false;
            }
            continue;
        case (rune)'\n':
            if (!printable_cstr(out, "\\n")) {
                return false;
            }
            continue;
        case (rune)'\r':
            if (!printable_cstr(out, "\\r")) {
                return false;
            }
            continue;
        case (rune)'\t':
            if (!printable_cstr(out, "\\t")) {
                return false;
            }
            continue;
        default:
            break;
        }

        if (r < (rune)0x20u) {
            /* Every other control character has only the escape form. */
            if (!print_hex4(out, (u32)r)) {
                return false;
            }
            continue;
        }
        if (r >= (rune)0xD800u && r <= (rune)0xDFFFu) {
            /*
             * A lone surrogate, which came from a string that holds one. JSON has
             * no spelling for it as UTF-8, and writing WTF-8 would produce a
             * document that its own parser rejects — so it is an escape, and the
             * round trip works because the escape is what produced it.
             */
            if (!print_hex4(out, (u32)r)) {
                return false;
            }
            continue;
        }
        if (!put_utf8_rune(out, r)) {
            return false;
        }
    }

    return cvm_str_buf_put_unit(out, (u32)'"');
}

/* Finite without math.h: an exponent of all ones means an infinity or a NaN. */
CVM_INLINE bool number_is_finite(f64 v) {
    u64 bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

static bool print_value(CvmJson const* v, CvmStrBuf* out, u32 depth);

static bool print_members(CvmJson const* object, CvmStrBuf* out, u32 depth) {
    bool first = true;
    for (CvmJsonMember const* m = cvm_json_object_first(object); m != NULL;
         m = cvm_json_object_next(object, m)) {
        if (!first && !cvm_str_buf_put_unit(out, (u32)',')) {
            return false;
        }
        first = false;
        if (!print_string(m->key, out) || !cvm_str_buf_put_unit(out, (u32)':') ||
            !print_value(m->value, out, depth)) {
            return false;
        }
    }
    return true;
}

static bool print_elements(CvmJson const* array, CvmStrBuf* out, u32 depth) {
    for (usize i = 0; i < array->size; i++) {
        if (i != 0u && !cvm_str_buf_put_unit(out, (u32)',')) {
            return false;
        }
        if (!print_value(cvm_json_array_at(array, i), out, depth)) {
            return false;
        }
    }
    return true;
}

static bool print_value(CvmJson const* v, CvmStrBuf* out, u32 depth) {
    if (depth > CVM_JSON_MAX_DEPTH) {
        /* The parser bounds what it builds, so this is a structure someone built
         * by hand. Refusing is better than the stack overflow that would
         * otherwise be the limit. */
        return false;
    }

    switch (cvm_json_kind(v)) {
    case CVM_JSON_NULL:
        return printable_cstr(out, "null");
    case CVM_JSON_BOOL:
        return printable_cstr(out, v->boolean ? "true" : "false");
    case CVM_JSON_NUMBER:
        /* JSON has no Infinity and no NaN, and every alternative to `null` for
         * them produces text that no parser accepts. */
        return number_is_finite(v->number) ? cvm_str_buf_put_f64(out, v->number)
                                           : printable_cstr(out, "null");
    case CVM_JSON_INT:
        /* The integer path: no shortest-round-trip float printing, just the
         * digits. This is the whole reason the kind exists — a document of
         * small integers printed as floats costs an order of magnitude more
         * than the same document printed as integers. */
        return cvm_str_buf_put_i64(out, v->integer);
    case CVM_JSON_STRING:
        return print_string(v->string, out);
    case CVM_JSON_ARRAY:
        return cvm_str_buf_put_unit(out, (u32)'[') &&
               print_elements(v, out, depth + 1u) &&
               cvm_str_buf_put_unit(out, (u32)']');
    case CVM_JSON_OBJECT:
        return cvm_str_buf_put_unit(out, (u32)'{') &&
               print_members(v, out, depth + 1u) && cvm_str_buf_put_unit(out, (u32)'}');
    }
    return false;
}

bool cvm_json_print(CvmJson const* value, CvmStrBuf* out) {
    return print_value(value, out, 0u);
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

typedef struct JsonParser {
    str text;
    usize pos;
    u32 depth;
} JsonParser;

CVM_INLINE bool at_end(JsonParser const* p) {
    return p->pos >= p->text.len;
}

CVM_INLINE u8 peek(JsonParser const* p) {
    return at_end(p) ? 0u : p->text.ptr[p->pos];
}

CVM_INLINE void advance(JsonParser* p) {
    if (!at_end(p)) {
        p->pos++;
    }
}

/* Space, tab, newline and carriage return, and nothing else: JSON has no room
 * for the whitespace libc's isspace accepts. */
static void skip_whitespace(JsonParser* p) {
    for (;;) {
        u8 c = peek(p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            advance(p);
            continue;
        }
        return;
    }
}

static CvmJson* parse_value(JsonParser* p);

/* The four characters that spell true, false and null. */
static bool take_word(JsonParser* p, char const* word) {
    usize i = 0;
    while (word[i] != '\0') {
        if (peek(p) != (u8)word[i]) {
            return false;
        }
        advance(p);
        i++;
    }
    return true;
}

/* One escape sequence's worth of a string, after the backslash. */
static bool parse_escape(JsonParser* p, CvmStrBuf* out) {
    u8 c = peek(p);
    advance(p);

    switch (c) {
    case '"':
    case '\\':
    case '/':
        return cvm_str_buf_put_unit(out, (u32)c);
    case 'b':
        return cvm_str_buf_put_unit(out, (u32)'\b');
    case 'f':
        return cvm_str_buf_put_unit(out, (u32)'\f');
    case 'n':
        return cvm_str_buf_put_unit(out, (u32)'\n');
    case 'r':
        return cvm_str_buf_put_unit(out, (u32)'\r');
    case 't':
        return cvm_str_buf_put_unit(out, (u32)'\t');
    case 'u':
        break;
    default:
        return false;
    }

    u32 value = 0;
    for (u32 i = 0; i < 4u; i++) {
        u8 digit = peek(p);
        advance(p);
        value <<= 4;
        if (digit >= '0' && digit <= '9') {
            value |= (u32)(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value |= (u32)(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            value |= (u32)(digit - 'A' + 10);
        } else {
            return false;
        }
    }

    /*
     * A pair is one code point, and a half on its own is a code unit. Both go
     * into the builder; which one it is decides which call, and that is the
     * whole of the surrogate rule on the way in.
     */
    if (value >= 0xD800u && value <= 0xDBFFu && peek(p) == '\\' &&
        p->text.len > p->pos + 1u && p->text.ptr[p->pos + 1u] == 'u') {
        usize save = p->pos;
        advance(p); /* the backslash */
        advance(p); /* the u */
        u32 trail = 0;
        bool ok = true;
        for (u32 i = 0; i < 4u && ok; i++) {
            u8 digit = peek(p);
            advance(p);
            trail <<= 4;
            if (digit >= '0' && digit <= '9') {
                trail |= (u32)(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                trail |= (u32)(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                trail |= (u32)(digit - 'A' + 10);
            } else {
                ok = false;
            }
        }
        if (ok && trail >= 0xDC00u && trail <= 0xDFFFu) {
            rune combined =
                (rune)(0x10000u + ((value - 0xD800u) << 10) + (trail - 0xDC00u));
            return cvm_str_buf_put_rune(out, combined);
        }
        p->pos = save; /* not a pair: the half stands on its own */
    }

    return cvm_str_buf_put_unit(out, value);
}

static CvmJson* parse_string(JsonParser* p) {
    cvm_assert(peek(p) == '"');
    advance(p);

    CvmStrBuf out;
    cvm_str_buf_init(&out);

    while (!at_end(p)) {
        u8 c = peek(p);

        if (c == '"') {
            advance(p);
            CvmString* s = cvm_str_buf_finish(&out);
            if (s == NULL) {
                return NULL;
            }
            CvmJson* v = cvm_json_string(s);
            return v;
        }
        if (c == '\\') {
            advance(p);
            if (!parse_escape(p, &out)) {
                cvm_str_buf_free(&out);
                return NULL;
            }
            continue;
        }
        if (c < 0x20u) {
            /* Control characters have to be escaped; a raw one is not JSON. */
            cvm_str_buf_free(&out);
            return NULL;
        }

        /* Otherwise a UTF-8 sequence, which must be well-formed. */
        usize offset = 0;
        rune r = 0;
        str rest = {.ptr = p->text.ptr + p->pos, .len = p->text.len - p->pos};
        if (cvm_utf8_next(rest, &offset, &r) != CVM_UTF8_RUNE) {
            cvm_str_buf_free(&out);
            return NULL;
        }
        if (!cvm_str_buf_put_rune(&out, r)) {
            cvm_str_buf_free(&out);
            return NULL;
        }
        p->pos += offset;
    }

    /* Ran out of input before the closing quote. */
    cvm_str_buf_free(&out);
    return NULL;
}

static CvmJson* parse_number(JsonParser* p) {
    usize start = p->pos;

    if (peek(p) == '-') {
        advance(p);
    }

    /* JSON's grammar: an integer part with no leading zero, an optional
     * fraction, an optional exponent. Anything else is not a number, and the
     * number parser below is told exactly which bytes to read. */
    if (peek(p) == '0') {
        advance(p);
    } else if (peek(p) >= '1' && peek(p) <= '9') {
        while (peek(p) >= '0' && peek(p) <= '9') {
            advance(p);
        }
    } else {
        return NULL;
    }

    if (peek(p) == '.') {
        /* The position is put back on failure so that the error names the dot
         * rather than whatever follows it. */
        usize dot = p->pos;
        advance(p);
        if (!(peek(p) >= '0' && peek(p) <= '9')) {
            p->pos = dot;
            return NULL;
        }
        while (peek(p) >= '0' && peek(p) <= '9') {
            advance(p);
        }
    }

    if (peek(p) == 'e' || peek(p) == 'E') {
        usize exponent = p->pos;
        advance(p);
        if (peek(p) == '+' || peek(p) == '-') {
            advance(p);
        }
        if (!(peek(p) >= '0' && peek(p) <= '9')) {
            p->pos = exponent;
            return NULL;
        }
        while (peek(p) >= '0' && peek(p) <= '9') {
            advance(p);
        }
    }

    str number = {.ptr = p->text.ptr + start, .len = p->pos - start};

    /*
     * An integer with no fraction and no exponent, fitting in 64 bits, is
     * stored as an integer: the parse is a scan rather than a float
     * conversion, and the print is digits rather than a shortest-round-trip
     * float rendering. A document of small integers pays for the float
     * machinery on both sides without ever needing it, which is why this
     * branch exists.
     */
    bool has_fraction_or_exponent = false;
    for (usize i = 0; i < number.len; i++) {
        u8 c = number.ptr[i];
        if (c == '.' || c == 'e' || c == 'E') {
            has_fraction_or_exponent = true;
            break;
        }
    }
    if (!has_fraction_or_exponent) {
        bool negative = number.ptr[0] == '-';
        u64 magnitude = 0;
        bool overflow = false;
        for (usize i = negative ? 1u : 0u; i < number.len; i++) {
            u8 digit = number.ptr[i] - (u8)'0';
            if (magnitude > ((u64)0x7FFFFFFFFFFFFFFFull - digit) / 10u) {
                overflow = true;
                break;
            }
            magnitude = magnitude * 10u + digit;
        }
        /* "-0" stays a float: negative zero is distinguishable in an f64 and
         * not in an i64, so storing it as an integer would change the text it
         * prints back as. */
        if (!overflow && !(negative && magnitude == 0u)) {
            return cvm_json_int(negative ? -(i64)magnitude : (i64)magnitude);
        }
        /* Past i64, or negative zero: fall through to the float. */
    }

    CvmAtodTempMem tmp;
    usize consumed = 0;
    f64 value = cvm_atod(number, &consumed, 10, 0, &tmp);
    if (consumed != number.len) {
        return NULL;
    }
    return cvm_json_number(value);
}

static CvmJson* parse_array(JsonParser* p) {
    cvm_assert(peek(p) == '[');
    advance(p);
    p->depth++;

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = cvm_json_array();

    skip_whitespace(p);
    if (peek(p) == ']') {
        advance(p);
        p->depth--;
        CvmJson* done = (CvmJson*)f_roots[0];
        cvm_frame_pop(&f_frame);
        return done;
    }

    for (;;) {
        skip_whitespace(p);
        CvmJson* element = parse_value(p);
        if (element == NULL) {
            cvm_frame_pop(&f_frame);
            return NULL;
        }

        /*
         * The element is a local and appending can allocate, which is safe only
         * because cvm_ref_array_push roots its own arguments — the same
         * contract cvm/array.h states. The array is in the frame before the
         * call, and the result goes back into it.
         */
        f_roots[0] = cvm_json_array_push((CvmJson*)f_roots[0], element);
        CvmJson* array = (CvmJson*)f_roots[0];

        skip_whitespace(p);
        if (peek(p) == ',') {
            advance(p);
            continue;
        }
        if (peek(p) == ']') {
            advance(p);
            p->depth--;
            cvm_frame_pop(&f_frame);
            return array;
        }
        cvm_frame_pop(&f_frame);
        return NULL;
    }
}

static CvmJson* parse_object(JsonParser* p) {
    cvm_assert(peek(p) == '{');
    advance(p);
    p->depth++;

    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = cvm_json_object();

    skip_whitespace(p);
    if (peek(p) == '}') {
        advance(p);
        p->depth--;
        CvmJson* done = (CvmJson*)f_roots[0];
        cvm_frame_pop(&f_frame);
        return done;
    }

    for (;;) {
        skip_whitespace(p);
        if (peek(p) != '"') {
            cvm_frame_pop(&f_frame);
            return NULL;
        }
        CvmJson* key_holder = parse_string(p);
        if (key_holder == NULL) {
            cvm_frame_pop(&f_frame);
            return NULL;
        }
        /* The key has to survive the value's parse, which allocates. */
        f_roots[1] = cvm_json_as_string(key_holder);

        skip_whitespace(p);
        if (peek(p) != ':') {
            cvm_frame_pop(&f_frame);
            return NULL;
        }
        advance(p);

        skip_whitespace(p);
        CvmJson* value = parse_value(p);
        if (value == NULL) {
            cvm_frame_pop(&f_frame);
            return NULL;
        }

        /* Both the object and the value are live across the append. */
        object_append((CvmJson*)f_roots[0], (CvmString*)f_roots[1], value);
        CvmJson* object = (CvmJson*)f_roots[0];

        skip_whitespace(p);
        if (peek(p) == ',') {
            advance(p);
            continue;
        }
        if (peek(p) == '}') {
            advance(p);
            p->depth--;
            /* Duplicates resolved once, at the close, rather than per member. */
            object_dedup((CvmJson*)f_roots[0]);
            cvm_frame_pop(&f_frame);
            return object;
        }
        cvm_frame_pop(&f_frame);
        return NULL;
    }
}

static CvmJson* parse_value(JsonParser* p) {
    if (p->depth >= CVM_JSON_MAX_DEPTH) {
        return NULL;
    }

    switch (peek(p)) {
    case '{':
        return parse_object(p);
    case '[':
        return parse_array(p);
    case '"':
        return parse_string(p);
    case 't':
        if (take_word(p, "true")) {
            return cvm_json_bool(true);
        }
        return NULL;
    case 'f':
        if (take_word(p, "false")) {
            return cvm_json_bool(false);
        }
        return NULL;
    case 'n':
        if (take_word(p, "null")) {
            return cvm_json_null();
        }
        return NULL;
    default:
        return parse_number(p);
    }
}

CvmJson* cvm_json_parse(str text, usize* error_offset) {
    JsonParser p = {.text = text, .pos = 0, .depth = 0};

    skip_whitespace(&p);
    CvmJson* value = parse_value(&p);
    if (value == NULL) {
        if (error_offset != NULL) {
            *error_offset = p.pos;
        }
        return NULL;
    }

    skip_whitespace(&p);
    if (!at_end(&p)) {
        /* Trailing content after a complete value: "1 2" is not a document. */
        if (error_offset != NULL) {
            *error_offset = p.pos;
        }
        return NULL;
    }

    if (error_offset != NULL) {
        *error_offset = p.pos;
    }
    return value;
}
