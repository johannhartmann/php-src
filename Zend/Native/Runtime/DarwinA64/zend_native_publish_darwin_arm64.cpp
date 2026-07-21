// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

#include <cstdlib>

zend_result zend_native_publish_darwin_arm64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (image == nullptr || image->target != ZEND_NATIVE_TARGET_DARWIN_ARM64
			|| image->target_state == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Darwin A64 publisher requires a non-empty Darwin image");
		return FAILURE;
	}
	zend_native_code *code = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*code)));
	if (code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Darwin native-code state");
		return FAILURE;
	}
	code->target = ZEND_NATIVE_TARGET_DARWIN_ARM64;
	code->slot_count = image->slot_count;
	code->argument_count = image->argument_count;
	if (zend_tpde_map_darwin_arm64(image, code, diag) == FAILURE) {
		std::free(code);
		return FAILURE;
	}
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
	if (code->destroy_target_state != nullptr) {
		code->destroy_target_state(code->target_state);
		code->target_state = nullptr;
	}
#else
	(void) code;
#endif
}
