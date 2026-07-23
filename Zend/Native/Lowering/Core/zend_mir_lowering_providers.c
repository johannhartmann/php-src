/*
   +----------------------------------------------------------------------+
   | Zend Engine                                                          |
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   +----------------------------------------------------------------------+
*/

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/zend_compile.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"
#include "Zend/Native/Calls/Model/zend_mir_call_model.h"
#include "Zend/Native/Values/Lowering/zend_mir_value_lowering.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source_internal.h"
#include "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic.h"
#include "Zend/Native/Lowering/Scalar/Numeric/zend_mir_lower_numeric.h"
#include "Zend/Native/Lowering/StraightLine/zend_mir_straight_line.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"

#include "zend_mir_lowering_internal.h"
#include "zend_mir_lowering_w04.h"

#define ZEND_MIR_W03_MODULE_ID UINT32_C(0)
#define ZEND_MIR_W03_OP_ARRAY_ID UINT32_C(0)
#define ZEND_MIR_W03_FILE_SYMBOL_ID UINT32_C(0)
#define ZEND_MIR_W03_FUNCTION_SYMBOL_ID UINT32_C(0)
#define ZEND_MIR_W03_CODE_VERSION_ID UINT32_C(1)
#define ZEND_MIR_W03_NOP_FAMILY_ID UINT32_C(1)

typedef struct _zend_mir_w03_integration zend_mir_w03_integration;

typedef enum _zend_mir_w11_fact_mode {
	ZEND_MIR_W11_FACT_ORIGINAL = 0,
	ZEND_MIR_W11_FACT_NULL = 1,
	ZEND_MIR_W11_FACT_HIDDEN = 2
} zend_mir_w11_fact_mode;

struct _zend_mir_w03_integration {
	zend_op_array projected_op_array;
	zend_ssa projected_ssa;
	zend_op *projected_opcodes;
	zval *projected_literals;
	zend_ssa_op *projected_ssa_ops;
	zend_ssa_var *projected_ssa_vars;
	zend_ssa_var_info *projected_ssa_var_info;
	zend_mir_zend_source source;
	zend_mir_lowering_source_view base_source_view;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_source_view provider_source_view;
	uint8_t *w11_suppressed_opcodes;
	int *w11_ssa_replacements;
	uint8_t *w11_fact_modes;
	uint32_t w11_ssa_use_count;
	uint32_t w11_ssa_def_count;
	uint32_t w11_fact_count;
	zend_mir_value_fact_ref *facts;
	uint32_t fact_count;
	zend_mir_logic_value_binding *logic_bindings;
	uint32_t logic_binding_count;
	zend_mir_logic_opcode_proof *logic_proofs;
	uint32_t logic_proof_count;
	zend_mir_straight_line_value *lifetime_values;
	zend_mir_straight_line_lifetime lifetime;
	zend_mir_straight_line_slot *slots;
	zend_mir_straight_line_entry entry;
	zend_mir_straight_line_provider_context lifetime_context;
	zend_mir_numeric_provider_context numeric_context;
	zend_mir_numeric_provider_set numeric_providers;
	zend_mir_logic_context logic_context;
	zend_mir_lowering_provider logic_provider;
	zend_mir_lowering_provider lifetime_provider;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_context lowering_context;
	zend_mir_lowering_module_ops target_module_ops;
	zend_mir_lowering_module_ops module_ops;
	zend_mir_module *module;
	zend_mir_mutator *target_mutator;
	zend_mir_mutator mutator;
	bool module_seeded;
	bool w04;
	bool w05;
	bool w06;
	bool w08;
	bool w09;
	bool w10;
	bool w11;
	const zend_op_array *w09_op_array;
	bool deferred_finalize;
};

static zend_mir_lowering_result zend_mir_w03_result(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code)
{
	zend_mir_lowering_result result;

	result.status = status;
	result.diagnostic_code = code;
	result.guarantees = 0;
	result.module = NULL;
	return result;
}

static zend_mir_w05_lowering_result zend_mir_w05_integration_result(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code)
{
	zend_mir_w05_lowering_result result;

	memset(&result, 0, sizeof(result));
	result.lowering = zend_mir_w03_result(status, code);
	return result;
}

static void zend_mir_w06_sanitize_prerequisite_facts(
	zend_mir_w03_integration *integration);
static void zend_mir_w06_hide_private_call_results(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array, const zend_ssa *ssa);
static void zend_mir_w08_hide_byref_argument_facts(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array, const zend_ssa *ssa);
static void zend_mir_w08_hide_method_receiver_facts(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array, const zend_ssa *ssa);

static bool zend_mir_w03_value_fragment(
	const zend_mir_w03_integration *integration, uint32_t opcode)
{
	bool w09_iterator_control = opcode == ZEND_FE_RESET_R
		|| opcode == ZEND_FE_FETCH_R
		|| opcode == ZEND_FE_RESET_RW
		|| opcode == ZEND_FE_FETCH_RW;

	return integration->w06
		&& (integration->w09
			? ((integration->w10
				&& opcode == ZEND_VERIFY_RETURN_TYPE)
				|| (integration->w10
				? (integration->w11
					? zend_mir_w11_opcode_is_executable(opcode)
					: zend_mir_w10_opcode_is_executable(opcode))
				: zend_mir_w09_opcode_is_executable(opcode)))
				&& !w09_iterator_control
			: zend_mir_w06_opcode_is_accepted(opcode));
}

static bool zend_mir_w09_source_value_branch(uint32_t opcode)
{
	return opcode == ZEND_JMPZ || opcode == ZEND_JMPNZ
		|| opcode == ZEND_JMPZ_EX || opcode == ZEND_JMPNZ_EX;
}

static bool zend_mir_w03_receive_fragment(
	const zend_mir_w03_integration *integration, uint32_t opcode)
{
	return opcode == ZEND_RECV
		|| (integration != NULL && integration->w09
			&& (opcode == ZEND_RECV_INIT || opcode == ZEND_RECV_VARIADIC));
}

static bool zend_mir_w03_checked_add(
	uint32_t left, uint32_t right, uint32_t *out)
{
	if (out == NULL || left > UINT32_MAX - right) {
		return false;
	}
	*out = left + right;
	return true;
}

static void *zend_mir_w03_calloc(uint32_t count, size_t size)
{
	if (count == 0) {
		return NULL;
	}
	if (size != 0 && count > SIZE_MAX / size) {
		return NULL;
	}
	return calloc(count, size);
}

static void zend_mir_w03_clear_ssa_operand(zend_ssa *ssa, int *operand)
{
	if (*operand >= 0 && *operand < ssa->vars_count) {
		ssa->vars[*operand].definition = -1;
	}
	*operand = -1;
}

static void zend_mir_w06_hide_ssa_definition(
	zend_mir_w03_integration *integration, int definition)
{
	zend_ssa_var *variable;
	zend_ssa_var_info *info;

	if (definition < 0
			|| definition >= integration->projected_ssa.vars_count) {
		return;
	}
	variable = &integration->projected_ssa_vars[definition];
	info = &integration->projected_ssa_var_info[definition];
	variable->alias = NO_ALIAS;
	info->type = MAY_BE_NULL;
	info->guarded_reference = 0;
	info->indirect_reference = 0;
	info->ce = NULL;
	info->is_instanceof = 0;
	info->has_range = 0;
}

static void zend_mir_w06_record_projected_definition(
	zend_mir_w03_integration *integration,
	int *replacements, int definition, int replacement)
{
	if (definition >= 0) {
		replacements[definition] = replacement;
		if (replacement >= 0) {
			/*
			 * Every projected use now names the predecessor value. Keeping
			 * a scalar fact for the removed definition would materialize an
			 * unreferenced MIR value with neither a definition nor an entry
			 * publication.
			 */
			integration->projected_ssa_var_info[definition].type = 0;
		}
	}
}

static int zend_mir_w06_projected_replacement(
	const int *replacements, int vars_count, int value)
{
	int steps = 0;

	while (value >= 0 && value < vars_count
			&& replacements[value] >= 0
			&& replacements[value] != value
			&& steps++ < vars_count) {
		value = replacements[value];
	}
	return value;
}

/*
 * W04 sees a scalar prerequisite projection in which W06-owned instructions
 * are absent. Preserve the value flowing through an op1/op2 definition so a
 * later retained instruction never refers to the removed definition. This
 * deliberately rewrites only the private projected op table: the original
 * SSA, including PHI inputs, remains the source of W06 ownership evidence.
 */
static void zend_mir_w06_apply_projected_replacements(
	zend_mir_w03_integration *integration, const int *replacements)
{
	uint32_t index;

	for (index = 0; index < integration->projected_op_array.last; index++) {
		zend_ssa_op *ssa_op = &integration->projected_ssa_ops[index];

		ssa_op->op1_use = zend_mir_w06_projected_replacement(
			replacements, integration->projected_ssa.vars_count,
			ssa_op->op1_use);
		ssa_op->op2_use = zend_mir_w06_projected_replacement(
			replacements, integration->projected_ssa.vars_count,
			ssa_op->op2_use);
		ssa_op->result_use = zend_mir_w06_projected_replacement(
			replacements, integration->projected_ssa.vars_count,
			ssa_op->result_use);
	}
}

static bool zend_mir_w05_call_fragment(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_CHECK_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_PLACEHOLDER:
		case ZEND_CHECK_UNDEF_ARGS:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DO_ICALL:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			return true;
		default:
			return false;
	}
}

static bool zend_mir_w08_exception_fragment(uint8_t opcode)
{
	return opcode == ZEND_CATCH;
}

static bool zend_mir_w05_call_init_fragment(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
			return true;
		default:
			return false;
	}
}

static bool zend_mir_w05_call_completion_fragment(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DO_ICALL:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			return true;
		default:
			return false;
	}
}

static int zend_mir_w11_resolve_ssa(
	const zend_mir_w03_integration *integration, int value)
{
	int steps = 0;
	int replacement;

	if (integration == NULL || integration->w11_ssa_replacements == NULL) {
		return value;
	}
	while (value >= 0
			&& value < integration->source.ssa_count
			&& steps++ < integration->source.ssa_count) {
		replacement = integration->w11_ssa_replacements[value];
		if (replacement < 0 || replacement == value) {
			break;
		}
		value = replacement;
	}
	return value;
}

static bool zend_mir_w11_ssa_operand(
	const zend_ssa_op *ssa_op, uint32_t operand_index,
	int *use, int *definition)
{
	if (ssa_op == NULL || use == NULL || definition == NULL) {
		return false;
	}
	switch (operand_index) {
		case ZEND_MIR_FRONTEND_OP1:
			*use = ssa_op->op1_use;
			*definition = ssa_op->op1_def;
			return true;
		case ZEND_MIR_FRONTEND_OP2:
			*use = ssa_op->op2_use;
			*definition = ssa_op->op2_def;
			return true;
		case ZEND_MIR_FRONTEND_RESULT:
			*use = ssa_op->result_use;
			*definition = ssa_op->result_def;
			return true;
		default:
			return false;
	}
}

static void zend_mir_w11_hide_definition(
	zend_mir_w03_integration *integration, int definition, int replacement)
{
	if (definition < 0
			|| definition >= integration->source.ssa_count) {
		return;
	}
	integration->w11_ssa_replacements[definition] = replacement;
	integration->w11_fact_modes[definition] = replacement >= 0
		? ZEND_MIR_W11_FACT_HIDDEN : ZEND_MIR_W11_FACT_NULL;
}

static bool zend_mir_w11_original_fact_is_pointer(
	const zend_ssa_var *variable, const zend_ssa_var_info *info)
{
	const uint32_t pointer_types =
		MAY_BE_STRING | MAY_BE_ARRAY | MAY_BE_OBJECT | MAY_BE_RESOURCE
		| MAY_BE_REF | MAY_BE_INDIRECT | MAY_BE_CLASS | MAY_BE_RC1
		| MAY_BE_RCN | MAY_BE_ARRAY_KEY_ANY | MAY_BE_ARRAY_OF_ANY
		| MAY_BE_ARRAY_OF_REF;

	return variable->alias != NO_ALIAS || info->guarded_reference
		|| info->indirect_reference || info->ce != NULL
		|| info->is_instanceof || (info->type & pointer_types) != 0;
}

