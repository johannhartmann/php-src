// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_darwin_assembler.hpp"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#include <tpde/ELF.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
# include <libkern/OSCacheControl.h>
# include <pthread.h>
# include <sys/mman.h>
# include <unistd.h>

extern "C" void __register_frame(void *);
extern "C" void __deregister_frame(void *);
extern "C" void __unw_add_dynamic_eh_frame_section(uintptr_t)
	__attribute__((weak_import));
extern "C" void __unw_remove_dynamic_eh_frame_section(uintptr_t)
	__attribute__((weak_import));
#endif

namespace {

using DarwinAssembler = zend::native::tpde::AssemblerDarwinA64;

#if defined(__APPLE__) && defined(__aarch64__)

struct MappedSection {
	void *mapping = nullptr;
	size_t mapping_size = 0;
	uint32_t flags = 0;
};

/* Without __unw_add_dynamic_eh_frame_section, Darwin's libunwind takes one
 * FDE per __register_frame call: visit each FDE of the .eh_frame section. */
template <typename Visit>
void for_each_eh_frame_fde(void *section, size_t size, Visit visit) {
	uint8_t *cursor = static_cast<uint8_t *>(section);
	uint8_t *const end = cursor + size;

	while (end - cursor >= 8) {
		uint32_t length;
		uint32_t cie_id;
		std::memcpy(&length, cursor, sizeof(length));
		if (length == 0 || length == UINT32_MAX
				|| length > static_cast<size_t>(end - cursor) - 4) {
			break;
		}
		std::memcpy(&cie_id, cursor + 4, sizeof(cie_id));
		if (cie_id != 0) {
			visit(cursor);
		}
		cursor += 4 + static_cast<size_t>(length);
	}
}

struct A64PublishedState {
	std::vector<MappedSection> sections;
	void *unwind_section = nullptr;
	size_t unwind_section_size = 0;
	bool unwind_registered = false;

	~A64PublishedState() {
		if (unwind_registered) {
			if (__unw_remove_dynamic_eh_frame_section != nullptr) {
				__unw_remove_dynamic_eh_frame_section(
					reinterpret_cast<uintptr_t>(unwind_section));
			} else {
				for_each_eh_frame_fde(unwind_section, unwind_section_size,
					[](void *fde) { __deregister_frame(fde); });
			}
			unwind_registered = false;
		}
		for (const MappedSection &section : sections) {
			if (section.mapping != nullptr) {
				munmap(section.mapping, section.mapping_size);
			}
		}
	}
};

void destroy_a64_published_state(void *state) {
	delete static_cast<A64PublishedState *>(state);
}

bool signed_range(int64_t value, unsigned bits) {
	const int64_t minimum = -(INT64_C(1) << (bits - 1));
	const int64_t maximum = (INT64_C(1) << (bits - 1)) - 1;
	return value >= minimum && value <= maximum;
}

bool apply_relocation(uint8_t *location, uint32_t type,
	uintptr_t symbol, int32_t addend) {
	using namespace ::tpde::elf;
	const uintptr_t place = reinterpret_cast<uintptr_t>(location);
	const uintptr_t target = symbol + static_cast<intptr_t>(addend);
	auto load32 = [&]() {
		uint32_t value;
		std::memcpy(&value, location, sizeof(value));
		return value;
	};
	auto store32 = [&](uint32_t value) {
		std::memcpy(location, &value, sizeof(value));
	};
	switch (type) {
		case R_AARCH64_ABS64:
			std::memcpy(location, &target, sizeof(target));
			return true;
		case R_AARCH64_PREL32: {
			int64_t delta = static_cast<int64_t>(target - place);
			if (!signed_range(delta, 32)) return false;
			int32_t value = static_cast<int32_t>(delta);
			std::memcpy(location, &value, sizeof(value));
			return true;
		}
		case R_AARCH64_JUMP26:
		case R_AARCH64_CALL26: {
			int64_t delta = static_cast<int64_t>(target - place);
			if ((delta & 3) != 0 || !signed_range(delta >> 2, 26)) return false;
			store32((load32() & UINT32_C(0xfc000000))
				| (static_cast<uint32_t>(delta >> 2) & UINT32_C(0x03ffffff)));
			return true;
		}
		case R_AARCH64_ADR_PREL_PG_HI21:
		case R_AARCH64_ADR_PREL_PG_HI21_NC: {
			int64_t pages = (static_cast<int64_t>(target & ~uintptr_t{0xfff})
				- static_cast<int64_t>(place & ~uintptr_t{0xfff})) >> 12;
			if (type == R_AARCH64_ADR_PREL_PG_HI21 && !signed_range(pages, 21)) {
				return false;
			}
			uint32_t instruction = load32() & UINT32_C(0x9f00001f);
			uint32_t encoded = static_cast<uint32_t>(pages) & UINT32_C(0x1fffff);
			instruction |= (encoded & 3) << 29;
			instruction |= ((encoded >> 2) & UINT32_C(0x7ffff)) << 5;
			store32(instruction);
			return true;
		}
		case R_AARCH64_ADD_ABS_LO12_NC:
			store32((load32() & ~UINT32_C(0x003ffc00))
				| ((static_cast<uint32_t>(target) & UINT32_C(0xfff)) << 10));
			return true;
		case R_AARCH64_LDST8_ABS_LO12_NC:
		case R_AARCH64_LDST16_ABS_LO12_NC:
		case R_AARCH64_LDST32_ABS_LO12_NC:
		case R_AARCH64_LDST64_ABS_LO12_NC:
		case R_AARCH64_LDST128_ABS_LO12_NC: {
			unsigned shift = type == R_AARCH64_LDST8_ABS_LO12_NC ? 0
				: type == R_AARCH64_LDST16_ABS_LO12_NC ? 1
				: type == R_AARCH64_LDST32_ABS_LO12_NC ? 2
				: type == R_AARCH64_LDST64_ABS_LO12_NC ? 3 : 4;
			store32((load32() & ~UINT32_C(0x003ffc00))
				| (((static_cast<uint32_t>(target) & UINT32_C(0xfff)) >> shift) << 10));
			return true;
		}
		default:
			return false;
	}
}

struct SerializedRelocation {
	uint32_t offset;
	uint32_t symbol;
	uint32_t type;
	int32_t addend;
};

struct SerializedSection {
	bool present = false;
	uint32_t type = 0;
	uint32_t flags = 0;
	uint32_t alignment = 0;
	uint64_t size = 0;
	bool is_virtual = false;
	std::string_view name;
	const uint8_t *data = nullptr;
	std::vector<SerializedRelocation> relocations;
};

struct SerializedSymbol {
	std::string_view name;
	uint32_t section = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
	uint8_t binding = 0;
	uint8_t kind = 0;
	bool defined = false;
};

struct SerializedA64Object {
	std::vector<SerializedSection> sections;
	std::vector<SerializedSymbol> symbols;
	uint32_t unwind_section = UINT32_MAX;
};

class ObjectCursor {
	const uint8_t *current_;
	size_t remaining_;

public:
	ObjectCursor(const uint8_t *data, size_t size)
		: current_{data}, remaining_{size} {}

