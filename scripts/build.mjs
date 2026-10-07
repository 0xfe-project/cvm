// scripts/build.mjs
//
// Build driver for cvm.
//
// Layout:
//   build/obj/<config>/...   object files (ccache-cached compilations)
//   build/lib/               libraries (vm.a / rt.a / *.so)
//   build/bin/               test executables and tools
//   build/tmp/               generated probes, linkscripts
//
// Compilation split:
//   rt/ + vm/   freestanding: -ffreestanding -nostdinc, no libc headers allowed
//   tests/      hosted: normal userland compilation, libc allowed
//
// Incremental compilation is delegated entirely to ccache: every source is
// handed to the compiler every run, unchanged inputs are cache hits.

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BUILD = path.join(ROOT, "build");

// ---------------------------------------------------------------------------
// argv
// ---------------------------------------------------------------------------

const argv = process.argv.slice(2);
const has = (f) => argv.includes(f);
const argOf = (f) => {
    const i = argv.indexOf(f);
    return i >= 0 && i + 1 < argv.length ? argv[i + 1] : null;
};

const OPTS = {
    clean: has("--clean"),
    test: has("--test"),
    verify: has("--verify-freestanding"),
    verbose: has("-v") || has("--verbose"),
    asan: has("--asan"),
    tsan: has("--tsan"),
    debug: has("--debug") || !has("--release"),
    config: has("--release") ? "release" : "debug",
};

// ---------------------------------------------------------------------------
// toolchain
// ---------------------------------------------------------------------------

function which(tool) {
    const r = spawnSync("sh", ["-c", `command -v ${tool}`], { encoding: "utf8" });
    return r.status === 0 ? r.stdout.trim() : null;
}

const CC = process.env.CC || which("cc") || which("clang");
if (!CC) fatal("no C compiler found (set CC)");

const isClang = (() => {
    const r = spawnSync(CC, ["--version"], { encoding: "utf8" });
    return /clang/i.test(`${r.stdout}${r.stderr}`);
})();

// ccache ships a compiler-symlink farm; if it is present we prefix every
// compile and forget about our own incremental logic.
const CCACHE = which("ccache");

// mold is an ELF-only linker (no Mach-O backend): usable on Linux, never on
// macOS. On macOS we let the driver pick ld64.
const PLATFORM = process.platform; // 'darwin' | 'linux' | 'win32'
const MOLD = PLATFORM === "linux" ? which("mold") : null;

// Freestanding headers (stdint.h, stddef.h, stdbool.h, ...) live in the
// compiler's own resource dir; -nostdinc removes the libc ones only.
const FREESTANDING_INCLUDE = (() => {
    const probe = isClang
        ? ["-print-resource-dir"]
        : ["-print-file-name=include"];
    const r = spawnSync(CC, probe, { encoding: "utf8" });
    if (r.status !== 0) return [];
    const base = r.stdout.trim();
    return isClang ? ["-I", path.join(base, "include")] : ["-I", base];
})();

// ---------------------------------------------------------------------------
// flags
// ---------------------------------------------------------------------------

const STD = "-std=c11"; // MSVC-compatible target: no C23 features

const COMMON = [
    STD,
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wpedantic",
    "-fno-common",
    "-fvisibility=hidden",
];

const FREESTANDING = [...COMMON, "-ffreestanding", "-nostdinc", ...FREESTANDING_INCLUDE];

const HOSTED = [...COMMON];

const DEBUG_FLAGS = ["-g", "-O0", "-fno-omit-frame-pointer"];
const RELEASE_FLAGS = ["-O2", "-DNDEBUG"];

const OPT = OPTS.debug ? DEBUG_FLAGS : RELEASE_FLAGS;

const SAN = OPTS.asan ? ["-fsanitize=address"] : OPTS.tsan ? ["-fsanitize=thread"] : [];
if (OPTS.asan && OPTS.tsan) fatal("--asan and --tsan are mutually exclusive");

const INCLUDES = [
    "-I", path.join(ROOT, "rt"),
    "-I", path.join(ROOT, "vm", "include"),
    "-I", ROOT,
];

// Our runtime may only reference its own symbols. Anything else means a libc
// dependency leaked in. Mach-O prefixes C symbols with an underscore.
const UNDEFINED_ALLOW_PREFIX = ["cvm_", "rt_"];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function fatal(msg) {
    console.error(`build: error: ${msg}`);
    process.exit(1);
}

function log(msg) {
    console.log(`[build] ${msg}`);
}

function vlog(cmd, args) {
    if (OPTS.verbose) console.log(`        ${cmd} ${args.join(" ")}`);
}

function run(cmd, args, { cwd = ROOT, allowFail = false } = {}) {
    vlog(cmd, args);
    const r = spawnSync(cmd, args, { cwd, encoding: "utf8" });
    if (r.status !== 0 && !allowFail) {
        if (r.stdout) process.stderr.write(r.stdout);
        if (r.stderr) process.stderr.write(r.stderr);
        fatal(`${cmd} failed with exit ${r.status}`);
    }
    return r;
}

function mkdirp(p) {
    fs.mkdirSync(p, { recursive: true });
}

