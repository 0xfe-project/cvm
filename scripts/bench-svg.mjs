// scripts/bench-svg.mjs
//
// Draws the benchmark's results as SVG, from the results CSV.
//
// Three figures, one per statistical property, because the interesting thing
// about these numbers is not only where they sit but how much they move:
//
//   wall.svg    where each collector's time sits, with the min and max of its
//               seven runs drawn as a range. On a log axis the length of that
//               range is the relative spread, so a reader can see at a glance
//               which rows are measurements and which are noise.
//
//   pause.svg   the same for the longest stop-the-world window each collector
//               reported. This is the axis the four designs differ on most:
//               it spans five orders of magnitude between Go's concurrent
//               collector and a conservative sweep.
//
//   spread.svg  run-to-run dispersion as a percentage of the median, sorted.
//               The answer to "can I quote this row", which a table of
//               medians cannot give.
//
// Generated rather than drawn, so the figures cannot drift from the data:
// run the benchmark, run this, and the pictures are of the current tree.
//
//   node scripts/bench-svg.mjs [path/to/results.csv]

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const BENCH = path.join(ROOT, "bench");

/* ------------------------------------------------------------------ */
/* the data                                                            */
/* ------------------------------------------------------------------ */

function parseCsv(text) {
    const [head, ...rest] = text.trim().split("\n");
    const cols = head.split(",");
    return rest.filter(Boolean).map((line) => {
        const cells = line.split(",");
        const row = {};
        cols.forEach((c, i) => (row[c] = cells[i]));
        return {
            scenario: row.scenario,
            backend: row.backend,
            mode: row.mode,
            runs: Number(row.runs),
            wallMin: Number(row.wall_ns_min),
            wallMed: Number(row.wall_ns_median),
            wallMax: Number(row.wall_ns_max),
            pauseMed: Number(row.pause_max_ns_median),
            pauseMax: Number(row.pause_max_ns_max),
            collections: Number(row.collections_median),
        };
    });
}

