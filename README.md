# WildJIT, an Unison Language JIT

(OBS: This is EXTREMELY experimental. Not ready for production. Thanks (: )

WildJIT compiles a Unison program and runs the result as a native shared library. The Unison file stays the source. A small driver asks the Unison codebase manager (`ucm`) for a dump of the term, a compiler written in Unison turns that dump into C, and `gcc` links that C with the runtime. The driver then loads the library and calls it.

This repository holds the compiler, the runtime, the driver, and the test programs. The Unison codebase, `out/anf-cc`, `out/ujc`, and the cached libraries are local build products and are not committed.

`ucm` on this machine is release/1.5.0, at `bench/unison/unison`.

## The pieces


| File                                   | Role                                                                                                          |
| -------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `ujit.u`                      | The compiler. It reads the dump and writes C.                                                                 |
| `out/anf-cc`                  | That compiler, already built as a native binary.                                                              |
| `rt.c`, `rt.h`       | The runtime linked into every program: frames, handlers, text, numbers.                                       |
| `cache.c`, `out/ujc` | The driver. It dumps a term, runs the compiler on a cache miss, asks `gcc` for a `.so`, and `dlopen`s `main`. |
| `prog/`                       | The Unison programs used as tests.                                                                            |


A compiled program is cached by the hash `ucm` gives the term. The shared library is `out/cache/<hash>.so`. The next run of the same hash loads that file. The compiler stays idle.

`gcc` is invoked as `gcc -shared -fPIC -O0`. The cached library is an `-O0` build.

## From a Unison file to a run

1. `ujc` reads `prog/<file>.u` and the symbol to run.
2. If that symbol has no dump yet, `ujc` asks `ucm` to load the file and print the term. The dump is stored under `anf/`.
3. On a cache miss, `ujc` writes the hash to `anf/request.txt` and runs `out/anf-cc`. The compiler writes `out/anf-<hash>.c`.
4. `gcc` compiles that C together with `rt.c` into `out/cache/<hash>.so`.
5. `ujc` loads the library and calls `main`.

A later run of the same file skips `ucm` when the dump is still newer than the source, and skips the compiler when the `.so` is already there.

Two programs both named `main` would share one sidecar, `out/cache/src/main.hash`. The test programs that would collide use another symbol (`hiMain`, `fzMain`, `slugMain`).

## What the C looks like

A plain function has no `perform` and no handler in its body or in the functions it calls. The compiler emits it as a C function that takes its arguments and returns a `uint64_t`. A call to it is a C call. Tail recursion in that function is a `goto` back to the start of the function. The generated file also keeps a `Frame *` trampoline, used when the function is stored as a closure or used as the program entry.

A function that performs an ability, or handles one, stays on the frame stack. It is `void fn(Frame *)`, with a program counter and a `switch`. A handler can pause the caller and resume it. One-resume handlers and two-resume handlers both use that stack. Plain code stays off it.

Text is a slice or a concatenation of slices. `Text.take` and `Text.drop` allocate the slice header with the runtime's bump allocator. The bytes of a literal stay where they are. A slice is not a C string. The runtime copies it out only when something needs a C string.

Numbers used by the tests are `Nat`, compiled as `uint64_t`.

The reader understands the dump of the programs below. A construct outside that set stops the compiler. List matching is outside it, so the Share programs further down are rewrites with `Nat` and `Text` only.

## Test programs

These live in `prog/`. The output is what the compiled library printed.


| Program     | Symbol   | Output                                                                                      |
| ----------- | -------- | ------------------------------------------------------------------------------------------- |
| `hi.u`      | `hiMain` | `hi`, then `11`                                                                             |
| `fruit.u`   | `main`   | `10`                                                                                        |
| `choose.u`  | `main`   | `11`                                                                                        |
| `abort.u`   | `main`   | `0`                                                                                         |
| `local.u`   | `main`   | `42`                                                                                        |
| `color.u`   | `main`   | `2`, then `8`                                                                               |
| `snapin.u`  | `main`   | `1`                                                                                         |
| `snapout.u` | `main`   | `10`                                                                                        |
| `state.u`   | `main`   | `5000050000`, twice                                                                         |
| `mix.u`     | `main`   | 16 lines: `0`, `0`, `10`, `10`, `9`, `1`, `0`, `0`, `1`, `2`, `3`, `4`, `11`, `2`, `8`, `0` |


`hi`, `fruit`, `choose`, `abort`, and `state` were compiled again after plain functions became direct C calls, and printed the same lines. The other rows are from the test pass on the previous build of this same pipeline, when a plain call still went through the runtime.

`hi.u` has to force the thunk. The binding is `n = handle !chooseBody with runChoose`.

## Comparison with ucm

`ucm` runs the program by interpreting it. The time below is the whole `run.file` command, from process start through the last line. WildJIT's time is a later run, after the `.so` is already built: load the library and run it. The cached library is `-O0`.

Same printed answers on both sides. The WildJIT column is the `Nat` and `Text` rewrite. The `ucm` column is the original library.

### Fuzzy distance

Source: `@nusentry/fuzzy`, release 0.0.2, `fuzzy.levenshtein.distance`. The task is the distance from `kitten` to `sitting` (3), from `test` to `tests` (1), and the sum of 10000 distances of `kitten` / `sitting` (30000).

The rewrite in `prog/fuzzy.u` keeps one row of the same dynamic program packed in a `Nat`, base 16. `at` is `Text.take 1` of `Text.drop`.


|                                        | Time                        | Output                     |
| -------------------------------------- | --------------------------- | -------------------------- |
| `ucm`, original library, whole command | 2819.9 ms, 2851.1 ms        | `3`, `1`, `30000`          |
| WildJIT, warm                          | 129.0 ms, 111.2 ms, 99.0 ms | `fuzzy`, `3`, `1`, `30000` |


The first WildJIT command for this program, with the dump already on disk, was 642.8 ms. That includes writing the C, `gcc`, and the run.

### Slug

Source: `@hojberg/slug`, release 3.1.0, `Slug.tryFromText`. The original lowercases the text, turns anything that is not a letter or digit into a break, drops empty words, and joins the words with `-`.

The rewrite in `prog/slug.u` walks the text by index. The punctuation example is `--!@==)/()`, ASCII only. The library's own example also contains a middle dot. That character was left out of this pair of runs.


| Input                                            | Both sides    |
| ------------------------------------------------ | ------------- |
| `hello world`                                    | `hello-world` |
| `HelloWorld`                                     | `helloworld`  |
| `--!@==)/()`                                     | `none`        |
| `Hello, World!`, 10000 times, sum of the lengths | `110000`      |



|                                                   | Time                         |
| ------------------------------------------------- | ---------------------------- |
| `ucm`, original `Slug.tryFromText`, whole command | 2950.8 ms, 2815.7 ms         |
| WildJIT, warm                                     | 513.1 ms, 512.0 ms, 508.7 ms |


The first WildJIT command for `slug.u`, including the `ucm` dump, was 10.8 s.

## Run

From the repository root:

```bash
export HOME=/run/media/honinbou/HD/UnisonDB/bench/home
export TMPDIR=/run/media/honinbou/HD/UnisonDB/tmp
export UCM_WEB_UI=/run/media/honinbou/HD/UnisonDB/bench/ui

./out/ujc prog/hi.u hiMain
./out/ujc prog/fuzzy.u fzMain
./out/ujc prog/slug.u slugMain
```

`ujc` returns 0 even when a later step fails. The lines that matter are stderr `call_ns` and the program's own stdout. `call_ns` is the whole cached call. On a miss that includes the compiler and `gcc`. On a hit it is the load and the run.

`gcc ran` on that line means the `.so` was produced during the command. `gcc idle` means the cached library was reused.

To force that hash to compile again:

```bash
./out/ujc <hash> reset
```

The original Share libraries were run in their own codebases, separate from this one:

```bash
# fuzzy, codebase tmp/share-fuzzy, file tmp/fuzzy-lib.u
# slug,  codebase tmp/share-slug,  file tmp/slug-lib.u
```

A second `ucm` must not share this codebase while `ujc` is dumping a file. The Share codebases stay separate.