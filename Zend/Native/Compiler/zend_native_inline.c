#include "Zend/Native/Compiler/zend_native_inline.h"

#include "Zend/zend.h"
#include "Zend/zend_API.h"
#include "Zend/zend_vm_opcodes.h"
#include <stdlib.h>
#include <string.h>

/*
 * Tier-2 inlining by splicing (ADR 0025 section 5).
 *
 * The host copy keeps every operation of the host at its position. A call
 * site INIT ... SEND ... DO whose target is known and small becomes a jump
 * to a region appended behind the host's operations:
 *
 *   T = ISSET flag            entry guard: is the fallback site bound to
 *   JMPNZ T, fallback         the callee in this epoch?
 *   CHECK_VAR / ASSIGN        parameters: CV arguments are read in place,
 *                             literals and defaults go to the body's CVs
 *   body ...                  the callee's operations; after each one that
 *   T = ISSET flag            has a slow path, a test of the bail flag its
 *   JMPNZ T, bail_n           slow path sets instead of running
 *   r = QM_ASSIGN x           RETURN x
 *   UNSET_CV body CVs, flag   exit: the flag's UNSET restores the host's
 *   JMP after_do              run-time cache
 * bail_n:
 *   FREE live temporaries, UNSET_CV body CVs, flag, JMP fallback
 * fallback:
 *   INIT, SEND ..., r = DO    the original call
 *   JMP after_do
 *
 * The body runs before any of its effects can be observed: every slow path
 * bails to the original call, which then does all of the call's work.
 */

#define INLINE_MAX_CALLEE_OPS 48
#define INLINE_MAX_REGIONS 32

bool zend_native_inline_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0) {
#if defined(__x86_64__) && !defined(__APPLE__)
		const char *setting = getenv("ZEND_NATIVE_TIER2_INLINE");
		enabled = setting != NULL && setting[0] == '1';
#else
		/* Only the Linux x64 emitter implements host entries. */
		enabled = 0;
#endif
	}
	return enabled != 0;
}

static bool inline_op1_jump(uint32_t flags)
{
	return (ZEND_VM_OP1_FLAGS(flags) & ZEND_VM_OP_MASK) == ZEND_VM_OP_JMP_ADDR;
}

static bool inline_op2_jump(uint32_t flags)
{
	return (ZEND_VM_OP2_FLAGS(flags) & ZEND_VM_OP_MASK) == ZEND_VM_OP_JMP_ADDR;
}

static bool inline_ext_jump(uint32_t flags)
{
	return (flags & ZEND_VM_EXT_MASK) == ZEND_VM_EXT_JMP_ADDR;
}

/* An operation with compile-time operands: literal indexes and opline
 * numbers, as before pass_two(). */
static void inline_undo_pass_two(
	zend_op *dst, const zend_op *src, const zend_op_array *op_array)
{
	const uint32_t flags = zend_get_opcode_flags(src->opcode);

	*dst = *src;
	if (src->op1_type == IS_CONST) {
		dst->op1.constant =
			(uint32_t) (RT_CONSTANT(src, src->op1) - op_array->literals);
	} else if (inline_op1_jump(flags)) {
		dst->op1.opline_num =
			(uint32_t) (OP_JMP_ADDR(src, src->op1) - op_array->opcodes);
	}
	if (src->op2_type == IS_CONST) {
		dst->op2.constant =
			(uint32_t) (RT_CONSTANT(src, src->op2) - op_array->literals);
	} else if (inline_op2_jump(flags)) {
		dst->op2.opline_num =
			(uint32_t) (OP_JMP_ADDR(src, src->op2) - op_array->opcodes);
	}
	if (inline_ext_jump(flags)) {
		dst->extended_value = (uint32_t) (ZEND_OFFSET_TO_OPLINE(
			src, src->extended_value) - op_array->opcodes);
	}
}

static void inline_redo_pass_two(zend_op *op, zend_op *opcodes, zval *literals)
{
	const uint32_t flags = zend_get_opcode_flags(op->opcode);

	if (op->op1_type == IS_CONST) {
		op->op1.constant = (uint32_t) ((char *) (literals + op->op1.constant)
			- (char *) op);
	} else if (inline_op1_jump(flags)) {
		op->op1.jmp_offset = (uint32_t) ((char *) (opcodes
			+ op->op1.opline_num) - (char *) op);
	}
	if (op->op2_type == IS_CONST) {
		op->op2.constant = (uint32_t) ((char *) (literals + op->op2.constant)
			- (char *) op);
	} else if (inline_op2_jump(flags)) {
		op->op2.jmp_offset = (uint32_t) ((char *) (opcodes
			+ op->op2.opline_num) - (char *) op);
	}
	if (inline_ext_jump(flags)) {
		op->extended_value = (uint32_t) ((char *) (opcodes
			+ op->extended_value) - (char *) op);
	}
}

/* The operations a body may contain: each either completes on its fast
 * path or leaves through its slow path, which bails. */
static bool inline_body_opcode(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
		case ZEND_ISSET_ISEMPTY_PROP_OBJ:
		case ZEND_FETCH_DIM_R:
		case ZEND_FETCH_DIM_IS:
		case ZEND_FETCH_OBJ_R:
		case ZEND_FETCH_OBJ_IS:
		case ZEND_FETCH_STATIC_PROP_R:
		case ZEND_FETCH_STATIC_PROP_IS:
		case ZEND_FETCH_CLASS_CONSTANT:
		case ZEND_FETCH_CONSTANT:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_TYPE_CHECK:
		case ZEND_BOOL:
		case ZEND_BOOL_NOT:
		case ZEND_QM_ASSIGN:
		case ZEND_ARRAY_KEY_EXISTS:
		case ZEND_BIND_GLOBAL:
		case ZEND_VERIFY_RETURN_TYPE:
		case ZEND_INSTANCEOF:
		case ZEND_STRLEN:
		case ZEND_COUNT:
		case ZEND_ASSIGN:
		case ZEND_FREE:
		case ZEND_CHECK_VAR:
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_PRE_INC:
		case ZEND_PRE_DEC:
		case ZEND_POST_INC:
		case ZEND_POST_DEC:
			return true;
		default:
			return false;
	}
}

/*
 * Whether some path from a callee's entry returns without an operation a
 * body cannot hold (which bails right away): a callee whose every path
 * bails would only add its guard and bail to the call.
 */
