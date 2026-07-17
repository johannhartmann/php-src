# Statepoint and value-location hooks

The proposed TPDE surface is generic. It contains no PHP opcode, frame, VM-slot,
or bailout types. An adaptor may use an opaque, cheap statepoint reference and
translate the resulting records into its consumer's metadata.

## Required ordering

For a statepoint that may reconstruct state, compilation follows this order:

```text
identify semantic statepoint and requested live values
        |
        v
materialize requested values or fail compilation
        |
        v
snapshot immutable normalized locations
        |
        v
report before-emission section/offset
        |
        v
emit the statepoint instruction or call
        |
        v
report after-emission section/offset
```

The before and after offsets are both explicit because calls, fused
instructions, and zero-byte semantic markers make a single undocumented offset
ambiguous. Materialization precedes the snapshot; the snapshot never aliases
mutable register-allocation state.

## Minimal generic pseudocode

```cpp
enum class StatepointPhase : uint8_t {
    BeforeEmission,
    AfterEmission,
};

enum class LocationKind : uint8_t {
    Register,
    Stack,
    Constant,
    Unavailable,
};

struct CodePosition {
    SectionRef section;
    uint64_t offset;
};

struct ValuePartLocation {
    uint32_t part;
    uint32_t size;
    RegisterBank bank;
    LocationKind kind;
    RegisterRef reg;        // valid only for Register
    int64_t frame_offset;   // CFA/frame-relative; valid only for Stack
    ConstantRef constant;   // valid only for Constant
};

struct MaterializePolicy {
    bool accept_registers;
    bool require_stack_copy;
    bool include_fixed_assignments;
};

template<class Adaptor>
concept StatepointAdaptor = requires(
    Adaptor &a,
    typename Adaptor::IRStatepointRef point,
    StatepointPhase phase,
    CodePosition position,
    std::span<const ValuePartLocation> locations) {
    { a.statepoint_values(point) } -> RangeOf<typename Adaptor::IRValueRef>;
    { a.statepoint_emitted(point, phase, position, locations) };
};

bool materialize_values(
    std::span<const IRValueRef> values,
    MaterializePolicy policy);

bool snapshot_value_locations(
    std::span<const IRValueRef> values,
    LocationVisitor visitor) const;
```

Names are illustrative, not a frozen C++ ABI. The semantic requirements are:

- a materialization request names an explicit value set;
- inability to materialize returns failure and cannot silently omit a part;
- fixed assignments are included when requested;
- every multipart value yields one descriptor per part in stable part order;
- register identifiers carry a target-neutral bank plus a target register ID;
- stack locations define their base convention, not merely a raw internal
  `frame_off` integer;
- constant and unavailable are explicit rather than inferred sentinels;
- callback data is valid for the callback or copied into caller-owned storage;
- the before/after position refers to the finalized section coordinate system.

## Why existing interfaces are insufficient

`FunctionWriter::offset()` can answer the current offset but has no association
with an IR statepoint and no before/after rule. `ValueAssignment` and
`AssignmentPartRef` contain mutable register and stack facts but are allocation
implementation details. Exporting their pointers would expose unstable lifetime
and target encoding.

`spill(AssignmentPartRef)` handles one part. `spill_before_branch(true)` handles
branch convergence, not a requested safepoint value set; it deliberately skips
fixed assignments and can omit values whose liveness does not cross the branch.
The proposed materialization operation may reuse those internals, but its
observable success rule is stronger.

## PHP-owned consumption

PHP assigns stable statepoint, resume, VM-slot, and bailout identities. It copies
the generic callback records into the versioned frame/stackmap schema frozen by
W01-C and verifies that every required value has an accepted location. TPDE never
needs to know what an opline or Zend value is.

For resume/OSR, PHP metadata names a separately generated function symbol. It
does not convert a reported code offset into a second entry point inside the
original function.
