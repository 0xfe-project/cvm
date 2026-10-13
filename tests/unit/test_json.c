/*
 * tests/unit/test_json.c
 *
 * JSON: the value model, the parser, the printer, and the property that ties
 * them together.
 *
 * Round-tripping is the test that matters, and it is not "the parse produced an
 * equal structure" — a document that comes back with its keys in a different
 * order is equal and still wrong, which is the reason an object here is a list
 * and not a hash map. So the check is text: parse it, print it, and compare the
 * bytes to the canonical spelling of the same document.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/builder.h"
#include "cvm/gc.h"
#include "cvm/json.h"
#include "cvm/str.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

static str literal(char const* text) {
    str s = {.ptr = (u8 const*)text, .len = 0};
    while (text[s.len] != '\0') {
        s.len++;
    }
    return s;
}

/* Print a value into `out` as a NUL-terminated C string. */
static bool print_into(CvmJson* v, char* out, usize cap) {
    CvmStrBuf b;
    cvm_str_buf_init(&b);
    if (!cvm_json_print(v, &b)) {
        cvm_str_buf_free(&b);
        return false;
    }
    usize n = cvm_str_buf_length(&b);
    if (n + 1u > cap || cvm_str_buf_is_wide(&b)) {
        cvm_str_buf_free(&b);
        return false;
    }
    for (usize i = 0; i < n; i++) {
        out[i] = (char)b.data[i];
    }
    out[n] = '\0';
    cvm_str_buf_free(&b);
    return true;
}

static bool prints_as(CvmJson* v, char const* want) {
    char got[256];
    if (!print_into(v, got, sizeof(got))) {
        return false;
    }
    usize i = 0;
    while (got[i] != '\0' && want[i] != '\0' && got[i] == want[i]) {
        i++;
    }
    return got[i] == want[i];
}

/* ------------------------------------------------------------------ */
/* scalars                                                             */
/* ------------------------------------------------------------------ */

TEST scalars_parse_to_their_kinds(void) {
    fixture_enter();

    usize at = 0;
    CvmJson* null_value = cvm_json_parse(literal("null"), &at);
    ASSERT(null_value != NULL);
    ASSERT_EQ_FMT((u32)CVM_JSON_NULL, (u32)cvm_json_kind(null_value), "%u");

    CvmJson* yes = cvm_json_parse(literal("true"), &at);
    ASSERT(cvm_json_kind(yes) == CVM_JSON_BOOL);
    ASSERT(cvm_json_as_bool(yes));

    CvmJson* no = cvm_json_parse(literal("false"), &at);
    ASSERT_FALSE(cvm_json_as_bool(no));

    /* whitespace around the value is fine, and JSON's whitespace is four
     * characters wide */
    CvmJson* spaced = cvm_json_parse(literal(" \t\r\n 42 \n"), &at);
    ASSERT(cvm_json_kind(spaced) == CVM_JSON_INT);
    ASSERT_EQ_FMT(42.0, cvm_json_as_number(spaced), "%.0f");

    fixture_leave();
    PASS();
}