static bool inline_callee_completes(const zend_op_array *callee)
{
	uint32_t *stack = safe_emalloc(callee->last + 1, sizeof(uint32_t), 0);
	uint8_t *seen = ecalloc(callee->last + 1, 1);
	uint32_t depth = 0;
	uint32_t first = 0;
	bool completes = false;

	while (first < callee->last
			&& (callee->opcodes[first].opcode == ZEND_RECV
				|| callee->opcodes[first].opcode == ZEND_RECV_INIT)) {
		first++;
	}
	stack[depth++] = first;
	seen[first] = 1;
	while (depth > 0 && !completes) {
		const uint32_t index = stack[--depth];
		const zend_op *op;
		uint32_t successors[2];
		uint32_t count = 0;

		if (index >= callee->last) {
			continue;
		}
		op = &callee->opcodes[index];
		switch (op->opcode) {
			case ZEND_RETURN:
				completes = true;
				continue;
			case ZEND_JMP:
				successors[count++] =
					(uint32_t) (OP_JMP_ADDR(op, op->op1) - callee->opcodes);
				break;
			case ZEND_JMPZ:
			case ZEND_JMPNZ:
			case ZEND_JMPZ_EX:
			case ZEND_JMPNZ_EX:
				successors[count++] =
					(uint32_t) (OP_JMP_ADDR(op, op->op2) - callee->opcodes);
				successors[count++] = index + 1;
				break;
			case ZEND_NOP:
			case ZEND_EXT_STMT:
			case ZEND_EXT_NOP:
				successors[count++] = index + 1;
				break;
			default: {
				const uint32_t flags = zend_get_opcode_flags(op->opcode);

				if (!inline_body_opcode(op->opcode) || inline_op1_jump(flags)
						|| inline_op2_jump(flags) || inline_ext_jump(flags)) {
					continue;
				}
				successors[count++] = index + 1;
				break;
			}
		}
		for (uint32_t successor = 0; successor < count; successor++) {
			if (successors[successor] <= callee->last
					&& !seen[successors[successor]]) {
				seen[successors[successor]] = 1;
				stack[depth++] = successors[successor];
			}
		}
	}
	efree(seen);
	efree(stack);
	return completes;
}

static bool inline_bool_opcode(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
		case ZEND_ISSET_ISEMPTY_PROP_OBJ:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_TYPE_CHECK:
		case ZEND_BOOL:
		case ZEND_BOOL_NOT:
		case ZEND_ARRAY_KEY_EXISTS:
		case ZEND_INSTANCEOF:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
			return true;
		default:
			return false;
	}
}

typedef struct {
	const zend_op_array *callee;
	uint32_t init, first_send, sends, call;
	bool this_method;
	/* Per callee parameter: the host CV (frame offset) holding its CV
	 * argument, or UINT32_MAX for a body CV. */
	uint32_t param_host_var[INLINE_MAX_CALLEE_OPS];
	uint32_t cv_base, tmp_base, literal_base;
} inline_site;

/* The callee a site calls, if the copy may carry its body. */
static const zend_op_array *inline_site_callee(
	const zend_op_array *host, const zend_op *init, bool persistent,
	bool *this_method)
{
	zend_function *function = NULL;
	const zend_op_array *callee;
	uint32_t index;

	*this_method = false;
	if (init->opcode == ZEND_INIT_FCALL && init->op2_type == IS_CONST) {
		function = zend_hash_find_ptr(EG(function_table),
			Z_STR_P(RT_CONSTANT(init, init->op2)));
	} else if (init->opcode == ZEND_INIT_FCALL_BY_NAME
			&& init->op2_type == IS_CONST) {
		function = zend_hash_find_ptr(EG(function_table),
			Z_STR_P(RT_CONSTANT(init, init->op2) + 1));
	} else if (init->opcode == ZEND_INIT_METHOD_CALL
			&& init->op1_type == IS_UNUSED && init->op2_type == IS_CONST
			&& host->scope != NULL
			&& (host->fn_flags & ZEND_ACC_STATIC) == 0) {
		function = zend_hash_find_ptr(&host->scope->function_table,
			Z_STR_P(RT_CONSTANT(init, init->op2) + 1));
		if (function == NULL || function->common.scope != host->scope
				|| (function->common.fn_flags & ZEND_ACC_STATIC) != 0) {
			return NULL;
		}
		*this_method = true;
	}
	if (function == NULL || function->type != ZEND_USER_FUNCTION) {
		return NULL;
	}
	callee = &function->op_array;
	if (callee == host
			|| callee->last > INLINE_MAX_CALLEE_OPS
			|| callee->num_args > INLINE_MAX_CALLEE_OPS
			|| callee->last_var > INLINE_MAX_CALLEE_OPS
			|| callee->T >= 62
			|| (callee->fn_flags & (ZEND_ACC_VARIADIC
				| ZEND_ACC_RETURN_REFERENCE | ZEND_ACC_GENERATOR
				| ZEND_ACC_CLOSURE | ZEND_ACC_HAS_FINALLY_BLOCK
				| ZEND_ACC_ABSTRACT)) != 0
			|| callee->last_try_catch != 0
			|| callee->static_variables != NULL
			|| (persistent && (callee->fn_flags & ZEND_ACC_IMMUTABLE) == 0)
			|| (!*this_method && callee->scope != NULL
				&& (callee->fn_flags & ZEND_ACC_STATIC) == 0)) {
		return NULL;
	}
	for (index = 0; index < callee->num_args; index++) {
		const zend_arg_info *arg_info = &callee->arg_info[index];

		if (ZEND_ARG_SEND_MODE(arg_info) != 0
				|| ZEND_TYPE_IS_SET(arg_info->type)) {
			return NULL;
		}
	}
	for (index = 0; index < callee->last; index++) {
		const zend_op *op = &callee->opcodes[index];

		switch (op->opcode) {
			case ZEND_SWITCH_LONG:
			case ZEND_SWITCH_STRING:
			case ZEND_MATCH:
			case ZEND_FUNC_GET_ARGS:
			case ZEND_FUNC_NUM_ARGS:
			case ZEND_RETURN_BY_REF:
			case ZEND_GENERATOR_RETURN:
			case ZEND_FETCH_R:
			case ZEND_FETCH_W:
			case ZEND_FETCH_RW:
			case ZEND_FETCH_IS:
			case ZEND_FETCH_UNSET:
			case ZEND_FETCH_FUNC_ARG:
			case ZEND_UNSET_VAR:
			case ZEND_ISSET_ISEMPTY_VAR:
			case ZEND_COALESCE:
			case ZEND_JMP_SET:
			case ZEND_JMP_NULL:
			case ZEND_CASE:
			case ZEND_CASE_STRICT:
				return NULL;
			case ZEND_RECV_INIT:
				if (Z_TYPE_P(RT_CONSTANT(op, op->op2)) == IS_CONSTANT_AST) {
					return NULL;
				}
				break;
			default:
				break;
		}
		/* Parameters are read in place: no operation writes them. */
		if (op->result_type == IS_CV
				&& EX_VAR_TO_NUM(op->result.var) < callee->num_args
				&& op->opcode != ZEND_RECV && op->opcode != ZEND_RECV_INIT) {
			return NULL;
		}
		if (op->op1_type == IS_CV
				&& EX_VAR_TO_NUM(op->op1.var) < callee->num_args
				&& (op->opcode == ZEND_ASSIGN || op->opcode == ZEND_BIND_GLOBAL
					|| op->opcode == ZEND_UNSET_CV
					|| op->opcode == ZEND_ASSIGN_OP
					|| op->opcode == ZEND_ASSIGN_DIM
					|| op->opcode == ZEND_ASSIGN_OBJ
					|| op->opcode == ZEND_ASSIGN_REF
					|| op->opcode == ZEND_PRE_INC || op->opcode == ZEND_PRE_DEC
					|| op->opcode == ZEND_POST_INC
					|| op->opcode == ZEND_POST_DEC
					|| op->opcode == ZEND_FETCH_DIM_W
					|| op->opcode == ZEND_FETCH_DIM_RW
					|| op->opcode == ZEND_FETCH_DIM_UNSET
					|| op->opcode == ZEND_FETCH_OBJ_W
					|| op->opcode == ZEND_FETCH_OBJ_RW
					|| op->opcode == ZEND_FE_RESET_RW
					|| op->opcode == ZEND_SEND_REF
					|| op->opcode == ZEND_MAKE_REF)) {
			return NULL;
		}
		/* $this of a plain call's body would be another object. */
		if (!*this_method && (op->op1_type == IS_UNUSED
				&& (op->opcode == ZEND_FETCH_OBJ_R
					|| op->opcode == ZEND_FETCH_OBJ_IS
					|| op->opcode == ZEND_ISSET_ISEMPTY_PROP_OBJ
					|| op->opcode == ZEND_FETCH_THIS))) {
			return NULL;
		}
	}
	return inline_callee_completes(callee) ? callee : NULL;
}

