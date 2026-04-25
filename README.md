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
- `interesting/`, `crashes/`: local directories in the repo. The runtime currently writes interesting inputs under `/dev/shm/trial14/interesting`.
- `xpdf-4.06/`: target source tree used in the current setup.

## Build

The Makefile builds a static library, `libobj14hook.a`, that is linked into the target.

Available build targets:

- `make fuzzer_step1`: strict warning-oriented build
- `make fuzzer_step3`: optimized build
- `make llvm_testing_step5`: ASan + UBSan build
- `make fuzzer_release_step5`: optimized build with strict warnings enabled

Typical build:

```bash
make fuzzer_release_step5
```

This produces:

```text
libobj14hook.a
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
mkdir -p /dev/shm/trial14/interesting
./xpdf-4.06/build/xpdf/pdftotext ./xpdf-4.06/build/xpdf/pdftotext 1
```

If your actual executable name differs, use that executable and pass the same target path as the first argument.

Each trial runs for up to 24 hours:

- one mapped temp input file is reused
- progress is printed every 10,000 testcases
- coverage-increasing inputs are added to the in-memory corpus

## Runtime Artifacts

By default, the fuzzer uses these paths from `fuzzer_internal.h`:

- temp input: `/dev/shm/trial14/temp_fuzz_input.pdf`
- result log: `/dev/shm/trial14/Real_World_Fuzzer_Result.txt`
- interesting inputs: `/dev/shm/trial14/interesting`
- seed corpus: `seeds`
- dictionary: `pdf.dict`

Make sure `/dev/shm/trial14/interesting` exists before starting a run.

## How Interesting Inputs Are Chosen

An input is considered interesting when it increases at least one byte in the global 64K coverage map. When that happens, the fuzzer:

- copies the testcase into the in-memory corpus
- increments `interesting_cnt`
- writes the input to `/dev/shm/trial14/interesting`

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
- The default output directory is hardcoded to `/dev/shm/trial14`.
- The current setup is tied to the way the Xpdf target was instrumented and linked.

## Quick Start

```bash
make fuzzer_release_step5
mkdir -p /dev/shm/trial14/interesting
./xpdf-4.06/build/xpdf/pdftotext ./xpdf-4.06/build/xpdf/pdftotext 1
```

After the run starts, check:

- stdout for periodic stats
- `/dev/shm/trial14/Real_World_Fuzzer_Result.txt` for the persistent log
- `/dev/shm/trial14/interesting/` for coverage-increasing PDFs
