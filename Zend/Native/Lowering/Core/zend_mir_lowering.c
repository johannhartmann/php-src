/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <string.h>

#include "zend_mir_lowering_internal.h"

typedef struct _zend_mir_lowering_preflight_result {
	bool ok;
	zend_mir_lowering_status status;
	zend_mir_lowering_diagnostic_code code;
	zend_mir_source_opcode_ref source_opcode;
	bool has_source_opcode;
	const char *detail;
} zend_mir_lowering_preflight_result;
