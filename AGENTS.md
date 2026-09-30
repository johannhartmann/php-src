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
- TPDE: current upstream `master`, pinned in
  `Zend/Native/TPDE/ThirdParty/tpde/REVISION` (local changes in `PATCHES.md`);
  no compatibility with older TPDE revisions. The capability analysis in
  `docs/native-engine/tpde` reviewed `338d41890e424b058e2053b6a5787e1348e3dd57`.

Do not introduce a production VM fallback in native-engine code. Do not change
public ABI, persistent formats, or dependencies without an explicit contract,
compatibility analysis, and the tests required by that contract.

## Execution-model direction

The goal is maximum execution performance for real PHP applications such as
WordPress, measured on warm requests. ADR
`docs/native-engine/adr/0025-native-canonical-execution.md` defines the
execution model and supersedes the "no deoptimization / no speculation /
no separate call ABI" rules of ADR 0024 and of earlier plans:

- Native state (registers, native stack) is canonical. The Zend frame is
  reconstructed on demand from compiler frame-state metadata wherever PHP
  state is observable (exceptions, warnings, observers, backtraces, reentry,
  generators, fibers); it is not kept in sync on every operation.
- Native-to-native PHP calls may use their own native calling convention;
  Zend frames exist only where the frame contract requires them.
- Speculative specialization from type feedback is allowed. A failed guard
  deoptimizes into the generic native version of the same function, never
  into the VM.
- Inlining across functions is allowed with class and target guards.
- Runtime helpers take values and addresses (semantic primitives), not
  encoded opcode operands.

Still forbidden: a production VM fallback, reuse of Zend VM opcode handlers,
the Zend JIT IR, and a second register allocator. Every optimization must keep
PHP-observable behaviour identical at every observation point; prove it with
PHPTs against a stock PHP oracle and the WordPress page checks.

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
  debug suites) before a commit, `--tier full` (commit plus ASan and UBSan,
  built in parallel and run in turn) before a push. It builds incrementally, reports tests slower
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
