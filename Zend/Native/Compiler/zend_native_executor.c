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
	uint64_t epoch;
	bool persistent;
	struct _zend_native_executor_generation *next;
} zend_native_executor_generation;

typedef struct _zend_native_executor_request {
	zend_native_executor_generation *persistent_generations;
	zend_native_executor_generation *request_generations;
	zend_native_executor_generation *retired_generations;
	uint64_t observed_epoch;
	bool active;
} zend_native_executor_request;

ZEND_TLS zend_native_executor_request zend_native_executor_request_state;
static bool zend_native_executor_installed;
static uint64_t zend_native_executor_epoch = 1;

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
zend_native_executor_find_generation(
	zend_native_executor_generation *generation,
	zend_op_array *root)
{
	while (generation != NULL) {
		if (generation->root == root) {
			return generation;
		}
		generation = generation->next;
	}
	return NULL;
}

static bool zend_native_executor_same_source(
	const zend_string *left, const zend_string *right)
{
	return left == right
		|| (left != NULL && right != NULL
			&& zend_string_equals(left, right));
}

static bool zend_native_executor_build_persistent_script(
	zend_native_executor_generation *generation,
	zend_op_array *root)
{
	zend_string *name;
	zend_function *function;
	zend_class_entry *class_entry;

	memset(&generation->script, 0, sizeof(generation->script));
	generation->script.main_op_array = *root;
	generation->script.filename = root->filename;
	zend_hash_init(
		&generation->script.function_table, 8, NULL, NULL, true);
	zend_hash_init(
		&generation->script.class_table, 8, NULL, NULL, true);

	ZEND_HASH_FOREACH_STR_KEY_PTR(
			EG(function_table), name, function) {
		zend_string *persistent_name;

		if (name == NULL || function == NULL
				|| function->type != ZEND_USER_FUNCTION
				|| (function->common.fn_flags
					& ZEND_ACC_IMMUTABLE) == 0
				|| !zend_native_executor_same_source(
					function->op_array.filename, root->filename)) {
			continue;
		}
		persistent_name = zend_string_init(
			ZSTR_VAL(name), ZSTR_LEN(name), true);
		if (zend_hash_add_ptr(
				&generation->script.function_table,
				persistent_name, function) == NULL) {
			zend_string_release_ex(persistent_name, true);
			goto failure;
		}
		zend_string_release_ex(persistent_name, true);
	} ZEND_HASH_FOREACH_END();

	ZEND_HASH_FOREACH_STR_KEY_PTR(
			EG(class_table), name, class_entry) {
		zend_string *persistent_name;

		if (name == NULL || class_entry == NULL
				|| class_entry->type != ZEND_USER_CLASS
				|| (class_entry->ce_flags & ZEND_ACC_IMMUTABLE) == 0
				|| !zend_native_executor_same_source(
					class_entry->info.user.filename,
					root->filename)) {
			continue;
		}
		persistent_name = zend_string_init(
			ZSTR_VAL(name), ZSTR_LEN(name), true);
		if (zend_hash_add_ptr(
				&generation->script.class_table,
				persistent_name, class_entry) == NULL) {
			zend_string_release_ex(persistent_name, true);
			goto failure;
		}
		zend_string_release_ex(persistent_name, true);
	} ZEND_HASH_FOREACH_END();
	return true;

failure:
	zend_hash_destroy(&generation->script.class_table);
	zend_hash_destroy(&generation->script.function_table);
	return false;
}

static void zend_native_executor_destroy_generation(
	zend_native_executor_generation *generation)
{
	if (generation == NULL) {
		return;
	}
	zend_native_compiler_destroy(generation->compiler);
	if (generation->persistent) {
		zend_hash_destroy(&generation->script.class_table);
		zend_hash_destroy(&generation->script.function_table);
	}
	pefree(generation, generation->persistent);
}

static void zend_native_executor_destroy_generations(
	zend_native_executor_generation **head)
{
	zend_native_executor_generation *generation = *head;

	*head = NULL;
	while (generation != NULL) {
		zend_native_executor_generation *next = generation->next;

		zend_native_executor_destroy_generation(generation);
		generation = next;
	}
}

static void zend_native_executor_reap_retired(void)
{
	zend_native_executor_generation **link =
		&zend_native_executor_request_state.retired_generations;

	while (*link != NULL) {
		zend_native_executor_generation *generation = *link;

		if (!zend_native_compiler_is_quiescent(
				generation->compiler)) {
			link = &generation->next;
			continue;
		}
		*link = generation->next;
		zend_native_executor_destroy_generation(generation);
	}
}

