# Native call-1-through-10 benchmark harness

This W00 harness measures deterministic PHP workloads without depending on a
future native engine. It does not claim that outer wall time is JIT compilation
time, and it never invents native-code size when no source exists.

## Quick start

Use explicit binary and scenario paths and keep ordinary results in an artifact
directory outside version control:

```sh
python3 bench/native/benchmark_runner.py \
  --php /absolute/path/to/php \
  --scenario bench/native/scenarios \
  --output-dir /tmp/php-native-benchmark \
  --repetitions 3 \
  --steady-state-calls 5 \
  --timeout 30
python3 tests/native/validate_json.py \
  /tmp/php-native-benchmark/binary-manifest.json \
  /tmp/php-native-benchmark/benchmark-result.json
```

Each repetition starts one PHP process. That process loads one descriptor,
performs its configured warmup, then invokes the same callable ten times. Calls
after call 10 are optional steady-state observations. Ten timings therefore do
not mean ten process starts.

## Scenario protocol

Descriptors conform to `scenario.schema.json`. `php_file` is relative to and
must remain below the descriptor directory. The PHP file returns a callable
accepting the descriptor's `config` object. After every invocation, outside the
timed interval, the driver computes `sha256(serialize(result))` and compares it
with `expected_checksum`. A bad result fails the sample instead of producing a
misleading timing.

The checked-in scenarios cover:

- integer/double arithmetic and a loop;
- packed and hash-like arrays;
- object properties and method calls;
- normal and thrown exception paths with `finally`;
- references and copy-on-write;
- closures and callbacks;
- generator resume;
- dynamic function calls.

They deliberately avoid I/O, clocks, random input, locale, environment values,
and unordered external data.

## Metric definitions and limits

| JSON field | Definition |
|---|---|
| `samples[].call_ns[0..9]` | `hrtime(true)` duration around each callable invocation in one PHP process. Checksum calculation is outside the timed interval. |
| `samples[].steady_state_ns` | Optional calls after call 10, measured identically. Empty unless requested. |
| `samples[].process.wall_ns` | Monotonic outer duration from spawning PHP until it terminates. This is the W00 `process_startup` measurement class despite covering the complete process lifetime; it includes PHP startup, script parsing, warmup, calls, JSON work, and teardown. |
| `samples[].process.user_ns` / `system_ns` | Child CPU time from `getrusage(RUSAGE_CHILDREN)` in a fresh probe process. |
| `samples[].process.max_rss_bytes` | Per-child maximum resident set from the same fresh probe. Linux KiB and Darwin bytes are normalized to bytes. |
| `samples[].compile_phase` | `null` in the included scenarios because they expose no independently verifiable compilation marker. The adjacent reason is mandatory. |
| `samples[].opcache` | Named snapshots from `opcache_get_status(false)` before and after timed calls, or `null` when unavailable/disabled. These are not native-engine metrics. |
| `code_size.binary_bytes` | Executable file size from `stat.st_size`. |
| `code_size.elf_text` | Optional `.text` section size from `size -A`, including tool provenance or an unsupported reason. |
| `native_code_size` | Always `{bytes: null, source: null, unsupported_reason: ...}` in W00. |

Host/kernel/CPU data appears once in `run_manifest`, never in each sample.
Aggregates contain median, linearly interpolated p90/p95, min, max, and count.
All repetitions and every raw call remain in `samples`; no outlier is removed.
Microbenchmark timings are host-, load-, build-, and clock-dependent and are not
portable performance claims.

The resource probe measures the PHP child, not its own Python startup. The outer
wall metric cannot isolate PHP process startup, parsing, OPcache compilation,
JIT compilation, warmup, or teardown. It must not be renamed to any one of those
sub-phases.

## Artifact layout

```text
<output-dir>/
  binary-manifest.json
  benchmark-result.json
  artifacts/
    <scenario-id>-<repetition>.process.json
    <scenario-id>-<repetition>.stdout.bin
    <scenario-id>-<repetition>.stderr.bin
```

The `.stdout.bin` artifact contains the raw driver protocol JSON; stderr is kept
separately. JSON artifact paths are relative. Stable key sorting is used where
meaningful. Synthetic checked-in shape examples live in `examples/` and contain
no observed host latency.

## Canonical baseline promotion

Ordinary runs can only write `--output-dir`; they never write below `baselines/`.
Promotion is a distinct guarded mode:

```sh
python3 bench/native/benchmark_runner.py \
  --php /absolute/path/inside/clean/php-src/sapi/cli/php \
  --scenario bench/native/scenarios \
  --repetitions 10 \
  --timeout 30 \
  --promote-baseline linux-x86_64-example \
  --expected-commit 47355da494ba696b1bdb6d10448a225e742bd316
```

Promotion is refused unless the canonical executable resides inside a Git
worktree, that worktree is clean, its exact commit is known, and it equals the
full `--expected-commit`. The ID is restricted to lowercase safe characters,
the destination must not already exist, and the completed result is atomically
renamed from a private staging directory into
`bench/native/baselines/<baseline-id>/`. Binary, PHP build facts, SHA-256,
source commit, host, kernel, CPU, configuration, and raw samples are retained.
The executable itself is never copied.

Review the generated provenance and measurements before committing a promoted
baseline. Do not promote a Codex sandbox or arbitrary workstation run as a
canonical result.
