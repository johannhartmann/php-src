# Mapper, relocation, protection, and unwind boundaries

These layers have different persistence and lifetime properties. Combining them
under “ELF support” would obscure the work PHP must own.

## 1. Assembler and object output

`AssemblerElf` owns sections, ELF symbols, and relocation records while the
compiler emits a module. `build_object_file()` serializes a conventional ELF
object. This is architecture-specific output for x86-64 or AArch64, but it is not
a live mapping and does not by itself define an OPcache image format.

The relocation records remain reachable internally by `ElfMapper` through
friend access to private `get_relocs()`. That access is enough for the bundled
mapper, not a stable custom-mapper or persistence API.

## 2. Existing `ElfMapper`

At the pinned revision, `ElfMapper::map()` performs one coupled live operation:

1. reserve PLT/GOT slots and sort sections by final permission;
2. compute one mapping layout;
3. allocate a private anonymous mapping with read/write permission;
4. resolve undefined symbols through the caller's process-local resolver;
5. create mapper-local symbol-address and PLT/GOT state;
6. copy section bytes and apply target relocations;
7. apply final section permissions with `mprotect`;
8. register mapped EH-frame data.

`reset()` deregisters the frame and unmaps the range. The mapper is non-copyable
and stores its mapped base, size, registered-frame offset, and resolved symbol
addresses. This is a suitable reference lifecycle for request/process-local
code. It is not shared OPcache storage: `MAP_PRIVATE | MAP_ANONYMOUS` has no
cross-process persistent identity, and the resolved addresses are meaningful
only in the current process.

## 3. PHP-local mapping

The first PHP mapper should remain process-local but use PHP-owned allocation
and lifecycle policy. It needs a generic finalized-image view from TPDE:

```text
section descriptors: kind, bytes, size, alignment, final permissions
symbol descriptors: stable index, name/binding, section and offset or undefined
relocation descriptors: section, offset, type, symbol index, addend
target layout constraints: PLT/GOT/veneer size, alignment, reachability
unwind descriptor: EH-frame section and registration range
```

TPDE should also expose target relocation application over caller-provided
addresses. PHP chooses memory, copies data, invokes relocation, performs the
target publish operation, applies final protections, registers unwind data, and
publishes symbols/perf information. No PHP type is needed in these APIs.

The PHP mapper must enforce W^X: construction memory is writable and not
executable; executable sections become read/execute only after copy and
relocation. The pinned mapper's final protection calculation does not reject an
input section carrying both write and execute flags, so that validation must be
added rather than assumed. Failure before publication must not leave a callable
address or registered unwind range.

## 4. Future relocatable OPcache image

A persistent image is address-neutral data, not a dump of `ElfMapper` state. It
may contain immutable section bytes, stable symbol names/indices, relocation
records, target identity, format version, and validation hashes. It must not
contain resolved helper pointers, mapped addresses, unwind-registration handles,
or process-specific GOT contents.

On load, each process:

1. validates image and target identity;
2. allocates its live mapping;
3. constructs per-process GOT/PLT/veneer storage;
4. resolves external symbols in that process;
5. applies relocations at the chosen base;
6. completes target cache publication;
7. sets final permissions;
8. registers the live EH-frame range;
9. publishes code and diagnostic symbols atomically.

This split permits immutable OPcache data to be shared while keeping ASLR and
runtime addresses local.

## 5. Target relocation and cache differences

The existing x86-64 mapper applies its target relocation set and can use PLT
slots for distant external calls. The proposed exported descriptors must
preserve exact relocation types and signed-range checks; PHP must not reinterpret
them generically.

AArch64 has CALL26, page-relative address, low-12, and GOT relocation behavior.
The existing CALL26 path redirects a distant target through a mapper-created PLT
slot, then checks the replacement displacement as signed 32-bit rather than the
instruction's aligned signed 28-bit byte range. That must be corrected. A custom
mapper also needs an explicit reachability and veneer-allocation contract; large
layouts can require more than one reachable veneer region.

After writing or relocating AArch64 instructions, the publishing path must
perform the platform-required cache maintenance before execution. The pinned
`ElfMapper::map()` sequence contains no such operation between copying and
publishing. Adding a target-aware publish primitive is a core correctness patch,
not an optional PHP optimization. On x86-64 the corresponding operation may be a
documented no-op.

## 6. Unwind differences

`FunctionWriter` emits DWARF CFI/EH-frame records. The existing mapper registers
the mapped frame data with the process unwinder and deregisters it before unmap.
That covers live `ElfMapper` mappings.

An ELF object merely contains unwind sections; writing it does not register a
live range. Likewise, a persistent OPcache image stores unwind bytes but cannot
persist registration. Every process must register the relocated live range only
after mapping succeeds, retain the registration for at least the code lifetime,
and deregister before releasing or replacing that range.
