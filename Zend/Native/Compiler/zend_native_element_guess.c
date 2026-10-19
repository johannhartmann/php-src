#include "Zend/Native/Compiler/zend_native_element_guess.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_type_info.h"

#include <stdint.h>
#include <string.h>

/*
 * The guessed types of a value: its own MAY_BE_* type bits, those of its
 * elements when it is an array, and those of their elements. 0 is no value
 * yet; MAY_BE_ANY is nothing known.
 */
typedef struct _zend_native_guess_value {
	uint32_t type;
	uint32_t elements;
	uint32_t nested;
} zend_native_guess_value;

typedef struct _zend_native_guess_function {
	const zend_op_array *op_array;
	zend_native_guess_value *variables;
	/* The temporaries, as the latest definition in opline order left
	 * them, and the element a write fetch left in a VAR: the CV plus one
	 * times four plus its depth, or 0. */
	zend_native_guess_value *temporaries;
	uint32_t *addresses;
	zend_native_guess_value *parameters;
	zend_native_guess_value result;
	/* The element types each FETCH_DIM_R reads. */
	uint32_t *reads;
} zend_native_guess_function;

struct _zend_native_element_guesses {
	zend_script *script;
	HashTable by_opcodes;
	zend_native_guess_function *functions;
	uint32_t count;
	uint32_t capacity;
	bool changed;
};

#define ZEND_NATIVE_GUESS_NUMBERS (MAY_BE_LONG | MAY_BE_DOUBLE)
#define ZEND_NATIVE_GUESS_PENDING_CALLS 32

static const zend_native_guess_value zend_native_guess_unknown = {
	MAY_BE_ANY, MAY_BE_ANY, MAY_BE_ANY};

static bool zend_native_guess_join_type(uint32_t *target, uint32_t type)
{
	if ((*target | type) == *target) {
		return false;
	}
	*target |= type;
	return true;
}

static void zend_native_guess_join(zend_native_element_guesses *guesses,
	zend_native_guess_value *target, const zend_native_guess_value *value)
{
	guesses->changed |= zend_native_guess_join_type(&target->type, value->type);
	guesses->changed |= zend_native_guess_join_type(
		&target->elements, value->elements);
	guesses->changed |= zend_native_guess_join_type(
		&target->nested, value->nested);
}

/* The types of a literal, an array's two levels from up to 256 elements. */
static zend_native_guess_value zend_native_guess_literal(const zval *literal,
	uint32_t depth)
{
	zend_native_guess_value value = {0, 0, 0};
	const zval *element;
	uint32_t seen = 0;

	if (Z_TYPE_P(literal) > IS_RESOURCE) {
		return zend_native_guess_unknown;
	}
	value.type = 1u << Z_TYPE_P(literal);
	if (Z_TYPE_P(literal) != IS_ARRAY) {
		return value;
	}
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(literal), element) {
		zend_native_guess_value inner;

		if (++seen > 256 || depth > 1) {
			value.elements = MAY_BE_ANY;
			value.nested = MAY_BE_ANY;
			break;
		}
		inner = zend_native_guess_literal(element, depth + 1);
		value.elements |= inner.type;
		value.nested |= inner.elements;
	} ZEND_HASH_FOREACH_END();
	return value;
}

static zend_native_guess_value zend_native_guess_operand(
	const zend_native_guess_function *function, const zend_op *opline,
	uint8_t type, znode_op operand)
{
	const zend_op_array *op_array = function->op_array;
	const zend_native_guess_value none = {0, 0, 0};

	switch (type) {
		case IS_CONST:
			return zend_native_guess_literal(
				RT_CONSTANT(opline, operand), 0);
		case IS_CV:
			return function->variables[EX_VAR_TO_NUM(operand.var)];
		case IS_TMP_VAR:
		case IS_VAR:
			return function->temporaries[
				EX_VAR_TO_NUM(operand.var) - op_array->last_var];
		default:
			return none;
	}
}

