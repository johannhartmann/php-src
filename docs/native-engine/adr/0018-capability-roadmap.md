# ADR 0018: Capability and semantic-debt roadmap

## Status

Accepted.

## Context

Sequential wave numbers do not imply semantic completeness. In particular,
modeling a direct user call does not provide C-ABI interoperability, exception
behavior, dynamic dispatch, reference ownership, or executable target code.

## Decision

The machine-readable capability roadmap assigns every capability explicit
prerequisites and records semantic debts separately. W05 models a narrow
source-backed direct user-call sequence and frame descriptors without executing
the call. W12 may emit target code only when all required capabilities are
present and the module debt set is empty. Internal functions and C-ABI
interoperability remain W15 work.

Receipts publish capabilities and debts; they never infer completeness merely
from a wave number. `codegen_eligible` remains false through this program
foundation.

## Consequences

Downstream gates can make positive capability checks and exact debt checks.
References, argument containers, object targets, protected continuations,
dynamic calls, target emission, and internal/C-ABI calls remain visible rather
than being hidden behind a generic calls milestone.
