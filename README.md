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
- `xpdf-4.06/`: target source tree used in the current setup.

## Build

The Makefile builds a static library, `libmainhook.a`, that is linked into the target.

Available build targets:

- `make`: default build; optimized with strict warnings enabled

Typical build:

```bash
make
```

This produces:

```text
libmainhook.a
```

## Target Integration

The fuzzer library expects the target to be linked with:

- LLVM coverage instrumentation using `-fsanitize-coverage=inline-8bit-counters`
- linker wrapping for:
  - `malloc`
  - `calloc`
  - `realloc`
  - `free`
  - `fopen`
  - `fclose`
  - `exit`
- the target entry renamed to `targetMain`

The existing Xpdf build already shows this pattern for `pdftotext`, including:

- `-Dmain=targetMain`
- `-fsanitize-coverage=inline-8bit-counters`
- `-Wl,--wrap=malloc` and related wrappers

In the Xpdf source tree, these lines belong in
`xpdf-4.06/xpdf/CMakeLists.txt`, in the `pdftotext` target block
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

## Running

The fuzzer binary interface is:

```bash
./fuzzer_binary <target_path> <number_of_trials>
```

In the current tree, the main driver takes:

```bash
<target_path> <number_of_trials>
```

Example with the instrumented Xpdf `pdftotext` target:

```bash
mkdir -p /dev/shm/dev/shm/temp_fuzzer_workdir/interesting
./xpdf-4.06/build/xpdf/pdftotext ./xpdf-4.06/build/xpdf/pdftotext 1
```

If your actual executable name differs, use that executable and pass the same target path as the first argument.

Each trial runs for up to 24 hours:

- one mapped temp input file is reused
- progress is printed every 10,000 testcases
- coverage-increasing inputs are added to the in-memory corpus

## Runtime Artifacts

By default, the fuzzer uses these paths from `fuzzer_internal.h`:

- temp input: `/dev/shm/temp_fuzzer_workdir/temp_fuzz_input.pdf`
- result log: `/dev/shm/temp_fuzzer_workdir/Real_World_Fuzzer_Result.txt`
- interesting inputs: `/dev/shm/temp_fuzzer_workdir/interesting`
- seed corpus: `seeds`
- dictionary: `pdf.dict`

Make sure `/dev/shm/temp_fuzzer_workdir/interesting` exists before starting a run.

## How Interesting Inputs Are Chosen

An input is considered interesting when it increases at least one byte in the global 64K coverage map. When that happens, the fuzzer:

- copies the testcase into the in-memory corpus
- increments `interesting_cnt`
- writes the input to `/dev/shm/dev/shm/temp_fuzzer_workdir/interesting`

This makes the corpus self-expanding during a run.

## Design Notes

The single-process model depends on restoring state after each execution:

- heap allocations made by the target are tracked and freed
- open `FILE *` handles created by the target are tracked and closed
- writable global data is restored from a startup snapshot
- explicit `exit()` from the target is redirected through `longjmp`
- sanitizer counters are zeroed between executions

This is the core tradeoff: higher throughput than process-per-input fuzzing, but correctness depends on how well target state is reset.

## Limitations

- The current mutation strategy is PDF-specific.
- Crash persistence is not fully wired into a dedicated `crashes/` output path in the current main loop.
- The runtime assumes the target can be safely reinvoked after the custom cleanup/reset sequence.
- The current setup is tied to the way the Xpdf target was instrumented and linked.

## Quick Start

```bash
make
mkdir -p /dev/shm/dev/shm/temp_fuzzer_workdir/interesting
./xpdf-4.06/build/xpdf/pdftotext ./xpdf-4.06/build/xpdf/pdftotext 1
```

After the run starts, check:

- stdout for periodic stats
- `/dev/shm/dev/shm/temp_fuzzer_workdir/Real_World_Fuzzer_Result.txt` for the persistent log
- `/dev/shm/dev/shm/temp_fuzzer_workdir/interesting/` for coverage-increasing PDFs
