# Vendored test dependencies

## greatest.h

Single-header C test framework, ISC licensed.
Source: https://github.com/silentbicycle/greatest

Chosen for minimal footprint (one file, no build system, no pkg-config)
with readable colored output, suite grouping, and per-test filtering.

Update with:

    curl -sL -o tests/vendor/greatest.h \
      https://raw.githubusercontent.com/silentbicycle/greatest/master/greatest.h

Not on the freestanding include path: only `tests/` translation units may
see it (see scripts/lib/flags.mjs).
