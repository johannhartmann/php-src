# ADR 0008: Differential testing as the semantic oracle

## Status

Accepted on 2026-07-17.

## Context

PHP behavior includes output, diagnostics, exit status, exceptions, ordering,
and side effects that unit tests of compiler internals cannot fully define. A
full cutover needs comparison against the pinned reference implementation
without hiding native mismatches through broad normalization.

## Decision

Use a caller-supplied, explicit reference PHP binary as the semantic oracle for
differential tests. Run the same deterministic fixture and declared environment
with reference and candidate binaries. Compare observable output, diagnostics,
exit status, and declared side effects, including separately attributable calls
1 through 10 for repeated-call scenarios.

Apply no silent normalization. Allow a narrowly defined normalization only when
the field is inherently unstable, the rule is recorded in result metadata, and
the normalizer has direct tests. Record binary paths, binary identities, source
revision, platform, configuration, fixture identity, and normalization set with
each baseline or result.

## Consequences

- Expected behavior comes from a reproducible executable provenance rather than
  hand-maintained output alone.
- Native mismatches fail visibly and remain attributable to a fixture and call.
- Baselines are invalid when provenance or required comparison fields are
  missing.
- Reference execution is test infrastructure, not a production fallback.

## Alternatives

- Comparing only PHPT expected output was rejected because it can omit process
  and repeated-call behavior needed by the native harness.
- Broad filtering of addresses, ordering, warnings, or timing was rejected
  because it can erase real semantic differences.

## Verification impact

Harness self-tests must inject mismatches into every compared channel, verify
call attribution and provenance rejection, and test each permitted
normalization. A real baseline must name the exact reference and candidate
binaries and must not be inferred from the current `PATH`.
