// scripts/lib/util.mjs
//
// Logging and filesystem helpers shared by every script under scripts/.

import fs from "node:fs";
import path from "node:path";

/**
 * Bind a log tag once per script:
 *   const ui = logger("build");
 *   ui.log("...")            always prints, prefixed with [tag]
 *   ui.vlog(verbose, "...")  prints only when verbose
 *   ui.fatal("...")          prints to stderr and exits 1
 */
export function logger(tag) {
    return {
        log: (msg) => console.log(`[${tag}] ${msg}`),
        vlog: (verbose, msg) => {
            if (verbose) console.log(`        ${msg}`);
        },
        fatal: (msg) => {
            console.error(`${tag}: error: ${msg}`);
            process.exit(1);
        },
    };
}

export function mkdirp(p) {
    fs.mkdirSync(p, { recursive: true });
}

export function rmrf(p) {
    fs.rmSync(p, { recursive: true, force: true });
}

/** Recursively collect files under dir whose name ends with one of exts. */
export function collect(dir, exts) {
    if (!fs.existsSync(dir)) return [];
    const out = [];
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, ent.name);
        if (ent.isDirectory()) out.push(...collect(p, exts));
        else if (exts.some((e) => ent.name.endsWith(e))) out.push(p);
    }
    return out.sort();
}
