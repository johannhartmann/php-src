// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

#include <atomic>

namespace zend::native::tpde::plan_detail {

constexpr size_t MAX_NATIVE_IMAGE_BYTES = size_t{1} << 28;

/* The serialized image's layout is part of ZEND_NATIVE_RUNTIME_ABI_VERSION,
 * which the enclosing bundle and the system id check. */
constexpr uint64_t NATIVE_IMAGE_BUILD_ID_SEED =
	UINT64_C(0x5750313300000000)
	^ static_cast<uint64_t>(ZEND_NATIVE_RUNTIME_ABI_VERSION);

static std::atomic_uint32_t live_unwind_registrations{0};

struct zend_native_serial_image_header {
	uint64_t build_id;
	uint32_t target;
	uint32_t argument_count;
	uint32_t frame_variable_count;
	uint32_t frame_temporary_count;
	uint32_t symbol_count;
	uint32_t binding_count;
	uint32_t component_count;
	uint32_t reserved;
	zend_native_image_metrics metrics;
	uint64_t text_size;
	uint64_t total_size;
};

struct zend_native_serial_binding {
	uint32_t symbol_index;
	uint32_t payload_size;
	uint64_t primary_reference;
	uint64_t scope_reference;
	uint32_t receiver_kind;
	uint32_t reserved;
};

struct zend_native_byte_buffer {
	unsigned char *bytes;
	size_t size;
	size_t capacity;
};

uint64_t native_image_build_id(zend_native_target target) {
	uint64_t hash = NATIVE_IMAGE_BUILD_ID_SEED
		^ static_cast<uint64_t>(static_cast<uint32_t>(target));

	for (size_t index = 0; index < sizeof(zend_system_id); ++index) {
		hash ^= static_cast<unsigned char>(zend_system_id[index]);
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

[[nodiscard]]
static bool native_buffer_append(
	zend_native_byte_buffer *buffer, const void *bytes, size_t size) {
	if (buffer == nullptr || (size != 0 && bytes == nullptr)
			|| size > MAX_NATIVE_IMAGE_BYTES
			|| buffer->size > MAX_NATIVE_IMAGE_BYTES - size) {
		return false;
	}
	const size_t required = buffer->size + size;
	if (required > buffer->capacity) {
		size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
		while (capacity < required) {
			capacity = capacity > MAX_NATIVE_IMAGE_BYTES / 2
				? MAX_NATIVE_IMAGE_BYTES : capacity * 2;
		}
		void *resized = std::realloc(buffer->bytes, capacity);
		if (resized == nullptr) {
			return false;
		}
		buffer->bytes = static_cast<unsigned char *>(resized);
		buffer->capacity = capacity;
	}
	if (size != 0) {
		std::memcpy(buffer->bytes + buffer->size, bytes, size);
	}
	buffer->size = required;
	return true;
}

/*
 * The image's symbol lookup index: the first symbol of each (kind,
 * namespace, id) and the first binding of each symbol. Symbols and bindings
 * are only ever appended, so the index catches up with them on each use.
 */
struct zend_native_image_symbol_lookup {
	struct key {
		uint32_t kind;
		uint32_t symbol_namespace;
		uint32_t id;

		bool operator==(const key &other) const {
			return kind == other.kind
				&& symbol_namespace == other.symbol_namespace
				&& id == other.id;
		}
	};
	struct key_hash {
		size_t operator()(const key &value) const {
			return static_cast<size_t>(
				((uint64_t{value.kind} * UINT64_C(0x9e3779b97f4a7c15))
					^ (uint64_t{value.symbol_namespace} << 32)
					^ value.id)
				* UINT64_C(0xff51afd7ed558ccd));
		}
	};
	std::unordered_map<key, uint32_t, key_hash> symbols;
	std::vector<uint32_t> first_binding;
	uint32_t indexed_symbols = 0;
	uint32_t indexed_bindings = 0;
	/* Up to here every symbol carries the name image_add_symbol() gives
	 * its key, so the first symbol of a name is the first of its key. */
	uint32_t checked_names = 0;
	bool names_from_keys = true;
};

static zend_native_image_symbol_lookup *native_image_symbol_lookup(
	const zend_native_image *image) {
	auto *lookup = static_cast<zend_native_image_symbol_lookup *>(
		image->symbol_lookup);
	if (lookup == nullptr) {
		lookup = new zend_native_image_symbol_lookup();
		const_cast<zend_native_image *>(image)->symbol_lookup = lookup;
	}
	for (; lookup->indexed_symbols < image->symbol_count;
			++lookup->indexed_symbols) {
		const zend_native_image_symbol &symbol =
			image->symbols[lookup->indexed_symbols];
		lookup->symbols.try_emplace(
			{symbol.kind, symbol.symbol_namespace, symbol.id},
			lookup->indexed_symbols);
	}
	if (lookup->first_binding.size() < image->symbol_count) {
		lookup->first_binding.resize(image->symbol_count, UINT32_MAX);
	}
	for (; lookup->indexed_bindings < image->symbol_binding_count;
			++lookup->indexed_bindings) {
		const uint32_t symbol_index =
			image->symbol_bindings[lookup->indexed_bindings].symbol_index;
		if (symbol_index >= lookup->first_binding.size()) {
			lookup->first_binding.resize(
				static_cast<size_t>(symbol_index) + 1, UINT32_MAX);
		}
		if (lookup->first_binding[symbol_index] == UINT32_MAX) {
			lookup->first_binding[symbol_index] = lookup->indexed_bindings;
		}
	}
	return lookup;
}

static void native_image_symbol_lookup_destroy(zend_native_image *image) {
	delete static_cast<zend_native_image_symbol_lookup *>(
		image->symbol_lookup);
	image->symbol_lookup = nullptr;
}

/* The first symbol of the key, or UINT32_MAX. */
static uint32_t native_image_symbol_index(
	const zend_native_image *image,
	uint32_t kind, uint32_t id, uint32_t symbol_namespace) {
	const zend_native_image_symbol_lookup *lookup =
		native_image_symbol_lookup(image);
	const auto found = lookup->symbols.find({kind, symbol_namespace, id});
	return found == lookup->symbols.end() ? UINT32_MAX : found->second;
}

/* A symbol's name spells its key: kind, namespace and id. */
#define NATIVE_IMAGE_SYMBOL_NAME_FORMAT "__znmir_%u_%u_%u"

[[nodiscard]]
static bool native_image_symbol_name(
	char (&name)[sizeof(zend_native_image_symbol::name)],
	uint32_t kind, uint32_t symbol_namespace, uint32_t id) {
	const int written = std::snprintf(name, sizeof(name),
		NATIVE_IMAGE_SYMBOL_NAME_FORMAT, kind, symbol_namespace, id);
	return written > 0 && static_cast<size_t>(written) < sizeof(name);
}

static const zend_native_image_symbol_binding *native_image_binding(
	const zend_native_image *image, uint32_t symbol_index) {
	const zend_native_image_symbol_lookup *lookup =
		native_image_symbol_lookup(image);
	return symbol_index < lookup->first_binding.size()
			&& lookup->first_binding[symbol_index] != UINT32_MAX
		? &image->symbol_bindings[lookup->first_binding[symbol_index]]
		: nullptr;
}

static bool native_descriptor_size(
	uint32_t kind, const void *address, size_t *size) {
	if (address == nullptr || size == nullptr) {
		return false;
	}
	uint32_t argument_count;
	uint32_t trailing_word_count = 0;
	size_t base_size;
	size_t argument_size;
	switch (kind) {
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR: {
			const auto *descriptor =
				static_cast<const zend_native_direct_call_descriptor *>(
					address);
			argument_count = descriptor->argument_count;
			trailing_word_count = descriptor->default_literal_count;
			base_size = offsetof(
				zend_native_direct_call_descriptor, arguments);
			argument_size = sizeof(zend_native_direct_call_argument);
			break;
		}
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR:
			argument_count =
				static_cast<const zend_native_direct_internal_call_descriptor *>(
					address)->argument_count;
			base_size = offsetof(
				zend_native_direct_internal_call_descriptor, arguments);
			argument_size =
				sizeof(zend_native_direct_internal_call_argument);
			break;
		case ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR:
			argument_count =
				static_cast<const zend_native_user_call_descriptor *>(
					address)->argument_count;
			base_size = offsetof(zend_native_user_call_descriptor, arguments);
			argument_size =
				sizeof(zend_native_direct_internal_call_argument);
			break;
		default:
			return false;
	}
	if (!checked_count(argument_count)
			|| argument_count > (MAX_NATIVE_IMAGE_BYTES - base_size)
				/ argument_size) {
		return false;
	}
	const size_t arguments_size =
		static_cast<size_t>(argument_count) * argument_size;
	if (!checked_count(trailing_word_count)
			|| trailing_word_count
				> (MAX_NATIVE_IMAGE_BYTES - base_size - arguments_size)
					/ sizeof(uint32_t)) {
		return false;
	}
	*size = base_size + arguments_size
		+ static_cast<size_t>(trailing_word_count) * sizeof(uint32_t);
	return true;
}

[[nodiscard]]
static bool image_add_symbol(
	zend_native_image *image,
	zend_native_image_symbol_kind kind,
	uint32_t id,
	uint32_t symbol_namespace,
	const void *address = nullptr) {
	if (image == nullptr || !zend_mir_id_is_valid(id)) {
		return false;
	}
	if (const uint32_t index = native_image_symbol_index(
			image, kind, id, symbol_namespace); index != UINT32_MAX) {
		if (address == nullptr) {
			return true;
		}
		const zend_native_image_symbol_binding *binding =
			native_image_binding(image, index);
		return binding != nullptr && binding->address == address;
	}
	if (image->symbol_count == image->symbol_capacity) {
		uint32_t capacity = image->symbol_capacity == 0
			? 16 : image->symbol_capacity * 2;
		if (capacity > MAX_RECORDS) {
			return false;
		}
		void *resized = std::realloc(image->symbols,
			static_cast<size_t>(capacity) * sizeof(*image->symbols));
		if (resized == nullptr) {
			return false;
		}
		image->symbols = static_cast<zend_native_image_symbol *>(resized);
		image->symbol_capacity = capacity;
	}
	zend_native_image_symbol &symbol = image->symbols[image->symbol_count];
	std::memset(&symbol, 0, sizeof(symbol));
	symbol.kind = kind;
	symbol.id = id;
	symbol.symbol_namespace = symbol_namespace;
	if (!native_image_symbol_name(symbol.name,
			static_cast<uint32_t>(kind), symbol_namespace, id)) {
		return false;
	}
	const uint32_t symbol_index = image->symbol_count++;
	if (address != nullptr) {
		if (image->symbol_binding_count == image->symbol_binding_capacity) {
			uint32_t capacity = image->symbol_binding_capacity == 0
				? 16 : image->symbol_binding_capacity * 2;
			if (capacity > MAX_RECORDS) {
				return false;
			}
			void *resized = std::realloc(image->symbol_bindings,
				static_cast<size_t>(capacity)
					* sizeof(*image->symbol_bindings));
			if (resized == nullptr) {
				return false;
			}
			image->symbol_bindings =
				static_cast<zend_native_image_symbol_binding *>(resized);
			image->symbol_binding_capacity = capacity;
		}
		image->symbol_bindings[image->symbol_binding_count++] = {
			symbol_index, address};
	}
	return true;
}

bool prepare_image_symbols(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	for (uint32_t id = 1; id < ZEND_NATIVE_HELPER_COUNT; ++id) {
		if (!zend_tpde_runtime_helper_required(plan,
				static_cast<zend_native_runtime_helper_id>(id))) {
			continue;
		}
		if (!image_add_symbol(image,
				ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER, id, 0)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image runtime symbol table");
			return false;
		}
	}
	if (plan->call_count_cell != nullptr
			&& (!image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					ZEND_NATIVE_TIER2_COUNT_SYMBOL_ID,
					plan->symbol_namespace,
					plan->call_count_cell)
				|| !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
					ZEND_NATIVE_HELPER_TIER2_NOTE, 0)
				|| !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
					ZEND_NATIVE_HELPER_TIER2_RECORD, 0))) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to create the native image call-count symbols");
		return false;
	}
	if (plan->speculation_cell != nullptr
			&& !image_add_symbol(image,
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				ZEND_NATIVE_SPECULATION_SYMBOL_ID,
				plan->symbol_namespace,
				plan->speculation_cell)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to create the native image speculation symbol");
		return false;
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		if (instruction.entry_cell != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					instruction.call_site->target_id,
					plan->symbol_namespace,
					instruction.entry_cell)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image entry-cell symbol");
			return false;
		}
		if (instruction.internal_call_cell != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
					instruction.call_site->target_id,
					plan->symbol_namespace,
					instruction.internal_call_cell)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image internal-call symbol");
			return false;
		}
		if (instruction.direct_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
					instruction.record.id, plan->symbol_namespace,
					instruction.direct_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image direct-call symbol");
			return false;
		}
		if (instruction.direct_internal_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					instruction.record.id, plan->symbol_namespace,
					instruction.direct_internal_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image direct internal-call symbol");
			return false;
		}
		if (instruction.user_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
					instruction.record.id, plan->symbol_namespace,
					instruction.user_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image user-call symbol");
			return false;
		}
	}
	return true;
}