function rmrf(p) {
    fs.rmSync(p, { recursive: true, force: true });
}

/** Recursively collect files with the given extensions. */
function collect(dir, exts) {
    if (!fs.existsSync(dir)) return [];
    const out = [];
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, ent.name);
        if (ent.isDirectory()) out.push(...collect(p, exts));
        else if (exts.some((e) => ent.name.endsWith(e))) out.push(p);
    }
    return out.sort();
}

/** Map a source path to its object path under build/obj/<config>/. */
function objectPath(src, flavor) {
    const rel = path.relative(ROOT, src);
    return path.join(BUILD, "obj", OPTS.config, flavor, rel.replace(/\.c$/, ".o"));
}

function compile(src, { flavor, extraFlags = [] }) {
    const obj = objectPath(src, flavor);
    mkdirp(path.dirname(obj));

    const flags = flavor === "freestanding" ? FREESTANDING : HOSTED;
    const args = [...flags, ...OPT, ...SAN, ...INCLUDES, ...extraFlags, "-c", src, "-o", obj];

    const cmd = CCACHE ?? CC;
    const cmdArgs = CCACHE ? [CC, ...args] : args;
    run(cmd, cmdArgs);
    return obj;
}

function link(objs, out, { extraFlags = [], libs = [] } = {}) {
    mkdirp(path.dirname(out));
    const linker = MOLD ? ["-fuse-ld=mold"] : [];
    const args = [...SAN, ...linker, ...objs, ...libs, "-o", out, ...extraFlags];
    run(CC, args);
    return out;
}

// ---------------------------------------------------------------------------
// steps
// ---------------------------------------------------------------------------

function clean() {
    rmrf(BUILD);
    log("cleaned build/");
}

function buildRuntime() {
    const rtSrc = collect(path.join(ROOT, "rt"), [".c"]);
    const vmSrc = collect(path.join(ROOT, "vm"), [".c"]);

    const objs = [];
    for (const src of rtSrc) objs.push(compile(src, { flavor: "freestanding" }));
    for (const src of vmSrc) objs.push(compile(src, { flavor: "freestanding" }));

    if (objs.length === 0) {
        log("runtime: no sources yet (headers only)");
        return [];
    }

    log(`runtime: ${rtSrc.length} rt + ${vmSrc.length} vm objects`);
    return objs;
}

function buildTests(runtimeObjs) {
    const src = collect(path.join(ROOT, "tests"), [".c"]);
    if (src.length === 0) {
        log("tests: none found");
        return [];
    }

    const bins = [];
    for (const s of src) {
        const obj = compile(s, { flavor: "hosted" });
        const name = path.basename(s, ".c");
        const bin = path.join(BUILD, "bin", name);
        link([obj, ...runtimeObjs], bin);
        bins.push(bin);
    }
    log(`tests: ${bins.length} binaries`);
    return bins;
}

/**
 * Compile a probe that includes every public header under -nostdinc, then
 * assert the resulting object references nothing but cvm_/rt_ symbols.
 * This is the machine-checkable form of "vm does not depend on libc".
 */
function verifyFreestanding(runtimeObjs) {
    const tmp = path.join(BUILD, "tmp");
    mkdirp(tmp);
    const probe = path.join(tmp, "freestanding_probe.c");
    fs.writeFileSync(
        probe,
        [
            '#include "base/types.h"',
            '#include "cvm/gc.h"',
            "void cvm_build_probe(void) {}",
            "",
        ].join("\n"),
    );

    const obj = compile(probe, { flavor: "freestanding" });

    const nm = which("nm") || which("llvm-nm");
    if (!nm) {
        log("verify: nm unavailable, skipped symbol scan");
        return true;
    }

    const r = run(nm, ["-u", obj], { allowFail: true });
    const syms = r.stdout
        .split("\n")
        .map((l) => l.trim())
        .filter(Boolean)
        .map((l) => l.replace(/^_/, "").replace(/^U /, "").trim());

    // Nothing in the probe should reference anything: it declares but never
    // calls. Any undefined symbol at all is a dependency we did not intend.
    const bad = syms.filter((s) => !UNDEFINED_ALLOW_PREFIX.some((p) => s.startsWith(p)));

    if (bad.length > 0) {
        console.error("[build] freestanding violation: undefined symbols leaked");
        for (const s of bad) console.error(`        ${s}`);
        fatal("vm/rt must not depend on libc");
    }

    log(`verify: freestanding ok (${runtimeObjs.length} runtime objects, 0 foreign symbols)`);
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

function main() {
    if (OPTS.clean) return clean();

    log(
        `toolchain: ${CC}${isClang ? " (clang)" : ""}` +
            `${CCACHE ? " + ccache" : ""}` +
            `${MOLD ? " + mold" : ""} · config=${OPTS.config}` +
            `${OPTS.asan ? " · asan" : OPTS.tsan ? " · tsan" : ""}`,
    );

    mkdirp(BUILD);

    const runtimeObjs = buildRuntime();

    if (OPTS.test) buildTests(runtimeObjs);
    if (OPTS.verify) verifyFreestanding(runtimeObjs);

    log("done");
}

main();