static bool zend_mir_w11_prepare_overlay(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array,
	const zend_ssa *ssa)
{
	uint32_t index;

	if (integration == NULL || op_array == NULL || ssa == NULL
			|| (op_array->last != 0
				&& (op_array->opcodes == NULL || ssa->ops == NULL))
			|| ssa->vars_count < 0
			|| (ssa->vars_count != 0
				&& (ssa->vars == NULL || ssa->var_info == NULL))) {
		return false;
	}
	integration->w11_suppressed_opcodes = zend_mir_w03_calloc(
		op_array->last == 0 ? 1 : op_array->last,
		sizeof(*integration->w11_suppressed_opcodes));
	integration->w11_ssa_replacements = zend_mir_w03_calloc(
		ssa->vars_count == 0 ? 1 : (uint32_t) ssa->vars_count,
		sizeof(*integration->w11_ssa_replacements));
	integration->w11_fact_modes = zend_mir_w03_calloc(
		ssa->vars_count == 0 ? 1 : (uint32_t) ssa->vars_count,
		sizeof(*integration->w11_fact_modes));
	if (integration->w11_suppressed_opcodes == NULL
			|| integration->w11_ssa_replacements == NULL
			|| integration->w11_fact_modes == NULL) {
		return false;
	}
	for (index = 0; index < (uint32_t) ssa->vars_count; index++) {
		integration->w11_ssa_replacements[index] = -1;
		if (zend_mir_w11_original_fact_is_pointer(
				&ssa->vars[index], &ssa->var_info[index])) {
			integration->w11_fact_modes[index] = ZEND_MIR_W11_FACT_NULL;
		}
	}
	for (index = 0; index < op_array->last; index++) {
		uint8_t opcode = op_array->opcodes[index].opcode;
		const zend_ssa_op *ssa_op = &ssa->ops[index];
		bool value_fragment = zend_mir_w03_value_fragment(
			integration, opcode);
		bool call_fragment = zend_mir_w05_call_fragment(opcode);

		if (opcode == ZEND_RETURN_BY_REF) {
			continue;
		}
		if (!zend_mir_w03_receive_fragment(integration, opcode)
				&& !call_fragment && !value_fragment
				&& !zend_mir_w08_exception_fragment(opcode)) {
			continue;
		}
		integration->w11_suppressed_opcodes[index] = 1;
		if (value_fragment || call_fragment) {
			zend_mir_w11_hide_definition(
				integration, ssa_op->op1_def, ssa_op->op1_use);
			zend_mir_w11_hide_definition(
				integration, ssa_op->op2_def, ssa_op->op2_use);
			zend_mir_w11_hide_definition(
				integration, ssa_op->result_def, ssa_op->result_use);
		}
	}

	/* Results consumed exclusively by source-backed call/value operations do
	 * not belong to the scalar prerequisite view. */
	for (index = 0; index < op_array->last; index++) {
		int result = ssa->ops[index].result_def;
		uint32_t use_index;
		bool seen = false;
		bool private_result = true;

		if (!zend_mir_w05_call_completion_fragment(
				op_array->opcodes[index].opcode)
				|| result < 0 || result >= ssa->vars_count) {
			continue;
		}
		for (use_index = 0; use_index < op_array->last; use_index++) {
			const zend_ssa_op *use = &ssa->ops[use_index];
			uint8_t opcode;
			if (use->op1_use != result && use->op2_use != result
					&& use->result_use != result) {
				continue;
			}
			seen = true;
			opcode = op_array->opcodes[use_index].opcode;
			if (!zend_mir_w06_opcode_is_accepted(opcode)
					&& !zend_mir_w05_call_fragment(opcode)) {
				private_result = false;
				break;
			}
		}
		if (seen && private_result) {
			integration->w11_fact_modes[result] =
				ZEND_MIR_W11_FACT_HIDDEN;
		}
	}
	for (index = 0; index < op_array->last; index++) {
		const zend_ssa_op *ssa_op = &ssa->ops[index];
		int receiver;

		if (op_array->opcodes[index].opcode == ZEND_SEND_REF) {
			if (ssa_op->op1_use >= 0
					&& ssa_op->op1_use < ssa->vars_count) {
				integration->w11_fact_modes[ssa_op->op1_use] =
					ZEND_MIR_W11_FACT_HIDDEN;
			}
			if (ssa_op->op1_def >= 0
					&& ssa_op->op1_def < ssa->vars_count) {
				integration->w11_fact_modes[ssa_op->op1_def] =
					ZEND_MIR_W11_FACT_HIDDEN;
			}
		}
		if (op_array->opcodes[index].opcode != ZEND_INIT_METHOD_CALL
				|| op_array->opcodes[index].op1_type == IS_UNUSED) {
			continue;
		}
		receiver = ssa_op->op1_use;
		if (receiver >= 0 && receiver < ssa->vars_count) {
			integration->w11_fact_modes[receiver] =
				ZEND_MIR_W11_FACT_HIDDEN;
		}
	}
	for (index = 0; index < (uint32_t) ssa->vars_count; index++) {
		const zend_ssa_var *variable = &ssa->vars[index];
		if (variable->var >= op_array->last_var
				&& variable->definition == -1
				&& variable->definition_phi == NULL
				&& variable->use_chain == -1
				&& variable->phi_use_chain == NULL
				&& variable->sym_use_chain == NULL) {
			integration->w11_fact_modes[index] =
				ZEND_MIR_W11_FACT_HIDDEN;
		}
	}

	for (index = 0; index < op_array->last; index++) {
		uint32_t operand_index;
		if (integration->w11_suppressed_opcodes[index]) {
			continue;
		}
		for (operand_index = 0; operand_index < 3; operand_index++) {
			int use;
			int definition;
			if (!zend_mir_w11_ssa_operand(
					&ssa->ops[index], operand_index, &use, &definition)) {
				return false;
			}
			if (use >= 0) {
				if (integration->w11_ssa_use_count == ZEND_MIR_ID_MAX) {
					return false;
				}
				integration->w11_ssa_use_count++;
			}
			if (definition >= 0) {
				if (integration->w11_ssa_def_count == ZEND_MIR_ID_MAX) {
					return false;
				}
				integration->w11_ssa_def_count++;
			}
		}
	}
	for (index = 0; index < (uint32_t) ssa->vars_count; index++) {
		zend_mir_value_fact_ref ignored;
		if (integration->w11_fact_modes[index] == ZEND_MIR_W11_FACT_NULL
				|| (integration->w11_fact_modes[index]
						== ZEND_MIR_W11_FACT_ORIGINAL
					&& zend_mir_frontend_fact_for_ssa(
						op_array, ssa, index, &ignored))) {
			if (integration->w11_fact_count == ZEND_MIR_ID_MAX) {
				return false;
			}
			integration->w11_fact_count++;
		}
	}
	return true;
}

static uint32_t zend_mir_w11_view_opcode_count(const void *context)
{
	const zend_mir_w03_integration *integration = context;
	return integration == NULL ? 0 : integration->source.opcode_count;
}

static void zend_mir_w11_unused_operand(zend_mir_source_operand_ref *operand)
{
	memset(operand, 0, sizeof(*operand));
	operand->kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	operand->slot_kind = ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	operand->index = ZEND_MIR_ID_INVALID;
	operand->ssa_variable_id = ZEND_MIR_ID_INVALID;
}

static bool zend_mir_w11_view_opcode_at(
	const void *context, uint32_t index, zend_mir_source_opcode_ref *out)
{
	const zend_mir_w03_integration *integration = context;
	const zend_ssa *ssa;
	const zend_ssa_op *ssa_op;

	if (integration == NULL || out == NULL
			|| index >= integration->source.opcode_count
			|| !integration->base_source_view.opcode_at(
				integration->base_source_view.context, index, out)) {
		return false;
	}
	if (integration->w11_suppressed_opcodes[index]) {
		/*
		 * CATCH is structural control flow, not a prerequisite value
		 * operation.  Keep its branch spelling while hiding its Zend-private
		 * operands exactly as the historical projected source did.
		 */
		if (out->zend_opcode_number != ZEND_CATCH) {
			out->zend_opcode_number = ZEND_NOP;
		}
		out->extended_value = 0;
		zend_mir_w11_unused_operand(&out->op1);
		zend_mir_w11_unused_operand(&out->op2);
		zend_mir_w11_unused_operand(&out->result);
		return true;
	}
	if (out->zend_opcode_number == ZEND_RETURN_BY_REF) {
		out->zend_opcode_number = ZEND_RETURN;
	}
	ssa = zend_mir_source_ssa(&integration->source);
	ssa_op = &ssa->ops[index];
	if (out->op1.kind == ZEND_MIR_SOURCE_OPERAND_SSA
			&& ssa_op->op1_use >= 0) {
		out->op1.ssa_variable_id = (uint32_t) zend_mir_w11_resolve_ssa(
			integration, ssa_op->op1_use);
	}
	if (out->op2.kind == ZEND_MIR_SOURCE_OPERAND_SSA
			&& ssa_op->op2_use >= 0) {
		out->op2.ssa_variable_id = (uint32_t) zend_mir_w11_resolve_ssa(
			integration, ssa_op->op2_use);
	}
	if (out->result.kind == ZEND_MIR_SOURCE_OPERAND_SSA
			&& ssa_op->result_def < 0 && ssa_op->result_use >= 0) {
		out->result.ssa_variable_id = (uint32_t) zend_mir_w11_resolve_ssa(
			integration, ssa_op->result_use);
	}
	return true;
}

static uint32_t zend_mir_w11_view_ssa_count(const void *context)
{
	const zend_mir_w03_integration *integration = context;
	return integration == NULL ? 0 : integration->source.ssa_count;
}

static bool zend_mir_w11_view_ssa_at(
	const void *context, uint32_t index, zend_mir_source_ssa_ref *out)
{
	const zend_mir_w03_integration *integration = context;
	const zend_ssa *ssa;
	int definition;

	if (integration == NULL || out == NULL
			|| !integration->base_source_view.ssa_at(
				integration->base_source_view.context, index, out)) {
		return false;
	}
	ssa = zend_mir_source_ssa(&integration->source);
	definition = ssa->vars[index].definition;
	if (definition >= 0
			&& (uint32_t) definition < integration->source.opcode_count
			&& integration->w11_suppressed_opcodes[definition]) {
		out->definition_opline_index = ZEND_MIR_ID_INVALID;
	}
	return true;
}

static uint32_t zend_mir_w11_view_ssa_use_count(const void *context)
{
	const zend_mir_w03_integration *integration = context;
	return integration == NULL ? 0 : integration->w11_ssa_use_count;
}

static bool zend_mir_w11_nth_use_or_def(
	const zend_mir_w03_integration *integration, uint32_t requested,
	bool want_definition, uint32_t *opline_index_out,
	uint32_t *operand_index_out, int *ssa_id_out)
{
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	uint32_t current = 0;
	uint32_t index;

	if (integration == NULL || opline_index_out == NULL
			|| operand_index_out == NULL || ssa_id_out == NULL) {
		return false;
	}
	op_array = zend_mir_source_op_array(&integration->source);
	ssa = zend_mir_source_ssa(&integration->source);
	for (index = 0; index < op_array->last; index++) {
		uint32_t operand_index;
		if (integration->w11_suppressed_opcodes[index]) {
			continue;
		}
		for (operand_index = 0; operand_index < 3; operand_index++) {
			int use;
			int definition;
			if (!zend_mir_w11_ssa_operand(
					&ssa->ops[index], operand_index, &use, &definition)) {
				return false;
			}
			*ssa_id_out = want_definition ? definition
				: zend_mir_w11_resolve_ssa(integration, use);
			if (*ssa_id_out >= 0 && current++ == requested) {
				*opline_index_out = index;
				*operand_index_out = operand_index;
				return true;
			}
		}
	}
	return false;
}

static bool zend_mir_w11_view_ssa_use_at(
	const void *context, uint32_t index, zend_mir_source_ssa_use_ref *out)
{
	const zend_mir_w03_integration *integration = context;
	int ssa_id;

	if (integration == NULL || out == NULL
			|| index >= integration->w11_ssa_use_count
			|| !zend_mir_w11_nth_use_or_def(
				integration, index, false, &out->opline_index,
				&out->operand_index, &ssa_id)) {
		return false;
	}
	out->ssa_variable_id = (uint32_t) ssa_id;
	return true;
}

static uint32_t zend_mir_w11_view_ssa_def_count(const void *context)
{
	const zend_mir_w03_integration *integration = context;
	return integration == NULL ? 0 : integration->w11_ssa_def_count;
}

static bool zend_mir_w11_view_ssa_def_at(
	const void *context, uint32_t index, zend_mir_source_ssa_def_ref *out)
{
	const zend_mir_w03_integration *integration = context;
	zend_mir_source_opcode_ref opcode;
	zend_mir_source_operand_ref *operand;
	uint32_t operand_index;
	int ssa_id;

	if (integration == NULL || out == NULL
			|| index >= integration->w11_ssa_def_count
			|| !zend_mir_w11_nth_use_or_def(
				integration, index, true, &out->opline_index,
				&operand_index, &ssa_id)
			|| !zend_mir_w11_view_opcode_at(
				integration, out->opline_index, &opcode)) {
		return false;
	}
	operand = operand_index == ZEND_MIR_FRONTEND_OP1 ? &opcode.op1
		: operand_index == ZEND_MIR_FRONTEND_OP2 ? &opcode.op2
		: &opcode.result;
	out->ssa_variable_id = (uint32_t) ssa_id;
	out->destination = *operand;
	out->destination.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	out->destination.ssa_variable_id = (uint32_t) ssa_id;
	return true;
}

static uint32_t zend_mir_w11_view_literal_count(const void *context)
{
	const zend_mir_w03_integration *integration = context;
	return integration == NULL ? 0 : integration->source.literal_count;
}

static bool zend_mir_w11_view_literal_at(
	const void *context, uint32_t index, zend_mir_source_literal_ref *out)
{
	const zend_mir_w03_integration *integration = context;

	if (integration == NULL || out == NULL
			|| index >= integration->source.literal_count) {
		return false;
	}
	if (integration->base_source_view.literal_at(
			integration->base_source_view.context, index, out)) {
		return true;
	}
	memset(out, 0, sizeof(*out));
	out->literal_index = index;
	out->kind = ZEND_MIR_SOURCE_LITERAL_NULL;
	return true;
}

