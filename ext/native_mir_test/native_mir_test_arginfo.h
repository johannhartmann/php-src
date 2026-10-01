/* This is a generated file, edit native_mir_test.stub.php instead.
 * Stub hash: 16bb88c5f52a5257ffcc9e59a69f09522993315b */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_native_mir_test_compile_dump, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, source, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_native_mir_test_compile_execute, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, source, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, filename, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arguments, IS_ARRAY, 0, "[]")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_native_mir_test_unwind_probe, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_native_mir_test_call_cache_invalidate, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_FUNCTION(native_mir_test_compile_dump);
ZEND_FUNCTION(native_mir_test_compile_execute);
ZEND_FUNCTION(native_mir_test_unwind_probe);
ZEND_FUNCTION(native_mir_test_call_cache_invalidate);

static const zend_function_entry ext_functions[] = {
	ZEND_FE(native_mir_test_compile_dump, arginfo_native_mir_test_compile_dump)
	ZEND_FE(native_mir_test_compile_execute, arginfo_native_mir_test_compile_execute)
	ZEND_FE(native_mir_test_unwind_probe, arginfo_native_mir_test_unwind_probe)
	ZEND_FE(native_mir_test_call_cache_invalidate, arginfo_native_mir_test_call_cache_invalidate)
	ZEND_FE_END
};
