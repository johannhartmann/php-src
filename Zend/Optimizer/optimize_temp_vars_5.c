/*
   +----------------------------------------------------------------------+
   | Zend OPcache                                                         |
   +----------------------------------------------------------------------+
   | Copyright © The PHP Group and Contributors.                          |
   +----------------------------------------------------------------------+
   | This source file is subject to the Modified BSD License that is      |
   | bundled with this package in the file LICENSE, and is available      |
   | through the World Wide Web at <https://www.php.net/license/>.        |
   |                                                                      |
   | SPDX-License-Identifier: BSD-3-Clause                                |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@php.net>                                 |
   |          Zeev Suraski <zeev@php.net>                                 |
   |          Stanislav Malyshev <stas@zend.com>                          |
   |          Dmitry Stogov <dmitry@php.net>                              |
   +----------------------------------------------------------------------+
*/

#include "Optimizer/zend_optimizer.h"
#include "Optimizer/zend_optimizer_internal.h"
#include "zend_API.h"
#include "zend_constants.h"
#include "zend_execute.h"
#include "zend_vm.h"
#include "zend_bitset.h"
#include "zend_observer.h"

#define INVALID_VAR ((uint32_t)-1)
#define GET_AVAILABLE_T()					\
	for (i = 0; i < T; i++) {				\
		if (!zend_bitset_in(taken_T, i)) {	\
			break;							\
		}									\
	}										\
	zend_bitset_incl(taken_T, i);			\
	if (i > max) {							\
		max = i;							\
	}

static bool zend_temp_vars_is_call_end(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_DO_FCALL:
		case ZEND_DO_ICALL:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			return true;
		default:
			return false;
	}
}

static bool zend_temp_vars_is_call_start(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
			return true;
		default:
			return false;
	}
}

static bool zend_temp_vars_is_send(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_SEND_USER:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
			return true;
		default:
			return false;
	}
}