#define ZEND_MIR_W11_FORWARD_COUNT(name, field) \
	static uint32_t zend_mir_w11_view_##name##_count(const void *context) \
	{ \
		const zend_mir_w03_integration *integration = context; \
		return integration == NULL ? 0 \
			: integration->base_source_view.field##_count( \
				integration->base_source_view.context); \
	}

#define ZEND_MIR_W11_FORWARD_AT(name, type, field) \
	static bool zend_mir_w11_view_##name##_at( \
		const void *context, uint32_t index, type *out) \
	{ \
		const zend_mir_w03_integration *integration = context; \
		return integration != NULL \
			&& integration->base_source_view.field##_at( \
				integration->base_source_view.context, index, out); \
	}

ZEND_MIR_W11_FORWARD_COUNT(block, block)
ZEND_MIR_W11_FORWARD_AT(block, zend_mir_source_block_ref, block)
ZEND_MIR_W11_FORWARD_COUNT(edge, edge)
ZEND_MIR_W11_FORWARD_AT(edge, zend_mir_source_edge_ref, edge)
ZEND_MIR_W11_FORWARD_COUNT(phi, phi)
ZEND_MIR_W11_FORWARD_AT(phi, zend_mir_source_phi_ref, phi)
ZEND_MIR_W11_FORWARD_COUNT(phi_input, phi_input)
ZEND_MIR_W11_FORWARD_AT(
	phi_input, zend_mir_source_phi_input_ref, phi_input)

#undef ZEND_MIR_W11_FORWARD_COUNT
#undef ZEND_MIR_W11_FORWARD_AT

static bool zend_mir_w11_source_view(
	zend_mir_w03_integration *integration)
{
	if (integration == NULL
			|| !zend_mir_zend_source_view(
				&integration->source, &integration->base_source_view)) {
		return false;
	}
	integration->source_view = integration->base_source_view;
	integration->source_view.context = integration;
	integration->source_view.opcode_count = zend_mir_w11_view_opcode_count;
	integration->source_view.opcode_at = zend_mir_w11_view_opcode_at;
	integration->source_view.ssa_count = zend_mir_w11_view_ssa_count;
	integration->source_view.ssa_at = zend_mir_w11_view_ssa_at;
	integration->source_view.ssa_use_count = zend_mir_w11_view_ssa_use_count;
	integration->source_view.ssa_use_at = zend_mir_w11_view_ssa_use_at;
	integration->source_view.ssa_def_count = zend_mir_w11_view_ssa_def_count;
	integration->source_view.ssa_def_at = zend_mir_w11_view_ssa_def_at;
	integration->source_view.literal_count = zend_mir_w11_view_literal_count;
	integration->source_view.literal_at = zend_mir_w11_view_literal_at;
	integration->source_view.block_count = zend_mir_w11_view_block_count;
	integration->source_view.block_at = zend_mir_w11_view_block_at;
	integration->source_view.edge_count = zend_mir_w11_view_edge_count;
	integration->source_view.edge_at = zend_mir_w11_view_edge_at;
	integration->source_view.phi_count = zend_mir_w11_view_phi_count;
	integration->source_view.phi_at = zend_mir_w11_view_phi_at;
	integration->source_view.phi_input_count =
		zend_mir_w11_view_phi_input_count;
	integration->source_view.phi_input_at = zend_mir_w11_view_phi_input_at;
	return true;
}

/*
 * Required arguments are already materialized in the entry frame when W03
 * lowering begins. Project their RECV definitions to NOP/live-in values so
 * the frozen opcode profile remains unchanged and no call semantics enter MIR.
 */
static bool zend_mir_w03_prepare_source(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_op_array **projected_op_array,
	const zend_ssa **projected_ssa)
{
	uint32_t index;
	bool has_recv = false;
	int *w06_replacements = NULL;

	if (integration == NULL || op_array == NULL || ssa == NULL
			|| projected_op_array == NULL || projected_ssa == NULL) {
		return false;
	}
	for (index = 0; index < op_array->last; index++) {
		if (zend_mir_w03_receive_fragment(
				integration, op_array->opcodes[index].opcode)
				|| (integration->w05
					&& zend_mir_w05_call_fragment(
						op_array->opcodes[index].opcode))
				|| zend_mir_w03_value_fragment(
					integration, op_array->opcodes[index].opcode)
				|| (integration->w08
					&& zend_mir_w08_exception_fragment(
						op_array->opcodes[index].opcode))) {
			has_recv = true;
			break;
		}
	}
	/* Call lowering projects result and receiver facts even for functions that
	 * contain no call/receive fragment themselves (for example a trait method
	 * consisting only of an object-property read).  Give those functions a
	 * private SSA copy as well; the projection must never mutate optimizer SSA. */
	if (!has_recv && !integration->w05) {
		*projected_op_array = op_array;
		*projected_ssa = ssa;
		return true;
	}
	if ((op_array->last != 0 && (op_array->opcodes == NULL || ssa->ops == NULL))
			|| (op_array->last_literal != 0 && op_array->literals == NULL)
			|| ssa->vars_count < 0
			|| (ssa->vars_count != 0 && ssa->vars == NULL)) {
		return false;
	}
	{
		size_t opcode_bytes =
			(size_t) op_array->last * sizeof(*integration->projected_opcodes);
		size_t aligned_opcode_bytes;
		size_t literal_bytes =
			(size_t) op_array->last_literal * sizeof(*integration->projected_literals);
		size_t storage_bytes;

		if (opcode_bytes > SIZE_MAX - 15) {
			return false;
		}
		aligned_opcode_bytes = (opcode_bytes + 15) & ~(size_t) 15;
		if (aligned_opcode_bytes > SIZE_MAX - literal_bytes) {
			return false;
		}
		storage_bytes = aligned_opcode_bytes + literal_bytes;
		integration->projected_opcodes =
			calloc(1, storage_bytes == 0 ? 1 : storage_bytes);
		if (integration->projected_opcodes != NULL
				&& op_array->last_literal != 0) {
			integration->projected_literals = (zval *) (
				(char *) integration->projected_opcodes
				+ aligned_opcode_bytes);
		}
	}
	integration->projected_ssa_ops = zend_mir_w03_calloc(
		op_array->last, sizeof(*integration->projected_ssa_ops));
	integration->projected_ssa_vars = zend_mir_w03_calloc(
		(uint32_t) ssa->vars_count, sizeof(*integration->projected_ssa_vars));
	integration->projected_ssa_var_info = zend_mir_w03_calloc(
		(uint32_t) ssa->vars_count,
		sizeof(*integration->projected_ssa_var_info));
	if (integration->projected_opcodes == NULL
			|| integration->projected_ssa_ops == NULL
			|| (ssa->vars_count != 0
				&& (integration->projected_ssa_vars == NULL
					|| integration->projected_ssa_var_info == NULL
					|| ssa->var_info == NULL))) {
		return false;
	}
	memcpy(
		integration->projected_opcodes, op_array->opcodes,
		(size_t) op_array->last * sizeof(*integration->projected_opcodes));
	memcpy(
		integration->projected_ssa_ops, ssa->ops,
		(size_t) op_array->last * sizeof(*integration->projected_ssa_ops));
	if (ssa->vars_count != 0) {
		memcpy(
			integration->projected_ssa_vars, ssa->vars,
			(size_t) ssa->vars_count * sizeof(*integration->projected_ssa_vars));
		memcpy(
			integration->projected_ssa_var_info, ssa->var_info,
			(size_t) ssa->vars_count
				* sizeof(*integration->projected_ssa_var_info));
	}
	integration->projected_op_array = *op_array;
	integration->projected_op_array.opcodes = integration->projected_opcodes;
	integration->projected_op_array.literals = integration->projected_literals;
	if (op_array->last_literal != 0) {
		memcpy(
			integration->projected_literals, op_array->literals,
			(size_t) op_array->last_literal
				* sizeof(*integration->projected_literals));
	}
	integration->projected_ssa = *ssa;
	integration->projected_ssa.ops = integration->projected_ssa_ops;
	integration->projected_ssa.vars = integration->projected_ssa_vars;
	integration->projected_ssa.var_info =
		integration->projected_ssa_var_info;
	if (integration->w06 && ssa->vars_count != 0) {
		w06_replacements = malloc(
			(size_t) ssa->vars_count * sizeof(*w06_replacements));
		if (w06_replacements == NULL) {
			return false;
		}
		for (index = 0; index < (uint32_t) ssa->vars_count; index++) {
			w06_replacements[index] = -1;
		}
	}

	for (index = 0; index < op_array->last; index++) {
		const zend_op *original_opline = &op_array->opcodes[index];
		zend_op *opline = &integration->projected_opcodes[index];
		zend_ssa_op *ssa_op = &integration->projected_ssa_ops[index];
		ptrdiff_t literal_index;
		uint32_t lineno;

		if (opline->op1_type == IS_CONST) {
			literal_index =
				RT_CONSTANT(original_opline, original_opline->op1)
				- op_array->literals;
			if (literal_index < 0
					|| (uint32_t) literal_index >= op_array->last_literal) {
				free(w06_replacements);
				return false;
			}
#if ZEND_USE_ABS_CONST_ADDR
			opline->op1.zv =
				&integration->projected_literals[literal_index];
#else
			opline->op1.constant = (uint32_t) (
				(char *) &integration->projected_literals[literal_index]
					- (char *) opline);
#endif
			if (integration->w05
					&& (zend_mir_w05_call_init_fragment(opline->opcode)
						|| (zend_mir_w05_call_fragment(opline->opcode)
							&& Z_TYPE(op_array->literals[literal_index])
								> IS_DOUBLE))) {
				ZVAL_NULL(&integration->projected_literals[literal_index]);
			}
		}
		if (opline->op2_type == IS_CONST) {
			literal_index =
				RT_CONSTANT(original_opline, original_opline->op2)
				- op_array->literals;
			if (literal_index < 0
					|| (uint32_t) literal_index >= op_array->last_literal) {
				free(w06_replacements);
				return false;
			}
#if ZEND_USE_ABS_CONST_ADDR
			opline->op2.zv =
				&integration->projected_literals[literal_index];
#else
			opline->op2.constant = (uint32_t) (
				(char *) &integration->projected_literals[literal_index]
					- (char *) opline);
#endif
			if (integration->w05
					&& (zend_mir_w05_call_init_fragment(opline->opcode)
						|| (zend_mir_w05_call_fragment(opline->opcode)
							&& Z_TYPE(op_array->literals[literal_index])
								> IS_DOUBLE))) {
				ZVAL_NULL(&integration->projected_literals[literal_index]);
			}
		}
		/*
		 * W04 needs a terminator in the private prerequisite projection.
		 * W06 retains the original RETURN_BY_REF opcode and reference
		 * semantics in its source/value records; only the prerequisite CFG
		 * sees the operand-compatible scalar RETURN spelling.
		 */
		if (integration->w06
				&& opline->opcode == ZEND_RETURN_BY_REF) {
			opline->opcode = ZEND_RETURN;
			continue;
		}
		if (!zend_mir_w03_receive_fragment(integration, opline->opcode)
				&& !(integration->w05
					&& zend_mir_w05_call_fragment(opline->opcode))
				&& !zend_mir_w03_value_fragment(
					integration, opline->opcode)
				&& !(integration->w08
					&& zend_mir_w08_exception_fragment(opline->opcode))) {
			continue;
		}
		if ((zend_mir_w03_value_fragment(integration, opline->opcode)
					|| (integration->w06
						&& zend_mir_w05_call_fragment(opline->opcode)))) {
			zend_mir_w06_hide_ssa_definition(
				integration, ssa_op->op1_def);
			zend_mir_w06_hide_ssa_definition(
				integration, ssa_op->op2_def);
			zend_mir_w06_hide_ssa_definition(
				integration, ssa_op->result_def);
			zend_mir_w06_record_projected_definition(
				integration, w06_replacements,
				ssa_op->op1_def, ssa_op->op1_use);
			zend_mir_w06_record_projected_definition(
				integration, w06_replacements,
				ssa_op->op2_def, ssa_op->op2_use);
			zend_mir_w06_record_projected_definition(
				integration, w06_replacements, ssa_op->result_def,
				ssa_op->result_use);
		}
		zend_mir_w03_clear_ssa_operand(
			&integration->projected_ssa, &ssa_op->op1_def);
		zend_mir_w03_clear_ssa_operand(
			&integration->projected_ssa, &ssa_op->op2_def);
		zend_mir_w03_clear_ssa_operand(
			&integration->projected_ssa, &ssa_op->result_def);
		lineno = opline->lineno;
		if ((integration->w05 && zend_mir_w05_call_fragment(opline->opcode))
				|| zend_mir_w03_value_fragment(
					integration, opline->opcode)
				|| (integration->w08
					&& zend_mir_w08_exception_fragment(opline->opcode))) {
			zend_ssa_remove_instr(
				&integration->projected_ssa, opline, ssa_op);
			opline->lineno = lineno;
		} else {
			memset(opline, 0, sizeof(*opline));
			opline->opcode = ZEND_NOP;
			opline->lineno = lineno;
			ssa_op->op1_use = -1;
			ssa_op->op2_use = -1;
			ssa_op->result_use = -1;
			ssa_op->op1_def = -1;
			ssa_op->op2_def = -1;
			ssa_op->result_def = -1;
			ssa_op->op1_use_chain = -1;
			ssa_op->op2_use_chain = -1;
			ssa_op->res_use_chain = -1;
		}
	}
	if (w06_replacements != NULL) {
		zend_mir_w06_apply_projected_replacements(
			integration, w06_replacements);
		free(w06_replacements);
	}
	*projected_op_array = &integration->projected_op_array;
	*projected_ssa = &integration->projected_ssa;
	return true;
}

