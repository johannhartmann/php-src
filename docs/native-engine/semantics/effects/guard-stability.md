# Guard stability

A guard fact is valid only from its establishing proof to the first listed
invalidation.  `stable_across` is an allow-list; absence from that list means
the optimizer needs a new proof.  Memory invalidations apply through
`may_alias`, including undeclared alias edges discovered locally.

| Fact | Stable across | Invalidated by | Required proof |
| --- | --- | --- | --- |
| value type | read-only, non-aliasing operations | value/reference writes, calls, reentry | dominating type test and complete alias proof |
| object class identity | read-only scalar operations | object/reference writes, calls, magic access, reentry | exact-class and identity guard |
| refcount uniqueness | non-escaping reads | copies, writes, calls, destructors, reentry | count one and no writable aliases |
| packed array layout | plain element reads | array mutation, COW separation, calls, destructors | packed-layout and complete alias guard |
| array key presence | reads that cannot expose references | array/reference writes, calls, magic behavior | key lookup and complete alias guard |
| reference binding | reads outside the pointee | reference/slot writes, calls, reentry | binding identity and complete alias guard |
| runtime cache entry | unrelated local reads | cache/table writes, calls, reentry | cache generation and target guard |
| function target | unrelated local reads | function-table/cache writes, calls, reentry | resolved entry identity guard |
| class table entry | unrelated local reads | class-table/cache writes, calls, reentry | resolved entry identity guard |
| no exception pending | proven-total local reads | throw, calls, destructors, magic, interrupts | exception-state check and no-throw path |
| frame slot value | reads of other proven-disjoint slots | frame writes and observation barriers | materialized slot/version guard |
| destructor absence | reads that preserve exact type | value/class writes, calls, reentry | exact non-destructing type and lifetime proof |

For example, a packed-array guard cannot float across an aliasing write merely
because the syntactic array variable did not change.  Likewise, an internal
call is not assumed harmless: without a modeled summary it receives the
fail-closed behavior and invalidates every guard.
