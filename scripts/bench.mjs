// scripts/bench.mjs
//
// Builds and runs the collector benchmark described in bench/BENCH.md.
//
// Same shape as scripts/build.mjs: this file orchestrates and everything
// substantive lives in scripts/lib/. The bench-specific parts are in
// scripts/lib/bench/.
//
//   node scripts/bench.mjs                build everything, then measure
//   node scripts/bench.mjs --build-only   build only
//   node scripts/bench.mjs --quick        fewer repetitions
//   node scripts/bench.mjs -v             echo every command
//
// Results go to bench/results/<date>-<machine>.csv, with a header line and
// one row per (scenario, backend, mode). A row carries the median of K runs
// and the spread around it, because a median with no dispersion is a number
// pretending to be a measurement.

import fs from "node:fs";
import os from "node:os";
import { spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { logger, mkdirp } from "./lib/util.mjs";
import { discover } from "./lib/toolchain.mjs";
import { buildThirdParty } from "./lib/bench/thirdparty.mjs";
import {
    BACKENDS,
    buildBinaries,
    buildGoBinary,
    runOne,
    runGoOne,
} from "./lib/bench/harness.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BENCH = path.join(ROOT, "bench");

const ui = logger("bench");

const argv = process.argv.slice(2);
const has = (f) => argv.includes(f);

const opts = {
    buildOnly: has("--build-only"),
    quick: has("--quick"),
    verbose: has("-v") || has("--verbose"),
};

/*
 * Repetitions per measurement.
 *
 * The median of several runs rather than the best of them: collection
 * points depend on allocation counts, so wall time is noisy in a way that
 * makes the minimum systematically flatter whichever run collected least.
 */
const REPETITIONS = opts.quick ? 3 : 7;

/** The modes a scenario asks for, by name. */
const MODE_BITS = { alloc: 1, fixed: 2, auto: 4 };

/**
 * Rows a backend cannot produce, and why.
 *
 * A bare "failed" leaves a reader unable to tell a limitation of the
 * collector from a broken harness, and the difference matters: one of these
 * is a fact about a 459-line toy and the other would be a bug in this
 * directory.
 *
 *   tgc × read-heavy — tgc_mark_ptr recurses once per pointer field, and
 *   read-heavy's graph is a chain 200,000 nodes long, so marking nests
 *   200,000 frames deep. At roughly 56 bytes a frame that is about 11 MiB
 *   against an 8 MiB stack. It is a stack overflow in the collector's own
 *   marker, arithmetic rather than a race, and "fixing" it would mean
 *   rewriting the collector this row exists to compare against.
 */
const KNOWN_GAPS = [
    { scenario: "read-heavy", backend: "tgc",
      why: "its recursive marker overflows the stack on a 200k-node chain" },
];

function knownGap(scenario, backend) {
    return KNOWN_GAPS.find((g) => g.scenario === scenario && g.backend === backend);
}

/**
 * Which modes to run for a scenario.
 *
 * Read from the binary rather than duplicated here: the scenario declares
 * them (see bench/scenarios/scenario.h), and a second copy in this file
 * would be a second source of truth about what was measured. `--modes` on
 * the command line prints them.
 */
function modesFor(scenario) {
    const r = runScenarioProbe(scenario, "--modes");
    return r
        .trim()
        .split(/\s+/)
        .filter((m) => MODE_BITS[m] !== undefined);
}

/** Ask a binary something; it answers on stdout without measuring. */
function runScenarioProbe(scenario, arg) {
    const r = spawnSync(scenario, [arg], { encoding: "utf8" });
    return r.stdout ?? "";
}

/** Which modes the Go binary declares for one scenario. */
/** The scenario name a C binary declares for itself. */
function scenarioName(binary) {
    const r = spawnSync(binary, ["--name"], { encoding: "utf8" });
    return (r.stdout ?? "").trim();
}

function goModes(goBin, scenario) {
    const r = spawnSync(goBin, [`--scenario=${scenario}`, "--modes"], {
        encoding: "utf8",
    });
    return (r.stdout ?? "")
        .trim()
        .split(/\s+/)
        .filter((m) => MODE_BITS[m] !== undefined);
}

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
        compiler: process.env.CC ?? "cc",
        optimization: "-O1",
        repetitions: REPETITIONS,
    };
}

