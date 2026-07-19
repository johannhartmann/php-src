# Native-engine capability roadmap

`capabilities.json` is the machine-readable dependency and semantic-debt model.
It deliberately does not treat wave order as proof of completeness. Receipts
list the capabilities actually provided and every still-open semantic debt.

W05 models direct user-call structure only. It does not execute MIR, call the
VM, emit target code, or cross the C ABI.
