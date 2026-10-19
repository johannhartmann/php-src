#ifndef ZEND_NATIVE_ELEMENT_GUESS_H
#define ZEND_NATIVE_ELEMENT_GUESS_H

#include "Zend/zend_compile.h"
#include "Zend/Optimizer/zend_optimizer.h"

/*
 * Element type guesses (ADR 0025 section 4): the type an array element read
 * of a script is expected to yield, from the values the script's own code
 * stores into arrays, two levels deep, followed through assignments,
 * parameters and return values of the script's functions. A specialized
 * version assumes a guessed type behind an exit that checks it; nothing
 * here needs to hold.
 */
typedef struct _zend_native_element_guesses zend_native_element_guesses;

zend_native_element_guesses *zend_native_element_guesses_create(
	zend_script *script);
void zend_native_element_guesses_destroy(
	zend_native_element_guesses *guesses);
/* MAY_BE_LONG or MAY_BE_DOUBLE when the FETCH_DIM_R at opline of op_array
 * is guessed to yield that type, else 0. */
uint32_t zend_native_element_guess(
	const zend_native_element_guesses *guesses,
	const zend_op_array *op_array, uint32_t opline);

#endif