/*
 * Room for count call descriptors of each kind and as many internal call
 * cells; false when an allocation failed.
 */
[[nodiscard]]
static bool allocate_call_descriptors(
	zend_native_image *image, uint32_t count) {
	image->owned_internal_call_cells =
		static_cast<zend_native_internal_call_cell **>(std::calloc(
			count, sizeof(*image->owned_internal_call_cells)));
	image->direct_calls =
		static_cast<zend_native_direct_call_descriptor **>(std::calloc(
			count, sizeof(*image->direct_calls)));
	image->direct_internal_calls =
		static_cast<zend_native_direct_internal_call_descriptor **>(
			std::calloc(count, sizeof(*image->direct_internal_calls)));
	image->user_calls =
		static_cast<zend_native_user_call_descriptor **>(std::calloc(
			count, sizeof(*image->user_calls)));
	return image->owned_internal_call_cells != nullptr
		&& image->direct_calls != nullptr
		&& image->direct_internal_calls != nullptr
		&& image->user_calls != nullptr;
}

/* Hands an image's call descriptors and internal call cells to its code. */
static void move_call_descriptors(
	zend_native_code *code, zend_native_image *image) {
	code->direct_calls = image->direct_calls;
	code->direct_call_count = image->direct_call_count;
	image->direct_calls = nullptr;
	image->direct_call_count = 0;
	code->direct_internal_calls = image->direct_internal_calls;
	code->direct_internal_call_count = image->direct_internal_call_count;
	image->direct_internal_calls = nullptr;
	image->direct_internal_call_count = 0;
	code->user_calls = image->user_calls;
	code->user_call_count = image->user_call_count;
	image->user_calls = nullptr;
	image->user_call_count = 0;
	code->owned_internal_call_cells = image->owned_internal_call_cells;
	code->owned_internal_call_cell_count =
		image->owned_internal_call_cell_count;
	image->owned_internal_call_cells = nullptr;
	image->owned_internal_call_cell_count = 0;
}

} // namespace zend::native::tpde::plan_detail

