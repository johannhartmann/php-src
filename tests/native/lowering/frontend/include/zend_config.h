#ifndef ZEND_CONFIG_H
#define ZEND_CONFIG_H

/*
 * Minimal standalone test configuration for Zend data-layout headers.
 * Production builds use the configure-generated Zend/zend_config.h.
 */
#define HAVE_ALLOCA_H 1
#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_SIGACTION 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

#define SIZEOF_INT __SIZEOF_INT__
#define SIZEOF_LONG __SIZEOF_LONG__
#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__
#define ZEND_DEBUG 0
#define ZEND_MM_ALIGNMENT 8
#define ZEND_MM_ALIGNMENT_LOG2 3
#define ZEND_MM_NEED_EIGHT_BYTE_REALIGNMENT 0
#define ZEND_SIGNALS 1

#endif /* ZEND_CONFIG_H */
