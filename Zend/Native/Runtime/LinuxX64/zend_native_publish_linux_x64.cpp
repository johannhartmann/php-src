// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#include <tpde/ELF.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
# include <sys/mman.h>
# include <unistd.h>

extern "C" void __register_frame(void *);
extern "C" void __deregister_frame(void *);
extern "C" void __unw_add_dynamic_eh_frame_section(uintptr_t)
	__attribute__((weak));
extern "C" void __unw_remove_dynamic_eh_frame_section(uintptr_t)
	__attribute__((weak));

namespace {

using namespace tpde::elf;

constexpr size_t ZEND_NATIVE_PLT_ENTRY_SIZE = 16;
constexpr size_t ZEND_NATIVE_NO_OFFSET = std::numeric_limits<size_t>::max();

struct LinuxX64PublishedState {
	unsigned char *mapping = nullptr;
	size_t mapping_size = 0;
	void *eh_frame = nullptr;
	bool unwind_registered = false;
};

struct AllocSection {
	uint32_t index;
	uint32_t permission_key;
};

struct PermissionBoundary {
	size_t offset;
	uint64_t flags;
};

bool checked_range(size_t size, uint64_t offset, uint64_t length) {
	return offset <= size && length <= size - static_cast<size_t>(offset);
}

bool checked_align(size_t value, size_t alignment, size_t *out) {
	if (alignment == 0) {
		alignment = 1;
	}
	if ((alignment & (alignment - 1)) != 0) {
		return false;
	}
	const size_t mask = alignment - 1;
	if (value > std::numeric_limits<size_t>::max() - mask) {
		return false;
	}
	*out = (value + mask) & ~mask;
	return true;
}

const char *string_at(
	const unsigned char *data, size_t size, uint32_t offset) {
	if (offset >= size) {
		return nullptr;
	}
	const char *value = reinterpret_cast<const char *>(data + offset);
	return std::memchr(value, '\0', size - offset) != nullptr ? value : nullptr;
}

void register_eh_frame(void *eh_frame) {
	if (__unw_add_dynamic_eh_frame_section != nullptr) {
		__unw_add_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(eh_frame));
	} else {
		__register_frame(eh_frame);
	}
}

void deregister_eh_frame(void *eh_frame) {
	if (__unw_remove_dynamic_eh_frame_section != nullptr) {
		__unw_remove_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(eh_frame));
	} else {
		__deregister_frame(eh_frame);
	}
}

void destroy_linux_x64_published_state(void *opaque) {
	auto *state = static_cast<LinuxX64PublishedState *>(opaque);
	if (state == nullptr) {
		return;
	}
	if (state->unwind_registered) {
		deregister_eh_frame(state->eh_frame);
	}
	if (state->mapping != nullptr) {
		::munmap(state->mapping, state->mapping_size);
	}
	delete state;
}

bool signed_32_displacement(uintptr_t target, int64_t addend, uintptr_t pc,
	uint32_t *out) {
	const __int128 displacement =
		static_cast<__int128>(target) + addend - pc;
	if (displacement < INT32_MIN || displacement > INT32_MAX) {
		return false;
	}
	*out = static_cast<uint32_t>(static_cast<int32_t>(displacement));
	return true;
}

