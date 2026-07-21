// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

#include <cstdlib>
#include <cstring>

#if defined(__APPLE__) && defined(__aarch64__)
# include <libkern/OSCacheControl.h>
# include <pthread.h>
# include <sys/mman.h>
# include <unistd.h>
#endif

zend_result zend_native_publish_darwin_arm64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (image == nullptr || image->target != ZEND_NATIVE_TARGET_DARWIN_ARM64
			|| image->text == nullptr || image->text_size == 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Darwin A64 publisher requires a non-empty Darwin image");
		return FAILURE;
	}
	long page_size_value = sysconf(_SC_PAGESIZE);
	if (page_size_value <= 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin page size is unavailable");
		return FAILURE;
	}
	size_t page_size = static_cast<size_t>(page_size_value);
	if (image->text_size > SIZE_MAX - (page_size - 1)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin JIT image size overflows page alignment");
		return FAILURE;
	}
	size_t mapping_size = (image->text_size + page_size - 1) & ~(page_size - 1);
	void *mapping = mmap(nullptr, mapping_size,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (mapping == MAP_FAILED) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"MAP_JIT failed for the Darwin A64 image");
		return FAILURE;
	}
	pthread_jit_write_protect_np(0);
	std::memcpy(mapping, image->text, image->text_size);
	sys_icache_invalidate(mapping, image->text_size);
	pthread_jit_write_protect_np(1);
	/* MAP_JIT mappings retain their maximum VM protections on Darwin.  The
	 * per-thread write-protect switch is the platform's W^X boundary: after
	 * this point the publishing thread can execute but cannot write the image. */

	zend_native_code *code = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*code)));
	if (code == nullptr) {
		munmap(mapping, mapping_size);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Darwin native-code state");
		return FAILURE;
	}
	code->target = ZEND_NATIVE_TARGET_DARWIN_ARM64;
	code->mapping = mapping;
	code->mapping_size = mapping_size;
	code->entry = reinterpret_cast<zend_native_entry>(mapping);
	code->slot_count = image->slot_count;
	code->argument_count = image->argument_count;
	code->writable = false;
	code->executable = true;
	*out_code = code;
	return SUCCESS;
#else
	(void) image;
	(void) out_code;
	zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
		"darwin-arm64-dev publication requires native Apple Silicon");
	return FAILURE;
#endif
}

void zend_native_unmap_darwin_arm64(zend_native_code *code) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (code->mapping != nullptr && code->mapping_size != 0) {
		munmap(code->mapping, code->mapping_size);
	}
#else
	(void) code;
#endif
}
