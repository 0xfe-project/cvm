// scripts/build.mjs
//
// Orchestration only. Everything substantive lives in scripts/lib/:
//
//   args.mjs       command-line parsing
//   toolchain.mjs  compiler / ccache / linker discovery
//   flags.mjs      flag sets for the freestanding and hosted flavors
//   cc.mjs         compile and link primitives
//   verify.mjs     the "no libc leaked" checks
//   util.mjs       logging and filesystem helpers
//   exec.mjs       subprocess helpers
//
// This file decides *what* to build and in what order, nothing else.
//
// Incremental compilation is delegated entirely to ccache: every source is
// handed to the compiler on every run, unchanged inputs are cache hits.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { parseArgs, help } from "./lib/args.mjs";
import { logger, mkdirp, rmrf, collect } from "./lib/util.mjs";
import { discover } from "./lib/toolchain.mjs";
import { which } from "./lib/exec.mjs";
import { flagsFor, sanitizerFlags } from "./lib/flags.mjs";
import { compile, link, objectPath } from "./lib/cc.mjs";
import {
    checkFreestanding,
    checkPublicAbi,
    checkNoDeadFunctions,
} from "./lib/verify.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BUILD = path.join(ROOT, "build");

const ui = logger("build");
const opts = parseArgs(process.argv.slice(2));

if (process.argv.includes("--help") || process.argv.includes("-h")) {
    console.log(help.usage);
    for (const o of help.options) console.log(`  ${o}`);
    process.exit(0);
}

// ---------------------------------------------------------------------------
// steps
// ---------------------------------------------------------------------------

function clean() {
    rmrf(BUILD);
    ui.log("cleaned build/");
}

/**
 * Compile every source under a directory tree with one flag flavor.
 *
 * `.S` is included because the safepoint trampoline has to be assembly:
 * it runs on a stack the faulting code was using, at a point where no
 * compiler-generated prologue has run, so there is nothing for a C
 * function to save and restore. The `.S` spelling means the file is run
 * through the preprocessor first, which is how a single file can cover
 * two architectures and three platforms.
 */
function compileTree({ dir, flavor, toolchain, verbose }) {
    const srcs = collect(dir, [".c", ".S"]);
    const objs = [];

    for (const src of srcs) {
        const obj = objectPath({
            root: ROOT,
            buildDir: BUILD,
            config: opts.config,
            flavor,
            src,
        });
        objs.push(
            compile({
                src,
                obj,
                flags: flagsFor({ flavor, opts, toolchain, root: ROOT }),
                toolchain,
                verbose,
                ui,
            }),
        );
    }

    return { srcs, objs };
}

/**
 * Compile the two runtime layers.
 *
 * Flavor split: vm/ is freestanding (no libc at all), rt/ is hosted (it is
 * the one layer allowed to use a thin libc, and it is where the platform
 * syscalls live). Headers under rt/base/ are shared with vm, so they stay
 * libc-free regardless — the freestanding probe enforces that.
 */
function buildRuntime(toolchain) {
    const rt = compileTree({
        dir: path.join(ROOT, "rt"),
        flavor: "hosted",
        toolchain,
        verbose: opts.verbose,
    });
    const vm = compileTree({
        dir: path.join(ROOT, "vm"),
        flavor: "freestanding",
        toolchain,
        verbose: opts.verbose,
    });

    if (rt.objs.length + vm.objs.length === 0) {
        ui.log("runtime: no sources yet (headers only)");
        return { objs: [], vmObjs: [] };
    }

    ui.log(`runtime: ${rt.objs.length} rt (hosted) + ${vm.objs.length} vm (freestanding)`);
    return { objs: [...rt.objs, ...vm.objs], vmObjs: vm.objs };
}

function buildTests(toolchain, runtimeObjs) {
    const binDir = path.join(BUILD, "bin");

    /*
     * Wipe the output directory before linking, so a binary whose source was
     * deleted or renamed does not survive as a stale artefact. The test
     * runner runs *every* binary it finds there, so a leftover is not merely
     * untidy: it is executed, and it fails for reasons belonging to a file
     * that no longer exists.
     */
    rmrf(binDir);
    mkdirp(binDir);

    const srcs = collect(path.join(ROOT, "tests"), [".c"]);
    if (srcs.length === 0) {
        ui.log("tests: none found");
        return [];
    }

    const bins = [];
    for (const src of srcs) {
        const obj = compile({
            src,
            obj: objectPath({
                root: ROOT,
                buildDir: BUILD,
                config: opts.config,
                flavor: "hosted",
                src,
            }),
            flags: flagsFor({
                flavor: "hosted",
                opts,
                toolchain,
                root: ROOT,
            }),
            toolchain,
            verbose: opts.verbose,
            ui,
        });

        const bin = path.join(BUILD, "bin", path.basename(src, ".c"));
        link({
            objs: [obj, ...runtimeObjs],
            out: bin,
            toolchain,
            // The sanitizer runtime is linked by the driver, so the same
            // -fsanitize flag must appear on the link line as on the
            // compile line. Omitting it leaves the instrumented objects
            // with undefined __tsan_*/__asan_* symbols.
            flags: sanitizerFlags(opts),
            // Tests are hosted and use pthreads for the concurrent cases.
            // -lpthread is a no-op on macOS and needed on older glibc.
            libs: ["-lpthread"],
            verbose: opts.verbose,
            ui,
        });
        bins.push(bin);
    }

    ui.log(`tests: ${bins.length} binaries`);
    return bins;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

function main() {
    if (opts.clean) return clean();

    const toolchain = discover();

    ui.log(
        `toolchain: ${toolchain.cc}${toolchain.isClang ? " (clang)" : ""}` +
            `${toolchain.ccache ? " + ccache" : ""}` +
            `${toolchain.mold ? " + mold" : ""}` +
            ` · config=${opts.config}` +
            `${opts.asan ? " · asan" : ""}${opts.tsan ? " · tsan" : ""}`,
    );

    mkdirp(BUILD);

    /*
     * Record which sanitizer this build is, in a file the test runner reads.
     *
     * The runner needs to know because the suppressions file is only
     * relevant under ThreadSanitizer, and "which sanitizer" is not something
     * a binary's name says. A file rather than an environment variable
     * because the two are separate processes, often separated by a shell.
     */
    fs.writeFileSync(
        path.join(BUILD, "config.json"),
        JSON.stringify({ sanitizer: opts.sanitizer, config: opts.config }) + "\n",
    );

    const runtime = buildRuntime(toolchain);

    if (opts.test) buildTests(toolchain, runtime.objs);

    checkFreestanding({
        root: ROOT,
        buildDir: BUILD,
        opts,
        toolchain,
        verbose: opts.verbose,
        ui,
        runtimeObjs: runtime.vmObjs,
    });

    /*
     * The public ABI audit needs the *rt* objects too, because some public
     * functions are implemented there — rt_mem_*, rt_thread_*, the channels.
     */
    checkPublicAbi({
        root: ROOT,
        runtimeObjs: runtime.objs,
        opts,
        ui,
        nm: which("nm") || which("llvm-nm"),
        verbose: opts.verbose,
    });

    checkNoDeadFunctions({ root: ROOT, ui });

    ui.log("done");
}

main();
