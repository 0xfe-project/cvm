// scripts/lib/flags.mjs
//
// Compiler flag sets. Two flavors exist because the project is split:
//   freestanding  vm/ only — no libc at all
//   hosted        rt/ and tests/ — ordinary userland, libc allowed
//
// (rt/ used to be freestanding too; it is the platform layer, so it is the
// one place that has to talk to the OS.)

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

/*
 * Stack protector is off for the freestanding flavor.
 *
 * The compiler emits references to __stack_chk_guard and __stack_chk_fail
 * (a libSystem/libc symbol on macOS, a TLS slot on x86_64 Linux). Neither
 * is something a libc-free translation unit can provide correctly: the
 * guard must hold a per-process random value, and on macOS the symbol is
 * shared process-wide, so defining our own would interpose on libSystem's
 * and weaken the protection for everything else in the process.
 *
 * The alternative -- a vm-owned guard initialised by rt at startup -- is
 * possible but needs the interposition question answered first, and buys
 * little today: vm has no unbounded stack buffers, and its one formatter
 * is bounded by construction.
 *
 * Leaving it enabled and whitelisting the symbols in the audit was the
 * other option, and was rejected: the audit's value comes from being
 * strict, and a benign-looking entry in the allowlist is how a real libc
 * dependency gets in unnoticed later.
 */
/*
 * Trap-based safepoints, where the platform supports them and the build
 * allows them.
 *
 * The poll becomes a load from a per-thread page instead of a flag check:
 * one instruction, no branch, and nothing for the compiler to reorder
 * around. It needs a signal handler and a per-architecture trampoline.
 *
 * Off in two cases, for two different reasons:
 *
 *   Windows — no trampoline, so the collector polls. The mechanism is
 *   portable C plus per-architecture assembly, and the assembly is what is
 *   missing there.
 *
 *   Under a sanitizer — asan and tsan install their own SIGSEGV handlers
 *   and expect to see every fault. A trap fault is deliberately handled
 *   and resumed, which the sanitizer runtime does not expect, and the
 *   observed result is a hang before the program prints anything. The
 *   polling mode is what the sanitizer configurations exercise, which
 *   means both modes are covered across the matrix: traps in the plain
 *   debug and release runs, polling under asan and tsan.
 */
function safepointTrapFlags(opts) {
    if (process.platform === "win32" || opts.asan || opts.tsan) return [];
    return ["-DCVM_SAFEPOINT_TRAPS"];
}

/** Flags shared by both flavors, before the flavor-specific ones. */
function commonFor(opts) {
    return [...COMMON, ...featureTestFlags(), ...safepointTrapFlags(opts)];
}

/**
 * The platform's feature-test macro, where one is needed.
 *
 * macOS exposes the BSD-style jump API -- sigjmp_buf, sigsetjmp,
 * siglongjmp -- unconditionally, and this project uses it: a signal that
 * arrives during a safepoint is not something setjmp/longjmp can be trusted
 * to survive, so rt_jump stores a sigjmp_buf. glibc declares the same API
 * only under _GNU_SOURCE (it is in the __USE_MISC group, which -std=c11
 * turns off by defining __STRICT_ANSI__). Without this, the first Linux
 * compile stops at rt/rt_jump.h: unknown type name 'sigjmp_buf' -- and the
 * errors after it are consequences, because an undeclared type name is
 * parsed as an expression.
 *
 * Harmless for the freestanding flavor, which never includes a libc header.
 */
function featureTestFlags() {
    return process.platform === "linux" ? ["-D_GNU_SOURCE"] : [];
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
    const base =
        flavor === "freestanding"
            ? [...commonFor(opts), "-ffreestanding", "-nostdinc",
               "-fno-stack-protector",
               ...freestandingInclude(toolchain.cc, toolchain.isClang)]
            : commonFor(opts);

    return [
        ...base,
        ...(opts.release ? RELEASE_FLAGS : DEBUG_FLAGS),
        ...sanitizerFlags(opts),
        ...includeFlags(root, flavor),
        ...extra,
    ];
}