using namespace zend::native::tpde::plan_detail;

bool zend_tpde_image_append(
	zend_native_image *image, const void *bytes, size_t length) {
	if (length > MAX_NATIVE_IMAGE_BYTES
			|| image->text_size > MAX_NATIVE_IMAGE_BYTES - length) {
		return false;
	}
	size_t needed = image->text_size + length;
	if (needed > image->text_capacity) {
		size_t capacity = image->text_capacity == 0 ? 4096 : image->text_capacity;
		while (capacity < needed) {
			capacity = capacity > MAX_NATIVE_IMAGE_BYTES / 2
				? MAX_NATIVE_IMAGE_BYTES
				: capacity * 2;
		}
		void *resized = std::realloc(image->text, capacity);
		if (resized == nullptr) {
			return false;
		}
		image->text = static_cast<unsigned char *>(resized);
		image->text_capacity = capacity;
	}
	std::memcpy(image->text + image->text_size, bytes, length);
	image->text_size = needed;
	return true;
}

const zend_native_image_symbol *zend_tpde_image_symbol_find(
	const zend_native_image *image,
	zend_native_image_symbol_kind kind,
	uint32_t id,
	uint32_t symbol_namespace) {
	if (image == nullptr) {
		return nullptr;
	}
	const uint32_t index =
		native_image_symbol_index(image, kind, id, symbol_namespace);
	return index == UINT32_MAX ? nullptr : &image->symbols[index];
}

const zend_native_image_symbol *zend_tpde_required_runtime_symbol(
	const zend_native_image *image,
	zend_native_runtime_helper_id helper) {
	const zend_native_image_symbol *symbol = zend_tpde_image_symbol_find(
		image, ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
		static_cast<uint32_t>(helper), 0);
	/* prepare_image_symbols() named each helper planning required. */
	ZEND_ASSERT(symbol != nullptr);
	return symbol;
}

