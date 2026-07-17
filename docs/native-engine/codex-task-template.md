# Native-engine Codex task template

Copy this template into a task and replace every placeholder before work begins.

## Basiscommit

- Repository root: `{ABSOLUTE_WORKTREE_PATH}`
- Expected HEAD: `{FULL_COMMIT_ID}`
- Current branch: `{BRANCH_NAME}`
- Integration base: `{INTEGRATION_BRANCH_OR_COMMIT}`

Verify the root, HEAD, branch, applicable `AGENTS.md` files, and a clean
worktree. Stop without changes if they do not match or foreign changes exist.

## Ziel

`{ONE_TESTABLE_OUTCOME}`

## Pfadbesitz

- May change: `{EXACT_PATH_OR_GLOB}`
- Must not change: `{RESERVED_OR_PARALLEL_PATHS}`
- Owner of adjacent integration work: `{OWNER_OR_TASK}`

Do not change paths outside this declaration. Record necessary cross-owner work
as an integration item.

## Nichtziele

- `{EXPLICIT_NON_GOAL}`
- No opportunistic refactoring or undeclared product behavior changes.

## Contracts

- Applicable ADRs: `{RELATIVE_LINKS}`
- Inputs and outputs: `{FORMATS_AND_PATHS}`
- ABI/persistence/concurrency constraints: `{CONSTRAINTS_OR_NOT_APPLICABLE}`
- Required failure behavior: `{FAIL_CLOSED_BEHAVIOR}`

## Akzeptanzkriterien

- [ ] `{OBJECTIVELY_VERIFIABLE_RESULT}`
- [ ] Only owned paths changed.
- [ ] No test failure was suppressed or reclassified.

## Testbefehle

| Command | What it proves | Required environment |
|---|---|---|
| `{EXACT_COMMAND}` | `{COVERED_REQUIREMENT}` | `{TOOLS/PLATFORM}` |
| `git diff --check` | Patch formatting | Git worktree |
| `git status --short` | Final cleanliness | Git worktree |

Report unavailable tests with the exact command, cause, and required runner.

## Handoff

Use exactly these headings:

```markdown
## Ergebnis
PASS | PARTIAL | BLOCKED

## Basis und Commit
- erwartete Basis: ...
- tatsächliche Basis: ...
- Branch: ...
- erzeugter Commit: ...

## Änderungen
- `path`: purpose

## Verifikation
| Befehl | Ergebnis | Dauer/Anmerkung |
|---|---|---|

## Akzeptanzkriterien
- [x] ...
- [ ] ... — Blocker: ...

## Risiken und Integrationshinweise
- ...

## Worktree-Zustand
- `git status --short`: sauber/nicht sauber
```

Use `PASS` only when every mandatory acceptance criterion is satisfied.
