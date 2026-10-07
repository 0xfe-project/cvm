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
import { mkdirp } from "./util.mjs";
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
    const base = ["cvm_", "rt_"];
    if (opts.asan) base.push("asan_", "sanitizer_", "sancov_");
    if (opts.tsan) base.push("tsan_", "sanitizer_", "sancov_");
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

/** Undefined symbols of an object that are not ours. */
function foreignSymbols(nm, obj, opts, report) {
    const allowed = allowedPrefixes(opts);
    const exceptions = PLATFORM_EXCEPTIONS[process.platform] ?? [];

    return run(nm, ["-u", obj])
        .stdout.split("\n")
        .map((l) => l.trim())
        .filter(Boolean)
        .map((l) => stripMachOSymbolNoise(l.replace(/^U\s+/, "")))
        .filter((s) => {
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
 * Headers the probe covers. Public headers belong here; internal headers
 * are covered by pass 2 as soon as a .c file includes them.
 */
const PROBE_HEADERS = [
    "base/types.h",
    "base/math.h",
    "base/assert.h",
    "base/format.h",
    "rt.h",
    "log.h",
    "stats.h",
    "context.h",
    "safepoint.h",
    "cvm/gc.h",
    "gc/layout.h",
    "gc/meta.h",
    "gc/bytemap.h",
    "gc/heap.h",
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

    const violations = [];
    for (const obj of [probeObj, ...runtimeObjs]) {
        const foreign = foreignSymbols(nm, obj, opts, report);
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