/* Defines a result: a CV joins it, a temporary takes it. */
static void zend_native_guess_define(zend_native_element_guesses *guesses,
	zend_native_guess_function *function, uint8_t type, znode_op operand,
	const zend_native_guess_value *value)
{
	const zend_op_array *op_array = function->op_array;

	if (type == IS_CV) {
		zend_native_guess_join(guesses,
			&function->variables[EX_VAR_TO_NUM(operand.var)], value);
	} else if (type == IS_TMP_VAR || type == IS_VAR) {
		const uint32_t slot =
			EX_VAR_TO_NUM(operand.var) - op_array->last_var;

		function->temporaries[slot] = *value;
		function->addresses[slot] = 0;
	}
}

/* The result types of arithmetic on values of these types (overflow into
 * a double is not guessed). */
static uint32_t zend_native_guess_arithmetic(uint32_t opcode,
	uint32_t left, uint32_t right)
{
	switch (opcode) {
		case ZEND_MOD:
		case ZEND_SL:
		case ZEND_SR:
			return MAY_BE_LONG;
		case ZEND_CONCAT:
		case ZEND_FAST_CONCAT:
			return MAY_BE_STRING;
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_DIV:
		case ZEND_POW:
			break;
		default:
			return MAY_BE_ANY;
	}
	if (left == 0 || right == 0) {
		return 0;
	}
	if (((left | right) & ~(uint32_t) ZEND_NATIVE_GUESS_NUMBERS) != 0) {
		return MAY_BE_ANY;
	}
	return ((left & right & MAY_BE_LONG) != 0
			? (opcode == ZEND_DIV ? ZEND_NATIVE_GUESS_NUMBERS : MAY_BE_LONG)
			: 0)
		| ((left | right) & MAY_BE_DOUBLE);
}

/* The declared type of a parameter or return value, as far as it is a
 * scalar or array type. */
static uint32_t zend_native_guess_declared(const zend_arg_info *arg_info)
{
	const uint32_t mask = arg_info != NULL
		? ZEND_TYPE_PURE_MASK(arg_info->type) : 0;

	if (arg_info == NULL || !ZEND_TYPE_IS_SET(arg_info->type)
			|| ZEND_TYPE_IS_COMPLEX(arg_info->type)
			|| (mask & MAY_BE_ANY) == 0) {
		return MAY_BE_ANY;
	}
	return mask & MAY_BE_ANY;
}

static zend_native_guess_function *zend_native_guess_lookup(
	const zend_native_element_guesses *guesses, const zend_op_array *op_array)
{
	const zval *found = op_array->opcodes != NULL
		? zend_hash_index_find(&guesses->by_opcodes,
			(zend_ulong) (uintptr_t) op_array->opcodes)
		: NULL;

	return found != NULL ? &guesses->functions[Z_LVAL_P(found)] : NULL;
}

/* The user function of the script a call names, or an internal one. */
static const zend_function *zend_native_guess_callee(
	const zend_native_element_guesses *guesses, const zend_op *opline)
{
	const zval *name;

	switch (opline->opcode) {
		case ZEND_INIT_FCALL:
			name = RT_CONSTANT(opline, opline->op2);
			break;
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
			name = RT_CONSTANT(opline, opline->op2) + 1;
			break;
		default:
			return NULL;
	}
	if (Z_TYPE_P(name) != IS_STRING) {
		return NULL;
	}
	{
		const zend_function *function = zend_hash_find_ptr(
			&guesses->script->function_table, Z_STR_P(name));

		return function != NULL ? function
			: zend_hash_find_ptr(EG(function_table), Z_STR_P(name));
	}
}

/* The element of the write fetch or CV an operation addresses: its CV and
 * depth, the CV itself being depth 0. */
static bool zend_native_guess_address(
	const zend_native_guess_function *function, uint8_t type,
	znode_op operand, uint32_t *variable, uint32_t *depth)
{
	const zend_op_array *op_array = function->op_array;

	if (type == IS_CV) {
		*variable = EX_VAR_TO_NUM(operand.var);
		*depth = 0;
		return true;
	}
	if (type == IS_VAR) {
		const uint32_t address = function->addresses[
			EX_VAR_TO_NUM(operand.var) - op_array->last_var];

		if (address != 0) {
			*variable = address / 4 - 1;
			*depth = address % 4;
			return true;
		}
	}
	return false;
}

