#include "Zend/Native/Compiler/zend_native_executor.h"

#include "Zend/Native/Compiler/zend_native_compiler_internal.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_extensions.h"
#include "Zend/Optimizer/zend_optimizer.h"

#include <stddef.h>
#include <string.h>

typedef struct _zend_native_executor_dispatch
	zend_native_executor_dispatch;

typedef struct _zend_native_executor_generation {
	zend_op_array *root;
	const zend_script *owner;
	zend_script script;
	zend_native_compiler *compiler;
	uint64_t epoch;
	uint32_t active_requests;
	bool persistent;
	bool owns_script_tables;
	struct _zend_native_executor_generation *next;
} zend_native_executor_generation;

typedef struct _zend_native_executor_lease {
	zend_native_executor_generation *generation;
	struct _zend_native_executor_lease *next;
} zend_native_executor_lease;

typedef struct _zend_native_executor_epoch_ref {
	uint64_t value;
	uint32_t active_requests;
	struct _zend_native_executor_epoch_ref *next;
} zend_native_executor_epoch_ref;

typedef struct _zend_native_executor_dispatch_key {
	uintptr_t thread_id;
	uintptr_t source_opcodes;
} zend_native_executor_dispatch_key;

typedef struct _zend_native_executor_request {
	zend_native_executor_generation *request_generations;
	zend_native_executor_lease *leases;
	zend_native_executor_epoch_ref *epoch;
	HashTable dispatch;
	HashTable owners;
	zend_native_compiler *active_compiler;
	uint64_t observed_epoch;
	uint32_t execution_depth;
	const zend_op *pending_opcodes;
	zend_native_executor_dispatch *pending_dispatch;
	bool dispatch_active;
	bool active;
} zend_native_executor_request;

struct _zend_native_executor_dispatch {
	zend_native_executor_generation *generation;
	zend_native_entry_cell *entry_cell;
	zend_op_array *op_array;
	uint64_t epoch;
	bool persistent;
};

ZEND_TLS zend_native_executor_request zend_native_executor_request_state;
static bool zend_native_executor_installed;
static uint64_t zend_native_executor_epoch = 1;
static zend_native_executor_generation
	*zend_native_executor_persistent_generations;
static zend_native_executor_generation
	*zend_native_executor_retired_generations;
static zend_native_executor_epoch_ref *zend_native_executor_epochs;
static HashTable zend_native_executor_persistent_dispatches;
static bool zend_native_executor_persistent_dispatches_active;
static HashTable zend_native_executor_preloaded_owners;
static bool zend_native_executor_preloaded_owners_active;
#ifdef ZTS
static MUTEX_T zend_native_executor_generation_mutex;
#endif
static int zend_native_executor_bundle_rid = -1;
static int zend_native_executor_entry_handle = -1;

static zend_native_executor_generation *
zend_native_executor_create_or_acquire_generation(zend_op_array *root);
static zend_native_entry_cell *
zend_native_executor_resolve_external_reentry(
	void *context, zend_function *function);
static void zend_native_executor_reap_retired_locked(void);

static void zend_native_executor_generation_lock(void)
{
#ifdef ZTS
	ZEND_ASSERT(zend_native_executor_generation_mutex != NULL);
	tsrm_mutex_lock(zend_native_executor_generation_mutex);
#endif
}

static void zend_native_executor_generation_unlock(void)
{
#ifdef ZTS
	tsrm_mutex_unlock(zend_native_executor_generation_mutex);
#endif
}

static zend_native_executor_epoch_ref *
zend_native_executor_find_epoch_locked(uint64_t value)
{
	zend_native_executor_epoch_ref *epoch = zend_native_executor_epochs;

	while (epoch != NULL) {
		if (epoch->value == value) {
			return epoch;
		}
		epoch = epoch->next;
	}
	return NULL;
}

static bool zend_native_executor_epoch_is_active_locked(uint64_t value)
{
	zend_native_executor_epoch_ref *epoch =
		zend_native_executor_find_epoch_locked(value);

	return epoch != NULL && epoch->active_requests != 0;
}

static void zend_native_executor_acquire_request_epoch(void)
{
	zend_native_executor_epoch_ref *epoch;
	uint64_t value;

	ZEND_ASSERT(zend_native_executor_request_state.epoch == NULL);
	for (;;) {
		value = __atomic_load_n(
			&zend_native_executor_epoch, __ATOMIC_ACQUIRE);
		zend_native_executor_generation_lock();
		if (value != __atomic_load_n(
				&zend_native_executor_epoch, __ATOMIC_RELAXED)) {
			zend_native_executor_generation_unlock();
			continue;
		}
		epoch = zend_native_executor_find_epoch_locked(value);
		if (epoch == NULL) {
			epoch = pecalloc(1, sizeof(*epoch), true);
			epoch->value = value;
			epoch->next = zend_native_executor_epochs;
			zend_native_executor_epochs = epoch;
		}
		epoch->active_requests++;
		zend_native_executor_request_state.epoch = epoch;
		zend_native_executor_request_state.observed_epoch = value;
		zend_native_executor_generation_unlock();
		return;
	}
}

static void zend_native_executor_release_request_epoch(void)
{
	zend_native_executor_epoch_ref *epoch =
		zend_native_executor_request_state.epoch;
	zend_native_executor_epoch_ref **link;

	if (epoch == NULL) {
		return;
	}
	zend_native_executor_request_state.epoch = NULL;
	zend_native_executor_generation_lock();
	ZEND_ASSERT(epoch->active_requests != 0);
	epoch->active_requests--;
	zend_native_executor_reap_retired_locked();
	if (epoch->active_requests == 0) {
		link = &zend_native_executor_epochs;
		while (*link != NULL && *link != epoch) {
			link = &(*link)->next;
		}
		if (*link == epoch) {
			*link = epoch->next;
			pefree(epoch, true);
		}
	}
	zend_native_executor_generation_unlock();
}

static zend_native_executor_dispatch *
zend_native_executor_dispatch_load(const zend_op_array *op_array)
{
	void **run_time_cache;

	if (op_array == NULL || zend_native_executor_entry_handle < 0) {
		return NULL;
	}
	run_time_cache = RUN_TIME_CACHE(op_array);
	if (run_time_cache == NULL) {
		return NULL;
	}
	return run_time_cache[zend_native_executor_entry_handle];
}