/** The most recent run, or the one named on the command line. */
function resultsPath() {
    const arg = process.argv[2];
    if (arg) return path.resolve(arg);
    const dir = path.join(BENCH, "results");
    const files = fs
        .readdirSync(dir)
        .filter((f) => f.endsWith(".csv"))
        .sort();
    if (files.length === 0) {
        console.error("no results in bench/results/ — run `pnpm run bench` first");
        process.exit(1);
    }
    return path.join(dir, files[files.length - 1]);
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

/*
 * A colour per collector, and the same one in every figure. Chosen to stay
 * distinguishable in greyscale as well as in colour, because a printed figure
 * is still a figure.
 */
const COLORS = {
    cvm: "#1f6feb",
    go: "#2ea043",
    bdwgc: "#d29922",
    tgc: "#8b949e",
};
const BACKENDS = Object.keys(COLORS);

const FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif";

function escapeXml(s) {
    return s.replace(/[<>&]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;" })[c]);
}

/** Where a value sits on a log axis, as a fraction of the plot width. */
function logPos(value, lo, hi) {
    const a = Math.log10(Math.max(value, lo));
    const b = Math.log10(lo);
    const c = Math.log10(hi);
    return (a - b) / (c - b);
}

/** Decade ticks between lo and hi, as powers of ten with a unit. */
function ticks(lo, hi, unit) {
    const out = [];
    for (let e = Math.floor(Math.log10(lo)); e <= Math.ceil(Math.log10(hi)); e++) {
        const v = Math.pow(10, e);
        if (v < lo || v > hi) continue;
        /* Enough decimals to keep the leading digit, and no more: 0.001 must
         * not print as 0.0. */
        const decimals = v >= 1 ? 0 : Math.max(0, -Math.floor(Math.log10(v)));
        out.push({ v, label: `${v.toFixed(decimals)}${unit}` });
    }
    return out;
}

/**
 * The shared body of the two range figures.
 *
 * One row per (scenario, backend, mode), grouped by scenario, with the median
 * as a dot and the run-to-run range as a line through it. Log x-axis: the
 * values span three to five orders of magnitude and a linear axis would draw
 * everything but the slowest row as a single point.
 */
function range_figure({ title, subtitle, rows, pick, unit, scale, zeroNote, perMs = 1e6 }) {
    const M = { top: 92, left: 250, right: 40, bottom: 46, rowH: 20, groupGap: 12 };
    const plotW = 560;
    const groups = [...new Set(rows.map((r) => r.scenario))];

    /* Rows in draw order, with a gap before each new scenario. */
    let y = M.top;
    const items = [];
    for (const g of groups) {
        for (const r of rows.filter((x) => x.scenario === g)) {
            items.push({ ...r, y });
            y += M.rowH;
        }
        y += M.groupGap;
    }

    /* Nanoseconds in the file, milliseconds on the axis: a tick reading
     * "1000000000ns" is a number nobody reads. */
    const values = rows.map((r) => pick(r) / perMs).filter((v) => v > 0);
    const lo = Math.pow(10, Math.floor(Math.log10(Math.min(...values))));
    const hi = Math.pow(10, Math.ceil(Math.log10(Math.max(...values))));
    const width = M.left + plotW + M.right;
    const height = y + M.bottom;

    const x = (v) => M.left + logPos(v, lo, hi) * plotW;

    const parts = [];
    parts.push(
        `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" ` +
            `viewBox="0 0 ${width} ${height}" font-family="${FONT}">`,
        `<rect width="${width}" height="${height}" fill="#ffffff"/>`,
        `<text x="24" y="38" font-size="19" font-weight="600" fill="#1f2328">${escapeXml(title)}</text>`,
        `<text x="24" y="60" font-size="12.5" fill="#57606a">${escapeXml(subtitle)}</text>`,
    );

    /* Legend. */
    let lx = 24;
    for (const b of BACKENDS) {
        if (!rows.some((r) => r.backend === b)) continue;
        parts.push(
            `<circle cx="${lx + 5}" cy="80" r="5" fill="${COLORS[b]}"/>`,
            `<text x="${lx + 16}" y="84" font-size="12.5" fill="#1f2328">${b}</text>`,
        );
        lx += 34 + b.length * 7.4;
    }

    /* Grid and axis labels. */
    for (const t of ticks(lo, hi, unit)) {
        const px = x(t.v);
        parts.push(
            `<line x1="${px.toFixed(1)}" y1="${M.top - 6}" x2="${px.toFixed(1)}" ` +
                `y2="${(height - M.bottom + 6).toFixed(1)}" stroke="#d8dee4" stroke-width="1"/>`,
            `<text x="${px.toFixed(1)}" y="${height - M.bottom + 24}" font-size="11.5" ` +
                `fill="#57606a" text-anchor="middle">${t.label}</text>`,
        );
    }

    /* Rows. */
    let lastScenario = null;
    for (const it of items) {
        const cy = it.y + M.rowH / 2;

        if (it.scenario !== lastScenario) {
            if (lastScenario !== null) {
                parts.push(
                    `<line x1="24" y1="${(it.y - M.groupGap / 2).toFixed(1)}" ` +
                        `x2="${(width - 24).toFixed(1)}" y2="${(it.y - M.groupGap / 2).toFixed(1)}" ` +
                        `stroke="#eaeef2" stroke-width="1"/>`,
                );
            }
            parts.push(
                `<text x="24" y="${(cy + 4).toFixed(1)}" font-size="13" font-weight="600" ` +
                    `fill="#1f2328">${escapeXml(it.scenario)}</text>`,
            );
            lastScenario = it.scenario;
        }

        parts.push(
            `<text x="150" y="${(cy + 4).toFixed(1)}" font-size="12" ` +
                `fill="#57606a">${escapeXml(it.backendLabel ?? it.backend)}</text>`,
        );

        const med = pick(it) / perMs;
        if (med <= 0) {
            /* No collection happened, so there is no interval to draw. Said
             * rather than left blank: an absent row reads as missing data. */
            parts.push(
                `<text x="${(M.left + 6).toFixed(1)}" y="${(cy + 4).toFixed(1)}" ` +
                    `font-size="11.5" fill="#8b949e">${escapeXml(zeroNote)}</text>`,
            );
            continue;
        }

        const loV = pick(it, "lo") > 0 ? pick(it, "lo") / perMs : med;
        const hiV = pick(it, "hi") > 0 ? pick(it, "hi") / perMs : med;
        const c = COLORS[it.backend];

        if (scale === "log" && hiV > loV) {
            parts.push(
                `<line x1="${x(loV).toFixed(1)}" y1="${cy.toFixed(1)}" ` +
                    `x2="${x(hiV).toFixed(1)}" y2="${cy.toFixed(1)}" stroke="${c}" ` +
                    `stroke-width="2" stroke-linecap="round" opacity="0.45"/>`,
            );
        }
        parts.push(`<circle cx="${x(med).toFixed(1)}" cy="${cy.toFixed(1)}" r="4.5" fill="${c}"/>`);
    }

    parts.push("</svg>");
    return parts.join("\n") + "\n";
}

/** Dispersion as a percentage of the median, one row each, sorted. */
function spread_figure(rows) {
    const items = rows
        .map((r) => ({
            ...r,
            pct: r.wallMed > 0 ? ((r.wallMax - r.wallMin) / r.wallMed) * 100 : 0,
        }))
        .sort((a, b) => b.pct - a.pct);

    const M = { top: 92, left: 250, right: 90, bottom: 46, rowH: 20 };
    const plotW = 560;
    const maxPct = 100; /* clamped; anything above is drawn as overflowing */
    const width = M.left + plotW + M.right;
    const height = M.top + items.length * M.rowH + M.bottom;
    const x = (p) => M.left + (Math.min(p, maxPct) / (maxPct * 1.18)) * plotW;

    const parts = [
        `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" ` +
            `viewBox="0 0 ${width} ${height}" font-family="${FONT}">`,
        `<rect width="${width}" height="${height}" fill="#ffffff"/>`,
        `<text x="24" y="38" font-size="19" font-weight="600" fill="#1f2328">` +
            `How much these numbers move between runs</text>`,
        `<text x="24" y="60" font-size="12.5" fill="#57606a">` +
            `(max − min) as a percentage of the median, over each row's repetitions. ` +
            `A median with no dispersion is a number pretending to be a measurement.</text>`,
    ];

    for (const pct of [0, 5, 10, 25, 50, 100]) {
        const px = x(pct);
        parts.push(
            `<line x1="${px.toFixed(1)}" y1="${M.top - 6}" x2="${px.toFixed(1)}" ` +
                `y2="${(height - M.bottom + 6).toFixed(1)}" stroke="#d8dee4"/>`,
            `<text x="${px.toFixed(1)}" y="${height - M.bottom + 24}" font-size="11.5" ` +
                `fill="#57606a" text-anchor="middle">${pct}%</text>`,
        );
    }

    /*
     * Every row carries its own scenario name.
     *
     * The rows are sorted by dispersion, which interleaves scenarios, so a
     * group heading printed when the scenario changes appears four or five
     * times down the list and reads as a bug rather than as a heading. In a
     * sorted list each row has to stand alone.
     */
    items.forEach((it, i) => {
        const y = M.top + i * M.rowH;
        const cy = y + M.rowH / 2;

        parts.push(
            `<text x="24" y="${(cy + 4).toFixed(1)}" font-size="12.5" font-weight="600" ` +
                `fill="#1f2328">${escapeXml(it.scenario)}</text>`,
            `<text x="150" y="${(cy + 4).toFixed(1)}" font-size="12" fill="#57606a">` +
                `${escapeXml(it.backend)} × ${escapeXml(it.mode)}</text>`,
        );

        const c = COLORS[it.backend];
        parts.push(
            `<rect x="${M.left}" y="${(cy - 6).toFixed(1)}" width="${(x(it.pct) - M.left).toFixed(1)}" ` +
                `height="12" fill="${c}" opacity="0.32"/>`,
            `<circle cx="${x(it.pct).toFixed(1)}" cy="${cy.toFixed(1)}" r="3.5" fill="${c}"/>`,
            `<text x="${(width - M.right + 8).toFixed(1)}" y="${(cy + 4).toFixed(1)}" ` +
                `font-size="11.5" fill="#57606a">${it.pct.toFixed(1)}%</text>`,
        );
    });

    parts.push(
        `<text x="${(M.left + plotW / 2).toFixed(0)}" y="${height - 10}" font-size="11.5" ` +
            `fill="#8b949e" text-anchor="middle">anything past 100% is drawn at the edge and ` +
            `labelled with its real value</text>`,
        "</svg>",
    );
    return parts.join("\n") + "\n";
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

const csvPath = resultsPath();
const rows = parseCsv(fs.readFileSync(csvPath, "utf8"));

/* Labelled by backend and mode, because a scenario runs in several modes and
 * a row that does not say which is a row nobody can use. */
const labelled = rows.map((r) => ({
    ...r,
    backendLabel: `${r.backend} × ${r.mode}`,
}));

/*
 * One figure per (metric, mode).
 *
 * The mode is the experimental condition, not a grouping for convenience:
 * alloc-only never collects and measures the allocator, fixed forces the same
 * collection count on every backend and measures the mechanism, auto lets
 * each collector choose and measures the policy. A figure holding rows from
 * two of them invites a comparison between them and there is none to make —
 * the fastest alloc-only row and the fastest fixed row were produced under
 * different rules.
 *
 * So the mode is in the file name and the title, and a figure exists only
 * where the mode has something to say: alloc-only performs no collection, so
 * it has no pause to report and gets no pause figure.
 */

const MODE_NOTE = {
    alloc: "alloc-only: no collection happens, so this is the allocator",
    fixed: "fixed cadence: every backend forced to collect at the same allocation count",
    auto: "auto: each collector's own policy",
};

function wallFigure(mode, rows) {
    return range_figure({
        title: `Wall time, ${mode} mode`,
        subtitle:
            `${MODE_NOTE[mode]}. Dot is the median of the row's repetitions; the ` +
            `line is that run's shortest to longest, so its length is the spread.`,
        rows,
        pick: (r, which) =>
            which === "lo" ? r.wallMin : which === "hi" ? r.wallMax : r.wallMed,
        unit: "ms",
        scale: "log",
        zeroNote: "measured, but no time recorded",
    });
}

function pauseFigure(mode, rows) {
    return range_figure({
        title: `Longest stop-the-world pause, ${mode} mode`,
        /*
         * The line here is NOT the same interval as the wall figures' line,
         * and the two would otherwise look identical. The results carry a min
         * and a max for wall time but only a median and a worst for the
         * pause, so this line runs from the typical pause to the worst one
         * observed — a different thing, said out loud.
         */
        subtitle:
            `${MODE_NOTE[mode]}. Each collector's own longest pause. Dot is the ` +
            `median across repetitions; the line runs from there to the worst ` +
            `single pause in the run — not a min-to-max spread, which the ` +
            `results do not record for this column. Log scale: this is the axis ` +
            `the four designs differ on most.`,
        rows,
        pick: (r, which) =>
            which === "lo" ? r.pauseMed : which === "hi" ? r.pauseMax : r.pauseMed,
        unit: "ms",
        scale: "log",
        zeroNote: "no collection happened",
    });
}

const figures = [
    { name: "wall-alloc.svg", svg: wallFigure("alloc", labelled.filter((r) => r.mode === "alloc")) },
    { name: "wall-auto.svg", svg: wallFigure("auto", labelled.filter((r) => r.mode === "auto")) },
    { name: "pause-auto.svg", svg: pauseFigure("auto", labelled.filter((r) => r.mode === "auto")) },
    { name: "spread-auto.svg", svg: spread_figure(labelled.filter((r) => r.mode === "auto"), "auto") },
];
for (const { name, svg } of figures) {
    fs.writeFileSync(path.join(BENCH, name), svg);
    console.log(`[bench] wrote bench/${name}`);
}
console.log(`[bench] ${figures.length} figure(s), one experimental condition each`);
console.log(`[bench] from ${path.relative(ROOT, csvPath)}`);