static const zend_mir_value_fact_ref *zend_mir_w03_fact_for_value(
	const zend_mir_w03_integration *integration,
	zend_mir_value_id value_id)
{
	uint32_t index;

	if (integration == NULL) {
		return NULL;
	}
	for (index = 0; index < integration->fact_count; index++) {
		if (integration->facts[index].value_id == value_id) {
			return &integration->facts[index];
		}
	}
	return NULL;
}

static bool zend_mir_w03_source_position(
	const void *context, zend_mir_source_position_id requested_id,
	zend_mir_source_position_ref *out)
{
	const zend_mir_w03_integration *integration = context;

	return integration != NULL
		&& zend_mir_zend_source_position_at(
			&integration->source, requested_id, out);
}

static bool zend_mir_w03_resolve_operand(
	const void *context, const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out)
{
	const zend_mir_w03_integration *integration = context;
	zend_mir_frame_slot_kind slot_kind;
	uint32_t index;

	if (integration == NULL || operand == NULL || value_id_out == NULL) {
		return false;
	}
	switch (operand->kind) {
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			*value_id_out = zend_mir_value_from_synthetic(operand->index);
			return zend_mir_id_is_valid(*value_id_out);
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			*value_id_out = zend_mir_value_from_original_ssa(
				operand->ssa_variable_id);
			return zend_mir_id_is_valid(*value_id_out);
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
			switch (operand->slot_kind) {
				case ZEND_MIR_SOURCE_SLOT_CV:
					slot_kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
					break;
				case ZEND_MIR_SOURCE_SLOT_TMP:
					slot_kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
					break;
				case ZEND_MIR_SOURCE_SLOT_VAR:
					slot_kind = ZEND_MIR_FRAME_SLOT_KIND_VAR;
					break;
				default:
					return false;
			}
			for (index = 0; index < integration->entry.slot_count; index++) {
				const zend_mir_straight_line_slot *slot =
					&integration->entry.slots[index];

				if (slot->kind == slot_kind
						&& slot->index == operand->index
						&& slot->materialization
							== ZEND_MIR_MATERIALIZATION_MATERIALIZED
						&& zend_mir_id_is_valid(slot->value_id)) {
					*value_id_out = slot->value_id;
					return true;
				}
			}
			return false;
		default:
			return false;
	}
}

static bool zend_mir_w03_value_fact(
	const void *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *fact_out)
{
	const zend_mir_w03_integration *integration = context;
	const zend_mir_value_fact_ref *fact =
		zend_mir_w03_fact_for_value(integration, value_id);

	if (fact == NULL || fact_out == NULL) {
		return false;
	}
	*fact_out = *fact;
	return true;
}

static bool zend_mir_w03_literal_fact(
	const zend_mir_source_literal_ref *literal,
	zend_mir_value_fact_ref *fact,
	zend_mir_constant_record *constant)
{
	double value;

	if (literal == NULL || fact == NULL || constant == NULL) {
		return false;
	}
	memset(fact, 0, sizeof(*fact));
	memset(constant, 0, sizeof(*constant));
	fact->id = ZEND_MIR_ID_INVALID;
	fact->value_id = zend_mir_value_from_synthetic(literal->literal_index);
	fact->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	fact->provenance = ZEND_MIR_FACT_PROVENANCE_LITERAL;
	fact->provenance_source_position_id = 0;
	constant->value_id = fact->value_id;
	constant->symbol_id = ZEND_MIR_ID_INVALID;

	switch (literal->kind) {
		case ZEND_MIR_SOURCE_LITERAL_NULL:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_NULL;
			constant->representation = ZEND_MIR_REPRESENTATION_ZVAL;
			constant->kind = ZEND_MIR_CONSTANT_KIND_NULL_VALUE;
			break;
		case ZEND_MIR_SOURCE_LITERAL_FALSE:
		case ZEND_MIR_SOURCE_LITERAL_TRUE:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_I1;
			constant->representation = ZEND_MIR_REPRESENTATION_I1;
			constant->kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
			constant->payload_bits =
				literal->kind == ZEND_MIR_SOURCE_LITERAL_TRUE ? 1 : 0;
			break;
		case ZEND_MIR_SOURCE_LITERAL_LONG_BITS:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_I64;
			fact->flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
			fact->integer_min = (int64_t) literal->payload_bits;
			fact->integer_max = (int64_t) literal->payload_bits;
			if ((int64_t) literal->payload_bits != 0) {
				fact->flags |= ZEND_MIR_VALUE_FACT_NONZERO;
			}
			constant->representation = ZEND_MIR_REPRESENTATION_I64;
			constant->kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
			constant->payload_bits = literal->payload_bits;
			break;
		case ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_F64;
			memcpy(&value, &literal->payload_bits, sizeof(value));
			if (isfinite(value)) {
				fact->flags |= ZEND_MIR_VALUE_FACT_FINITE;
			}
			constant->representation = ZEND_MIR_REPRESENTATION_DOUBLE;
			constant->kind = ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS;
			constant->payload_bits = literal->payload_bits;
			break;
		default:
			return false;
	}
	return zend_mir_id_is_valid(fact->value_id);
}

static bool zend_mir_w11_fact_for_ssa(
	const zend_mir_w03_integration *integration,
	uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *out)
{
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	uint8_t mode;
	int definition;

	if (integration == NULL || out == NULL
			|| ssa_variable_id >= integration->source.ssa_count) {
		return false;
	}
	mode = integration->w11_fact_modes[ssa_variable_id];
	if (mode == ZEND_MIR_W11_FACT_HIDDEN) {
		return false;
	}
	op_array = zend_mir_source_op_array(&integration->source);
	ssa = zend_mir_source_ssa(&integration->source);
	if (mode == ZEND_MIR_W11_FACT_ORIGINAL) {
		if (!zend_mir_frontend_fact_for_ssa(
				op_array, ssa, ssa_variable_id, out)) {
			return false;
		}
		definition = ssa->vars[ssa_variable_id].definition;
		if (definition >= 0 && (uint32_t) definition < op_array->last
				&& zend_mir_w05_call_completion_fragment(
					op_array->opcodes[definition].opcode)) {
			out->flags &= ~(uint32_t) (
				ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
				| ZEND_MIR_VALUE_FACT_NONZERO);
			out->integer_min = 0;
			out->integer_max = 0;
		}
		return true;
	}
	memset(out, 0, sizeof(*out));
	out->value_id = zend_mir_value_from_original_ssa(ssa_variable_id);
	out->exact_type = ZEND_MIR_SCALAR_TYPE_NULL;
	out->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	out->provenance = ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS;
	definition = ssa->vars[ssa_variable_id].definition;
	out->provenance_source_position_id = definition >= 0
		? (uint32_t) definition
		: (op_array->last == 0 ? ZEND_MIR_ID_INVALID : 0);
	return zend_mir_id_is_valid(out->value_id);
}

static bool zend_mir_w03_prepare_facts(
	zend_mir_w03_integration *integration)
{
	uint32_t literal_count =
		integration->source_view.literal_count(integration->source_view.context);
	uint32_t ssa_fact_count = integration->w11
		? integration->w11_fact_count
		: zend_mir_zend_source_value_fact_count(&integration->source);
	uint32_t capacity;
	uint32_t index;

	if (!zend_mir_w03_checked_add(
			literal_count, ssa_fact_count, &capacity)) {
		return false;
	}
	integration->facts = zend_mir_w03_calloc(
		capacity == 0 ? 1 : capacity, sizeof(*integration->facts));
	if (integration->facts == NULL) {
		return false;
	}
	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;
		zend_mir_constant_record constant;

		if (!integration->source_view.literal_at(
				integration->source_view.context, index, &literal)
				|| !zend_mir_w03_literal_fact(
					&literal, &integration->facts[integration->fact_count],
					&constant)) {
			return false;
		}
		integration->fact_count++;
	}
	if (integration->w11) {
		for (index = 0; index < integration->source.ssa_count; index++) {
			zend_mir_value_fact_ref fact;
			if (!zend_mir_w11_fact_for_ssa(integration, index, &fact)) {
				continue;
			}
			fact.id = integration->fact_count;
			integration->facts[integration->fact_count++] = fact;
		}
		if (integration->fact_count != capacity) {
			return false;
		}
	} else {
		for (index = 0; index < ssa_fact_count; index++) {
			if (!zend_mir_zend_source_value_fact_at(
					&integration->source, index,
					&integration->facts[integration->fact_count])) {
				return false;
			}
			integration->fact_count++;
		}
	}
	return true;
}

static bool zend_mir_w03_operand_equal(
	const zend_mir_source_operand_ref *left,
	const zend_mir_source_operand_ref *right)
{
	return left->kind == right->kind
		&& left->slot_kind == right->slot_kind
		&& left->index == right->index
		&& left->ssa_variable_id == right->ssa_variable_id;
}

static bool zend_mir_w03_add_logic_binding(
	zend_mir_w03_integration *integration,
	const zend_mir_source_operand_ref *operand)
{
	zend_mir_logic_value_binding *binding;
	zend_mir_value_id value_id;
	const zend_mir_value_fact_ref *fact;
	uint32_t index;

	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return true;
	}
	for (index = 0; index < integration->logic_binding_count; index++) {
		if (zend_mir_w03_operand_equal(
				&integration->logic_bindings[index].source, operand)) {
			return true;
		}
	}
	if (!zend_mir_w03_resolve_operand(integration, operand, &value_id)) {
		return false;
	}
	fact = zend_mir_w03_fact_for_value(integration, value_id);
	if (fact == NULL) {
		return false;
	}
	binding =
		&integration->logic_bindings[integration->logic_binding_count++];
	memset(binding, 0, sizeof(*binding));
	binding->source = *operand;
	binding->value_id = value_id;
	binding->has_fact = true;
	binding->fact = *fact;
	return true;
}

static bool zend_mir_w03_prepare_logic(
	zend_mir_w03_integration *integration)
{
	uint32_t opcode_count =
		integration->source_view.opcode_count(integration->source_view.context);
	uint32_t binding_capacity;
	uint32_t index;

	if (opcode_count > UINT32_MAX / 3) {
		return false;
	}
	binding_capacity = opcode_count * 3;
	integration->logic_bindings = zend_mir_w03_calloc(
		binding_capacity == 0 ? 1 : binding_capacity,
		sizeof(*integration->logic_bindings));
	integration->logic_proofs = zend_mir_w03_calloc(
		opcode_count == 0 ? 1 : opcode_count,
		sizeof(*integration->logic_proofs));
	if (integration->logic_bindings == NULL
			|| integration->logic_proofs == NULL) {
		return false;
	}
	for (index = 0; index < opcode_count; index++) {
		zend_mir_source_opcode_ref opcode;
		zend_mir_logic_opcode_proof *proof;
		uint32_t temporary_payload;
		bool source_zval_return;
		bool w09_executable;

		if (!integration->source_view.opcode_at(
				integration->source_view.context, index, &opcode)) {
			return false;
		}
		source_zval_return = integration->w08
			&& opcode.zend_opcode_number == ZEND_RETURN
			&& zend_mir_zend_source_w08_return_source_zval(
				&integration->source, opcode.opline_index);
		w09_executable = integration->w09
			&& ((integration->w10
					? (integration->w11
						? zend_mir_w11_opcode_is_executable(
							opcode.zend_opcode_number)
						: zend_mir_w10_opcode_is_executable(
							opcode.zend_opcode_number))
					: zend_mir_w09_opcode_is_executable(
						opcode.zend_opcode_number))
				|| zend_mir_w09_source_value_branch(
					opcode.zend_opcode_number)
				|| opcode.zend_opcode_number == ZEND_COALESCE
				|| opcode.zend_opcode_number == ZEND_JMP_SET
				|| opcode.zend_opcode_number == ZEND_CATCH
				|| opcode.zend_opcode_number == ZEND_FAST_CALL
				|| opcode.zend_opcode_number == ZEND_FAST_RET
				|| (integration->w10
					&& (opcode.zend_opcode_number == ZEND_JMP_NULL
						|| opcode.zend_opcode_number == ZEND_THROW)));
		if ((!w09_executable && !source_zval_return
				&& !zend_mir_w03_add_logic_binding(
					integration, &opcode.op1))
				|| (!w09_executable && !zend_mir_w03_add_logic_binding(
					integration, &opcode.op2))
				|| (!w09_executable && !zend_mir_w03_add_logic_binding(
					integration, &opcode.result))
				|| !zend_mir_w03_checked_add(
					integration->source.literal_count,
					opcode_count, &temporary_payload)
				|| !zend_mir_w03_checked_add(
					temporary_payload, index, &temporary_payload)) {
			return false;
		}
		proof = &integration->logic_proofs[integration->logic_proof_count++];
		proof->opline_index = opcode.opline_index;
		proof->proofs = ZEND_MIR_LOGIC_PROOF_ALL;
		if (integration->w04) {
			proof->proofs &=
				~ZEND_MIR_LOGIC_PROOF_SINGLE_REACHABLE_BLOCK;
			proof->proofs |= ZEND_MIR_LOGIC_PROOF_SOURCE_CFG;
		}
		proof->temporary_value_id =
			zend_mir_value_from_synthetic(temporary_payload);
		if (!zend_mir_id_is_valid(proof->temporary_value_id)) {
			return false;
		}
	}
	integration->logic_context.bindings = integration->logic_bindings;
	integration->logic_context.binding_count =
		integration->logic_binding_count;
	integration->logic_context.opcode_proofs = integration->logic_proofs;
	integration->logic_context.opcode_proof_count =
		integration->logic_proof_count;
	integration->logic_context.values_predeclared = integration->w04;
	zend_mir_logic_provider_init(
		&integration->logic_provider, &integration->logic_context);
	return true;
}

