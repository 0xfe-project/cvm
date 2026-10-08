/*
 * bench/dtoa/dtoa-bench.c
 *
 * Which f64 <-> text implementation do we port: quickjs's dtoa.c, or
 * ljson's jnum.c?
 *
 * Both are compiled into this one binary and measured on the same corpus in
 * the same process, because two numbers taken in two runs on one machine are
 * not comparable and these two are close enough that the difference matters.
 *
 * What is measured, and why each one:
 *
 *   round trip     format then parse, bit-compared. The contract of a
 *                  shortest-precise dtoa is that this is exact for every
 *                  finite double, so one failure is disqualifying rather
 *                  than a percentage.
 *
 *   strtod         the same string parsed by the libc parser. An
 *                  implementation that round-trips through itself can be
 *                  wrong in a matching way in both directions; an
 *                  independent parser is what rules that out.
 *
 *   bytes/value    total emitted length over the corpus, as a proxy for
 *                  "shortest". An implementation that always prints 17
 *                  significant digits round-trips perfectly and is still not
 *                  what we want.
 *
 *   ns/op          format, and parse, timed separately over a fixed corpus
 *                  and repeated. Parsing is timed over strings formatted
 *                  beforehand, so it is not measuring the formatter.
 *
 * corpus: a splitmix64 walk over random bit patterns (finite only), plus a
 * fixed list of the values where the two are most likely to disagree: the
 * denormal boundary, the exponent-notation thresholds, powers of two, and
 * -0.0.
 *
 * build and run: see run.sh in this directory.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/types.h" /* our u8/u32/u64/f64/usize/bool */

#include "dtoa.h"           /* quickjs: js_dtoa, js_atod, JSDTOA*TempMem */
#include "jnum.h"           /* ljson:  jnum_dtoa, jnum_atod */
#include "str/dtoa.h"       /* ours, ported from the quickjs one */

#define BUF_CAP 64

/* ------------------------------------------------------------------ */
/* the two implementations, behind one shape                           */
/* ------------------------------------------------------------------ */

static u32 fmt_quickjs(f64 d, char* buf) {
    JSDTOATempMem tmp;
    int n = js_dtoa(buf, d, 10, 0, JS_DTOA_FORMAT_FREE | JS_DTOA_MINUS_ZERO, &tmp);
    return (u32)(n < 0 ? 0 : n);
}

static f64 parse_quickjs(char const* s) {
    JSATODTempMem tmp;
    char const* end = NULL;
    f64 v = js_atod(s, &end, 10, 0, &tmp);
    /* The caller only ever parses a whole string it just produced; a parser
     * that stopped early would otherwise look like a round-trip success. */
    if (end == NULL || *end != '\0') {
        return 0.0 / 0.0;
    }
    return v;
}

static u32 fmt_jnum(f64 d, char* buf) {
    int n = jnum_dtoa(d, buf);
    return (u32)(n < 0 ? 0 : n);
}

static f64 parse_jnum(char const* s) {
    return jnum_atod(s);
}

/*
 * The port itself. It is the quickjs algorithm, so its row is not a third
 * candidate — it is the check that rewriting the file into our types and its
 * parser onto (ptr, len) did not change a single conversion. It should be
 * indistinguishable from the quickjs row, including bytes/value.
 */
static u32 fmt_ours(f64 d, char* buf) {
    CvmDtoaTempMem tmp;
    i32 n = cvm_dtoa((u8*)buf, d, 10, 0, CVM_DTOA_FORMAT_FREE | CVM_DTOA_MINUS_ZERO, &tmp);
    return (u32)(n < 0 ? 0 : n);
}

static f64 parse_ours(char const* s) {
    CvmAtodTempMem tmp;
    usize len = strlen(s);
    usize consumed = 0;
    str text = {.ptr = (u8 const*)s, .len = len};
    f64 v = cvm_atod(text, &consumed, 10, 0, &tmp);
    /* The scanners differ in shape here: ours reports how much it used, and a
     * number that stops early is a different answer, not a success. */
    if (consumed != len) {
        return 0.0 / 0.0;
    }
    return v;
}

typedef struct Impl {
    char const* name;
    u32 (*fmt)(f64, char*);
    f64 (*parse)(char const*);
} Impl;

static Impl const IMPLS[] = {
    {"quickjs", fmt_quickjs, parse_quickjs},
    {"jnum", fmt_jnum, parse_jnum},
    {"ours", fmt_ours, parse_ours},
};