/* Whether a call is pending at each opline, in the linear order the
 * native call model pairs INIT and DO in. */
static uint8_t *inline_call_depths(const zend_op_array *host)
{
	uint8_t *depths = ecalloc(host->last + 1, 1);
	uint32_t depth = 0;
	uint32_t index;

	for (index = 0; index < host->last; index++) {
		depths[index] = depth > 255 ? 255 : (uint8_t) depth;
		switch (host->opcodes[index].opcode) {
			case ZEND_INIT_FCALL:
			case ZEND_INIT_FCALL_BY_NAME:
			case ZEND_INIT_NS_FCALL_BY_NAME:
			case ZEND_INIT_METHOD_CALL:
			case ZEND_INIT_STATIC_METHOD_CALL:
			case ZEND_INIT_DYNAMIC_CALL:
			case ZEND_INIT_USER_CALL:
			case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
			case ZEND_NEW:
				depth++;
				break;
			case ZEND_DO_FCALL:
			case ZEND_DO_ICALL:
			case ZEND_DO_UCALL:
			case ZEND_DO_FCALL_BY_NAME:
				if (depth > 0) {
					depth--;
				}
				break;
			default:
				break;
		}
	}
	return depths;
}

/* Recognize INIT, SENDs of CVs and literals, DO at init. */
static bool inline_site_at(const zend_op_array *host, uint32_t init,
	bool persistent, inline_site *site)
{
	const zend_op *op = &host->opcodes[init];
	uint32_t position = init + 1;
	uint32_t sends = 0;
	uint32_t index;

	memset(site, 0, sizeof(*site));
	if (op->opcode != ZEND_INIT_FCALL && op->opcode != ZEND_INIT_FCALL_BY_NAME
			&& op->opcode != ZEND_INIT_METHOD_CALL) {
		return false;
	}
	while (position < host->last) {
		const zend_op *send = &host->opcodes[position];

		if ((send->opcode == ZEND_SEND_VAR || send->opcode == ZEND_SEND_VAR_EX)
				&& send->op1_type == IS_CV) {
		} else if ((send->opcode == ZEND_SEND_VAL
					|| send->opcode == ZEND_SEND_VAL_EX)
				&& send->op1_type == IS_CONST) {
		} else {
			break;
		}
		if (send->op2.num != sends + 1) {
			return false;
		}
		sends++;
		position++;
	}
	if (position >= host->last) {
		return false;
	}
	op = &host->opcodes[position];
	if ((op->opcode != ZEND_DO_UCALL && op->opcode != ZEND_DO_FCALL
				&& op->opcode != ZEND_DO_FCALL_BY_NAME)
			|| (op->result_type != IS_TMP_VAR
				&& op->result_type != IS_UNUSED)
			|| position + 1 >= host->last) {
		return false;
	}
	site->callee = inline_site_callee(host, &host->opcodes[init], persistent,
		&site->this_method);
	if (site->callee == NULL || sends > site->callee->num_args
			|| sends < site->callee->required_num_args) {
		return false;
	}
	site->init = init;
	site->first_send = init + 1;
	site->sends = sends;
	site->call = position;
	for (index = 0; index < site->callee->num_args; index++) {
		const zend_op *send = &host->opcodes[init + 1 + index];

		site->param_host_var[index] = index < sends
				&& send->op1_type == IS_CV
			? send->op1.var : UINT32_MAX;
	}
	return true;
}

