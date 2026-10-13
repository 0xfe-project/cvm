/*
 * bench/json/json-bench.c
 *
 * Three JSON implementations, one corpus, one process:
 *
 *   ours     vm/json.c — nodes are GC objects, strings are CvmString
 *   ljson    refs/json-5.0.3 — a block pool, in-place string reuse
 *   yyjson   refs/yyjson-0.13.0 — a tape, SIMD scanning, no editing
 *
 * The question this answers is whether a JSON path should be a special case
 * built on a vendored library instead of our own value model, and the phases are
 * chosen to answer it honestly:
 *
 *   parse        build the document. Ours allocates a GC object per node and a
 *                CvmString per string; the other two allocate a pool and borrow
 *                the input's strings. So ours has already paid at the end of this
 *                column what the others have not — which is what the next column
 *                is for.
 *
 *   navigate     touch every node. What a reader of the document pays.
 *
 *   materialize  turn every string into a CvmString, interned. For ours this is
 *                already done (parse did it) and the column reads n/a; for the
 *                others it is the actual cost of crossing into our runtime, and it
 *                is the cost that decides whether a vendored document is worth
 *                keeping in one shape or the other.
 *
 *   print        serialize back to text.
 *
 * Neither library can parse a const buffer in its fast mode: ljson's reuse mode
 * and yyjson's in-situ mode both want to write into the input (padding, string
 * terminators). We hand each one its own writable copy and do not time the copy.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/types.h"
#include "cvm/builder.h"
#include "cvm/buf.h"
#include "cvm/hash.h"
#include "cvm/intern.h"
#include "cvm/json.h"
#include "cvm/str.h"

#include "cvm/vm.h"

#include "json.h"    /* ljson */
#include "yyjson.h"  /* yyjson 0.13.0 */

/* ------------------------------------------------------------------ */
/* the corpus                                                          */
/* ------------------------------------------------------------------ */

typedef struct Text {
    char* data;
    usize len;
    usize cap;
} Text;

static void text_init(Text* t) {
    t->cap = 1u << 16;
    t->data = malloc(t->cap);
    t->len = 0;
}

static void text_add(Text* t, char const* bytes, usize n) {
    while (t->len + n > t->cap) {
        t->cap *= 2u;
        t->data = realloc(t->data, t->cap);
    }
    memcpy(t->data + t->len, bytes, n);
    t->len += n;
}

static void text_printf(Text* t, char const* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        text_add(t, buf, (usize)n);
    }
}

/* An array of integers: the cheapest possible document per byte. */
static void build_numbers(Text* t, u32 count) {
    text_add(t, "[", 1u);
    for (u32 i = 0; i < count; i++) {
        text_printf(t, "%s%u", i ? "," : "", i * 7u);
    }
    text_add(t, "]", 1u);
}

/* Objects with short keys and short values: what a real document looks like. */
static void build_records(Text* t, u32 count) {
    text_add(t, "[", 1u);
    for (u32 i = 0; i < count; i++) {
        char buf[192];
        int n = snprintf(buf, sizeof(buf),
                         "%s{\"id\":%u,\"name\":\"alpha-bravo-%u\",\"ok\":%s,"
                         "\"score\":%u.%u}",
                         i ? "," : "", i, i, (i & 1u) ? "true" : "false", i, i % 100u);
        text_add(t, buf, (usize)n);
    }
    text_add(t, "]", 1u);
}

/* Strings with escapes: the part of a parser that is a scan, not a tree. The
 * text is written through one format string so no length is counted by hand. */
static void build_strings(Text* t, u32 count) {
    text_add(t, "[", 1u);
    for (u32 i = 0; i < count; i++) {
        text_printf(t,
                    "%s\"some text with \\\" and \\n and unicode \\u00e9 "
                    "more text here, and yet more, to make this an ordinary string\"",
                    i ? "," : "");
    }
    text_add(t, "]", 1u);
}

/* One object with `count` members: the shape that measures the cost of keeping
 * the member list sorted by hash, where every insert walks to its position. */
static void build_big_object(Text* t, u32 count) {
    text_printf(t, "{");
    for (u32 i = 0; i < count; i++) {
        text_printf(t, "%s\"key-%u\":%u", i ? "," : "", i, i);
    }
    text_printf(t, "}");
}

/* ------------------------------------------------------------------ */
/* what a run measures                                                 */
/* ------------------------------------------------------------------ */