	template <typename T>
	bool read(T *out) {
		if (remaining_ < sizeof(T)) {
			return false;
		}
		std::memcpy(out, current_, sizeof(T));
		current_ += sizeof(T);
		remaining_ -= sizeof(T);
		return true;
	}

	bool skip(size_t size) {
		if (remaining_ < size) {
			return false;
		}
		current_ += size;
		remaining_ -= size;
		return true;
	}

	bool view(size_t size, const uint8_t **out) {
		if (remaining_ < size) {
			return false;
		}
		*out = current_;
		current_ += size;
		remaining_ -= size;
		return true;
	}

	size_t remaining() const { return remaining_; }
};

bool parse_a64_object(
	const zend_native_image *image, SerializedA64Object *out) {
	static constexpr std::array<uint8_t, 16> MAGIC{
		'Z', 'N', 'M', 'I', 'R', '-', 'T', 'P', 'D', 'E', '-', 'A', '6', '4', 0, 2};
	if (image->text == nullptr || image->text_size < MAGIC.size()
			|| std::memcmp(image->text, MAGIC.data(), MAGIC.size()) != 0) {
		return false;
	}
	ObjectCursor cursor{
		image->text + MAGIC.size(), image->text_size - MAGIC.size()};
	uint32_t section_count;
	uint32_t present_section_count;
	uint32_t symbol_count;
	if (!cursor.read(&section_count)
			|| !cursor.read(&present_section_count)
			|| !cursor.read(&symbol_count)
			|| !cursor.read(&out->unwind_section)
			|| section_count == 0
			|| present_section_count >= section_count
			|| symbol_count == 0
			|| out->unwind_section >= section_count
			|| section_count > image->text_size
			|| symbol_count > image->text_size) {
		return false;
	}
	out->sections.clear();
	out->sections.resize(section_count);
	out->symbols.clear();
	out->symbols.resize(symbol_count);
	for (uint32_t present = 0; present < present_section_count; ++present) {
		uint32_t index;
		SerializedSection section;
		uint32_t name_size;
		uint32_t relocation_count;
		uint8_t is_virtual;
		if (!cursor.read(&index)
				|| !cursor.read(&section.type)
				|| !cursor.read(&section.flags)
				|| !cursor.read(&section.alignment)
				|| !cursor.read(&section.size)
				|| !cursor.read(&name_size)
				|| !cursor.read(&relocation_count)
				|| !cursor.read(&is_virtual)
				|| !cursor.skip(7)
				|| index == 0 || index >= section_count
				|| out->sections[index].present
				|| is_virtual > 1
				|| (is_virtual && relocation_count != 0)
				|| section.size > SIZE_MAX
				|| relocation_count > image->text_size) {
			return false;
		}
		const uint8_t *name;
		if (!cursor.view(name_size, &name)) {
			return false;
		}
		section.name = std::string_view{
			reinterpret_cast<const char *>(name), name_size};
		section.is_virtual = is_virtual != 0;
		if (!section.is_virtual
				&& !cursor.view(static_cast<size_t>(section.size),
					&section.data)) {
			return false;
		}
		section.relocations.reserve(relocation_count);
		for (uint32_t relocation_index = 0;
				relocation_index < relocation_count; ++relocation_index) {
			SerializedRelocation relocation;
			if (!cursor.read(&relocation.offset)
					|| !cursor.read(&relocation.symbol)
					|| !cursor.read(&relocation.type)
					|| !cursor.read(&relocation.addend)) {
				return false;
			}
			section.relocations.push_back(relocation);
		}
		section.present = true;
		out->sections[index] = std::move(section);
	}
	for (uint32_t index = 0; index < symbol_count; ++index) {
		SerializedSymbol symbol;
		uint32_t name_size;
		uint8_t defined;
		if (!cursor.read(&name_size)
				|| !cursor.read(&symbol.section)
				|| !cursor.read(&symbol.offset)
				|| !cursor.read(&symbol.size)
				|| !cursor.read(&symbol.binding)
				|| !cursor.read(&symbol.kind)
				|| !cursor.read(&defined)
				|| !cursor.skip(5)
				|| defined > 1
				|| (defined && (symbol.section == 0
					|| symbol.section >= section_count))) {
			return false;
		}
		const uint8_t *name;
		if (!cursor.view(name_size, &name)) {
			return false;
		}
		symbol.name = std::string_view{
			reinterpret_cast<const char *>(name), name_size};
		symbol.defined = defined != 0;
		out->symbols[index] = symbol;
	}
	return cursor.remaining() == 0;
}

#endif

/*
 * The component a defined entry symbol names: zend_native_entry is
 * component 0, zend_native_component_<index> (canonical decimal, as the
 * adaptor names them) the others; UINT32_MAX for any other name.
 */
static uint32_t component_entry_index(
	const char *name, size_t length, uint32_t count)
{
	static constexpr char entry[] = "zend_native_entry";
	static constexpr char prefix[] = "zend_native_component_";
	constexpr size_t prefix_length = sizeof(prefix) - 1;
	uint64_t index = 0;

	if (length == sizeof(entry) - 1 && std::memcmp(name, entry, length) == 0) {
		return count != 0 ? 0 : UINT32_MAX;
	}
	if (length <= prefix_length || length > prefix_length + 10
			|| std::memcmp(name, prefix, prefix_length) != 0
			|| name[prefix_length] == '0') {
		return UINT32_MAX;
	}
	for (size_t at = prefix_length; at < length; ++at) {
		if (name[at] < '0' || name[at] > '9') {
			return UINT32_MAX;
		}
		index = index * 10 + static_cast<uint64_t>(name[at] - '0');
	}
	return index < count ? static_cast<uint32_t>(index) : UINT32_MAX;
}

} // namespace

