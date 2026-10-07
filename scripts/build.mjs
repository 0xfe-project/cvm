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

import path from "node:path";
import { fileURLToPath } from "node:url";

import { parseArgs, help } from "./lib/args.mjs";
import { logger, mkdirp, rmrf, collect } from "./lib/util.mjs";
import { discover } from "./lib/toolchain.mjs";
import { flagsFor } from "./lib/flags.mjs";
import { compile, link, objectPath } from "./lib/cc.mjs";
import { checkFreestanding } from "./lib/verify.mjs";

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

/** Compile every .c under a directory tree with one flag flavor. */
function compileTree({ dir, flavor, toolchain, verbose }) {
    const srcs = collect(dir, [".c"]);
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

    ui.log("done");
}

main();
