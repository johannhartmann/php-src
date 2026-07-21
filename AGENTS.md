# Repository instructions

## Scope and precedence

These instructions apply to the entire repository. Read and obey
`CONTRIBUTING.md`, `CODING_STANDARDS.md`, and every deeper `AGENTS.md` that
applies to a changed path. Deeper instructions may add constraints for their
scope; they do not relax repository-wide safety or testing rules.

Keep changes focused and preserve unrelated work already present in the
worktree. Repository paths are not assigned through persistent task, phase, or
wave ownership manifests.

## Native-engine contracts

Use these pinned references when work depends on the native-engine design:

- php-src baseline: `47355da494ba696b1bdb6d10448a225e742bd316`;
- TPDE reference: `338d41890e424b058e2053b6a5787e1348e3dd57`.

Do not introduce a production VM fallback in native-engine code. Do not change
public ABI, persistent formats, or dependencies without an explicit contract,
compatibility analysis, and the tests required by that contract.

The native-engine build and test entry points are:

- `scripts/native/configure-dev.sh`
- `scripts/native/build.sh`
- `scripts/native/test-smoke.sh`
- `scripts/native/test-sanitizers.sh`

See `docs/native-engine/test-command-contract.md` for exit semantics.

## Completion

Run the task-specific checks and the applicable php-src tests before completion.
Run `git diff --check` and inspect `git status --short`. Report every command and
its real result; never hide failures or claim an unavailable check passed. Leave
the worktree clean at completion.