bool zend_tpde_image_resolve_symbol(
	const zend_native_image *image,
	const char *name,
	const void **address) {
	if (image == nullptr || name == nullptr || address == nullptr) {
		return false;
	}
	*address = nullptr;
	const zend_native_image_symbol *symbol = nullptr;
	zend_native_image_symbol_lookup *lookup =
		native_image_symbol_lookup(image);
	for (; lookup->names_from_keys
				&& lookup->checked_names < image->symbol_count;
			++lookup->checked_names) {
		const zend_native_image_symbol &candidate =
			image->symbols[lookup->checked_names];
		char expected[sizeof(candidate.name)];
		lookup->names_from_keys = native_image_symbol_name(expected,
				candidate.kind, candidate.symbol_namespace, candidate.id)
			&& std::strncmp(expected, candidate.name, sizeof(expected)) == 0;
	}
	unsigned int kind;
	unsigned int symbol_namespace;
	unsigned int id;
	int consumed = 0;
	if (lookup->names_from_keys
			&& std::sscanf(name, NATIVE_IMAGE_SYMBOL_NAME_FORMAT "%n",
				&kind, &symbol_namespace, &id, &consumed) == 3
			&& name[consumed] == 0) {
		const uint32_t index =
			native_image_symbol_index(image, kind, id, symbol_namespace);
		if (index != UINT32_MAX
				&& std::strcmp(image->symbols[index].name, name) == 0) {
			symbol = &image->symbols[index];
		}
	} else {
		for (uint32_t index = 0; index < image->symbol_count; ++index) {
			if (std::strcmp(image->symbols[index].name, name) == 0) {
				symbol = &image->symbols[index];
				break;
			}
		}
	}
	if (symbol == nullptr) {
		return false;
	}
	if (symbol->kind != ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER) {
		if ((symbol->kind != ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR)) {
			return false;
		}
		const uint32_t symbol_index =
			static_cast<uint32_t>(symbol - image->symbols);
		const zend_native_image_symbol_binding *first =
			native_image_binding(image, symbol_index);
		if (first != nullptr && first->address != nullptr) {
			*address = first->address;
			return true;
		}
		/* Only a binding without an address can hide a later one. */
		for (uint32_t index = 0;
				first != nullptr && index < image->symbol_binding_count;
				++index) {
			const zend_native_image_symbol_binding &binding =
				image->symbol_bindings[index];
			if (binding.symbol_index == symbol_index
					&& binding.address != nullptr) {
				*address = binding.address;
				return true;
			}
		}
		return false;
	}
	const zend_native_runtime_helper *helper =
		zend_native_runtime_helper_find(
			static_cast<zend_native_runtime_helper_id>(symbol->id));
	if (helper == nullptr) {
		return false;
	}
	*address = helper->address;
	return true;
}

extern "C" zend_result zend_native_image_serialize(
	const zend_native_image *image,
	zend_native_image_encode_reference_t encode_reference,
	void *reference_context,
	unsigned char **out_bytes,
	size_t *out_size,
	zend_native_diagnostic *diag) {
	zend_native_byte_buffer buffer{};
	zend_native_serial_image_header header{};

	if (out_bytes == nullptr || out_size == nullptr || image == nullptr
			|| encode_reference == nullptr
			|| image->text_size > MAX_NATIVE_IMAGE_BYTES
			|| !checked_count(image->symbol_count)
			|| !checked_count(image->symbol_binding_count)
			|| image->component_entry_count == 0
			|| !checked_count(image->component_entry_count)
			|| image->component_entries == nullptr
			|| !checked_count(image->frame_variable_count)
			|| !checked_count(image->frame_temporary_count)
			|| image->frame_variable_count < image->argument_count
			|| image->frame_temporary_count
				> MAX_RECORDS - image->frame_variable_count) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native image cannot be serialized");
		return FAILURE;
	}
	*out_bytes = nullptr;
	*out_size = 0;
	for (uint32_t index = 0;
			index < image->component_entry_count; ++index) {
		const zend_native_component_entry &entry =
			image->component_entries[index];
		if (!checked_count(entry.frame_variable_count)
				|| !checked_count(entry.frame_temporary_count)
				|| entry.frame_variable_count < entry.argument_count
				|| entry.frame_temporary_count
					> MAX_RECORDS - entry.frame_variable_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"native component metadata cannot be serialized");
			return FAILURE;
		}
	}
	header.target = static_cast<uint32_t>(image->target);
	header.build_id = image->build_id;
	header.argument_count = image->argument_count;
	header.frame_variable_count = image->frame_variable_count;
	header.frame_temporary_count = image->frame_temporary_count;
	header.metrics = image->metrics;
	header.text_size = image->text_size;
	header.symbol_count = image->symbol_count;
	header.binding_count = image->symbol_binding_count;
	header.component_count = image->component_entry_count;
	if (!native_buffer_append(&buffer, &header, sizeof(header))
			|| !native_buffer_append(
				&buffer, image->text, image->text_size)
			|| !native_buffer_append(
				&buffer, image->symbols,
				static_cast<size_t>(image->symbol_count)
					* sizeof(*image->symbols))
			|| !native_buffer_append(
				&buffer, image->component_entries,
				static_cast<size_t>(image->component_entry_count)
					* sizeof(*image->component_entries))) {
		goto allocation_failure;
	}
	for (uint32_t symbol_index = 0;
			symbol_index < image->symbol_count; ++symbol_index) {
		const zend_native_image_symbol &symbol =
			image->symbols[symbol_index];
		if (symbol.kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER) {
			continue;
		}
		const zend_native_image_symbol_binding *binding =
			native_image_binding(image, symbol_index);
		if (binding == nullptr || binding->address == nullptr) {
			goto invalid_image;
		}
		zend_native_serial_binding serialized{};
		serialized.symbol_index = symbol_index;
		const void *payload = nullptr;
		size_t payload_size = 0;
		const zend_native_internal_call_cell *internal_cell;

		switch (symbol.kind) {
			case ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL:
				if (!encode_reference(
						reference_context,
						ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL,
						binding->address,
						&serialized.primary_reference)
						|| serialized.primary_reference == 0) {
					goto invalid_image;
				}
				break;
			case ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL:
				internal_cell =
					static_cast<const zend_native_internal_call_cell *>(
						binding->address);
				if (internal_cell->function == nullptr
						|| !encode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
							internal_cell->function,
							&serialized.primary_reference)
						|| serialized.primary_reference == 0
						|| (internal_cell->called_scope != nullptr
							&& (!encode_reference(
								reference_context,
								ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
								internal_cell->called_scope,
								&serialized.scope_reference)
								|| serialized.scope_reference == 0))) {
					goto invalid_image;
				}
				serialized.receiver_kind =
					static_cast<uint32_t>(internal_cell->receiver_kind);
				break;
			case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR: {
				const auto *descriptor =
					static_cast<const zend_native_direct_call_descriptor *>(
						binding->address);
				if (!native_descriptor_size(
						symbol.kind, descriptor, &payload_size)
						|| payload_size > UINT32_MAX
						|| descriptor->expected_function == nullptr
						|| !encode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
							descriptor->expected_function,
							&serialized.primary_reference)
						|| serialized.primary_reference == 0
						|| (descriptor->called_scope != nullptr
							&& (!encode_reference(
								reference_context,
								ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
								descriptor->called_scope,
								&serialized.scope_reference)
								|| serialized.scope_reference == 0))) {
					goto invalid_image;
				}
				payload = descriptor;
				break;
			}
			case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR:
			case ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR:
				if (!native_descriptor_size(
						symbol.kind, binding->address, &payload_size)
						|| payload_size > UINT32_MAX) {
					goto invalid_image;
				}
				payload = binding->address;
				break;
			default:
				goto invalid_image;
		}
		serialized.payload_size = static_cast<uint32_t>(payload_size);
		if (!native_buffer_append(
				&buffer, &serialized, sizeof(serialized))
				|| !native_buffer_append(
					&buffer, payload, payload_size)) {
			goto allocation_failure;
		}
		if (symbol.kind
				== ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR) {
			/* The function and scope are encoded as references; their
			 * pointers are stored as null. */
			unsigned char *stored = buffer.bytes + buffer.size - payload_size;
			std::memset(stored
					+ offsetof(zend_native_direct_call_descriptor,
						expected_function),
				0, sizeof(zend_function *));
			std::memset(stored
					+ offsetof(zend_native_direct_call_descriptor,
						called_scope),
				0, sizeof(zend_class_entry *));
		}
	}
	if (buffer.size > MAX_NATIVE_IMAGE_BYTES) {
		goto invalid_image;
	}
	header.total_size = buffer.size;
	std::memcpy(buffer.bytes, &header, sizeof(header));
	*out_bytes = buffer.bytes;
	*out_size = buffer.size;
	return SUCCESS;

