# W02 cross-track contract

## Pinned base

All six specialist results are reviewed against the master contract commit
`b2d0e87766fce3659a3de41ff72f06655d896aae` (H). They do not use the
wave-definition commit (P) as their implementation base. The `master` branch
retains P and the integrated specialist commits.

H freezes the headers, W01 numeric bindings, ADRs, contract checker, and the
test-only fixed-array host. Specialist tracks must not change those frozen
files. Any incompatible contract discovery stops the affected track and is
resolved centrally before integration; numeric IDs are never repurposed.

## Track ownership

| Track | Owned implementation and test paths |
| --- | --- |
| W02-A | Core storage, arena, ID implementation, and `tests/native/mir/core/**` |
| W02-B | CFG, PHI, dominance implementation, and `tests/native/mir/cfg/**` |
| W02-C | Effect/ownership binding implementation and `tests/native/mir/effects/**` |
| W02-D | Frame-state/source-map implementation and `tests/native/mir/frame-state/**` |
| W02-E | Canonical text/parser implementation and `tests/native/mir/text/**` |
| W02-F | Stage-one verifier implementation and `tests/native/mir/verifier/**` |

The exact file lists are normative in `waves.json`. Fachtrack ownership is
disjoint. The integration task may touch the combined W02 paths only after the
specialist commits are produced; this serial ownership does not authorize a
parallel track to modify another track's files.

## Shared invariants

- `zend_mir_view` is the read-only observation boundary for B, C, D, E, and F.
- Construction uses `zend_mir_mutator`; test construction uses only the host in
  `tests/native/mir/contracts`, never a production mock linked into PHP.
- All W01 catalog values and the original/synthetic value-ID split remain exact.
- Unknown, overflowing, malformed, or unmodeled input fails closed with a
  bounded stable diagnostic. It never acquires VM-fallback semantics.
- Text output and stage-one verification are storage- and target-independent.
- No track introduces target registers, TPDE types, machine offsets, persistent
  raw pointers, runtime/build integration, or production opcode dispatch.

## Integration order

The integration branch begins at P, then takes A through F in dependency-safe
order. Each task result declares H as both expected and actual base. The W02
gate remains incomplete until all six specialist task results and
`W02-integration-gate` pass against the frozen contract.
