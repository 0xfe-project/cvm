// scripts/lib/flags.mjs
//
// Compiler flag sets. Two flavors exist because the project is split:
//   freestanding  rt/ and vm/  — no libc at all
//   hosted        tests/       — ordinary userland, libc allowed

import path from "node:path";
import { freestandingInclude } from "./toolchain.mjs";

export const STD = "-std=c11";

/** Warnings and codegen knobs shared by both flavors. */
const COMMON = [
    STD,
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wpedantic",
    "-fno-common",
];

export const DEBUG_FLAGS = ["-g", "-O0", "-fno-omit-frame-pointer", "-DCVM_DEBUG"];
export const RELEASE_FLAGS = ["-O2", "-DNDEBUG"];

export function sanitizerFlags({ asan, tsan }) {
    if (asan) return ["-fsanitize=address"];
    if (tsan) return ["-fsanitize=thread"];
    return [];
}

/**
 * Build the include path list.
 *
 *   rt/          so `#include "base/types.h"` resolves
 *   vm/include/  so `#include "cvm/gc.h"` resolves  (public ABI)
 *   vm/          so `#include "gc/layout.h"` resolves (internal)
 *   tests/       hosted only — test-only headers must not be reachable
 *                from freestanding translation units.
 *
 * Note the layering rule this encodes: headers under rt/base/ are shared
 * with the freestanding vm layer, so they must stay libc-free even though
 * rt/*.c files may use libc freely. The freestanding probe is what
 * enforces that.
 */
export function includeFlags(root, flavor) {
    const dirs = [
        path.join(root, "rt"),
        path.join(root, "vm", "include"),
        path.join(root, "vm"),
    ];
    if (flavor === "hosted") dirs.push(path.join(root, "tests"));
    return dirs.flatMap((d) => ["-I", d]);
}

export function flagsFor({ flavor, opts, toolchain, root, extra = [] }) {
    const base = flavor === "freestanding"
        ? [...COMMON, "-ffreestanding", "-nostdinc",
           ...freestandingInclude(toolchain.cc, toolchain.isClang)]
        : [...COMMON];

    return [
        ...base,
        ...(opts.release ? RELEASE_FLAGS : DEBUG_FLAGS),
        ...sanitizerFlags(opts),
        ...includeFlags(root, flavor),
        ...extra,
    ];
}