invalid_image:
	std::free(buffer.bytes);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"native image contains a non-persistent binding");
	return FAILURE;
allocation_failure:
	std::free(buffer.bytes);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
		"unable to serialize native image");
	return FAILURE;
}

/* How a step of restoring a serialized image ends. */
enum class image_restore : uint8_t {
	restored,
	invalid,
	allocation_failed,
};

/* What zend_native_image_deserialize() reads and restores. */
struct image_deserializer {
	const unsigned char *bytes;
	size_t size;
	zend_native_image_decode_reference_t decode_reference;
	void *reference_context;
	zend_native_serial_image_header header;
	zend_native_image *image;
	/* The next unread byte. */
	size_t offset;
	size_t symbol_bytes;
	size_t component_bytes;
	/* Per symbol: 1 when a binding restored it. */
	std::vector<uint8_t> bound;
};

/* Reads the header and checks that the sections it counts fit. */
static image_restore read_image_header(image_deserializer &state) {
	const unsigned char *bytes = state.bytes;
	const size_t size = state.size;
	zend_native_serial_image_header &header = state.header;
	std::memcpy(&header, bytes, sizeof(header));
	if (header.target != ZEND_NATIVE_HOST_TARGET
			|| header.build_id != native_image_build_id(
				static_cast<zend_native_target>(header.target))
			|| header.total_size != size
			|| header.text_size > size
			|| !checked_count(header.symbol_count)
			|| !checked_count(header.binding_count)
			|| header.binding_count > header.symbol_count
			|| header.component_count == 0
			|| !checked_count(header.component_count)
			|| !checked_count(header.frame_variable_count)
			|| !checked_count(header.frame_temporary_count)
			|| header.frame_variable_count < header.argument_count
			|| header.frame_temporary_count
				> MAX_RECORDS - header.frame_variable_count) {
		return image_restore::invalid;
	}
	state.offset = sizeof(header);
	const size_t offset = state.offset;
	if (header.text_size > size - offset) {
		return image_restore::invalid;
	}
	state.symbol_bytes =
		static_cast<size_t>(header.symbol_count)
			* sizeof(zend_native_image_symbol);
	if (state.symbol_bytes > size - offset - header.text_size) {
		return image_restore::invalid;
	}
	state.component_bytes =
		static_cast<size_t>(header.component_count)
			* sizeof(zend_native_component_entry);
	if (state.component_bytes
			> size - offset - header.text_size - state.symbol_bytes) {
		return image_restore::invalid;
	}
	return image_restore::restored;
}

/* Restores the image with its text, symbols and component entries, and
 * allocates its bindings. */
