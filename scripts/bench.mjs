// scripts/bench.mjs
//
// Builds and runs the collector benchmark described in bench/BENCH.md.
//
// Same shape as scripts/build.mjs: this file orchestrates, and everything
// substantive lives in scripts/lib/. The bench-specific parts are in
// scripts/lib/bench/.
//
//   node scripts/bench.mjs                build everything, then run
//   node scripts/bench.mjs --build-only   build only
//   node scripts/bench.mjs --quick        fewer repetitions (for a smoke run)
//   node scripts/bench.mjs -v             echo every command
//
// Results are written under bench/results/ with the date, the machine and
// the flags, because a number without those three is not a result.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { logger, mkdirp } from "./lib/util.mjs";
import { discover } from "./lib/toolchain.mjs";
import { buildThirdParty } from "./lib/bench/thirdparty.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BENCH = path.join(ROOT, "bench");

const ui = logger("bench");

const argv = process.argv.slice(2);
const has = (f) => argv.includes(f);

const opts = {
    buildOnly: has("--build-only"),
    runOnly: has("--run-only"),
    quick: has("--quick"),
    verbose: has("-v") || has("--verbose"),
};

/*
 * Repetitions per measurement.
 *
 * The median of several runs rather than the best of them: a collector's
 * timing is noisy because collection points depend on allocation counts,
 * and the minimum systematically flatters whichever run happened to collect
 * least.
 */
const REPETITIONS = opts.quick ? 3 : 7;

/** What a run of the benchmark records about the machine it ran on. */
function environment() {
    const cpus =
        typeof os.availableParallelism === "function"
            ? os.availableParallelism()
            : os.cpus().length;
    return {
        date: new Date().toISOString(),
        platform: process.platform,
        arch: process.arch,
        cpuCount: cpus,
        cpuModel: os.cpus()[0]?.model ?? "unknown",
        totalMemoryBytes: os.totalmem(),
    };
}

/* ------------------------------------------------------------------ */
/* steps                                                              */
/* ------------------------------------------------------------------ */

/**
 * Build the scenario binaries.
 *
 * One binary per (scenario, backend) pair, linked from the same scenario
 * source so that the workload cannot drift between backends. See BENCH.md
 * §2.4.
 *
 * Not written yet: the harness in bench/harness/ and the scenarios in
 * bench/scenarios/ are the next pieces of work. Until they exist this
 * reports what is missing rather than pretending to have run something.
 */
function buildScenarios({ toolchain, thirdparty }) {
    const harness = path.join(BENCH, "harness", "run.c");
    const scenarios = fs.existsSync(path.join(BENCH, "scenarios"))
        ? fs
              .readdirSync(path.join(BENCH, "scenarios"))
              .filter((f) => f.endsWith(".c"))
        : [];

    if (!fs.existsSync(harness) || scenarios.length === 0) {
        ui.log("scenarios: not built yet");
        ui.log("        the harness (bench/harness/run.c) and the");
        ui.log("        scenario sources (bench/scenarios/) are the next step;");
        ui.log("        see bench/BENCH.md §6 for the list and §8 for the layout");
        return [];
    }

    // Reachable once the harness exists. Left as the shape the work takes
    // rather than a guess at its details.
    ui.log(
        `scenarios: ${scenarios.length} source(s), ${thirdparty ? 1 : 0} backend set(s)`,
    );
    return [];
}

function writeResults(record) {
    const dir = path.join(BENCH, "results");
    mkdirp(dir);
    const stamp = record.environment.date.replace(/[:.]/g, "-");
    const file = path.join(dir, `env-${stamp}.json`);
    fs.writeFileSync(file, JSON.stringify(record, null, 2) + "\n");
    ui.log(`results: ${path.relative(ROOT, file)}`);
    return file;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

function main() {
    const toolchain = discover();

    ui.log(
        `toolchain: ${toolchain.cc}${toolchain.isClang ? " (clang)" : ""}` +
            `${toolchain.ccache ? " + ccache" : ""}` +
            ` · ${REPETITIONS} repetition(s)`,
    );

    mkdirp(path.join(BENCH, "build"));

    /* The third-party collectors are needed either way: a build-only run
     * should leave everything ready to run. */
    const thirdparty = buildThirdParty({
        root: ROOT,
        toolchain,
        ui,
        verbose: opts.verbose,
    });

    const binaries = buildScenarios({ toolchain, thirdparty });

    if (opts.buildOnly) {
        ui.log("done (build only)");
        return;
    }

    if (binaries.length === 0) {
        /*
         * Nothing to measure, so no results file. Writing one would put a
         * dated artefact in bench/results/ that records no measurement, and
         * a directory of those is worse than an empty one: the point of the
         * directory is that every file in it is a result.
         *
         * Not a failure: the third-party builds succeeding is real
         * progress, and the log says so.
         */
        ui.log("done (nothing measured: no scenarios to run yet)");
        return;
    }

    ui.log(`running ${binaries.length} scenario(s)`);
    writeResults({
        environment: environment(),
        measured: binaries.map((b) => path.relative(ROOT, b)),
    });
    ui.log("done");
}

main();
