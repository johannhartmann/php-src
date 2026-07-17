# Native differential and provenance harness

These W00 tools establish an engine-independent oracle. Both PHP executables are
always explicit; using the same executable on both sides is the harness smoke
test.

## Quick start

Capture binary and environment provenance:

```sh
python3 tests/native/capture_manifest.py \
  --php /absolute/path/to/reference/php \
  --json-out /tmp/reference-manifest.json
python3 tests/native/validate_json.py /tmp/reference-manifest.json
```

The manifest includes the canonical path, file SHA-256 and size, optional ELF
build ID with tool provenance, raw `php -v` bytes, selected safe `php -i` build
fields, PHP/Zend version, ZTS/NTS, debug and architecture facts, OPcache
availability, source Git state when it can be established, and separate
host/kernel/CPU metadata. Unknown source provenance is `null` with a reason; it
is never guessed. The PHP executable is never copied.

Compare one PHP file or every `*.php` below a directory:

```sh
python3 tests/native/diff_runner.py \
  --reference /absolute/path/to/reference/php \
  --candidate /absolute/path/to/candidate/php \
  --case tests/native/fixtures \
  --timeout 5 \
  --json-out /tmp/native-diff/result.json
python3 tests/native/validate_json.py /tmp/native-diff/result.json
```

No shell evaluates the executable, case, or artifact paths. Directory cases use
the relative POSIX path as their ID and are ordered bytewise by that path.

## Comparison contract

`stdout` and `stderr` are separate, byte-exact streams. They are streamed to
`.bin` artifacts, so NUL, invalid UTF-8, and large output do not enter a text
conversion or require unbounded runner memory. JSON records each artifact's
relative path, length, and SHA-256. Exit code, terminating signal, and timeout
are separate termination fields.

Normalization is disabled and the v1 contract contains no normalization rules.
Fixtures should remove nondeterminism at its source. In particular, avoid dates,
temporary absolute paths, randomized iteration order, locale-sensitive messages,
process IDs, addresses, and nondeterministic warning text. A future rule would
need a name, version, opt-in switch, documentation, and positive and negative
tests before it could enter the contract.

Runner exit codes are:

| Code | Meaning |
|---:|---|
| 0 | Every case is byte-for-byte and termination-equivalent. |
| 1 | At least one non-timeout semantic difference exists. |
| 2 | Harness/input/spawn/artifact error. |
| 3 | At least one side timed out; the complete process group was killed. |

A timeout is never reported as equivalence, even if both sides time out.

## Result and artifact layout

For `--json-out /tmp/run/result.json`, raw outputs are written below
`/tmp/run/result.artifacts/`. Artifact references inside JSON are relative to
the JSON file. Reusing the JSON path overwrites only the runner's deterministic
artifact filenames; the runner does not remove the artifact directory.

Fields listed in `volatile_fields` are the only expected per-execution changes:
timestamps, process durations, and artifact locations when `--json-out` changes.
Case IDs, hashes, lengths, termination state, ordering, and comparison status
must remain stable for deterministic fixtures.

The checked-in schemas are under `tests/native/schemas/`. Because W00 does not
add a third-party JSON Schema package, `validate_json.py` is the authoritative
strict runtime validator. The schemas document the same v1 interchange shape
for external consumers such as W00-D.

## Self-tests

```sh
python3 -m unittest discover -s tests/native -p 'test_*.py' -v
```

The suite includes deliberately failing candidate wrappers for stdout, stderr,
exit, signal, and timeout; a timeout grandchild; binary and 5 MiB outputs; a
path with spaces; stable directory ordering; repeated-run stability; manifest
capture; benchmark integration; and baseline-promotion refusal.
