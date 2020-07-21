# LuaJIT metrics

* **Status**: In progress
* **Start date**: 17-07-2020
* **Authors**: Sergey Kaplun @Buristan skaplun@tarantool.org,
               Igor Munkin @igormunkin imun@tarantool.org,
               Sergey Ostanevich @sergos sergos@tarantool.org
* **Issues**: [#5187](https://github.com/tarantool/tarantool/issues/5187)

## Summary

LuaJIT metrics provide extra information about the Lua state. They consist of
GC metrics (overall amount of objects and memory usage), JIT stats (both
related to the compiled traces and the engine itself), string hash hits/misses.

## Background and motivation

One can be curious about their application performance. We are going to provide
various metrics about the several platform subsystems behaviour. GC pressure
produced by user code can weight down all application performance. Irrelevant
traces compiled by the JIT engine can just burn CPU time with no benefits as a
result. String hash collisions can lead to DoS caused by a single request. All
these metrics should be well monitored by users wanting to improve the
performance of their application.

## Detailed design

The additional header <lmisclib.h> is introduced to extend the existing LuaJIT
C API with new interfaces. The first function provided via this header is the
following:

```
/* API for obtaining various platform metrics. */

LUAMISC_API void luaM_metrics(lua_State *L, struct luam_Metrics *metrics);
```

This function fills the structure pointed to by `metrics` with the corresponding
metrics related to Lua state anchored to the given coroutine `L`.

The `struct luam_Metrics` has the following definition:

```
struct luam_Metrics {
  /* Strings amount found in string hash instead of allocation of new one. */
  size_t strhash_hit;
  /* Strings amount allocated and put into string hash. */
  size_t strhash_miss;

  /* Amount of allocated string objects. */
  size_t gc_strnum;
  /* Amount of allocated table objects. */
  size_t gc_tabnum;
  /* Amount of allocated udata objects. */
  size_t gc_udatanum;
  /* Amount of allocated cdata objects. */
  size_t gc_cdatanum;

  /* Amount of white objects. */
  size_t gc_whitenum;
  /* Amount of gray objects. */
  size_t gc_graynum;
  /* Amount of black objects. */
  size_t gc_blacknum;

  /* Memory currently allocated. */
  size_t gc_total;
  /* Total amount of freed memory. */
  size_t gc_freed;
  /* Total amount of allocated memory. */
  size_t gc_allocated;

  /* Count of incremental GC steps per state. */
  size_t gc_steps_pause;
  size_t gc_steps_propagate;
  size_t gc_steps_atomic;
  size_t gc_steps_sweepstring;
  size_t gc_steps_sweep;
  size_t gc_steps_finalize;

  /*
  ** Overall number of snap restores (amount of guard assertions
  ** leading to stopping trace executions and trace exits,
  ** that are not stitching with other traces).
  */
  size_t jit_snap_restore;
  /* Overall number of abort traces. */
  size_t jit_trace_abort;
  /* Total size of all allocated machine code areas. */
  size_t jit_mcode_size;
  /* Amount of JIT traces. */
  unsigned int jit_trace_num;
};
```

Couple of words about how metrics are collected:
- `strhash_*` -- whenever existing string is returned after attemption to
  create new string there is incremented `strhash_hit` counter, if new string
  created then `strhash_miss` is incremented instead.
- `gc_*num`, `jit_trace_num` -- corresponding counter incremented whenever new
  object is allocated. When object become garbage collected its counter is
  decremented.
- `gc_whitenum`, `gc_graynum`, `gc_blacknum` -- in so far as all objects are
  created  as the current white, `gc_whitenum` is incremented at any object
  creation. Whenever  color of object changes counter for old color is
  decremented and counter for  new color is incremented instead.
  *NB*: after full cycle of Garbage Collector there are only white objects.
- `gc_total`, `gc_allocated`, `gc_freed` -- any time when allocation function
  is called `gc_allocated` and/or `gc_freed` is increased and `gc_total`
  increase when memory is allocated or reallocated, decrease when memory is
  freed.
- `gc_steps_*` -- corresponding counter increments whenever Garbage Collector
  starts to execute 1 step of garbage collection.
- `jit_snap_restore` -- whenever JIT machine exits from the trace and restores
  interpreter state `jit_snap_restore` counter is incremented.
- `jit_trace_abort` -- whenever JIT compiler can't record the trace in case NYI
  BC this counter is incremented.
- `jit_mcode_size` -- whenever new MCode area is allocated `jit_mcode_size` is
  increased at corresponding size in bytes. Sets to 0 when all mcode area is
  freed.

All metrics are collected throughout the platform uptime. These metrics
increase monotonically and can overflow:
  - `strhash_hit`
  - `strhash_miss`
  - `gc_freed`
  - `gc_allocated`,
  - `gc_steps_pause`
  - `gc_steps_propagate`
  - `gc_steps_atomic`
  - `gc_steps_sweepstring`
  - `gc_steps_sweep`
  - `gc_steps_finalize`
  - `jit_snap_restore`
  - `jit_trace_abort`

They make sense only with comparing with their value from a previous
`luaM_metrics()` call.

There is also a complement introduced for Lua space -- `misc.getmetrics()`.
This function is just a wrapper for `luaM_metrics()` returning a Lua table with
the similar metrics. All returned values are presented as numbers with cast to
double, so there is a corresponding precision loss. Function usage is quite
simple:
```
$ ./src/tarantool
Tarantool 2.5.0-267-gbf047ad44
type 'help' for interactive help
tarantool> misc.getmetrics()
---
- gc_graynum: 4443
  strhash_hit: 53965
  gc_steps_atomic: 6
  strhash_miss: 6879
  gc_steps_sweepstring: 17920
  gc_strnum: 5759
  gc_tabnum: 1813
  gc_cdatanum: 89
  jit_snap_restore: 0
  gc_total: 1370836
  gc_udatanum: 17
  gc_steps_finalize: 0
  gc_allocated: 3616689
  jit_trace_num: 0
  gc_whitenum: 3460
  jit_mcode_size: 0
  gc_steps_sweep: 297
  jit_trace_abort: 0
  gc_freed: 2245853
  gc_steps_pause: 7
  gc_steps_propagate: 10171
  gc_blacknum: 3979
...
```

## Benchmarks

Benchmarks was taken from repo:
[LuaJIT-test-cleanup](https://github.com/LuaJIT/LuaJIT-test-cleanup).

Example of usage:
```
/usr/bin/time -f"array3d %U" ./luajit $BENCH_DIR/array3d.lua  300 >/dev/null
```
Taking into account the measurement error ~ 2%, it can be said that there is no
difference in the performance.

Benchmark results after and before patch (less is better):
```
   Benchmark   | AFTER (s) | BEFORE (s)
---------------+-----------+-----------
array3d        |   0.21    |   0.20
binary-trees   |   3.34    |   3.24
chameneos      |   2.95    |   2.99
coroutine-ring |   1.02    |   1.02
euler14-bit    |   1.04    |   1.05
fannkuch       |   6.99    |   6.81
fasta          |   8.28    |   8.28
life           |   0.48    |   0.46
mandelbrot     |   2.66    |   2.68
mandelbrot-bit |   2.01    |   1.97
md5            |   1.59    |   1.54
nbody          |   1.36    |   1.56
nsieve         |   2.11    |   2.06
nsieve-bit     |   1.54    |   1.50
nsieve-bit-fp  |   4.51    |   4.60
partialsums    |   0.58    |   0.55
pidigits-nogmp |   3.48    |   3.46
ray            |   1.62    |   1.63
recursive-ack  |   0.19    |   0.20
recursive-fib  |   1.64    |   1.67
scimark-fft    |   5.84    |   5.86
scimark-lu     |   3.33    |   3.64
scimark-sor    |   2.34    |   2.34
scimark-sparse |   4.99    |   4.93
series         |   0.95    |   0.94
spectral-norm  |   0.95    |   0.97
```
