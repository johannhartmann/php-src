# TPDE in the PHP native engine

This branch replaces the Zend VM with native machine code. PHP functions are
compiled through TPDE, a fast single-pass compiler back-end framework, into
x86-64 code on Linux; Darwin arm64 has a reduced back-end. There is no
production fallback to the Zend VM: a function the engine cannot compile is
rejected, never interpreted (ADR 0001).

This document explains:

1. what TPDE is and how it works internally;
2. how it is integrated into the engine, from a Zend op array to running code;
3. how to build, run, test and tune the engine.

The architecture decisions behind the design live in
[`docs/native-engine/`](docs/native-engine/README.md). The rules for working
on the code live in the `AGENTS.md` files of the repository root and of
`Zend/Native/**`.

---

## 1. TPDE

### 1.1 What it is

[TPDE](https://tpde.org) is a C++20 framework for building compiler back-ends
that generate machine code in a single pass, with very low compile times. It
is the code generator behind fast baseline compilers such as TPDE-LLVM.

The engine vendors it under `Zend/Native/TPDE/ThirdParty/tpde`:

- **Pinned revisions** (`REVISION`):
  - TPDE `tpde2/tpde` at commit `9779acf4ada3`;
  - the Fadec x86-64 encoder at commit `3994d89`;
  - the Disarm AArch64 encoder at commit `2d13d3f`.
- **Local changes:** every one is recorded in `PATCHES.md`.
- **License:** Apache-2.0 WITH LLVM-exception (`LICENSE.txt`).
- **Policy:** the pin follows upstream `master`, and no compatibility with
  older revisions is kept.

TPDE is not an optimizer. It takes an IR that is already in SSA form and
emits code for it in one forward walk over the blocks, doing three things on
the way:

- register allocation with liveness information;
- spilling at block boundaries;
- folding of operands into instruction forms.

Its design shows these traits throughout:

- **It reuses the IR's own storage.** The front-end exposes its IR through an
  adaptor and TPDE never copies it. References to functions, blocks and values
  are at most 8 bytes.
- **Analysis is linear-time.** It consists of the block layout, loop detection
  and liveness.
- **There is no register state per block.** Values are spilled at branches
  instead, which avoids a per-block register map.
- **The hot path has no virtual calls.** The compiler is built from CRTP
  templates and constrained by C++20 concepts.

### 1.2 The IR adaptor

A front-end connects to TPDE by implementing the `IRAdaptor` concept
(`include/tpde/IRAdaptor.hpp`). The adaptor provides the following:

| Area | What the adaptor provides |
|---|---|
| Reference types | `IRValueRef`, `IRBlockRef`, `IRFuncRef`, `IRInstRef` and their `INVALID_*` constants |
| Module | `func_count`, `funcs`, `funcs_to_compile`, `func_link_name`, `func_extern`, `func_only_local`, `func_has_weak_linkage` |
| Current function | `switch_func`, `cur_args`, `cur_entry_block`, `cur_blocks`, `cur_static_allocas`, `cur_has_dynamic_alloca`, `cur_is_vararg`, `cur_needs_unwind_info`, argument attributes (`byval`, `sret`) |
| Blocks | `block_succs`, `block_insts`, `block_phis`, and two `u32` slots the compiler stores per block (`block_info`, `block_info2`) |
| Values | `val_local_idx`, a dense index per function that keys every array the compiler keeps; `val_is_phi`, `val_as_phi`, `val_ignore_in_liveness_analysis`, alloca size and alignment |
| PHIs | `incoming_count`, `incoming_val_for_slot`, `incoming_block_for_slot`, `incoming_val_for_block` |
| Instructions | `inst_operands`, `inst_results`, `inst_fused` (the instruction was folded into another) |
| Lifecycle | `start_compile`, `end_compile`, `reset` |

The engine also uses `block_is_cold(IRBlockRef)`, an optional hook added by a
local patch, which sends whole blocks to the cold area (see 1.5).

How a value is split into machine-sized parts is decided by the compiler, not
by the adaptor. `val_parts(v)` returns the number of parts, each part's size
and its register bank.

### 1.3 The compiler pipeline

`tpde::CompilerBase<Adaptor, Derived, Config>` (`include/tpde/CompilerBase.hpp`)
drives compilation. Below it sits a platform layer,
`tpde::x64::CompilerX64` or the A64 equivalent. The front-end supplies the
most derived class and implements `compile_inst`.

**`compile()`.** It creates a symbol per function and then calls
`compile_func` for every function that is not extern. At the end it finalizes
the assembler.

**Analysis** (`include/tpde/Analyzer.hpp`). This runs per function:

1. **Block layout.** It computes a reverse post-order and identifies loops
   with the algorithm of Wei et al. Each loop is then laid out as one
   contiguous range of blocks.
2. **Liveness.** It records, per value, the first and last block where the
   value is live, a reference count and the innermost common loop. Live
   ranges are extended to cover whole loops.

**Code generation** (`compile_func` and `compile_block`):

1. The prologue is emitted and the arguments are given their initial
   locations.
2. Each block, in layout order, places its label and calls
   `derived()->compile_inst(inst, remaining)` for every instruction. Because
   the remaining instructions of the block are passed along, a compiler can
   fuse an instruction with the ones that follow it.
3. At the end of a block, values whose last use was in that block are freed.

**Value model.** Every live value has a `ValueAssignment` with one part per
machine word. A part is in one of four states:

- uninitialized;
- in a register that differs from its stack slot;
- in its stack slot only;
- in a register that equals its stack slot.

The front-end works with the following handles:

- **`ValueRef` and `ValuePartRef`** refer to a value or one of its parts.
  They provide `load_to_reg`, `alloc_reg`, `into_scratch`, `set_value`,
  `lock` and similar operations.
- **`ScratchReg`** is an owned temporary register that cannot be spilled and
  is released automatically.
- **`GenericValuePart`** is a register, a value part, or an address
  expression (base + index * scale + displacement), so addresses can be
  folded into instructions.

The register file keeps three bitsets: allocatable, used and clobbered
registers. The clobbered set later decides which callee-saved registers the
prologue saves. When a register is needed, the eviction order is:

1. a stack variable reference;
2. another variable reference;
3. a value that is already spilled;
4. the value whose last use is farthest away.

Values that stay live across blocks, inside a loop, may get a **fixed
register** (on x64 at most 5 general-purpose and 6 FP registers). Fixed
registers are never spilled at branches.

**Branches.** A branch follows a fixed protocol:

1. `spill_before_branch()` writes every dirty register that is not fixed back
   to its stack slot. Values whose liveness ends at the branch are skipped.
2. `begin_branch_region()` starts the branch region.
3. `generate_branch_to_block()` is called once per edge. It moves values into
   the PHI slots of the target block and splits conditional edges into
   blocks that have PHIs.
4. `end_branch_region()` ends the region.
5. `release_spilled_regs()` releases the registers that were spilled.

**Prologue and epilogue (x64).** The prologue cannot be written first,
because the frame size and the set of clobbered callee-saved registers are
only known at the end of the function. TPDE therefore reserves space for it
up front. In `finish_func` it:

1. writes the epilogue and appends the cold area;
2. relaxes the jumps;
3. fills in `push rbp; mov rbp, rsp; push <callee-saved>; sub rsp, N`,
   together with the DWARF CFI;
4. removes the bytes of the reserved space it did not need.

### 1.4 x86-64 platform layer

- **Encoding.** Instructions are encoded by Fadec. In compiler code,
  `ASM(op, ...)` expands to the matching Fadec function (`fe64_<op>`). The
  encoder tables are regenerated from the vendored table and generator
  during configure.
- **Calls.** `CCAssignerSysV` implements the System V ABI.
  `CallBuilder::add_arg`, `call` and `add_ret` place the arguments, evict
  caller-saved registers and collect the results.
- **Labels and jumps.** `label_create`, `label_place`, `generate_raw_jump`,
  jump tables and `relax_jumps`. With the local patch, every jump starts as
  rel32 and is shortened to rel8 later.
- **Output.**
  - `AssemblerElf::build_object_file()` produces a relocatable ELF object
    with an `.eh_frame` section.
  - Upstream also ships `ElfMapper` for mapping code in memory. The engine
    does not use it; it maps images itself (see 2.6).

### 1.5 Local patches

`ThirdParty/tpde/PATCHES.md` is authoritative. In short:

1. **Assembler access.** The assembler exposes its finalized sections and
   relocations to the Darwin in-memory mapper.
2. **Fadec UB fix.** The encoder widens shift operands, fixing undefined
   behaviour that UBSan reports.
3. **Scratch value parts.** A part with no value assigned yet can be created,
   for call results and snippet outputs.
4. **Jump threading.** A jump to a block that only forwards goes straight to
   the final target, and a jump to the next instruction becomes nothing.
5. **Allocator and control flow.** Fixes for:
   - conditional branches whose two targets are the same;
   - switch PHIs;
   - multi-part fixed registers;
   - fast and slow paths inside one IR instruction (`ret_local_path`,
     spilling at branches of values that are still referenced).
6. **Cold area.** Each function gets a side buffer (`begin_cold_area` /
   `end_cold_area`) that is appended after the epilogue, with its own CFI
   state. Rarely executed code goes there, out of the hot instruction
   stream.
7. **Code before the prologue.** `remove_prologue_bytes` and
   `begin_func_after_prefix` let a function have a second, frameless entry
   placed before its body. The engine uses this for its native call entry.
8. **Short jumps.** Jumps start as rel32. `relax_jumps` shortens them to rel8
   until nothing changes, removes jumps to the next instruction and turns
   `jcc A; jmp B; A:` into an inverted `jcc B`.
9. **Biased base register.** `set_biased_reg` rewrites the displacements of
   one register. The engine keeps the frame pointer of the Zend frame in RBX,
   biased by 0x80, so that most frame-slot offsets fit in a one-byte
   displacement. RBX is therefore not allocatable.

### 1.6 EncodeGen snippets

Small operations are written once in C and turned into TPDE encoder methods
by upstream's `tpde_encodegen`. It compiles C via LLVM to machine IR and
emits a C++ method `encode_<name>(operands..., result)` for each function.
The generated method folds operands into immediates and memory forms where
it can.

The snippet sources live in `Zend/Native/TPDE/EncodeGen/`:

- `zend_tpde_encodegen.c`: scalar arithmetic and comparisons.
- `zend_tpde_encodegen_values.c`: PHP value operations for x86-64, such as
  zval copies, property slot access and array probes by key.
- **Snippet rules:** a snippet has no calls, no globals and no frame. A probe
  changes nothing before it succeeds, so on a miss the runtime helper can
  redo the whole operation.
- `tpde-encodegen-cold-blocks.diff`: adds `--cold-blocks`. Blocks inside a
  snippet with an estimated frequency below 1%, such as `UNEXPECTED()`
  branches, are emitted into the cold area.
- `regenerate.sh`: rebuilds `LinuxX64/zend_tpde_encodegen_x64.hpp` and
  `DarwinA64/zend_tpde_encodegen_a64.hpp`. It is a developer tool that needs
  LLVM/clang 21. Normal builds use the checked-in headers and need no LLVM.

---

## 2. Integration into the PHP engine

### 2.1 Overview

```
PHP source
  -> Zend compiler: op_array            (Zend/zend_compile.c)
  -> OPcache optimizer: SSA and type inference
  -> ZNMIR lowering                     (Zend/Native/Lowering, Zend/Native/MIR)
  -> TPDE plan and machine plan         (Zend/Native/TPDE/Common/zend_tpde_plan*.cpp)
  -> IR adaptor -> TPDE code generation (zend_tpde_ir_adaptor.*, LinuxX64/zend_tpde_linux_x64.cpp)
  -> ELF image -> mapped code           (zend_tpde_image.cpp, Runtime/LinuxX64/zend_native_publish_linux_x64.cpp)
  -> entry cell publishes the code -> zend_native_execute_frame_entered()
```

The source tree:

| Directory | Contents |
|---|---|
| `Zend/Native/Compiler/` | Compiler driver (`zend_native_compiler.c`), executor hook (`zend_native_executor.c`), dynamic code |
| `Zend/Native/MIR/`, `Zend/Native/Lowering/` | ZNMIR: the canonical mid-level IR, its verifier and text format, and the lowering from Zend opcodes |
| `Zend/Native/TPDE/Common/` | Planning, IR adaptor, shared code generator, image format, layout profile |
| `Zend/Native/TPDE/LinuxX64/` | `ZendCompilerX64`, the x86-64 code generator, and the generated snippet encoders |
| `Zend/Native/TPDE/DarwinA64/` | Darwin arm64 code generator and PHP's own A64 assembler |
| `Zend/Native/Runtime/` | Runtime helpers called by generated code, frame execution, and code publication per platform |
| `ext/native_mir_test/` | Build switch (`config.m4`), test bridge functions and the native PHPT suite |

### 2.2 When code is compiled

**Executor hook.** `zend_post_startup()` calls
`zend_native_executor_startup()` (`Zend/Native/Compiler/zend_native_executor.c`).
That function:

- validates the runtime helper table;
- reserves an op_array and extension handle;
- installs `zend_native_executor_execute_ex` as `zend_execute_ex`.

The executor also activates and deactivates per request, from `Zend/zend.c`.

**First execution.** `zend_native_executor_execute_ex` resolves which code
should run a frame:

- A thread-local reentry cache serves entries from C, such as callbacks and
  magic methods.
- Otherwise it finds the **generation** that owns the function's script. A
  generation is a compiler bound to one script owner: a persistent one when
  the script is in OPcache, a request-local one when it is not.
- `zend_native_compiler_prepare_function()` then compiles the function on its
  first execution and runs the published code.

**Unit of compilation: the component.** `zend_native_compiler_compile_pending()`
lowers the root function and discovers the user functions it calls whose
targets are resolved statically. All of them that still need compiling form
one **component**, and one TPDE image is produced per component. Because the
image is shared, calls within the component are direct and the whole
component is published at once.

**OPcache.** When OPcache caches a script, `zend_accel_prepare_native_script()`
(`ext/opcache/ZendAccelerator.c`) compiles the script's main code, its
functions, methods and closures ahead of time and serializes them into a
**bundle**. The bundle is stored with the script in shared memory or the file
cache. Each worker process later imports the bundle and maps it locally. A
content-keyed cache in the process reuses the bundle of a script whose content
is identical.

### 2.3 Immutable code and entry cells (ADR 0005)

Each compiled function has a `zend_native_entry_cell`
(`Zend/Native/Runtime/Common/zend_native_calls.h`).

- **Contents:** its state (uncompiled, compiling, ready or failed), the
  published code pointer, counts of active and suspended frames, publication
  epochs and the facts the fast call paths need.
- **Publication** stores the code with release ordering, and callers load it
  with acquire ordering.
- **Immutability:** published code is never modified in place. A new version
  replaces the old one, and the old one is retired only once no frame uses it
  any more.

### 2.4 The pipeline in detail

1. **Type facts.** `zend_native_compiler_build_ssa()` runs OPcache's
   data-flow analysis with type inference. Its facts (what an operand may
   hold) drive specialization throughout.
2. **ZNMIR.** `zend_mir_lower_typed_zend_op_array()` (typed tier, Linux x64)
   or `zend_mir_lower_zend_op_array()` translates opcodes into ZNMIR. Opcodes
   are lowered by registered providers, and an opcode with no lowering
   rejects the function with a diagnostic. Exception routes and source
   effects are attached at this stage.
3. **Plan.** `zend_tpde_compile_component()` (`zend_tpde_backend.cpp`) builds
   two plans:
   - per function, `initialize_plan()` freezes the control-flow graph, the
     values, calls, value operations, argument and return ABI, storage
     aliases and the runtime helpers the function needs;
   - per component, `freeze_component_machine_plan()` decides machine value
     representations, typed bodies, which operations may fail (guarded) and
     the deoptimization exits.
4. **IR adaptor.** `ZendIRAdaptor` (`zend_tpde_ir_adaptor.*`) presents the
   plan to TPDE. Its instruction nodes (`InstKind`) include:
   - frame loads and boxing or unboxing of scalars;
   - the call phases `UserCallInit`, `UserCallSend`, `UserCallCheck`,
     `UserCallExpand` and `UserCallDo`;
   - generator resume points;
   - **`GuardedFast` / `GuardedCold` pairs**: the inline fast path of an
     operation, and the out-of-line path that calls a helper when a guard
     fails.

   Nodes also carry deoptimization and frame-state metadata.
5. **Code generation.** `ZendCompilerCommon` (`zend_tpde_compiler_common.hpp`)
   sits between TPDE's `CompilerBase` and the architecture compiler.
   `ZendCompilerX64` (`LinuxX64/zend_tpde_linux_x64.cpp`) derives from
   `tpde::x64::CompilerX64` and from the generated snippet encoder.
   `compile_inst` dispatches on the node kind. Rarely executed paths go to
   the cold area. A layout profile can move further blocks there (see 2.8).
6. **Image.** The emitter produces an ELF object (`build_object_file()`) and
   appends it to the component image (`zend_tpde_image.cpp`).
   - **Symbols** name runtime helpers, entry cells, internal-call cells and
     call descriptors. They are resolved when the image is mapped.
   - **Persisted images** contain no process addresses. Generated code reaches
     process state through the `zend_native_execution_context` argument
     (VM stack, current frame, exception, interrupt flag, call-cache epoch).
7. **Mapping.** `zend_native_publish_linux_x64.cpp` parses the ELF,
   applies the relocations (`R_X86_64_64`, `PC32`, `PLT32`, `GOTPCREL`),
   sets the page protections and registers the `.eh_frame` for unwinding.
   Images are placed within 1 GiB below the executable, so calls to runtime
   helpers are direct `rel32` calls without a PLT.
8. **Execution.** `zend_native_execute_frame_entered()`
   (`Runtime/Common/zend_native_execute.c`) calls the entry as
   `entry(execute_data, context)` and handles the return, interrupts,
   bailouts and frame cleanup.

### 2.5 Execution model (ADR 0025)

- **Native state is canonical.** SSA values live in registers and native
  stack slots. Only some state is written to the Zend frame eagerly: the
  frame header, the arguments and `EX(opline)` before any transition into C.
  Other CVs and temporaries are written to their Zend slots only at points
  where PHP state can be observed. Those points are warnings, exceptions,
  observers, backtraces, reentry, generators and fibers, plus helpers whose
  effects read the slot.
- **No VM.** Zend opcode handlers, the Zend JIT IR and a second register
  allocator are all forbidden. Runtime helpers take values and addresses,
  never encoded opcode operands.
- **Speculation and deoptimization.** These are opt-in
  (`ZEND_NATIVE_SPECULATE=1`, Linux x64 only).
  - The specialized code relies on guards.
  - A failing guard transfers into a generic native copy of the same
    function, through `zend_native_deopt_transfer` and landing stubs.
  - It never transfers into the VM.
- **Correctness bar.** Every optimization must keep PHP-observable behaviour
  identical. This is proven with PHPTs against a stock PHP oracle and with
  application page checks.

### 2.6 Runtime helpers and the ABI

Generated code calls C helpers for everything that is not inlined. These
cover the slow paths of value operations, arrays, objects, calls and
generators. The helpers live in `Zend/Native/Runtime/Common/`
(`zend_native_values.c`, `zend_native_objects.c`, `zend_native_calls.c`,
`zend_native_internal_call.c`, `zend_native_generators.c`, ...).

- **Helper table.** Each helper has a fixed id in
  `zend_native_runtime_helper_id` and an entry with an effects mask
  (allocate, throw, bailout, reenter, frame read or write) in
  `zend_native_runtime_helpers[]`. The plan records which helpers a function
  needs. The emitter references each one as an image symbol, and the
  publisher resolves it.
- **ABI version.** `ZEND_NATIVE_RUNTIME_ABI_VERSION`
  (`zend_native_runtime.h`) is part of the OPcache system id, the bundle
  header and the image build id, so an incompatible image is never loaded.
  Bump it whenever emitted code or a structure shared with generated code
  changes.

### 2.7 Calls

There are four kinds of call path:

- **Fast call sites.** Every call site has a descriptor, and a header local
  to the process sits in front of it (`zend_native_user_call_site_header`).
  The header publishes the site's resolved target: function, receiver class
  key, entry, frame size and flags. While the header is valid for the
  current call-cache epoch, the generated code does the following without
  any C resolution:
  - pushes the Zend frame inline;
  - stores the arguments;
  - calls the target's entry.
- **Universal protocol.** A site with no valid target takes the cold
  universal protocol (`zend_native_call_universal_init`, `_send`, `_expand`,
  `_do`). It resolves the target as the VM would and publishes the site.
  - Sites go stale at the end of a request. They re-arm once per request
    through a cold stub (`zend_native_call_fast_rearm`) when their target
    still applies.
  - Self-forwarding sites (`self::`, `parent::`) inside trait methods and
    closures compare the caller's scope with the site's key.
- **Dynamic calls.** `$f(...)`, `call_user_func()`, `$obj->$m(...)` and
  `$class::m(...)` look up a table of recorded targets
  (`zend_native_call_recorded_target`). Targets are keyed by a stable
  identity: an interned name, a closure's code, or a class plus method name
  plus calling scope.
- **Direct native calls.** A call within a component whose target is known
  can use a native calling convention.
  - **Typed bodies** pass scalars in registers.
  - **Frameless native call entries** are placed before the function body.

### 2.8 Code layout, tiers and stress modes

- **Cold area.** Slow paths, re-arm stubs, failure exits and snippet blocks
  that rarely run are emitted after the epilogue.
- **Layout profile** (`zend_tpde_layout.*`). Jumps that training finds
  taken at least 9 times in 10, over at least 256 runs, are inverted or
  moved so that the hot path falls through. FPM trains this automatically:
  - it counts in shared memory;
  - after `ZEND_NATIVE_LAYOUT_REQUESTS` requests it restarts OPcache so that
    scripts are recompiled with the profile.
- **Rare functions.** A function called at most `ZEND_NATIVE_RARE_CALLS`
  times per request (default 12) gets no native call entry, which keeps it
  small.
- **Tier 2.** Off unless `ZEND_NATIVE_TIER2_THRESHOLD > 0`. It recompiles
  hot functions with argument-type guards, and the result stays local to the
  process.
- **Deoptimization stress** (debug builds, `ZEND_NATIVE_DEOPT_STRESS=N`).
  Every N-th deoptimization point moves its frame to a new activation. This
  is used by the full test tier.

### 2.9 Platforms

| | Linux x86-64 | Darwin arm64 |
|---|---|---|
| Code generator | `ZendCompilerX64` on TPDE x64 and Fadec | `zend_tpde_darwin_arm64.cpp` with a PHP-owned A64 assembler (upstream TPDE only emits ELF for A64) |
| Mapping | ELF relocation, placement near the executable | `MAP_JIT`, `pthread_jit_write_protect_np` |
| Typed lowering, speculation, entry variants, layout training | yes | no |

Each host builds only its own back-end. On Linux,
`scripts/native/check-darwin-backend.sh` syntax-checks the Darwin back-end so
that shared changes keep compiling there.

---

## 3. Building

### 3.1 Prerequisites

- Linux x86-64 or Darwin arm64. Other hosts fail at configure (ADR 0006).
- **clang/clang++.** The linux-amd64 profiles set `PROFILE_CC=clang`. GCC
  cannot compile the TPDE x64 templates, so do not override `CC` with it.
- **Build tools:** C++20, python3 >= 3.9 (it generates the Fadec tables),
  autoconf, bison, re2c, make, pkg-config, git and flock.
- **Optional, for release builds:** `xz` (for PGO profiles) and binutils
  >= 2.43 (for `--section-ordering-file`). Without them the build continues,
  just without those features.
- **Configure switch:** `--enable-native-engine`, defined in
  `ext/native_mir_test/config.m4`. It cannot be built as a shared extension.
  - `--enable-native-mir-test` additionally builds the test bridge.
  - OPcache is always built in.

### 3.2 Profiles

The scripts in `scripts/native/` build named profiles
(`scripts/native/profiles/*.env`). Always pass a `*-native-*` profile: the
default, `debug-nts`, is stock PHP without the native engine.

| Profile | Purpose |
|---|---|
| `linux-amd64-native-debug-nts` (`-zts`) | Debug build at `-Og`, with FPM and the test bridge; the tier and test profile |
| `linux-amd64-native-asan-nts`, `-ubsan-nts` | Sanitizer builds at `-O1` |
| `linux-amd64-native-release-nts` | Release build with function order and PGO |
| `linux-amd64-native-product-release-nts` | Release build with FPM, CGI, phpdbg and embed |
| `linux-amd64-native-app-debug-nts`, `-app-release-nts`, `-app-asan-nts` | Builds with the extensions applications need (mbstring, session, PDO/SQLite, mysqli, DOM, zlib, ...), used for WordPress, Symfony and Laravel |
| `darwin-arm64-native-*` | The Darwin counterparts |

**Variables.**

- `NATIVE_WORK_ROOT` is the absolute root for build trees; the default is
  `${TMPDIR:-/tmp}/php-native`. Each profile builds into
  `$NATIVE_WORK_ROOT/<worktree>-<hash>/<profile>/build`, with logs and
  manifests next to it.
- `NATIVE_PHPT_RUNNER` is the PHP binary that runs `run-tests.php`.
- `NATIVE_SANITIZER_CPPFLAGS` is for hosts whose clang lacks the sanitizer
  headers.
- `NATIVE_JOBS` sets the default parallelism.
- `NATIVE_PGO=off` disables PGO.
- `CC`, `CFLAGS`, `CPPFLAGS` and `LDFLAGS` are honoured.

### 3.3 Commands

```bash
# configure once (slow step); reuse the configured profile afterwards
scripts/native/configure-dev.sh --profile linux-amd64-native-debug-nts

# build incrementally, on all cores by default
scripts/native/build.sh --profile linux-amd64-native-debug-nts [--print-binary]
```

`build.sh` reconfigures only when build-system inputs change:
`configure.ac`, `*.m4`, `config*.m4`, `Makefile.frag*`, the profile, the
toolchain or the flags. Source edits rebuild incrementally in seconds. Use
`--force-configure` only when you need it, and never build with `--jobs 1`.

---

## 4. Running

The native executor replaces the VM in every binary built with
`--enable-native-engine`; no INI setting turns it on. Enable OPcache so that
scripts are compiled once, stored in OPcache, and their images shared across
requests and workers:

```bash
php -d opcache.enable=1 -d opcache.enable_cli=1 \
    -d opcache.file_update_protection=0 script.php
```

`opcache.file_update_protection=0` matters for freshly written scripts:
OPcache does not cache a file modified in the last two seconds, so without it
new test scripts run without their optimized, cached form.

FPM works the same way, for example:

```bash
php-fpm -n -y pool.conf -F -O -d opcache.enable=1 \
    -d opcache.validate_timestamps=0 -d opcache.file_update_protection=0
```

FPM trains the branch layout automatically (see 2.8).

The environment variables below are read at startup, before FPM clears the
environment:

| Variable | Effect |
|---|---|
| `ZEND_NATIVE_LAYOUT=path\|off` | Use `path` as the layout profile if it exists, else train into it; `off` disables FPM's automatic training |
| `ZEND_NATIVE_LAYOUT_PROFILE=path` | Apply a layout profile; `*` sends every eligible region to the cold area (a stress mode) |
| `ZEND_NATIVE_LAYOUT_TRAIN=path\|off` | Count jumps in shared memory and write a training file |
| `ZEND_NATIVE_LAYOUT_REQUESTS=N` | Requests before switching to the trained profile (default 128, 0 = never) |
| `ZEND_NATIVE_RARE_CALLS=N` | Calls per request below which a function counts as rare (default 12, 0 = off) |
| `ZEND_NATIVE_SPECULATE=1` | Enable speculative specialization (Linux x64) |
| `ZEND_NATIVE_TIER2_THRESHOLD=N`, `ZEND_NATIVE_TIER2_TRACE` | Enable tier-2 recompilation after N calls; trace its outcomes |
| `ZEND_NATIVE_DEOPT_STRESS=N` | Debug builds: stress every N-th deoptimization point |
| `ZEND_NATIVE_COMPILE_TRACE=path` | Append a compile trace (do not measure timing with it on) |
| `ZEND_NATIVE_PERF_MAP` | Write perf JIT maps (`/tmp/perf-<pid>.map`) so `perf` can name generated code |

---

## 5. Testing

The native CLI cannot run `run-tests.php` itself. Tests use a separate,
plain PHP as the runner, with no ini loaded, and the native binary as the
binary under test.

```bash
# tiers: quick after every change, commit before a commit, full before a push
NATIVE_PHPT_RUNNER=/path/to/plain/php \
  scripts/native/test-phpt.sh --tier quick|commit|full \
  [--baseline FILE] [--show-slow MS] [--deopt-stress N] [PATH...]
```

- **quick:** the native PHPT suite `ext/native_mir_test/tests`, plus any
  paths you pass.
- **commit:** quick, plus `Zend/tests`, `ext/opcache/tests`, `ext/spl/tests`,
  `ext/reflection/tests` and parts of `ext/standard/tests`.
- **full:** commit, then commit again under deoptimization and layout
  stress, then the ASan and UBSan builds. The sanitizer builds are built in
  parallel and run one after the other.
- **On Linux,** every tier also syntax-checks the Darwin back-end.
- **Exit codes** (`docs/native-engine/test-command-contract.md`): 0 pass,
  1 test failures, 2 usage error, 3 missing prerequisite.

**Manual runs:**

```bash
env -u PHP_INI_SCAN_DIR -u PHPRC \
  TEST_PHP_EXECUTABLE=<build>/sapi/cli/php TEST_PHP_SRCDIR=$PWD \
  <runner php> -n run-tests.php -j$(nproc) <paths>
```

A distribution PHP wrapper that injects its own extensions makes every test
fail; use a plain PHP as the runner.

**Other entry points:**

- `scripts/native/test-smoke.sh` checks the version, the modules and three
  PHPTs.
- `scripts/native/test-sanitizers.sh --profile linux-amd64-native-asan-nts`
  runs a sanitizer profile; without `--profile` it runs the stock profiles.
- `test-fpm-runtime.sh` and `test-native-product-fpm.sh` test FPM.
- FPM PHPTs live in `sapi/fpm/tests`.

**Rules for new optimizations:**

- Prove the optimization with PHPTs whose expected output comes from stock
  PHP.
- For anything that spans requests, add an FPM PHPT.
- Check application pages (WordPress, Symfony, Laravel) against their known
  output.
- Under the sanitizers no failure is accepted.

---

## 6. Release tuning: PGO and function order

Release profiles optimize the C runtime with two artifacts in
`scripts/native/layout/`; its `README.md` has the full recipes:

- **`linux-amd64-apps.order`** lists the runtime functions that are hot in
  warm WordPress, Symfony and Laravel requests. The linker places them first
  (`--section-ordering-file`). Regenerate it from callgrind profiles with
  `function-order.py`.
- **`linux-amd64-apps.profdata.xz`** is the clang PGO profile. To regenerate
  it:
  1. Build with `NATIVE_PGO=off` and `-fprofile-instr-generate`.
  2. Run each application's FPM pool with
     `LLVM_PROFILE_FILE=<dir>/%4m.profraw`. Stop the workers with `SIGQUIT`
     before the master, otherwise they do not write their profiles.
  3. Merge with an `llvm-profdata` of the same LLVM major version as clang,
     keeping the full (non-sparse) profile, and compress with `xz -9`.

  Regenerate it after larger changes to the C runtime.

The runtime layout profile for generated code (2.8) is separate from both:
FPM trains it while it runs.

---

## 7. Where to start reading

| Topic | File |
|---|---|
| Execution model | `docs/native-engine/adr/0025-native-canonical-execution.md` |
| Executor hook and compile triggers | `Zend/Native/Compiler/zend_native_executor.c`, `zend_native_compiler.c` |
| Component compilation entry | `Zend/Native/TPDE/Common/zend_tpde_backend.cpp` (`zend_tpde_compile_component`) |
| Plan | `Zend/Native/TPDE/Common/zend_tpde_plan.cpp`, `zend_tpde_machine_plan.cpp` |
| TPDE adaptor | `Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp` |
| x86-64 code generator | `Zend/Native/TPDE/LinuxX64/zend_tpde_linux_x64.cpp` (`ZendCompilerX64::compile_inst`) |
| Calls | `Zend/Native/Runtime/Common/zend_native_calls.c` and `zend_native_calls.h` |
| Image and publication | `Zend/Native/TPDE/Common/zend_tpde_image.cpp`, `Zend/Native/Runtime/LinuxX64/zend_native_publish_linux_x64.cpp` |
| TPDE itself | `Zend/Native/TPDE/ThirdParty/tpde/include/tpde/CompilerBase.hpp`, `PATCHES.md` |
