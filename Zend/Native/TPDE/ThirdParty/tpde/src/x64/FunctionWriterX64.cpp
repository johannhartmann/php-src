// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde/x64/FunctionWriterX64.hpp"
#include "fadec-enc2.h"

#include <algorithm>

namespace tpde::x64 {

// TODO: use static constexpr array in C++23.
static consteval auto get_cie_initial_instrs_x64() {
  std::array<u8, 32> data{};
  // the current frame setup does not have a constant offset from the FP
  // to the CFA so we need to encode that at the end
  // for now just encode the CFA before the first sub sp

  // we always emit a frame-setup so we can encode that in the CIE

  u8 *dst = data.data();
  // def_cfa rsp, 8
  dst += FunctionWriterX64::write_eh_inst(
      dst, dwarf::DW_CFA_def_cfa, dwarf::x64::DW_reg_rsp, 8);
  // cfa_offset ra, 8
  dst += FunctionWriterX64::write_eh_inst(
      dst, dwarf::DW_CFA_offset, dwarf::x64::DW_reg_ra, 1);
  return std::make_pair(data, dst - data.data());
}

static constexpr auto cie_instrs_x64 = get_cie_initial_instrs_x64();

const FunctionWriterX64::TargetCIEInfo FunctionWriterX64::CIEInfo{
    .instrs = {cie_instrs_x64.first.data(), cie_instrs_x64.second},
    .return_addr_register = dwarf::x64::DW_reg_ra,
    .code_alignment_factor = 1, // ULEB128 1
    .data_alignment_factor = 120, // SLEB128 -8
};

void FunctionWriterX64::relax_jumps(u32 body_begin) {
  assert(label_skew == 0 && !cold_active);
  struct Jump {
    u32 start;   ///< Offset of the jmp/jcc rel32 instruction.
    u32 fixup;   ///< Index of its label fixup.
    Label label; ///< Resolved target label.
    u8 len;      ///< Current length: 5/6 (rel32), 2 (rel8) or 0 (removed).
    u8 opcode;   ///< 0xeb for jmp, 0x70|cc for jcc.
  };
  util::SmallVector<Jump, 0> jumps;
  for (u32 i = 0; i < label_fixups.size(); ++i) {
    const LabelFixup &fixup = label_fixups[i];
    if (fixup.kind != LabelFixupKind::X64_JMP_OR_MEM_DISP ||
        fixup.off < body_begin + 2) {
      continue;
    }
    const u8 *disp = begin_ptr() + fixup.off;
    if (disp[-1] == 0xe9) {
      jumps.push_back(Jump{fixup.off - 1, i, label_resolve_jump(fixup.label),
                           5, 0xeb});
    } else if (disp[-2] == 0x0f && (disp[-1] & 0xf0) == 0x80) {
      jumps.push_back(Jump{fixup.off - 2, i, label_resolve_jump(fixup.label),
                           6, u8(0x70 | (disp[-1] & 0x0f))});
    }
  }
  if (jumps.empty()) {
    return;
  }
  std::sort(jumps.begin(), jumps.end(),
            [](const Jump &a, const Jump &b) { return a.start < b.start; });

  // removed[k]: bytes removed by jumps[0..k-1]. Shortening a jump never
  // lengthens another, so decisions taken with stale sums stay valid.
  util::SmallVector<u32, 0> removed;
  removed.resize(jumps.size() + 1);
  const auto recount = [&] {
    removed[0] = 0;
    for (u32 k = 0; k < jumps.size(); ++k) {
      const u32 full = jumps[k].opcode == 0xeb ? 5 : 6;
      removed[k + 1] = removed[k] + (full - jumps[k].len);
    }
  };
  // New offset of an old offset: bytes removed by jumps starting before it.
  const auto map = [&](u32 off) -> u32 {
    const auto it = std::lower_bound(
        jumps.begin(), jumps.end(), off,
        [](const Jump &jump, u32 value) { return jump.start < value; });
    return off - removed[it - jumps.begin()];
  };

  recount();
  for (bool changed = true; changed;) {
    changed = false;
    for (u32 k = 0; k < jumps.size(); ++k) {
      Jump &jump = jumps[k];
      if (jump.len <= 2) {
        continue;
      }
      const u32 target = label_offsets[u32(jump.label)];
      assert(target != ~0u && target < ColdAreaBase);
      if (target == jump.start + jump.len) {
        jump.len = 0;
        changed = true;
        continue;
      }
      const i64 delta = i64(map(target)) - (i64(jump.start - removed[k]) + 2);
      if (delta >= -128 && delta <= 127) {
        jump.len = 2;
        changed = true;
      }
    }
    recount();
  }
  if (removed[jumps.size()] == 0) {
    return;
  }

  // Compact the code in place.
  util::SmallVector<u32, 0> new_start;
  new_start.resize(jumps.size());
  u8 *const base = begin_ptr();
  u32 read = jumps[0].start;
  u32 write = read;
  for (u32 k = 0; k < jumps.size(); ++k) {
    const Jump &jump = jumps[k];
    const u32 full = jump.opcode == 0xeb ? 5 : 6;
    std::memmove(base + write, base + read, jump.start - read);
    write += jump.start - read;
    new_start[k] = write;
    if (jump.len == 2) {
      base[write] = jump.opcode;
      base[write + 1] = 0;
    } else if (jump.len == full) {
      std::memmove(base + write, base + jump.start, full);
    }
    write += jump.len;
    read = jump.start + full;
  }
  const u32 end = offset();
  std::memmove(base + write, base + read, end - read);
  write += end - read;
  assert(end - write == removed[jumps.size()]);

  for (u32 &off : label_offsets) {
    if (off != ~0u && off >= body_begin) {
      off = map(off);
    }
  }
  util::SmallVector<u8, 0> is_jump;
  is_jump.resize(label_fixups.size());
  for (u32 k = 0; k < jumps.size(); ++k) {
    is_jump[jumps[k].fixup] = 1;
  }
  util::SmallVector<LabelFixup, 0> kept;
  for (u32 i = 0; i < label_fixups.size(); ++i) {
    if (!is_jump[i]) {
      LabelFixup fixup = label_fixups[i];
      if (fixup.off >= body_begin) {
        fixup.off = map(fixup.off);
      }
      kept.push_back(fixup);
    }
  }
  for (u32 k = 0; k < jumps.size(); ++k) {
    const Jump &jump = jumps[k];
    if (jump.len == 2) {
      const i64 delta =
          i64(label_offsets[u32(jump.label)]) - (i64(new_start[k]) + 2);
      assert(delta >= -128 && delta <= 127);
      base[new_start[k] + 1] = u8(i8(delta));
    } else if (jump.len != 0) {
      // Keep the resolved target; handle_fixups resolves aliases again.
      kept.push_back(LabelFixup{jump.label, new_start[k] + jump.len - 4u,
                                LabelFixupKind::X64_JMP_OR_MEM_DISP});
    }
  }
  label_fixups.clear();
  for (const LabelFixup &fixup : kept) {
    label_fixups.push_back(fixup);
  }
  for (JumpTable *jt : jump_tables) {
    jt->off = map(jt->off);
  }
  section->remap_relocation_offsets(reloc_begin, body_begin,
                                    [&](u64 off) { return u64(map(u32(off))); });
  data_cur = base + write;
  label_place_off = ~0u;
  labels_at_place_off.clear();
}

void FunctionWriterX64::handle_fixups() {
  for (const LabelFixup &fixup : label_fixups) {
    u32 fixup_off = fixup.off - label_skew;
    u8 *dst_ptr = begin_ptr() + fixup_off;
    // A jmp rel32 (E9) or jcc rel32 (0F 8x) precedes its displacement; no
    // RIP-relative operand has such a ModRM byte.
    const bool is_jmp = fixup_off >= 1 && dst_ptr[-1] == 0xe9;
    const bool is_jcc = fixup_off >= 2 && dst_ptr[-2] == 0x0f
                        && (dst_ptr[-1] & 0xf0) == 0x80;
    u32 label_off = label_offset(is_jmp || is_jcc
                                     ? label_resolve_jump(fixup.label)
                                     : fixup.label);
    switch (fixup.kind) {
    case LabelFixupKind::X64_JMP_OR_MEM_DISP: {
      // fix the jump immediate
      u32 value = (label_off - fixup_off) - 4;
      if (value == 0 && is_jmp) {
        // A jump to the next instruction becomes a 5-byte NOP.
        static constexpr u8 nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
        std::memcpy(dst_ptr - 1, nop5, sizeof(nop5));
        break;
      }
      if (value == 0 && is_jcc) {
        static constexpr u8 nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
        std::memcpy(dst_ptr - 2, nop6, sizeof(nop6));
        break;
      }
      std::memcpy(dst_ptr, &value, sizeof(u32));
      break;
    }
    default: TPDE_UNREACHABLE("unexpected label fixup kind");
    }
  }

  // TODO: move jump tables to read-only data section.
  for (JumpTable *jt : jump_tables) {
    align(4);
    ensure_space(jt->size * sizeof(i32));
    u32 code_off = jt->off - label_skew;
    u32 table_off = offset();
    {
      FeRegGP idx = FE_GP(jt->idx.id());
      FeRegGP tmp = FE_GP(jt->tmp.id());
      u8 *start = begin_ptr() + code_off;
      u8 *write_ptr = start;
      auto table_mem = FE_MEM(FE_IP, 0, FE_NOREG, i32(cur_ptr() - write_ptr));
      write_ptr += fe64_LEA64rm(write_ptr, 0, tmp, table_mem);
      write_ptr += fe64_MOV32rm(write_ptr, 0, idx, FE_MEM(tmp, 4, idx, 0));
      write_ptr += fe64_SUB64rr(write_ptr, 0, tmp, idx);
      write_ptr += fe64_JMPr(write_ptr, 0, tmp);
      if (write_ptr != start + JumpTableCodeSize) {
        assert(write_ptr < start + JumpTableCodeSize);
        fe64_NOP(write_ptr, (start + JumpTableCodeSize) - write_ptr);
      }
    }
    for (Label label : jt->labels()) {
      assert(label_offset(label) < table_off);
      write_unchecked<i32>(table_off - label_offset(label));
    }
  }
}

} // end namespace tpde::x64