/* A host must not reach its variables by name. */
static bool inline_host_eligible(const zend_op_array *host)
{
	uint32_t index;

	if ((host->fn_flags & (ZEND_ACC_GENERATOR | ZEND_ACC_CLOSURE
				| ZEND_ACC_HAS_FINALLY_BLOCK)) != 0
			|| host->last_try_catch != 0
			|| host->function_name == NULL) {
		return false;
	}
	for (index = 0; index < host->last; index++) {
		const zend_op *op = &host->opcodes[index];

		switch (op->opcode) {
			case ZEND_FETCH_R:
			case ZEND_FETCH_W:
			case ZEND_FETCH_RW:
			case ZEND_FETCH_IS:
			case ZEND_FETCH_UNSET:
			case ZEND_FETCH_FUNC_ARG:
			case ZEND_UNSET_VAR:
			case ZEND_ISSET_ISEMPTY_VAR:
			case ZEND_FUNC_GET_ARGS:
			case ZEND_INCLUDE_OR_EVAL:
			case ZEND_YIELD:
			case ZEND_YIELD_FROM:
				return false;
			case ZEND_INIT_FCALL:
			case ZEND_INIT_FCALL_BY_NAME:
				if (op->op2_type == IS_CONST) {
					const zend_string *name = Z_STR_P(
						RT_CONSTANT(op, op->op2)
							+ (op->opcode == ZEND_INIT_FCALL_BY_NAME));

					if (zend_string_equals_literal(name, "extract")
							|| zend_string_equals_literal(name, "compact")
							|| zend_string_equals_literal(name,
								"get_defined_vars")
							|| zend_string_equals_literal(name,
								"func_get_args")
							|| zend_string_equals_literal(name,
								"func_get_arg")) {
						return false;
					}
				}
				break;
			default:
				break;
		}
	}
	return true;
}

typedef struct {
	zend_op *ops;
	uint8_t *kinds;
	uint8_t *cold;
	uint32_t *regions;
	uint32_t count;
	uint32_t capacity;
	uint32_t lineno;
	/* Operations emitted now run rarely (bail blocks, fallback calls). */
	bool emitting_cold;
} inline_builder;

static uint32_t inline_emit(inline_builder *builder, const zend_op *op,
	uint8_t kind, uint32_t region)
{
	if (builder->count == builder->capacity) {
		builder->capacity = builder->capacity * 2 + 64;
		builder->ops = erealloc(builder->ops,
			builder->capacity * sizeof(zend_op));
		builder->kinds = erealloc(builder->kinds, builder->capacity);
		builder->cold = erealloc(builder->cold, builder->capacity);
		builder->regions = erealloc(builder->regions,
			builder->capacity * sizeof(uint32_t));
	}
	builder->ops[builder->count] = *op;
	builder->kinds[builder->count] = kind;
	builder->cold[builder->count] = builder->emitting_cold;
	builder->regions[builder->count] = region;
	return builder->count++;
}

static zend_op inline_op(uint8_t opcode, uint32_t lineno)
{
	zend_op op;

	memset(&op, 0, sizeof(op));
	op.opcode = opcode;
	op.op1_type = IS_UNUSED;
	op.op2_type = IS_UNUSED;
	op.result_type = IS_UNUSED;
	op.lineno = lineno;
	return op;
}

/* Must-live temporaries of a body (callee TMP numbers, bit per TMP):
 * defined on every path and not yet consumed, before each operation. */
static void inline_live_temporaries(const zend_op_array *callee,
	uint64_t *live_before)
{
	uint32_t index;
	bool changed = true;
	uint64_t *out = ecalloc(callee->last, sizeof(uint64_t));
	bool *reached = ecalloc(callee->last, sizeof(bool));

	for (index = 0; index < callee->last; index++) {
		live_before[index] = ~UINT64_C(0);
	}
	live_before[0] = 0;
	reached[0] = true;
	while (changed) {
		changed = false;
		for (index = 0; index < callee->last; index++) {
			const zend_op *op = &callee->opcodes[index];
			const uint32_t flags = zend_get_opcode_flags(op->opcode);
			uint64_t state;
			uint32_t successors[2];
			uint32_t successor_count = 0;
			uint32_t successor;

			if (!reached[index]) {
				continue;
			}
			state = live_before[index];
			if ((op->op1_type & (IS_TMP_VAR | IS_VAR))
					&& op->opcode != ZEND_VERIFY_RETURN_TYPE) {
				state &= ~(UINT64_C(1)
					<< (EX_VAR_TO_NUM(op->op1.var) - callee->last_var));
			}
			if (op->op2_type & (IS_TMP_VAR | IS_VAR)) {
				state &= ~(UINT64_C(1)
					<< (EX_VAR_TO_NUM(op->op2.var) - callee->last_var));
			}
			if (op->result_type & (IS_TMP_VAR | IS_VAR)) {
				state |= UINT64_C(1)
					<< (EX_VAR_TO_NUM(op->result.var) - callee->last_var);
			}
			out[index] = state;
			if (op->opcode != ZEND_JMP && op->opcode != ZEND_RETURN
					&& index + 1 < callee->last) {
				successors[successor_count++] = index + 1;
			}
			if (op->opcode == ZEND_JMP) {
				successors[successor_count++] = (uint32_t) (
					OP_JMP_ADDR(op, op->op1) - callee->opcodes);
			} else if (inline_op2_jump(flags) && op->op2_type == IS_UNUSED) {
				successors[successor_count++] = (uint32_t) (
					OP_JMP_ADDR(op, op->op2) - callee->opcodes);
			}
			for (successor = 0; successor < successor_count; successor++) {
				const uint32_t target = successors[successor];
				const uint64_t merged = reached[target]
					? live_before[target] & state : state;

				if (!reached[target] || merged != live_before[target]) {
					reached[target] = true;
					live_before[target] = merged;
					changed = true;
				}
			}
		}
	}
	for (index = 0; index < callee->last; index++) {
		if (!reached[index]) {
			live_before[index] = 0;
		}
	}
	efree(out);
	efree(reached);
}

typedef struct {
	inline_builder *builder;
	const zend_op_array *host;
	const inline_site *site;
	uint32_t region;
	uint32_t host_last_var;
	uint32_t new_last_var;
	uint32_t flag_var;
	uint32_t guard_var;
	uint32_t flag_tmp;
	uint32_t bool_tmp;
	uint32_t fallback_jumps[INLINE_MAX_CALLEE_OPS * 4];
	uint32_t fallback_jump_count;
	/* Bail tests and jumps waiting for their bail block: builder index
	 * and live set. */
	uint32_t bail_jumps[INLINE_MAX_CALLEE_OPS * 2];
	uint64_t bail_live[INLINE_MAX_CALLEE_OPS * 2];
	uint32_t bail_count;
	bool failed;
} inline_region_state;

