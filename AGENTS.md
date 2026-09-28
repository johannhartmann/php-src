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

## Building and testing efficiently

- Build in parallel. `build.sh` defaults to all cores; do not pass `--jobs 1`
  or `make -j1`. A clean native build takes seconds on a large host, and
  configure is the slow step, so reuse a configured profile instead of
  passing `--force`. `build.sh` reconfigures only when build-system inputs
  (`configure.ac`, `*.m4`, `config*.m4`, `Makefile.frag*`), the profile or
  the toolchain change; source edits rebuild incrementally.
- Test in tiers with `scripts/native/test-phpt.sh`: `--tier quick` (native
  PHPTs plus given paths, seconds) after every change, `--tier commit` (full
  debug suites) before a commit, `--tier full` (commit plus ASan and UBSan
  in parallel) before a push. It builds incrementally, reports tests slower
  than `--show-slow` ms, and lists failures missing from a `--baseline`.
- The linux debug profiles compile at `-Og` and the sanitizer profiles at
  `-O1` (`PROFILE_EXTRA_CFLAGS`); assertions and MIR verification stay on.
  Hosts whose clang lacks the sanitizer headers set
  `NATIVE_SANITIZER_CPPFLAGS`.
- The linux-amd64 profiles build with clang (`PROFILE_CC`). GCC cannot
  compile the TPDE x64 templates; do not override `CC` with it.
- The native CLI cannot yet run `run-tests.php` itself. Run PHPTs with a
  separate plain PHP as the runner, with no ini loaded, and the native binary
  under test:
  `env -u PHP_INI_SCAN_DIR -u PHPRC TEST_PHP_EXECUTABLE=<native php>
  TEST_PHP_SRCDIR=$PWD <runner php> -n run-tests.php -j$(nproc) <paths>`.
  A distribution PHP wrapper that injects its own extensions makes every
  test fail. `scripts/native/test-smoke.sh` takes the same runner through
  `NATIVE_PHPT_RUNNER`.
- Compare against a baseline before claiming a fix: record the failing PHPT
  set before and after a change and report any test that newly fails.
- To triage many compile failures at once, run the failing PHPTs under gdb in
  parallel, stop where `compile_inst_impl` returns false, and tally the node
  kind and MIR opcode. One missing lowering is often behind many failures.
- Rebase onto upstream with `git rebase --rebase-merges`. Do not hand-merge
  `Zend/zend_vm_execute.h`; regenerate it with `php Zend/zend_vm_gen.php`.

## Completion

Run the task-specific checks and the applicable php-src tests before completion.
Run `git diff --check` and inspect `git status --short`. Report every command and
its real result; never hide failures or claim an unavailable check passed. Leave
the worktree clean at completion.

## Vulnerability reports

When scanning php-src for vulnerabilities, please respect our security policy
summarized in `./SECURITY.md`. You can find the full policy
[here](https://raw.githubusercontent.com/php/policies/refs/heads/main/security-classification.rst).
