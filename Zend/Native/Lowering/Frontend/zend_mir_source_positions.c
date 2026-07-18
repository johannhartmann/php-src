#include "zend_mir_zend_source_internal.h"

bool zend_mir_frontend_source_position_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_position_ref *out)
{
	const zend_op_array *op_array;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->source_position_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	out->id = index;
	out->file_symbol_id = source->file_symbol_id;
	out->line = op_array->opcodes[index].lineno;
	out->column_start = 0;
	out->column_end = 0;
	return true;
}
