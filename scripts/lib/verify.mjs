// scripts/lib/verify.mjs
//
// The machine-checkable form of "vm/rt do not depend on libc".
//
// Two passes, because they catch different mistakes:
//
//   1. A probe translation unit that includes every public header under
//      -ffreestanding -nostdinc. A header that pulls in a libc header, or
//      that defines an inline function calling outside our prefix, fails
//      here.
//
//   2. Every object compiled from rt/ and vm/. This is where a libc call
//      hidden inside a .c file is caught; the probe alone would miss it,
//      because a call only becomes an undefined symbol once something
//      actually emits it.

import path from "node:path";
import fs from "node:fs";
import { run, which } from "./exec.mjs";
import { mkdirp, collect } from "./util.mjs";
import { compile } from "./cc.mjs";
import { flagsFor } from "./flags.mjs";

/**
 * Everything the runtime is allowed to reference when unresolved.
 *
 * Sanitizer instrumentation injects calls into the sanitizer runtime
 * (__asan_init etc). Those are compiler-injected, not a libc dependency,
 * so they are only whitelisted when the corresponding flag is on.
 */
function allowedPrefixes(opts) {
    /*
     * cvm_/rt_ are a fallback for a symbol the runtime defines but that is
     * not in the current object set (a file excluded from a build, say).
     * The defined-symbol set is what does the real work.
     */
    const base = ["cvm_", "rt_"];

    /*
     * A sanitizer build is not freestanding and does not claim to be: the
     * sanitizer's runtime is linked in, and the compiler writes its entry
     * points into every object it instruments. *Which* ones it writes
     * depends on the compiler version. Clang 21, which development uses,
     * emits only asan_/sanitizer_/sancov_ names; the clang on the CI runner
     * also references __cxa_atexit and __dso_handle, the pair a sanitizer
     * uses to register its exit-time teardown.
     *
     * Those two are the sanitizer's, not the runtime's, and they are allowed
     * only in a sanitizer build. In a release object a reference to
     * __cxa_atexit would be a real libc dependency -- a C runtime has no
     * static destructors -- and it is still a violation there. That is the
     * check this list must not hollow out: the reason to scan the release
     * objects is exactly to catch what the sanitizer's own runtime would
     * otherwise excuse.
     */
    const sanitizerEmitted = ["cxa_atexit", "dso_handle"];
    if (opts.asan) {
        base.push("asan_", "sanitizer_", "sancov_", ...sanitizerEmitted);
    }
    if (opts.tsan) {
        base.push("tsan_", "sanitizer_", "sancov_", ...sanitizerEmitted);
    }
    return base;
}

/**
 * Symbols the platform itself requires, keyed by platform.
 *
 * The distinction from a prefix allowance matters. A prefix allowance
 * says "this family of symbols is expected"; these say "the platform
 * implements something we cannot implement any other way". Each entry has
 * to justify itself, is scoped to one platform, and is announced when it
 * applies, so an exception cannot accumulate quietly.
 *
 * The rejected alternative for each of these was to widen a prefix and
 * hope the audit still notices something. The audit's value is that it is
 * strict, so exceptions are named one at a time.
 */
const PLATFORM_EXCEPTIONS = {
    darwin: [
        {
            symbol: "tlv_bootstrap",
            why:
                "Darwin implements thread-local variable access inside a " +
                "dylib through dyld's TLS bootstrap: the generated code " +
                "loads a function pointer from the TLV descriptor and calls " +
                "it. This is not a libc facility we chose to use, it is how " +
                "the platform implements __thread, and there is no " +
                "alternative short of owning TLS setup ourselves. The " +
                "symbol does not exist on Linux, where the same access is a " +
                "single segment-offset instruction.",
        },
    ],
};

function stripMachOSymbolNoise(sym) {
    // Mach-O C symbols are underscore-prefixed, and sanitizer entry points
    // carry an extra one (___asan_init). nm output also carries a leading
    // "U " type column, which callers drop before this runs.
    return sym.replace(/^_+/, "");
}