void zend_optimize_temporary_variables(zend_op_array *op_array, zend_optimizer_ctx *ctx)
{
	uint32_t T = op_array->T;
	int offset = op_array->last_var;
	uint32_t bitset_len;
	zend_bitset taken_T;	/* T index in use */
	zend_op **start_of_T;	/* opline where T is first used */
	int *map_T;				/* Map's the T to its new index */
	zend_op *opline, *end;
	int currT;
	int i;
	int max = -1;
	void *checkpoint = zend_arena_checkpoint(ctx->arena);

	bitset_len = zend_bitset_len(T);
	taken_T = (zend_bitset) zend_arena_alloc(&ctx->arena, bitset_len * ZEND_BITSET_ELM_SIZE);
	start_of_T = (zend_op **) zend_arena_alloc(&ctx->arena, T * sizeof(zend_op *));
	map_T = (int *) zend_arena_alloc(&ctx->arena, T * sizeof(int));
	memset(map_T, 0xff, T * sizeof(int));

	end = op_array->opcodes;
	opline = &op_array->opcodes[op_array->last - 1];

	/* Find T definition points */
	while (opline >= end) {
		if (opline->result_type & (IS_VAR | IS_TMP_VAR)) {
			start_of_T[VAR_NUM(opline->result.var) - offset] = opline;
		}
		opline--;
	}

	zend_bitset_clear(taken_T, bitset_len);

	end = op_array->opcodes;
	opline = &op_array->opcodes[op_array->last - 1];

	while (opline >= end) {
		if ((opline->op1_type & (IS_VAR | IS_TMP_VAR))) {
			currT = VAR_NUM(opline->op1.var) - offset;
			if (opline->opcode == ZEND_ROPE_END) {
				int num = (((opline->extended_value + 1) * sizeof(zend_string*)) + (sizeof(zval) - 1)) / sizeof(zval);
				int var;

				var = max;
				while (var >= 0 && !zend_bitset_in(taken_T, var)) {
					var--;
				}
				max = MAX(max, var + num);
				var = var + 1;
				map_T[currT] = var;
				zend_bitset_incl(taken_T, var);
				opline->op1.var = NUM_VAR(var + offset);
				while (num > 1) {
					num--;
					zend_bitset_incl(taken_T, var + num);
				}
			} else {
				if (map_T[currT] == INVALID_VAR) {
					int use_new_var = 0;

					/* Code in "finally" blocks may modify temporary variables.
					 * We allocate new temporaries for values that need to
					 * relive FAST_CALLs.
					 */
					if ((op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) &&
					    (opline->opcode == ZEND_RETURN ||
					     opline->opcode == ZEND_GENERATOR_RETURN ||
					     opline->opcode == ZEND_RETURN_BY_REF ||
					     opline->opcode == ZEND_FREE ||
					     opline->opcode == ZEND_FE_FREE)) {
						const zend_op *curr = opline;

						while (--curr >= end) {
							if (curr->opcode == ZEND_FAST_CALL) {
								use_new_var = 1;
								break;
							} else if (curr->opcode != ZEND_FREE &&
							           curr->opcode != ZEND_FE_FREE &&
							           curr->opcode != ZEND_VERIFY_RETURN_TYPE &&
							           curr->opcode != ZEND_DISCARD_EXCEPTION) {
								break;
							}
						}
					}
					if (use_new_var) {
						i = ++max;
						zend_bitset_incl(taken_T, i);
					} else {
						GET_AVAILABLE_T();
					}
					map_T[currT] = i;
				}
				opline->op1.var = NUM_VAR(map_T[currT] + offset);
			}
		}

		if ((opline->op2_type & (IS_VAR | IS_TMP_VAR))) {
			currT = VAR_NUM(opline->op2.var) - offset;
			if (map_T[currT] == INVALID_VAR) {
				GET_AVAILABLE_T();
				map_T[currT] = i;
			}
			opline->op2.var = NUM_VAR(map_T[currT] + offset);
		}

		if (opline->result_type & (IS_VAR | IS_TMP_VAR)) {
			currT = VAR_NUM(opline->result.var) - offset;
			if (map_T[currT] == INVALID_VAR) {
				/* As a result of DCE, an opcode may have an unused result. */
				GET_AVAILABLE_T();
				map_T[currT] = i;
			}
			opline->result.var = NUM_VAR(map_T[currT] + offset);
			if (start_of_T[currT] == opline) {
				/* ZEND_FAST_CALL can not share temporary var with others
				 * since the fast_var could also be set by ZEND_HANDLE_EXCEPTION
				 * which could be ahead of it */
				if (opline->opcode != ZEND_FAST_CALL) {
					zend_bitset_excl(taken_T, map_T[currT]);
				}
				if (opline->opcode == ZEND_ROPE_INIT) {
					uint32_t num = ((opline->extended_value * sizeof(zend_string*)) + (sizeof(zval) - 1)) / sizeof(zval);
					while (num > 1) {
						num--;
						zend_bitset_excl(taken_T, map_T[currT]+num);
					}
				}
			}
		}

		/* A temporary sent to a call stays in its slot until the call's DO,
		 * so that code which transfers arguments at DO (the native engine's
		 * direct calls) still finds the SEND-time value. */
		if (zend_temp_vars_is_call_end(opline->opcode)) {
			const zend_op *send = opline;
			uint32_t level = 0;

			while (--send >= end) {
				if (zend_temp_vars_is_call_end(send->opcode)) {
					level++;
				} else if (zend_temp_vars_is_call_start(send->opcode)) {
					if (level == 0) {
						break;
					}
					level--;
				} else if (level == 0
						&& zend_temp_vars_is_send(send->opcode)
						&& (send->op1_type & (IS_VAR | IS_TMP_VAR))) {
					currT = VAR_NUM(send->op1.var) - offset;
					if (map_T[currT] == INVALID_VAR) {
						GET_AVAILABLE_T();
						map_T[currT] = i;
					}
				}
			}
		}

		opline--;
	}

	zend_arena_release(&ctx->arena, checkpoint);
	op_array->T = max + 1 + ZEND_OBSERVER_ENABLED; // reserve last temporary for observers if enabled
}