static image_restore restore_image_sections(image_deserializer &state) {
	const unsigned char *bytes = state.bytes;
	const zend_native_serial_image_header &header = state.header;
	const size_t symbol_bytes = state.symbol_bytes;
	const size_t component_bytes = state.component_bytes;
	state.image = static_cast<zend_native_image *>(
		std::calloc(1, sizeof(*state.image)));
	zend_native_image *image = state.image;
	if (image == nullptr) {
		return image_restore::allocation_failed;
	}
	image->target = static_cast<zend_native_target>(header.target);
	image->build_id = header.build_id;
	image->argument_count = header.argument_count;
	image->frame_variable_count = header.frame_variable_count;
	image->frame_temporary_count = header.frame_temporary_count;
	image->metrics = header.metrics;
	image->text_size = header.text_size;
	image->text_capacity = header.text_size;
	if (header.text_size != 0) {
		image->text = static_cast<unsigned char *>(
			std::malloc(header.text_size));
		if (image->text == nullptr) {
			return image_restore::allocation_failed;
		}
		std::memcpy(image->text, bytes + state.offset, header.text_size);
	}
	state.offset += header.text_size;
	image->symbol_count = header.symbol_count;
	image->symbol_capacity = header.symbol_count;
	if (symbol_bytes != 0) {
		image->symbols = static_cast<zend_native_image_symbol *>(
			std::malloc(symbol_bytes));
		state.bound.assign(header.symbol_count, 0);
		if (image->symbols == nullptr) {
			return image_restore::allocation_failed;
		}
		std::memcpy(image->symbols, bytes + state.offset, symbol_bytes);
	}
	state.offset += symbol_bytes;
	image->component_entries = static_cast<zend_native_component_entry *>(
		std::malloc(component_bytes));
	if (image->component_entries == nullptr) {
		return image_restore::allocation_failed;
	}
	std::memcpy(
		image->component_entries, bytes + state.offset, component_bytes);
	image->component_entry_count = header.component_count;
	state.offset += component_bytes;
	for (uint32_t index = 0;
			index < image->component_entry_count; ++index) {
		const zend_native_component_entry &entry =
			image->component_entries[index];
		if (!checked_count(entry.frame_variable_count)
				|| !checked_count(entry.frame_temporary_count)
				|| entry.frame_variable_count < entry.argument_count
				|| entry.frame_temporary_count
					> MAX_RECORDS - entry.frame_variable_count) {
			return image_restore::invalid;
		}
	}
	if (header.binding_count != 0) {
		image->symbol_bindings =
			static_cast<zend_native_image_symbol_binding *>(
				std::calloc(header.binding_count,
					sizeof(*image->symbol_bindings)));
		/* Each binding owns at most one cell or descriptor. */
		const bool call_descriptors_allocated =
			allocate_call_descriptors(image, header.binding_count);
		if (image->symbol_bindings == nullptr
				|| !call_descriptors_allocated) {
			return image_restore::allocation_failed;
		}
		image->symbol_binding_capacity = header.binding_count;
	}
	return image_restore::restored;
}

/* An internal call cell for the function and scope the references name. */
static image_restore restore_internal_call_cell(
	image_deserializer &state,
	const zend_native_serial_binding &serialized,
	const void **address) {
	zend_native_image *image = state.image;
	if (serialized.payload_size != 0
			|| serialized.primary_reference == 0
			|| serialized.receiver_kind
				> ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
		return image_restore::invalid;
	}
	auto *cell =
		static_cast<zend_native_internal_call_cell *>(
			std::calloc(
				1, sizeof(zend_native_internal_call_cell)));
	const void *resolved_function = nullptr;
	const void *resolved_scope = nullptr;
	if (cell == nullptr
			|| !state.decode_reference(
				state.reference_context,
				ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
				serialized.primary_reference,
				&resolved_function)
			|| resolved_function == nullptr
			|| (serialized.scope_reference != 0
				&& !state.decode_reference(
					state.reference_context,
					ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
					serialized.scope_reference,
					&resolved_scope))) {
		std::free(cell);
		return image_restore::invalid;
	}
	/* The checks of a compiled cell hold for a loaded one. */
	if (zend_native_internal_call_cell_init(cell,
			const_cast<zend_function *>(
				static_cast<const zend_function *>(
					resolved_function)),
			const_cast<zend_class_entry *>(
				static_cast<const zend_class_entry *>(
					resolved_scope)),
			static_cast<zend_native_internal_receiver_kind>(
				serialized.receiver_kind)) != SUCCESS) {
		std::free(cell);
		return image_restore::invalid;
	}
	image->owned_internal_call_cells[
		image->owned_internal_call_cell_count++] = cell;
	*address = cell;
	return image_restore::restored;
}

/* A direct call descriptor from its payload, for the function and scope
 * the references name. */
static image_restore restore_direct_call_descriptor(
	image_deserializer &state,
	const zend_native_serial_binding &serialized,
	const zend_native_image_symbol &symbol,
	const void **address) {
	zend_native_image *image = state.image;
	if (serialized.primary_reference == 0) {
		return image_restore::invalid;
	}
	auto *descriptor =
		static_cast<zend_native_direct_call_descriptor *>(
			std::malloc(serialized.payload_size));
	if (descriptor == nullptr) {
		return image_restore::allocation_failed;
	}
	std::memcpy(
		descriptor, state.bytes + state.offset, serialized.payload_size);
	size_t expected_size;
	const void *resolved_function = nullptr;
	const void *resolved_scope = nullptr;
	if (!native_descriptor_size(
			symbol.kind, descriptor, &expected_size)
			|| expected_size != serialized.payload_size
			|| descriptor->expected_function != nullptr
			|| descriptor->called_scope != nullptr
			|| !state.decode_reference(
				state.reference_context,
				ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
				serialized.primary_reference,
				&resolved_function)
			|| resolved_function == nullptr
			|| (serialized.scope_reference != 0
				&& !state.decode_reference(
					state.reference_context,
					ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
					serialized.scope_reference,
					&resolved_scope))) {
		std::free(descriptor);
		return image_restore::invalid;
	}
	descriptor->expected_function =
		const_cast<zend_function *>(
			static_cast<const zend_function *>(
				resolved_function));
	descriptor->called_scope =
		const_cast<zend_class_entry *>(
			static_cast<const zend_class_entry *>(
				resolved_scope));
	/* The derived flags hold for the function this process
	 * resolved. */
	zend_native_direct_call_descriptor_derive(descriptor);
	image->direct_calls[image->direct_call_count++] = descriptor;
	*address = descriptor;
	return image_restore::restored;
}