typedef struct Stats {
    u64 nodes;
    u64 numbers;
    u64 strings;
    f64 checksum;
} Stats;

static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* ours                                                                */
/* ------------------------------------------------------------------ */

/*
 * The document leaves the parser in a local, and a local is invisible to the
 * collector. So it goes into the frame the harness holds open for the whole
 * measurement — which is exactly what a generated program does with a value it is
 * about to use.
 */
static opaque_mut g_ours_root;

/*
 * Where the harness keeps things alive: slot 1 (the table) and slot 2 (a string
 * that is on its way into the table). The visitors reach the scratch slot through
 * this pointer because a local variable is not something the collector can see.
 */
static opaque_mut* g_roots;

static void* ours_parse(char* text, usize len) {
    usize err = 0;
    CvmJson* doc = cvm_json_parse((str){.ptr = (u8 const*)text, .len = len}, &err);
    g_ours_root = doc;
    return doc;
}

static void ours_walk(CvmJson* v, Stats* s, u32 depth) {
    s->nodes++;
    switch (cvm_json_kind(v)) {
    case CVM_JSON_NUMBER:
        s->numbers++;
        s->checksum += cvm_json_as_number(v);
        break;
    case CVM_JSON_STRING: {
        s->strings++;
        CvmString* str = cvm_json_as_string(v);
        usize n = cvm_string_length(str);
        for (usize i = 0; i < n; i++) {
            s->checksum += (f64)cvm_string_unit(str, i);
        }
        break;
    }
    case CVM_JSON_ARRAY:
        for (usize i = 0; i < cvm_json_array_length(v); i++) {
            ours_walk(cvm_json_array_at(v, i), s, depth + 1u);
        }
        break;
    case CVM_JSON_OBJECT:
        for (CvmJsonMember const* m = cvm_json_object_first(v); m != NULL;
             m = cvm_json_object_next(v, m)) {
            ours_walk(cvm_json_member_value(m), s, depth + 1u);
        }
        break;
    default:
        break;
    }
}

static void ours_navigate(void* doc, Stats* s) {
    ours_walk((CvmJson*)doc, s, 0u);
}

static bool ours_print(void* doc, CvmBuf* out) {
    CvmStrBuf b;
    cvm_str_buf_init(&b);
    bool ok = cvm_json_print((CvmJson*)doc, &b);
    if (ok && !cvm_str_buf_is_wide(&b)) {
        ok = cvm_buf_put(out, b.data, b.len);
    } else {
        ok = false;
    }
    cvm_str_buf_free(&b);
    return ok;
}

static void ours_release(void* doc) {
    CVM_UNUSED(doc);
}

/* ------------------------------------------------------------------ */
/* ljson                                                               */
/* ------------------------------------------------------------------ */

typedef struct LjsonDoc {
    json_object* root;
    json_mem_t mem;
    u32 items;
} LjsonDoc;

static void* ljson_parse(char* text, usize len) {
    LjsonDoc* d = calloc(1, sizeof(LjsonDoc));
    /* `valid = false` on entry is how ljson is told to initialise the pool. */
    d->root = json_fast_parse_str(text, len, &d->mem);
    if (d->root == NULL) {
        free(d);
        return NULL;
    }
    return d;
}

