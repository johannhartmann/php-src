// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

#include <cstdlib>
#include <cstring>

#if defined(__linux__) && defined(__x86_64__)
# include <sys/mman.h>
# include <unistd.h>
#endif

zend_result zend_native_publish_linux_x64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
#if defined(__linux__) && defined(__x86_64__)
	if (image == nullptr || image->target != ZEND_NATIVE_TARGET_LINUX_AMD64
			|| image->text == nullptr || image->text_size == 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Linux x86-64 publisher requires a non-empty Linux image");
		return FAILURE;
	}
	long page_size_value = sysconf(_SC_PAGESIZE);
	if (page_size_value <= 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux page size is unavailable");
		return FAILURE;
	}
	size_t page_size = static_cast<size_t>(page_size_value);
	if (image->text_size > SIZE_MAX - (page_size - 1)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image size overflows page alignment");
		return FAILURE;
	}
	size_t mapping_size = (image->text_size + page_size - 1) & ~(page_size - 1);
	/* Linux is writable-only while copying, then executable-only. Never RWX. */
	void *mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux RW image mapping failed");
		return FAILURE;
	}
	std::memcpy(mapping, image->text, image->text_size);
	if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
		munmap(mapping, mapping_size);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Linux native image could not transition from RW to RX");
		return FAILURE;
	}
	zend_native_code *code = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*code)));
	if (code == nullptr) {
		munmap(mapping, mapping_size);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Linux native-code state");
		return FAILURE;
	}
	code->target = ZEND_NATIVE_TARGET_LINUX_AMD64;
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
		"linux-amd64-prod publication requires native Linux x86-64");
	return FAILURE;
#endif
}
void zend_native_unmap_linux_x64(zend_native_code *code) {
#if defined(__linux__) && defined(__x86_64__)
	if (code->mapping != nullptr && code->mapping_size != 0) {
		munmap(code->mapping, code->mapping_size);
	}
#else
	(void) code;
#endif
}
