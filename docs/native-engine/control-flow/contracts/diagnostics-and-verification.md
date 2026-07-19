# W04 diagnostics and verification

W03 codes `[MIRL0000]` through `[MIRL0013]` and the W03 guarantee mask remain
unchanged. W04 adds:

| Token | Meaning |
| --- | --- |
| `[MIRL0014]` | malformed source CFG |
| `[MIRL0015]` | protected source region |
| `[MIRL0016]` | irreducible loop |
| `[MIRL0017]` | unsupported PHI or Pi |
| `[MIRL0018]` | missing or contradictory branch proof |
| `[MIRL0019]` | incomplete source/MIR mapping |
| `[MIRL0020]` | stage-3 verification failure |

Stage-3 verifier tokens are `[MIRV0600]` block, `[MIRV0601]` edge,
`[MIRV0602]` branch, `[MIRV0603]` PHI, `[MIRV0604]` loop, and `[MIRV0605]`
edge-statepoint mismatch.

A successful W04 result contains finalization plus stage 1, 2, and 3 guarantee
bits and a module. Every other result contains zero guarantees and no module.
Verification runs while the source view and process-local map are alive.
