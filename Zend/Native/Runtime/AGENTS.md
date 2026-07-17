# Native runtime instructions

These instructions apply to `Zend/Native/Runtime/**`.

- Runtime builtins must implement bounded operations; they must not invoke or
  contain an opcode-dispatch loop.
- Preserve Zend bailout, exception, observer, interrupt, and reentrancy
  semantics at every runtime boundary.
- Document whether each helper may allocate, call user code, reenter PHP, throw,
  or bail out. Encode the corresponding effects and frame state in MIR.
- Root every live Zend value across allocation, calls, reentrancy, and bailout
  points according to the active frame contract.
- Do not use a runtime helper to provide a production VM fallback.