/* A direct internal or user call descriptor from its payload. */
static image_restore restore_call_descriptor(
	image_deserializer &state,
	const zend_native_serial_binding &serialized,
	const zend_native_image_symbol &symbol,
	const void **address) {
	zend_native_image *image = state.image;
	const bool user_descriptor = symbol.kind
		== ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR;
	auto free_descriptor = [user_descriptor](void *allocated) {
		if (user_descriptor) {
			zend_native_user_call_descriptor_free(
				static_cast<zend_native_user_call_descriptor *>(
					allocated));
		} else {
			std::free(allocated);
		}
	};
	void *descriptor = user_descriptor
		? static_cast<void *>(
			zend_native_user_call_descriptor_alloc(
				serialized.payload_size))
		: std::malloc(serialized.payload_size);
	if (descriptor == nullptr) {
		return image_restore::allocation_failed;
	}
	std::memcpy(
		descriptor, state.bytes + state.offset, serialized.payload_size);
	size_t expected_size;
	if (!native_descriptor_size(
			symbol.kind, descriptor, &expected_size)
			|| expected_size != serialized.payload_size) {
		free_descriptor(descriptor);
		return image_restore::invalid;
	}
	if (symbol.kind
			== ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR) {
		image->direct_internal_calls[
			image->direct_internal_call_count++] =
				static_cast<
					zend_native_direct_internal_call_descriptor *>(
						descriptor);
	} else {
		image->user_calls[image->user_call_count++] =
			static_cast<zend_native_user_call_descriptor *>(
				descriptor);
	}
	*address = descriptor;
	return image_restore::restored;
}

/* Restores the next serialized binding and the cell or descriptor it
 * owns. */
static image_restore restore_image_binding(image_deserializer &state) {
	const size_t size = state.size;
	zend_native_image *image = state.image;
	zend_native_serial_binding serialized;
	if (sizeof(serialized) > size - state.offset) {
		return image_restore::invalid;
	}
	std::memcpy(&serialized, state.bytes + state.offset, sizeof(serialized));
	state.offset += sizeof(serialized);
	if (serialized.symbol_index >= image->symbol_count
			|| state.bound[serialized.symbol_index]
			|| serialized.payload_size > size - state.offset) {
		return image_restore::invalid;
	}
	const zend_native_image_symbol &symbol =
		image->symbols[serialized.symbol_index];
	const void *address = nullptr;
	image_restore status;
	switch (symbol.kind) {
		case ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL:
			if (serialized.payload_size != 0
					|| serialized.primary_reference == 0
					|| !state.decode_reference(
						state.reference_context,
						ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL,
						serialized.primary_reference, &address)
					|| address == nullptr) {
				return image_restore::invalid;
			}
			status = image_restore::restored;
			break;
		case ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL:
			status = restore_internal_call_cell(state, serialized, &address);
			break;
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR:
			status = restore_direct_call_descriptor(
				state, serialized, symbol, &address);
			break;
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR:
		case ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR:
			status = restore_call_descriptor(
				state, serialized, symbol, &address);
			break;
		default:
			return image_restore::invalid;
	}
	if (status != image_restore::restored) {
		return status;
	}
	state.offset += serialized.payload_size;
	image->symbol_bindings[image->symbol_binding_count++] = {
		serialized.symbol_index, address};
	state.bound[serialized.symbol_index] = 1;
	return image_restore::restored;
}

/* Every byte is read, every symbol has a name, and exactly the symbols
 * other than runtime helpers are bound. */
static image_restore check_image_symbols(const image_deserializer &state) {
	const zend_native_image *image = state.image;
	if (state.offset != state.size) {
		return image_restore::invalid;
	}
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		const zend_native_image_symbol &symbol = image->symbols[index];
		if (std::memchr(symbol.name, '\0', sizeof(symbol.name)) == nullptr
				|| (symbol.kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER
					? state.bound[index]
					: !state.bound[index])) {
			return image_restore::invalid;
		}
	}
	return image_restore::restored;
}

/* Destroys what a failed restore built and reports why it failed. */
static zend_result fail_image_restore(
	image_deserializer &state,
	image_restore status,
	zend_native_diagnostic *diag) {
	zend_native_image_destroy(state.image);
	if (status == image_restore::invalid) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"serialized native image is incompatible or malformed");
	} else {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to restore serialized native image");
	}
	return FAILURE;
}

extern "C" zend_result zend_native_image_deserialize(
	const unsigned char *bytes,
	size_t size,
	zend_native_image_decode_reference_t decode_reference,
	void *reference_context,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	image_deserializer state{};
	state.bytes = bytes;
	state.size = size;
	state.decode_reference = decode_reference;
	state.reference_context = reference_context;

	if (bytes == nullptr || out_image == nullptr
			|| decode_reference == nullptr
			|| size < sizeof(state.header)
			|| size > MAX_NATIVE_IMAGE_BYTES) {
		return fail_image_restore(state, image_restore::invalid, diag);
	}
	*out_image = nullptr;
	image_restore status = read_image_header(state);
	if (status == image_restore::restored) {
		status = restore_image_sections(state);
	}
	for (uint32_t binding_index = 0; status == image_restore::restored
			&& binding_index < state.header.binding_count; ++binding_index) {
		status = restore_image_binding(state);
	}
	if (status == image_restore::restored) {
		status = check_image_symbols(state);
	}
	if (status != image_restore::restored) {
		return fail_image_restore(state, status, diag);
	}
	*out_image = state.image;
	return SUCCESS;
}

extern "C" void zend_native_serialized_image_destroy(
	unsigned char *bytes) {
	std::free(bytes);
}

extern "C" zend_result zend_native_publish_image(
	zend_native_target target,
	zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
	if (image == nullptr || out_code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"image and out_code are required");
		return FAILURE;
	}
	*out_code = nullptr;
	if (target != image->target) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
			"publish target differs from compiled image target");
		return FAILURE;
	}
	zend_result result = target == ZEND_NATIVE_HOST_TARGET
		? zend_native_publish_host(image, out_code, diag)
		: FAILURE;
	if (result == SUCCESS && *out_code != nullptr
			&& (*out_code)->unwind_registered) {
		live_unwind_registrations.fetch_add(1, std::memory_order_relaxed);
	}
	if (result == SUCCESS && *out_code != nullptr) {
		(*out_code)->owner = *out_code;
		(*out_code)->owner_refcount = 1;
		(*out_code)->component_metadata = image->component_entries;
		(*out_code)->component_entry_count = image->component_entry_count;
		image->component_entries = nullptr;
		image->component_entry_count = 0;
		/*
		 * Direct-call descriptors are resolved into process-local relocation
		 * slots at publication. Transfer their storage with the mapping so
		 * callers may destroy the intermediate image immediately afterward.
		 */
		move_call_descriptors(*out_code, image);
	}
	return result;
}

