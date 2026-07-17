# Native-engine wave gates

This directory keeps three data classes deliberately separate:

- `waves.json` is the static, reviewed definition of waves, task IDs, ownership, and required gates.
- A task result is immutable evidence from one concrete Codex, build, test, or integration run.
- `status.md` is a generated, deterministic projection of the definition and recorded results. It is never a source of truth.

All checked-in schemas use JSON Schema Draft 2020-12. The CLI uses only the Python standard library and implements a small structural validator for the fields, types, IDs, and enums used here. It does **not** claim general JSON Schema support. External tooling should validate against the files in `schemas/`.

## Format and ID stability

The current `format_version` is `1.0.0`. Wave, task, and gate IDs are immutable within version 1.x; display titles may change. An incompatible field or ID change requires:

1. a new major format version;
2. a migration note describing old-to-new fields and IDs;
3. dual-read support in `wave-gate.py` for one integration wave before old input is rejected.

## Status model

Task statuses are `pending`, `running`, `pass`, `fail`, `blocked`, and `skipped`.

```text
pending -> running -> pass
                   -> fail
                   -> blocked
                   -> skipped (optional gates only)
```

A later audited result may replace a terminal result after a rerun. `skipped` is allowed only for an optional gate whose platform or feature is explicitly outside the current wave, with the reason captured in evidence or blockers. A required gate that is `skipped`, `blocked`, or `fail` can never make a wave pass. Missing required results, stale base commits, dirty implementation worktrees, failed acceptance criteria/tests, and missing passing evidence also prevent `PASS`.

Wave summaries additionally use `missing` when at least one required task has no recorded result. Optional results are displayed but do not determine the required gate.

## CLI

Run from the repository root:

```bash
python3 scripts/native/wave-gate.py validate-definition
python3 scripts/native/wave-gate.py validate-result result.json
python3 scripts/native/wave-gate.py record --wave W00 --result result.json --results-dir artifacts/waves
python3 scripts/native/wave-gate.py check --wave W00 --results-dir artifacts/waves
python3 scripts/native/wave-gate.py list-missing --wave W00 --results-dir artifacts/waves
python3 scripts/native/wave-gate.py render --results-dir artifacts/waves --output docs/native-engine/waves/status.md
```

Exit codes are stable:

| Code | Meaning |
| --- | --- |
| `0` | Input valid or requested wave passes |
| `1` | Gate does not pass or required results are missing |
| `2` | JSON, structural schema, result identity, duplicate, or audit data is invalid |
| `64` | Command-line usage error |

`record` writes `<results-dir>/<wave-id>/<task-id>.json` through a temporary file, `fsync`, and atomic rename. Recording byte-equivalent parsed JSON is idempotent. Different content or commit/task identity is rejected unless `--replace` is explicit; replacement appends old and new identities to `<results-dir>/.audit.json`. Results from separate runners should be merged by task ID before `check`; a duplicate with differing identity is a conflict, not “last writer wins.”

Local artifact references must be relative and need not exist on the machine that performs aggregation. CI artifacts use `{"kind":"ci","reference":"<provider artifact ID>"}` and are treated as remote references. Fixtures never contain credentials or absolute worktree paths.

`render` sorts waves and task IDs. Timestamps are omitted by default so identical inputs produce byte-identical Markdown; `--include-timestamps` adds recorded timestamps when desired.

## W00 hard gate

W00 passes only when all required IDs in `waves.json` pass. They cover repository contracts, four normal build profiles, ASan and UBSan, worktree isolation, differential self-tests, a referenced warm-call baseline, a reference manifest, the D schema and dashboard checks, unchanged existing tests, and confirmation that W00 made no functional PHP change. Every implementation result must report a clean worktree and passing evidence.

The AArch64 runner is optional in W00 and becomes a later platform gate. Linux/ELF x86-64 is the primary W00 platform.

## W01 hard gate

W01 passes only when all seven required semantic-contract results pass. Every
executable Zend opcode must be catalogued exactly once with a conservative
effect and ownership description; the accepted frame, safepoint, bailout, and
resume contracts must cover every observable boundary; and every pinned TPDE
gap must have a source-backed classification. Each required opcode family must
also have deterministic semantic-oracle evidence.

References, exceptions, and destructors may not contain unresolved semantic
placeholders. Missing A--E results, unregistered cross-track identifiers,
unresolved critical blockers, or a missing integration result prevent W01 from
passing. W01 inventories and freezes contracts only; it must not change PHP
runtime semantics, the public ABI, or the build system.

## Codex and CI result capture

Codex event output and the final task result are different artifacts. Capture the JSONL event stream for execution diagnostics while writing the schema-constrained final response separately:

```bash
codex exec - \
  --sandbox workspace-write \
  --json \
  --output-schema docs/native-engine/waves/schemas/codex-task-result.schema.json \
  --output-last-message artifacts/waves/final-task-result.json \
  < implementation-prompt.txt \
  > artifacts/waves/codex-events.jsonl
```

`codex-events.jsonl` contains lifecycle and tool events and must not be passed to `record`. Validate and record only `final-task-result.json`. This separation follows the current Codex CLI contracts for [JSONL output and schema-constrained final output](https://learn.chatgpt.com/docs/developer-commands?surface=cli#cli-codex-exec).

The four `fixtures/valid/task-result-{A,B,C,D}.json` files are synthetic examples, not claims that W00 gates ran. Real latency, build, sanitizer, and benchmark evidence belongs in CI/local artifact storage rather than this repository.

## Files

- `schemas/wave-definition.schema.json`: static definition contract.
- `schemas/codex-task-result.schema.json`: final task response contract and Codex output schema.
- `schemas/gate-evidence.schema.json`: evidence reference contract.
- `schemas/wave-summary.schema.json`: deterministic aggregate contract.
- `fixtures/`: synthetic positive and negative inputs.
- `tests/test_wave_gate.py`: standard-library self-tests.
- `status.md`: generated empty-results projection for the checked-in definition.
