# ADR 0018: Capability and semantic-debt roadmap

## Status

Accepted.

## Context

Sequential wave numbers do not imply semantic completeness. In particular,
modeling a direct user call does not provide C-ABI interoperability, exception
behavior, dynamic dispatch, reference ownership, or executable target code.

## Decision

W05 models a narrow source-backed direct user-call sequence and frame
descriptors without executing the call. The lowering result explicitly reports
which call-model capabilities are present and which execution gaps remain.
Target emission must stay disabled while any required execution semantics are
missing. Internal functions and C-ABI interoperability remain future work.

Persistent MIR does not carry roadmap state, workflow history, or planning IDs.
`codegen_eligible` remains false until the implementation and its direct
verifiers prove the required runtime semantics.

## Consequences

Downstream compiler code can inspect the successful lowering result directly.
References, argument containers, object targets, protected continuations,
dynamic calls, target emission, and internal/C-ABI calls remain visible rather
than being hidden behind a generic calls milestone.
