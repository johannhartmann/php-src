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
user function from the same compiled script, an explicit argument count
between required and declared counts, no by-reference slots, no variadic flag,
and no by-reference return. The zero symbol/op-array pair identifies the
currently lowered declaration and is an exact direct self-call.

The caller frame descriptor references its lowered MIR function and frame
state. Because W05 does not lower the callee body, the callee entry descriptor
uses invalid lowered `function_id` and `frame_state_id` values and identifies
the target declaration by its stable `function_symbol_id` and `op_array_id`.
It is not a generic MIR function or frame record. The named W05 verifier must
match both declaration IDs to the call target. For an exact direct self-call,
the declaration IDs intentionally equal the caller declaration while the
callee's lowered `function_id` and `frame_state_id` remain invalid.

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

Arguments are ordered, by-value, defined, and exactly `null`, `bool`, `long`,
or `double`. A named call is accepted after the compiler has fully normalized
it into a complete static ordinal sequence, provided no runtime named
container, placeholder, or default materialization remains. A default-bearing
target is accepted when every argument is explicitly supplied. Unpacked,
variadic, reference, indirect, COW, object, method, internal, or protected
semantics are not accepted. The result is unused or has an exact
non-refcounted scalar summary.
A used result is mapped from its original source SSA ID into the call site's
`result_id`; W05 never synthesizes or guesses a call-result identity.

Parameter modes are an ordered table keyed by stable target ID and zero-based
ordinal, without a 64-parameter ceiling. W05 still rejects a by-reference mode
from the exact record. It does not preserve named source spelling in a private
compiler bit.

## Verification and execution status

`zend_mir_verify_w05_calls()` checks call sites, targets, arguments, frames,
continuations, capabilities, and debts with stable `MIRV0700` through
`MIRV0705` codes. A final composition verifier independently checks the
extension-aware structural records, exact scalar facts, reciprocal CFG edges
and nonterminating call placement, and the complete call model. These checks
run directly before the module is returned. The stable module and source
projections are recomputed once to detect mutation during verification; no
verification or capability records are persisted in MIR. Planning and
lowering failures use `MIRL0021` through `MIRL0029`.

W04 Stage 1/2/3 guarantees describe the pre-mutation W04 projection and are
stored separately as W05 `prerequisite_guarantees`. The final W05 module
reports only `FINALIZED`; it does not claim that the frozen W03 generic
verifier accepts opcode 41. The named verifier validates the final call
extension and proves that it preserves the scalar and reducible-CFG
capabilities of that prerequisite projection.

The lowering result exposes all W05 capabilities and explicit W05 debts. It
therefore states:

```text
modeled = true
codegen_eligible = false
```

W05 performs no call, invokes no Zend handler, exposes no public C-ABI, emits no
target code, and provides no VM fallback.