/* Frame offset of a callee operand variable in the copy. */
static uint32_t inline_map_var(const inline_region_state *state,
	uint8_t type, uint32_t var)
{
	const zend_op_array *callee = state->site->callee;
	const uint32_t number = EX_VAR_TO_NUM(var);

	if (type == IS_CV) {
		if (number < callee->num_args
				&& state->site->param_host_var[number] != UINT32_MAX) {
			return state->site->param_host_var[number];
		}
		return (uint32_t) EX_NUM_TO_VAR(state->site->cv_base + number);
	}
	return (uint32_t) EX_NUM_TO_VAR(state->new_last_var
		+ state->site->tmp_base + (number - callee->last_var));
}

static uint32_t inline_tmp_var(const inline_region_state *state, uint32_t tmp)
{
	return (uint32_t) EX_NUM_TO_VAR(state->new_last_var + tmp);
}

/* The bail test: the flag, false since the entry guard, is true once a
 * slow path bailed. */
static void inline_emit_bail_test(inline_region_state *state, uint64_t live)
{
	zend_op op = inline_op(ZEND_JMPNZ, state->builder->lineno);

	if (state->bail_count == sizeof(state->bail_jumps)
			/ sizeof(state->bail_jumps[0])) {
		state->failed = true;
		return;
	}
	op.op1_type = IS_CV;
	op.op1.var = (uint32_t) EX_NUM_TO_VAR(state->flag_var);
	state->bail_jumps[state->bail_count] = inline_emit(state->builder, &op,
		ZEND_NATIVE_INLINE_OP_HOST, state->region);
	state->bail_live[state->bail_count++] = live;
}

static void inline_emit_exit(inline_region_state *state)
{
	const zend_op_array *callee = state->site->callee;
	uint32_t cv;
	zend_op op;

	for (cv = 0; cv < callee->last_var; cv++) {
		if (cv < callee->num_args
				&& state->site->param_host_var[cv] != UINT32_MAX) {
			continue;
		}
		op = inline_op(ZEND_UNSET_CV, state->builder->lineno);
		op.op1_type = IS_CV;
		op.op1.var = (uint32_t) EX_NUM_TO_VAR(state->site->cv_base + cv);
		inline_emit(state->builder, &op, ZEND_NATIVE_INLINE_OP_RELEASE,
			state->region);
	}
	op = inline_op(ZEND_UNSET_CV, state->builder->lineno);
	op.op1_type = IS_CV;
	op.op1.var = (uint32_t) EX_NUM_TO_VAR(state->flag_var);
	inline_emit(state->builder, &op, ZEND_NATIVE_INLINE_OP_EXIT,
		state->region);
}

/* Emit the region of one site; returns false when its body cannot be
 * carried. fallback is patched by the caller. */
