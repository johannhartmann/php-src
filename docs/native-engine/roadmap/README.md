# Native-engine implementation roadmap

The next implementation work is closing the semantic gaps that keep W05's
direct-user-call model from executing: reference ownership, exception and
bailout cleanup, dynamic targets, observer integration, target emission, and
the internal C ABI. These are engineering tasks, not repository workflow states.

W05 currently models direct user-call structure only. It does not execute MIR,
call the VM, emit target code, or cross the C ABI.
