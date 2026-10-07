// scripts/lib/verify.mjs
//
// The machine-checkable form of "vm/rt do not depend on libc".
//
// Method: compile a probe that includes every public header under
// -ffreestanding -nostdinc, then inspect the object's undefined symbols.
// A freestanding translation unit that only declares (never calls) must
// reference nothing at all. Any undefined symbol is a leaked dependency.
//
// Note this catches header-level leakage (a libc include, an inline function
// calling something outside our prefix). It cannot catch a libc call that
// sits inside a .c file, because that only shows up at link time — that is
// what the shared-object symbol scan in checkSharedObject() is for.

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

function stripMachOSymbolNoise(sym) {
    // Mach-O C symbols are underscore-prefixed, and sanitizer entry points
    // carry an extra one (___asan_init). nm output also carries a leading
    // "U " type column, which callers drop before this runs.
    return sym.replace(/^_+/, "");
}

export function checkFreestanding({ root, buildDir, opts, toolchain, verbose, ui }) {
    const tmp = path.join(buildDir, "tmp");
    mkdirp(tmp);

    const probe = path.join(tmp, "freestanding_probe.c");
    fs.writeFileSync(
        probe,
        [
            '#include "base/types.h"',
            '#include "cvm/gc.h"',
            "",
            "/* Declares nothing, calls nothing: a clean probe has zero",
            " * undefined symbols. */",
            "void cvm_build_probe(void) {}",
            "",
        ].join("\n"),
    );

    const obj = compile({
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

    const r = run(nm, ["-u", obj]);
    const syms = r.stdout
        .split("\n")
        .map((l) => l.trim())
        .filter(Boolean)
        .map((l) => stripMachOSymbolNoise(l.replace(/^U\s+/, "")));

    const allowed = allowedPrefixes(opts);
    const leaked = syms.filter((s) => !allowed.some((p) => s.startsWith(p)));

    if (leaked.length > 0) {
        console.error("[verify] freestanding violation: foreign symbols in headers");
        for (const s of leaked) console.error(`         ${s}`);
        ui.fatal("vm/rt headers must not depend on libc");
    }

    ui.log(`verify: freestanding ok (${syms.length} undefined symbols, all cvm_/rt_)`);
    return true;
}

/**
 * Inspect a linked shared object for undefined symbols outside our prefixes.
 * This is where a libc call hidden inside a .c file gets caught.
 */
export function checkSharedObject(soPath, opts, ui) {
    const nm = which("nm") || which("llvm-nm");
    if (!nm || !fs.existsSync(soPath)) return null;

    const allowed = allowedPrefixes(opts);
    const leaked = run(nm, ["-u", soPath])
        .stdout.split("\n")
        .map((l) => l.trim())
        .filter(Boolean)
        .map((l) => stripMachOSymbolNoise(l.replace(/^U\s+/, "")))
        .filter((s) => !allowed.some((p) => s.startsWith(p)));

    return leaked;
}