zend_result map_linux_x64_object(
	const zend_native_image *image,
	zend_native_code *code,
	zend_native_diagnostic *diag) {
	const unsigned char *object = image->text;
	const size_t object_size = image->text_size;
	if (object == nullptr || object_size < sizeof(Elf64_Ehdr)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image does not contain an ELF object");
		return FAILURE;
	}

	const auto *header = reinterpret_cast<const Elf64_Ehdr *>(object);
	if (std::memcmp(header->e_ident, ELFMAG, 4) != 0
			|| header->e_ident[EI_CLASS] != ELFCLASS64
			|| header->e_ident[EI_DATA] != ELFDATA2LSB
			|| header->e_ident[EI_VERSION] != EV_CURRENT
			|| header->e_type != ET_REL
			|| header->e_machine != EM_X86_64
			|| header->e_version != EV_CURRENT
			|| header->e_ehsize != sizeof(Elf64_Ehdr)
			|| header->e_shentsize != sizeof(Elf64_Shdr)
			|| header->e_shnum == 0
			|| !checked_range(object_size, header->e_shoff,
				static_cast<uint64_t>(header->e_shnum)
					* sizeof(Elf64_Shdr))) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has an invalid ELF header");
		return FAILURE;
	}

	const auto *sections = reinterpret_cast<const Elf64_Shdr *>(
		object + header->e_shoff);
	if (header->e_shstrndx >= header->e_shnum) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has no valid section-name table");
		return FAILURE;
	}
	const Elf64_Shdr &shstr_section = sections[header->e_shstrndx];
	if (shstr_section.sh_type != SHT_STRTAB
			|| !checked_range(object_size, shstr_section.sh_offset,
				shstr_section.sh_size)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has an invalid section-name table");
		return FAILURE;
	}
	const unsigned char *section_names = object + shstr_section.sh_offset;
	const size_t section_names_size = shstr_section.sh_size;

	uint32_t symbol_section_index = UINT32_MAX;
	std::vector<AllocSection> alloc_sections;
	for (uint32_t i = 0; i < header->e_shnum; ++i) {
		const Elf64_Shdr &section = sections[i];
		if (section.sh_type != SHT_NOBITS
				&& !checked_range(
					object_size, section.sh_offset, section.sh_size)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Linux native image contains an out-of-range section");
			return FAILURE;
		}
		if (section.sh_type == SHT_SYMTAB) {
			if (symbol_section_index != UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Linux native image contains multiple symbol tables");
				return FAILURE;
			}
			symbol_section_index = i;
		}
		if ((section.sh_flags & SHF_ALLOC) != 0) {
			if ((section.sh_flags & (SHF_WRITE | SHF_EXECINSTR))
					== (SHF_WRITE | SHF_EXECINSTR)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Linux native image requests writable executable memory");
				return FAILURE;
			}
			uint32_t key = (section.sh_flags & SHF_EXECINSTR) ? 0 : 4;
			key |= (section.sh_flags & SHF_WRITE) ? 2 : 0;
			key |= section.sh_type == SHT_NOBITS ? 1 : 0;
			alloc_sections.push_back({i, key});
		}
	}
	if (symbol_section_index == UINT32_MAX) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has no symbol table");
		return FAILURE;
	}
	std::stable_sort(alloc_sections.begin(), alloc_sections.end(),
		[](const AllocSection &left, const AllocSection &right) {
			return left.permission_key < right.permission_key;
		});

	const Elf64_Shdr &symbol_section = sections[symbol_section_index];
	if (symbol_section.sh_entsize != sizeof(Elf64_Sym)
			|| symbol_section.sh_size % sizeof(Elf64_Sym) != 0
			|| symbol_section.sh_link >= header->e_shnum) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has an invalid symbol table");
		return FAILURE;
	}
	const Elf64_Shdr &string_section = sections[symbol_section.sh_link];
	if (string_section.sh_type != SHT_STRTAB
			|| !checked_range(object_size, string_section.sh_offset,
				string_section.sh_size)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has an invalid symbol string table");
		return FAILURE;
	}
	const auto *symbols = reinterpret_cast<const Elf64_Sym *>(
		object + symbol_section.sh_offset);
	const size_t symbol_count =
		symbol_section.sh_size / sizeof(Elf64_Sym);
	const unsigned char *symbol_names = object + string_section.sh_offset;
	const size_t symbol_names_size = string_section.sh_size;
	if (symbol_count >
			std::numeric_limits<size_t>::max() / ZEND_NATIVE_PLT_ENTRY_SIZE) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image symbol table is too large");
		return FAILURE;
	}

	const long system_page_size = ::sysconf(_SC_PAGESIZE);
	const size_t page_size =
		system_page_size > 0 ? static_cast<size_t>(system_page_size) : 4096;
	std::vector<size_t> section_offsets(
		header->e_shnum, ZEND_NATIVE_NO_OFFSET);
	std::vector<PermissionBoundary> permission_boundaries;
	size_t mapped_size = symbol_count * ZEND_NATIVE_PLT_ENTRY_SIZE;
	uint64_t previous_permissions = SHF_ALLOC | SHF_EXECINSTR;
	if (mapped_size != 0) {
		permission_boundaries.push_back({0, previous_permissions});
	}
	uint32_t eh_frame_section = UINT32_MAX;
	for (const AllocSection &allocated : alloc_sections) {
		const Elf64_Shdr &section = sections[allocated.index];
		const uint64_t permissions =
			section.sh_flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR);
		const char *name =
			string_at(section_names, section_names_size, section.sh_name);
		if (name == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Linux native image contains an invalid section name");
			return FAILURE;
		}
		if (permissions != previous_permissions) {
			if (!checked_align(mapped_size, page_size, &mapped_size)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Linux native image mapping size overflow");
				return FAILURE;
			}
			permission_boundaries.push_back({mapped_size, permissions});
			previous_permissions = permissions;
		} else if (!checked_align(mapped_size,
					   static_cast<size_t>(
						   std::max<uint64_t>(section.sh_addralign, 1)),
					   &mapped_size)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Linux native image section alignment overflow");
			return FAILURE;
		}
		section_offsets[allocated.index] = mapped_size;
		size_t section_size = static_cast<size_t>(section.sh_size);
		if (std::strcmp(name, ".eh_frame") == 0) {
			eh_frame_section = allocated.index;
			if (section_size > std::numeric_limits<size_t>::max() - 4) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Linux native image unwind section is too large");
				return FAILURE;
			}
			section_size += 4;
		}
		if (mapped_size > std::numeric_limits<size_t>::max() - section_size) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Linux native image mapping size overflow");
			return FAILURE;
		}
		mapped_size += section_size;
	}
	if (mapped_size == 0
			|| !checked_align(mapped_size, page_size, &mapped_size)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image has no allocatable contents");
		return FAILURE;
	}
	permission_boundaries.push_back({mapped_size, 0});

	auto *state = new (std::nothrow) LinuxX64PublishedState();
	if (state == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Linux mapping state");
		return FAILURE;
	}
	state->mapping = static_cast<unsigned char *>(::mmap(nullptr, mapped_size,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (state->mapping == MAP_FAILED) {
		state->mapping = nullptr;
		delete state;
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"unable to allocate Linux native mapping");
		return FAILURE;
	}
	state->mapping_size = mapped_size;

	std::vector<uintptr_t> symbol_addresses(symbol_count, 0);
	std::vector<unsigned char> symbol_resolved(symbol_count, 0);
	bool success = true;
	auto resolve_symbol = [&](size_t index) -> uintptr_t {
		if (index >= symbol_count) {
			success = false;
			return 0;
		}
		if (symbol_resolved[index]) {
			return symbol_addresses[index];
		}
		symbol_resolved[index] = 1;
		const Elf64_Sym &symbol = symbols[index];
		const char *name =
			string_at(symbol_names, symbol_names_size, symbol.st_name);
		if (name == nullptr) {
			success = false;
			return 0;
		}
		uintptr_t address = 0;
		if (symbol.st_shndx == SHN_UNDEF) {
			const void *resolved = nullptr;
			if (!zend_tpde_image_resolve_symbol(image, name, &resolved)
					&& symbol.st_bind() != STB_WEAK) {
				success = false;
			}
			address = reinterpret_cast<uintptr_t>(resolved);
		} else if (symbol.st_shndx == SHN_ABS) {
			address = static_cast<uintptr_t>(symbol.st_value);
		} else if (symbol.st_shndx < header->e_shnum
				&& section_offsets[symbol.st_shndx]
					!= ZEND_NATIVE_NO_OFFSET
				&& symbol.st_value
					<= sections[symbol.st_shndx].sh_size) {
			address = reinterpret_cast<uintptr_t>(state->mapping)
				+ section_offsets[symbol.st_shndx]
				+ static_cast<size_t>(symbol.st_value);
		} else {
			success = false;
		}
		symbol_addresses[index] = address;
		return address;
	};

	std::vector<unsigned char *> plt_slots(symbol_count, nullptr);
	unsigned char *next_plt = state->mapping;
	auto plt_entry = [&](size_t index, uintptr_t address) -> uintptr_t {
		if (index >= symbol_count) {
			success = false;
			return 0;
		}
		if (plt_slots[index] == nullptr) {
			unsigned char *slot = next_plt;
			slot[0] = 0xff;
			slot[1] = 0x25;
			slot[2] = 0x02;
			slot[3] = 0x00;
			slot[4] = 0x00;
			slot[5] = 0x00;
			slot[6] = 0x0f;
			slot[7] = 0x0b;
			std::memcpy(slot + 8, &address, sizeof(address));
			plt_slots[index] = slot;
			next_plt += ZEND_NATIVE_PLT_ENTRY_SIZE;
		}
		return reinterpret_cast<uintptr_t>(plt_slots[index]);
	};

	for (const AllocSection &allocated : alloc_sections) {
		const Elf64_Shdr &section = sections[allocated.index];
		if (section.sh_type != SHT_NOBITS && section.sh_size != 0) {
			std::memcpy(state->mapping + section_offsets[allocated.index],
				object + section.sh_offset,
				static_cast<size_t>(section.sh_size));
		}
	}

	for (uint32_t i = 0; i < header->e_shnum && success; ++i) {
		const Elf64_Shdr &relocation_section = sections[i];
		if (relocation_section.sh_type != SHT_RELA) {
			continue;
		}
		if (relocation_section.sh_link != symbol_section_index
				|| relocation_section.sh_info >= header->e_shnum
				|| relocation_section.sh_entsize != sizeof(Elf64_Rela)
				|| relocation_section.sh_size % sizeof(Elf64_Rela) != 0) {
			success = false;
			break;
		}
		const size_t target_offset =
			section_offsets[relocation_section.sh_info];
		if (target_offset == ZEND_NATIVE_NO_OFFSET) {
			continue;
		}
		const Elf64_Shdr &target_section =
			sections[relocation_section.sh_info];
		const auto *relocations = reinterpret_cast<const Elf64_Rela *>(
			object + relocation_section.sh_offset);
		const size_t relocation_count =
			relocation_section.sh_size / sizeof(Elf64_Rela);
		for (size_t relocation_index = 0;
				relocation_index < relocation_count && success;
				++relocation_index) {
			const Elf64_Rela &relocation = relocations[relocation_index];
			const uint32_t symbol_index =
				static_cast<uint32_t>(relocation.r_info >> 32);
			const uint32_t type =
				static_cast<uint32_t>(relocation.r_info);
			const size_t width = type == R_X86_64_64 ? 8 : 4;
			if (symbol_index >= symbol_count
					|| relocation.r_offset > target_section.sh_size
					|| width > target_section.sh_size
						- relocation.r_offset) {
				success = false;
				break;
			}
			const uintptr_t symbol = resolve_symbol(symbol_index);
			const uintptr_t pc = reinterpret_cast<uintptr_t>(state->mapping)
				+ target_offset
				+ static_cast<size_t>(relocation.r_offset);
			if (!success) {
				break;
			}
			if (type == R_X86_64_64) {
				const uint64_t value = static_cast<uint64_t>(
					static_cast<__int128>(symbol)
					+ relocation.r_addend);
				std::memcpy(reinterpret_cast<void *>(pc), &value,
					sizeof(value));
				continue;
			}
			uintptr_t target = symbol;
			if (type == R_X86_64_PLT32) {
				uint32_t direct_displacement;
				if (!signed_32_displacement(symbol, relocation.r_addend, pc,
						&direct_displacement)) {
					target = plt_entry(symbol_index, symbol);
				}
			} else if (type == R_X86_64_GOTPCREL) {
				target = plt_entry(symbol_index, symbol) + 8;
			} else if (type != R_X86_64_PC32) {
				success = false;
				break;
			}
			uint32_t displacement;
			if (!signed_32_displacement(
					target, relocation.r_addend, pc, &displacement)) {
				success = false;
				break;
			}
			std::memcpy(reinterpret_cast<void *>(pc), &displacement,
				sizeof(displacement));
		}
	}

	void *entry = nullptr;
	for (size_t i = 0; i < symbol_count && success; ++i) {
		const char *name =
			string_at(symbol_names, symbol_names_size, symbols[i].st_name);
		if (name != nullptr && std::strcmp(name, "zend_native_entry") == 0
				&& symbols[i].st_shndx != SHN_UNDEF) {
			entry = reinterpret_cast<void *>(resolve_symbol(i));
			break;
		}
	}
	if (!success || entry == nullptr) {
		destroy_linux_x64_published_state(state);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image contains unresolved symbols or relocations");
		return FAILURE;
	}

	for (size_t i = 0; i + 1 < permission_boundaries.size(); ++i) {
		const PermissionBoundary &current = permission_boundaries[i];
		const size_t length =
			permission_boundaries[i + 1].offset - current.offset;
		if (length == 0) {
			continue;
		}
		int protection = PROT_READ;
		if ((current.flags & SHF_EXECINSTR) != 0) {
			protection |= PROT_EXEC;
		}
		if ((current.flags & SHF_WRITE) != 0) {
			protection |= PROT_WRITE;
		}
		if (::mprotect(state->mapping + current.offset, length, protection)
				!= 0) {
			destroy_linux_x64_published_state(state);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"unable to apply Linux native mapping permissions");
			return FAILURE;
		}
	}

	if (eh_frame_section != UINT32_MAX) {
		state->eh_frame =
			state->mapping + section_offsets[eh_frame_section];
		register_eh_frame(state->eh_frame);
		state->unwind_registered = true;
	}
	code->mapping = state->mapping;
	code->mapping_size = state->mapping_size;
	code->entry = reinterpret_cast<zend_native_frame_entry_t>(entry);
	code->unwind_registered = state->unwind_registered;
	code->target_state = state;
	code->destroy_target_state = destroy_linux_x64_published_state;
	return SUCCESS;
}

} // namespace
#endif