function resultsPath() {
    const stamp = environment().date.replace(/[:.]/g, "-");
    const machine = `${process.platform}-${process.arch}`;
    return path.join(BENCH, "results", `${stamp}-${machine}.csv`);
}

function main() {
    const toolchain = discover();

    ui.log(
        `toolchain: ${toolchain.cc}${toolchain.isClang ? " (clang)" : ""}` +
            `${toolchain.ccache ? " + ccache" : ""}` +
            ` · ${REPETITIONS} repetition(s)`,
    );

    mkdirp(path.join(BENCH, "build"));

    const thirdparty = buildThirdParty({
        root: ROOT,
        toolchain,
        ui,
        verbose: opts.verbose,
    });

    const binaries = buildBinaries({
        root: ROOT,
        toolchain,
        thirdparty,
        ui,
        verbose: opts.verbose,
    });

    if (opts.buildOnly) {
        ui.log("done (build only)");
        return;
    }

    const csv = resultsPath();
    mkdirp(path.dirname(csv));

    /*
     * The header is written here rather than by the binaries: one file, one
     * header, and a binary that appends cannot get the columns wrong.
     */
    fs.writeFileSync(
        csv,
        "scenario,backend,mode,runs," +
            "wall_ns_min,wall_ns_median,wall_ns_max," +
            "allocations_median,allocations_min,collections_median," +
            "pause_max_ns_median,pause_max_ns_max,live_objects,heap_bytes\n",
    );

    console.log("");
    let rows = 0;
    let unavailable = 0;
    let gaps = 0;

    for (const binary of binaries) {
        binary.scenarioName = scenarioName(binary.path);
        for (const mode of modesFor(binary.path)) {
            const r = runOne({
                binary: binary.path,
                mode,
                reps: REPETITIONS,
                csvPath: csv,
                verbose: opts.verbose,
            });

            if (r.stdout) process.stdout.write(r.stdout);
            if (r.status !== 0) {
                const gap = knownGap(binary.scenarioName, binary.backend);
                if (gap) {
                    process.stderr.write(
                        `       ${binary.scenario} × ${binary.backend} × ${mode}` +
                            `: known gap — ${gap.why}\n`,
                    );
                    gaps++;
                } else {
                    process.stderr.write(
                        `       ${binary.scenario} × ${binary.backend} × ${mode} failed\n`,
                    );
                }
                continue;
            }
            if (r.stdout.includes("unavailable")) unavailable++;
            rows++;
        }
    }

    /*
     * The Go backend, if there is a toolchain. Its rows are produced by a
     * separate binary and land in the same CSV, because a comparison whose
     * numbers live in two files is a comparison nobody does.
     */
    const goBin = buildGoBinary({ root: ROOT, ui, verbose: opts.verbose });
    if (goBin !== null) {
        /*
         * The names come from the C binaries rather than from their file
         * names: a file called churn_unary.c declares the scenario
         * "churn-unary", and matching the two by hand is how a Go row ends
         * up attached to the wrong scenario.
         */
        const names = [...new Set(binaries.map((b) => b.scenarioName))].sort();
        for (const name of names) {
            for (const mode of goModes(goBin, name)) {
                const r = runGoOne({
                    binary: goBin,
                    scenario: name,
                    mode,
                    reps: REPETITIONS,
                    csvPath: csv,
                    verbose: opts.verbose,
                });
                if (r.stdout) process.stdout.write(r.stdout);
                if (r.status !== 0) {
                    process.stderr.write(
                        `       ${name} × go × ${mode} failed\n`,
                    );
                    continue;
                }
                rows++;
            }
        }
    }

    console.log("");
    ui.log(`${rows} row(s) → ${path.relative(ROOT, csv)}`);
    if (unavailable > 0) {
        ui.log(`${unavailable} row(s) unavailable: a backend that cannot be asked`);
        ui.log("        to collect is reported rather than approximated");
    }
    if (gaps > 0) {
        ui.log(`${gaps} row(s) are known gaps, not failures — see KNOWN_GAPS`);
        ui.log("        in this file for what each one is");
    }
    ui.log("done");
}

main();
