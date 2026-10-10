# tools/containers — running the Linux path without waiting on CI

The platform layer has two halves. The Darwin half runs every day. The other
half — `rt/pal/posix/` on Linux, `vm/safepoint_trap.S`'s `__linux__` branches —
was written from the documentation and has been *read* ever since, never run.
"Compile-time checked only" is what the audit calls it, and the first time it
was actually compiled it did not compile.

These three containers are how that gets fixed without a CI queue in the loop.
Each answers a different question:

| container | question | speed |
|---|---|---|
| **debian**, arm64 | does the glibc path compile and pass its tests? | native — the iteration loop |
| **debian**, `--platform linux/amd64` | the x86_64 half of the assembly, and the architecture CI uses | qemu — the confirming run |
| **alpine**, arm64 | is the port real, or is it glibc's headers? | native |

## Build

```sh
podman build -t cvm-linux       -f tools/containers/Containerfile.debian tools/containers
podman build -t cvm-linux-amd64 -f tools/containers/Containerfile.debian \
    --platform linux/amd64 tools/containers
podman build -t cvm-alpine      -f tools/containers/Containerfile.alpine tools/containers
```

## Run

From the repository root. `:Z` relabels the mount for podman's SELinux
handling; it is harmless where there is no SELinux.

```sh
podman run --rm -v "$PWD:/src:Z" -w /src cvm-linux \
    node scripts/build.mjs --test
```

Mount a ccache directory to keep it across runs — the gate compiles the whole
runtime four times, once per configuration:

```sh
mkdir -p ~/.cache/cvm-linux-ccache
podman run --rm -v "$PWD:/src:Z" -w /src \
    -v ~/.cache/cvm-linux-ccache:/ccache \
    cvm-linux node scripts/build.mjs --test
```

## What they do not replace

**They do not replace the CI run.** The runner is x86_64 Ubuntu 24.04 with
clang 18 and glibc 2.39; the Debian container is clang 19 and glibc 2.41. Two
things follow:

- A local container narrows the problem to "this is broken on Linux, not only
  on the runner", which is the expensive half to discover. The compiler
  difference is what is left, and it has already produced one real finding:
  the runner's clang writes `__cxa_atexit`/`__dso_handle` into a sanitizer
  build's objects where the development machine's does not.
- Anything that passes in a container still has to pass on the runner before
  the matrix is enabled. The matrix is enabled when the runner is green, not
  when the container is.
