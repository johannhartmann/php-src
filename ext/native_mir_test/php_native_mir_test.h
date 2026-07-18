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

#ifndef PHP_NATIVE_MIR_TEST_H
#define PHP_NATIVE_MIR_TEST_H

extern zend_module_entry native_mir_test_module_entry;
#define phpext_native_mir_test_ptr &native_mir_test_module_entry

#define PHP_NATIVE_MIR_TEST_VERSION PHP_VERSION

#endif /* PHP_NATIVE_MIR_TEST_H */
