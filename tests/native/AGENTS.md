# Native test instructions

These instructions apply to `tests/native/**` in addition to php-src's PHPT
rules in `CONTRIBUTING.md` and `docs/source/miscellaneous/`.

- Make fixtures deterministic and self-contained. Pin seeds, locale, timezone,
  environment, and ordering whenever they could affect output.
- Accept an explicit reference-PHP binary path and use that binary as the
  semantic oracle. Do not discover a reference binary implicitly.
- Compare observable output, exit status, diagnostics, and declared side
  effects. Do not silently normalize values or discard mismatches.
- Permit normalization only when a documented field is inherently unstable;
  make every normalization visible in result metadata and test it separately.
- Keep call-1-through-call-10 coverage independently attributable when a
  differential scenario exercises repeated calls.
- Clean up all resources created by a fixture and report skips with a concrete
  unmet prerequisite.
