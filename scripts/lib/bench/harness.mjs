// scripts/lib/bench/harness.mjs
//
// Building and running the benchmark binaries.
//
// The shape of the build is the fairness argument in BENCH.md §2.4 made
// mechanical: one binary per (scenario, backend) pair, all linked from the
// same scenario source, so a workload cannot drift between collectors. If
// two binaries for one scenario differ, it is only in which collector they
// link — which is what is being compared.
//
// Everything C is built at -O1 (BENCH.md §2.1), including the cvm runtime
// itself. That is deliberate and it is the only reason this module compiles
// the runtime rather than reusing build/obj: the test build is -O0 or -O2,
// and a benchmark that measured a different optimization level than the
// other backends would be measuring the wrong thing.

import fs from "node:fs";
import path from "node:path";

import { run, runOrDie, which } from "../exec.mjs";
import { mkdirp, collect } from "../util.mjs";
import { BENCH_OPT, bdwgcIncludeDirs, bdwgcLibrary, tgcIncludeDir, tgcObject } from "./thirdparty.mjs";

const BENCH_BUILD = "bench/build";
const OBJ_DIR = "bench/build/obj";
const BIN_DIR = "bench/build/bin";

/* ------------------------------------------------------------------ */
/* the backends                                                        */
/* ------------------------------------------------------------------ */

/**
 * One entry per collector.
 *
 * `defines` is how the scenario sources know which roots protocol to use —
 * see harness/roots.h. `objects` and `libs` are what that collector needs
 * linked, and they differ enough that a uniform treatment would hide a real
 * difference: cvm is a pile of objects, bdwgc is a static library, tgc is
 * one file.
 */
export const BACKENDS = [
    {
        name: "cvm",
        source: "bench/cvm/backend_cvm.c",
        defines: ["BENCH_BACKEND_CVM"],
        includes: ["rt", "rt/base", "vm", "vm/include"],
        runtime: true,
        libs: [],
    },
    {
        name: "bdwgc",
        source: "bench/bdwgc/backend_bdwgc.c",
        defines: ["BENCH_BACKEND_BDWGC"],
        includes: [],
        bdwgc: true,
        libs: ["-lpthread"],
    },
    {
        name: "tgc",
        source: "bench/tgc/backend_tgc.c",
        defines: ["BENCH_BACKEND_TGC"],
        includes: [],
        tgc: true,
        libs: [],
    },
];

/* ------------------------------------------------------------------ */
/* the cvm runtime, at the benchmark's optimization level              */
/* ------------------------------------------------------------------ */

/**
 * Compile the runtime into bench/build/obj at -O1.
 *
 * Cached by timestamp against every source it was built from: rebuilding
 * twenty-nine objects on every run would make the benchmark unusable, and
 * getting the staleness rule wrong would make it lie.
 */
export function buildRuntime({ root, toolchain, ui, verbose }) {
    const objDir = path.join(root, OBJ_DIR);
    mkdirp(objDir);

    const sources = [
        ...collect(path.join(root, "rt"), [".c"]),
        ...collect(path.join(root, "vm"), [".c"]),
    ].filter((f) => !f.includes("/build/"));

    const asm = collect(path.join(root, "vm"), [".S"]);

    const includes = [
        "-Irt",
        "-Irt/base",
        "-Ivm",
        "-Ivm/include",
    ];

    /*
     * Every header is a dependency of every object.
     *
     * Conservative on purpose. The freshness check used to compare an object
     * only against the source file it was compiled from, which means editing
     * a header rebuilt nothing — and the collector's constants live in
     * headers. Chasing a performance regression through a stale build cost an
     * hour: a value was changed, the build said nothing, and the measurement
     * was of the previous binary twice over. Rebuilding twenty-nine small
     * files when a header moves is a moment's work and cannot lie.
     */
    const headers = [
        ...collect(path.join(root, "vm"), [".h"]),
        ...collect(path.join(root, "rt"), [".h"]),
    ];
    let newestHeader = 0;
    for (const h of headers) {
        const t = fs.statSync(h).mtimeMs;
        if (t > newestHeader) newestHeader = t;
    }

    const objects = [];
    let built = 0;

    for (const src of [...sources, ...asm]) {
        const rel = path.relative(root, src);
        const obj = path.join(objDir, rel.replace(/[\\/]/g, "_") + ".o");
        objects.push(obj);

        const fresh =
            fs.existsSync(obj) &&
            fs.statSync(obj).mtimeMs >= fs.statSync(src).mtimeMs &&
            fs.statSync(obj).mtimeMs >= newestHeader;
        if (fresh) continue;

        runOrDie(
            toolchain.cc,
            [
                "-std=c11",
                BENCH_OPT,
                "-g0",
                "-DNDEBUG",
                ...includes,
                "-c",
                rel,
                "-o",
                path.relative(root, obj),
            ],
            { cwd: root, verbose, ui, what: `cvm runtime: ${rel}` },
        );
        built++;
    }

    ui.log(`cvm runtime: ${objects.length} object(s), ${built} rebuilt (${BENCH_OPT})`);
    return objects;
}

