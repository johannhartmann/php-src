# W05 direct-user-call modeling contract

W05 accepts only a complete source-backed call sequence. The source view,
process-local resolver, immutable plan, MIR call tables, and named verifier are
defined by:

- `Zend/Native/Calls/Contracts/zend_mir_call_source.h`;
- `Zend/Native/Calls/Contracts/zend_mir_call_plan.h`;
- `Zend/Native/MIR/zend_mir_call.h`;
- `Zend/Native/Lowering/zend_mir_lowering_zend.h`.

## Semantic order and identity

Call sites enumerate by ascending INIT opline. Parent call-site IDs encode
nesting. Targets enumerate by stable source target ID. Arguments enumerate by
`(call_site_id, ordinal)`; ordinal is zero-based and contiguous. INIT, SEND,
DO, block, target, symbol, op-array, SSA, frame, and MIR identities are stable
integer IDs, never process addresses.

Each immutable MIR call site carries `result_id`. It is invalid exactly when
the source call result is unused; otherwise it is the exact MIR value mapped
from the source result SSA and its summary must be one non-refcounted scalar
type. The call instruction defines that same value. Calls and surrounding MIR
instructions preserve source-opline order: a call is placed at its DO opline,
after every earlier source instruction and before every later one in its
source block.

The target resolver is process-local. It may inspect a `zend_function *`, but
returns only a pointer-free target snapshot. That snapshot must prove a direct
user function from the same compiled script, exact required and declared
argument counts, no by-reference slots, no variadic flag, and no
by-reference return.

The caller frame descriptor references its lowered MIR function and frame
state. Because W05 does not lower the callee body, the callee entry descriptor
uses invalid lowered `function_id` and `frame_state_id` values and identifies
the target declaration by its stable `function_symbol_id` and `op_array_id`.
It is not a generic MIR function or frame record. The named W05 verifier must
match both declaration IDs to the call target and reject a caller identity
reused as the callee identity.

## Atomic planning

Before mutation, the planner:

1. scans every reachable source opcode;
2. balances a LIFO INIT/SEND/DO stack;
3. assigns each SEND to its active call site;
4. validates target, arguments, result, nesting, frames, and protected-region
   exclusion;
5. classifies each complete call as accepted, deferred, or rejected;
6. publishes a complete immutable plan or a zero-entry incomplete plan.

Fragments are never independently lowerable. Allocation failure, an orphan
SEND/DO, mismatched nesting, or an unsupported target leaves MIR unchanged.

## Accepted grammar

The only accepted grammar is:

```text
INIT_FCALL
  (SEND_VAL | SEND_VAL_EX | SEND_VAR | SEND_VAR_EX)*
DO_UCALL | proved-user DO_FCALL
```

Arguments are positional, by-value, defined, and exactly `null`, `bool`,
`long`, or `double`. No default, named, unpacked, variadic, placeholder,
reference, indirect, COW, object, method, internal, or protected semantics are
accepted. The result is unused or has an exact non-refcounted scalar summary.
A used result is mapped from its original source SSA ID into the call site's
`result_id`; W05 never synthesizes or guesses a call-result identity.

Named syntax is a source property, not something W05 may infer from the final
argument position. A private compiler marker tags every SEND produced from
`ZEND_AST_NAMED_ARG`, including arguments whose exact target lets Zend
normalize `op2` to a numeric position. The Native contract freezes that
`extended_value` bit as `ZEND_MIR_ZEND_SEND_SYNTACTIC_NAMED`; the source view
copies it to
`ZEND_MIR_SOURCE_CALL_ARGUMENT_SYNTACTIC_NAMED`; W05 must return `MIRL0024`
with deferred wave W07 whenever it is set. The bit lives in the existing
`zend_op.extended_value` field, changes no record layout, public header, or
public ABI, and is ignored by the VM.

## Verification and execution status

`zend_mir_verify_w05_calls()` checks call sites, targets, arguments, frames,
continuations, capabilities, and debts with stable `MIRV0700` through
`MIRV0705` codes. Planning and lowering failures use `MIRL0021` through
`MIRL0029`.

W04 Stage 1/2/3 guarantees describe the pre-mutation W04 projection and are
stored separately as W05 `prerequisite_guarantees`. The final W05 module
reports only `FINALIZED`; it does not claim that the frozen W03 generic
verifier accepts opcode 41. The named verifier validates the final call
extension and proves that it preserves the scalar and reducible-CFG
capabilities of that prerequisite projection.

The receipt requires all W05 capabilities and all explicit W05 debts. It
therefore states:

```text
modeled = true
codegen_eligible = false
```

W05 performs no call, invokes no Zend handler, exposes no public C-ABI, emits no
target code, and provides no VM fallback.
