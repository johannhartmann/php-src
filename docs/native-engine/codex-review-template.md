# Native-engine read-only review template

Review `{CHANGESET}` against `{BASE}`. Work read-only: do not edit, format,
commit, or otherwise change files.

Read all applicable `AGENTS.md` files, the task contract, linked ADRs, and the
entire diff. Verify:

1. semantic equivalence and intentional behavior changes;
2. public and internal ABI boundaries, Zend frame compatibility, and C/C++
   isolation;
3. ownership, lifetime, persistence, relocation, and code-version invariants;
4. concurrency, reentrancy, bailout, exception, and publication behavior;
5. test coverage, determinism, oracle provenance, and untested failure paths;
6. path ownership, non-goals, generated files, and unrelated refactoring;
7. contract and ADR compliance, including the ban on production VM fallback.

Report findings first, ordered by severity:

- **P0:** immediate, broad catastrophic impact or release-blocking compromise;
- **P1:** likely serious correctness, security, ABI, or data-integrity failure;
- **P2:** bounded defect, missing important test, or maintainability issue with a
  concrete failure mode;
- **P3:** low-impact but actionable contract, clarity, or test-quality defect.

For every finding provide `path:line`, the violated invariant, a concrete impact
scenario, and the smallest reasonable correction. Do not report preferences
without a demonstrable impact. If there are no findings, say so and list only
the residual risks or verification gaps; do not imply that unrun tests passed.