/* ------------------------------------------------------------------ */
/* binaries                                                            */
/* ------------------------------------------------------------------ */

/**
 * Every .c in bench/scenarios that defines a scenario.
 *
 * shapes.c is shared code rather than a scenario — it has no
 * `bench_scenario` — so it is compiled into every binary instead of being
 * treated as one.
 */
function scenarioSources(root) {
    const dir = path.join(root, "bench/scenarios");
    return fs
        .readdirSync(dir)
        .filter((f) => f.endsWith(".c") && f !== "shapes.c")
        .map((f) => path.join("bench/scenarios", f))
        .sort();
}

/** The newest header mtime under vm/ and rt/. A dependency of everything. */
function newestHeaderMtime(root) {
    let newest = 0;
    for (const dir of ["vm", "rt"]) {
        for (const h of collect(path.join(root, dir), [".h"])) {
            const t = fs.statSync(h).mtimeMs;
            if (t > newest) newest = t;
        }
    }
    return newest;
}

export function buildBinaries({ root, toolchain, thirdparty, ui, verbose }) {
    const newestHeader = newestHeaderMtime(root);
    const runtime = buildRuntime({ root, toolchain, ui, verbose });
    const binDir = path.join(root, BIN_DIR);
    mkdirp(binDir);

    const harness = [
        "bench/harness/run.c",
        "bench/harness/report.c",
        "bench/scenarios/shapes.c",
    ];
    const scenarios = scenarioSources(root);
    const binaries = [];

    for (const backend of BACKENDS) {
        /* The collector's own pieces. */
        const collectorObjects = [];
        if (backend.runtime) collectorObjects.push(...runtime);
        if (backend.tgc) collectorObjects.push(path.relative(root, thirdparty.tgc));

        const includes = [
            "-Ibench/harness",
            "-Ibench/scenarios",
            ...backend.includes.map((i) => `-I${i}`),
        ];
        if (backend.bdwgc) {
            includes.push(...bdwgcIncludeDirs(root).map((i) => `-I${i}`));
        }
        if (backend.tgc) includes.push(`-I${tgcIncludeDir(root)}`);

        const links = [];
        if (backend.bdwgc) links.push(bdwgcLibrary(root));
        links.push(...backend.libs);

        for (const scenario of scenarios) {
            const base = path.basename(scenario, ".c");
            const out = path.join(binDir, `${base}_${backend.name}`);

            /* Rebuild only when something it is made of changed. */
            const deps = [
                scenario,
                backend.source,
                ...harness,
                ...(backend.runtime ? runtime : []),
            ].map((d) => (path.isAbsolute(d) ? d : path.join(root, d)));

            const outAbs = path.join(root, path.relative(root, out));
            const fresh =
                fs.existsSync(outAbs) &&
                deps.every((d) => fs.existsSync(d) && fs.statSync(outAbs).mtimeMs >= fs.statSync(d).mtimeMs) &&
                fs.statSync(outAbs).mtimeMs >= newestHeader &&
                (!backend.bdwgc || fs.statSync(outAbs).mtimeMs >= fs.statSync(bdwgcLibrary(root)).mtimeMs);

            if (!fresh) {
                runOrDie(
                    toolchain.cc,
                    [
                        "-std=c11",
                        BENCH_OPT,
                        "-g0",
                        "-DNDEBUG",
                        ...backend.defines.map((d) => `-D${d}`),
                        ...includes,
                        path.relative(root, scenario),
                        ...harness,
                        path.relative(root, backend.source),
                        ...collectorObjects,
                        "-o",
                        path.relative(root, out),
                        ...links,
                    ],
                    {
                        cwd: root,
                        verbose,
                        ui,
                        what: `${base} × ${backend.name}`,
                    },
                );
            }

            binaries.push({ scenario: base, backend: backend.name, path: outAbs });
        }
    }

    ui.log(`scenarios: ${binaries.length} binaries (${scenarios.length} × ${BACKENDS.length})`);
    return binaries;
}