#define IMPL_COUNT (sizeof(IMPLS) / sizeof(IMPLS[0]))

/* ------------------------------------------------------------------ */
/* corpus                                                              */
/* ------------------------------------------------------------------ */

static u64 rng_state = 0x9e3779b97f4a7c15ull;

static u64 splitmix64(void) {
    u64 z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static f64 from_bits(u64 bits) {
    f64 d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static u64 to_bits(f64 d) {
    u64 bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static bool is_finite(f64 d) {
    return (to_bits(d) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

/* The values that sit on the boundaries: the smallest denormals, the
 * thresholds where the spelling changes to exponent form, powers of two,
 * and the two spellings of zero. */
static f64 const SPECIALS[] = {
    0.0,  -0.0, 1.0,           -1.0,          0.1,   0.2,   0.3,   1.0 / 3.0,
    2.0 / 3.0, 1e-5, 1e-4,      1e15,          1e16,  1e17,  1e20,  1e21,
    1e22, 1e23,  1234.5678,     123456789012345678.0,
    9007199254740992.0,         9007199254740994.0,
    1e-300,       1e300,        2.2250738585072014e-308, 5e-324,
    1.7976931348623157e308,     4.9e-324,      2.5e-324,      1e-323,
    3.141592653589793,          2.718281828459045,     6.02214076e23,
    1.602176634e-19,            0.30000000000000004,   4.9406564584124654e-324,
};

typedef struct Corpus {
    f64* v;
    usize n;
} Corpus;

typedef enum CorpusMode {
    /* Random bit patterns. The worst case for the shortest-form search:
     * most of these need the full 17 digits. */
    CORPUS_RANDOM = 0,
    /* What JSON and source code actually carry: small integers, decimals
     * with a few places, and the occasional wild one. */
    CORPUS_JSONLIKE = 1,
} CorpusMode;

static Corpus corpus_build(CorpusMode mode, usize random_count) {
    usize specials = sizeof(SPECIALS) / sizeof(SPECIALS[0]);
    usize total = random_count + specials;
    f64* v = (f64*)malloc(total * sizeof(f64));
    if (v == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    usize n = 0;
    for (usize i = 0; i < specials; i++) {
        v[n++] = SPECIALS[i];
    }
    while (n < total) {
        u64 r = splitmix64();
        f64 d;
        if (mode == CORPUS_RANDOM) {
            d = from_bits(r);
        } else {
            switch (r & 7u) {
            case 0:
            case 1:
            case 2:
                d = (f64)(r >> 8) - 4503599627370496.0; /* small integers */
                break;
            case 3:
            case 4:
                d = (f64)(r % 1000000000000000ull) / 1000.0; /* 3 decimals */
                break;
            case 5:
                d = (f64)(i64)(r % 2000000000ull) / 100.0 * 0.01; /* signed */
                break;
            case 6:
                d = (f64)(r % 1000000ull) * 1e-6; /* small magnitudes */
                break;
            default:
                d = from_bits(r); /* the wild one */
                break;
            }
        }
        /* NaN and the infinities are a different question: their spelling is
         * a language decision ("Infinity" against "inf") and neither library
         * is being asked about it here. */
        if (is_finite(d)) {
            v[n++] = d;
        }
    }

    Corpus c = {v, n};
    return c;
}

/* ------------------------------------------------------------------ */
/* measurement                                                         */
/* ------------------------------------------------------------------ */

static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

typedef struct Result {
    usize fmt_failures;     /* our text, read by an independent parser, is not the value */
    usize parse_failures;   /* correct text, read by our parser, is not the value */
    usize roundtrip_failures; /* the two of ours together are not the value */
    usize len_mismatch;     /* returned length != strlen */
    f64 bytes_per_value;
    u64 ns_fmt;
    u64 ns_parse;
} Result;

/*
 * The three counts answer three different questions, and separating them is
 * the point of the harness: a pair that round-trips through itself can be
 * wrong in a matching way in both directions, which is exactly what one of
 * these two implementations turns out to do.
 *
 * strtod is the oracle. It is not fast and is not what we will ship, but it is
 * correctly rounded, and correctness is what is being asked here.
 */
static Result measure(Impl const* impl, Corpus const* c, char const* const* reference,
                      u32 repeats) {
    Result r;
    memset(&r, 0, sizeof(r));

    char buf[BUF_CAP];
    char* table = (char*)malloc(c->n * BUF_CAP);
    if (table == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    usize total_bytes = 0;
    for (usize i = 0; i < c->n; i++) {
        f64 d = c->v[i];
        char* slot = table + i * BUF_CAP;
        u32 n = impl->fmt(d, slot);

        if ((usize)n != strlen(slot)) {
            r.len_mismatch++;
        }
        total_bytes += strlen(slot);

        /* (1) the formatter, judged by a parser that is not ours */
        if (to_bits(strtod(slot, NULL)) != to_bits(d)) {
            if (r.fmt_failures < 4) {
                fprintf(stderr, "  %s fmt: %.17g -> \"%s\" -> %.17g\n", impl->name, d,
                        slot, strtod(slot, NULL));
            }
            r.fmt_failures++;
        }

        /* (2) the parser, fed text known to be right */
        if (to_bits(impl->parse(reference[i])) != to_bits(d)) {
            if (r.parse_failures < 4) {
                fprintf(stderr, "  %s parse: \"%s\" -> %.17g, want %.17g\n", impl->name,
                        reference[i], impl->parse(reference[i]), d);
            }
            r.parse_failures++;
        }

        /* (3) what a program actually does: format with ours, parse with ours */
        if (to_bits(impl->parse(slot)) != to_bits(d)) {
            r.roundtrip_failures++;
        }
    }
    r.bytes_per_value = (f64)total_bytes / (f64)c->n;

    volatile u32 sink = 0;

    u64 t0 = now_ns();
    for (u32 rep = 0; rep < repeats; rep++) {
        for (usize i = 0; i < c->n; i++) {
            sink ^= impl->fmt(c->v[i], buf);
        }
    }
    u64 t1 = now_ns();
    r.ns_fmt = (t1 - t0) / ((u64)c->n * (u64)repeats);

    t0 = now_ns();
    for (u32 rep = 0; rep < repeats; rep++) {
        for (usize i = 0; i < c->n; i++) {
            sink ^= (u32)to_bits(impl->parse(table + i * BUF_CAP));
        }
    }
    t1 = now_ns();
    r.ns_parse = (t1 - t0) / ((u64)c->n * (u64)repeats);

    free(table);
    return r;
}

int main(int argc, char** argv) {
    usize corpus_size = 200000;
    u32 repeats = 8;
    if (argc > 1) {
        corpus_size = (usize)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        repeats = (u32)strtoul(argv[2], NULL, 10);
    }

    /*
     * One set of reference strings, produced by the quickjs formatter, used
     * to judge both parsers on identical input. Which formatter it comes from
     * does not matter as long as it is a *correct* one, and the fmt column
     * below is what establishes that.
     */
    char const* mode_names[] = {"random bits (worst case)", "json-like values"};
    CorpusMode modes[] = {CORPUS_RANDOM, CORPUS_JSONLIKE};

    for (usize m = 0; m < 2; m++) {
        Corpus c = corpus_build(modes[m], corpus_size);

        char* refs = (char*)malloc(c.n * BUF_CAP);
        char const** ref_ptrs = (char const**)malloc(c.n * sizeof(char const*));
        if (refs == NULL || ref_ptrs == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        for (usize i = 0; i < c.n; i++) {
            fmt_quickjs(c.v[i], refs + i * BUF_CAP);
            ref_ptrs[i] = refs + i * BUF_CAP;
        }

        printf("== corpus: %s, %zu values, %u repeats\n", mode_names[m], c.n, repeats);
        printf("   oracle: strtod (correctly rounded); parsers fed quickjs-formatted text\n");
        printf("   %-9s %8s %8s %9s %9s %10s %8s %9s\n", "impl", "fmt bad", "parse bad",
               "roundtrip", "len!=str", "bytes/val", "ns fmt", "ns parse");

        for (usize k = 0; k < IMPL_COUNT; k++) {
            Result r = measure(&IMPLS[k], &c, ref_ptrs, repeats);
            printf("   %-9s %8zu %8zu %9zu %9zu %10.2f %8llu %9llu\n", IMPLS[k].name,
                   r.fmt_failures, r.parse_failures, r.roundtrip_failures, r.len_mismatch,
                   r.bytes_per_value, (unsigned long long)r.ns_fmt,
                   (unsigned long long)r.ns_parse);
        }
        printf("\n");

        free(ref_ptrs);
        free(refs);
        free(c.v);
    }
    return 0;
}
