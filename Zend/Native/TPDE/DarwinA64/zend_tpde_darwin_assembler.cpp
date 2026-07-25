// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_assembler.hpp"

#include <tpde/ELF.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace zend::native::tpde {

namespace {

using Flags = ::tpde::Assembler::TargetInfo::SectionFlags;

constexpr Flags section_flags(
	uint32_t type, uint32_t flags, uint8_t alignment,
	bool relocations = true, bool bss = false) {
	return Flags{
		.type = type,
		.flags = flags,
		.name = 0,
		.align = alignment,
		.has_relocs = relocations,
		.is_bss = bss};
}

const ::tpde::Assembler::TargetInfo TARGET_INFO{
	.reloc_pc32 = ::tpde::elf::R_AARCH64_PREL32,
	.reloc_abs64 = ::tpde::elf::R_AARCH64_ABS64,
	.section_flags = {
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC |
			AssemblerDarwinA64::SECTION_EXEC, 16),
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC, 16),
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC, 8),
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC, 4),
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC |
			AssemblerDarwinA64::SECTION_WRITE, 16),
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC, 16),
		section_flags(2, AssemblerDarwinA64::SECTION_ALLOC |
			AssemblerDarwinA64::SECTION_WRITE, 16, false, true),
		section_flags(1, AssemblerDarwinA64::SECTION_ALLOC |
			AssemblerDarwinA64::SECTION_WRITE, 16),
		section_flags(2, AssemblerDarwinA64::SECTION_ALLOC |
			AssemblerDarwinA64::SECTION_WRITE, 16, false, true),
	}};

template <typename T>
void append(std::vector<::tpde::u8> &output, const T &value) {
	size_t offset = output.size();
	output.resize(offset + sizeof(value));
	std::memcpy(output.data() + offset, &value, sizeof(value));
}

void append_bytes(std::vector<::tpde::u8> &output,
	const void *bytes, size_t size) {
	size_t offset = output.size();
	output.resize(offset + size);
	if (size != 0) {
		std::memcpy(output.data() + offset, bytes, size);
	}
}

} // namespace

const ::tpde::Assembler::TargetInfo &AssemblerDarwinA64::darwin_target_info() {
	return TARGET_INFO;
}

AssemblerDarwinA64::AssemblerDarwinA64() : Assembler(darwin_target_info()) {
	reset();
}

void AssemblerDarwinA64::reset() {
	Assembler::reset();
	sections.emplace_back(nullptr);
	symbols_.clear();
	symbols_.emplace_back();
	section_names_.assign(1, {});
	section_symbols_.assign(1, {});
}

void AssemblerDarwinA64::rename_section(
	::tpde::SecRef section, std::string_view name) {
	if (section_names_.size() < section_count()) {
		section_names_.resize(section_count());
		section_symbols_.resize(section_count());
	}
	section_names_[section.id()] = name;
}

::tpde::SymRef AssemblerDarwinA64::add_symbol(
	std::string_view name, SymBinding binding, SymbolKind kind) {
	symbols_.push_back(Symbol{
		std::string{name},
		binding,
		kind,
		::tpde::SecRef{},
		0,
		0,
		false});
	return ::tpde::SymRef{static_cast<uint32_t>(symbols_.size() - 1)};
}

::tpde::SymRef AssemblerDarwinA64::section_symbol(::tpde::SecRef section) {
	if (section_symbols_.size() < section_count()) {
		section_names_.resize(section_count());
		section_symbols_.resize(section_count());
	}
	::tpde::SymRef &symbol_ref = section_symbols_[section.id()];
	if (!symbol_ref.valid()) {
		symbol_ref = add_symbol(section_names_[section.id()],
			SymBinding::LOCAL, SymbolKind::Section);
		sym_def(symbol_ref, section, 0, get_section(section).size());
	}
	return symbol_ref;
}

::tpde::SymRef AssemblerDarwinA64::sym_add_undef(
	std::string_view name, SymBinding binding) {
	return add_symbol(name, binding, SymbolKind::Unknown);
}