static void ljson_walk(json_object* json, Stats* s) {
    s->nodes++;
    switch (json->ikey.type) {
    case JSON_INT:
    case JSON_LINT:
    case JSON_DOUBLE:
        s->numbers++;
        s->checksum += json->value.vnum.vdbl;
        break;
    case JSON_STRING: {
        s->strings++;
        usize n = json->istr.len;
        for (usize i = 0; i < n; i++) {
            s->checksum += (f64)(u8)json->value.vstr[i];
        }
        break;
    }
    case JSON_ARRAY:
    case JSON_OBJECT: {
        json_items_t items;
        memset(&items, 0, sizeof(items));
        if (json_get_items(json, &items) == 0) {
            for (u32 i = 0; i < items.count; i++) {
                ljson_walk(items.items[i].json, s);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void ljson_navigate(void* doc, Stats* s) {
    ljson_walk(((LjsonDoc*)doc)->root, s);
    /* The printer takes an item count as a capacity hint; giving it the real one
     * keeps it from reallocating its way through a large document. */
    ((LjsonDoc*)doc)->items = (u32)s->nodes;
}

static bool ljson_print(void* doc, CvmBuf* out) {
    LjsonDoc* d = (LjsonDoc*)doc;
    usize len = 0;
    json_print_ptr_t ptr;
    memset(&ptr, 0, sizeof(ptr));
    char* text = json_print_unformat(d->root, (int)(d->items ? d->items : 16u), &len, &ptr);
    if (text == NULL) {
        return false;
    }
    bool ok = cvm_buf_put(out, text, len);
    json_memory_free(text);
    return ok;
}

static void ljson_release(void* doc) {
    LjsonDoc* d = (LjsonDoc*)doc;
    pjson_memory_free(&d->mem);
    free(d);
}

/* ------------------------------------------------------------------ */
/* yyjson                                                              */
/* ------------------------------------------------------------------ */

static void* yyjson_parse(char* text, usize len) {
    yyjson_read_err err;
    yyjson_doc* doc = yyjson_read_opts(text, len, YYJSON_READ_NOFLAG, NULL, &err);
    return doc;
}

static void yyjson_walk(yyjson_val* val, Stats* s) {
    s->nodes++;
    switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_NUM:
        s->numbers++;
        s->checksum += yyjson_get_num(val);
        break;
    case YYJSON_TYPE_STR: {
        s->strings++;
        const char* str = yyjson_get_str(val);
        usize n = yyjson_get_len(val);
        for (usize i = 0; i < n; i++) {
            s->checksum += (f64)(u8)str[i];
        }
        break;
    }
    case YYJSON_TYPE_ARR: {
        yyjson_arr_iter it = yyjson_arr_iter_with(val);
        yyjson_val* child;
        while ((child = yyjson_arr_iter_next(&it)) != NULL) {
            yyjson_walk(child, s);
        }
        break;
    }
    case YYJSON_TYPE_OBJ: {
        /* yyjson_obj_iter_next hands back the *key*; the value is one hop away.
         * Member names are not walked: the `strs` column counts value strings, so
         * that it means the same thing in all three columns. They are still
         * created, in materialize, because a program can read them. */
        yyjson_obj_iter it = yyjson_obj_iter_with(val);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            yyjson_walk(yyjson_obj_iter_get_val(key), s);
        }
        break;
    }
    default:
        break;
    }
}

static void yyjson_navigate(void* doc, Stats* s) {
    yyjson_walk(yyjson_doc_get_root((yyjson_doc*)doc), s);
}

static bool yyjson_print(void* doc, CvmBuf* out) {
    usize len = 0;
    char* text = yyjson_write((yyjson_doc*)doc, YYJSON_WRITE_NOFLAG, &len);
    if (text == NULL) {
        return false;
    }
    bool ok = cvm_buf_put(out, text, len);
    free(text);
    return ok;
}

static void yyjson_release(void* doc) {
    yyjson_doc_free((yyjson_doc*)doc);
}

/* ------------------------------------------------------------------ */
/* the harness                                                         */
/* ------------------------------------------------------------------ */

typedef struct Impl {
    char const* name;
    void* (*parse)(char*, usize);
    void (*navigate)(void*, Stats*);
    bool (*print)(void*, CvmBuf*);
    void (*release)(void*);
    /* Whether parse already produced the runtime's own strings. */
    bool strings_are_ours;
} Impl;

static Impl const IMPLS[] = {
    {"ours", ours_parse, ours_navigate, ours_print, ours_release, true},
    {"ljson", ljson_parse, ljson_navigate, ljson_print, ljson_release, false},
    {"yyjson", yyjson_parse, yyjson_navigate, yyjson_print, yyjson_release, false},
};

#define IMPL_COUNT (sizeof(IMPLS) / sizeof(IMPLS[0]))

/*
 * Turn every string in a borrowed document into an interned CvmString, which is
 * what a vendored document costs at the boundary between the library's memory and
 * ours. Ours has already paid it.
 *
 * Note the node's type: ljson keeps it in the low four bits of a bitfield word
 * shared with the key's length, not in a field of its own — `json->ikey.type` is
 * how the library reads it too.
 */
/*
 * Interning allocates: a map that runs out of room builds a bigger key array, and
 * that allocation may collect. So the candidate and the table must both be in
 * roots across the call — which is the frame discipline every caller of cvm_intern
 * owes, and the reason the API takes a rooted value rather than a borrowed one.
 */
static void intern_rooted(CvmHashMap* table, CvmString* s) {
    g_roots[2] = s;
    (void)cvm_intern(table, s);
    g_roots[2] = 0;
}

static void ours_materialize_visit(CvmJson* v, CvmHashMap* table, Stats* s) {
    switch (cvm_json_kind(v)) {
    case CVM_JSON_STRING:
        intern_rooted(table, cvm_json_as_string(v));
        break;
    case CVM_JSON_ARRAY:
        for (usize i = 0; i < cvm_json_array_length(v); i++) {
            ours_materialize_visit(cvm_json_array_at(v, i), table, s);
        }
        break;
    case CVM_JSON_OBJECT:
        for (CvmJsonMember const* m = cvm_json_object_first(v); m != NULL;
             m = cvm_json_object_next(v, m)) {
            ours_materialize_visit(cvm_json_member_value(m), table, s);
        }
        break;
    default:
        break;
    }
}

static u32 ljson_materialize_visit(json_object* json, CvmHashMap* table) {
    u32 n = 0;
    switch (json->ikey.type) {
    case JSON_STRING: {
        str raw = {.ptr = (u8 const*)json->value.vstr, .len = json->istr.len};
        CvmString* s = cvm_string_from_utf8(raw);
        if (s != NULL) {
            intern_rooted(table, s);
            n = 1u;
        }
        break;
    }
    case JSON_ARRAY:
    case JSON_OBJECT: {
        json_items_t items;
        memset(&items, 0, sizeof(items));
        if (json_get_items(json, &items) == 0) {
            for (u32 i = 0; i < items.count; i++) {
                json_object* child = items.items[i].json;
                if (child->key != NULL && child->ikey.len != 0u) {
                    str raw = {.ptr = (u8 const*)child->key, .len = child->ikey.len};
                    CvmString* name = cvm_string_from_utf8(raw);
                    if (name != NULL) {
                        intern_rooted(table, name);
                        n++;
                    }
                }
                n += ljson_materialize_visit(child, table);
            }
        }
        break;
    }
    default:
        break;
    }
    return n;
}

static u32 yyjson_materialize_visit(yyjson_val* val, CvmHashMap* table) {
    u32 n = 0;
    switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_STR: {
        str raw = {.ptr = (u8 const*)yyjson_get_str(val), .len = yyjson_get_len(val)};
        CvmString* s = cvm_string_from_utf8(raw);
        if (s != NULL) {
            (void)cvm_intern(table, s);
            n = 1u;
        }
        break;
    }
    case YYJSON_TYPE_ARR: {
        yyjson_arr_iter it = yyjson_arr_iter_with(val);
        yyjson_val* child;
        while ((child = yyjson_arr_iter_next(&it)) != NULL) {
            n += yyjson_materialize_visit(child, table);
        }
        break;
    }
    case YYJSON_TYPE_OBJ: {
        yyjson_obj_iter it = yyjson_obj_iter_with(val);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&it)) != NULL) {
            /* The member name is a string the program can read: borrowed here,
             * already a CvmString in our own representation. */
            str raw = {.ptr = (u8 const*)yyjson_get_str(key), .len = yyjson_get_len(key)};
            CvmString* name = cvm_string_from_utf8(raw);
            if (name != NULL) {
                intern_rooted(table, name);
                n++;
            }
            n += yyjson_materialize_visit(yyjson_obj_iter_get_val(key), table);
        }
        break;
    }
    default:
        break;
    }
    return n;
}