static zend_mir_frame_slot_kind zend_mir_w03_slot_kind(
	zend_mir_source_slot_kind kind)
{
	switch (kind) {
		case ZEND_MIR_SOURCE_SLOT_CV:
			return ZEND_MIR_FRAME_SLOT_KIND_CV;
		case ZEND_MIR_SOURCE_SLOT_TMP:
			return ZEND_MIR_FRAME_SLOT_KIND_TMP;
		case ZEND_MIR_SOURCE_SLOT_VAR:
			return ZEND_MIR_FRAME_SLOT_KIND_VAR;
		default:
			return ZEND_MIR_FRAME_SLOT_KIND_INVALID;
	}
}

static bool zend_mir_w03_is_qm_result(
	const zend_mir_w03_integration *integration,
	zend_mir_value_id value_id)
{
	uint32_t opcode_count =
		integration->source_view.opcode_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < opcode_count; index++) {
		zend_mir_source_opcode_ref opcode;

		if (!integration->source_view.opcode_at(
				integration->source_view.context, index, &opcode)) {
			return false;
		}
		if (opcode.zend_opcode_number == ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN
				&& opcode.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA
				&& zend_mir_value_from_original_ssa(
					opcode.result.ssa_variable_id) == value_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w03_track_lifetime_values(
	zend_mir_w03_integration *integration)
{
	uint32_t capacity;
	uint32_t index;

	if (!zend_mir_w03_checked_add(
			integration->fact_count,
			integration->source.opcode_count, &capacity)
			|| !zend_mir_w03_checked_add(capacity, 1, &capacity)) {
		return false;
	}
	integration->lifetime_values = zend_mir_w03_calloc(
		capacity, sizeof(*integration->lifetime_values));
	if (integration->lifetime_values == NULL
			|| !zend_mir_straight_line_lifetime_init(
				&integration->lifetime, integration->lifetime_values,
				capacity)) {
		return false;
	}
	for (index = 0; index < integration->fact_count; index++) {
		const zend_mir_value_fact_ref *fact = &integration->facts[index];
		zend_mir_straight_line_value value;

		if (zend_mir_w03_is_qm_result(integration, fact->value_id)) {
			continue;
		}
		memset(&value, 0, sizeof(value));
		value.value_id = fact->value_id;
		value.representation =
			zend_mir_scalar_type_representation(fact->exact_type);
		value.ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
		value.exact_type = fact->exact_type;
		value.fact_flags = fact->flags;
		value.integer_min = fact->integer_min;
		value.integer_max = fact->integer_max;
		if (!zend_mir_straight_line_track_value(
				&integration->lifetime, &value)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w03_initial_slot_value(
	const zend_mir_w03_integration *integration,
	const zend_mir_source_slot_ref *source_slot,
	zend_mir_value_id *value_id_out)
{
	uint32_t ssa_count =
		integration->source_view.ssa_count(integration->source_view.context);
	uint32_t index;
	bool found = false;

	for (index = 0; index < ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;
		bool phi_result = false;
		uint32_t phi_index;

		if (!integration->source_view.ssa_at(
				integration->source_view.context, index, &ssa)) {
			return false;
		}
		if (integration->w04) {
			uint32_t phi_count = integration->source_view.phi_count(
				integration->source_view.context);

			for (phi_index = 0; phi_index < phi_count; phi_index++) {
				zend_mir_source_phi_ref phi;

				if (!integration->source_view.phi_at(
						integration->source_view.context, phi_index, &phi)) {
					return false;
				}
				if (phi.result_ssa_variable_id == ssa.ssa_variable_id) {
					phi_result = true;
					break;
				}
			}
		}
		if (ssa.definition_opline_index == ZEND_MIR_ID_INVALID
				&& !phi_result
				&& ssa.source_slot_kind == source_slot->kind
				&& ssa.source_slot == source_slot->kind_index) {
			zend_mir_value_id candidate =
				zend_mir_value_from_original_ssa(
				ssa.ssa_variable_id);

			if (zend_mir_id_is_valid(candidate)
					&& zend_mir_w03_fact_for_value(
						integration, candidate) != NULL) {
				*value_id_out = candidate;
				found = true;
				/*
				 * Removed call/value fragments may define the same TMP slot
				 * repeatedly. The retained RETURN sees the latest SSA
				 * representative, so the private prerequisite entry state
				 * must publish that representative as well.
				 */
				if (!integration->w05 && !integration->w06) {
					return true;
				}
			}
		}
	}
	return found;
}

static bool zend_mir_w03_prepare_slots(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array)
{
	uint32_t slot_count =
		zend_mir_zend_source_slot_count(&integration->source);
	uint32_t index;

	integration->slots = zend_mir_w03_calloc(
		slot_count == 0 ? 1 : slot_count, sizeof(*integration->slots));
	if (integration->slots == NULL) {
		return false;
	}
	for (index = 0; index < slot_count; index++) {
		zend_mir_source_slot_ref source_slot;
		zend_mir_straight_line_slot *slot = &integration->slots[index];
		zend_mir_value_id value_id;
		const zend_mir_value_fact_ref *fact;

		if (!zend_mir_zend_source_slot_at(
				&integration->source, index, &source_slot)) {
			return false;
		}
		memset(slot, 0, sizeof(*slot));
		slot->slot_id = source_slot.slot_id;
		slot->index = source_slot.kind_index;
		slot->kind = zend_mir_w03_slot_kind(source_slot.kind);
		slot->value_id = ZEND_MIR_ID_INVALID;
		slot->value_representation = ZEND_MIR_REPRESENTATION_INVALID;
		slot->materialization = ZEND_MIR_MATERIALIZATION_UNDEF;
		slot->ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		if (slot->kind == ZEND_MIR_FRAME_SLOT_KIND_INVALID) {
			return false;
		}
		if (zend_mir_w03_initial_slot_value(
				integration, &source_slot, &value_id)) {
			fact = zend_mir_w03_fact_for_value(integration, value_id);
			if (fact == NULL) {
				return false;
			}
			slot->value_id = value_id;
			slot->value_representation =
				zend_mir_scalar_type_representation(fact->exact_type);
			slot->materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
		}
	}
	memset(&integration->entry, 0, sizeof(integration->entry));
	integration->entry.function_kind = op_array->function_name == NULL
		? ZEND_MIR_FUNCTION_KIND_MAIN : ZEND_MIR_FUNCTION_KIND_USER;
	integration->entry.op_array_id = ZEND_MIR_W03_OP_ARRAY_ID;
	integration->entry.code_version_id = ZEND_MIR_W03_CODE_VERSION_ID;
	integration->entry.slots = slot_count == 0 ? NULL : integration->slots;
	integration->entry.slot_count = slot_count;
	integration->entry.source_context = integration;
	integration->entry.source_position_at = zend_mir_w03_source_position;
	integration->entry.resolve_operand = zend_mir_w03_resolve_operand;
	return true;
}

static bool zend_mir_w03_prepare_providers(
	zend_mir_w03_integration *integration)
{
	zend_mir_lowering_diagnostic_code diagnostic;
	zend_mir_lowering_provider provider;
	uint32_t index;

	/*
	 * W04 extends the source-view tail while reusing the frozen W03 scalar
	 * providers. Present those providers with their 1.2 prefix contract;
	 * the W04 lowering context retains the full 1.3 view for CFG callbacks.
	 */
	integration->provider_source_view = integration->source_view;
	integration->provider_source_view.contract_version =
		ZEND_MIR_CONTRACT_VERSION;

	memset(&integration->numeric_context, 0,
		sizeof(integration->numeric_context));
	integration->numeric_context.source = &integration->provider_source_view;
	integration->numeric_context.source_context = integration;
	integration->numeric_context.resolve_operand = zend_mir_w03_resolve_operand;
	integration->numeric_context.value_fact = zend_mir_w03_value_fact;
	integration->numeric_context.source_position = zend_mir_w03_source_position;
	integration->numeric_context.values_predeclared = integration->w04;
	integration->numeric_context.proofs =
		ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK
		| ZEND_MIR_NUMERIC_PROOF_NO_CALLS
		| ZEND_MIR_NUMERIC_PROOF_NO_REENTRY
		| ZEND_MIR_NUMERIC_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_NUMERIC_PROOF_NO_EXCEPTION;
	if (integration->w04) {
		integration->numeric_context.proofs &=
			~ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK;
		integration->numeric_context.proofs |=
			ZEND_MIR_NUMERIC_PROOF_SOURCE_CFG;
	}
	if (!zend_mir_numeric_provider_set_init(
			&integration->numeric_context,
			&integration->numeric_providers)) {
		return false;
	}

	memset(&integration->lifetime_context, 0,
		sizeof(integration->lifetime_context));
	integration->lifetime_context.source = &integration->provider_source_view;
	integration->lifetime_context.lifetime = &integration->lifetime;
	integration->lifetime_context.entry = &integration->entry;
	integration->lifetime_context.values_predeclared = integration->w04;
	integration->lifetime_context.proofs =
		ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY
		| ZEND_MIR_STRAIGHT_LINE_PROOF_EXACT_SCALAR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NOT_BY_REFERENCE
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_OBSERVER
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_EXCEPTION;
	if (integration->w04) {
		integration->lifetime_context.proofs &=
			~ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK;
		integration->lifetime_context.proofs |=
			ZEND_MIR_STRAIGHT_LINE_PROOF_SOURCE_CFG;
	}
	if (!zend_mir_lifetime_provider_init(
			&integration->lifetime_context,
			&integration->lifetime_provider)
			|| !zend_mir_lowering_registry_init(
				&integration->registry, zend_mir_lowering_w03_profile(),
				&diagnostic)
			|| !zend_mir_lowering_register_nop(
				&integration->registry, ZEND_MIR_W03_NOP_FAMILY_ID,
				&diagnostic)) {
		return false;
	}
	for (index = 0;
			index < zend_mir_numeric_provider_count(
				&integration->numeric_providers);
			index++) {
		if (!zend_mir_numeric_provider_at(
				&integration->numeric_providers, index, &provider)
				|| !zend_mir_lowering_registry_register(
					&integration->registry, &provider, &diagnostic)) {
			return false;
		}
	}
	return zend_mir_lowering_registry_register(
			&integration->registry, &integration->logic_provider, &diagnostic)
		&& zend_mir_lowering_registry_register(
			&integration->registry, &integration->lifetime_provider,
			&diagnostic)
		&& zend_mir_lowering_registry_validate(
			&integration->registry, &diagnostic);
}

static bool zend_mir_w03_forward_add_function(
	void *context, zend_mir_symbol_id symbol_id, zend_mir_function_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_function(
		integration->target_mutator->context, symbol_id, out);
}

static bool zend_mir_w03_forward_add_block(
	void *context, zend_mir_function_id function_id, zend_mir_block_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_block(
		integration->target_mutator->context, function_id, out);
}

static bool zend_mir_w03_forward_add_value(
	void *context, zend_mir_value_id requested_id,
	zend_mir_representation representation, zend_mir_ownership_state ownership)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_value(
		integration->target_mutator->context, requested_id,
		representation, ownership);
}

static bool zend_mir_w03_forward_add_constant(
	void *context, const zend_mir_constant_record *constant)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_constant(
		integration->target_mutator->context, constant);
}

static bool zend_mir_w03_forward_add_instruction(
	void *context, const zend_mir_instruction_record *record,
	zend_mir_instruction_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_instruction(
		integration->target_mutator->context, record, out);
}

static bool zend_mir_w03_forward_add_operand(
	void *context, zend_mir_instruction_id instruction_id,
	zend_mir_value_id value_id)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_operand(
		integration->target_mutator->context, instruction_id, value_id);
}

static bool zend_mir_w03_forward_add_edge(
	void *context, zend_mir_block_id from, zend_mir_block_id to)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_edge(
		integration->target_mutator->context, from, to);
}

static bool zend_mir_w03_forward_add_source_position(
	void *context, const zend_mir_source_position_ref *source_position,
	zend_mir_source_position_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_source_position(
		integration->target_mutator->context, source_position, out);
}

static bool zend_mir_w03_forward_add_frame_slot(
	void *context, const zend_mir_frame_slot_ref *slot, uint32_t *index_out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_frame_slot(
		integration->target_mutator->context, slot, index_out);
}

static bool zend_mir_w03_forward_add_root(
	void *context, uint32_t slot_id, uint32_t *index_out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_root(
		integration->target_mutator->context, slot_id, index_out);
}

static bool zend_mir_w03_forward_add_cleanup(
	void *context, const zend_mir_cleanup_ref *cleanup, uint32_t *index_out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_cleanup(
		integration->target_mutator->context, cleanup, index_out);
}

static bool zend_mir_w03_forward_add_frame_state(
	void *context, const zend_mir_frame_state_ref *frame_state,
	zend_mir_frame_state_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_frame_state(
		integration->target_mutator->context, frame_state, out);
}

static bool zend_mir_w03_forward_seal_function(
	void *context, zend_mir_function_id function_id)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->seal_function(
		integration->target_mutator->context, function_id);
}

