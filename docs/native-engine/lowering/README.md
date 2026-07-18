# W03 lowering profile

`w03-opcode-profile.json` is generated from the validated W01 opcode matrix. It
classifies every active opcode as:

- `required`: accepted without opcode-specific value proofs;
- `conditional`: accepted only when every named proof is present;
- `deferred`: rejected by W03 with an explicit later wave.

The live W01 source contains 212 active opcode numbers (plus reserved 45 and
79). This source-backed count supersedes the earlier W03 planning snapshot of
210. No opcode is omitted to preserve that obsolete count.

The accepted surface is deliberately narrow: one reachable straight-line
block, exact null/bool/long/double facts, no calls or re-entry, and no hidden
reference-count, destructor, observer, exception, or overflow path. `DIV`,
`POW`, strings, mutating assignment/inc/dec, calls, branches, switch/match,
exceptions, references, arrays, objects, generators, fibers, observers,
interrupts, and I/O remain deferred.

Regenerate with:

```sh
python3 scripts/native/lowering/generate-profile.py --write
python3 scripts/native/lowering/generate-profile.py --check
```
