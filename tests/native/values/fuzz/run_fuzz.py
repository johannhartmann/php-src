#!/usr/bin/env python3
"""Fixed-seed failure-atomic model fuzz for W06 value-plan spans."""

from __future__ import annotations

import argparse
import random


UINT32_MAX = (1 << 32) - 1
FAULT_STAGES = (
    "inventory",
    "plan",
    "storage",
    "reference",
    "alias",
    "event",
    "separation",
    "call_transfer",
    "verify",
    "finalize",
)


def bounded_span(offset: int, count: int, limit: int) -> tuple[int, int]:
    assert 0 <= offset <= UINT32_MAX
    assert 0 <= count <= UINT32_MAX
    assert offset <= limit
    assert count <= limit - offset
    return offset, count


def simulate_atomic_publish(fault: str | None) -> tuple[bool, tuple[str, ...]]:
    staged: list[str] = []
    for stage in FAULT_STAGES:
        if fault == stage:
            staged.clear()
            return False, tuple(staged)
        staged.append(stage)
    return True, ("module",)


def one_case(rng: random.Random) -> None:
    slots = rng.randrange(1, 513)
    references = {rng.randrange(slots) for _ in range(rng.randrange(slots + 1))}
    call_sites = rng.randrange(0, 33)
    literal_arguments = rng.randrange(0, 65)
    storage_count = slots + len(references) + call_sites + literal_arguments
    assert storage_count >= slots
    modes = rng.randrange(1, 513)
    offset = rng.randrange(0, 1 << 20)
    bounded_span(offset, modes, UINT32_MAX)
    cells = {slot: index for index, slot in enumerate(sorted(references))}
    assert len(cells) == len(references)
    assert all(0 <= cell < len(references) for cell in cells.values())

    opcode_count = rng.randrange(1, 513)
    event_counts = [rng.randrange(0, 3) for _ in range(opcode_count)]
    separation_counts = [rng.randrange(0, 2) for _ in range(opcode_count)]
    event_offset = 0
    separation_offset = 0
    for events, separations in zip(event_counts, separation_counts):
        bounded_span(event_offset, events, sum(event_counts))
        bounded_span(
            separation_offset, separations, sum(separation_counts)
        )
        event_offset += events
        separation_offset += separations
    assert event_offset == sum(event_counts)
    assert separation_offset == sum(separation_counts)

    return_base = slots + len(references)
    return_slots = [return_base + site for site in range(call_sites)]
    assert len(set(return_slots)) == call_sites
    assert all(slot < storage_count for slot in return_slots)
    for site in range(call_sites):
        parameter_count = rng.randrange(1, 257)
        argument_ordinal = rng.randrange(parameter_count)
        bounded_span(offset, parameter_count, UINT32_MAX)
        assert argument_ordinal < parameter_count
        assert return_slots[site] < storage_count

    fault = rng.choice((None, *FAULT_STAGES))
    published, records = simulate_atomic_publish(fault)
    if fault is None:
        assert published and records == ("module",)
    else:
        assert not published and records == ()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--cases", type=int, required=True)
    args = parser.parse_args()
    if args.cases < 1:
        raise SystemExit("cases must be positive")
    rng = random.Random(args.seed)
    for _ in range(args.cases):
        one_case(rng)
    print(f"W06 model fuzz: seed={args.seed} cases={args.cases} pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