static void **zend_native_executor_dispatch_slot(
	const zend_op_array *op_array)
{
	void **run_time_cache;

	if (op_array == NULL || zend_native_executor_entry_handle < 0) {
		return NULL;
	}
	run_time_cache = RUN_TIME_CACHE(op_array);
	return run_time_cache != NULL
		? &run_time_cache[zend_native_executor_entry_handle] : NULL;
}

static zend_native_executor_dispatch_key
zend_native_executor_dispatch_key_make(const zend_op_array *op_array)
{
	zend_native_executor_dispatch_key key = {0};

#ifdef ZTS
	key.thread_id = (uintptr_t) tsrm_thread_id();
#endif
	/*
	 * OPcache main op_arrays own a request-local runtime cache, while their
	 * immutable opcode storage is stable for the process generation.  The
	 * process-local registry must therefore key the source codeunit rather
	 * than the transient cache allocation.
	 */
	key.source_opcodes = (uintptr_t) op_array->opcodes;
	return key;
}

static void zend_native_executor_dispatch_store(
	zend_op_array *op_array, zend_native_executor_dispatch *dispatch)
{
	void **run_time_cache;

	ZEND_ASSERT(op_array != NULL);
	ZEND_ASSERT(zend_native_executor_entry_handle >= 0);
	run_time_cache = RUN_TIME_CACHE(op_array);
	ZEND_ASSERT(run_time_cache != NULL);
	run_time_cache[zend_native_executor_entry_handle] = dispatch;
}

static zend_always_inline uint64_t zend_native_executor_dispatch_epoch_load(
	const zend_native_executor_dispatch *dispatch)
{
	return dispatch != NULL
		? __atomic_load_n(&dispatch->epoch, __ATOMIC_ACQUIRE) : 0;
}

static zend_always_inline void zend_native_executor_dispatch_epoch_store(
	zend_native_executor_dispatch *dispatch, uint64_t epoch)
{
	__atomic_store_n(&dispatch->epoch, epoch, __ATOMIC_RELEASE);
}

static void zend_native_executor_dispatch_clear(
	zend_native_executor_dispatch *dispatch)
{
	void **run_time_cache;

	if (dispatch == NULL || dispatch->persistent
			|| dispatch->op_array == NULL
			|| zend_native_executor_entry_handle < 0) {
		return;
	}
	run_time_cache = RUN_TIME_CACHE(dispatch->op_array);
	if (run_time_cache == NULL) {
		return;
	}
	if (run_time_cache[zend_native_executor_entry_handle] == dispatch) {
		run_time_cache[zend_native_executor_entry_handle] = NULL;
	}
}

static void zend_native_executor_dispatch_dtor(zval *value)
{
	zend_native_executor_dispatch *dispatch = Z_PTR_P(value);

	/*
	 * During request deactivation, temporary op_arrays (notably closures
	 * owned by completed or destroyed fibers) may already be gone. Their
	 * request-local runtime caches are gone with them, so no cache word needs
	 * clearing. Deletions while the request is active still detach the
	 * dispatch atomically before releasing it.
	 */
	if (zend_native_executor_request_state.active) {
		zend_native_executor_dispatch_clear(dispatch);
	}
	efree(dispatch);
}

static void zend_native_executor_persistent_dispatch_dtor(zval *value)
{
	zend_native_executor_dispatch *dispatch = Z_PTR_P(value);

	pefree(dispatch, true);
}

#define ZEND_NATIVE_OPCACHE_BUNDLE_MAGIC UINT64_C(0x313342434f4e5a)
#define ZEND_NATIVE_OPCACHE_BUNDLE_FORMAT 1u
#define ZEND_NATIVE_OPCACHE_BUNDLE_HEAP 1u
#define ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT 2u
#define ZEND_NATIVE_OPCACHE_BUNDLE_REQUEST_ARENA 3u

typedef struct _zend_native_opcache_bundle {
	uint64_t magic;
	uint32_t format;
	uint32_t storage;
	size_t size;
	unsigned char bytes[1];
} zend_native_opcache_bundle;

static zend_native_opcache_bundle *zend_native_executor_bundle(
	const zend_op_array *op_array)
{
	zend_native_opcache_bundle *bundle;

	if (op_array == NULL || zend_native_executor_bundle_rid < 0) {
		return NULL;
	}
	bundle = op_array->reserved[zend_native_executor_bundle_rid];
	return bundle != NULL
			&& bundle->magic == ZEND_NATIVE_OPCACHE_BUNDLE_MAGIC
			&& bundle->format == ZEND_NATIVE_OPCACHE_BUNDLE_FORMAT
			&& bundle->size != 0
		? bundle : NULL;
}

