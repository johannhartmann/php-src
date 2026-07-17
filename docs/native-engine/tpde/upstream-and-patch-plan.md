# TPDE upstream and patch plan

The uncovered work is divided into small changes with independent tests. PHP
may prototype against a pinned patch queue, but an indefinite private fork is
not the default. Generic backend changes should be proposed upstream after the
contract tests demonstrate their shape.

## Patch sequence

### P1: immutable finalized-image inspection

Expose read-only section, symbol, and relocation descriptors after assembler
finalization. Descriptors use stable indices and spans, contain no live pointers,
and retain target relocation types.

- **Unlocks:** custom mapper and persistable relocation consumers.
- **Owner/upstream:** TPDE, good upstream candidate.
- **Tests:** descriptor/object equivalence, lifetime, malformed-index rejection,
  x86-64 and AArch64 relocation enumeration.
- **Fallback decision:** if the exact descriptor ABI is rejected, keep a narrow
  versioned PHP adapter over a pinned TPDE patch while revising the generic API;
  do not make private TPDE members public wholesale.

### P2: caller-owned layout and relocation application

Add target layout constraints and relocation application over caller-supplied
section addresses. Allocation policy stays outside TPDE. Include explicit
PLT/GOT/veneer planning, correct target-width range checks, rejection of W+X
section flags, and checked failure for impossible layouts.

- **Depends on:** P1.
- **Unlocks:** PHP-local mapper and relocatable image loader.
- **Owner/upstream:** shared design, TPDE implementation, good upstream
  candidate.
- **Tests:** compare bytes and symbol addresses with `ElfMapper`; inject allocator,
  resolver, relocation, and protection failures; boundary-test displacement
  ranges.
- **Fallback decision:** use the existing process-local `ElfMapper` only for an
  explicitly non-persistent prototype. It is not a fallback for OPcache-shared
  images.

### P3: generic statepoint emission hooks

Add opaque adaptor statepoint references and before/after `CodePosition`
callbacks around semantic emission. Do not expose PHP types or mutable writer
pointers.

- **Depends on:** no mapper patch.
- **Unlocks:** stable native-offset capture.
- **Owner/upstream:** shared design, good upstream candidate.
- **Tests:** zero-byte markers, calls, fused instructions, sections, and stable
  ordering.
- **Fallback decision:** a derived-compiler PHP hook is acceptable while the
  generic callback is reviewed, provided it obeys the same test contract; raw
  offsets sampled ad hoc are not acceptable.

### P4: explicit materialization and immutable locations

Add requested-value-set materialization plus a copied normalized location
visitor. Include fixed assignments, multipart values, constants, stack-base
semantics, and explicit failure/unavailable results.

- **Depends on:** P3 for end-to-end statepoint records, but can be reviewed as a
  separate allocator API.
- **Unlocks:** bailout-safe value recovery.
- **Owner/upstream:** shared design, good upstream candidate.
- **Tests:** dirty/clean registers, fixed values, calls, spills, constants,
  multipart values, unavailable values, and snapshot lifetime.
- **Fallback decision:** a PHP-derived compiler may translate internal
  assignments temporarily. It must copy descriptors and pass the same negative
  tests; exposing `ValueAssignment *` outside compilation is rejected.

### P5: AArch64 cache publication

Add a target publish primitive used by `ElfMapper` and caller-owned mappers. It
performs the required cache maintenance over written executable ranges before
they become callable; x86-64 documents a no-op.

- **Depends on:** independent for `ElfMapper`; P2 for custom mapping.
- **Unlocks:** AArch64 correctness, but not PHP AArch64 release by itself.
- **Owner/upstream:** TPDE core patch, requires core review.
- **Tests:** generated and modified code on real AArch64 or a cache-faithful
  runner, empty and multiple ranges, ordering with protection changes.
- **Fallback decision:** keep PHP AArch64 unreleased. There is no safe omission
  or production VM fallback.

### P6: AArch64 layout and veneer guarantees

Generalize CALL26 reachability handling so a mapper can plan enough reachable
veneer/PLT regions for its maximum layout, with checked failure when impossible.

- **Depends on:** P2 for the custom-mapper surface.
- **Unlocks:** large/custom AArch64 mappings and persistent image replay.
- **Owner/upstream:** TPDE core patch, requires core review.
- **Tests:** both range boundaries, very large synthetic layouts, multiple veneer
  regions, local and external calls.
- **Fallback decision:** keep PHP AArch64 unreleased or enforce a validated image
  size/layout bound. Never truncate a relocation or assume reachability.

### P7: PHP-owned consumers

Implement the PHP frame/stackmap builder, separate resume/OSR functions,
per-compilation ownership, mapping lifecycle, diagnostics, and per-process image
loader. These use the generic TPDE APIs but are not upstream TPDE features.

- **Depends on:** P1–P4 for primary x86-64 persistence and bailout metadata;
  P5–P6 additionally for AArch64.
- **Owner/upstream:** PHP-local.
- **Tests:** W01-C contract suite, resolver replay at different bases, W^X,
  unwind lifecycle, concurrent independent compilations, perf/symbol lifecycle.
- **Fallback decision:** disable the unsupported native configuration at startup
  or compilation selection. Do not silently transfer a failing production
  execution to the VM.

## Integration and pin policy

Each upstreamable patch should carry its focused TPDE unit tests and a short API
rationale independent of PHP. PHP keeps an explicit TPDE commit plus any
temporary patch IDs. Moving the pin requires rerunning the source-anchor
validator, reviewing changed capabilities, and updating provenance in the same
change. A green build alone does not reclassify a capability.
