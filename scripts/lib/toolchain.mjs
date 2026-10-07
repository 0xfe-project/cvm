// scripts/lib/toolchain.mjs
//
// Toolchain discovery: who compiles, who links, which headers are available.
// Knows about tools; knows nothing about our source layout.

import path from "node:path";
import { which, run } from "./exec.mjs";

export const PLATFORM = process.platform; // 'darwin' | 'linux' | 'win32'

/**
 * Locate the C compiler, the optional ccache front end, and decide on the
 * linker.
 *
 * Linker notes:
 *   - mold has no Mach-O backend (it reports only elf_x86_64), so it is
 *     never usable on macOS. On Linux it is preferred when installed.
 *   - On macOS the driver's default ld64 is the right answer.
 *   - Windows would want lld-link; that path is not written yet.
 */
export function discover() {
    const cc = process.env.CC || which("cc") || which("clang");
    if (!cc) throw new Error("no C compiler found (set CC)");

    const ver = run(cc, ["--version"]);
    const isClang = /clang/i.test(ver.stdout + ver.stderr);

    const ccache = which("ccache");
    const mold = PLATFORM === "linux" ? which("mold") : null;

    return { cc, isClang, ccache, mold, platform: PLATFORM };
}

/**
 * The compiler's own include directory, which holds the freestanding headers
 * (stdint.h, stddef.h, stdbool.h, ...). -nostdinc removes libc's headers, so
 * this path must be added back explicitly or even <stdint.h> disappears.
 */
export function freestandingInclude(cc, isClang) {
    const probe = isClang
        ? ["-print-resource-dir"]
        : ["-print-file-name=include"];
    const r = run(cc, probe);
    if (r.status !== 0) return [];
    const base = r.stdout.trim();
    return ["-I", isClang ? path.join(base, "include") : base];
}