static bool zend_mir_w03_forward_add_source_map(
	void *context, const zend_mir_source_map_ref *source_map,
	zend_mir_source_map_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_source_map(
		integration->target_mutator->context, source_map, out);
}

static bool zend_mir_w03_forward_add_value_fact(
	void *context, const zend_mir_value_fact_ref *fact,
	zend_mir_value_fact_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_value_fact(
		integration->target_mutator->context, fact, out);
}

static bool zend_mir_w03_emit_constants(
	zend_mir_w03_integration *integration, zend_mir_block_id block_id)
{
	uint32_t literal_count =
		integration->source_view.literal_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;
		const zend_mir_value_fact_ref *fact;
		zend_mir_instruction_record instruction;
		zend_mir_instruction_id instruction_id;

		if (!integration->source_view.literal_at(
				integration->source_view.context, index, &literal)) {
			return false;
		}
		fact = zend_mir_w03_fact_for_value(
			integration, zend_mir_value_from_synthetic(literal.literal_index));
		if (fact == NULL) {
			return false;
		}
		memset(&instruction, 0, sizeof(instruction));
		instruction.id = ZEND_MIR_ID_INVALID;
		instruction.block_id = block_id;
		instruction.opcode = ZEND_MIR_OPCODE_CONSTANT;
		instruction.representation =
			zend_mir_scalar_type_representation(fact->exact_type);
		instruction.result_id = fact->value_id;
		instruction.frame_state_id = ZEND_MIR_ID_INVALID;
		instruction.source_position_id = 0;
		if (!integration->target_mutator->add_instruction(
				integration->target_mutator->context, &instruction,
				&instruction_id)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w03_set_entry_block(
	void *context, zend_mir_function_id function_id,
	zend_mir_block_id block_id)
{
	zend_mir_w03_integration *integration = context;
	zend_mir_source_opcode_ref first_opcode;
	zend_mir_lowering_diagnostic_code diagnostic;

	if (!integration->target_mutator->set_entry_block(
			integration->target_mutator->context, function_id, block_id)
			|| !integration->source_view.opcode_at(
				integration->source_view.context, 0, &first_opcode)
			|| zend_mir_lower_entry_state(
				&integration->lowering_context, &first_opcode,
				&integration->mutator, &integration->lifetime_context,
				&diagnostic) != ZEND_MIR_LOWERING_SUCCESS
			|| !zend_mir_w03_emit_constants(integration, block_id)) {
		return false;
	}
	return true;
}

static void zend_mir_w03_init_mutator(
	zend_mir_w03_integration *integration,
	zend_mir_mutator *target_mutator)
{
	memset(&integration->mutator, 0, sizeof(integration->mutator));
	integration->target_mutator = target_mutator;
	integration->mutator.contract_version = target_mutator->contract_version;
	integration->mutator.context = integration;
	integration->mutator.diagnostics = target_mutator->diagnostics;
	integration->mutator.add_function = zend_mir_w03_forward_add_function;
	integration->mutator.add_block = zend_mir_w03_forward_add_block;
	integration->mutator.set_entry_block = zend_mir_w03_set_entry_block;
	integration->mutator.add_value = zend_mir_w03_forward_add_value;
	integration->mutator.add_constant = zend_mir_w03_forward_add_constant;
	integration->mutator.add_instruction =
		zend_mir_w03_forward_add_instruction;
	integration->mutator.add_operand = zend_mir_w03_forward_add_operand;
	integration->mutator.add_edge = zend_mir_w03_forward_add_edge;
	integration->mutator.add_source_position =
		zend_mir_w03_forward_add_source_position;
	integration->mutator.add_frame_slot =
		zend_mir_w03_forward_add_frame_slot;
	integration->mutator.add_root = zend_mir_w03_forward_add_root;
	integration->mutator.add_cleanup = zend_mir_w03_forward_add_cleanup;
	integration->mutator.add_frame_state =
		zend_mir_w03_forward_add_frame_state;
	integration->mutator.seal_function = zend_mir_w03_forward_seal_function;
	integration->mutator.add_source_map =
		zend_mir_w03_forward_add_source_map;
	integration->mutator.add_value_fact =
		zend_mir_w03_forward_add_value_fact;
}

static bool zend_mir_w03_seed_module(zend_mir_w03_integration *integration)
{
	uint32_t source_count =
		zend_mir_zend_source_position_count(&integration->source);
	uint32_t literal_count =
		integration->source_view.literal_count(integration->source_view.context);
	uint32_t ssa_count =
		integration->source_view.ssa_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < source_count; index++) {
		zend_mir_source_position_ref source_position;
		zend_mir_source_position_id source_id;

		if (!zend_mir_zend_source_position_at(
				&integration->source, index, &source_position)
				|| !integration->target_mutator->add_source_position(
					integration->target_mutator->context, &source_position,
					&source_id)
				|| source_id != source_position.id) {
			return false;
		}
	}
	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;
		zend_mir_value_fact_ref fact;
		zend_mir_constant_record constant;
		zend_mir_value_fact_id fact_id;

		if (!integration->source_view.literal_at(
				integration->source_view.context, index, &literal)
				|| !zend_mir_w03_literal_fact(
					&literal, &fact, &constant)
				|| !integration->target_mutator->add_value(
					integration->target_mutator->context, fact.value_id,
					constant.representation, ZEND_MIR_OWNERSHIP_STATE_OWNED)
				|| !integration->target_mutator->add_constant(
					integration->target_mutator->context, &constant)
				|| !integration->target_mutator->add_value_fact(
					integration->target_mutator->context, &fact, &fact_id)) {
			return false;
		}
	}
	for (index = 0; index < ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;
		zend_mir_value_id value_id;
		const zend_mir_value_fact_ref *fact;
		zend_mir_value_fact_id fact_id;

		if (integration->w04) {
			break;
		}
		if (!integration->source_view.ssa_at(
				integration->source_view.context, index, &ssa)) {
			return false;
		}
		if (ssa.definition_opline_index != ZEND_MIR_ID_INVALID) {
			continue;
		}
		value_id = zend_mir_value_from_original_ssa(ssa.ssa_variable_id);
		fact = zend_mir_w03_fact_for_value(integration, value_id);
		if (fact == NULL) {
			continue;
		}
		if (!integration->target_mutator->add_value(
					integration->target_mutator->context, value_id,
					zend_mir_scalar_type_representation(fact->exact_type),
					ZEND_MIR_OWNERSHIP_STATE_OWNED)
				|| !integration->target_mutator->add_value_fact(
					integration->target_mutator->context, fact, &fact_id)) {
			return false;
		}
	}
	return true;
}

static zend_mir_module *zend_mir_w03_module_create(
	void *context, zend_mir_module_id module_id,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration *integration = context;

	integration->module = integration->target_module_ops.create(
		integration->target_module_ops.context, module_id, diagnostics);
	if (integration->module == NULL) {
		return NULL;
	}
	integration->target_mutator = integration->target_module_ops.mutator(
		integration->target_module_ops.context, integration->module);
	if (integration->target_mutator == NULL) {
		return integration->module;
	}
	zend_mir_w03_init_mutator(integration, integration->target_mutator);
	integration->module_seeded = zend_mir_w03_seed_module(integration);
	return integration->module;
}

static void zend_mir_w03_module_destroy(
	void *context, zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	integration->target_module_ops.destroy(
		integration->target_module_ops.context, module);
	integration->module = NULL;
	integration->target_mutator = NULL;
	integration->module_seeded = false;
}

static zend_mir_mutator *zend_mir_w03_module_mutator(
	void *context, zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	return module == integration->module && integration->target_mutator != NULL
			&& integration->module_seeded
		? &integration->mutator : NULL;
}

static const zend_mir_view *zend_mir_w03_module_view(
	void *context, const zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_module_ops.view(
		integration->target_module_ops.context, module);
}

static bool zend_mir_w03_module_finalize(
	void *context, zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	if (integration->w05 && !integration->deferred_finalize) {
		integration->deferred_finalize = true;
		return true;
	}
	return integration->target_module_ops.finalize(
		integration->target_module_ops.context, module);
}

static bool zend_mir_w03_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration *integration = context;
	if (integration->w08 && integration->deferred_finalize) {
		/* W08 publishes calls and catch entries before structural verification. */
		return true;
	}

	return integration->target_module_ops.verify_stage1(
		integration->target_module_ops.context, view, diagnostics);
}

static bool zend_mir_w03_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration *integration = context;
	if (integration->w08 && integration->deferred_finalize) {
		return true;
	}

	return integration->target_module_ops.verify_stage2(
		integration->target_module_ops.context, view, diagnostics);
}

static void zend_mir_w03_prepare_module_ops(
	zend_mir_w03_integration *integration,
	const zend_mir_lowering_module_ops *target)
{
	integration->target_module_ops = *target;
	memset(&integration->module_ops, 0, sizeof(integration->module_ops));
	integration->module_ops.context = integration;
	integration->module_ops.create = zend_mir_w03_module_create;
	integration->module_ops.destroy = zend_mir_w03_module_destroy;
	integration->module_ops.mutator = zend_mir_w03_module_mutator;
	integration->module_ops.view = zend_mir_w03_module_view;
	integration->module_ops.finalize = zend_mir_w03_module_finalize;
	integration->module_ops.verify_stage1 = zend_mir_w03_verify_stage1;
	integration->module_ops.verify_stage2 = zend_mir_w03_verify_stage2;
}

static void zend_mir_w03_release(zend_mir_w03_integration *integration)
{
	zend_mir_zend_source_release_w05(&integration->source);
	free(integration->slots);
	free(integration->lifetime_values);
	free(integration->logic_proofs);
	free(integration->logic_bindings);
	free(integration->facts);
	free(integration->w11_fact_modes);
	free(integration->w11_ssa_replacements);
	free(integration->w11_suppressed_opcodes);
	free(integration->projected_ssa_var_info);
	free(integration->projected_ssa_vars);
	free(integration->projected_ssa_ops);
	free(integration->projected_opcodes);
}

