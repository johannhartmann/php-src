# ADR 0004: Zend-compatible baseline frames

## Status

Accepted on 2026-07-17.

## Context

PHP extensions, observers, exceptions, bailouts, generators, and reentrant calls
rely on Zend execution and value-lifetime semantics. A native-only frame layout
that cannot present those semantics would require pervasive ABI changes or
unsafe shadow state.

## Decision

Define the native baseline frame as Zend-compatible at every externally
observable boundary. The contract preserves required call-frame identity,
arguments, locals, return values, exception and bailout state, observer-visible
metadata, and GC roots. Native-private data may be attached only through
versioned internal metadata that does not alter public Zend C ABI structures
without a separately approved ABI decision.

Every call, allocation, reentrancy, bailout, exception, and resume edge carries
an explicit frame state sufficient to expose or reconstruct the compatible
baseline frame.

## Consequences

- Existing Zend-facing code observes defined frame semantics across native
  execution.
- Native code must maintain roots and frame metadata even when a target ABI
  keeps values in registers.
- Frame-layout changes require compatibility analysis and cannot be hidden in a
  backend.

## Alternatives

- A wholly separate native frame ABI was rejected because it would leak into
  extensions and runtime services.
- Lazy best-effort frame reconstruction was rejected because asynchronous and
  exceptional observation points cannot tolerate missing state.

## Verification impact

ABI tests must cover arguments, locals, returns, observers, exceptions,
bailouts, generators, GC, and reentrancy. Frame-state verification must run
before code publication, and public headers must remain C-compatible.
