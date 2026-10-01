// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde/x64/FunctionWriterX64.hpp"
#include "fadec-enc2.h"

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