typedef struct Result {
    u64 ns_parse;
    u64 ns_navigate;
    u64 ns_materialize;
    u64 ns_print;
    Stats stats;
    u32 materialized;
    bool ok;
} Result;

/*
 * One corpus through one implementation, averaged over rounds.
 *
 * Averaging rather than taking the best round is deliberate: the collector is
 * live, the previous round's document is garbage by now, and a collection that
 * lands inside a parse is a cost a real program pays. "Best of N" would quietly
 * report the round where nothing was collected.
 *
 * The corpus is re-copied every round because the fast modes write into it.
 */
static Result measure(Impl const* impl, char const* text, usize len, u32 rounds) {
    Result r;
    memset(&r, 0, sizeof(r));
    r.ok = true;

    CvmHashMap* table = cvm_intern_new();

    /* One frame for the whole measurement: our documents are GC objects and the
     * other two are not, so it is held for both and costs nothing for theirs.
     * Each round overwrites the slot, which is what turns the previous round's
     * document into garbage and makes the collector part of the measurement. */
    CVM_FRAME_DECL(f, 3);
    cvm_frame_push(&f_frame, f_roots, 3);
    g_roots = f_roots;
    f_roots[1] = table; /* the table is a GC object too */

    char* copy = (char*)malloc(len + 8u);

    for (u32 round = 0; round < rounds; round++) {
        memcpy(copy, text, len);
        memset(copy + len, 0, 8u);

        u64 t0 = now_ns();
        void* doc = impl->parse(copy, len);
        u64 t1 = now_ns();
        if (doc == NULL) {
            cvm_frame_pop(&f_frame);
            free(copy);
            r.ok = false;
            return r;
        }
        f_roots[0] = g_ours_root;
        r.ns_parse += t1 - t0;

        memset(&r.stats, 0, sizeof(r.stats)); /* the counts are per document */
        t0 = now_ns();
        impl->navigate(doc, &r.stats);
        t1 = now_ns();
        r.ns_navigate += t1 - t0;

        t0 = now_ns();
        if (impl->strings_are_ours) {
            /* Already CvmStrings: interning them again would measure our own hash
             * table, not a boundary. */
        } else if (strcmp(impl->name, "ljson") == 0) {
            r.materialized = ljson_materialize_visit(((LjsonDoc*)doc)->root, table);
        } else {
            r.materialized =
                yyjson_materialize_visit(yyjson_doc_get_root((yyjson_doc*)doc), table);
        }
        t1 = now_ns();
        r.ns_materialize += t1 - t0;

        /* A fresh buffer each round: growing it is part of printing. */
        CvmBuf out;
        cvm_buf_init(&out, cvm_buf_system_alloc());
        t0 = now_ns();
        if (!impl->print(doc, &out)) {
            r.ok = false;
        }
        t1 = now_ns();
        r.ns_print += t1 - t0;
        cvm_buf_free(&out);

        impl->release(doc);
    }

    g_roots = NULL;
    cvm_frame_pop(&f_frame);
    free(copy);

    r.ns_parse /= rounds;
    r.ns_navigate /= rounds;
    r.ns_materialize /= rounds;
    r.ns_print /= rounds;
    return r;
}