static size_t zend_native_executor_bundle_allocation_size(
	size_t payload_size)
{
	if (payload_size > SIZE_MAX
			- offsetof(zend_native_opcache_bundle, bytes)) {
		return 0;
	}
	return offsetof(zend_native_opcache_bundle, bytes) + payload_size;
}

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
zend_native_executor_find_root_generation(
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

static zend_native_executor_generation *
zend_native_executor_find_function_generation(
	zend_native_executor_generation *generation,
	zend_function *function)
{
	while (generation != NULL) {
		if (zend_native_compiler_lookup(
				generation->compiler, function) != NULL) {
			return generation;
		}
		generation = generation->next;
	}
	return NULL;
}

static zend_native_executor_generation *
zend_native_executor_find_function_epoch_generation(
	zend_native_executor_generation *generation,
	zend_function *function, uint64_t epoch)
{
	while (generation != NULL) {
		if (generation->epoch == epoch
				&& zend_native_compiler_lookup(
					generation->compiler, function) != NULL) {
			return generation;
		}
		generation = generation->next;
	}
	return NULL;
}

static zend_native_executor_generation *
zend_native_executor_find_owner_epoch_generation(
	zend_native_executor_generation *generation,
	const zend_script *owner, uint64_t epoch)
{
	while (generation != NULL) {
		if (generation->owner == owner && generation->epoch == epoch) {
			return generation;
		}
		generation = generation->next;
	}
	return NULL;
}

static const zend_script *zend_native_executor_script_owner(
	const zend_op_array *op_array)
{
	const zend_script *owner;

	if (op_array == NULL || op_array->opcodes == NULL) {
		return NULL;
	}
	if (zend_native_executor_request_state.dispatch_active) {
		owner = zend_hash_index_find_ptr(
			&zend_native_executor_request_state.owners,
			(zend_ulong) (uintptr_t) op_array->opcodes);
		if (owner != NULL) {
			return owner;
		}
	}
	return zend_native_executor_preloaded_owners_active
		? zend_hash_index_find_ptr(
			&zend_native_executor_preloaded_owners,
			(zend_ulong) (uintptr_t) op_array->opcodes)
		: NULL;
}

static bool zend_native_executor_build_owner_script(
	zend_native_executor_generation *generation,
	zend_op_array *root, const zend_script *owner, bool persistent)
{
	const HashTable *function_table =
		owner != NULL ? &owner->function_table : EG(function_table);
	const HashTable *class_table =
		owner != NULL ? &owner->class_table : EG(class_table);
	zend_string *name;
	zend_function *function;
	zend_class_entry *class_entry;

	if (generation == NULL || root == NULL) {
		return false;
	}
	memset(&generation->script, 0, sizeof(generation->script));
	generation->script.main_op_array =
		owner != NULL ? owner->main_op_array : *root;
	generation->script.filename =
		owner != NULL ? owner->filename : root->filename;
	zend_hash_init(
		&generation->script.function_table, 8, NULL, NULL, persistent);
	zend_hash_init(
		&generation->script.class_table, 8, NULL, NULL, persistent);

	ZEND_HASH_FOREACH_STR_KEY_PTR(
			function_table, name, function) {
		zend_string *persistent_name;
		zend_function *runtime_function;

		if (name == NULL || function == NULL
				|| function->type != ZEND_USER_FUNCTION) {
			continue;
		}
		runtime_function = zend_hash_find_ptr(EG(function_table), name);
		if (runtime_function != NULL
				&& runtime_function->type == ZEND_USER_FUNCTION
				&& runtime_function->op_array.opcodes
					== function->op_array.opcodes) {
			function = runtime_function;
		}
		persistent_name = zend_string_init(
			ZSTR_VAL(name), ZSTR_LEN(name), persistent);
		if (zend_hash_add_ptr(
				&generation->script.function_table,
				persistent_name, function) == NULL) {
			zend_string_release_ex(persistent_name, persistent);
			goto failure;
		}
		zend_string_release_ex(persistent_name, persistent);
	} ZEND_HASH_FOREACH_END();

	ZEND_HASH_FOREACH_STR_KEY_PTR(
			class_table, name, class_entry) {
		zend_string *persistent_name;
		zend_class_entry *runtime_class;

		if (name == NULL || class_entry == NULL
				|| class_entry->type != ZEND_USER_CLASS) {
			continue;
		}
		runtime_class = zend_hash_find_ptr(EG(class_table), name);
		if (runtime_class != NULL
				&& runtime_class->type == ZEND_USER_CLASS) {
			class_entry = runtime_class;
		}
		persistent_name = zend_string_init(
			ZSTR_VAL(name), ZSTR_LEN(name), persistent);
		if (zend_hash_add_ptr(
				&generation->script.class_table,
				persistent_name, class_entry) == NULL) {
			zend_string_release_ex(persistent_name, persistent);
			goto failure;
		}
		zend_string_release_ex(persistent_name, persistent);
	} ZEND_HASH_FOREACH_END();
	generation->owns_script_tables = true;
	return true;

failure:
	zend_hash_destroy(&generation->script.class_table);
	zend_hash_destroy(&generation->script.function_table);
	return false;
}

static void zend_native_executor_destroy_generation(
	zend_native_executor_generation *generation)
{
	zend_native_executor_dispatch *dispatch;

	if (generation == NULL) {
		return;
	}
	if (generation->persistent
			&& zend_native_executor_persistent_dispatches_active) {
		ZEND_HASH_FOREACH_PTR(
				&zend_native_executor_persistent_dispatches,
				dispatch) {
			if (dispatch->generation == generation) {
				/*
				 * Runtime-cache slots outlive OPcache generations, notably
				 * in other ZTS threads. Keep their process-local dispatch
				 * object stable and tombstone only the retired binding.
				 */
				zend_native_executor_dispatch_epoch_store(dispatch, 0);
				dispatch->entry_cell = NULL;
				dispatch->generation = NULL;
			}
		} ZEND_HASH_FOREACH_END();
	}
	zend_native_compiler_destroy(generation->compiler);
	if (generation->owns_script_tables) {
		zend_hash_destroy(&generation->script.class_table);
		zend_hash_destroy(&generation->script.function_table);
	}
	pefree(generation, generation->persistent);
}

static void zend_native_executor_bind_dispatch(
	zend_native_executor_generation *generation,
	zend_op_array *op_array,
	zend_native_entry_cell *entry_cell)
{
	zend_native_executor_dispatch *dispatch;
	zend_native_executor_dispatch_key key;
	void **slot;

	if (generation == NULL || op_array == NULL || entry_cell == NULL) {
		return;
	}
	if (RUN_TIME_CACHE(op_array) == NULL) {
		zend_init_func_run_time_cache(op_array);
	}
	dispatch = zend_native_executor_dispatch_load(op_array);
	if (dispatch != NULL
			&& zend_native_executor_dispatch_epoch_load(dispatch)
				== generation->epoch
			&& dispatch->generation == generation
			&& dispatch->entry_cell == entry_cell) {
		return;
	}
	if (generation->persistent) {
		slot = zend_native_executor_dispatch_slot(op_array);
		ZEND_ASSERT(slot != NULL);
		key = zend_native_executor_dispatch_key_make(op_array);
		zend_native_executor_generation_lock();
		dispatch = zend_hash_str_find_ptr(
			&zend_native_executor_persistent_dispatches,
			(const char *) &key, sizeof(key));
		if (dispatch == NULL) {
			dispatch = pecalloc(1, sizeof(*dispatch), true);
			zend_hash_str_add_ptr(
				&zend_native_executor_persistent_dispatches,
				(const char *) &key, sizeof(key), dispatch);
		}
		dispatch->generation = generation;
		dispatch->entry_cell = entry_cell;
		dispatch->op_array = op_array;
		dispatch->persistent = true;
		zend_native_executor_dispatch_epoch_store(
			dispatch, generation->epoch);
		*slot = dispatch;
		zend_native_executor_generation_unlock();
		return;
	}
	dispatch = emalloc(sizeof(*dispatch));
	memset(dispatch, 0, sizeof(*dispatch));
	dispatch->generation = generation;
	dispatch->entry_cell = entry_cell;
	dispatch->op_array = op_array;
	dispatch->epoch = generation->epoch;
	zend_hash_index_update_ptr(
		&zend_native_executor_request_state.dispatch,
		(zend_ulong) (uintptr_t) op_array, dispatch);
	zend_native_executor_dispatch_store(op_array, dispatch);
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

static bool zend_native_executor_request_has_lease(
	const zend_native_executor_generation *generation)
{
	zend_native_executor_lease *lease =
		zend_native_executor_request_state.leases;

	while (lease != NULL) {
		if (lease->generation == generation) {
			return true;
		}
		lease = lease->next;
	}
	return false;
}

static zend_native_executor_generation *
zend_native_executor_find_leased_function(zend_function *function)
{
	zend_native_executor_lease *lease =
		zend_native_executor_request_state.leases;

	while (lease != NULL) {
		if (zend_native_compiler_lookup(
				lease->generation->compiler, function) != NULL) {
			return lease->generation;
		}
		lease = lease->next;
	}
	return NULL;
}

static bool zend_native_executor_acquire_generation_locked(
	zend_native_executor_generation *generation)
{
	zend_native_executor_lease *lease;

	if (generation == NULL) {
		return false;
	}
	if (zend_native_executor_request_has_lease(generation)) {
		return true;
	}
	lease = emalloc(sizeof(*lease));
	lease->generation = generation;
	lease->next = zend_native_executor_request_state.leases;
	zend_native_executor_request_state.leases = lease;
	generation->active_requests++;
	return true;
}

static void zend_native_executor_reap_retired_locked(void)
{
	zend_native_executor_generation **link =
		&zend_native_executor_retired_generations;

	while (*link != NULL) {
		zend_native_executor_generation *generation = *link;

		if (generation->active_requests != 0
				|| zend_native_executor_epoch_is_active_locked(
					generation->epoch)
				|| !zend_native_compiler_is_quiescent(
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
		&zend_native_executor_persistent_generations;

	zend_native_executor_generation_lock();
	while (*link != NULL) {
		zend_native_executor_generation *generation = *link;

		if (generation->epoch == epoch) {
			link = &generation->next;
			continue;
		}
		*link = generation->next;
		if (!zend_native_executor_epoch_is_active_locked(
				generation->epoch)
				&& generation->active_requests == 0
				&& zend_native_compiler_is_quiescent(
				generation->compiler)) {
			zend_native_executor_destroy_generation(generation);
		} else {
			generation->next = zend_native_executor_retired_generations;
			zend_native_executor_retired_generations = generation;
		}
	}
	zend_native_executor_reap_retired_locked();
	zend_native_executor_generation_unlock();
}

static void zend_native_executor_release_request_leases(void)
{
	zend_native_executor_lease *lease =
		zend_native_executor_request_state.leases;

	if (zend_native_executor_request_state.active_compiler != NULL) {
		zend_native_compiler_deactivate_session(
			zend_native_executor_request_state.active_compiler);
		zend_native_executor_request_state.active_compiler = NULL;
	}
	zend_native_executor_request_state.leases = NULL;
	while (lease != NULL) {
		zend_native_executor_lease *next = lease->next;

		zend_native_compiler_end_request(lease->generation->compiler);
		zend_native_executor_generation_lock();
		ZEND_ASSERT(lease->generation->active_requests != 0);
		lease->generation->active_requests--;
		zend_native_executor_reap_retired_locked();
		zend_native_executor_generation_unlock();
		efree(lease);
		lease = next;
	}
}

static zend_result zend_native_executor_activate_generation(
	zend_native_executor_generation *generation)
{
	zend_native_compiler *compiler;

	if (generation == NULL
			|| generation->epoch
				!= zend_native_executor_request_state.observed_epoch) {
		return FAILURE;
	}
	compiler = generation->compiler;
	if (zend_native_executor_request_state.active_compiler == compiler) {
		return SUCCESS;
	}
	if (!zend_native_executor_request_has_lease(generation)) {
		zend_native_executor_generation_lock();
		if (!zend_native_executor_acquire_generation_locked(generation)) {
			zend_native_executor_generation_unlock();
			return FAILURE;
		}
		zend_native_executor_generation_unlock();
	}
	if (zend_native_executor_request_state.active_compiler != NULL) {
		zend_native_compiler_deactivate_session(
			zend_native_executor_request_state.active_compiler);
		zend_native_executor_request_state.active_compiler = NULL;
	}
	if (zend_native_compiler_activate_session(compiler) == FAILURE) {
		return FAILURE;
	}
	zend_native_executor_request_state.active_compiler = compiler;
	return SUCCESS;
}

static void zend_native_executor_deactivate_compiler(void)
{
	if (zend_native_executor_request_state.active_compiler == NULL) {
		return;
	}
	zend_native_compiler_deactivate_session(
		zend_native_executor_request_state.active_compiler);
	zend_native_executor_request_state.active_compiler = NULL;
}

static zend_native_executor_generation *
zend_native_executor_create_generation(zend_op_array *root)
{
	zend_native_executor_generation *generation;
	zend_native_compiler_config config;
	zend_native_compile_diagnostic diagnostic;
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(root);
	const zend_script *owner =
		zend_native_executor_script_owner(root);
	bool persistent =
		owner != NULL
		|| (bundle != NULL
			&& bundle->storage
				== ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT);

	generation = pecalloc(1, sizeof(*generation), persistent);
	generation->root = persistent ? NULL : root;
	generation->owner = owner;
	generation->persistent = persistent;
	generation->epoch = zend_native_executor_request_state.epoch != NULL
		? zend_native_executor_request_state.observed_epoch
		: __atomic_load_n(
			&zend_native_executor_epoch, __ATOMIC_ACQUIRE);
	if (persistent || bundle != NULL) {
		if (!zend_native_executor_build_owner_script(
				generation, root, owner, persistent)) {
			pefree(generation, persistent);
			zend_throw_error(NULL,
				"Native script registry allocation failed");
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
	config.source_probe = zend_native_runtime_source_probe_enabled();
	config.direct_reentry = true;
	if (persistent) {
		config.external_reentry_resolver =
			zend_native_executor_resolve_external_reentry;
		config.external_reentry_context = generation;
	}
	generation->compiler =
		zend_native_compiler_create(&config, &diagnostic);
	if (generation->compiler == NULL) {
		if (generation->owns_script_tables) {
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
	if (bundle != NULL
			&& zend_native_compiler_import_bundle(
				generation->compiler, bundle->bytes, bundle->size,
				&diagnostic) == FAILURE) {
		zend_native_compiler_destroy(generation->compiler);
		if (generation->owns_script_tables) {
			zend_hash_destroy(&generation->script.class_table);
			zend_hash_destroy(&generation->script.function_table);
		}
		pefree(generation, persistent);
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Persistent native image publication failed");
		}
		return NULL;
	}
	if (persistent) {
		zend_native_compiler_end_request(generation->compiler);
	}
	if (!persistent) {
		generation->next =
			zend_native_executor_request_state.request_generations;
		zend_native_executor_request_state.request_generations =
			generation;
	}
	return generation;
}

static zend_native_executor_generation *
zend_native_executor_find_persistent_function(zend_function *function)
{
	zend_native_executor_generation *generation;

	zend_native_executor_generation_lock();
	generation = zend_native_executor_find_function_epoch_generation(
		zend_native_executor_persistent_generations, function,
		zend_native_executor_request_state.observed_epoch);
	if (generation == NULL) {
		generation = zend_native_executor_find_function_epoch_generation(
			zend_native_executor_retired_generations, function,
			zend_native_executor_request_state.observed_epoch);
	}
	if (generation != NULL) {
		(void) zend_native_executor_acquire_generation_locked(generation);
	}
	zend_native_executor_generation_unlock();
	return generation;
}

static zend_native_executor_generation *
zend_native_executor_create_or_acquire_generation(zend_op_array *root)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(root);
	const zend_script *owner =
		zend_native_executor_script_owner(root);
	bool persistent =
		owner != NULL
		|| (bundle != NULL
			&& bundle->storage
				== ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT);
	zend_native_executor_generation *generation;

	if (!persistent) {
		return zend_native_executor_create_generation(root);
	}
	if (owner == NULL) {
		zend_throw_error(NULL,
			"Persistent native script owner is unavailable");
		return NULL;
	}
	zend_native_executor_generation_lock();
	generation = zend_native_executor_find_owner_epoch_generation(
		zend_native_executor_persistent_generations, owner,
		zend_native_executor_request_state.observed_epoch);
	if (generation == NULL) {
		generation = zend_native_executor_find_owner_epoch_generation(
			zend_native_executor_retired_generations, owner,
			zend_native_executor_request_state.observed_epoch);
	}
	if (generation == NULL) {
		generation = zend_native_executor_create_generation(root);
		if (generation != NULL) {
			if (generation->epoch == __atomic_load_n(
					&zend_native_executor_epoch, __ATOMIC_RELAXED)) {
				generation->next =
					zend_native_executor_persistent_generations;
				zend_native_executor_persistent_generations =
					generation;
			} else {
				generation->next =
					zend_native_executor_retired_generations;
				zend_native_executor_retired_generations =
					generation;
			}
		}
	}
	if (generation != NULL) {
		(void) zend_native_executor_acquire_generation_locked(generation);
	}
	zend_native_executor_generation_unlock();
	return generation;
}

static zend_native_entry_cell *
zend_native_executor_resolve_external_reentry(
	void *context, zend_function *function)
{
	zend_native_executor_generation *generation;
	zend_native_compile_diagnostic diagnostic;
	zend_native_entry_cell *entry_cell;

	(void) context;
	if (function == NULL || !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	generation = zend_native_executor_find_leased_function(function);
	if (generation == NULL) {
		generation = zend_native_executor_find_persistent_function(function);
	}
	if (generation == NULL) {
		generation = zend_native_executor_create_or_acquire_generation(
			&function->op_array);
	}
	if (generation == NULL) {
		return NULL;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	entry_cell = zend_native_compiler_prepare_function(
		generation->compiler, function, &diagnostic);
	if (entry_cell == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native external codeunit compilation failed");
		}
		return NULL;
	}
	return entry_cell;
}

zend_result zend_native_executor_startup(void)
{
	if (zend_native_executor_installed) {
		return FAILURE;
	}
#ifdef ZTS
	zend_native_executor_generation_mutex = tsrm_mutex_alloc();
	if (zend_native_executor_generation_mutex == NULL) {
		return FAILURE;
	}
#endif
	if (zend_native_reentry_startup() == FAILURE) {
#ifdef ZTS
		tsrm_mutex_free(zend_native_executor_generation_mutex);
		zend_native_executor_generation_mutex = NULL;
#endif
		return FAILURE;
	}
	zend_native_executor_bundle_rid =
		zend_get_resource_handle("Zend Native");
	zend_native_executor_entry_handle =
		zend_get_op_array_extension_handle("Zend Native");
	if (zend_native_executor_bundle_rid < 0
			|| zend_native_executor_entry_handle < 0) {
		zend_native_reentry_shutdown();
		zend_native_executor_bundle_rid = -1;
		zend_native_executor_entry_handle = -1;
#ifdef ZTS
		tsrm_mutex_free(zend_native_executor_generation_mutex);
		zend_native_executor_generation_mutex = NULL;
#endif
		return FAILURE;
	}
	if (!zend_native_executor_preloaded_owners_active) {
		zend_hash_init(
			&zend_native_executor_preloaded_owners,
			32, NULL, NULL, true);
		zend_native_executor_preloaded_owners_active = true;
	}
	zend_hash_init(
		&zend_native_executor_persistent_dispatches,
		32, NULL, zend_native_executor_persistent_dispatch_dtor, true);
	zend_native_executor_persistent_dispatches_active = true;
	zend_execute_ex = zend_native_executor_execute_ex;
	zend_native_executor_installed = true;
	return SUCCESS;
}

void zend_native_executor_shutdown(void)
{
	zend_native_executor_deactivate();
	zend_native_executor_destroy_generations(
		&zend_native_executor_persistent_generations);
	zend_native_executor_destroy_generations(
		&zend_native_executor_retired_generations);
	if (zend_native_executor_installed
			&& zend_execute_ex == zend_native_executor_execute_ex) {
		zend_execute_ex = execute_ex;
	}
	zend_native_executor_installed = false;
	zend_native_executor_bundle_rid = -1;
	zend_native_executor_entry_handle = -1;
	if (zend_native_executor_persistent_dispatches_active) {
		zend_hash_destroy(
			&zend_native_executor_persistent_dispatches);
		zend_native_executor_persistent_dispatches_active = false;
	}
	if (zend_native_executor_preloaded_owners_active) {
		zend_hash_destroy(&zend_native_executor_preloaded_owners);
		zend_native_executor_preloaded_owners_active = false;
	}
	zend_native_reentry_shutdown();
#ifdef ZTS
	tsrm_mutex_free(zend_native_executor_generation_mutex);
	zend_native_executor_generation_mutex = NULL;
#endif
}

void zend_native_executor_activate(void)
{
	uint64_t previous_epoch =
		zend_native_executor_request_state.observed_epoch;

	ZEND_ASSERT(
		zend_native_executor_request_state.request_generations == NULL);
	ZEND_ASSERT(!zend_native_executor_request_state.dispatch_active);
	ZEND_ASSERT(zend_native_executor_request_state.epoch == NULL);
	zend_native_executor_acquire_request_epoch();
	if (previous_epoch
			!= zend_native_executor_request_state.observed_epoch) {
		zend_native_executor_retire_stale(
			zend_native_executor_request_state.observed_epoch);
	}
	zend_hash_init(
		&zend_native_executor_request_state.dispatch, 32, NULL,
		zend_native_executor_dispatch_dtor, false);
	zend_hash_init(
		&zend_native_executor_request_state.owners, 8, NULL, NULL, false);
	zend_native_executor_request_state.pending_opcodes = NULL;
	zend_native_executor_request_state.pending_dispatch = NULL;
	zend_native_executor_request_state.dispatch_active = true;
	zend_native_executor_request_state.active = true;
}

void zend_native_executor_deactivate(void)
{
	zend_native_executor_request_state.active = false;
	zend_native_executor_request_state.pending_opcodes = NULL;
	zend_native_executor_request_state.pending_dispatch = NULL;
	zend_native_executor_deactivate_compiler();
	if (zend_native_executor_request_state.dispatch_active) {
		zend_hash_destroy(
			&zend_native_executor_request_state.owners);
		zend_hash_destroy(
			&zend_native_executor_request_state.dispatch);
		zend_native_executor_request_state.dispatch_active = false;
	}
	zend_native_executor_destroy_generations(
		&zend_native_executor_request_state.request_generations);
	zend_native_executor_release_request_leases();
	zend_native_executor_release_request_epoch();
}

void zend_native_executor_invalidate(void)
{
	(void) __atomic_add_fetch(
		&zend_native_executor_epoch, 1, __ATOMIC_RELEASE);

	/*
	 * OPcache RINIT runs after Zend request activation but before userland.
	 * Refresh immediately at that boundary so a worker never serves one
	 * extra request from a generation invalidated by another process.
	 * Invalidations raised by executing userland retain the current lease
	 * until request shutdown.
	 */
	if (zend_native_executor_request_state.active
			&& EG(current_execute_data) == NULL) {
		uint64_t previous_epoch =
			zend_native_executor_request_state.observed_epoch;

		zend_native_executor_release_request_leases();
		zend_native_executor_release_request_epoch();
		zend_native_executor_acquire_request_epoch();
		if (previous_epoch
				!= zend_native_executor_request_state.observed_epoch) {
			zend_native_executor_retire_stale(
				zend_native_executor_request_state.observed_epoch);
		}
	}
}

void zend_native_executor_set_source_probe(
	zend_native_source_probe_t probe, void *context)
{
	zend_native_runtime_set_source_probe(probe, context);
}

zend_result zend_native_executor_prepare_script(zend_script *script)
{
	zend_native_compiler_config config;
	zend_native_compile_diagnostic diagnostic;
	zend_native_compiler *compiler;
	zend_native_opcache_bundle *bundle;
	unsigned char *bytes = NULL;
	size_t size = 0;
	size_t allocation_size;

	if (script == NULL || zend_native_executor_bundle_rid < 0) {
		return FAILURE;
	}
	if (zend_native_executor_bundle(&script->main_op_array) != NULL) {
		return SUCCESS;
	}
	memset(&config, 0, sizeof(config));
	memset(&diagnostic, 0, sizeof(diagnostic));
	config.script = script;
	config.target = zend_native_executor_target();
	config.source_probe = zend_native_runtime_source_probe_enabled();
	config.defer_publication = true;
	compiler = zend_native_compiler_create(&config, &diagnostic);
	if (compiler == NULL) {
		return FAILURE;
	}
	if (zend_native_compiler_compile(
			compiler, &script->main_op_array, NULL, 0,
			&diagnostic) == FAILURE
			|| zend_native_compiler_serialize_bundle(
				compiler, &bytes, &size, &diagnostic) == FAILURE) {
		zend_native_compiler_destroy(compiler);
		return FAILURE;
	}
	zend_native_compiler_destroy(compiler);
	allocation_size =
		zend_native_executor_bundle_allocation_size(size);
	if (allocation_size == 0) {
		zend_native_compiler_bundle_destroy(bytes);
		return FAILURE;
	}
	bundle = emalloc(allocation_size);
	bundle->magic = ZEND_NATIVE_OPCACHE_BUNDLE_MAGIC;
	bundle->format = ZEND_NATIVE_OPCACHE_BUNDLE_FORMAT;
	bundle->storage = ZEND_NATIVE_OPCACHE_BUNDLE_HEAP;
	bundle->size = size;
	memcpy(bundle->bytes, bytes, size);
	zend_native_compiler_bundle_destroy(bytes);
	script->main_op_array.reserved[zend_native_executor_bundle_rid] =
		bundle;
	return SUCCESS;
}

static bool zend_native_executor_register_preloaded_op_array(
	HashTable *owners, const zend_op_array *op_array,
	const zend_script *script, uint32_t depth)
{
	uint32_t index;

	if (owners == NULL || op_array == NULL || script == NULL
			|| depth > 64) {
		return false;
	}
	if (op_array->opcodes != NULL
			&& zend_hash_index_update_ptr(
				owners,
				(zend_ulong) (uintptr_t) op_array->opcodes,
				(void *) script) == NULL) {
		return false;
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (!zend_native_executor_register_preloaded_op_array(
				owners, op_array->dynamic_func_defs[index],
				script, depth + 1)) {
			return false;
		}
	}
	return true;
}

zend_result zend_native_executor_register_preloaded_script(
	const zend_script *script)
{
	zend_function *function;
	zend_class_entry *class_entry;
	zend_property_info *property_info;
	uint32_t hook_index;

	if (script == NULL) {
		return FAILURE;
	}
	if (!zend_native_executor_preloaded_owners_active) {
		zend_hash_init(
			&zend_native_executor_preloaded_owners,
			32, NULL, NULL, true);
		zend_native_executor_preloaded_owners_active = true;
	}
	if (!zend_native_executor_register_preloaded_op_array(
				&zend_native_executor_preloaded_owners,
				&script->main_op_array,
				script, 0)) {
		return FAILURE;
	}
	ZEND_HASH_FOREACH_PTR(&script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_executor_register_preloaded_op_array(
					&zend_native_executor_preloaded_owners,
					&function->op_array, script, 0)) {
			return FAILURE;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&script->class_table, class_entry) {
		if (class_entry == NULL
				|| class_entry->type != ZEND_USER_CLASS) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(
				&class_entry->function_table, function) {
			if (function != NULL
					&& function->type == ZEND_USER_FUNCTION
					&& !zend_native_executor_register_preloaded_op_array(
						&zend_native_executor_preloaded_owners,
						&function->op_array, script, 0)) {
				return FAILURE;
			}
		} ZEND_HASH_FOREACH_END();
		if (class_entry->num_hooked_props == 0) {
			continue;
		}
		ZEND_HASH_MAP_FOREACH_PTR(
				&class_entry->properties_info, property_info) {
			if (property_info->ce != class_entry
					|| property_info->hooks == NULL) {
				continue;
			}
			for (hook_index = 0;
					hook_index < ZEND_PROPERTY_HOOK_COUNT;
					hook_index++) {
				function = property_info->hooks[hook_index];
				if (function != NULL
						&& function->type == ZEND_USER_FUNCTION
						&& !zend_native_executor_register_preloaded_op_array(
							&zend_native_executor_preloaded_owners,
							&function->op_array, script, 0)) {
					return FAILURE;
				}
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();
	return SUCCESS;
}

zend_result zend_native_executor_register_script_owner(
	zend_op_array *op_array, const zend_script *script)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(op_array);
	zend_native_executor_dispatch *dispatch;
	zend_native_executor_dispatch_key key;

	if (bundle == NULL
			|| bundle->storage
				!= ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT) {
		return SUCCESS;
	}
	if (op_array == NULL || script == NULL
			|| op_array->opcodes == NULL
			|| !zend_native_executor_request_state.dispatch_active
			|| zend_hash_index_update_ptr(
				&zend_native_executor_request_state.owners,
				(zend_ulong) (uintptr_t) op_array->opcodes,
				(void *) script) == NULL) {
		return FAILURE;
	}
	dispatch = zend_native_executor_dispatch_load(op_array);
	if (dispatch == NULL) {
		key = zend_native_executor_dispatch_key_make(op_array);
		zend_native_executor_generation_lock();
		dispatch = zend_hash_str_find_ptr(
			&zend_native_executor_persistent_dispatches,
			(const char *) &key, sizeof(key));
		if (dispatch != NULL
				&& zend_native_executor_dispatch_epoch_load(dispatch)
					== zend_native_executor_request_state
						.observed_epoch
				&& dispatch->generation != NULL
				&& dispatch->entry_cell != NULL) {
			zend_native_executor_request_state.pending_opcodes =
				op_array->opcodes;
			zend_native_executor_request_state.pending_dispatch =
				dispatch;
		} else {
			dispatch = NULL;
		}
		zend_native_executor_generation_unlock();
	}
	if (dispatch != NULL
			&& zend_native_executor_dispatch_epoch_load(dispatch)
				== zend_native_executor_request_state.observed_epoch
			&& dispatch->generation != NULL
			&& !zend_native_executor_request_has_lease(
				dispatch->generation)) {
		zend_native_executor_generation_lock();
		if (!zend_native_executor_acquire_generation_locked(
				dispatch->generation)) {
			zend_native_executor_generation_unlock();
			return FAILURE;
		}
		zend_native_executor_generation_unlock();
	}
	if (dispatch != NULL
			&& zend_native_executor_dispatch_epoch_load(dispatch)
				== zend_native_executor_request_state.observed_epoch
			&& dispatch->generation != NULL
			&& zend_native_executor_request_state.execution_depth == 0
			&& zend_native_executor_request_state.active_compiler
				!= dispatch->generation->compiler
			&& zend_native_executor_activate_generation(
				dispatch->generation) == FAILURE) {
		return FAILURE;
	}
	return SUCCESS;
}

size_t zend_native_executor_persist_calc(
	const zend_op_array *op_array)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(op_array);
	return bundle != NULL
		? zend_native_executor_bundle_allocation_size(bundle->size) : 0;
}

size_t zend_native_executor_persist(
	zend_op_array *op_array, void *memory)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(op_array);
	size_t size;

	if (bundle == NULL || memory == NULL
			|| (size = zend_native_executor_bundle_allocation_size(
				bundle->size)) == 0) {
		return 0;
	}
	memcpy(memory, bundle, size);
	zend_native_opcache_bundle *persistent = memory;
	persistent->storage = ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT;
	op_array->reserved[zend_native_executor_bundle_rid] = persistent;
	if (bundle->storage == ZEND_NATIVE_OPCACHE_BUNDLE_HEAP) {
		efree(bundle);
	}
	return size;
}

void zend_native_executor_discard_bundle(zend_op_array *op_array)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(op_array);

	if (bundle != NULL
			&& bundle->storage == ZEND_NATIVE_OPCACHE_BUNDLE_HEAP) {
		efree(bundle);
	}
	if (op_array != NULL && zend_native_executor_bundle_rid >= 0) {
		op_array->reserved[zend_native_executor_bundle_rid] = NULL;
	}
}

void zend_native_executor_set_bundle_persistent(
	zend_op_array *op_array, bool persistent)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(op_array);

	if (bundle != NULL
			&& bundle->storage
				!= ZEND_NATIVE_OPCACHE_BUNDLE_HEAP) {
		bundle->storage = persistent
			? ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT
			: ZEND_NATIVE_OPCACHE_BUNDLE_REQUEST_ARENA;
	}
}

int zend_native_executor_op_array_handle(void)
{
	return zend_native_executor_bundle_rid;
}

void zend_native_executor_execute_ex(zend_execute_data *execute_data)
{
	zend_native_executor_dispatch *dispatch;
	zend_native_executor_generation *generation;
	zend_native_entry_cell *entry_cell;
	const zend_native_code *code;
	zend_execute_data *previous;
	zend_native_diagnostic diagnostic;
	zend_native_status status;

	if (!zend_native_executor_request_state.active
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		zend_throw_error(NULL, "Invalid native userland executor activation");
		if (execute_data != NULL) {
			EG(current_execute_data) = execute_data->prev_execute_data;
		}
		return;
	}
	previous = execute_data->prev_execute_data;
	if (zend_native_executor_request_state.execution_depth != 0) {
		entry_cell = zend_native_reentry_resolve(execute_data->func);
	} else {
		entry_cell = NULL;
	}
	if (entry_cell != NULL
			&& (code = zend_native_entry_cell_load(entry_cell)) != NULL) {
		if (entry_cell->frame_probe != NULL) {
			entry_cell->frame_probe(
				entry_cell->frame_probe_context, previous,
				execute_data);
		}
		memset(&diagnostic, 0, sizeof(diagnostic));
		zend_native_entry_cell_retain_active(entry_cell);
		EG(current_execute_data) = execute_data;
		zend_native_executor_request_state.execution_depth++;
		status = zend_native_execute_observed_frame(
			code, execute_data, &diagnostic);
		zend_native_executor_request_state.execution_depth--;
		EG(current_execute_data) = previous;
		zend_native_entry_cell_release_active(entry_cell);
		goto complete;
	}
	dispatch = zend_native_executor_dispatch_load(
		&execute_data->func->op_array);
	if (dispatch == NULL
			&& zend_native_executor_request_state.pending_opcodes
				== execute_data->func->op_array.opcodes) {
		dispatch = zend_native_executor_request_state.pending_dispatch;
		zend_native_executor_request_state.pending_opcodes = NULL;
		zend_native_executor_request_state.pending_dispatch = NULL;
		if (dispatch != NULL) {
			zend_native_executor_dispatch_store(
				&execute_data->func->op_array, dispatch);
		}
	}
	if (dispatch != NULL
			&& (!dispatch->persistent
				|| zend_native_executor_dispatch_epoch_load(dispatch)
					== zend_native_executor_request_state
						.observed_epoch)
			&& dispatch->generation != NULL
			&& dispatch->entry_cell != NULL
			&& (code = zend_native_entry_cell_load(
				dispatch->entry_cell)) != NULL) {
		if (zend_native_executor_request_state.active_compiler
					!= dispatch->generation->compiler
				&& zend_native_executor_activate_generation(
					dispatch->generation) == FAILURE) {
			memset(&diagnostic, 0, sizeof(diagnostic));
			status = ZEND_NATIVE_EXCEPTION;
		} else {
			memset(&diagnostic, 0, sizeof(diagnostic));
			zend_native_entry_cell_retain_active(
				dispatch->entry_cell);
			EG(current_execute_data) = execute_data;
			zend_native_executor_request_state.execution_depth++;
			status = zend_native_execute_observed_frame(
				code, execute_data, &diagnostic);
			zend_native_executor_request_state.execution_depth--;
			EG(current_execute_data) = previous;
			zend_native_entry_cell_release_active(
				dispatch->entry_cell);
		}
		goto complete;
	}
	if (dispatch != NULL) {
		zend_native_executor_dispatch_clear(dispatch);
		if (!dispatch->persistent) {
			zend_hash_index_del(
				&zend_native_executor_request_state.dispatch,
				(zend_ulong) (uintptr_t)
					&execute_data->func->op_array);
		}
	}
	if ((execute_data->func->common.fn_flags
			& ZEND_ACC_IMMUTABLE) != 0) {
		generation = zend_native_executor_find_leased_function(
			execute_data->func);
		if (generation == NULL) {
			generation = zend_native_executor_find_persistent_function(
				execute_data->func);
			if (generation == NULL) {
				generation =
					zend_native_executor_find_function_generation(
						zend_native_executor_request_state
							.request_generations,
						execute_data->func);
			}
		}
	} else {
		generation = zend_native_executor_find_root_generation(
			zend_native_executor_request_state.request_generations,
			&execute_data->func->op_array);
		if (generation == NULL) {
			generation = zend_native_executor_find_function_generation(
				zend_native_executor_request_state.request_generations,
				execute_data->func);
		}
		if (generation == NULL
				&& zend_native_executor_bundle(
					&execute_data->func->op_array) != NULL) {
			generation = zend_native_executor_find_leased_function(
				execute_data->func);
			if (generation == NULL) {
				generation =
					zend_native_executor_find_persistent_function(
						execute_data->func);
			}
		}
	}
	if (generation == NULL) {
		generation = zend_native_executor_create_or_acquire_generation(
			&execute_data->func->op_array);
		if (generation == NULL) {
			EG(current_execute_data) = previous;
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
	zend_native_executor_deactivate_compiler();
	zend_native_executor_request_state.execution_depth++;
	status = zend_native_compiler_execute_observed_data(
		generation->compiler, execute_data, &diagnostic);
	zend_native_executor_request_state.execution_depth--;
	entry_cell = zend_native_compiler_lookup(
		generation->compiler, execute_data->func);
	if (entry_cell != NULL) {
		zend_native_executor_bind_dispatch(
			generation, &execute_data->func->op_array, entry_cell);
	}
complete:
	if (status == ZEND_NATIVE_BAILOUT) {
		EG(current_execute_data) = previous;
		zend_bailout();
	}
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) == NULL) {
		zend_throw_error(NULL, "%s",
			diagnostic.message[0] != '\0'
				? diagnostic.message
				: "Native userland execution failed");
	}
	EG(current_execute_data) = previous;
}
