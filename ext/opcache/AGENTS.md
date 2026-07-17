# OPcache native-engine instructions

These instructions apply to `ext/opcache/**` in addition to the repository root
instructions.

- Do not store process-local raw pointers in persistent OPcache images.
- Represent persistent references with relocatable, validated identifiers or
  offsets and reconstruct process-local state after loading.
- Version every persistent-format change. Define compatibility, rejection, and
  cache-invalidation behavior before implementation.
- Validate bounds, alignment, identity, and code-version provenance while
  loading persistent native metadata.
- Keep OPcache/JIT behavior unchanged during W00 contract work.
