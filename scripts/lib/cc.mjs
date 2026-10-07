// scripts/lib/cc.mjs
//
// Compile and link primitives. Given sources and flags, produce artifacts.
// Knows about object layout under build/; knows nothing about GC, and
// nothing about what the artifacts are for.

import path from "node:path";
import fs from "node:fs";
import { runOrDie } from "./exec.mjs";
import { mkdirp } from "./util.mjs";

/**
 * ccache works as a compiler prefix: `ccache cc -c ...`.
 * When absent we invoke the compiler directly.
 */
function compilerCommand(toolchain) {
    return toolchain.ccache
        ? { cmd: toolchain.ccache, wrap: [toolchain.cc, ] }
        : { cmd: toolchain.cc, wrap: [] };
}

export function compile({ src, obj, flags, toolchain, verbose, ui }) {
    mkdirp(path.dirname(obj));
    const { cmd, wrap } = compilerCommand(toolchain);
    const args = [...wrap, ...flags, "-c", src, "-o", obj];
    runOrDie(cmd, args, { verbose, ui, what: `compile ${src}` });
    return obj;
}

export function link({ objs, out, toolchain, flags = [], libs = [], verbose, ui }) {
    mkdirp(path.dirname(out));
    // mold is ELF-only; on macOS the driver default (ld64) is correct.
    const linker = toolchain.mold ? ["-fuse-ld=mold"] : [];
    const args = [...linker, ...objs, ...libs, "-o", out, ...flags];
    runOrDie(toolchain.cc, args, { verbose, ui, what: `link ${out}` });
    return out;
}

/**
 * Object path for a source file under build/obj/<config>/<flavor>/.
 *
 * The extension is kept rather than replaced: `foo.c` and `foo.S` are two
 * different translation units, and mapping both to `foo.o` means one
 * silently overwrites the other. That is exactly what happened to the
 * safepoint trampoline, whose assembly and C halves share a stem — the
 * link failed with an undefined symbol while the build reported success.
 */
export function objectPath({ root, buildDir, config, flavor, src }) {
    const rel = path.relative(root, src) + ".o";
    return path.join(buildDir, "obj", config, flavor, rel);
}

export function exists(p) {
    return fs.existsSync(p);
}
