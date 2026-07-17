/* Private process-local storage binding for ZNMIR implementations. */

#ifndef ZEND_MIR_PRIVATE_H
#define ZEND_MIR_PRIVATE_H

#include "zend_mir.h"

/* This binding is neither a persistent nor a textual MIR structure. */
typedef struct _zend_mir_storage_binding {
	zend_mir_module *module;
	zend_mir_allocator allocator;
	zend_mir_view view;
	zend_mir_mutator mutator;
} zend_mir_storage_binding;

#endif /* ZEND_MIR_PRIVATE_H */
