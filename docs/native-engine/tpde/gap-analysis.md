# TPDE gap analysis

This analysis maps PHP's frozen W01 contracts to the pinned source described in
`source-provenance.md`. `covered` means the cited API and implementation directly
satisfy the named capability. A similar internal mechanism is not coverage.

## Classification summary

| Capability | Classification | Consequence |
|---|---|---|
| IR adaptor, SSA, PHI | `covered` | Implement the PHP adaptor against the existing concepts. |
| Single-entry functions | `covered` | Do not create mid-function entry assumptions. |
| Multipart values | `covered` | Map PHP/ABI parts to the existing value-part contract. |
| Calls and calling conventions | `covered` | Supply and test the PHP helper ABI. |
| Fixed context registers | `covered` | Select only target-permitted fixed assignments. |
| Branches and switches | `covered` | Use the architecture-independent generation helpers. |
| Symbols and live relocations | `covered` | Existing assembler records support live output. |
| ELF object output | `covered` | Object generation is separate from mapping. |
| Custom code allocation | `tpde_extension` | Add a caller-owned allocation/layout boundary. |
| Custom mapping pipeline | `tpde_extension` | Expose image and relocation stages; do not reuse private internals. |
| Persistable relocations | `tpde_extension` | Export address-neutral immutable descriptors. |
| Process-local symbol resolution | `covered` | Resolve again in every process. |
| W^X publication | `tpde_core_patch` | Reject W+X section flags and define checked publication. |
| AArch64 instruction cache | `tpde_core_patch` | Add cache maintenance before publication. |
| AArch64 branch veneers | `tpde_core_patch` | Make reachability/layout guarantees reusable. |
| Unwind emission and live registration | `covered` | Preserve registration/deregistration lifecycle. |
| Statepoint code offset | `tpde_extension` | Add semantic before/after emission hooks. |
| Force materialization | `tpde_extension` | Add explicit value-set materialization. |
| Value-location snapshot | `tpde_extension` | Export immutable normalized part locations. |
| PHP frame/stackmap metadata | `php_shim` | Build the PHP schema outside TPDE. |
| Resume/OSR | `php_shim` | Generate separate single-entry functions. |
| Debug/perf publication | `php_shim` | Translate symbols and ranges into PHP events. |
| Thread safety | `php_shim` | Own one mutable compiler graph per compilation. |
| Reset/re-use | `covered` | Call reset before reuse and regression-test isolation. |
| C++20/no-EH build | `covered` | Keep the pinned build mode. |
| x86-64 target | `covered` | It is the primary PHP release target. |
| AArch64 backend | `covered` | TPDE has it; PHP still labels it unreleased. |

The complete requirements, source anchors, target effects, and tests are in
`required-capabilities.json`.

## Statepoint capabilities are four separate contracts

1. **Code offset** answers where emission is before or after a semantic marker.
   `FunctionWriter::offset()` supplies a number, but TPDE has no callback that
   binds it to an adaptor statepoint or defines before/after semantics.
2. **Value-location export** answers where every part is at that instant.
   `AssignmentPartRef` exposes mutable allocation facts internally, but no
   immutable enumeration or normalized exported vocabulary exists.
3. **Force spill/materialize** changes allocation state so requested values are
   reconstructible. `spill_before_branch(true)` is not this contract: it is tied
   to branch state, skips fixed assignments, and may skip dead values.
4. **Frame/stackmap metadata** combines offsets and locations with PHP VM slots,
   oplines, resume identities, and bailout reasons. Those are PHP concepts and
   belong in a PHP-owned table, not in TPDE core.

None of these four may be marked complete because another one exists. The
generic hooks in `statepoint-and-location-hooks.md` make their ordering explicit.

## One entry does not mean one function

`IRAdaptor::cur_entry_block()` and `cur_blocks()` define one entry block per
function. The adaptor separately exposes a range of functions to compile. TPDE
therefore does not model several arbitrary entry PCs inside one generated
function, and PHP metadata must never jump into its body.

Resume and OSR are still possible without changing this invariant: PHP creates a
separate generated function for each accepted resume/OSR shape. Its ordinary
entry reconstructs the fixed context and frame state, then enters its own CFG.
A PHP-owned dispatcher maps a stable resume ID to that function symbol. This is
more metadata and code than a raw internal label, but it preserves unwind,
prologue, calling-convention, and value-state invariants.

## Mapping is not persistence

`AssemblerElf::build_object_file()` produces ELF bytes. `ElfMapper::map()` is a
specific live mapper: it calculates a layout, allocates one private anonymous RW
mapping, resolves symbols into mapper-local addresses, copies sections, applies
relocations, changes permissions, and registers EH-frame data. Its private
mapping is not an OPcache shared image and its in-memory symbol addresses are not
persistable.

A PHP-local first implementation can use exported generic image descriptors and
PHP-owned memory while preserving the same relocation and W^X behavior. A future
relocatable OPcache image additionally needs stable address-neutral sections,
symbols, and relocations. Each loading process must recreate GOT/PLT or equivalent
slots, resolve helper symbols, apply relocations, perform target cache
maintenance, set final protections, and register unwind data for its live range.

## Target boundaries

The existing mapper has an auditable RW construction-to-final-permission
transition, but its final `mprotect` flags independently mirror `SHF_WRITE` and
`SHF_EXECINSTR`. It does not reject a section containing both. W^X is therefore a
core hardening patch, not a `covered` guarantee. A future PHP mapper must use the
same checked publication rule.

On x86-64, no separate instruction-cache flush is required by this publication
model. In the reviewed AArch64 `ElfMapper::map()` copy/relocate/protect sequence,
no instruction-cache maintenance operation appears. That is a source-review
inference over the complete publish path, recorded as a core patch rather than a
`covered` claim.

The AArch64 mapper has a CALL26-to-PLT fallback, but its second range check uses
32 signed bits rather than CALL26's aligned signed 28-bit byte range. Thus the
current path does not prove that the replacement displacement is encodable. It
also has no public target-layout and veneer contract that a custom mapper can
obey. A persistable or very large image must allocate range-safe veneers
deterministically. Both AArch64 gaps must close before PHP changes its target
status from analyzed/unreleased.

## No hidden blockers

The inventory has no `blocked` capability: each uncovered critical need has a
specific PHP shim or finite TPDE extension/core-patch strategy. This does not
pretend the gaps are implemented. The validator rejects any critical entry
changed to `blocked`, even if a synthetic decision ID is added.