zend_mir_lowering_result zend_mir_lower_w03_zend_source(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration integration;
	zend_mir_frontend_diagnostic frontend_diagnostic;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_status status;
	zend_mir_lowering_result result;
	const zend_op_array *source_op_array;
	const zend_ssa *source_ssa;

	memset(&integration, 0, sizeof(integration));
	memset(&frontend_diagnostic, 0, sizeof(frontend_diagnostic));
	if (module_ops == NULL) {
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	if (!zend_mir_w03_prepare_source(
			&integration, op_array, ssa, &source_op_array, &source_ssa)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	status = zend_mir_zend_source_init(
		&integration.source, source_op_array, source_ssa,
		ZEND_MIR_W03_OP_ARRAY_ID,
		ZEND_MIR_W03_FILE_SYMBOL_ID, &frontend_diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(status, frontend_diagnostic.code);
	}
	if (!zend_mir_zend_source_view(
			&integration.source, &integration.source_view)
			|| !zend_mir_w03_prepare_facts(&integration)
			|| !zend_mir_w03_track_lifetime_values(&integration)
			|| !zend_mir_w03_prepare_slots(&integration, source_op_array)
			|| !zend_mir_w03_prepare_logic(&integration)
			|| !zend_mir_w03_prepare_providers(&integration)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	zend_mir_w03_prepare_module_ops(&integration, module_ops);
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 1;
	shape.ssa_complete = true;
	if (!zend_mir_lowering_context_init(
			&integration.lowering_context, &integration.source_view, &shape,
			&integration.registry, &integration.module_ops, diagnostics,
			ZEND_MIR_W03_MODULE_ID, ZEND_MIR_W03_FUNCTION_SYMBOL_ID, NULL)
			|| !zend_mir_lowering_context_set_value_fact_resolver(
				&integration.lowering_context, &integration,
				zend_mir_w03_value_fact)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_INVALID_SOURCE);
	}
	result = zend_mir_lower_source(&integration.lowering_context, NULL);
	zend_mir_w03_release(&integration);
	return result;
}

zend_mir_lowering_result zend_mir_lower_w04_zend_op_array(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration integration;
	zend_mir_frontend_diagnostic frontend_diagnostic;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_status status;
	zend_mir_lowering_result result;
	zend_mir_control_flow_map map;
	const zend_op_array *source_op_array;
	const zend_ssa *source_ssa;
	uint32_t index;

	memset(&integration, 0, sizeof(integration));
	memset(&frontend_diagnostic, 0, sizeof(frontend_diagnostic));
	memset(&map, 0, sizeof(map));
	if (module_ops == NULL) {
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	integration.w04 = true;
	if (!zend_mir_w03_prepare_source(
		&integration, op_array, ssa, &source_op_array, &source_ssa)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	status = zend_mir_zend_source_init_w04(
		&integration.source, source_op_array, source_ssa,
		ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
		&frontend_diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(status, frontend_diagnostic.code);
	}
	if (!zend_mir_zend_source_view(
			&integration.source, &integration.source_view)
			|| !zend_mir_w03_prepare_facts(&integration)
			|| !zend_mir_w03_track_lifetime_values(&integration)
			|| !zend_mir_w03_prepare_slots(&integration, source_op_array)
			|| !zend_mir_w03_prepare_logic(&integration)
			|| !zend_mir_w03_prepare_providers(&integration)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	zend_mir_w03_prepare_module_ops(&integration, module_ops);
	memset(&shape, 0, sizeof(shape));
	for (index = 0;
			index < integration.source_view.block_count(
				integration.source_view.context);
			index++) {
		zend_mir_source_block_ref block;
		if (!integration.source_view.block_at(
				integration.source_view.context, index, &block)) {
			zend_mir_w03_release(&integration);
			return zend_mir_w03_result(
				ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W04_MALFORMED_CFG);
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) != 0) {
			shape.reachable_block_count++;
		}
	}
	shape.has_control_flow = true;
	shape.ssa_complete = true;
	if (!zend_mir_lowering_context_init(
			&integration.lowering_context, &integration.source_view, &shape,
			&integration.registry, &integration.module_ops, diagnostics,
			ZEND_MIR_W03_MODULE_ID, ZEND_MIR_W03_FUNCTION_SYMBOL_ID, NULL)
			|| !zend_mir_lowering_context_set_value_fact_resolver(
				&integration.lowering_context, &integration,
				zend_mir_w03_value_fact)
			|| !zend_mir_lowering_context_set_zend_source(
				&integration.lowering_context, &integration.source)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_INVALID_SOURCE);
	}
	result = zend_mir_lower_w04_zend_source(
		&integration.lowering_context, NULL, &map);
	zend_mir_w03_release(&integration);
	return result;
}

static bool zend_mir_w09_post_call_composition(
	const void *composition_context,
	zend_mir_lowering_context *lowering_context,
	zend_mir_module *module,
	const zend_mir_control_flow_map *control_flow_map)
{
	zend_mir_w03_integration *integration =
		(zend_mir_w03_integration *) composition_context;

	return integration != NULL && integration->w09_op_array != NULL
		&& zend_mir_w09_emit_executable_values(
			integration->w09_op_array, lowering_context, module,
			control_flow_map, &integration->lifetime_context,
			integration->w10, integration->w11);
}

static zend_mir_w05_lowering_result zend_mir_lower_direct_user_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics,
	bool w07_execution,
	bool w08_execution,
	bool w09_execution,
	bool w10_execution,
	bool w11_execution)
{
	zend_mir_w03_integration integration;
	zend_mir_frontend_diagnostic frontend_diagnostic;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_status status;
	zend_mir_w05_lowering_result result;
	zend_mir_control_flow_map map;
	zend_mir_source_call_view calls;
	zend_mir_source_call_target_resolver resolver;
	const zend_op_array *source_op_array;
	const zend_ssa *source_ssa;
	uint32_t index;

	memset(&integration, 0, sizeof(integration));
	memset(&frontend_diagnostic, 0, sizeof(frontend_diagnostic));
	memset(&map, 0, sizeof(map));
	memset(&calls, 0, sizeof(calls));
	memset(&resolver, 0, sizeof(resolver));
	if (script == NULL || module_ops == NULL) {
		return zend_mir_w05_integration_result(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	status = w10_execution
		? zend_mir_zend_source_preflight_w10(
			script, op_array, ssa, &frontend_diagnostic)
		: w09_execution
		? zend_mir_zend_source_preflight_w09(
			script, op_array, ssa, &frontend_diagnostic)
		: w08_execution
		? zend_mir_zend_source_preflight_w08(
			script, op_array, ssa, &frontend_diagnostic)
		: w07_execution
		? zend_mir_zend_source_preflight_w07(
			script, op_array, ssa, &frontend_diagnostic)
		: zend_mir_zend_source_preflight_w05(
			script, op_array, ssa, &frontend_diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return zend_mir_w05_integration_result(
			status, frontend_diagnostic.code);
	}
	integration.w04 = true;
	integration.w05 = true;
	integration.w06 = w09_execution;
	integration.w08 = w08_execution || w09_execution;
	integration.w09 = w09_execution;
	integration.w10 = w10_execution;
	integration.w11 = w11_execution;
	integration.w09_op_array = w09_execution ? op_array : NULL;
	if (w11_execution) {
		source_op_array = op_array;
		source_ssa = ssa;
	} else if (!zend_mir_w03_prepare_source(
			&integration, op_array, ssa, &source_op_array, &source_ssa)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w05_integration_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	status = w11_execution
		? ZEND_MIR_LOWERING_SUCCESS
		: w10_execution
		? zend_mir_frontend_project_w10_result_facts(
			script, op_array, ssa, &integration.projected_ssa,
			&frontend_diagnostic)
		: w09_execution
		? zend_mir_frontend_project_w09_result_facts(
			script, op_array, ssa, &integration.projected_ssa,
			&frontend_diagnostic)
		: w08_execution
		? zend_mir_frontend_project_w08_result_facts(
			script, op_array, ssa, &integration.projected_ssa,
			&frontend_diagnostic)
		: zend_mir_frontend_project_w05_result_facts(
			script, op_array, ssa, &integration.projected_ssa,
			&frontend_diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		zend_mir_w03_release(&integration);
		return zend_mir_w05_integration_result(
			status, frontend_diagnostic.code);
	}
	/*
	 * W08 carries string/reference arguments and by-reference mutations in
	 * its original source-backed call inventory.  The private W04 scalar
	 * prerequisite has had every call fragment removed, so retaining those
	 * pointer/reference SSA facts there would reject semantics that are owned
	 * and executed by the W08 runtime boundary.
	 */
	if (!w11_execution && (w08_execution || w09_execution)) {
		zend_mir_w06_sanitize_prerequisite_facts(&integration);
		zend_mir_w06_hide_private_call_results(
			&integration, op_array, ssa);
		zend_mir_w08_hide_byref_argument_facts(
			&integration, op_array, ssa);
		zend_mir_w08_hide_method_receiver_facts(
			&integration, op_array, ssa);
	}
	status = w11_execution
		? zend_mir_zend_source_init_w11_direct(
			&integration.source, op_array, ssa,
			ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
			&frontend_diagnostic)
		: w10_execution
		? zend_mir_zend_source_init_w10_projection(
			&integration.source, source_op_array, source_ssa, op_array, ssa,
			ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
			&frontend_diagnostic)
		: w09_execution
		? zend_mir_zend_source_init_w09_projection(
			&integration.source, source_op_array, source_ssa, op_array, ssa,
			ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
			&frontend_diagnostic)
		: w08_execution
		? zend_mir_zend_source_init_w08_projection(
			&integration.source, source_op_array, source_ssa, op_array, ssa,
			ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
			&frontend_diagnostic)
		: zend_mir_zend_source_init_w05_projection(
			&integration.source, source_op_array, source_ssa, op_array, ssa,
			ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
			&frontend_diagnostic);
	if (status == ZEND_MIR_LOWERING_SUCCESS) {
		status = zend_mir_zend_source_enable_w05(
			&integration.source, script, op_array, ssa,
			&frontend_diagnostic);
	}
	if (status == ZEND_MIR_LOWERING_SUCCESS && w11_execution
			&& !zend_mir_w11_prepare_overlay(
				&integration, op_array, ssa)) {
		status = ZEND_MIR_LOWERING_FAILED;
		frontend_diagnostic.code = ZEND_MIRL_MUTATION_FAILED;
	}
	if (status != ZEND_MIR_LOWERING_SUCCESS
			|| !(w11_execution
				? zend_mir_w11_source_view(&integration)
				: zend_mir_zend_source_view(
					&integration.source, &integration.source_view))
			|| !zend_mir_zend_source_call_view(&integration.source, &calls)
			|| !zend_mir_zend_source_call_target_resolver(
				&integration.source, &resolver)
			|| !zend_mir_w03_prepare_facts(&integration)
			|| !zend_mir_w03_track_lifetime_values(&integration)
			|| !zend_mir_w03_prepare_slots(&integration, source_op_array)
			|| !zend_mir_w03_prepare_logic(&integration)
			|| !zend_mir_w03_prepare_providers(&integration)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w05_integration_result(
			status == ZEND_MIR_LOWERING_SUCCESS
				? ZEND_MIR_LOWERING_FAILED : status,
			status == ZEND_MIR_LOWERING_SUCCESS
				? ZEND_MIRL_MUTATION_FAILED : frontend_diagnostic.code);
	}
	zend_mir_w03_prepare_module_ops(&integration, module_ops);
	memset(&shape, 0, sizeof(shape));
	for (index = 0; index < integration.source_view.block_count(
			integration.source_view.context); index++) {
		zend_mir_source_block_ref block;
		if (!integration.source_view.block_at(
				integration.source_view.context, index, &block)) {
			zend_mir_w03_release(&integration);
			return zend_mir_w05_integration_result(
				ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W04_MALFORMED_CFG);
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) != 0) {
			shape.reachable_block_count++;
		}
	}
	shape.has_control_flow = true;
	shape.has_calls = true;
	shape.has_try_regions = (w08_execution || w09_execution)
		&& op_array->last_try_catch != 0;
	shape.ssa_complete = true;
	if (!zend_mir_lowering_context_init(
			&integration.lowering_context, &integration.source_view, &shape,
			&integration.registry, &integration.module_ops, diagnostics,
			ZEND_MIR_W03_MODULE_ID, ZEND_MIR_W03_FUNCTION_SYMBOL_ID, NULL)
			|| !zend_mir_lowering_context_set_value_fact_resolver(
				&integration.lowering_context, &integration,
				zend_mir_w03_value_fact)
			|| !zend_mir_lowering_context_set_zend_source(
				&integration.lowering_context, &integration.source)
			|| (w09_execution
				&& !zend_mir_lowering_context_set_post_call_composition(
					&integration.lowering_context, &integration,
					zend_mir_w09_post_call_composition))) {
		zend_mir_w03_release(&integration);
		return zend_mir_w05_integration_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_INVALID_SOURCE);
	}
	result = w10_execution
		? zend_mir_lower_w10_zend_source(
			&integration.lowering_context, NULL, &map, &calls, &resolver, NULL)
		: w09_execution
		? zend_mir_lower_w09_zend_source(
			&integration.lowering_context, NULL, &map, &calls, &resolver, NULL)
		: w08_execution
		? zend_mir_lower_w08_zend_source(
			&integration.lowering_context, NULL, &map, &calls, &resolver, NULL)
		: w07_execution
		? zend_mir_lower_w07_zend_source(
			&integration.lowering_context, NULL, &map, &calls, &resolver, NULL)
		: zend_mir_lower_w05_zend_source(
			&integration.lowering_context, NULL, &map, &calls, &resolver, NULL);
	zend_mir_w03_release(&integration);
	return result;
}

zend_mir_w05_lowering_result zend_mir_lower_w05_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_lower_direct_user_op_array(
		script, op_array, ssa, module_ops, diagnostics,
		false, false, false, false, false);
}

zend_mir_w05_lowering_result zend_mir_lower_w07_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_lower_direct_user_op_array(
		script, op_array, ssa, module_ops, diagnostics,
		true, false, false, false, false);
}

zend_mir_w08_lowering_result zend_mir_lower_w08_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_lower_direct_user_op_array(
		script, op_array, ssa, module_ops, diagnostics,
		true, true, false, false, false);
}

zend_mir_w08_lowering_result zend_mir_lower_w09_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_lower_direct_user_op_array(
		script, op_array, ssa, module_ops, diagnostics,
		true, true, true, false, false);
}

zend_mir_w08_lowering_result zend_mir_lower_w10_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_lower_direct_user_op_array(
		script, op_array, ssa, module_ops, diagnostics,
		true, true, true, true, false);
}

zend_mir_w08_lowering_result zend_mir_lower_w11_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	return zend_mir_lower_direct_user_op_array(
		script, op_array, ssa, module_ops, diagnostics,
		true, true, true, true, true);
}

static zend_mir_w06_lowering_result zend_mir_w06_integration_result(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code)
{
	zend_mir_w06_lowering_result result;

	memset(&result, 0, sizeof(result));
	result.prerequisite.lowering = zend_mir_w03_result(status, code);
	return result;
}

/*
 * W06 owns pointer/reference facts in its process-local inventory. The frozen
 * W03-W05 prerequisite projection sees accepted W06 opcodes as NOPs and must
 * therefore not retain facts that the frozen scalar frontend deliberately
 * rejects. This changes only the private copied SSA snapshot.
 */
static void zend_mir_w06_sanitize_prerequisite_facts(
	zend_mir_w03_integration *integration)
{
	const uint32_t pointer_types =
		MAY_BE_STRING | MAY_BE_ARRAY | MAY_BE_OBJECT | MAY_BE_RESOURCE
		| MAY_BE_REF | MAY_BE_INDIRECT | MAY_BE_CLASS | MAY_BE_RC1
		| MAY_BE_RCN | MAY_BE_ARRAY_KEY_ANY | MAY_BE_ARRAY_OF_ANY
		| MAY_BE_ARRAY_OF_REF;
	uint32_t index;

	for (index = 0;
			index < integration->projected_op_array.last_literal;
			index++) {
		zval *literal = &integration->projected_literals[index];
		if (Z_TYPE_P(literal) != IS_NULL
				&& Z_TYPE_P(literal) != IS_FALSE
				&& Z_TYPE_P(literal) != IS_TRUE
				&& Z_TYPE_P(literal) != IS_LONG
				&& Z_TYPE_P(literal) != IS_DOUBLE) {
			ZVAL_NULL(literal);
		}
	}
	for (index = 0;
			index < (uint32_t) integration->projected_ssa.vars_count;
			index++) {
		zend_ssa_var *variable = &integration->projected_ssa_vars[index];
		zend_ssa_var_info *info =
			&integration->projected_ssa_var_info[index];
		if (variable->alias != NO_ALIAS || info->guarded_reference
				|| info->indirect_reference || info->ce != NULL
				|| info->is_instanceof
				|| (info->type & pointer_types) != 0) {
			variable->alias = NO_ALIAS;
			info->type = MAY_BE_NULL;
			info->guarded_reference = 0;
			info->indirect_reference = 0;
			info->ce = NULL;
			info->is_instanceof = 0;
			info->has_range = 0;
		}
	}
}

