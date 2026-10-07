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

    /*
     * The configuration name includes the sanitizer.
     *
     * Object files and test binaries are keyed by it, so without this a
     * `--asan` build and a plain one write to the same paths and the
     * second run can execute the first one's binaries. That produced a
     * hang that looked like a race in the collector and was nothing of the
     * kind — which is the sort of hour nobody gets back.
     */
    const variant = asan ? "-asan" : tsan ? "-tsan" : "";

    return {
        release,
        config: (release ? "release" : "debug") + variant,
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
