# Threading in Leht

This file records a measurement, not a preference. The numbers below were taken
with `bench/thread_scaling` on the machine described at the bottom, and they
overturned the design the project started with. Re-run the benchmark before
trusting any of it on different hardware.

## Summary

**Never render in parallel with cloned contexts.** Use one independent
`leht::Context` per thread instead. Cloned contexts get *slower* as you add
threads — two clones are already a net loss, eight are roughly five times
slower than a single thread.

## Why cloning looks right and isn't

MuPDF's documented multi-threaded pattern is to open the document on one
thread, build an `fz_display_list` per page, then render those lists on worker
threads each holding an `fz_clone_context()` of the original. Clones share the
parent's object store and glyph cache, so fonts are rasterised once and reused.

The catch is that sharing that state means sharing the lock set that protects
it, and MuPDF acquires `FZ_LOCK_ALLOC` around *every* allocation. Rendering
allocates constantly, so the workers spend their time queueing on one mutex.
Adding threads adds contention without adding throughput.

## Measurements

160-page and 500-page text-heavy corpora, zoom 1.5, rendered to RGB pixmaps.
Speedup is against that strategy's own single-thread time. Three consecutive
runs varied by under 10% and never changed the ranking.

**160 pages**

| threads | clone | | independent | |
|---:|---:|---:|---:|---:|
| | elapsed | speedup | elapsed | speedup |
| 1 | 0.288 s | 1.00x | 0.387 s | 1.00x |
| 2 | 0.432 s | **0.67x** | 0.187 s | 2.08x |
| 4 | 0.473 s | **0.61x** | 0.100 s | 3.86x |
| 8 | 1.421 s | **0.20x** | 0.069 s | 5.59x |

**500 pages** — same shape, so this is not a small-input artefact.

| threads | clone | | independent | |
|---:|---:|---:|---:|---:|
| | elapsed | speedup | elapsed | speedup |
| 1 | 0.828 s | 1.00x | 1.102 s | 1.00x |
| 2 | 1.332 s | **0.62x** | 0.577 s | 1.91x |
| 4 | 1.536 s | **0.54x** | 0.288 s | 3.83x |
| 8 | 4.955 s | **0.17x** | 0.184 s | 5.98x |

Independent contexts are slower at one thread (1.102 s vs 0.828 s) because each
worker opens and parses the file itself. That cost is real but it is paid once
per thread, and parallelism repays it immediately.

## What Leht does

**Viewer — one render thread, no pool.** Single-threaded rendering runs at
roughly 600 pages/s at zoom 1.5, about 1.7 ms per page. The interactive budget
is 16 ms. A viewer has no throughput problem to solve, so it uses one render
thread with one `Context` and a display-list cache. A pool would add contention
and complexity for a negative return.

**Batch and CLI — one independent `Context` per worker thread.** This is where
throughput matters, and independent contexts scale about 5.4x on 8 threads.

Note this replaced an earlier plan to use separate *processes* for batch work.
Processes are unnecessary: independent contexts within one process already
scale, which avoids fork, IPC and result merging entirely.

## The detail that makes independent contexts work

`fz_locks_context` carries a `user` pointer, and MuPDF hands it back to every
lock and unlock callback. Leht allocates one mutex array per base `Context` and
passes it through that pointer, so two independent contexts genuinely share no
locks.

Skipping this is an easy mistake with a severe cost. A widely used MuPDF binding
reached its mutexes through a file-scope global instead, so every context in the
process serialised on the same three mutexes no matter how they were created —
[ten threads ran 13.3x slower than one](https://github.com/messense/mupdf-rs/issues/260)
on a 6-core machine. With per-context lock sets, the independent strategy
measured here scales as the hardware allows.

This is why `Context::clone()` shares `locks_` via `shared_ptr`: clones *must*
share the parent's lock set for the shared store to be safe. It is also exactly
why clones cannot escape the contention.

## Constraints that still hold

- Only one thread may touch a document at a time — opening it, loading pages,
  building display lists. This is a MuPDF rule, not a Leht choice.
- A display list may only be rendered by the context that created it or a clone
  of that context. Independent contexts cannot share display lists, which is why
  each batch worker opens the file itself.
- `Context::clone()` remains in the API because sharing display lists is
  occasionally the right tool. It is not the tool for parallel rendering.

## Reproducing

```sh
./tests/corpus/generate.sh
cmake -S . -B build -G Ninja -DLEHT_BUILD_BENCH=ON
cmake --build build
./build/bench/bench_thread_scaling tests/corpus/text_160p.pdf 160 1.5
```

Measured on Fedora 44, MuPDF 1.28.2, GCC 16.2.1, RelWithDebInfo, on a 13th Gen
Intel Core i7-13700H (14 cores / 20 threads). Different hardware will shift the
magnitudes; re-measure before relying on the exact figures.
