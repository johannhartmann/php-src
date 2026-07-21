// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_assembler.hpp"

#include <tpde/arm64/CompilerA64.hpp>

#include <algorithm>

namespace zend::native::tpde {

/*
 * Apple arm64 follows AAPCS64 with the Darwin ABI differences that matter to
 * TPDE: x18 is reserved, register arguments are not skipped to obtain even
 * register numbers, and stack arguments use their natural size/alignment.
 *
 * The current native entry point has the fixed signature
 *
 *   void entry(const scalar *arguments, uint64_t *reserved, scalar *result)
 *
 * but this is a complete assigner rather than a three-register assertion so
 * the compiler and prologue share one ABI model.
 */
class CCAssignerAppleA64 final : public ::tpde::CCAssigner {
	using AsmReg = ::tpde::a64::AsmReg;

	static constexpr ::tpde::CCInfo Info{
		.allocatable_regs =
			UINT64_MAX & ~::tpde::a64::create_bitmask({
				AsmReg::SP, AsmReg::FP, AsmReg::R16, AsmReg::R17,
				AsmReg::R18}),
		.callee_saved_regs = ::tpde::a64::create_bitmask({
			AsmReg::R19, AsmReg::R20, AsmReg::R21, AsmReg::R22,
			AsmReg::R23, AsmReg::R24, AsmReg::R25, AsmReg::R26,
			AsmReg::R27, AsmReg::R28,
			AsmReg::V8, AsmReg::V9, AsmReg::V10, AsmReg::V11,
			AsmReg::V12, AsmReg::V13, AsmReg::V14, AsmReg::V15}),
		.arg_regs = ::tpde::a64::create_bitmask({
			AsmReg::R0, AsmReg::R1, AsmReg::R2, AsmReg::R3,
			AsmReg::R4, AsmReg::R5, AsmReg::R6, AsmReg::R7,
			AsmReg::R8,
			AsmReg::V0, AsmReg::V1, AsmReg::V2, AsmReg::V3,
			AsmReg::V4, AsmReg::V5, AsmReg::V6, AsmReg::V7}),
	};

	uint32_t ngrn_ = 0;
	uint32_t nsrn_ = 0;
	uint32_t nsaa_ = 0;
	uint32_t ret_ngrn_ = 0;
	uint32_t ret_nsrn_ = 0;

	static uint32_t natural_alignment(const ::tpde::CCAssignment &arg) {
		return std::max<uint32_t>(1, std::min<uint32_t>(arg.align, 16));
	}

public:
	CCAssignerAppleA64() : CCAssigner(Info) {}

	void reset() override {
		ngrn_ = nsrn_ = nsaa_ = ret_ngrn_ = ret_nsrn_ = 0;
	}

	void assign_arg(::tpde::CCAssignment &arg) override {
		if (arg.byval) {
			nsaa_ = ::tpde::util::align_up(nsaa_, natural_alignment(arg));
			arg.stack_off = nsaa_;
			nsaa_ += arg.size;
			return;
		}
		if (arg.sret) {
			arg.reg = AsmReg{AsmReg::R8};
			return;
		}
		if (arg.bank == ::tpde::RegBank{0}) {
			if (ngrn_ + arg.consecutive < 8) {
				arg.reg = ::tpde::Reg{AsmReg::R0 + ngrn_++};
				return;
			}
			ngrn_ = 8;
		} else {
			if (nsrn_ + arg.consecutive < 8) {
				arg.reg = ::tpde::Reg{AsmReg::V0 + nsrn_++};
				return;
			}
			nsrn_ = 8;
		}
		nsaa_ = ::tpde::util::align_up(nsaa_, natural_alignment(arg));
		arg.stack_off = nsaa_;
		nsaa_ += arg.size;
	}

	uint32_t get_stack_size() override { return nsaa_; }

	void assign_ret(::tpde::CCAssignment &arg) override {
		assert(!arg.byval && !arg.sret);
		if (arg.bank == ::tpde::RegBank{0}) {
			assert(ret_ngrn_ + arg.consecutive < 8);
			arg.reg = ::tpde::Reg{AsmReg::R0 + ret_ngrn_++};
		} else {
			assert(ret_nsrn_ + arg.consecutive < 8);
			arg.reg = ::tpde::Reg{AsmReg::V0 + ret_nsrn_++};
		}
	}

	static constexpr bool register_is_allocatable(uint32_t reg) {
		return (Info.allocatable_regs & (UINT64_C(1) << reg)) != 0;
	}
};

struct DarwinA64PlatformConfig : ::tpde::CompilerConfigDefault {
	using Assembler = AssemblerDarwinA64;
	using AsmReg = ::tpde::a64::AsmReg;
	using DefaultCCAssigner = CCAssignerAppleA64;
	using FunctionWriter = ::tpde::a64::FunctionWriterA64;

	static constexpr ::tpde::RegBank GP_BANK{0};
	static constexpr ::tpde::RegBank FP_BANK{1};
	static constexpr bool FRAME_INDEXING_NEGATIVE = false;
	static constexpr uint32_t PLATFORM_POINTER_SIZE = 8;
	static constexpr uint32_t NUM_BANKS = 2;
};

static_assert(!CCAssignerAppleA64::register_is_allocatable(18));
static_assert(CCAssignerAppleA64::register_is_allocatable(0));
static_assert(::tpde::CompilerConfig<DarwinA64PlatformConfig>);

} // namespace zend::native::tpde
