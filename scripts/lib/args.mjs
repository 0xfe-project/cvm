// scripts/lib/args.mjs
//
// Shared command-line surface for the build scripts.

export function parseArgs(argv) {
    const has = (f) => argv.includes(f);

    const asan = has("--asan");
    const tsan = has("--tsan");
    if (asan && tsan) {
        console.error("error: --asan and --tsan are mutually exclusive");
        process.exit(1);
    }

    const release = has("--release");

    return {
        release,
        config: release ? "release" : "debug",
        asan,
        tsan,
        test: has("--test"),
        clean: has("--clean"),
        verbose: has("-v") || has("--verbose"),
    };
}

export const help = {
    usage: "node scripts/build.mjs [options]",
    options: [
        "--release          optimize (-O2 -DNDEBUG); default is debug",
        "--asan             build with AddressSanitizer",
        "--tsan             build with ThreadSanitizer",
        "--test             also compile and link tests/",
        "--clean            remove build/ and exit",
        "-v, --verbose      echo every compiler invocation",
    ],
};
