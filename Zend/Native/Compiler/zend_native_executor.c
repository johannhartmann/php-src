#include "Zend/Native/Compiler/zend_native_executor.h"

#include "Zend/Native/Compiler/zend_native_compiler.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/Optimizer/zend_optimizer.h"

#include <string.h>

typedef struct _zend_native_executor_generation {
	zend_op_array *root;
	zend_script script;
	zend_native_compiler *compiler;
	struct _zend_native_executor_generation *next;
} zend_native_executor_generation;

typedef struct _zend_native_executor_request {
	zend_native_executor_generation *generations;
	bool active;
} zend_native_executor_request;

ZEND_TLS zend_native_executor_request zend_native_executor_request_state;
static bool zend_native_executor_installed;

static zend_native_target zend_native_executor_target(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
	return ZEND_NATIVE_TARGET_DARWIN_ARM64;
#elif defined(__linux__) && defined(__x86_64__)
	return ZEND_NATIVE_TARGET_LINUX_AMD64;
#else
# error "The native executor supports Darwin arm64 and Linux x86-64"
#endif
}

static zend_native_executor_generation *
zend_native_executor_find_generation(zend_op_array *root)
{
	zend_native_executor_generation *generation =
		zend_native_executor_request_state.generations;

	while (generation != NULL) {
		if (generation->root == root) {
			return generation;
		}
		generation = generation->next;
	}
	return NULL;
}

static zend_native_executor_generation *
zend_native_executor_create_generation(zend_op_array *root)
{
	zend_native_executor_generation *generation;
	zend_native_compiler_config config;
	zend_native_compile_diagnostic diagnostic;

	generation = ecalloc(1, sizeof(*generation));
	generation->root = root;
	generation->script.main_op_array = *root;
	generation->script.function_table = *EG(function_table);
	generation->script.class_table = *EG(class_table);
	generation->script.filename = root->filename;

	memset(&config, 0, sizeof(config));
	config.script = &generation->script;
	config.target = zend_native_executor_target();
	generation->compiler =
		zend_native_compiler_create(&config, &diagnostic);
	if (generation->compiler == NULL) {
		efree(generation);
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native compiler initialization failed");
		}
		return NULL;
	}
	generation->next = zend_native_executor_request_state.generations;
	zend_native_executor_request_state.generations = generation;
	return generation;
}

zend_result zend_native_executor_startup(void)
{
	if (zend_native_executor_installed) {
		return FAILURE;
	}
	if (zend_native_reentry_startup() == FAILURE) {
		return FAILURE;
	}
	zend_execute_ex = zend_native_executor_execute_ex;
	zend_native_executor_installed = true;
	return SUCCESS;
}

void zend_native_executor_shutdown(void)
{
	zend_native_executor_deactivate();
	if (zend_native_executor_installed
			&& zend_execute_ex == zend_native_executor_execute_ex) {
		zend_execute_ex = execute_ex;
	}
	zend_native_executor_installed = false;
	zend_native_reentry_shutdown();
}

void zend_native_executor_activate(void)
{
	ZEND_ASSERT(zend_native_executor_request_state.generations == NULL);
	zend_native_executor_request_state.active = true;
}

void zend_native_executor_deactivate(void)
{
	zend_native_executor_generation *generation =
		zend_native_executor_request_state.generations;

	zend_native_executor_request_state.generations = NULL;
	zend_native_executor_request_state.active = false;
	while (generation != NULL) {
		zend_native_executor_generation *next = generation->next;

		zend_native_compiler_destroy(generation->compiler);
		efree(generation);
		generation = next;
	}
}

void zend_native_executor_execute_ex(zend_execute_data *execute_data)
{
	zend_native_executor_generation *generation;
	zend_native_diagnostic diagnostic;
	zend_native_status status;

	if (!zend_native_executor_request_state.active
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		zend_throw_error(NULL, "Invalid native userland executor activation");
		return;
	}
	generation = zend_native_executor_find_generation(
		&execute_data->func->op_array);
	if (generation == NULL) {
		generation = zend_native_executor_create_generation(
			&execute_data->func->op_array);
		if (generation == NULL) {
			return;
		}
	} else {
		/*
		 * Hash-table storage may grow while include/autoload adds symbols.
		 * Refresh the borrowed views before compiling another lazy root.
		 */
		generation->script.function_table = *EG(function_table);
		generation->script.class_table = *EG(class_table);
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	status = zend_native_compiler_execute_data(
		generation->compiler, execute_data, &diagnostic);
	if (status == ZEND_NATIVE_BAILOUT) {
		zend_bailout();
	}
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) == NULL) {
		zend_throw_error(NULL, "%s",
			diagnostic.message[0] != '\0'
				? diagnostic.message
				: "Native userland execution failed");
	}
}
