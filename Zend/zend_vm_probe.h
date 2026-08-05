/*
   +----------------------------------------------------------------------+
   | PHP Version 8                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   +----------------------------------------------------------------------+
*/

#ifndef ZEND_VM_PROBE_H
#define ZEND_VM_PROBE_H

/*
 * These hooks are private to the native_mir_test build.  In normal builds
 * they disappear at preprocessing time and add neither code nor ABI.
 */
#ifdef HAVE_NATIVE_MIR_TEST
void zend_native_mir_test_probe_vm_handler(void);
void zend_native_mir_test_probe_execute_ex(void);
void zend_native_mir_test_probe_opline_handler(void);

# define ZEND_NATIVE_MIR_TEST_PROBE_VM_HANDLER() \
	zend_native_mir_test_probe_vm_handler()
# define ZEND_NATIVE_MIR_TEST_PROBE_EXECUTE_EX() \
	zend_native_mir_test_probe_execute_ex()
# define ZEND_NATIVE_MIR_TEST_PROBE_OPLINE_HANDLER() \
	zend_native_mir_test_probe_opline_handler()
#else
# define ZEND_NATIVE_MIR_TEST_PROBE_VM_HANDLER() do { } while (0)
# define ZEND_NATIVE_MIR_TEST_PROBE_EXECUTE_EX() do { } while (0)
# define ZEND_NATIVE_MIR_TEST_PROBE_OPLINE_HANDLER() do { } while (0)
#endif

#endif /* ZEND_VM_PROBE_H */
