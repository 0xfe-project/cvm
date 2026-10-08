#ifndef TGC_H
#define TGC_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

enum {
  TGC_MARK = 0x01,
  TGC_ROOT = 0x02,
  TGC_LEAF = 0x04
};

typedef struct {
  void *ptr;
  int flags;
  size_t size, hash;
  void (*dtor)(void*);
} tgc_ptr_t;

typedef struct {
  void *bottom;
  int paused;
  uintptr_t minptr, maxptr;
  tgc_ptr_t *items, *frees;
  double loadfactor, sweepfactor;
  size_t nitems, nslots, mitems, nfrees;

  /*
   * Metrics added by this project, not part of the upstream tiny GC.
   *
   * Upstream reports only table occupancy (nitems/nslots), which cannot
   * answer the two questions the benchmark asks of every backend: how many
   * allocations happened, and how long the longest collection stopped the
   * program. Both are counted here rather than inferred, and both are
   * additive — nothing in tgc's own behaviour reads them.
   *
   * They are in the vendored copy because a tgc that reports different
   * numbers than it did upstream would make the comparison a comparison of
   * two collectors. The three fields below are the entire difference, and
   * bench/tgc/README.md says so.
   */
  uint64_t bench_allocations;
  uint64_t bench_collections;
  uint64_t bench_pause_max_ns;
} tgc_t;

void tgc_start(tgc_t *gc, void *stk);
void tgc_stop(tgc_t *gc);
void tgc_pause(tgc_t *gc);
void tgc_resume(tgc_t *gc);
void tgc_run(tgc_t *gc);

void *tgc_alloc(tgc_t *gc, size_t size);
void *tgc_calloc(tgc_t *gc, size_t num, size_t size);
void *tgc_realloc(tgc_t *gc, void *ptr, size_t size);
void tgc_free(tgc_t *gc, void *ptr);

void *tgc_alloc_opt(tgc_t *gc, size_t size, int flags, void(*dtor)(void*));
void *tgc_calloc_opt(tgc_t *gc, size_t num, size_t size, int flags, void(*dtor)(void*));

void tgc_set_dtor(tgc_t *gc, void *ptr, void(*dtor)(void*));
void tgc_set_flags(tgc_t *gc, void *ptr, int flags);
int tgc_get_flags(tgc_t *gc, void *ptr);
void(*tgc_get_dtor(tgc_t *gc, void *ptr))(void*);
size_t tgc_get_size(tgc_t *gc, void *ptr);

#endif

