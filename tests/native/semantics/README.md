# W01 semantic test corpus

This directory is the W01-E semantic oracle inventory. `manifest.json` maps
deterministic fixtures to the sixteen required opcode families and the explicit
high-risk semantics. Existing php-src PHPTs are referenced in place. New PHPTs
exist only where the inventory needs a smaller test with directly observable
ordering or a forced boundary.

The corpus does not change PHP behavior and is not a production VM fallback.
Its reference side is always a caller-supplied PHP executable with captured
binary provenance. A reference-versus-reference result proves only harness and
fixture determinism; it is not evidence of native equivalence.

## Manifest contract

Every fixture records its kind and repository-relative source path, family and
risk attribution, exact opcodes only where directly attributable, required
extensions and modes, observable channels, repeated-call attribution, forced
boundary mechanism and source anchors, provenance requirement, determinism
constraints, and owner.

`validate_manifest.py` uses only the Python standard library. It rejects
unknown or missing fields, unsorted output, missing paths, duplicate IDs,
incomplete family or risk coverage, unproven forced boundaries, unstable
environment assumptions, incomplete call-1-through-call-10 attribution,
implicit reference provenance, and absolute checked-in paths.

The observer fixture requires the build-time `zend_test` extension. The bailout
fixture calls `zend_trigger_bailout()`, whose source-proven E_ERROR path reaches
`zend_bailout()` and `_zend_bailout()`'s `LONGJMP`. The ZTS fixture is a real
ZTS-only frame/reference test, not a label on an NTS execution.

## Checks that need no PHP binary

Run from the repository root:

```sh
python3 tests/native/semantics/validate_manifest.py --check
python3 -m unittest discover \
  -s tests/native/semantics/contracts/corpus -p 'test_*.py' -v
python3 -m json.tool tests/native/semantics/manifest.schema.json
python3 -m json.tool tests/native/semantics/manifest.json
```

Without `W01_E_REFERENCE_PHP`, the unit suite reports one concrete skip for the
reference capture/self-diff integration test. All other validator, negative,
PHPT-structure, provenance, path, and call-attribution tests run without PHP.

## Required reference PHP

`REFERENCE_PHP` must be an absolute path. It must be a ZTS build with
`zend_test` built in so that the mandatory observer, bailout, and ZTS fixtures
execute rather than silently skip. Neither `capture_reference.py` nor its
integration test searches `PATH`; missing capabilities are fatal prerequisites.

Use an external, initially empty artifact directory:

```sh
REFERENCE_PHP=/absolute/path/to/reference/php
ARTIFACT_ROOT=/absolute/path/outside/php-src/w01-e

python3 tests/native/capture_manifest.py \
  --php "$REFERENCE_PHP" \
  --json-out "$ARTIFACT_ROOT/reference-manifest.json"

TEST_PHP_EXECUTABLE="$REFERENCE_PHP" \
  "$REFERENCE_PHP" run-tests.php -q tests/native/semantics/corpus/phpt

python3 tests/native/semantics/capture_reference.py \
  --reference-php "$REFERENCE_PHP" \
  --artifact-dir "$ARTIFACT_ROOT/reference-bundle"

python3 tests/native/diff_runner.py \
  --reference "$REFERENCE_PHP" \
  --candidate "$REFERENCE_PHP" \
  --case tests/native/semantics/corpus/differential \
  --timeout 5 \
  --json-out "$ARTIFACT_ROOT/reference-self-diff.json"

W01_E_REFERENCE_PHP="$REFERENCE_PHP" \
  python3 -m unittest discover \
    -s tests/native/semantics/contracts/corpus -p 'test_*.py' -v
```

The capture tool starts every child from a minimal fixed environment, points
`PHPRC` at an empty external directory, disables INI scanning, and records that
effective environment. Caller PHP configuration, preload settings, and test
arguments are not inherited. It then validates the manifest and binary
capabilities, invokes
the W00 binary-manifest tool, runs every PHPT referenced by the manifest, runs
the differential directory reference-versus-reference, and writes raw output,
termination, hashes, and `bundle-index.json`. The index hashes every manifest
fixture and every auxiliary corpus input in addition to all generated payloads,
and records the corpus Git commit/dirty state. The bundle stores absolute binary
identity only in the external evidence. No golden output or local absolute path
is checked into the repository.

The ten files under `corpus/differential/calls/` are deliberately separate.
Each has one manifest entry and one `repeat_calls` value, keeping warm calls one
through ten attributable at the W00 differential runner's per-case granularity.