static bool inline_emit_region(inline_region_state *state, uint32_t *region_start)
{
	const inline_site *site = state->site;
	const zend_op_array *callee = site->callee;
	const zend_op_array *host = state->host;
	inline_builder *builder = state->builder;
	const zend_op *call = &host->opcodes[site->call];
	uint64_t *live_before = ecalloc(callee->last, sizeof(uint64_t));
	uint32_t *map = ecalloc(callee->last + 1, sizeof(uint32_t));
	uint32_t *jumps = ecalloc(callee->last * 2 + 1, sizeof(uint32_t));
	uint32_t *jump_targets = ecalloc(callee->last * 2 + 1, sizeof(uint32_t));
	uint8_t *jump_operand = ecalloc(callee->last * 2 + 1, 1);
	uint32_t jump_count = 0;
	bool *bool_tmps = ecalloc(callee->T + 1, sizeof(bool));
	bool *non_bool_tmps = ecalloc(callee->T + 1, sizeof(bool));
	uint32_t index;
	zend_op op;

	builder->lineno = call->lineno;
	inline_live_temporaries(callee, live_before);
	for (index = 0; index < callee->last; index++) {
		const zend_op *body = &callee->opcodes[index];

		if (body->result_type & (IS_TMP_VAR | IS_VAR)) {
			const uint32_t tmp =
				EX_VAR_TO_NUM(body->result.var) - callee->last_var;

			if (inline_bool_opcode(body->opcode)) {
				bool_tmps[tmp] = true;
			} else {
				non_bool_tmps[tmp] = true;
			}
		}
	}

	/* Entry guard. */
	op = inline_op(ZEND_ISSET_ISEMPTY_CV, builder->lineno);
	op.op1_type = IS_CV;
	op.op1.var = (uint32_t) EX_NUM_TO_VAR(state->guard_var);
	op.result_type = IS_TMP_VAR;
	op.result.var = inline_tmp_var(state, state->flag_tmp);
	*region_start = inline_emit(builder, &op,
		ZEND_NATIVE_INLINE_OP_ENTRY_GUARD, state->region);
	op = inline_op(ZEND_JMPNZ, builder->lineno);
	op.op1_type = IS_TMP_VAR;
	op.op1.var = inline_tmp_var(state, state->flag_tmp);
	state->fallback_jumps[state->fallback_jump_count++] =
		inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_HOST, state->region);

	/* Parameters. */
	for (index = 0; index < callee->num_args; index++) {
		if (index < site->sends && site->param_host_var[index] != UINT32_MAX) {
			op = inline_op(ZEND_CHECK_VAR, builder->lineno);
			op.op1_type = IS_CV;
			op.op1.var = site->param_host_var[index];
			inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_BODY,
				state->region);
			inline_emit_bail_test(state, 0);
			continue;
		}
		op = inline_op(ZEND_ASSIGN, builder->lineno);
		op.op1_type = IS_CV;
		op.op1.var = (uint32_t) EX_NUM_TO_VAR(site->cv_base + index);
		op.op2_type = IS_CONST;
		if (index < site->sends) {
			const zend_op *send = &host->opcodes[site->first_send + index];

			op.op2.constant = (uint32_t) (RT_CONSTANT(send, send->op1)
				- host->literals);
		} else {
			uint32_t recv;

			for (recv = 0; recv < callee->last; recv++) {
				const zend_op *body = &callee->opcodes[recv];

				if (body->opcode == ZEND_RECV_INIT
						&& body->op1.num == index + 1) {
					break;
				}
			}
			if (recv == callee->last) {
				state->failed = true;
				goto done;
			}
			op.op2.constant = site->literal_base + (uint32_t) (RT_CONSTANT(
				&callee->opcodes[recv], callee->opcodes[recv].op2)
				- callee->literals);
		}
		inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_BODY, state->region);
		inline_emit_bail_test(state, 0);
	}

	/* Body. */
	for (index = 0; index < callee->last && !state->failed; index++) {
		const zend_op *body = &callee->opcodes[index];
		const uint32_t flags = zend_get_opcode_flags(body->opcode);
		const uint64_t live = live_before[index];

		map[index] = builder->count;
		builder->lineno = body->lineno;
		switch (body->opcode) {
			case ZEND_RECV:
			case ZEND_RECV_INIT:
			case ZEND_NOP:
			case ZEND_EXT_STMT:
			case ZEND_EXT_NOP:
				continue;
			case ZEND_RETURN: {
				uint32_t value_var = 0;

				if (body->op1_type & (IS_CV | IS_TMP_VAR | IS_VAR)) {
					value_var = inline_map_var(state, body->op1_type,
						body->op1.var);
				}
				if (call->result_type == IS_TMP_VAR) {
					op = inline_op(ZEND_QM_ASSIGN, builder->lineno);
					op.op1_type = body->op1_type;
					if (body->op1_type == IS_CONST) {
						op.op1.constant = site->literal_base + (uint32_t) (
							RT_CONSTANT(body, body->op1) - callee->literals);
					} else {
						op.op1.var = value_var;
					}
					op.result_type = IS_TMP_VAR;
					op.result.var = call->result.var
						+ (state->new_last_var - state->host_last_var)
							* sizeof(zval);
					inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_BODY,
						state->region);
					inline_emit_bail_test(state, live);
				} else if (body->op1_type & (IS_TMP_VAR | IS_VAR)) {
					op = inline_op(ZEND_FREE, builder->lineno);
					op.op1_type = IS_TMP_VAR;
					op.op1.var = value_var;
					inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_HOST,
						state->region);
				}
				inline_emit_exit(state);
				op = inline_op(ZEND_JMP, builder->lineno);
				op.op1.opline_num = site->call + 1;
				inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_HOST,
					state->region);
				continue;
			}
			case ZEND_JMP:
				op = inline_op(ZEND_JMP, builder->lineno);
				jumps[jump_count] = inline_emit(builder, &op,
					ZEND_NATIVE_INLINE_OP_HOST, state->region);
				jump_operand[jump_count] = 1;
				jump_targets[jump_count++] = (uint32_t) (
					OP_JMP_ADDR(body, body->op1) - callee->opcodes);
				continue;
			case ZEND_JMPZ:
			case ZEND_JMPNZ:
			case ZEND_JMPZ_EX:
			case ZEND_JMPNZ_EX: {
				zend_op jump = *body;
				const bool boolean = body->op1_type == IS_TMP_VAR
					&& bool_tmps[EX_VAR_TO_NUM(body->op1.var)
						- callee->last_var]
					&& !non_bool_tmps[EX_VAR_TO_NUM(body->op1.var)
						- callee->last_var];

				if (body->op1_type == IS_CONST) {
					state->failed = true;
					goto done;
				}
				jump.handler = NULL;
				jump.op1.var = inline_map_var(state, body->op1_type,
					body->op1.var);
				if (!boolean) {
					/* A truth test of any value through BOOL, whose slow
					 * path bails before the branch. */
					op = inline_op(ZEND_BOOL, builder->lineno);
					op.op1_type = body->op1_type;
					op.op1.var = jump.op1.var;
					op.result_type = IS_TMP_VAR;
					op.result.var = inline_tmp_var(state, state->bool_tmp);
					inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_BODY,
						state->region);
					inline_emit_bail_test(state, live);
					jump.op1_type = IS_TMP_VAR;
					jump.op1.var = inline_tmp_var(state, state->bool_tmp);
				}
				if (jump.result_type & (IS_TMP_VAR | IS_VAR)) {
					jump.result.var = inline_map_var(state,
						body->result_type, body->result.var);
				}
				jumps[jump_count] = inline_emit(builder, &jump,
					ZEND_NATIVE_INLINE_OP_HOST, state->region);
				jump_operand[jump_count] = 2;
				jump_targets[jump_count++] = (uint32_t) (
					OP_JMP_ADDR(body, body->op2) - callee->opcodes);
				continue;
			}
			default:
				break;
		}
		if (!inline_body_opcode(body->opcode)
				|| inline_op1_jump(flags) || inline_op2_jump(flags)
				|| inline_ext_jump(flags)) {
			/* Not in a body: this path bails right away. */
			op = inline_op(ZEND_JMP, builder->lineno);
			if (state->bail_count == sizeof(state->bail_jumps)
					/ sizeof(state->bail_jumps[0])) {
				state->failed = true;
				goto done;
			}
			state->bail_jumps[state->bail_count] = inline_emit(builder, &op,
				ZEND_NATIVE_INLINE_OP_HOST, state->region);
			state->bail_live[state->bail_count++] = live;
			continue;
		}
		op = *body;
		op.handler = NULL;
		if (body->op1_type == IS_CONST) {
			op.op1.constant = site->literal_base
				+ (uint32_t) (RT_CONSTANT(body, body->op1) - callee->literals);
		} else if (body->op1_type & (IS_CV | IS_TMP_VAR | IS_VAR)) {
			op.op1.var = inline_map_var(state, body->op1_type, body->op1.var);
		}
		if (body->op2_type == IS_CONST) {
			op.op2.constant = site->literal_base
				+ (uint32_t) (RT_CONSTANT(body, body->op2) - callee->literals);
		} else if (body->op2_type & (IS_CV | IS_TMP_VAR | IS_VAR)) {
			op.op2.var = inline_map_var(state, body->op2_type, body->op2.var);
		}
		if (body->result_type & (IS_CV | IS_TMP_VAR | IS_VAR)) {
			op.result.var = inline_map_var(state, body->result_type,
				body->result.var);
		}
		/* A FREE runs as in the host; every other body operation is
		 * followed by its bail test, before anything uses its result. */
		if (body->opcode == ZEND_FREE) {
			inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_HOST,
				state->region);
			continue;
		}
		inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_BODY, state->region);
		inline_emit_bail_test(state, live);
	}
	map[callee->last] = builder->count;
	for (index = 0; index < jump_count && !state->failed; index++) {
		zend_op *jump = &builder->ops[jumps[index]];
		const uint32_t target = map[jump_targets[index]];

		if (jump_operand[index] == 1) {
			jump->op1.opline_num = target;
		} else {
			jump->op2.opline_num = target;
		}
	}

	/* Bail blocks, out of line as the fallback they lead to. */
	builder->emitting_cold = true;
	for (index = 0; index < state->bail_count && !state->failed; index++) {
		zend_op *jump = &builder->ops[state->bail_jumps[index]];
		const uint64_t live = state->bail_live[index];
		uint32_t tmp;
		uint32_t block = UINT32_MAX;
		uint32_t previous;

		for (previous = 0; previous < index; previous++) {
			if (state->bail_live[previous] == live) {
				const zend_op *other =
					&builder->ops[state->bail_jumps[previous]];

				block = other->opcode == ZEND_JMP
					? other->op1.opline_num : other->op2.opline_num;
				break;
			}
		}
		if (block == UINT32_MAX) {
			block = builder->count;
			for (tmp = 0; tmp < callee->T && tmp < 64; tmp++) {
				if ((live & (UINT64_C(1) << tmp)) == 0) {
					continue;
				}
				op = inline_op(ZEND_FREE, builder->lineno);
				op.op1_type = IS_TMP_VAR;
				op.op1.var = inline_tmp_var(state, site->tmp_base + tmp);
				inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_HOST,
					state->region);
			}
			inline_emit_exit(state);
			op = inline_op(ZEND_JMP, builder->lineno);
			state->fallback_jumps[state->fallback_jump_count++] =
				inline_emit(builder, &op, ZEND_NATIVE_INLINE_OP_HOST,
					state->region);
			jump = &builder->ops[state->bail_jumps[index]];
		}
		if (jump->opcode == ZEND_JMP) {
			jump->op1.opline_num = block;
		} else {
			jump->op2.opline_num = block;
		}
	}

