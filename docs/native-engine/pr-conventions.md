# Native-engine pull-request conventions

These rules add to, and do not replace, `CONTRIBUTING.md` and
`CODING_STANDARDS.md`.

- Give each pull request one reviewable purpose and one declared path-ownership
  scope.
- Prefer fewer than 20 logically changed production files. If a coherent change
  must exceed that guide, explain why it cannot be split without weakening the
  contract or verification.
- Land or include an accepted contract before its implementation. Record
  architectural decisions in an ADR before depending on them in code.
- Do not mix refactoring, formatting, generated-file churn, or drive-by fixes
  with a behavioral change.
- Keep commits linear and individually reviewable. Do not rewrite already
  integrated commits; add a focused follow-up commit.
- Follow php-src commit-message rules, including a descriptive subject and
  lines shorter than 80 characters.
- State the base commit, changed contracts, exact tests and results, unsupported
  runners, ABI or persistence effects, and integration dependencies in the PR.
- Never make a red test green by suppressing output, weakening an oracle,
  broadening normalization, or converting an unexplained failure into a skip.