static void zend_mir_w06_hide_private_call_results(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array,
	const zend_ssa *ssa)
{
	uint32_t definition_index;

	if (integration == NULL || op_array == NULL || ssa == NULL
			|| ssa->ops == NULL) {
		return;
	}
	for (definition_index = 0; definition_index < op_array->last;
			definition_index++) {
		int result = ssa->ops[definition_index].result_def;
		uint32_t use_index;
		bool seen = false;
		bool private_result = true;

		if (!zend_mir_w05_call_completion_fragment(
					op_array->opcodes[definition_index].opcode)
				|| result < 0 || result >= ssa->vars_count) {
			continue;
		}
		for (use_index = 0; use_index < op_array->last; use_index++) {
			const zend_ssa_op *ssa_op = &ssa->ops[use_index];
			uint8_t opcode;

			if (ssa_op->op1_use != result
					&& ssa_op->op2_use != result
					&& ssa_op->result_use != result) {
				continue;
			}
			seen = true;
			opcode = op_array->opcodes[use_index].opcode;
			if (!zend_mir_w06_opcode_is_accepted(opcode)
					&& !zend_mir_w05_call_fragment(opcode)) {
				private_result = false;
				break;
			}
		}
		if (seen && private_result) {
			integration->projected_ssa_var_info[result].type = 0;
		}
	}
}

/* Internal by-reference arguments remain canonical zvals in the source
 * execute frame.  Do not also publish their pre-reference SSA value into the
 * private scalar prerequisite: W08 call records carry the source operand and
 * the runtime boundary materializes the reference in that exact source slot. */
static void zend_mir_w08_hide_byref_argument_facts(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array,
	const zend_ssa *ssa)
{
	uint32_t index;

	if (integration == NULL || op_array == NULL || ssa == NULL
			|| ssa->ops == NULL) {
		return;
	}
	for (index = 0; index < op_array->last; index++) {
		const zend_ssa_op *ssa_op;
		if (op_array->opcodes[index].opcode != ZEND_SEND_REF) {
			continue;
		}
		ssa_op = &ssa->ops[index];
		if (ssa_op->op1_use >= 0
				&& ssa_op->op1_use < integration->projected_ssa.vars_count) {
			integration->projected_ssa_var_info[ssa_op->op1_use].type = 0;
		}
		if (ssa_op->op1_def >= 0
				&& ssa_op->op1_def < integration->projected_ssa.vars_count) {
			integration->projected_ssa_var_info[ssa_op->op1_def].type = 0;
		}
	}
}

/* Instance receivers stay as source zvals in the execute frame and are
 * validated by the exact internal-method binding at the runtime boundary.
 * The call fragment has been removed from the private scalar prerequisite,
 * so publishing its object SSA value there would create an unowned scalar
 * entry value. */
static void zend_mir_w08_hide_method_receiver_facts(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array,
	const zend_ssa *ssa)
{
	uint32_t index;

	if (integration == NULL || op_array == NULL || ssa == NULL
			|| ssa->ops == NULL) {
		return;
	}
	for (index = 0; index < op_array->last; index++) {
		int receiver;

		if (op_array->opcodes[index].opcode != ZEND_INIT_METHOD_CALL
				|| op_array->opcodes[index].op1_type == IS_UNUSED) {
			continue;
		}
		receiver = ssa->ops[index].op1_use;
		if (receiver >= 0
				&& receiver < integration->projected_ssa.vars_count) {
			integration->projected_ssa_var_info[receiver].type = 0;
		}
	}
	for (index = 0;
			index < (uint32_t) integration->projected_ssa.vars_count;
			index++) {
		zend_ssa_var *variable = &integration->projected_ssa_vars[index];

		/* Temporary slots cannot be live-ins. Optimizer-created dead SSA
		 * versions that remain after call projection carry no independent
		 * scalar fact; the source call result owns the physical slot. */
		if (variable->var >= integration->projected_op_array.last_var
				&& variable->definition == -1
				&& variable->definition_phi == NULL
				&& variable->use_chain == -1
				&& variable->phi_use_chain == NULL
				&& variable->sym_use_chain == NULL) {
			integration->projected_ssa_var_info[index].type = 0;
		}
	}
}

static bool zend_mir_w06_call_target_equal(
	const zend_mir_source_call_target_ref *left,
	const zend_mir_source_call_target_ref *right)
{
	return left->id == right->id
		&& left->kind == right->kind
		&& left->function_symbol_id == right->function_symbol_id
		&& left->op_array_id == right->op_array_id
		&& left->num_args == right->num_args
		&& left->required_num_args == right->required_num_args
		&& left->function_flags_snapshot == right->function_flags_snapshot
		&& left->parameter_modes.offset == right->parameter_modes.offset
		&& left->parameter_modes.count == right->parameter_modes.count
		&& left->variadic == right->variadic
		&& left->returns_by_reference == right->returns_by_reference;
}

static bool zend_mir_w06_validate_exact_call_targets(
	const zend_mir_source_call_view *calls,
	const zend_mir_source_call_target_resolver *resolver)
{
	uint32_t target_count;
	uint32_t mode_count;
	uint32_t target_index;

	if (calls == NULL || resolver == NULL
			|| calls->call_target_count == NULL
			|| calls->call_target_at == NULL
			|| calls->parameter_mode_count == NULL
			|| calls->parameter_mode_at == NULL
			|| resolver->resolve_exact_direct_user == NULL) {
		return false;
	}
	target_count = calls->call_target_count(calls->context);
	mode_count = calls->parameter_mode_count(calls->context);
	for (target_index = 0; target_index < target_count; target_index++) {
		zend_mir_source_call_target_ref source;
		zend_mir_source_call_target_ref resolved;
		uint32_t ordinal;

		if (!calls->call_target_at(calls->context, target_index, &source)
				|| !resolver->resolve_exact_direct_user(
					resolver->context, target_index, &resolved)
				|| !zend_mir_w06_call_target_equal(&source, &resolved)
				|| source.id != target_index
				|| source.kind
					!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
				|| source.variadic
				|| source.parameter_modes.count != source.num_args
				|| source.parameter_modes.offset > mode_count
				|| source.parameter_modes.count
					> mode_count - source.parameter_modes.offset) {
			return false;
		}
		for (ordinal = 0; ordinal < source.parameter_modes.count;
				ordinal++) {
			zend_mir_source_parameter_mode_ref mode;
			if (!calls->parameter_mode_at(
					calls->context,
					source.parameter_modes.offset + ordinal, &mode)
					|| mode.target_id != source.id
					|| mode.ordinal != ordinal
					|| (mode.mode
							!= ZEND_MIR_SOURCE_PARAMETER_BY_VALUE
						&& mode.mode
							!= ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE)) {
				return false;
			}
		}
	}
	return true;
}

zend_mir_w06_lowering_result zend_mir_lower_w06_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration integration;
	zend_mir_frontend_diagnostic frontend_diagnostic;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_status status;
	zend_mir_w06_lowering_result result;
	zend_mir_control_flow_map map;
	zend_mir_source_call_view calls;
	zend_mir_source_call_target_resolver resolver;
	zend_mir_w06_value_snapshot snapshot;
	const zend_op_array *source_op_array;
	const zend_ssa *source_ssa;
	uint32_t index;

	memset(&integration, 0, sizeof(integration));
	memset(&frontend_diagnostic, 0, sizeof(frontend_diagnostic));
	memset(&map, 0, sizeof(map));
	memset(&calls, 0, sizeof(calls));
	memset(&resolver, 0, sizeof(resolver));
	memset(&snapshot, 0, sizeof(snapshot));
	if (script == NULL || op_array == NULL || ssa == NULL
			|| module_ops == NULL) {
		return zend_mir_w06_integration_result(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	{
		zend_mir_lowering_diagnostic_code literal_code =
			zend_mir_w06_preflight_literals(op_array);
		if (literal_code != ZEND_MIRL_OK) {
			return zend_mir_w06_integration_result(
				ZEND_MIR_LOWERING_DEFERRED, literal_code);
		}
	}
	integration.w04 = true;
	integration.w05 = true;
	integration.w06 = true;
	if (!zend_mir_w03_prepare_source(
			&integration, op_array, ssa, &source_op_array, &source_ssa)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w06_integration_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	zend_mir_w06_sanitize_prerequisite_facts(&integration);
	zend_mir_w06_hide_private_call_results(
		&integration, op_array, ssa);
	status = zend_mir_zend_source_init_w05_projection(
		&integration.source, source_op_array, source_ssa, op_array, ssa,
		ZEND_MIR_W03_OP_ARRAY_ID, ZEND_MIR_W03_FILE_SYMBOL_ID,
		&frontend_diagnostic);
	if (status == ZEND_MIR_LOWERING_SUCCESS) {
		status = zend_mir_zend_source_enable_w05(
			&integration.source, script, op_array, ssa,
			&frontend_diagnostic);
	}
	if (status != ZEND_MIR_LOWERING_SUCCESS
			|| !zend_mir_zend_source_view(
				&integration.source, &integration.source_view)
			|| !zend_mir_zend_source_call_view(&integration.source, &calls)
			|| !zend_mir_zend_source_call_target_resolver(
				&integration.source, &resolver)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w06_integration_result(
			status == ZEND_MIR_LOWERING_SUCCESS
				? ZEND_MIR_LOWERING_FAILED : status,
			status == ZEND_MIR_LOWERING_SUCCESS
				? ZEND_MIRL_W06_CALL_TRANSFER_DEFERRED
				: frontend_diagnostic.code);
	}
	if (!zend_mir_w06_validate_exact_call_targets(&calls, &resolver)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w06_integration_result(
			ZEND_MIR_LOWERING_DEFERRED,
			ZEND_MIRL_W06_CALL_TRANSFER_DEFERRED);
	}
	{
		zend_mir_lowering_diagnostic_code plan_code =
			zend_mir_w06_build_value_snapshot(
				op_array, ssa, &integration.source, &calls, &snapshot);
		if (plan_code != ZEND_MIRL_OK) {
			zend_mir_w03_release(&integration);
			return zend_mir_w06_integration_result(
				plan_code == ZEND_MIRL_W06_INVALID_STORAGE
					? ZEND_MIR_LOWERING_FAILED
					: ZEND_MIR_LOWERING_DEFERRED,
				plan_code);
		}
	}
	if (!zend_mir_w03_prepare_facts(&integration)
			|| !zend_mir_w03_track_lifetime_values(&integration)
			|| !zend_mir_w03_prepare_slots(&integration, source_op_array)
			|| !zend_mir_w03_prepare_logic(&integration)
			|| !zend_mir_w03_prepare_providers(&integration)) {
		zend_mir_w06_release_value_snapshot(&snapshot);
		zend_mir_w03_release(&integration);
		return zend_mir_w06_integration_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	zend_mir_w03_prepare_module_ops(&integration, module_ops);
	memset(&shape, 0, sizeof(shape));
	for (index = 0; index < integration.source_view.block_count(
			integration.source_view.context); index++) {
		zend_mir_source_block_ref block;
		if (!integration.source_view.block_at(
				integration.source_view.context, index, &block)) {
			zend_mir_w06_release_value_snapshot(&snapshot);
			zend_mir_w03_release(&integration);
			return zend_mir_w06_integration_result(
				ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W04_MALFORMED_CFG);
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) != 0) {
			shape.reachable_block_count++;
		}
	}
	shape.has_control_flow = true;
	shape.has_calls = calls.call_site_count(calls.context) != 0;
	shape.ssa_complete = true;
	if (!zend_mir_lowering_context_init(
			&integration.lowering_context, &integration.source_view, &shape,
			&integration.registry, &integration.module_ops, diagnostics,
			ZEND_MIR_W03_MODULE_ID, ZEND_MIR_W03_FUNCTION_SYMBOL_ID, NULL)
			|| !zend_mir_lowering_context_set_value_fact_resolver(
				&integration.lowering_context, &integration,
				zend_mir_w03_value_fact)
			|| !zend_mir_lowering_context_set_zend_source(
				&integration.lowering_context, &integration.source)) {
		zend_mir_w06_release_value_snapshot(&snapshot);
		zend_mir_w03_release(&integration);
		return zend_mir_w06_integration_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_INVALID_SOURCE);
	}
	result = zend_mir_lower_w06_zend_source(
		&integration.lowering_context, NULL, &map, &calls, &resolver, NULL,
		&snapshot.source_view, &snapshot.plan, NULL);
	zend_mir_w06_release_value_snapshot(&snapshot);
	zend_mir_w03_release(&integration);
	return result;
}
