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
| **debian** | does the glibc path compile and pass its tests? | the base's architecture |
| **debian**, `--platform linux/amd64` | the x86_64 half of the assembly, and the architecture CI uses | qemu, if the host is arm64 |
| **alpine** | is the port real, or is it glibc's headers? | the base's architecture |

**Check which architecture you are running.** A plain `podman build` uses
whatever `debian:13-slim` is already in the local store, so the image follows
the last `--platform` that pulled a base rather than the host:

```sh
podman image inspect cvm-linux --format '{{.Architecture}}'
```

On this project's development machine that has meant amd64 under qemu, which
is the architecture CI uses and therefore the more useful of the two. For a
native arm64 image, pull the base first and build against it:

```sh
podman pull --platform linux/arm64 debian:13-slim
podman build -t cvm-linux-arm64 --platform linux/arm64 \
    -f tools/containers/Containerfile.debian tools/containers
```

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

## What the containers have already answered

`--asan` and `--tsan` fail in a container for two reasons that are not the
project's, and both are worth knowing before reading a red run:

- **asan** CHECK-fails inside its own allocator:
  `sanitizer_allocator_primary32.h:292 ((res)) < ((kNumPossibleRegions))`.
  That is asan on Linux meeting a large address-space reservation; macOS's
  sanitizer layout does not hit it. The runtime's reservation is the thing to
  look at, not the collector.
- **tsan** CHECK-fails on `personality(ADDR_NO_RANDOMIZE)` -- it disables ASLR
  to get a stable shadow layout, and qemu-user does not implement that
  syscall. This one is the container's fault entirely; on a real x86_64
  runner it should not appear, which is exactly the kind of thing a container
  cannot settle and the runner has to.

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