done:
	builder->emitting_cold = false;
	efree(live_before);
	efree(map);
	efree(jumps);
	efree(jump_targets);
	efree(jump_operand);
	efree(bool_tmps);
	efree(non_bool_tmps);
	return !state->failed;
}

static void inline_renumber_host_var(zend_op *op, uint32_t shift)
{
	if (op->op1_type & (IS_TMP_VAR | IS_VAR)) {
		op->op1.var += shift;
	}
	if (op->op2_type & (IS_TMP_VAR | IS_VAR)) {
		op->op2.var += shift;
	}
	if (op->result_type & (IS_TMP_VAR | IS_VAR)) {
		op->result.var += shift;
	}
}

zend_op_array *zend_native_inline_splice(
	const zend_op_array *host, bool persistent_callees_only,
	zend_native_inline_alloc_t alloc, void *alloc_context,
	zend_native_inline_host **out_host)
{
	inline_site *sites;
	uint8_t *depths;
	uint32_t site_count = 0;
	uint32_t index;
	uint32_t extra_cvs = 0;
	uint32_t extra_tmps = 0;
	uint32_t extra_literals = 0;
	uint32_t new_last_var;
	uint32_t new_T;
	uint32_t shift;
	uint32_t region;
	inline_builder builder = {0};
	zend_op_array *copy = NULL;
	zend_native_inline_host *metadata;
	zend_native_inline_region *regions;
	uint32_t live_range_count;
	zend_live_range *live_ranges = NULL;

	*out_host = NULL;
	if (!zend_native_inline_enabled() || !inline_host_eligible(host)) {
		return NULL;
	}
	sites = ecalloc(INLINE_MAX_REGIONS, sizeof(inline_site));
	depths = inline_call_depths(host);
	for (index = 0; index < host->last && site_count < INLINE_MAX_REGIONS;
			index++) {
		/* A site inside another pending call keeps its call: the
		 * fallback, behind the outer DO, would not pair with it. */
		if (depths[index] == 0
				&& inline_site_at(host, index, persistent_callees_only,
					&sites[site_count])) {
			inline_site *site = &sites[site_count++];

			site->cv_base = host->last_var + extra_cvs;
			site->tmp_base = host->T + extra_tmps;
			site->literal_base = host->last_literal + extra_literals;
			extra_cvs += site->callee->last_var;
			extra_tmps += site->callee->T + 3;
			extra_literals += site->callee->last_literal;
			index = site->call;
		}
	}
	efree(depths);
	if (site_count == 0) {
		efree(sites);
		return NULL;
	}
	new_last_var = host->last_var + extra_cvs + 3;
	new_T = host->T + extra_tmps;
	shift = (new_last_var - host->last_var) * sizeof(zval);

	/* The host's operations at their positions. */
	for (index = 0; index < host->last; index++) {
		zend_op op;

		inline_undo_pass_two(&op, &host->opcodes[index], host);
		inline_renumber_host_var(&op, shift);
		inline_emit(&builder, &op, ZEND_NATIVE_INLINE_OP_HOST, 0);
	}
	regions = alloc(alloc_context, site_count * sizeof(*regions));
	for (region = 0; region < site_count; region++) {
		inline_site *site = &sites[region];
		inline_region_state state;
		uint32_t region_start;
		uint32_t fallback;
		uint32_t position;
		uint32_t saved_count = builder.count;
		zend_op op;

		memset(&state, 0, sizeof(state));
		state.builder = &builder;
		state.host = host;
		state.site = site;
		state.region = region;
		state.host_last_var = host->last_var;
		state.new_last_var = new_last_var;
		state.flag_var = new_last_var - 3;
		state.guard_var = new_last_var - 2;
		state.flag_tmp = site->tmp_base + site->callee->T;
		state.bool_tmp = site->tmp_base + site->callee->T + 1;
		if (!inline_emit_region(&state, &region_start)) {
			/* The site keeps its call. */
			builder.count = saved_count;
			regions[region].callee = NULL;
			continue;
		}
		/* The fallback: the original call, whose result reaches the
		 * continuation as the body's does, through a QM_ASSIGN. */
		fallback = builder.count;
		builder.emitting_cold = true;
		for (position = site->init; position <= site->call; position++) {
			inline_undo_pass_two(&op, &host->opcodes[position], host);
			inline_renumber_host_var(&op, shift);
			if (position == site->call && op.result_type == IS_TMP_VAR) {
				zend_op assign = inline_op(ZEND_QM_ASSIGN, op.lineno);

				assign.op1_type = IS_TMP_VAR;
				assign.op1.var = inline_tmp_var(&state,
					site->tmp_base + site->callee->T + 2);
				assign.result_type = IS_TMP_VAR;
				assign.result.var = op.result.var;
				op.result.var = assign.op1.var;
				inline_emit(&builder, &op, ZEND_NATIVE_INLINE_OP_HOST,
					region);
				inline_emit(&builder, &assign, ZEND_NATIVE_INLINE_OP_HOST,
					region);
				continue;
			}
			inline_emit(&builder, &op, ZEND_NATIVE_INLINE_OP_HOST, region);
		}
		op = inline_op(ZEND_JMP, host->opcodes[site->call].lineno);
		op.op1.opline_num = site->call + 1;
		inline_emit(&builder, &op, ZEND_NATIVE_INLINE_OP_HOST, region);
		builder.emitting_cold = false;
		for (index = 0; index < state.fallback_jump_count; index++) {
			zend_op *jump = &builder.ops[state.fallback_jumps[index]];

			if (jump->opcode == ZEND_JMP) {
				jump->op1.opline_num = fallback;
			} else {
				jump->op2.opline_num = fallback;
			}
		}
		/* The original call site enters the region. */
		op = inline_op(ZEND_JMP, host->opcodes[site->init].lineno);
		op.op1.opline_num = region_start;
		builder.ops[site->init] = op;
		for (position = site->init + 1; position <= site->call; position++) {
			builder.ops[position] = inline_op(ZEND_NOP,
				host->opcodes[position].lineno);
		}
		regions[region].callee = site->callee;
		regions[region].fallback_init_opline = fallback;
		regions[region].this_method = site->this_method;
	}
	for (region = 0; region < site_count; region++) {
		if (regions[region].callee != NULL) {
			break;
		}
	}
	if (region == site_count) {
		goto cleanup;
	}

	/* Live ranges: the host's, renumbered, and each fallback's call
	 * covered as its original. */
	live_range_count = host->last_live_range;
	for (region = 0; region < site_count; region++) {
		const inline_site *site = &sites[region];

		if (regions[region].callee == NULL) {
			continue;
		}
		for (index = 0; index < host->last_live_range; index++) {
			const zend_live_range *range = &host->live_range[index];

			if (range->start <= site->call && site->call < range->end) {
				live_range_count++;
			}
		}
	}
	if (live_range_count != 0) {
		uint32_t next = 0;

		live_ranges = alloc(alloc_context,
			live_range_count * sizeof(zend_live_range));
		for (index = 0; index < host->last_live_range; index++) {
			live_ranges[next] = host->live_range[index];
			live_ranges[next++].var += shift;
		}
		for (region = 0; region < site_count; region++) {
			const inline_site *site = &sites[region];

			if (regions[region].callee == NULL) {
				continue;
			}
			for (index = 0; index < host->last_live_range; index++) {
				const zend_live_range *range = &host->live_range[index];

				if (range->start <= site->call && site->call < range->end) {
					live_ranges[next] = *range;
					live_ranges[next].var += shift;
					live_ranges[next].start =
						regions[region].fallback_init_opline;
					live_ranges[next++].end =
						regions[region].fallback_init_opline
							+ (site->call - site->init) + 1;
				}
			}
		}
	}

	/* The copy: operations and literals in one block, as pass_two()
	 * allocates them. */
	{
		const uint32_t literal_count = host->last_literal + extra_literals;
		const size_t ops_size = ZEND_MM_ALIGNED_SIZE_EX(
			builder.count * sizeof(zend_op), 16);
		char *block = alloc(alloc_context,
			ops_size + literal_count * sizeof(zval));
		zend_op *opcodes = (zend_op *) block;
		zval *literals = (zval *) (block + ops_size);
		zend_string **vars = alloc(alloc_context,
			new_last_var * sizeof(zend_string *));
		uint8_t *kinds = alloc(alloc_context, builder.count);
		uint8_t *cold = alloc(alloc_context, builder.count);
		uint32_t *op_regions = alloc(alloc_context,
			builder.count * sizeof(uint32_t));
		uint32_t literal = 0;

		memcpy(literals, host->literals,
			host->last_literal * sizeof(zval));
		literal = host->last_literal;
		for (region = 0; region < site_count; region++) {
			const zend_op_array *callee = sites[region].callee;

			memcpy(literals + literal, callee->literals,
				callee->last_literal * sizeof(zval));
			literal += callee->last_literal;
		}
		memcpy(vars, host->vars, host->last_var * sizeof(zend_string *));
		for (region = 0; region < site_count; region++) {
			const zend_op_array *callee = sites[region].callee;

			memcpy(vars + sites[region].cv_base, callee->vars,
				callee->last_var * sizeof(zend_string *));
		}
		vars[new_last_var - 3] = ZSTR_EMPTY_ALLOC();
		vars[new_last_var - 2] = ZSTR_EMPTY_ALLOC();
		vars[new_last_var - 1] = ZSTR_EMPTY_ALLOC();
		memcpy(opcodes, builder.ops, builder.count * sizeof(zend_op));
		for (index = 0; index < builder.count; index++) {
			inline_redo_pass_two(&opcodes[index], opcodes, literals);
		}
		memcpy(kinds, builder.kinds, builder.count);
		memcpy(cold, builder.cold, builder.count);
		memcpy(op_regions, builder.regions, builder.count * sizeof(uint32_t));

		copy = alloc(alloc_context, sizeof(*copy));
		*copy = *host;
		copy->opcodes = opcodes;
		copy->last = builder.count;
		copy->literals = literals;
		copy->last_literal = literal_count;
		copy->vars = vars;
		copy->last_var = (int) new_last_var;
		copy->T = new_T;
		copy->live_range = live_ranges;
		copy->last_live_range = live_range_count;
		memset(copy->reserved, 0, sizeof(copy->reserved));

		metadata = alloc(alloc_context, sizeof(*metadata));
		memset(metadata, 0, sizeof(*metadata));
		metadata->original = host;
		metadata->original_frame_slots = host->last_var + host->T;
		metadata->original_last_var = host->last_var;
		metadata->original_num_args = host->num_args;
		metadata->copy = copy;
		metadata->frame_slots = new_last_var + new_T;
		metadata->op_count = builder.count;
		metadata->op_kinds = kinds;
		metadata->op_cold = cold;
		metadata->op_regions = op_regions;
		metadata->regions = regions;
		metadata->region_count = site_count;
		metadata->flag_var = new_last_var - 3;
		metadata->guard_var = new_last_var - 2;
		metadata->cache_save_var = new_last_var - 1;
		*out_host = metadata;
	}

cleanup:
	efree(sites);
	efree(builder.ops);
	efree(builder.cold);
	efree(builder.kinds);
	efree(builder.regions);
	return copy;
}
