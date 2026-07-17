# Repository instructions

## Scope and precedence

These instructions apply to the entire repository. Read and obey
`CONTRIBUTING.md`, `CODING_STANDARDS.md`, and every deeper `AGENTS.md` that
applies to a changed path. Deeper instructions may add constraints for their
scope; they do not relax repository-wide safety, testing, or ownership rules.

Change only the paths declared in the task's path ownership. Stop and report a
conflict before changing an owned path assigned to another task or preserving
work would require overwriting unrelated changes.

## Native-engine contracts

Use these pinned references when work depends on the native-engine design:

- php-src baseline: `47355da494ba696b1bdb6d10448a225e742bd316`;
- TPDE reference: `338d41890e424b058e2053b6a5787e1348e3dd57`.

Do not introduce a production VM fallback in native-engine code. Do not change
public ABI, persistent formats, or dependencies without an explicit contract,
compatibility analysis, and the tests required by that contract.

The following paths are stable W00 command contracts; their absence before the
owning W00 work is integrated is not permission to invent replacements:

- `scripts/native/configure-dev.sh`
- `scripts/native/build.sh`
- `scripts/native/test-smoke.sh`
- `scripts/native/test-sanitizers.sh`
- `scripts/native/wave-gate.py`

See `docs/native-engine/test-command-contract.md` for ownership and exit
semantics.

## Completion

Run the task-specific checks and the applicable php-src tests before completion.
Run `git diff --check` and inspect `git status --short`. Report every command and
its real result; never hide failures or claim an unavailable check passed. Leave
the worktree clean at completion.