zend_result zend_native_publish_linux_x64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
#if defined(__linux__) && defined(__x86_64__)
	if (image == nullptr || image->target != ZEND_NATIVE_TARGET_LINUX_AMD64
			|| image->text == nullptr || image->text_size == 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Linux x86-64 publisher requires a non-empty Linux image");
		return FAILURE;
	}
	if (zend_native_runtime_validate(zend_native_runtime_get(),
			ZEND_NATIVE_RUNTIME_CAP_BAILOUT_BOUNDARY, diag) == FAILURE) {
		return FAILURE;
	}
	zend_native_code *code = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*code)));
	if (code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Linux native-code state");
		return FAILURE;
	}
	code->target = ZEND_NATIVE_TARGET_LINUX_AMD64;
	code->slot_count = image->slot_count;
	code->argument_count = image->argument_count;
	code->frame_variable_count = image->frame_variable_count;
	code->frame_temporary_count = image->frame_temporary_count;
	if (map_linux_x64_object(image, code, diag) == FAILURE) {
		std::free(code);
		return FAILURE;
	}
	code->writable = false;
	code->executable = true;
	*out_code = code;
	return SUCCESS;
#else
	(void) image;
	(void) out_code;
	zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
		"linux-amd64-prod publication requires native Linux x86-64");
	return FAILURE;
#endif
}
void zend_native_unmap_linux_x64(zend_native_code *code) {
#if defined(__linux__) && defined(__x86_64__)
	if (code->destroy_target_state != nullptr) {
		code->destroy_target_state(code->target_state);
		code->target_state = nullptr;
	}
#else
	(void) code;
#endif
}
