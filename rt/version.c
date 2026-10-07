/*
 * rt/version.c
 *
 * The ABI version symbols. Platform-independent, hence not under pal/.
 *
 * The version is exposed both as a constant and through a function. The
 * function matters: it forces a real symbol reference, so a vm object
 * built against one version and linked against a different rt fails at
 * load rather than silently resolving to a constant folded in at compile
 * time.
 */

#include "rt.h"

const u32 rt_abi_version = RT_ABI_VERSION;

const u32* rt_abi_version_ptr(void) {
    return &rt_abi_version;
}