TEST numbers_follow_the_json_grammar(void) {
    fixture_enter();

    usize at = 0;
    struct {
        char const* text;
        f64 value;
    } const good[] = {
        {"0", 0.0},
        {"-0", -0.0},
        {"1", 1.0},
        {"-1", -1.0},
        {"123456789", 123456789.0},
        {"1.5", 1.5},
        {"-0.5", -0.5},
        {"1e3", 1000.0},
        {"1E3", 1000.0},
        {"1e+3", 1000.0},
        {"1e-3", 0.001},
        {"0.1", 0.1},
        {"1.7976931348623157e308", 1.7976931348623157e308},
    };

    for (usize i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        CvmJson* v = cvm_json_parse(literal(good[i].text), &at);
        if (v == NULL ||
            (cvm_json_kind(v) != CVM_JSON_NUMBER && cvm_json_kind(v) != CVM_JSON_INT)) {
            fprintf(stderr, "failed on %s at %zu\n", good[i].text, at);
            FAILm("a valid number did not parse");
        }
        ASSERT_EQ_FMT(good[i].value, cvm_json_as_number(v), "%.17g");
    }

    /* And the shapes JSON does not have. */
    char const* const bad[] = {
        "01", "1.", ".5", "+1", "1e", "1e+", "0x10", "Infinity", "NaN", "- 1",
    };
    for (usize i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT(cvm_json_parse(literal(bad[i]), &at) == NULL);
    }

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

TEST strings_decode_escapes_and_utf8(void) {
    fixture_enter();

    usize at = 0;
    FRAME(1);

    CvmJson* plain = cvm_json_parse(literal("\"hello\""), &at);
    f_roots[0] = plain;
    ASSERT(plain != NULL);
    ASSERT_EQ_FMT((usize)5, cvm_string_length(cvm_json_as_string(plain)), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(cvm_json_as_string(plain)));

    /* every simple escape */
    CvmJson* escaped = cvm_json_parse(literal("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\""), &at);
    f_roots[0] = escaped;
    ASSERT(escaped != NULL);
    CvmString* s = cvm_json_as_string(escaped);
    ASSERT_EQ_FMT((usize)8, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT('"', (int)cvm_string_unit(s, 0), "%c");
    ASSERT_EQ_FMT('\\', (int)cvm_string_unit(s, 1), "%c");
    ASSERT_EQ_FMT('/', (int)cvm_string_unit(s, 2), "%c");
    ASSERT_EQ_FMT('\n', (int)cvm_string_unit(s, 5), "%c");
    ASSERT_EQ_FMT('\t', (int)cvm_string_unit(s, 7), "%c");

    /* a \u escape below 0x100 makes a narrow string; above it, a wide one */
    CvmJson* eacute = cvm_json_parse(literal("\"caf\\u00e9\""), &at);
    f_roots[0] = eacute;
    ASSERT(eacute != NULL);
    ASSERT_FALSE(cvm_string_is_wide(cvm_json_as_string(eacute)));
    ASSERT_EQ_FMT(0xE9u, cvm_string_unit(cvm_json_as_string(eacute), 3), "%u");

    CvmJson* cjk = cvm_json_parse(literal("\"\\u4e2d\""), &at);
    f_roots[0] = cjk;
    ASSERT(cjk != NULL);
    ASSERT(cvm_string_is_wide(cvm_json_as_string(cjk)));
    ASSERT_EQ_FMT(0x4E2Du, cvm_string_unit(cvm_json_as_string(cjk), 0), "%u");

    /* a surrogate pair in escapes is one code point: two units */
    CvmJson* emoji = cvm_json_parse(literal("\"\\ud83d\\ude00\""), &at);
    f_roots[0] = emoji;
    ASSERT(emoji != NULL);
    CvmString* e = cvm_json_as_string(emoji);
    ASSERT_EQ_FMT((usize)2, cvm_string_length(e), "%zu");
    ASSERT_EQ_FMT(0xD83Du, cvm_string_unit(e, 0), "%u");
    ASSERT_EQ_FMT(0xDE00u, cvm_string_unit(e, 1), "%u");
    ASSERT_EQ_FMT((usize)1, cvm_string_code_point_count(e), "%zu");

    /* a half on its own is kept as a unit */
    CvmJson* lone = cvm_json_parse(literal("\"\\ud800x\""), &at);
    f_roots[0] = lone;
    ASSERT(lone != NULL);
    ASSERT_EQ_FMT((usize)2, cvm_string_length(cvm_json_as_string(lone)), "%zu");
    ASSERT_EQ_FMT(0xD800u, cvm_string_unit(cvm_json_as_string(lone), 0), "%u");

    /* raw UTF-8 in the text is decoded, not copied byte for byte */
    CvmJson* raw = cvm_json_parse(literal("\"\xE2\x82\xAC\""), &at);
    f_roots[0] = raw;
    ASSERT(raw != NULL);
    ASSERT_EQ_FMT((usize)1, cvm_string_length(cvm_json_as_string(raw)), "%zu");
    ASSERT_EQ_FMT(0x20ACu, cvm_string_unit(cvm_json_as_string(raw), 0), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST unterminated_and_malformed_strings_are_errors(void) {
    fixture_enter();

    usize at = 0;
    ASSERT(cvm_json_parse(literal("\"abc"), &at) == NULL);
    ASSERT(cvm_json_parse(literal("\"a\\q\""), &at) == NULL);  /* unknown escape */
    ASSERT(cvm_json_parse(literal("\"\\u12\""), &at) == NULL); /* short hex */
    ASSERT(cvm_json_parse(literal("\"\\uZZZZ\""), &at) == NULL);
    /* a raw control character has to be escaped */
    ASSERT(cvm_json_parse((str){(u8 const*)"\"a\nb\"", 5}, &at) == NULL);
    /* and the bytes have to be UTF-8 */
    ASSERT(cvm_json_parse((str){(u8 const*)"\"\x80\"", 3}, &at) == NULL);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* containers                                                          */
/* ------------------------------------------------------------------ */

TEST arrays_index_by_position(void) {
    fixture_enter();

    usize at = 0;
    FRAME(1);
    CvmJson* a = cvm_json_parse(literal("[1, \"two\", [3], null, true]"), &at);
    f_roots[0] = a;
    ASSERT(a != NULL);
    ASSERT(cvm_json_kind(a) == CVM_JSON_ARRAY);
    ASSERT_EQ_FMT((usize)5, cvm_json_array_length(a), "%zu");
    ASSERT_EQ_FMT(1.0, cvm_json_as_number(cvm_json_array_at(a, 0)), "%.0f");
    ASSERT(cvm_json_kind(cvm_json_array_at(a, 1)) == CVM_JSON_STRING);
    ASSERT(cvm_json_kind(cvm_json_array_at(a, 2)) == CVM_JSON_ARRAY);
    ASSERT(cvm_json_kind(cvm_json_array_at(a, 3)) == CVM_JSON_NULL);
    ASSERT(cvm_json_as_bool(cvm_json_array_at(a, 4)));

    CvmJson* empty = cvm_json_parse(literal("[]"), &at);
    ASSERT(empty != NULL);
    ASSERT_EQ_FMT((usize)0, cvm_json_array_length(empty), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST objects_keep_their_order_and_find_their_keys(void) {
    fixture_enter();

    usize at = 0;
    FRAME(1);
    CvmJson* o =
        cvm_json_parse(literal("{\"b\": 1, \"a\": 2, \"c\": {\"d\": 3}}"), &at);
    f_roots[0] = o;
    ASSERT(o != NULL);
    ASSERT_EQ_FMT((usize)3, cvm_json_object_size(o), "%zu");

    /* insertion order, which the text had */
    char const* want[] = {"b", "a", "c"};
    usize i = 0;
    for (CvmJsonMember const* m = cvm_json_object_first(o); m != NULL;
         m = cvm_json_object_next(o, m)) {
        CvmString* key = cvm_json_member_key(m);
        ASSERT(i < 3);
        ASSERT_EQ_FMT((usize)1, cvm_string_length(key), "%zu");
        ASSERT_EQ_FMT((int)want[i][0], (int)cvm_string_unit(key, 0), "%c");
        i++;
    }
    ASSERT_EQ_FMT((usize)3, i, "%zu");

    /* lookup by a key built separately, and by a wide one: the index hashes
     * content, so both find the same member */
    CvmString* key = cvm_string_new_ascii(STR_LITERAL("a"));
    ASSERT(cvm_json_object_has(o, key));
    ASSERT_EQ_FMT(2.0, cvm_json_as_number(cvm_json_object_get(o, key)), "%.0f");

    CvmString* wide_key = cvm_string_from_utf8(STR_LITERAL("a"));
    ASSERT(cvm_json_object_get(o, wide_key) != NULL);

    CvmString* absent = cvm_string_new_ascii(STR_LITERAL("z"));
    ASSERT_FALSE(cvm_json_object_has(o, absent));
    ASSERT(cvm_json_object_get(o, absent) == NULL);

    CvmJson* nested = cvm_json_object_get(o, cvm_string_new_ascii(STR_LITERAL("c")));
    ASSERT(cvm_json_kind(nested) == CVM_JSON_OBJECT);
    ASSERT_EQ_FMT(3.0,
                  cvm_json_as_number(cvm_json_object_get(
                      nested, cvm_string_new_ascii(STR_LITERAL("d")))),
                  "%.0f");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_repeated_key_replaces_its_value(void) {
    /* JSON says nothing about duplicates and every implementation that matters
     * keeps the last; keeping both would leave a document with two answers to
     * one question. */
    fixture_enter();

    usize at = 0;
    FRAME(1);
    CvmJson* o = cvm_json_parse(literal("{\"a\": 1, \"b\": 2, \"a\": 3}"), &at);
    f_roots[0] = o;
    ASSERT(o != NULL);
    ASSERT_EQ_FMT((usize)2, cvm_json_object_size(o), "%zu");

    CvmString* key = cvm_string_new_ascii(STR_LITERAL("a"));
    ASSERT_EQ_FMT(3.0, cvm_json_as_number(cvm_json_object_get(o, key)), "%.0f");
    /* and it kept its place in the order */
    CvmJsonMember const* first = cvm_json_object_first(o);
    ASSERT_EQ_FMT('a', (int)cvm_string_unit(cvm_json_member_key(first), 0), "%c");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* printing                                                            */
/* ------------------------------------------------------------------ */

TEST printing_is_canonical_and_keeps_the_order(void) {
    fixture_enter();

    usize at = 0;
    FRAME(1);

    CvmJson* o = cvm_json_parse(literal("{\"b\":1,\"a\":[1,2],\"c\":\"x\"}"), &at);
    f_roots[0] = o;
    ASSERT(o != NULL);
    /* the same bytes out, with the whitespace normalised away and the order
     * untouched — this is the assertion a hash map's object representation
     * could not pass */
    ASSERT(prints_as(o, "{\"b\":1,\"a\":[1,2],\"c\":\"x\"}"));

    CvmJson* n = cvm_json_parse(literal("[1.5,-0,1e21,\"\\u0000\",null,false]"), &at);
    f_roots[0] = n;
    ASSERT(n != NULL);
    ASSERT(prints_as(n, "[1.5,-0,1e+21,\"\\u0000\",null,false]"));

    /* a string that needs escaping comes back escaped */
    CvmJson* s = cvm_json_parse(literal("\"a\\\"b\\\\c\\nd\""), &at);
    f_roots[0] = s;
    ASSERT(s != NULL);
    ASSERT(prints_as(s, "\"a\\\"b\\\\c\\nd\""));

    /* a lone surrogate keeps its escape, which is the only spelling JSON has
     * for it and what makes the round trip exact */
    CvmJson* lone = cvm_json_parse(literal("\"\\ud800\""), &at);
    f_roots[0] = lone;
    ASSERT(lone != NULL);
    ASSERT(prints_as(lone, "\"\\ud800\""));

    /* non-finite numbers have no JSON spelling, so they print as null */
    CvmJson* inf = cvm_json_number(1.0 / 0.0);
    f_roots[0] = inf;
    ASSERT(prints_as(inf, "null"));
    CvmJson* nan = cvm_json_number(0.0 / 0.0);
    f_roots[0] = nan;
    ASSERT(prints_as(nan, "null"));

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the round trip                                                      */
/* ------------------------------------------------------------------ */

TEST documents_survive_parse_print_parse(void) {
    fixture_enter();

    static char const* const documents[] = {
        "{}",
        "[]",
        "null",
        "0",
        "\"\"",
        "{\"a\":1}",
        "{\"one\":1,\"two\":2,\"three\":3}",
        "[1,[2,[3,[4,[5]]]]]",
        "{\"a\":{\"b\":{\"c\":{\"d\":null}}}}",
        "[{\"n\":\"\\u00e9\"},{\"n\":\"\\u4e2d\"},{\"n\":\"\\ud83d\\ude00\"}]",
        "{\"nested\":[true,false,null,0,-0,1.25,1e-7,1e21],\"s\":\"a\\tb\"}",
        "[0.1,0.2,0.3,1.7976931348623157e308,5e-324]",
        "{\"empty_obj\":{},\"empty_arr\":[],\"empty_str\":\"\"}",
    };

    for (usize i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
        usize at = 0;
        CvmJson* first = cvm_json_parse(literal(documents[i]), &at);
        if (first == NULL) {
            fprintf(stderr, "document %zu failed at %zu: %s\n", i, at, documents[i]);
            FAILm("a document did not parse");
        }

        FRAME(1);
        f_roots[0] = first;
        char once[512];
        if (!print_into((CvmJson*)f_roots[0], once, sizeof(once))) {
            FAILm("a parsed document did not print");
        }

        /* printing a document that was printed already must change nothing */
        usize at2 = 0;
        CvmJson* second = cvm_json_parse(literal(once), &at2);
        if (second == NULL) {
            fprintf(stderr, "re-parse failed at %zu for %s\n", at2, once);
            FAILm("printed text did not parse");
        }
        f_roots[0] = second;

        char twice[512];
        if (!print_into((CvmJson*)f_roots[0], twice, sizeof(twice))) {
            FAILm("a re-parsed document did not print");
        }

        usize k = 0;
        while (once[k] != '\0' && twice[k] != '\0' && once[k] == twice[k]) {
            k++;
        }
        if (once[k] != twice[k]) {
            fprintf(stderr, "unstable: \"%s\" then \"%s\"\n", once, twice);
            FAILm("printing is not idempotent");
        }
        FRAME_END();
    }

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

TEST syntax_errors_report_where_they_are(void) {
    fixture_enter();

    struct {
        char const* text;
        usize offset;
    } const cases[] = {
        {"", 0},
        /* nothing but whitespace: there is no value, so the error is at the end */
        {"   ", 3},
        {"{", 1},          /* an object with nothing in it, unterminated */
        {"{\"a\":}", 5},   /* a value where a closing brace is */
        {"{\"a\" 1}", 5},  /* a colon is missing */
        {"{\"a\":1,}", 7}, /* a trailing comma */
        {"[1,]", 3},       /* the same for an array */
        {"[1 2]", 3},
        {"tru", 3}, /* one letter short: the input ran out */
        {"nul", 3},
        {"1 2", 2}, /* a complete value and then more */
        {"\"abc", 4},
        {"01", 1}, /* no leading zeros */
        {"1.", 1},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        usize offset = 0;
        CvmJson* v = cvm_json_parse(literal(cases[i].text), &offset);
        if (v != NULL) {
            fprintf(stderr, "accepted: \"%s\"\n", cases[i].text);
            FAILm("invalid JSON was accepted");
        }
        if (offset != cases[i].offset) {
            fprintf(stderr, "\"%s\": offset %zu, wanted %zu\n", cases[i].text, offset,
                    cases[i].offset);
            FAILm("the error offset is not where the problem is");
        }
    }

    fixture_leave();
    PASS();
}

TEST nesting_is_bounded(void) {
    /*
     * A recursive parser's only other limit is the stack, and ten thousand open
     * brackets is then a crash in someone else's code rather than an error in
     * theirs.
     */
    fixture_enter();

    /* An array nested to exactly the limit parses ... */
    static u8 deep[CVM_JSON_MAX_DEPTH * 2u + 1u];
    for (u32 i = 0; i < CVM_JSON_MAX_DEPTH; i++) {
        deep[i] = '[';
        deep[CVM_JSON_MAX_DEPTH * 2u - 1u - i] = ']';
    }
    usize at = 0;
    CvmJson* ok = cvm_json_parse((str){deep, CVM_JSON_MAX_DEPTH * 2u}, &at);
    ASSERT(ok != NULL);

    /* ... and one deeper does not. */
    static u8 too_deep[CVM_JSON_MAX_DEPTH * 2u + 3u];
    for (u32 i = 0; i <= CVM_JSON_MAX_DEPTH; i++) {
        too_deep[i] = '[';
        too_deep[CVM_JSON_MAX_DEPTH * 2u + 1u - i] = ']';
    }
    ASSERT(cvm_json_parse((str){too_deep, CVM_JSON_MAX_DEPTH * 2u + 2u}, &at) == NULL);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the collector                                                       */
/* ------------------------------------------------------------------ */

TEST a_parsed_document_survives_a_collection(void) {
    /*
     * Everything a document is made of — its members, its keys, its nested
     * values — is reachable only through pointers the descriptors have to name.
     * A member list whose links were not traced would keep its first member and
     * lose the rest, which is exactly the bug this checks for.
     */
    fixture_enter();

    usize at = 0;
    FRAME(1);
    CvmJson* doc = cvm_json_parse(
        literal("{\"a\":[1,2,3],\"b\":{\"c\":\"deep\"},\"d\":\"s\"}"), &at);
    f_roots[0] = doc;
    ASSERT(doc != NULL);

    collect();

    CvmJson* after = (CvmJson*)f_roots[0];
    ASSERT_EQ_FMT((usize)3, cvm_json_object_size(after), "%zu");

    /* every member still there, in order, with its value */
    char const* keys[] = {"a", "b", "d"};
    usize i = 0;
    for (CvmJsonMember const* m = cvm_json_object_first(after); m != NULL;
         m = cvm_json_object_next(after, m)) {
        ASSERT(i < 3);
        ASSERT_EQ_FMT((int)keys[i][0], (int)cvm_string_unit(cvm_json_member_key(m), 0),
                      "%c");
        ASSERT(cvm_json_member_value(m) != NULL);
        i++;
    }
    ASSERT_EQ_FMT((usize)3, i, "%zu");

    CvmJson* array = cvm_json_object_get(after, cvm_string_new_ascii(STR_LITERAL("a")));
    ASSERT(array != NULL);
    ASSERT_EQ_FMT((usize)3, cvm_json_array_length(array), "%zu");
    ASSERT_EQ_FMT(3.0, cvm_json_as_number(cvm_json_array_at(array, 2)), "%.0f");

    CvmJson* nested =
        cvm_json_object_get(after, cvm_string_new_ascii(STR_LITERAL("b")));
    CvmJson* deep = cvm_json_object_get(nested, cvm_string_new_ascii(STR_LITERAL("c")));
    ASSERT(deep != NULL);
    CvmString* s = cvm_json_as_string(deep);
    ASSERT_EQ_FMT((usize)4, cvm_string_length(s), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(json_suite) {
    RUN_TEST(scalars_parse_to_their_kinds);
    RUN_TEST(numbers_follow_the_json_grammar);
    RUN_TEST(strings_decode_escapes_and_utf8);
    RUN_TEST(unterminated_and_malformed_strings_are_errors);
    RUN_TEST(arrays_index_by_position);
    RUN_TEST(objects_keep_their_order_and_find_their_keys);
    RUN_TEST(a_repeated_key_replaces_its_value);
    RUN_TEST(printing_is_canonical_and_keeps_the_order);
    RUN_TEST(documents_survive_parse_print_parse);
    RUN_TEST(syntax_errors_report_where_they_are);
    RUN_TEST(nesting_is_bounded);
    RUN_TEST(a_parsed_document_survives_a_collection);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(json_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#else /* _WIN32 */

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