extern "C" void zend_native_image_destroy(zend_native_image *image) {
	if (image != nullptr) {
		free_call_descriptors(image);
		free_internal_call_cells(image);
		std::free(image->component_entries);
		if (image->destroy_target_state != nullptr) {
			image->destroy_target_state(image->target_state);
		}
		native_image_symbol_lookup_destroy(image);
		std::free(image->symbol_bindings);
		std::free(image->symbols);
		std::free(image->text);
		std::free(image);
	}
}

extern "C" void zend_native_code_destroy(zend_native_code *code) {
	if (code == nullptr) {
		return;
	}
	zend_native_code *owner = code->owner != nullptr ? code->owner : code;
	const uint32_t previous = __atomic_fetch_sub(
		&owner->owner_refcount, 1, __ATOMIC_ACQ_REL);
	ZEND_ASSERT(previous != 0);
	if (code != owner) {
		std::free(code);
	}
	if (previous != 1) {
		return;
	}
	free_call_descriptors(owner);
	free_internal_call_cells(owner);
	std::free(owner->component_entries);
	std::free(owner->component_fast_entries);
	std::free(owner->component_exact_entries);
	std::free(owner->component_metadata);
	if (owner->unwind_registered) {
		uint32_t unwind_previous = live_unwind_registrations.fetch_sub(
			1, std::memory_order_relaxed);
		ZEND_ASSERT(unwind_previous != 0);
		owner->unwind_registered = false;
	}
	zend_native_unmap_host(owner);
	std::free(owner);
}

extern "C" zend_result zend_native_code_component_view(
	zend_native_code *code,
	uint32_t component_index,
	zend_native_code **out_view,
	zend_native_diagnostic *diag) {
	if (code == nullptr || out_view == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native code and output view are required");
		return FAILURE;
	}
	zend_native_code *owner = code->owner != nullptr ? code->owner : code;
	if (component_index >= owner->component_entry_count
			|| owner->component_entries == nullptr
			|| owner->component_metadata == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native component entry index is outside the published image");
		return FAILURE;
	}
	if (component_index == 0 && code == owner) {
		*out_view = code;
		return SUCCESS;
	}
	zend_native_code *view = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*view)));
	if (view == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate native component code view");
		return FAILURE;
	}
	view->target = owner->target;
	view->owner = owner;
	view->mapping = owner->mapping;
	view->mapping_size = owner->mapping_size;
	view->entry = owner->component_entries[component_index];
	view->fast_call_entry = owner->component_fast_entries != nullptr
		? owner->component_fast_entries[component_index] : nullptr;
	view->exact_call_entry = owner->component_exact_entries != nullptr
		? owner->component_exact_entries[component_index] : nullptr;
	view->argument_count =
		owner->component_metadata[component_index].argument_count;
	view->frame_variable_count =
		owner->component_metadata[component_index].frame_variable_count;
	view->frame_temporary_count =
		owner->component_metadata[component_index].frame_temporary_count;
	view->writable = owner->writable;
	view->executable = owner->executable;
	__atomic_fetch_add(&owner->owner_refcount, 1, __ATOMIC_RELAXED);
	*out_view = view;
	return SUCCESS;
}

extern "C" const char *zend_native_target_id(zend_native_target target) {
	return target == ZEND_NATIVE_TARGET_DARWIN_ARM64 ? "darwin-arm64-dev"
		: target == ZEND_NATIVE_TARGET_LINUX_AMD64 ? "linux-amd64-prod" : "invalid";
}

extern "C" const char *zend_native_target_triple(zend_native_target target) {
	return target == ZEND_NATIVE_TARGET_DARWIN_ARM64 ? "arm64-apple-darwin"
		: target == ZEND_NATIVE_TARGET_LINUX_AMD64 ? "x86_64-unknown-linux-gnu" : "invalid";
}

extern "C" size_t zend_native_image_size(const zend_native_image *image) {
	return image == nullptr ? 0 : image->text_size;
}

extern "C" const unsigned char *zend_native_image_bytes(const zend_native_image *image) {
	return image == nullptr ? nullptr : image->text;
}

extern "C" void zend_native_image_get_metrics(
		const zend_native_image *image, zend_native_image_metrics *metrics) {
	if (metrics == nullptr) {
		return;
	}
	*metrics = image != nullptr
		? image->metrics : zend_native_image_metrics{};
}

extern "C" bool zend_native_code_is_writable(const zend_native_code *code) {
	return code != nullptr && code->writable;
}

extern "C" bool zend_native_code_is_executable(const zend_native_code *code) {
	return code != nullptr && code->executable;
}

extern "C" bool zend_native_code_has_unwind_info(const zend_native_code *code) {
	return code != nullptr && code->unwind_registered;
}

extern "C" uint32_t zend_native_live_unwind_registration_count(void) {
	return live_unwind_registrations.load(std::memory_order_relaxed);
}

extern "C" bool zend_native_code_contains_address(
		const zend_native_code *code, const void *address) {
	if (code == nullptr || code->mapping == nullptr || address == nullptr) {
		return false;
	}
	const auto begin = reinterpret_cast<uintptr_t>(code->mapping);
	const auto candidate = reinterpret_cast<uintptr_t>(address);
	return candidate >= begin && candidate - begin < code->mapping_size;
}

extern "C" zend_native_frame_entry_t zend_native_code_frame_entry(
	const zend_native_code *code) {
	return code != nullptr ? code->entry : nullptr;
}

extern "C" zend_native_frame_entry_t zend_native_code_executable_entry(
	const zend_native_code *code) {
	return code != nullptr && code->executable ? code->entry : nullptr;
}

extern "C" void *zend_native_code_fast_call_entry(
	const zend_native_code *code) {
	return code != nullptr ? code->fast_call_entry : nullptr;
}

extern "C" void *zend_native_code_exact_call_entry(
	const zend_native_code *code) {
	return code != nullptr ? code->exact_call_entry : nullptr;
}