/*
 * Split nm's output into the symbols an object defines and the ones it
 * leaves undefined.
 *
 * Lines look like "0000000000000000 T _name" or "                 U _name".
 * The type letter is what distinguishes them.
 */
function symbolsOf(nm, obj) {
    const defined = [];
    const undefined_ = [];

    for (const line of run(nm, [obj]).stdout.split("\n")) {
        const m = line.match(/^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z])\s+(\S+)$/);
        if (!m) continue;
        const [, type, rawName] = m;
        const name = stripMachOSymbolNoise(rawName);
        if (type === "U") undefined_.push(name);
        else defined.push(name);
    }

    return { defined, undefined: undefined_ };
}

/**
 * Undefined symbols of an object that nothing in the runtime provides.
 *
 * The set of symbols defined by the runtime is built from the objects
 * themselves rather than from a prefix convention. That matters: a vm
 * object legitimately references symbols defined in another vm object
 * (block_list_push, say), and a prefix list cannot tell those apart from a
 * libc dependency. Comparing against what the runtime actually defines is
 * both stricter and more accurate.
 */
function foreignSymbols(nm, obj, opts, runtimeDefines, report) {
    const allowed = allowedPrefixes(opts);
    const exceptions = PLATFORM_EXCEPTIONS[process.platform] ?? [];

    return symbolsOf(nm, obj).undefined.filter((s) => {
        if (runtimeDefines.has(s)) return false;
        if (allowed.some((p) => s.startsWith(p))) return false;

        const exception = exceptions.find((e) => e.symbol === s);
        if (exception) {
            if (report) report(exception);
            return false;
        }
        return true;
    });
}

/** Wrap text to a column, for readable build output. */
function wrap(text, width) {
    const words = text.split(/\s+/);
    const lines = [];
    let line = "";
    for (const w of words) {
        if (line.length > 0 && line.length + 1 + w.length > width) {
            lines.push(line);
            line = w;
        } else {
            line = line.length > 0 ? `${line} ${w}` : w;
        }
    }
    if (line.length > 0) lines.push(line);
    return lines;
}

/**
 * Check that every function the public headers promise actually exists.
 *
 * A declaration with no definition is worse than a missing feature. A
 * frontend reads the header, writes a call, and finds out at link time; if
 * it is a large program, it finds out after a build. This happened:
 * `cvm_gc_scan_frame` and `cvm_gc_scan_object` sat in cvm/gc.h for most of
 * the project's life with no definition anywhere, and nothing noticed
 * because nothing called them.
 *
 * "Exists" means one of three things, and all three are legitimate:
 *
 *   - a symbol defined in one of the compiled runtime objects
 *   - an inline function defined in a header (the allocation fast path, the
 *     frame push and pop, the metadata accessors)
 *   - a macro
 *
 * Anything else is a promise the runtime does not keep.
 */