static void run_corpus(char const* name, Text* text, u32 repeats) {
    printf("== %s: %zu bytes\n", name, text->len);
    printf("   %-7s %10s %10s %12s %10s %8s %8s\n", "impl", "parse", "navigate",
           "materialize", "print", "nodes", "strs");

    for (usize k = 0; k < IMPL_COUNT; k++) {
        Result r = measure(&IMPLS[k], text->data, text->len, repeats);
        if (!r.ok) {
            printf("   %-7s parse failed\n", IMPLS[k].name);
            continue;
        }
        printf("   %-7s %8llu ns %8llu ns %10llu ns %8llu ns %8llu %8llu%s\n",
               IMPLS[k].name, (unsigned long long)r.ns_parse,
               (unsigned long long)r.ns_navigate, (unsigned long long)r.ns_materialize,
               (unsigned long long)r.ns_print, (unsigned long long)r.stats.nodes,
               (unsigned long long)r.stats.strings,
               IMPLS[k].strings_are_ours ? "  (parse already did this)" : "");
    }
    printf("\n");
}

int main(int argc, char** argv) {
    u32 repeats = 3;
    if (argc > 1) {
        repeats = (u32)strtoul(argv[1], NULL, 10);
    }

    /* The public entry point: it sizes the heap, starts the workers and
     * registers this thread, which is what a hosted program does. The unit tests
     * reach below it because they need the pieces; a benchmark is a program. */
    if (!cvm_init(NULL)) {
        fprintf(stderr, "could not bring up the runtime\n");
        return 1;
    }

    printf("json: parse, navigate, materialize, print. repeat=%u\n\n", repeats);

    Text t;
    text_init(&t);
    build_numbers(&t, 50000u);
    run_corpus("numbers", &t, repeats);
    free(t.data);

    text_init(&t);
    build_records(&t, 5000u);
    run_corpus("records", &t, repeats);
    free(t.data);

    text_init(&t);
    build_strings(&t, 2000u);
    run_corpus("strings", &t, repeats);
    free(t.data);

    text_init(&t);
    build_big_object(&t, 1000u);
    run_corpus("big-object", &t, repeats);
    free(t.data);

    cvm_shutdown();
    return 0;
}
