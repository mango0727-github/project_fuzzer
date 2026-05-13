# Single-Process PDF Fuzzer

This repository contains a coverage-guided, single-process fuzzer for PDF parsers. The fuzzer is designed to run the target in-process, reuse the same address space across test cases, and recover target state between executions by resetting sanitizer counters, tracked heap/file resources, and a snapshot of writable global data.

The current setup is tailored to an instrumented `pdftotext` target from Xpdf, but the runtime model is generic enough to reuse with another file-based target if it is linked the same way.

## What It Does

- Runs the target in a single process instead of `fork()` per testcase.
- Uses LLVM `inline-8bit-counters` coverage to decide when a testcase is interesting.
- Starts from seed PDFs in `seeds/`.
- Mutates existing corpus entries with PDF-aware heuristics:
  - object splicing between seeds
  - dictionary token insertion from `pdf.dict`
  - numeric field corruption for size/count-style keys
  - stream byte mutation
  - xref/trailer reconstruction
- Saves coverage-increasing inputs as new interesting seeds.
- Reuses the same mapped temp file for execution to reduce overhead.

## Repository Layout

- `main.c`: main fuzzing loop, progress logging, temp file management, trial execution.
- `mutator.c` / `corpus.c`: seed loading, corpus management, PDF-oriented mutation logic.
- `coverage.c`: global coverage map and coverage-delta check.
- `runtime.c`: re-entrant target execution and state reset after each run.
- `target_shim.cc`: wrappers for `malloc`/`free`, `fopen`/`fclose`, `realloc`, and `exit`; also snapshots writable global state.
- `trace_counter.c`: captures LLVM sanitizer counter section boundaries.
- `fuzzer_internal.h`: shared constants, defaults, and cross-module declarations.
- `pdf.dict`: mutation dictionary used for token insertion.
- `seeds/`: initial PDF corpus.
- `interesting/`, `crashes/`: local directories in the repo. The runtime currently writes interesting inputs under `/dev/shm/dev/shm/temp_fuzzer_workdir/interesting`.


## Target Integration

The fuzzer library expects the target to be linked with:

- linker wrapping for:
  - `malloc`
  - `calloc`
  - `realloc`
  - `free`
  - `fopen`
  - `fclose`
  - `exit`
- the target entry renamed to `targetMain`

In the Xpdf source tree, these lines should be added to `xpdf-4.06/xpdf/CMakeLists.txt`, in the `pdftotext` target block
immediately after `add_executable(pdftotext ...)`:

```cmake
target_compile_definitions(pdftotext PRIVATE main=targetMain)

target_link_options(pdftotext PRIVATE
    "-Wl,--wrap=malloc"
    "-Wl,--wrap=calloc"
    "-Wl,--wrap=free"
    "-Wl,--wrap=exit"
    "-Wl,--wrap=fopen"
    "-Wl,--wrap=fclose"
    "-Wl,--wrap=realloc"
)

target_link_libraries(pdftotext PRIVATE
    YOURWORKDIR/libmainhook.a
    goo
    fofi
    ${PAPER_LIBRARY}
    ${LCMS_LIBRARY}
    ${FONTCONFIG_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
)
```

The target shim also expects these environment variables to be set before fuzzing:

- `CLOSURE_GLOBAL_SECTION_ADDR`
- `CLOSURE_GLOBAL_SECTION_SIZE`

These are used to snapshot and restore writable global state between iterations.



## How to restore state

The single-process model depends on restoring state after each execution:

- heap allocations made by the target are tracked and freed
- open `FILE *` handles created by the target are tracked and closed
- writable global data is restored from a startup snapshot
- explicit `exit()` from the target is redirected through `longjmp`
- sanitizer counters are zeroed between executions

This is the core tradeoff: correctness depends on how well target state is reset, however throughput degrades by 4.8 times comparing to the single-process fuzzer without resetting state.