static void zend_native_executor_retire_stale(uint64_t epoch)
{
	zend_native_executor_generation **link =
		&zend_native_executor_request_state.persistent_generations;

	while (*link != NULL) {
		zend_native_executor_generation *generation = *link;

		if (generation->epoch == epoch) {
			link = &generation->next;
			continue;
		}
		*link = generation->next;
		if (zend_native_compiler_is_quiescent(
				generation->compiler)) {
			zend_native_executor_destroy_generation(generation);
		} else {
			generation->next =
				zend_native_executor_request_state.retired_generations;
			zend_native_executor_request_state.retired_generations =
				generation;
		}
	}
	zend_native_executor_reap_retired();
}

static zend_native_executor_generation *
zend_native_executor_create_generation(zend_op_array *root)
{
	zend_native_executor_generation *generation;
	zend_native_compiler_config config;
	zend_native_compile_diagnostic diagnostic;
	bool persistent =
		(root->fn_flags & ZEND_ACC_IMMUTABLE) != 0;

	generation = pecalloc(1, sizeof(*generation), persistent);
	generation->root = root;
	generation->persistent = persistent;
	generation->epoch = __atomic_load_n(
		&zend_native_executor_epoch, __ATOMIC_ACQUIRE);
	if (persistent) {
		if (!zend_native_executor_build_persistent_script(
				generation, root)) {
			pefree(generation, true);
			zend_throw_error(NULL,
				"Native persistent script registry allocation failed");
			return NULL;
		}
	} else {
		generation->script.main_op_array = *root;
		generation->script.function_table = *EG(function_table);
		generation->script.class_table = *EG(class_table);
		generation->script.filename = root->filename;
	}

	memset(&config, 0, sizeof(config));
	config.script = &generation->script;
	config.target = zend_native_executor_target();
	config.persistent = persistent;
	generation->compiler =
		zend_native_compiler_create(&config, &diagnostic);
	if (generation->compiler == NULL) {
		if (persistent) {
			zend_hash_destroy(&generation->script.class_table);
			zend_hash_destroy(&generation->script.function_table);
		}
		pefree(generation, persistent);
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native compiler initialization failed");
		}
		return NULL;
	}
	if (persistent) {
		generation->next =
			zend_native_executor_request_state.persistent_generations;
		zend_native_executor_request_state.persistent_generations =
			generation;
	} else {
		generation->next =
			zend_native_executor_request_state.request_generations;
		zend_native_executor_request_state.request_generations =
			generation;
	}
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
	zend_native_executor_destroy_generations(
		&zend_native_executor_request_state.persistent_generations);
	zend_native_executor_destroy_generations(
		&zend_native_executor_request_state.retired_generations);
	if (zend_native_executor_installed
			&& zend_execute_ex == zend_native_executor_execute_ex) {
		zend_execute_ex = execute_ex;
	}
	zend_native_executor_installed = false;
	zend_native_reentry_shutdown();
}

void zend_native_executor_activate(void)
{
	uint64_t epoch = __atomic_load_n(
		&zend_native_executor_epoch, __ATOMIC_ACQUIRE);

	ZEND_ASSERT(
		zend_native_executor_request_state.request_generations == NULL);
	if (zend_native_executor_request_state.observed_epoch != epoch) {
		zend_native_executor_retire_stale(epoch);
		zend_native_executor_request_state.observed_epoch = epoch;
	} else {
		zend_native_executor_reap_retired();
	}
	zend_native_executor_request_state.active = true;
}

void zend_native_executor_deactivate(void)
{
	zend_native_executor_generation *generation;

	zend_native_executor_request_state.active = false;
	zend_native_executor_destroy_generations(
		&zend_native_executor_request_state.request_generations);
	generation =
		zend_native_executor_request_state.persistent_generations;
	while (generation != NULL) {
		zend_native_compiler_end_request(generation->compiler);
		generation = generation->next;
	}
	zend_native_executor_reap_retired();
}

void zend_native_executor_invalidate(void)
{
	(void) __atomic_add_fetch(
		&zend_native_executor_epoch, 1, __ATOMIC_RELEASE);
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
	if ((execute_data->func->common.fn_flags
			& ZEND_ACC_IMMUTABLE) != 0) {
		generation = zend_native_executor_find_generation(
			zend_native_executor_request_state.persistent_generations,
			&execute_data->func->op_array);
	} else {
		generation = zend_native_executor_find_generation(
			zend_native_executor_request_state.request_generations,
			&execute_data->func->op_array);
	}
	if (generation == NULL) {
		generation = zend_native_executor_create_generation(
			&execute_data->func->op_array);
		if (generation == NULL) {
			return;
		}
	} else if (!generation->persistent) {
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