zend_result zend_tpde_map_darwin_arm64(
	const zend_native_image *image,
	zend_native_code *code,
	zend_native_diagnostic *diag) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (image == nullptr || image->text == nullptr || image->text_size == 0
			|| code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Darwin TPDE mapper requires a relocatable arm64 image");
		return FAILURE;
	}
	SerializedA64Object object;
	if (!parse_a64_object(image, &object)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image has an invalid object encoding");
		return FAILURE;
	}
	long page_size_value = sysconf(_SC_PAGESIZE);
	if (page_size_value <= 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin page size is unavailable");
		return FAILURE;
	}
	size_t page_size = static_cast<size_t>(page_size_value);
	const uint32_t unwind_section = object.unwind_section;
	auto published = std::make_unique<A64PublishedState>();
	published->sections.resize(object.sections.size());
	bool has_executable = false;

	for (size_t i = 1; i < object.sections.size(); ++i) {
		const SerializedSection &section = object.sections[i];
		if (!section.present
				|| (section.flags & DarwinAssembler::SECTION_ALLOC) == 0
				|| section.size == 0) {
			continue;
		}
		if ((section.flags & (DarwinAssembler::SECTION_WRITE
				| DarwinAssembler::SECTION_EXEC))
				== (DarwinAssembler::SECTION_WRITE
					| DarwinAssembler::SECTION_EXEC)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin TPDE image requests writable executable data");
			return FAILURE;
		}
		size_t logical_size = static_cast<size_t>(section.size);
		if (i == unwind_section) {
			if (logical_size > SIZE_MAX - sizeof(uint32_t)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin unwind section size overflows its terminator");
				return FAILURE;
			}
			logical_size += sizeof(uint32_t);
		}
		if (logical_size > SIZE_MAX - (page_size - 1)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin section size overflows page alignment");
			return FAILURE;
		}
		size_t mapping_size = (logical_size + page_size - 1)
			& ~(page_size - 1);
		bool executable = (section.flags & DarwinAssembler::SECTION_EXEC) != 0;
		int map_flags = MAP_PRIVATE | MAP_ANON | (executable ? MAP_JIT : 0);
		void *mapping = mmap(nullptr, mapping_size,
			executable ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE,
			map_flags, -1, 0);
		if (mapping == MAP_FAILED) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin section mapping failed");
			return FAILURE;
		}
		published->sections[i] = {mapping, mapping_size, section.flags};
		has_executable |= executable;
	}

	if (has_executable) pthread_jit_write_protect_np(0);
	for (size_t i = 1; i < object.sections.size(); ++i) {
		const SerializedSection &section = object.sections[i];
		if (!section.present || published->sections[i].mapping == nullptr) {
			continue;
		}
		if (!section.is_virtual && section.size != 0) {
			std::memcpy(published->sections[i].mapping,
				section.data, static_cast<size_t>(section.size));
		}
	}

	for (size_t i = 1; i < object.sections.size(); ++i) {
		const SerializedSection &section = object.sections[i];
		if (!section.present || published->sections[i].mapping == nullptr) {
			continue;
		}
		if (section.is_virtual) continue;
		for (const SerializedRelocation &relocation : section.relocations) {
			const size_t relocation_width =
				relocation.type == ::tpde::elf::R_AARCH64_ABS64
				? sizeof(uint64_t) : sizeof(uint32_t);
			if (relocation.symbol >= object.symbols.size()
					|| relocation.offset > section.size
					|| section.size - relocation.offset < relocation_width) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE relocation is outside its section");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
			const SerializedSymbol &symbol = object.symbols[relocation.symbol];
			uint8_t *location = static_cast<uint8_t *>(
				published->sections[i].mapping) + relocation.offset;
			uintptr_t symbol_address;
			if (symbol.defined) {
				if (symbol.section >= published->sections.size()
						|| published->sections[symbol.section].mapping
							== nullptr) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
						"Darwin TPDE image contains an invalid local symbol");
					if (has_executable) pthread_jit_write_protect_np(1);
					return FAILURE;
				}
				symbol_address = reinterpret_cast<uintptr_t>(
					published->sections[symbol.section].mapping)
					+ symbol.offset;
			} else {
				const void *resolved = nullptr;
				std::string external_name{symbol.name};
				if (!zend_tpde_image_resolve_symbol(
						image, external_name.c_str(), &resolved)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
						"Darwin TPDE image contains an unresolved external symbol");
					if (has_executable) pthread_jit_write_protect_np(1);
					return FAILURE;
				}
				symbol_address = reinterpret_cast<uintptr_t>(resolved);
			}
			if (!apply_relocation(location, relocation.type,
					symbol_address, relocation.addend)) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE relocation kind or range is unsupported");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
		}
	}
	if (image->component_entry_count == 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image contains no component entries");
		if (has_executable) pthread_jit_write_protect_np(1);
		return FAILURE;
	}
	code->component_entries = static_cast<zend_native_frame_entry_t *>(
		std::calloc(image->component_entry_count,
			sizeof(*code->component_entries)));
	if (code->component_entries == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Darwin component entry table");
		if (has_executable) pthread_jit_write_protect_np(1);
		return FAILURE;
	}
	uint32_t mapped_entry_count = 0;
	uint32_t entry_section_index = UINT32_MAX;
	for (const SerializedSymbol &symbol : object.symbols) {
		if (!symbol.defined
				|| symbol.section >= published->sections.size()
				|| published->sections[symbol.section].mapping == nullptr) {
			continue;
		}
		const uint32_t index = component_entry_index(symbol.name.data(),
			symbol.name.size(), image->component_entry_count);
		if (index == UINT32_MAX
				|| code->component_entries[index] != nullptr) {
			continue;
		}
		code->component_entries[index] =
			reinterpret_cast<zend_native_frame_entry_t>(
				static_cast<uint8_t *>(
					published->sections[symbol.section].mapping)
				+ symbol.offset);
		if (index == 0) {
			entry_section_index = symbol.section;
		}
		mapped_entry_count++;
	}
	if (mapped_entry_count != image->component_entry_count) {
		std::free(code->component_entries);
		code->component_entries = nullptr;
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE component entry symbol was not mapped");
		if (has_executable) pthread_jit_write_protect_np(1);
		return FAILURE;
	}

	for (const MappedSection &section : published->sections) {
		if (section.mapping == nullptr) continue;
		if ((section.flags & DarwinAssembler::SECTION_EXEC) != 0) {
			sys_icache_invalidate(section.mapping, section.mapping_size);
		}
	}
	if (has_executable) pthread_jit_write_protect_np(1);
	for (const MappedSection &section : published->sections) {
		if (section.mapping == nullptr) continue;
		/* MAP_JIT deliberately retains RWX as its maximum VM protection.  Apple
		 * enforces the effective W^X state per thread through
		 * pthread_jit_write_protect_np; mprotect(RX) is rejected for MAP_JIT
		 * mappings on supported Darwin versions. */
		if ((section.flags & DarwinAssembler::SECTION_EXEC) != 0) continue;
		int protection = PROT_READ;
		if ((section.flags & DarwinAssembler::SECTION_WRITE) != 0) {
			protection |= PROT_WRITE;
		}
		if (mprotect(section.mapping, section.mapping_size, protection) != 0) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin final section protection failed");
			return FAILURE;
		}
	}
	if (unwind_section == UINT32_MAX
			|| unwind_section >= published->sections.size()
			|| object.sections[unwind_section].is_virtual
			|| object.sections[unwind_section].size == 0
			|| published->sections[unwind_section].mapping == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image has no publishable unwind information");
		return FAILURE;
	}
	published->unwind_section =
		published->sections[unwind_section].mapping;
	published->unwind_section_size = object.sections[unwind_section].size;
	if (__unw_add_dynamic_eh_frame_section != nullptr) {
		__unw_add_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(published->unwind_section));
	} else {
		for_each_eh_frame_fde(published->unwind_section,
			published->unwind_section_size,
			[](void *fde) { __register_frame(fde); });
	}
	published->unwind_registered = true;

	const MappedSection &entry_section =
		published->sections[entry_section_index];
	code->mapping = entry_section.mapping;
	code->mapping_size = entry_section.mapping_size;
	code->entry = code->component_entries[0];
	code->component_entry_count = image->component_entry_count;
	code->unwind_registered = published->unwind_registered;
	code->target_state = published.release();
	code->destroy_target_state = destroy_a64_published_state;
	return SUCCESS;
#else
	(void) image;
	(void) code;
	zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
		"darwin-arm64-dev publication requires native Apple Silicon");
	return FAILURE;
#endif
}

zend_result zend_native_publish_darwin_arm64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (image == nullptr || image->target != ZEND_NATIVE_TARGET_DARWIN_ARM64
			|| image->text == nullptr || image->text_size == 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Darwin A64 publisher requires a non-empty Darwin image");
		return FAILURE;
	}
	zend_native_code *code = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*code)));
	if (code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Darwin native-code state");
		return FAILURE;
	}
	code->target = ZEND_NATIVE_TARGET_DARWIN_ARM64;
	code->argument_count = image->argument_count;
	code->frame_variable_count = image->frame_variable_count;
	code->frame_temporary_count = image->frame_temporary_count;
	if (zend_tpde_map_darwin_arm64(image, code, diag) == FAILURE) {
		std::free(code->component_entries);
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
		"darwin-arm64-dev publication requires native Apple Silicon");
	return FAILURE;
#endif
}

void zend_native_unmap_darwin_arm64(zend_native_code *code) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (code->destroy_target_state != nullptr) {
		code->destroy_target_state(code->target_state);
		code->target_state = nullptr;
	}
#else
	(void) code;
#endif
}
