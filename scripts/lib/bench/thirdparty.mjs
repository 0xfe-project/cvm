// scripts/lib/bench/thirdparty.mjs
//
// Builds the third-party collectors the benchmark compares against.
//
// Two of them, and they could hardly be more different in how they are
// built:
//
//   bdwgc   a full autotools release tree. Configured out-of-tree into
//           bench/build/bdwgc so the vendored sources stay untouched, and
//           built at -O1 to match the rule in bench/BENCH.md §2.1.
//
//   tgc     two files. Compiled directly.
//
// Both are static, because the scenario binaries link a collector into
// themselves — a shared library would add loader and PLT costs to a
// measurement that is about the collector.

import fs from "node:fs";
import path from "node:path";
import os from "node:os";

import { run, runOrDie, which } from "../exec.mjs";
import { mkdirp } from "../util.mjs";

/**
 * The optimization level every C and C++ backend is built at.
 *
 * -O1 rather than -O2 because the C that a frontend generates is not
 * hand-optimized, and this is closer to what generated code would be built
 * with. Go has no equivalent knob and uses its default; see BENCH.md §2.1
 * for why that is stated rather than papered over.
 */
export const BENCH_OPT = "-O1";

/** Jobs for the third-party builds. */
function jobs() {
    const n = typeof os.availableParallelism === "function"
        ? os.availableParallelism()
        : os.cpus().length;
    return String(Math.max(1, n));
}

/* ------------------------------------------------------------------ */
/* bdwgc                                                              */
/* ------------------------------------------------------------------ */

const BDWGC_SRC = "bench/bdwgc/gc-8.2.12";
const BDWGC_BUILD = "bench/build/bdwgc";

/**
 * Where bdwgc's static library ends up, and whether it is there.
 *
 * `libgc.a` under `.libs` is what libtool produces for a static build. Read
 * from the build tree rather than `make install`-ed somewhere: an install
 * step would copy headers into a prefix and give the benchmark a second
 * copy of the truth to get out of sync.
 */
export function bdwgcLibrary(root) {
    return path.join(root, BDWGC_BUILD, ".libs", "libgc.a");
}

export function bdwgcIncludeDirs(root) {
    return [
        path.join(root, BDWGC_SRC, "include"),
        path.join(root, BDWGC_BUILD, "include"),
    ];
}

export function buildBdwgc({ root, ui, verbose }) {
    const src = path.join(root, BDWGC_SRC);
    const build = path.join(root, BDWGC_BUILD);
    const lib = bdwgcLibrary(root);

    if (!fs.existsSync(src)) {
        /*
         * The tree is vendored but not committed — it is a release tarball
         * and belongs in our history no more than refs/ does. So a missing
         * tree is a "you have not unpacked it yet" and the message says
         * where from.
         */
        const hint = path.join(root, "bench/bdwgc/README.md");
        const text = fs.existsSync(hint) ? fs.readFileSync(hint, "utf8").trim() : "";
        ui.fatal(`bdwgc sources are not present at ${BDWGC_SRC}\n        ${text}`);
    }

    const makefile = path.join(build, "Makefile");
    if (fs.existsSync(lib) && fs.existsSync(makefile)) {
        ui.log("bdwgc: already built");
        return lib;
    }

    if (!which("make")) {
        ui.fatal("bdwgc: make is required");
    }

    mkdirp(build);

    if (!fs.existsSync(makefile)) {
        ui.log(`bdwgc: configuring (${BENCH_OPT}, static, no C++)`);
        runOrDie(
            path.join(src, "configure"),
            [
                `--prefix=${build}`,
                "--disable-shared",
                "--enable-static",
                // We benchmark the C API; the C++ layer would add a library
                // nobody calls here.
                "--enable-cplusplus=no",
                "--disable-dependency-tracking",
                // Explicit rather than defaulted: the thread scenarios need
                // thread registration, and a silently single-threaded bdwgc
                // would make them meaningless.
                "--enable-threads=posix",
                `CFLAGS=${BENCH_OPT} -g0`,
            ],
            { cwd: build, verbose, ui, what: "bdwgc configure" },
        );
    }

    ui.log(`bdwgc: building (-j${jobs()})`);
    runOrDie("make", [`-j${jobs()}`], {
        cwd: build,
        verbose,
        ui,
        what: "bdwgc make",
    });

    if (!fs.existsSync(lib)) {
        ui.fatal(`bdwgc: build finished but ${lib} is missing`);
    }
    ui.log(`bdwgc: ${path.relative(root, lib)}`);
    return lib;
}

/* ------------------------------------------------------------------ */
/* tgc                                                                */
/* ------------------------------------------------------------------ */

const TGC_SRC = "bench/tgc/gc";
const TGC_BUILD = "bench/build/tgc";

export function tgcObject(root) {
    return path.join(root, TGC_BUILD, "tgc.o");
}

export function tgcIncludeDir(root) {
    return path.join(root, TGC_SRC);
}

export function buildTgc({ root, toolchain, ui, verbose }) {
    const src = path.join(root, TGC_SRC, "tgc.c");
    const obj = tgcObject(root);

    if (!fs.existsSync(src)) {
        ui.fatal(`tgc sources are not present at ${TGC_SRC}`);
    }

    mkdirp(path.dirname(obj));

    /*
     * Rebuilt whenever the source is newer, which for a two-file library is
     * simpler than asking ccache to notice. tgc is small enough that a
     * rebuild costs nothing.
     */
    if (fs.existsSync(obj) &&
        fs.statSync(obj).mtimeMs >= fs.statSync(src).mtimeMs) {
        ui.log("tgc: already built");
        return obj;
    }

    ui.log(`tgc: compiling (${BENCH_OPT})`);
    runOrDie(
        toolchain.cc,
        [
            "-std=c11",
            BENCH_OPT,
            "-g0",
            "-c",
            src,
            "-o",
            obj,
        ],
        { verbose, ui, what: "tgc compile" },
    );

    ui.log(`tgc: ${path.relative(root, obj)}`);
    return obj;
}

/* ------------------------------------------------------------------ */
/* all of them                                                        */
/* ------------------------------------------------------------------ */

export function buildThirdParty({ root, toolchain, ui, verbose }) {
    const bdwgc = buildBdwgc({ root, ui, verbose });
    const tgc = buildTgc({ root, toolchain, ui, verbose });
    return { bdwgc, tgc };
}
