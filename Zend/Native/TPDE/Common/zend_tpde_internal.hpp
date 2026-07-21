// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"

#include <cstddef>
#include <cstdint>

struct zend_tpde_value {
	zend_mir_value_id id;
	zend_mir_representation representation;
	int32_t argument_index;
	bool constant;
	uint64_t constant_bits;
};

struct zend_tpde_instruction {
	zend_mir_instruction_record record;
	uint32_t operand_offset;
	uint32_t operand_count;
};

struct zend_tpde_plan {
	const zend_mir_view *view;
	zend_mir_function_record function;
	zend_mir_block_record *blocks;
	uint32_t block_count;
	zend_tpde_value *values;
	uint32_t value_count;
	zend_tpde_instruction *instructions;
	uint32_t instruction_count;
	zend_mir_value_id *operands;
	uint32_t operand_count;
	uint32_t argument_count;
};

struct zend_native_image {
	zend_native_target target;
	unsigned char *text;
	size_t text_size;
	size_t text_capacity;
	uint32_t slot_count;
	uint32_t argument_count;
};

typedef void (*zend_native_entry)(
	const zend_native_scalar *, uint64_t *, zend_native_scalar *);

struct zend_native_code {
	zend_native_target target;
	void *mapping;
	size_t mapping_size;
	zend_native_entry entry;
	uint32_t slot_count;
	uint32_t argument_count;
	bool writable;
	bool executable;
};

void zend_tpde_set_diagnostic(
	zend_native_diagnostic *diag,
	zend_native_diagnostic_code code,
	const char *message);
int32_t zend_tpde_value_index(
	const zend_tpde_plan *plan, zend_mir_value_id id);
const zend_tpde_instruction *zend_tpde_instruction_at(
	const zend_tpde_plan *plan, uint32_t index);
zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index);
bool zend_tpde_image_append(
	zend_native_image *image, const void *bytes, size_t length);
bool zend_tpde_image_u8(zend_native_image *image, uint8_t value);
bool zend_tpde_image_u32(zend_native_image *image, uint32_t value);
bool zend_tpde_image_u64(zend_native_image *image, uint64_t value);

zend_result zend_tpde_emit_darwin_arm64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag);
zend_result zend_tpde_emit_linux_x64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag);

zend_result zend_native_publish_darwin_arm64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag);
zend_result zend_native_publish_linux_x64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag);
void zend_native_unmap_darwin_arm64(zend_native_code *code);
void zend_native_unmap_linux_x64(zend_native_code *code);