/* ------------------------------------------------------------------ */
/* running                                                             */
/* ------------------------------------------------------------------ */

/**
 * Run one binary in one mode and return its CSV row.
 *
 * The row is the binary's own output rather than something parsed back out
 * of a log line: report.c writes it, and the benchmark's numbers are exactly
 * what the harness measured.
 */
export function runOne({ binary, mode, reps, cadence, csvPath, verbose }) {
    const args = [
        `--mode=${mode}`,
        `--reps=${reps}`,
        `--out=${csvPath}`,
    ];
    if (cadence) args.push(`--cadence=${cadence}`);

    const r = run(binary, args, { verbose });
    return {
        status: r.status,
        stdout: r.stdout,
        stderr: r.stderr,
    };
}

/* ------------------------------------------------------------------ */
/* the Go backend                                                      */
/* ------------------------------------------------------------------ */

const GO_MODULE = "bench/go";
const GO_BINARY = "bench/build/bin/bench_go";

/**
 * Build the Go side.
 *
 * One binary with a --scenario flag rather than one binary per scenario:
 * the C side needs a binary per (scenario, backend) because it links a
 * different collector into each, and Go has one runtime and seven workloads.
 * The scenarios are still separate code, and still compared against the C
 * ones — see bench/go/README.md.
 *
 * There is no optimization flag to pin. Go has no -O1, so its default
 * optimizer is what runs, and BENCH.md §2.1 states that rather than
 * pretending the two sides are aligned.
 */
export function buildGoBinary({ root, ui, verbose }) {
    const go = which("go");
    if (!go) {
        ui.log("go: not installed, skipping the Go backend");
        ui.log("    install a Go toolchain and re-run to include it");
        return null;
    }

    const bin = path.join(root, GO_BINARY);
    mkdirp(path.dirname(bin));

    const sources = collect(path.join(root, GO_MODULE), [".go"]);
    const fresh =
        fs.existsSync(bin) &&
        sources.every(
            (f) => fs.statSync(bin).mtimeMs >= fs.statSync(f).mtimeMs,
        );

    if (fresh) {
        ui.log("go: already built");
        return bin;
    }

    const version = run(go, ["version"], { verbose }).stdout.trim();
    ui.log(`go: building (${version})`);
    /*
     * Absolute -o: go build resolves the output relative to its working
     * directory, and a relative one here lands the binary at
     * bench/go/bench/build/bin/, where the runner never looks. The first
     * version of this produced "go: building" and then silently no rows.
     */
    runOrDie(go, ["build", "-o", bin, "."], {
        cwd: path.join(root, GO_MODULE),
        verbose,
        ui,
        what: "go build",
    });

    return bin;
}

/**
 * Run the Go binary for one scenario and mode.
 *
 * The arguments mirror the C harness's, because the two are compared row by
 * row and an argument that meant something different on one side would be a
 * silent difference in what was measured.
 */
export function runGoOne({ binary, scenario, mode, reps, cadence, csvPath, verbose }) {
    const args = [
        `--scenario=${scenario}`,
        `--mode=${mode}`,
        `--reps=${reps}`,
    ];
    if (csvPath) args.push(`--out=${csvPath}`);
    if (cadence) args.push(`--cadence=${cadence}`);

    const r = run(binary, args, { verbose });
    return { status: r.status, stdout: r.stdout, stderr: r.stderr };
}

export { BENCH_BUILD, BIN_DIR, GO_BINARY };