::tpde::SymRef AssemblerDarwinA64::sym_predef_func(
	std::string_view name, SymBinding binding) {
	return add_symbol(name, binding, SymbolKind::Function);
}

::tpde::SymRef AssemblerDarwinA64::sym_predef_data(
	std::string_view name, SymBinding binding) {
	return add_symbol(name, binding, SymbolKind::Data);
}

::tpde::SymRef AssemblerDarwinA64::sym_predef_tls(
	std::string_view name, SymBinding binding) {
	return add_symbol(name, binding, SymbolKind::TLS);
}

void AssemblerDarwinA64::sym_def(::tpde::SymRef symbol,
	::tpde::SecRef section, uint64_t position, uint64_t size) {
	Symbol &definition = symbols_[symbol.id()];
	definition.section = section;
	definition.offset = position;
	definition.size = size;
	definition.defined = true;
}

std::vector<::tpde::u8> AssemblerDarwinA64::build_object_file() {
	static constexpr std::array<::tpde::u8, 16> MAGIC{
		'Z', 'N', 'M', 'I', 'R', '-', 'T', 'P', 'D', 'E', '-', 'A', '6', '4', 0, 2};
	std::vector<::tpde::u8> output(MAGIC.begin(), MAGIC.end());
	uint32_t sections_count = static_cast<uint32_t>(section_count());
	uint32_t present_sections_count = 0;
	for (uint32_t i = 1; i < sections_count; ++i) {
		present_sections_count += section_present(i) ? 1 : 0;
	}
	uint32_t symbols_count = static_cast<uint32_t>(symbol_count());
	uint32_t unwind_section =
		default_sections[static_cast<unsigned>(::tpde::SectionKind::EHFrame)].id();
	append(output, sections_count);
	append(output, present_sections_count);
	append(output, symbols_count);
	append(output, unwind_section);
	for (uint32_t i = 1; i < sections_count; ++i) {
		if (!section_present(i)) {
			continue;
		}
		const ::tpde::DataSection &section = get_section(::tpde::SecRef{i});
		append(output, i);
		append(output, section.type);
		append(output, section.flags);
		append(output, section.align);
		uint64_t size = section.size();
		append(output, size);
		uint32_t name_size =
			static_cast<uint32_t>(section_names_[i].size());
		append(output, name_size);
		uint32_t relocation_count = section.is_virtual
			? 0 : static_cast<uint32_t>(section.relocations().size());
		append(output, relocation_count);
		uint8_t is_virtual = section.is_virtual ? 1 : 0;
		append(output, is_virtual);
		const std::array<uint8_t, 7> section_reserved{};
		append_bytes(output, section_reserved.data(), section_reserved.size());
		append_bytes(output, section_names_[i].data(), name_size);
		if (!section.is_virtual) {
			append_bytes(output, section.data.data(), section.data.size());
			for (const ::tpde::Relocation &relocation : section.relocations()) {
				append(output, relocation.offset);
				uint32_t symbol = relocation.symbol.id();
				append(output, symbol);
				append(output, relocation.type);
				append(output, relocation.addend);
			}
		}
	}
	for (const Symbol &symbol : symbols_) {
		uint32_t name_size = static_cast<uint32_t>(symbol.name.size());
		append(output, name_size);
		uint32_t section = symbol.section.id();
		append(output, section);
		append(output, symbol.offset);
		append(output, symbol.size);
		uint8_t binding = static_cast<uint8_t>(symbol.binding);
		uint8_t kind = static_cast<uint8_t>(symbol.kind);
		uint8_t defined = symbol.defined ? 1 : 0;
		append(output, binding);
		append(output, kind);
		append(output, defined);
		const std::array<uint8_t, 5> symbol_reserved{};
		append_bytes(output, symbol_reserved.data(), symbol_reserved.size());
		append_bytes(output, symbol.name.data(), name_size);
	}
	return output;
}

} // namespace zend::native::tpde