/* Stores a value of these types at a depth below a CV. */
static void zend_native_guess_store(zend_native_element_guesses *guesses,
	zend_native_guess_function *function, uint32_t variable, uint32_t depth,
	const zend_native_guess_value *value)
{
	zend_native_guess_value *target = &function->variables[variable];
	zend_native_guess_value stored = {MAY_BE_ARRAY, 0, 0};

	if (depth == 1) {
		stored.elements = value->type;
		stored.nested = value->elements;
	} else if (depth == 2) {
		stored.elements = MAY_BE_ARRAY;
		stored.nested = value->type;
	} else {
		stored.elements = MAY_BE_ARRAY;
		stored.nested = MAY_BE_ARRAY;
	}
	zend_native_guess_join(guesses, target, &stored);
}

/* The types at a depth below a CV. */
static uint32_t zend_native_guess_at(
	const zend_native_guess_function *function, uint32_t variable,
	uint32_t depth)
{
	const zend_native_guess_value *value = &function->variables[variable];

	return depth == 0 ? value->type
		: depth == 1 ? value->elements
		: depth == 2 ? value->nested : MAY_BE_ANY;
}

static void zend_native_guess_function_pass(
	zend_native_element_guesses *guesses,
	zend_native_guess_function *function)
{
	const zend_op_array *op_array = function->op_array;
	const zend_function *pending[ZEND_NATIVE_GUESS_PENDING_CALLS];
	uint32_t pending_count = 0;
	uint32_t pending_depth = 0;

	memset(function->temporaries, 0,
		sizeof(*function->temporaries) * (op_array->T + 1));
	memset(function->addresses, 0,
		sizeof(*function->addresses) * (op_array->T + 1));
	for (uint32_t index = 0; index < op_array->last; index++) {
		const zend_op *opline = &op_array->opcodes[index];
		const zend_native_guess_value op1 = zend_native_guess_operand(
			function, opline, opline->op1_type, opline->op1);
		const zend_native_guess_value op2 = zend_native_guess_operand(
			function, opline, opline->op2_type, opline->op2);
		zend_native_guess_value result = zend_native_guess_unknown;
		uint32_t variable;
		uint32_t depth;

		switch (opline->opcode) {
			case ZEND_RECV:
			case ZEND_RECV_INIT: {
				const uint32_t argument = opline->op1.num - 1;
				const zend_arg_info *arg_info =
					argument < op_array->num_args
						? &op_array->arg_info[argument] : NULL;

				result = argument < op_array->num_args
					? function->parameters[argument]
					: zend_native_guess_unknown;
				result.type = zend_native_guess_declared(arg_info);
				if (opline->opcode == ZEND_RECV_INIT) {
					const zend_native_guess_value initial =
						zend_native_guess_literal(
							RT_CONSTANT(opline, opline->op2), 0);

					result.elements |= initial.elements;
					result.nested |= initial.nested;
				}
				break;
			}
			case ZEND_ASSIGN:
				if (opline->op1_type == IS_CV) {
					zend_native_guess_join(guesses, &function->variables[
						EX_VAR_TO_NUM(opline->op1.var)], &op2);
				}
				result = op2;
				break;
			case ZEND_QM_ASSIGN:
			case ZEND_FE_RESET_R:
			case ZEND_FE_RESET_RW:
				result = op1;
				break;
			case ZEND_ADD:
			case ZEND_SUB:
			case ZEND_MUL:
			case ZEND_DIV:
			case ZEND_POW:
			case ZEND_MOD:
			case ZEND_SL:
			case ZEND_SR:
			case ZEND_CONCAT:
			case ZEND_FAST_CONCAT:
				result.type = zend_native_guess_arithmetic(
					opline->opcode, op1.type, op2.type);
				result.elements = result.type == MAY_BE_ARRAY
					? op1.elements | op2.elements : MAY_BE_ANY;
				result.nested = MAY_BE_ANY;
				break;
			case ZEND_BW_AND:
			case ZEND_BW_OR:
			case ZEND_BW_XOR:
			case ZEND_BW_NOT:
				result.type = ((op1.type | op2.type) & MAY_BE_STRING) != 0
					? MAY_BE_LONG | MAY_BE_STRING : MAY_BE_LONG;
				break;
			case ZEND_PRE_INC:
			case ZEND_PRE_DEC:
			case ZEND_POST_INC:
			case ZEND_POST_DEC:
				result.type = op1.type;
				break;
			case ZEND_ASSIGN_OP:
				if (zend_native_guess_address(function, opline->op1_type,
						opline->op1, &variable, &depth) && depth == 0) {
					zend_native_guess_value assigned = {0, 0, 0};

					assigned.type = zend_native_guess_arithmetic(
						opline->extended_value, op1.type, op2.type);
					zend_native_guess_join(guesses,
						&function->variables[variable], &assigned);
					result = assigned;
				}
				break;
			case ZEND_IS_IDENTICAL:
			case ZEND_IS_NOT_IDENTICAL:
			case ZEND_IS_EQUAL:
			case ZEND_IS_NOT_EQUAL:
			case ZEND_IS_SMALLER:
			case ZEND_IS_SMALLER_OR_EQUAL:
			case ZEND_BOOL:
			case ZEND_BOOL_NOT:
			case ZEND_BOOL_XOR:
			case ZEND_TYPE_CHECK:
			case ZEND_DEFINED:
			case ZEND_ISSET_ISEMPTY_CV:
			case ZEND_ISSET_ISEMPTY_DIM_OBJ:
			case ZEND_ISSET_ISEMPTY_PROP_OBJ:
			case ZEND_ISSET_ISEMPTY_VAR:
			case ZEND_ISSET_ISEMPTY_STATIC_PROP:
			case ZEND_ARRAY_KEY_EXISTS:
			case ZEND_IN_ARRAY:
			case ZEND_INSTANCEOF:
			case ZEND_CASE:
			case ZEND_CASE_STRICT:
			case ZEND_JMPZ_EX:
			case ZEND_JMPNZ_EX:
				result.type = MAY_BE_BOOL;
				break;
			case ZEND_STRLEN:
			case ZEND_COUNT:
			case ZEND_FUNC_NUM_ARGS:
				result.type = MAY_BE_LONG;
				break;
			case ZEND_CAST:
				result.type = opline->extended_value == _IS_BOOL
					? MAY_BE_BOOL
					: opline->extended_value <= IS_ARRAY
						? 1u << opline->extended_value : MAY_BE_ANY;
				result.elements = opline->extended_value == IS_ARRAY
					? op1.elements : MAY_BE_ANY;
				break;
			case ZEND_INIT_ARRAY:
			case ZEND_ADD_ARRAY_ELEMENT:
				result.type = MAY_BE_ARRAY;
				result.elements = 0;
				result.nested = 0;
				if (opline->opcode == ZEND_ADD_ARRAY_ELEMENT) {
					result = zend_native_guess_operand(function, opline,
						opline->result_type, opline->result);
				}
				if (opline->op1_type != IS_UNUSED) {
					if ((opline->extended_value & ZEND_ARRAY_ELEMENT_REF)
							!= 0) {
						result.elements = MAY_BE_ANY;
						result.nested = MAY_BE_ANY;
					} else {
						result.elements |= op1.type;
						result.nested |= op1.elements;
					}
				}
				break;
			case ZEND_FETCH_DIM_R:
			case ZEND_FETCH_DIM_IS:
			case ZEND_FETCH_LIST_R:
			case ZEND_FETCH_DIM_FUNC_ARG:
				if (opline->opcode == ZEND_FETCH_DIM_R) {
					function->reads[index] = op1.elements;
				}
				result.type = op1.elements;
				result.elements = op1.nested;
				result.nested = MAY_BE_ANY;
				break;
			case ZEND_FETCH_DIM_W:
			case ZEND_FETCH_DIM_RW:
			case ZEND_FETCH_DIM_UNSET:
				if (zend_native_guess_address(function, opline->op1_type,
						opline->op1, &variable, &depth)) {
					const zend_native_guess_value nested = {
						MAY_BE_ARRAY, 0, 0};

					/* An element written through is an array. */
					if (depth != 0) {
						zend_native_guess_store(
							guesses, function, variable, depth, &nested);
					}
					zend_native_guess_define(guesses, function,
						opline->result_type, opline->result, &result);
					if (opline->result_type == IS_VAR && depth < 3) {
						function->addresses[EX_VAR_TO_NUM(opline->result.var)
							- op_array->last_var] =
							(variable + 1) * 4 + depth + 1;
					}
					continue;
				}
				break;
			case ZEND_ASSIGN_DIM:
			case ZEND_ASSIGN_DIM_OP:
				if (index + 1 < op_array->last
						&& zend_native_guess_address(function,
							opline->op1_type, opline->op1, &variable,
							&depth)) {
					zend_native_guess_value value = zend_native_guess_operand(
						function, &opline[1], opline[1].op1_type,
						opline[1].op1);

					if (opline->opcode == ZEND_ASSIGN_DIM_OP) {
						value.type = zend_native_guess_arithmetic(
							opline->extended_value,
							zend_native_guess_at(function, variable,
								depth + 1),
							value.type);
						value.elements = MAY_BE_ANY;
					}
					zend_native_guess_store(
						guesses, function, variable, depth + 1, &value);
					result = value;
				}
				break;
			case ZEND_FE_FETCH_R:
			case ZEND_FE_FETCH_RW: {
				const zend_native_guess_value element = {
					op1.elements, op1.nested, MAY_BE_ANY};

				zend_native_guess_define(guesses, function,
					opline->op2_type, opline->op2, &element);
				result.type = MAY_BE_LONG | MAY_BE_STRING;
				break;
			}
			case ZEND_INIT_FCALL:
			case ZEND_INIT_FCALL_BY_NAME:
			case ZEND_INIT_NS_FCALL_BY_NAME:
			case ZEND_INIT_DYNAMIC_CALL:
			case ZEND_INIT_USER_CALL:
			case ZEND_INIT_METHOD_CALL:
			case ZEND_INIT_STATIC_METHOD_CALL:
			case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
			case ZEND_NEW:
				if (pending_count < ZEND_NATIVE_GUESS_PENDING_CALLS) {
					pending[pending_count++] =
						zend_native_guess_callee(guesses, opline);
				}
				pending_depth++;
				if (opline->opcode != ZEND_NEW) {
					continue;
				}
				break;
			case ZEND_SEND_VAL:
			case ZEND_SEND_VAL_EX:
			case ZEND_SEND_VAR:
			case ZEND_SEND_VAR_EX:
			case ZEND_SEND_VAR_NO_REF:
			case ZEND_SEND_VAR_NO_REF_EX:
			case ZEND_SEND_FUNC_ARG:
				if (pending_depth == pending_count && pending_count != 0
						&& pending[pending_count - 1] != NULL
						&& pending[pending_count - 1]->type
							== ZEND_USER_FUNCTION
						&& opline->op2_type == IS_UNUSED
						&& opline->op2.num >= 1) {
					zend_native_guess_function *callee =
						zend_native_guess_lookup(guesses,
							&pending[pending_count - 1]->op_array);

					if (callee != NULL && opline->op2.num
							<= callee->op_array->num_args) {
						zend_native_guess_join(guesses,
							&callee->parameters[opline->op2.num - 1], &op1);
					}
				}
				continue;
			case ZEND_DO_FCALL:
			case ZEND_DO_ICALL:
			case ZEND_DO_UCALL:
			case ZEND_DO_FCALL_BY_NAME: {
				const zend_function *callee = NULL;

				if (pending_depth == pending_count && pending_count != 0) {
					callee = pending[--pending_count];
				}
				if (pending_depth != 0) {
					pending_depth--;
				}
				if (callee != NULL && callee->type == ZEND_USER_FUNCTION) {
					const zend_native_guess_function *called =
						zend_native_guess_lookup(guesses, &callee->op_array);

					if (called != NULL) {
						result = called->result;
					}
				}
				if (callee != NULL && (callee->common.fn_flags
						& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
					result.type = zend_native_guess_declared(
						callee->common.arg_info - 1);
				}
				break;
			}
			case ZEND_RETURN:
			case ZEND_RETURN_BY_REF:
				zend_native_guess_join(guesses, &function->result, &op1);
				continue;
			default:
				break;
		}
		zend_native_guess_define(guesses, function, opline->result_type,
			opline->result, &result);
	}
}

static bool zend_native_guess_add(zend_native_element_guesses *guesses,
	const zend_op_array *op_array, uint32_t depth)
{
	zend_native_guess_function *function;
	zval index;

	if (op_array == NULL || op_array->type != ZEND_USER_FUNCTION
			|| op_array->opcodes == NULL || depth > 64
			|| zend_hash_index_exists(&guesses->by_opcodes,
				(zend_ulong) (uintptr_t) op_array->opcodes)) {
		return true;
	}
	if (guesses->count == guesses->capacity) {
		guesses->capacity = guesses->capacity == 0
			? 16 : guesses->capacity * 2;
		guesses->functions = perealloc(guesses->functions,
			sizeof(*guesses->functions) * guesses->capacity, 1);
	}
	function = &guesses->functions[guesses->count];
	memset(function, 0, sizeof(*function));
	function->op_array = op_array;
	function->variables = pecalloc(op_array->last_var + 1,
		sizeof(*function->variables), 1);
	function->temporaries = pecalloc(op_array->T + 1,
		sizeof(*function->temporaries), 1);
	function->addresses = pecalloc(op_array->T + 1,
		sizeof(*function->addresses), 1);
	function->parameters = pecalloc(op_array->num_args + 1,
		sizeof(*function->parameters), 1);
	function->reads = pecalloc(op_array->last + 1,
		sizeof(*function->reads), 1);
	ZVAL_LONG(&index, guesses->count);
	zend_hash_index_add(&guesses->by_opcodes,
		(zend_ulong) (uintptr_t) op_array->opcodes, &index);
	guesses->count++;
	for (uint32_t nested = 0; nested < op_array->num_dynamic_func_defs;
			nested++) {
		zend_native_guess_add(guesses, op_array->dynamic_func_defs[nested],
			depth + 1);
	}
	return true;
}

zend_native_element_guesses *zend_native_element_guesses_create(
	zend_script *script)
{
	zend_native_element_guesses *guesses =
		pecalloc(1, sizeof(*guesses), 1);
	zend_function *function;
	zend_class_entry *class_entry;

	guesses->script = script;
	zend_hash_init(&guesses->by_opcodes, 16, NULL, NULL, 1);
	zend_native_guess_add(guesses, &script->main_op_array, 0);
	ZEND_HASH_FOREACH_PTR(&script->function_table, function) {
		if (function->type == ZEND_USER_FUNCTION) {
			zend_native_guess_add(guesses, &function->op_array, 0);
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&script->class_table, class_entry) {
		if (class_entry->type != ZEND_USER_CLASS) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
			if (function->type == ZEND_USER_FUNCTION) {
				zend_native_guess_add(guesses, &function->op_array, 0);
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();
	/* Joins only grow; the bound keeps a pathological script cheap. */
	for (uint32_t pass = 0; pass < 32; pass++) {
		guesses->changed = false;
		for (uint32_t index = 0; index < guesses->count; index++) {
			zend_native_guess_function_pass(
				guesses, &guesses->functions[index]);
		}
		if (!guesses->changed) {
			break;
		}
	}
	return guesses;
}

void zend_native_element_guesses_destroy(
	zend_native_element_guesses *guesses)
{
	if (guesses == NULL) {
		return;
	}
	for (uint32_t index = 0; index < guesses->count; index++) {
		zend_native_guess_function *function = &guesses->functions[index];

		pefree(function->variables, 1);
		pefree(function->temporaries, 1);
		pefree(function->addresses, 1);
		pefree(function->parameters, 1);
		pefree(function->reads, 1);
	}
	if (guesses->functions != NULL) {
		pefree(guesses->functions, 1);
	}
	zend_hash_destroy(&guesses->by_opcodes);
	pefree(guesses, 1);
}

uint32_t zend_native_element_guess(
	const zend_native_element_guesses *guesses,
	const zend_op_array *op_array, uint32_t opline)
{
	const zend_native_guess_function *function = guesses != NULL
		? zend_native_guess_lookup(guesses, op_array) : NULL;
	uint32_t type;

	if (function == NULL || opline >= op_array->last) {
		return 0;
	}
	type = function->reads[opline];
	return type == MAY_BE_LONG || type == MAY_BE_DOUBLE ? type : 0;
}
