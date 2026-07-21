// SPDX-License-Identifier: PHP-3.01
#pragma once

#include <tpde/Assembler.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace zend::native::tpde {

class AssemblerDarwinA64 final : public ::tpde::Assembler {
public:
	enum : uint32_t {
		SECTION_ALLOC = 1U << 0,
		SECTION_WRITE = 1U << 1,
		SECTION_EXEC = 1U << 2,
	};

	enum class SymbolKind : uint8_t {
		Unknown,
		Function,
		Data,
		TLS,
		Section,
	};

	struct Symbol {
		std::string name;
		SymBinding binding = SymBinding::LOCAL;
		SymbolKind kind = SymbolKind::Unknown;
		::tpde::SecRef section;
		uint64_t offset = 0;
		uint64_t size = 0;
		bool defined = false;
	};

private:
	std::vector<Symbol> symbols_;
	std::vector<std::string> section_names_;
	std::vector<::tpde::SymRef> section_symbols_;

	static const TargetInfo &darwin_target_info();
	::tpde::SymRef add_symbol(
		std::string_view name, SymBinding binding, SymbolKind kind);

public:
	AssemblerDarwinA64();
	void reset() override;
	void rename_section(::tpde::SecRef section, std::string_view name) override;
	::tpde::SymRef section_symbol(::tpde::SecRef section) override;
	::tpde::SymRef sym_add_undef(
		std::string_view name, SymBinding binding) override;
	::tpde::SymRef sym_predef_func(
		std::string_view name, SymBinding binding) override;
	::tpde::SymRef sym_predef_data(
		std::string_view name, SymBinding binding) override;
	::tpde::SymRef sym_predef_tls(
		std::string_view name, SymBinding binding) override;
	void sym_def(::tpde::SymRef symbol, ::tpde::SecRef section,
		uint64_t position, uint64_t size) override;
	std::vector<::tpde::u8> build_object_file() override;

	const Symbol &symbol(::tpde::SymRef reference) const {
		return symbols_[reference.id()];
	}
	size_t symbol_count() const { return symbols_.size(); }
	std::string_view section_name(::tpde::SecRef reference) const {
		return section_names_[reference.id()];
	}
};

} // namespace zend::native::tpde