export function checkPublicAbi({ root, runtimeObjs, opts, ui, nm, verbose }) {
    const headerDir = path.join(root, "vm", "include", "cvm");
    const headers = fs.existsSync(headerDir)
        ? fs
              .readdirSync(headerDir)
              .filter((f) => f.endsWith(".h"))
              .map((f) => path.join(headerDir, f))
        : [];

    if (headers.length === 0) {
        return true;
    }

    const defined = new Set();
    for (const obj of runtimeObjs) {
        const r = run(nm, ["--defined-only", obj], { allowFail: true });
        for (const line of r.stdout.split("\n")) {
            const m = line.match(/^\S*\s*[A-Za-z]\s+(\S+)$/);
            if (m) defined.add(stripMachOSymbolNoise(m[1]));
        }
    }

    /*
     * Words that mean the matched line is not a declaration. Without this,
     * `return cvm_alloc_sized(type, size);` inside a body reads as a
     * declaration of cvm_alloc_sized, and the audit reports a function that
     * is in fact defined three lines above.
     */
    const NOT_A_DECLARATION = new Set([
        "return", "goto", "sizeof", "if", "while", "for", "switch", "case",
        "else", "do", "break", "continue", "typedef",
    ]);

    /* A return type, then the name. Prefix words are checked separately. */
    const DECL = /^\s*((?:[A-Za-z_][A-Za-z0-9_]*[ \t*]+)+)(cvm_[a-z0-9_]+)\s*\(/;

    /*
     * Whether a signature starting at `start` opens a body or ends in a
     * semicolon. A signature can span lines, so the answer is the first
     * brace or semicolon found looking forward — and a brace that comes
     * first means a definition, which needs no symbol in an object file.
     */
    function bodyOrSemicolon(lines, start) {
        const limit = Math.min(lines.length, start + 8);
        for (let i = start; i < limit; i++) {
            const brace = lines[i].indexOf("{");
            const semi = lines[i].indexOf(";");
            if (brace !== -1 && (semi === -1 || brace < semi)) return "body";
            if (semi !== -1) return "semicolon";
        }
        return "unknown";
    }

    const inlineDefined = new Set();
    const macros = new Set();
    const missing = [];

    for (const header of headers) {
        const text = fs.readFileSync(header, "utf8");
        const lines = text.split("\n");
        const rel = path.relative(root, header);

        for (let i = 0; i < lines.length; i++) {
            const macro = lines[i].match(
                /^\s*#\s*define\s+(cvm_[a-z0-9_]+)\s*\(/,
            );
            if (macro) macros.add(macro[1]);

            if (/^\s*(\*|\/\*|#)/.test(lines[i])) continue;

            const m = lines[i].match(DECL);
            if (!m) continue;

            const prefixWords = m[1].trim().split(/[ \t*]+/).filter(Boolean);
            if (prefixWords.some((w) => NOT_A_DECLARATION.has(w))) continue;
            if (prefixWords.length === 0) continue;

            const name = m[2];
            const kind = bodyOrSemicolon(lines, i);

            if (kind === "body") {
                inlineDefined.add(name);
                continue;
            }
            if (kind !== "semicolon") continue;
            if (macros.has(name) || inlineDefined.has(name)) continue;
            if (defined.has(name)) continue;

            missing.push({ name, header: rel, line: i + 1 });
        }
    }

    if (missing.length > 0) {
        console.error("[verify] public ABI: declared but never defined");
        for (const m of missing) {
            console.error(`         ${m.name}   (${m.header}:${m.line})`);
        }
        ui.fatal(
            "a frontend would link against a function the runtime does not " +
                "provide; define it, or drop the declaration",
        );
    }

    const lines = headers.reduce(
        (n, h) => n + fs.readFileSync(h, "utf8").split("\n").length,
        0,
    );
    ui.log(
        `verify: public ABI ok (${headers.length} header(s), ${lines} lines, ` +
            `${inlineDefined.size} inline, ${defined.size} symbol(s))`,
    );
    return true;
}

/**
 * Check for runtime functions that nothing calls.
 *
 * A function with no caller is not always wrong — it may be part of an
 * interface a caller outside this repository uses. But in this project every
 * such function so far has been a leftover, and there is no external caller:
 * the runtime's users are generated code and the test suite, both of which
 * are here. So nothing calling a function is worth failing the build over,
 * with named exceptions for the cases that genuinely have no in-tree caller.
 *
 * This found ten functions. The pattern they shared is worth naming, because
 * it is how they accumulated: each was written slightly ahead of the thing
 * that would use it — a channel getter beside a channel setter, heap
 * accessors beside direct field access, a semaphore's counter and try-wait
 * beside the two operations the pool actually needs. Writing one half of an
 * interface early is not unreasonable; leaving the half in place after
 * choosing a different spelling is what has to be caught.
 *
 * The analysis is textual, and a call is recognised as a line where the name
 * is followed by a paren and the text before it is not a declaration. That
 * is enough for this codebase because nothing calls a runtime function
 * through a function pointer. It would need to become real parsing if that
 * ever changed.
 */
export function checkNoDeadFunctions({ root, ui }) {
    /*
     * Functions that legitimately have no in-tree caller. Each entry says
     * why, because an allowlist without reasons is how a dead-code check
     * becomes a formality.
     */
    const ALLOWED = new Map([
        // "Whether this deque is empty" is an answer anyone may ask and a
        // promise nobody can make: the owner or a thief can change it the
        // moment it is read. Its caller is the concurrent-mark termination
        // check (t41), which does not exist yet; it is kept now because it
        // is the one lock-free half of that check and is exercised by the
        // worklist tests alongside the halves that do have callers.
        ["cvm_worklist_empty", "t41: termination check reads it"],
    ]);

    const runtimeSrcs = [
        ...collect(path.join(root, "vm"), [".c", ".h", ".S"]),
        ...collect(path.join(root, "rt"), [".c", ".h", ".S"]),
    ];
    const callers = [
        ...runtimeSrcs,
        ...collect(path.join(root, "tests"), [".c"]),
        ...collect(path.join(root, "bench"), [".c"]),
    ].filter((f) => fs.existsSync(f));

    const text = new Map(callers.map((f) => [f, fs.readFileSync(f, "utf8")]));

    /* Functions the runtime declares or defines. */
    const candidates = new Set();
    const DECL_RE =
        /^\s*(?!typedef)(?:static\s+|CVM_INLINE\s+|CVM_NOINLINE\s+)*[A-Za-z_][A-Za-z0-9_ *]*\s+\**((?:cvm|rt)_[a-z0-9_]+)\s*\(/;
    const MACRO_RE = /^\s*#\s*define\s+((?:cvm|rt)_[A-Za-z0-9_]+)\s*\(/;

    for (const f of runtimeSrcs) {
        for (const line of text.get(f).split("\n")) {
            const m = line.match(DECL_RE);
            if (m) candidates.add(m[1]);
            const mm = line.match(MACRO_RE);
            if (mm) candidates.add(mm[1]);
        }
    }

    /**
     * Whether a line is a declaration of `name` rather than a use of it.
     *
     * The test is what precedes the name: if it is only type-ish words, this
     * is a prototype; if it contains a return, an assignment, a control
     * keyword or a delimiter, it is a use inside a body.
     */
    function isDeclaration(line, name) {
        const idx = line.indexOf(name + "(");
        if (idx < 0) return false;
        const before = line.slice(0, idx).trim();
        if (before.length === 0) return false;
        if (/[=;,(]|return|goto|\bif\b|\bwhile\b|\bfor\b|\bswitch\b|\bsizeof\b/.test(before)) {
            return false;
        }
        return /^[A-Za-z_][A-Za-z0-9_ *]*$/.test(before);
    }

    const dead = [];
    for (const name of candidates) {
        if (ALLOWED.has(name)) continue;

        let uses = 0;
        for (const f of callers) {
            const body = text.get(f);
            if (f.endsWith(".S")) {
                /* Assembly: the symbol appears, possibly bare. */
                if (new RegExp(`\\b_?${name}\\b`).test(body)) uses++;
                continue;
            }
            for (const line of body.split("\n")) {
                if (!line.includes(name + "(")) continue;
                const trimmed = line.trim();
                if (trimmed.startsWith("*") || trimmed.startsWith("/*")) continue;
                if (isDeclaration(line, name)) continue;
                uses++;
            }
        }
        if (uses === 0) dead.push(name);
    }

    if (dead.length > 0) {
        console.error("[verify] these runtime functions have no caller:");
        for (const name of dead) console.error(`         ${name}`);
        ui.fatal(
            "delete them, or add them to ALLOWED in checkNoDeadFunctions " +
                "with a reason",
        );
    }

    ui.log(`verify: no dead functions (${candidates.size} checked)`);
    return true;
}

/**
 * Headers the probe covers. Public headers belong here; internal headers
 * are covered by pass 2 as soon as a .c file includes them.
 */
const PROBE_HEADERS = [
    "base/types.h",
    "base/math.h",
    "base/alloc.h",
    "base/assert.h",
    "base/format.h",
    "base/sort.h",
    "rt.h",
    "log.h",
    "stats.h",
    "context.h",
    "safepoint.h",
    "cvm/gc.h",
    "cvm/array.h",
    "cvm/builder.h",
    "cvm/buf.h",
    "cvm/hash.h",
    "cvm/intern.h",
    "cvm/json.h",
    "cvm/str.h",
    "cvm/unicode.h",
    "cvm/vm.h",
    "gc/layout.h",
    "gc/meta.h",
    "gc/bytemap.h",
    "gc/heap.h",
    "gc/block_range.h",
    "gc/block_alloc.h",
    "gc/alloc.h",
    "gc/large.h",
    "gc/world.h",
    "gc/grey_packet.h",
    "gc/marker.h",
    "cvm/weak.h",
    "gc/sweeper.h",
    "gc/pool.h",
    "gc/collect.h",
    "cvm/roots.h",
    "cvm/external.h",
    "gc/pool.h",
    "gc/block_list.h",
];

export function checkFreestanding({
    root,
    buildDir,
    opts,
    toolchain,
    verbose,
    ui,
    runtimeObjs = [],
}) {
    const tmp = path.join(buildDir, "tmp");
    mkdirp(tmp);

    const probe = path.join(tmp, "freestanding_probe.c");
    fs.writeFileSync(
        probe,
        [
            ...PROBE_HEADERS.map((h) => `#include "${h}"`),
            "",
            "/* Declares nothing, calls nothing: a clean probe has zero",
            " * undefined symbols. */",
            "void cvm_build_probe(void) {}",
            "",
        ].join("\n"),
    );

    const probeObj = compile({
        src: probe,
        obj: path.join(tmp, "freestanding_probe.o"),
        flags: flagsFor({ flavor: "freestanding", opts, toolchain, root }),
        toolchain,
        verbose,
        ui,
    });

    const nm = which("nm") || which("llvm-nm");
    if (!nm) {
        ui.log("verify: nm not found, symbol scan skipped");
        return true;
    }

    const applied = new Map();
    const report = (exception) => applied.set(exception.symbol, exception);

    const objects = [probeObj, ...runtimeObjs];

    /* Everything the runtime provides, so that cross-object references
     * inside vm/ and rt/ are not mistaken for external dependencies. */
    const runtimeDefines = new Set();
    for (const obj of objects) {
        for (const name of symbolsOf(nm, obj).defined) {
            runtimeDefines.add(name);
        }
    }

    const violations = [];
    for (const obj of objects) {
        const foreign = foreignSymbols(nm, obj, opts, runtimeDefines, report);
        if (foreign.length > 0) violations.push({ obj, foreign });
    }

    for (const exception of applied.values()) {
        ui.log(`verify: platform exception ${exception.symbol}`);
        for (const line of wrap(exception.why, 72)) ui.log(`        ${line}`);
    }

    if (violations.length > 0) {
        console.error("[verify] freestanding violation: foreign symbols");
        for (const v of violations) {
            console.error(`         ${path.relative(root, v.obj)}`);
            for (const s of v.foreign) console.error(`           ${s}`);
        }
        ui.fatal("vm/rt must not depend on libc");
    }

    const scanned = 1 + runtimeObjs.length;
    ui.log(`verify: freestanding ok (${scanned} object(s) scanned, 0 foreign)`);
    return true;
}

/**
 * Inspect a linked shared object for undefined symbols outside our prefixes.
 * Wired in once vm.so / rt.so are actually produced (entry-glue step); kept
 * here so the check lives next to the other one.
 */
export function checkSharedObject(soPath, opts) {
    const nm = which("nm") || which("llvm-nm");
    if (!nm || !fs.existsSync(soPath)) return null;
    return foreignSymbols(nm, soPath, opts);
}
