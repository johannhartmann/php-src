#include "Zend/Native/Compiler/zend_native_executor.h"

#include "Zend/Native/Compiler/zend_native_compiler_internal.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_extensions.h"
#include "Zend/zend_generators.h"
#include "Zend/zend_observer.h"
#include "Zend/Optimizer/zend_optimizer.h"

#include <stddef.h>
#include <string.h>

typedef struct _zend_native_executor_dispatch
	zend_native_executor_dispatch;

typedef struct _zend_native_executor_generation {
	zend_op_array *root;
	zend_native_op_array_identity root_identity;
	const zend_script *owner;
	zend_script script;
	zend_native_compiler *compiler;
	uint64_t epoch;
	uint64_t request_rank;
	uint32_t indexed_publication_count;
	uint32_t active_requests;
	bool persistent;
	/* Retired for its discarded script, not by an epoch: reaped once no
	 * request holds it. */
	bool owner_retired;
	bool owns_script_tables;
	/* See zend_native_executor_script_links_at_runtime(). */
	bool links_at_runtime;
	/* The ready entry of the last script this persistent generation
	 * resolved for an include (zend_native_executor_resolve_cached_include());
	 * immutable code of the generation, valid while it is leased. */
	zend_native_entry_cell *include_entry_cell;
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

typedef struct _zend_native_executor_preload_capture {
	HashTable roots;
	bool active;
} zend_native_executor_preload_capture;

typedef struct _zend_native_executor_generation_key {
	uint64_t epoch;
	uintptr_t identity;
} zend_native_executor_generation_key;

typedef struct _zend_native_executor_request {
	zend_native_executor_generation *request_generations;
	zend_native_executor_lease *leases;
	zend_native_executor_epoch_ref *epoch;
	HashTable dispatch;
	HashTable owners;
	/* Cached scripts by their interned filename, for functions and methods
	 * whose op arrays do not share the main op array's opcodes. */
	HashTable owners_by_file;
	/* Entry cells that persistent generations resolved for functions this
	 * request called; their generations stay leased until the request ends. */
	HashTable external_entries;
	HashTable request_generations_by_root;
	HashTable request_generations_by_opcodes;
	HashTable leased_generations;
	zend_native_compiler *active_compiler;
	uint64_t observed_epoch;
	uint64_t next_request_generation_rank;
	uint32_t execution_depth;
	const zend_op *pending_opcodes;
	zend_native_executor_dispatch *pending_dispatch;
	bool dispatch_active;
	bool lookup_indexes_active;
	bool active;
} zend_native_executor_request;

struct _zend_native_executor_dispatch {
	zend_native_executor_generation *generation;
	zend_native_entry_cell *entry_cell;
	zend_op_array *op_array;
	zend_native_op_array_identity op_array_identity;
	uint64_t epoch;
	bool persistent;
};

ZEND_TLS zend_native_executor_request zend_native_executor_request_state;
ZEND_TLS zend_native_executor_preload_capture
	zend_native_executor_preload_capture_state;
static bool zend_native_executor_installed;
static zend_native_frame_probe_t zend_native_executor_frame_probe;
static void *zend_native_executor_frame_probe_context;
static uint64_t zend_native_executor_epoch = 1;
static zend_native_executor_generation
	*zend_native_executor_persistent_generations;
static zend_native_executor_generation
	*zend_native_executor_retired_generations;
static zend_native_executor_epoch_ref *zend_native_executor_epochs;
static HashTable zend_native_executor_persistent_dispatches;
static bool zend_native_executor_persistent_dispatches_active;
static HashTable zend_native_executor_generations_by_owner;
static HashTable zend_native_executor_generations_by_opcodes;
static bool zend_native_executor_generation_indexes_active;
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
static void zend_native_executor_deactivate_compiler(void);
static bool zend_native_executor_recompile_tier2(
	zend_native_entry_cell *cell);

static bool zend_native_executor_capture_preload_root(
	const zend_op_array *op_array)
{
	zend_ulong key;

	if (!zend_native_executor_preload_capture_state.active) {
		return true;
	}
	if (op_array == NULL) {
		return false;
	}
	key = (zend_ulong) (uintptr_t) op_array;
	return zend_hash_index_exists(
			&zend_native_executor_preload_capture_state.roots, key)
		|| zend_hash_index_add_empty_element(
			&zend_native_executor_preload_capture_state.roots,
			key) != NULL;
}

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

static zend_native_executor_generation_key
zend_native_executor_generation_key_make(uint64_t epoch, const void *identity)
{
	zend_native_executor_generation_key key = {0};

	key.epoch = epoch;
	key.identity = (uintptr_t) identity;
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
	 * A request-local op_array can be destroyed before its generation is
	 * reclaimed. Its runtime cache is gone at that point, so the hash-table
	 * destructor must not dereference the retained op_array pointer. Callers
	 * that remove a dispatch while its op_array is known to be live detach the
	 * runtime-cache slot explicitly before deleting the hash entry.
	 */
	efree(dispatch);
}

static void zend_native_executor_persistent_dispatch_dtor(zval *value)
{
	zend_native_executor_dispatch *dispatch = Z_PTR_P(value);

	pefree(dispatch, true);
}

#define ZEND_NATIVE_OPCACHE_BUNDLE_MAGIC UINT64_C(0x313342434f4e5a)
#define ZEND_NATIVE_OPCACHE_BUNDLE_FORMAT 3u
#define ZEND_NATIVE_OPCACHE_BUNDLE_HEAP 1u
#define ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT 2u
#define ZEND_NATIVE_OPCACHE_BUNDLE_REQUEST_ARENA 3u
#define ZEND_NATIVE_OPCACHE_BUNDLE_SOURCE_PROBE (1u << 0)
#define ZEND_NATIVE_OPCACHE_BUNDLE_FRAME_PROBE (1u << 1)
/* Code of a layout training run counts in that run's shared memory. */
#define ZEND_NATIVE_OPCACHE_BUNDLE_LAYOUT_TRAINING (1u << 2)
#define ZEND_NATIVE_OPCACHE_BUNDLE_KNOWN_FLAGS \
	(ZEND_NATIVE_OPCACHE_BUNDLE_SOURCE_PROBE \
		| ZEND_NATIVE_OPCACHE_BUNDLE_FRAME_PROBE \
		| ZEND_NATIVE_OPCACHE_BUNDLE_LAYOUT_TRAINING)

typedef struct _zend_native_opcache_bundle {
	uint64_t magic;
	uint32_t format;
	uint32_t storage;
	uint32_t flags;
	/* The layout training run whose counters the code updates, or 0. */
	uint64_t training_session;
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
			&& (bundle->flags
				& ~ZEND_NATIVE_OPCACHE_BUNDLE_KNOWN_FLAGS) == 0
			&& bundle->size != 0
		? bundle : NULL;
}

bool zend_native_executor_op_array_is_cache_owned(
	const zend_op_array *op_array)
{
	zend_native_opcache_bundle *bundle =
		zend_native_executor_bundle(op_array);

	return bundle != NULL
		&& (bundle->storage == ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT
			|| bundle->storage == ZEND_NATIVE_OPCACHE_BUNDLE_REQUEST_ARENA);
}

static uint32_t zend_native_executor_bundle_flags(void)
{
	uint32_t flags = zend_native_runtime_source_probe_enabled()
		? ZEND_NATIVE_OPCACHE_BUNDLE_SOURCE_PROBE : 0;

	if (zend_native_executor_frame_probe != NULL) {
		flags |= ZEND_NATIVE_OPCACHE_BUNDLE_FRAME_PROBE;
	}
	if (zend_tpde_layout_training()) {
		flags |= ZEND_NATIVE_OPCACHE_BUNDLE_LAYOUT_TRAINING;
	}
	return flags;
}

/* A bundle another process compiled for its own layout training (through
 * the file cache) would count in memory this one does not share. */
static bool zend_native_executor_bundle_compatible(
	const zend_native_opcache_bundle *bundle)
{
	return bundle->flags == zend_native_executor_bundle_flags()
		&& bundle->training_session == zend_tpde_layout_training_session();
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
zend_native_executor_find_root_generation(zend_op_array *root)
{
	zend_native_executor_generation *generation = root != NULL
		&& zend_native_executor_request_state.lookup_indexes_active
		? zend_hash_index_find_ptr(
			&zend_native_executor_request_state
				.request_generations_by_root,
			(zend_ulong) (uintptr_t) root)
		: NULL;

	return generation != NULL
			&& zend_native_op_array_identity_matches(
				&generation->root_identity, root)
		? generation : NULL;
}

static zend_native_executor_generation *
zend_native_executor_find_function_generation(zend_function *function)
{
	zend_native_executor_generation *generation =
		function != NULL && function->op_array.opcodes != NULL
		&& zend_native_executor_request_state.lookup_indexes_active
		? zend_hash_index_find_ptr(
			&zend_native_executor_request_state
				.request_generations_by_opcodes,
			(zend_ulong) (uintptr_t) function->op_array.opcodes)
		: NULL;

	return generation != NULL
			&& zend_native_compiler_lookup(
				generation->compiler, function) != NULL
		? generation : NULL;
}

typedef struct _zend_native_executor_index_change {
	zend_ulong key;
	zend_native_executor_generation *previous;
	bool changed;
} zend_native_executor_index_change;

static bool zend_native_executor_index_ready_op_array(
	HashTable *index, zend_native_executor_generation *generation,
	zend_op_array *op_array, zend_native_executor_index_change *change)
{
	zend_native_executor_generation *indexed;
	zend_ulong key;

	if (index == NULL || generation == NULL || op_array == NULL
			|| op_array->opcodes == NULL || change == NULL) {
		return false;
	}
	key = (zend_ulong) (uintptr_t) op_array->opcodes;
	indexed = zend_hash_index_find_ptr(index, key);
	change->key = key;
	change->previous = indexed;
	change->changed = false;
	if (indexed != NULL && indexed != generation
			&& !indexed->persistent && !generation->persistent
			&& indexed->request_rank > generation->request_rank) {
		return true;
	}
	if (indexed == generation) {
		return true;
	}
	if (zend_hash_index_update_ptr(index, key, generation) == NULL) {
		return false;
	}
	change->changed = true;
	return true;
}

static void zend_native_executor_rollback_index_changes(
	HashTable *index, zend_native_executor_index_change *changes,
	uint32_t change_count)
{
	while (change_count != 0) {
		zend_native_executor_index_change *change =
			&changes[--change_count];

		if (!change->changed) {
			continue;
		}
		if (change->previous != NULL) {
			(void) zend_hash_index_update_ptr(
				index, change->key, change->previous);
		} else {
			zend_hash_index_del(index, change->key);
		}
	}
}

static bool zend_native_executor_sync_ready_functions(
	zend_native_executor_generation *generation, HashTable *index,
	uint32_t *publication_cursor)
{
	zend_op_array **op_arrays = NULL;
	zend_native_executor_index_change *changes = NULL;
	uint32_t capacity = 0;
	uint32_t required_count = 0;
	uint32_t next_index = 0;
	uint32_t op_array_index;
	zend_result result;

	if (generation == NULL || index == NULL || publication_cursor == NULL
			|| !zend_native_executor_request_state.lookup_indexes_active) {
		return false;
	}
	do {
		result = zend_native_compiler_snapshot_publication_delta(
			generation->compiler, *publication_cursor,
			op_arrays, capacity, &required_count, &next_index);
		if (result == SUCCESS) {
			break;
		}
		if (required_count <= capacity) {
			if (op_arrays != NULL) {
				efree(op_arrays);
			}
			return false;
		}
		op_arrays = safe_erealloc(
			op_arrays, required_count, sizeof(*op_arrays), 0);
		capacity = required_count;
	} while (true);
	if (required_count != 0) {
		changes = safe_emalloc(required_count, sizeof(*changes), 0);
	}
	for (op_array_index = 0; op_array_index < required_count;
			op_array_index++) {
		if (!zend_native_executor_index_ready_op_array(
				index, generation, op_arrays[op_array_index],
				&changes[op_array_index])) {
			zend_native_executor_rollback_index_changes(
				index, changes, op_array_index);
			efree(changes);
			efree(op_arrays);
			return false;
		}
	}
	if (changes != NULL) {
		efree(changes);
	}
	if (op_arrays != NULL) {
		efree(op_arrays);
	}
	*publication_cursor = next_index;
	return true;
}

static bool zend_native_executor_sync_request_generation(
	zend_native_executor_generation *generation)
{
	return generation != NULL && !generation->persistent
		&& zend_native_executor_sync_ready_functions(
			generation,
			&zend_native_executor_request_state
				.request_generations_by_opcodes,
			&generation->indexed_publication_count);
}

static bool zend_native_executor_index_request_generation(
	zend_native_executor_generation *generation)
{
	HashTable *root_index = &zend_native_executor_request_state
		.request_generations_by_root;
	zend_native_executor_generation *previous;
	zend_ulong key;

	if (generation == NULL || generation->root == NULL
			|| !zend_native_executor_request_state.lookup_indexes_active) {
		return false;
	}
	key = (zend_ulong) (uintptr_t) generation->root;
	previous = zend_hash_index_find_ptr(root_index, key);
	if (zend_hash_index_update_ptr(root_index, key, generation) == NULL) {
		return false;
	}
	if (zend_native_executor_sync_request_generation(generation)) {
		return true;
	}
	if (previous != NULL) {
		(void) zend_hash_index_update_ptr(root_index, key, previous);
	} else {
		zend_hash_index_del(root_index, key);
	}
	return false;
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

/*
 * A script whose classes are not all linked when it is loaded links them
 * while the include runs, from OPcache's inheritance cache. Its bundle was
 * compiled against the unlinked entries, so its persistent generation
 * compiles on demand and binds only linked class entries.
 */
static bool zend_native_executor_script_links_at_runtime(
	const zend_script *owner)
{
	zend_class_entry *class_entry;

	if (owner == NULL) {
		return false;
	}
	ZEND_HASH_MAP_FOREACH_PTR(&owner->class_table, class_entry) {
		if ((class_entry->ce_flags & ZEND_ACC_LINKED) == 0) {
			return true;
		}
	} ZEND_HASH_FOREACH_END();
	return false;
}

/* A class entry persistent code may bind: linked, and shared by every
 * request (immutable) unless the owner script itself holds it. */
static zend_class_entry *zend_native_executor_bindable_class(
	zend_class_entry *cached, zend_class_entry *runtime)
{
	const uint32_t shared = ZEND_ACC_LINKED | ZEND_ACC_IMMUTABLE;

	if (runtime != NULL && runtime != cached) {
		return runtime->type == ZEND_USER_CLASS
				&& (runtime->ce_flags & shared) == shared
			? runtime : NULL;
	}
	return (cached->ce_flags & ZEND_ACC_LINKED) != 0 ? cached : NULL;
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
		if (generation->links_at_runtime) {
			class_entry = zend_native_executor_bindable_class(
				class_entry, runtime_class);
			if (class_entry == NULL) {
				continue;
			}
		} else if (runtime_class != NULL
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

static bool zend_native_executor_index_op_array_locked(
	zend_native_executor_generation *generation,
	zend_op_array *op_array, uint32_t depth)
{
	zend_native_executor_generation_key key;
	zend_native_executor_generation *indexed;
	uint32_t index;

	if (generation == NULL || op_array == NULL || depth > 64) {
		return false;
	}
	if (op_array->opcodes != NULL) {
		key = zend_native_executor_generation_key_make(
			generation->epoch, op_array->opcodes);
		indexed = zend_hash_str_find_ptr(
			&zend_native_executor_generations_by_opcodes,
			(const char *) &key, sizeof(key));
		if (indexed != NULL && indexed != generation) {
			return false;
		}
		if (indexed == NULL
				&& zend_hash_str_add_ptr(
					&zend_native_executor_generations_by_opcodes,
					(const char *) &key, sizeof(key),
					generation) == NULL) {
			return false;
		}
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (!zend_native_executor_index_op_array_locked(
				generation, op_array->dynamic_func_defs[index],
				depth + 1)) {
			return false;
		}
	}
	return true;
}

static bool zend_native_executor_index_class_locked(
	zend_native_executor_generation *generation,
	zend_class_entry *class_entry)
{
	zend_function *function;
	zend_property_info *property_info;
	uint32_t hook_index;

	if (class_entry == NULL) {
		return true;
	}
	ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_executor_index_op_array_locked(
					generation, &function->op_array, 0)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	if (class_entry->num_hooked_props == 0) {
		return true;
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
					&& !zend_native_executor_index_op_array_locked(
						generation, &function->op_array, 0)) {
				return false;
			}
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static void zend_native_executor_unindex_op_array_locked(
	zend_native_executor_generation *generation,
	zend_op_array *op_array, uint32_t depth)
{
	zend_native_executor_generation_key key;
	uint32_t index;

	if (generation == NULL || op_array == NULL || depth > 64) {
		return;
	}
	if (op_array->opcodes != NULL) {
		key = zend_native_executor_generation_key_make(
			generation->epoch, op_array->opcodes);
		if (zend_hash_str_find_ptr(
				&zend_native_executor_generations_by_opcodes,
				(const char *) &key, sizeof(key)) == generation) {
			zend_hash_str_del(
				&zend_native_executor_generations_by_opcodes,
				(const char *) &key, sizeof(key));
		}
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		zend_native_executor_unindex_op_array_locked(
			generation, op_array->dynamic_func_defs[index],
			depth + 1);
	}
}

static void zend_native_executor_unindex_class_locked(
	zend_native_executor_generation *generation,
	zend_class_entry *class_entry)
{
	zend_function *function;
	zend_property_info *property_info;
	uint32_t hook_index;

	if (class_entry == NULL) {
		return;
	}
	ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION) {
			zend_native_executor_unindex_op_array_locked(
				generation, &function->op_array, 0);
		}
	} ZEND_HASH_FOREACH_END();
	if (class_entry->num_hooked_props == 0) {
		return;
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
					&& function->type == ZEND_USER_FUNCTION) {
				zend_native_executor_unindex_op_array_locked(
					generation, &function->op_array, 0);
			}
		}
	} ZEND_HASH_FOREACH_END();
}

static void zend_native_executor_unregister_generation_locked(
	zend_native_executor_generation *generation)
{
	zend_native_executor_generation_key key;
	zend_function *function;
	zend_class_entry *class_entry;

	if (generation == NULL
			|| !generation->persistent
			|| !zend_native_executor_generation_indexes_active) {
		return;
	}
	if (generation->owner != NULL) {
		key = zend_native_executor_generation_key_make(
			generation->epoch, generation->owner);
		if (zend_hash_str_find_ptr(
				&zend_native_executor_generations_by_owner,
				(const char *) &key, sizeof(key)) == generation) {
			zend_hash_str_del(
				&zend_native_executor_generations_by_owner,
				(const char *) &key, sizeof(key));
		}
	}
	zend_native_executor_unindex_op_array_locked(
		generation, &generation->script.main_op_array, 0);
	ZEND_HASH_FOREACH_PTR(
			&generation->script.function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION) {
			zend_native_executor_unindex_op_array_locked(
				generation, &function->op_array, 0);
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(
			&generation->script.class_table, class_entry) {
		zend_native_executor_unindex_class_locked(
			generation, class_entry);
	} ZEND_HASH_FOREACH_END();
}

static bool zend_native_executor_register_generation_locked(
	zend_native_executor_generation *generation)
{
	zend_native_executor_generation_key key;
	zend_native_executor_generation *indexed;
	zend_function *function;
	zend_class_entry *class_entry;

	if (generation == NULL || !generation->persistent
			|| generation->owner == NULL
			|| !zend_native_executor_generation_indexes_active) {
		return false;
	}
	key = zend_native_executor_generation_key_make(
		generation->epoch, generation->owner);
	indexed = zend_hash_str_find_ptr(
		&zend_native_executor_generations_by_owner,
		(const char *) &key, sizeof(key));
	if ((indexed != NULL && indexed != generation)
			|| (indexed == NULL
				&& zend_hash_str_add_ptr(
					&zend_native_executor_generations_by_owner,
					(const char *) &key, sizeof(key),
					generation) == NULL)
			|| !zend_native_executor_index_op_array_locked(
				generation, &generation->script.main_op_array, 0)) {
		goto failure;
	}
	ZEND_HASH_FOREACH_PTR(
			&generation->script.function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_executor_index_op_array_locked(
					generation, &function->op_array, 0)) {
			goto failure;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(
			&generation->script.class_table, class_entry) {
		if (!zend_native_executor_index_class_locked(
				generation, class_entry)) {
			goto failure;
		}
	} ZEND_HASH_FOREACH_END();
	return true;

failure:
	zend_native_executor_unregister_generation_locked(generation);
	return false;
}

static void zend_native_executor_destroy_generation(
	zend_native_executor_generation *generation)
{
	zend_native_executor_dispatch *dispatch;

	if (generation == NULL) {
		return;
	}
	zend_native_executor_unregister_generation_locked(generation);
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
			&& zend_native_op_array_identity_matches(
				&dispatch->op_array_identity, op_array)
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
		zend_native_op_array_identity_capture(
			&dispatch->op_array_identity, op_array);
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
	zend_native_op_array_identity_capture(
		&dispatch->op_array_identity, op_array);
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

static void zend_native_executor_release_completed_main_generation(
	zend_native_executor_generation *generation, zend_op_array *op_array)
{
	zend_native_executor_generation **link;
	zend_op_array **published_op_arrays = NULL;
	zend_native_executor_generation *indexed;
	zend_native_executor_dispatch *dispatch;
	uint32_t published_capacity = 0;
	uint32_t published_count = 0;
	uint32_t next_index = 0;
	uint32_t index;
	zend_result snapshot_result;

	if (generation == NULL || generation->persistent
			|| generation->root != op_array
			|| op_array == NULL || op_array->function_name != NULL
			|| !zend_native_compiler_is_quiescent(generation->compiler)) {
		return;
	}
	do {
		snapshot_result = zend_native_compiler_snapshot_publication_delta(
			generation->compiler, 0, published_op_arrays,
			published_capacity, &published_count, &next_index);
		if (snapshot_result == SUCCESS) {
			break;
		}
		if (published_count <= published_capacity) {
			if (published_op_arrays != NULL) {
				efree(published_op_arrays);
			}
			return;
		}
		published_op_arrays = safe_erealloc(
			published_op_arrays, published_count,
			sizeof(*published_op_arrays), 0);
		published_capacity = published_count;
	} while (true);
	indexed = zend_hash_index_find_ptr(
		&zend_native_executor_request_state.request_generations_by_root,
		(zend_ulong) (uintptr_t) op_array);
	if (indexed == generation) {
		zend_hash_index_del(
			&zend_native_executor_request_state.request_generations_by_root,
			(zend_ulong) (uintptr_t) op_array);
	}
	for (index = 0; index < published_count; index++) {
		zend_op_array *published = published_op_arrays[index];

		if (published == NULL || published->opcodes == NULL) {
			continue;
		}
		indexed = zend_hash_index_find_ptr(
			&zend_native_executor_request_state.request_generations_by_opcodes,
			(zend_ulong) (uintptr_t) published->opcodes);
		if (indexed == generation) {
			zend_hash_index_del(
				&zend_native_executor_request_state
					.request_generations_by_opcodes,
				(zend_ulong) (uintptr_t) published->opcodes);
		}
		dispatch = zend_hash_index_find_ptr(
			&zend_native_executor_request_state.dispatch,
			(zend_ulong) (uintptr_t) published);
		if (dispatch != NULL && dispatch->generation == generation) {
			zend_native_executor_dispatch_clear(dispatch);
			zend_hash_index_del(
				&zend_native_executor_request_state.dispatch,
				(zend_ulong) (uintptr_t) published);
		}
	}
	if (published_op_arrays != NULL) {
		efree(published_op_arrays);
	}
	link = &zend_native_executor_request_state.request_generations;
	while (*link != NULL && *link != generation) {
		link = &(*link)->next;
	}
	if (*link != generation) {
		return;
	}
	*link = generation->next;
	if (zend_native_executor_request_state.active_compiler
			== generation->compiler) {
		zend_native_executor_deactivate_compiler();
	}
	zend_native_executor_destroy_generation(generation);
}

static bool zend_native_executor_request_has_lease(
	const zend_native_executor_generation *generation)
{
	return generation != NULL
		&& zend_native_executor_request_state.lookup_indexes_active
		&& zend_hash_index_exists(
			&zend_native_executor_request_state.leased_generations,
			(zend_ulong) (uintptr_t) generation);
}

static zend_native_executor_generation *
zend_native_executor_find_leased_function(zend_function *function)
{
	zend_native_executor_generation_key key;
	zend_native_executor_generation *generation;

	if (function == NULL || function->op_array.opcodes == NULL
			|| !zend_native_executor_request_state.lookup_indexes_active) {
		return NULL;
	}
	key = zend_native_executor_generation_key_make(
		zend_native_executor_request_state.observed_epoch,
		function->op_array.opcodes);
	zend_native_executor_generation_lock();
	generation = zend_hash_str_find_ptr(
		&zend_native_executor_generations_by_opcodes,
		(const char *) &key, sizeof(key));
	zend_native_executor_generation_unlock();
	return zend_native_executor_request_has_lease(generation)
		? generation : NULL;
}

static bool zend_native_executor_acquire_generation_locked(
	zend_native_executor_generation *generation)
{
	zend_native_executor_lease *lease;

	if (generation == NULL) {
		return false;
	}
	/*
	 * Leases keep shared generations alive across requests. A request
	 * generation is owned by the request list, which destroys it before the
	 * leases are released, so it must not be leased.
	 */
	if (!generation->persistent
			|| zend_native_executor_request_has_lease(generation)) {
		return true;
	}
	lease = emalloc(sizeof(*lease));
	lease->generation = generation;
	if (!zend_native_executor_request_state.lookup_indexes_active
			|| zend_hash_index_add_ptr(
				&zend_native_executor_request_state.leased_generations,
				(zend_ulong) (uintptr_t) generation,
				lease) == NULL) {
		efree(lease);
		return false;
	}
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
				|| (!generation->owner_retired
					&& zend_native_executor_epoch_is_active_locked(
						generation->epoch))
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

void zend_native_executor_retire_owners(
	bool (*discarded)(const zend_script *owner))
{
	zend_native_executor_generation **link =
		&zend_native_executor_persistent_generations;

	if (discarded == NULL) {
		return;
	}
	zend_native_executor_generation_lock();
	while (*link != NULL) {
		zend_native_executor_generation *generation = *link;

		if (generation->owner == NULL || !discarded(generation->owner)) {
			link = &generation->next;
			continue;
		}
		*link = generation->next;
		/* No lookup finds it again; the replacement script has its own
		 * opcodes and owner. */
		zend_native_executor_unregister_generation_locked(generation);
		if (generation->active_requests == 0
				&& zend_native_compiler_is_quiescent(
					generation->compiler)) {
			zend_native_executor_destroy_generation(generation);
		} else {
			generation->owner_retired = true;
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
	if (zend_native_executor_request_state.lookup_indexes_active) {
		zend_hash_clean(
			&zend_native_executor_request_state.leased_generations);
	}
	if (lease == NULL) {
		return;
	}
	for (zend_native_executor_lease *current = lease; current != NULL;
			current = current->next) {
		zend_native_compiler_end_request(current->generation->compiler);
	}
	/* Retired generations are reaped once all leases are dropped. */
	zend_native_executor_generation_lock();
	for (zend_native_executor_lease *current = lease; current != NULL;
			current = current->next) {
		ZEND_ASSERT(current->generation->active_requests != 0);
		current->generation->active_requests--;
	}
	zend_native_executor_reap_retired_locked();
	zend_native_executor_generation_unlock();
	while (lease != NULL) {
		zend_native_executor_lease *next = lease->next;

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
	const zend_script *owner =
		zend_native_executor_script_owner(root);
	zend_native_opcache_bundle *bundle = owner != NULL
		? zend_native_executor_bundle(&owner->main_op_array)
		: zend_native_executor_bundle(root);
	bool persistent =
		owner != NULL
		|| (bundle != NULL
			&& bundle->storage
				== ZEND_NATIVE_OPCACHE_BUNDLE_PERSISTENT);

	generation = pecalloc(1, sizeof(*generation), persistent);
	generation->root = persistent ? NULL : root;
	if (!persistent) {
		zend_native_op_array_identity_capture(
			&generation->root_identity, root);
	}
	generation->owner = owner;
	generation->persistent = persistent;
	generation->links_at_runtime = persistent
		&& zend_native_executor_script_links_at_runtime(owner);
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
	config.frame_probe = zend_native_executor_frame_probe;
	config.frame_probe_context =
		zend_native_executor_frame_probe_context;
	config.source_probe = zend_native_runtime_source_probe_enabled();
	/* A request generation borrows the global symbol tables; indexing all of
	 * them for every root made each include cost O(all declarations). */
	config.lazy_source_index = !persistent && bundle == NULL;
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
	if (bundle != NULL && !generation->links_at_runtime
			&& zend_native_executor_bundle_compatible(bundle)
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
		if (zend_native_executor_request_state
				.next_request_generation_rank == UINT64_MAX) {
			zend_native_executor_destroy_generation(generation);
			zend_throw_error(NULL,
				"Native request generation rank exhausted");
			return NULL;
		}
		generation->request_rank = ++zend_native_executor_request_state
			.next_request_generation_rank;
		if (!zend_native_executor_index_request_generation(generation)) {
			zend_native_executor_destroy_generation(generation);
			if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native request generation index allocation failed");
			}
			return NULL;
		}
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
	zend_native_executor_generation_key key;
	zend_native_executor_generation *generation;
	bool acquired = false;

	if (function == NULL || function->op_array.opcodes == NULL) {
		return NULL;
	}
	key = zend_native_executor_generation_key_make(
		zend_native_executor_request_state.observed_epoch,
		function->op_array.opcodes);
	zend_native_executor_generation_lock();
	generation = zend_hash_str_find_ptr(
		&zend_native_executor_generations_by_opcodes,
		(const char *) &key, sizeof(key));
	if (generation != NULL) {
		acquired = zend_native_executor_acquire_generation_locked(generation);
	}
	zend_native_executor_generation_unlock();
	if (generation != NULL && !acquired) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Native generation lease allocation failed");
		}
		return NULL;
	}
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
	bool acquired = false;

	if (!persistent) {
		return zend_native_executor_create_generation(root);
	}
	if (owner == NULL) {
		zend_throw_error(NULL,
			"Persistent native script owner is unavailable");
		return NULL;
	}
	zend_native_executor_generation_lock();
	{
		zend_native_executor_generation_key key =
			zend_native_executor_generation_key_make(
				zend_native_executor_request_state.observed_epoch,
				owner);

		generation = zend_hash_str_find_ptr(
			&zend_native_executor_generations_by_owner,
			(const char *) &key, sizeof(key));
	}
	if (generation == NULL) {
		generation = zend_native_executor_create_generation(root);
		if (generation != NULL) {
			if (!zend_native_executor_register_generation_locked(
					generation)) {
				zend_native_executor_destroy_generation(generation);
				generation = NULL;
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native generation registry allocation failed");
				}
			} else if (generation->epoch == __atomic_load_n(
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
		acquired = zend_native_executor_acquire_generation_locked(generation);
	}
	zend_native_executor_generation_unlock();
	if (generation != NULL && !acquired) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Native generation lease allocation failed");
		}
		return NULL;
	}
	return generation;
}

/*
 * The persistent generation of the cached script that declares an immutable
 * function or method, found by the script's filename. A method of a class
 * the include linked at runtime joins the generation once its immutable,
 * inheritance-cached class entry exists, so every later request reuses the
 * compiled code. NULL keeps the request-local compilation.
 */
static zend_native_executor_generation *
zend_native_executor_find_owner_generation(zend_function *function)
{
	const uint32_t shared = ZEND_ACC_LINKED | ZEND_ACC_IMMUTABLE;
	zend_native_executor_generation *generation;
	zend_class_entry *scope = function->common.scope;
	const zend_script *owner;
	HashTable *table;
	zend_string *key;
	void *indexed;
	bool usable;

	if (!ZEND_USER_CODE(function->type)
			|| (function->common.fn_flags & ZEND_ACC_IMMUTABLE) == 0
			|| (function->common.fn_flags & ZEND_ACC_CLOSURE) != 0
			|| function->common.function_name == NULL
			|| function->op_array.filename == NULL
			|| !zend_native_executor_request_state.dispatch_active
			|| (scope != NULL && (scope->type != ZEND_USER_CLASS
				|| (scope->ce_flags & shared) != shared))
			|| (owner = zend_hash_index_find_ptr(
				&zend_native_executor_request_state.owners_by_file,
				(zend_ulong) (uintptr_t) function->op_array.filename))
				== NULL) {
		return NULL;
	}
	generation = zend_native_executor_create_or_acquire_generation(
		(zend_op_array *) &owner->main_op_array);
	if (generation == NULL || !generation->persistent
			|| generation->owner != owner) {
		if (EG(exception) != NULL) {
			zend_clear_exception();
		}
		return NULL;
	}
	table = scope != NULL
		? &generation->script.class_table
		: &generation->script.function_table;
	key = zend_string_tolower(
		scope != NULL ? scope->name : function->common.function_name);
	zend_native_executor_generation_lock();
	indexed = zend_hash_find_ptr(table, key);
	usable = scope != NULL ? indexed == scope : indexed == function;
	if (indexed == NULL && scope != NULL && generation->links_at_runtime) {
		zend_string *persistent_key = zend_string_init(
			ZSTR_VAL(key), ZSTR_LEN(key), true);

		usable = zend_hash_add_ptr(table, persistent_key, scope) != NULL
			&& zend_native_executor_index_class_locked(generation, scope);
		zend_string_release_ex(persistent_key, true);
	}
	zend_native_executor_generation_unlock();
	zend_string_release(key);
	return usable ? generation : NULL;
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
	if (zend_native_executor_request_state.dispatch_active) {
		entry_cell = zend_hash_index_find_ptr(
			&zend_native_executor_request_state.external_entries,
			(zend_ulong) (uintptr_t) function);
		/*
		 * The cell names the compiler's source op_array; a closure or an
		 * inherited copy shares its opcodes (the compiler's identity, see
		 * zend_native_compiler_canonical_reentry_op_array()) and resolves
		 * to the same cell. A recycled address with other code misses.
		 */
		if (entry_cell != NULL
				&& entry_cell->state == ZEND_NATIVE_ENTRY_READY
				&& (entry_cell->function == function
					|| (entry_cell->function->op_array.opcodes
							== function->op_array.opcodes
						&& entry_cell->function->op_array.last
							== function->op_array.last))) {
			return entry_cell;
		}
	}
	if (UNEXPECTED(!zend_native_executor_capture_preload_root(
			&function->op_array))) {
		zend_throw_error(NULL,
			"Native preload root capture failed");
		return NULL;
	}
	generation = zend_native_executor_find_leased_function(function);
	if (generation == NULL) {
		generation = zend_native_executor_find_persistent_function(function);
	}
	if (generation == NULL) {
		/*
		 * A request generation that already published this function, or
		 * that this function roots, serves every later reentry; creating a
		 * generation per call compiled a hot function thousands of times.
		 */
		generation = zend_native_executor_find_owner_generation(function);
	}
	if (generation == NULL) {
		generation = zend_native_executor_find_function_generation(function);
		if (generation == NULL) {
			generation = zend_native_executor_find_root_generation(
				&function->op_array);
		}
		if (generation != NULL) {
			/* Include and autoload may have grown the borrowed tables. */
			generation->script.function_table = *EG(function_table);
			generation->script.class_table = *EG(class_table);
		}
	}
	if (generation == NULL) {
		generation = zend_native_executor_create_or_acquire_generation(
			&function->op_array);
	}
	if (generation == NULL) {
		return NULL;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	{
		const char *reason = zend_native_compile_trace_reason;
		uint32_t first_compiled_function =
			zend_native_compiler_function_count(generation->compiler);

		zend_native_compile_trace_reason = "external-reentry";
		entry_cell = zend_native_compiler_prepare_function(
			generation->compiler, function, &diagnostic);
		zend_native_compile_trace_reason = reason;
		if (entry_cell != NULL) {
			/* Like lazy reentry: the published code and its cell stay,
			 * while SSA, MIR and image data can no longer be observed. */
			zend_native_compiler_release_ready_transients(
				generation->compiler, first_compiled_function);
		}
	}
	if (entry_cell == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native external codeunit compilation failed");
		}
		return NULL;
	}
	if (!generation->persistent
			&& !zend_native_executor_sync_request_generation(generation)) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Native generation function index allocation failed");
		}
		return NULL;
	}
	if (generation->persistent
			&& zend_native_executor_request_state.dispatch_active) {
		(void) zend_hash_index_update_ptr(
			&zend_native_executor_request_state.external_entries,
			(zend_ulong) (uintptr_t) function, entry_cell);
	}
	return entry_cell;
}

zend_native_entry_cell *zend_native_executor_resolve_cached_include(
	zend_op_array *op_array)
{
	zend_native_executor_generation *generation;
	zend_native_compile_diagnostic diagnostic;
	zend_native_entry_cell *entry_cell;
	uint32_t first_compiled_function;
	const zend_script *owner;

	if (op_array == NULL || !zend_native_executor_request_state.active
			|| !zend_native_executor_op_array_is_cache_owned(op_array)
			|| (owner = zend_native_executor_script_owner(op_array)) == NULL) {
		return NULL;
	}
	/* The persistent lookup also finds a generation this request leased. */
	generation = zend_native_executor_find_persistent_function(
		(zend_function *) op_array);
	if (generation == NULL) {
		generation = zend_native_executor_create_or_acquire_generation(
			op_array);
	}
	if (generation == NULL || !generation->persistent) {
		return NULL;
	}
	entry_cell = generation->include_entry_cell;
	if (entry_cell != NULL
			&& entry_cell->state == ZEND_NATIVE_ENTRY_READY
			&& entry_cell->function->op_array.opcodes == op_array->opcodes
			&& entry_cell->function->op_array.last == op_array->last) {
		return entry_cell;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	first_compiled_function =
		zend_native_compiler_function_count(generation->compiler);
	entry_cell = zend_native_compiler_prepare_function(
		generation->compiler, (zend_function *) op_array, &diagnostic);
	if (entry_cell == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native include codeunit compilation failed");
		}
		return NULL;
	}
	zend_native_compiler_release_ready_transients(
		generation->compiler, first_compiled_function);
	zend_native_executor_generation_lock();
	generation->include_entry_cell = entry_cell;
	zend_native_executor_generation_unlock();
	return entry_cell;
}

/* Set by a server SAPI before its module startup. */
static bool zend_native_executor_automatic_layout = false;

zend_result zend_native_executor_startup(void)
{
	if (zend_native_executor_installed
			|| zend_native_runtime_validate() == FAILURE) {
		return FAILURE;
	}
	zend_native_compile_trace_startup();
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
	zend_hash_init(
		&zend_native_executor_generations_by_owner,
		32, NULL, NULL, true);
	zend_hash_init(
		&zend_native_executor_generations_by_opcodes,
		128, NULL, NULL, true);
	zend_native_executor_generation_indexes_active = true;
	zend_execute_ex = zend_native_executor_execute_ex;
	zend_native_executor_installed = true;
	if (zend_native_executor_automatic_layout) {
		zend_tpde_layout_automatic();
	}
	zend_native_tier2_recompile_hook = zend_native_executor_recompile_tier2;
	return SUCCESS;
}

void zend_native_executor_shutdown(void)
{
	zend_native_executor_end_preload_capture();
	zend_native_executor_deactivate();
	zend_native_executor_destroy_generations(
		&zend_native_executor_persistent_generations);
	zend_native_executor_destroy_generations(
		&zend_native_executor_retired_generations);
	if (zend_native_executor_generation_indexes_active) {
		zend_hash_destroy(&zend_native_executor_generations_by_opcodes);
		zend_hash_destroy(&zend_native_executor_generations_by_owner);
		zend_native_executor_generation_indexes_active = false;
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
	ZEND_ASSERT(!zend_native_executor_request_state.lookup_indexes_active);
	ZEND_ASSERT(zend_native_executor_request_state.epoch == NULL);
	(void) zend_tpde_layout_switch();
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
	zend_hash_init(
		&zend_native_executor_request_state.owners_by_file, 8, NULL, NULL,
		false);
	zend_hash_init(
		&zend_native_executor_request_state.external_entries, 64, NULL, NULL,
		false);
	zend_hash_init(
		&zend_native_executor_request_state.request_generations_by_root,
		8, NULL, NULL, false);
	zend_hash_init(
		&zend_native_executor_request_state.request_generations_by_opcodes,
		32, NULL, NULL, false);
	zend_hash_init(
		&zend_native_executor_request_state.leased_generations,
		8, NULL, NULL, false);
	zend_native_executor_request_state.pending_opcodes = NULL;
	zend_native_executor_request_state.pending_dispatch = NULL;
	zend_native_executor_request_state.next_request_generation_rank = 0;
	zend_native_executor_request_state.dispatch_active = true;
	zend_native_executor_request_state.lookup_indexes_active = true;
	zend_native_executor_request_state.active = true;
}

static int zend_native_executor_remove_generation_dispatch(
	zval *value, void *argument)
{
	zend_native_executor_dispatch *dispatch = Z_PTR_P(value);
	zend_native_executor_generation *generation = argument;

	return dispatch != NULL && dispatch->generation == generation
		? ZEND_HASH_APPLY_REMOVE : ZEND_HASH_APPLY_KEEP;
}

void zend_native_executor_prepare_shutdown(void)
{
	zend_native_executor_generation **link;

	if (!zend_native_executor_request_state.active) {
		return;
	}
	zend_native_executor_deactivate_compiler();
	link = &zend_native_executor_request_state.request_generations;
	while (*link != NULL) {
		zend_native_executor_generation *generation = *link;

		if (zend_native_executor_request_has_lease(generation)
				|| !zend_native_compiler_is_quiescent(
					generation->compiler)) {
			link = &generation->next;
			continue;
		}
		*link = generation->next;
		if (zend_native_executor_request_state.dispatch_active) {
			zend_hash_apply_with_argument(
				&zend_native_executor_request_state.dispatch,
				zend_native_executor_remove_generation_dispatch,
				generation);
		}
		zend_native_executor_destroy_generation(generation);
	}
}

/*
 * Tier 2 (ADR 0025 section 4): recompiles a function whose countdown ran
 * out in the persistent generation that owns its entry cell.
 * ZEND_NATIVE_TIER2_TRACE lists the outcomes on stderr.
 */
static bool zend_native_executor_recompile_tier2(zend_native_entry_cell *cell)
{
	static int trace = -1;
	const zend_function *function = cell->function;
	zend_native_tier2_result result = ZEND_NATIVE_TIER2_NOT_OWNED;
	zend_native_compile_diagnostic diagnostic;

	memset(&diagnostic, 0, sizeof(diagnostic));
	if (trace < 0) {
		trace = getenv("ZEND_NATIVE_TIER2_TRACE") != NULL;
	}
	/* Persistent generations first, then the request's own. */
	for (zend_native_executor_generation *generation =
			zend_native_executor_persistent_generations;
			generation != NULL && result == ZEND_NATIVE_TIER2_NOT_OWNED;
			generation = generation->next) {
		if (generation->compiler != NULL) {
			result = zend_native_compiler_recompile_tier2(
				generation->compiler, cell, &diagnostic);
		}
	}
	for (zend_native_executor_generation *generation =
			zend_native_executor_request_state.request_generations;
			generation != NULL && result == ZEND_NATIVE_TIER2_NOT_OWNED;
			generation = generation->next) {
		if (generation->compiler != NULL) {
			result = zend_native_compiler_recompile_tier2(
				generation->compiler, cell, &diagnostic);
		}
	}
	cell->tier2_state = result == ZEND_NATIVE_TIER2_RECOMPILED
		? ZEND_NATIVE_TIER2_COMPILED : ZEND_NATIVE_TIER2_FAILED;
	if (trace && function != NULL) {
		fprintf(stderr, "tier2 %s %s%s%s%s%s\n",
			result == ZEND_NATIVE_TIER2_RECOMPILED ? "recompiled"
				: result == ZEND_NATIVE_TIER2_REJECTED ? "rejected"
				: "not-owned",
			function->common.scope
				? ZSTR_VAL(function->common.scope->name) : "",
			function->common.scope ? "::" : "",
			function->common.function_name
				? ZSTR_VAL(function->common.function_name) : "?",
			diagnostic.message[0] != '\0' ? ": " : "",
			diagnostic.message);
	}
	return true;
}

/* Recompiles the functions a countdown queued without recompiling. */
static void zend_native_executor_drain_tier2(void)
{
	zend_native_entry_cell *cells[64];
	uint32_t count;

	while ((count = zend_native_tier2_take_queue(cells, 64)) != 0) {
		for (uint32_t index = 0; index < count; index++) {
			cells[index]->tier2_state = ZEND_NATIVE_TIER2_NONE;
			(void) zend_native_executor_recompile_tier2(cells[index]);
		}
	}
}

void zend_native_executor_deactivate(void)
{
	zend_native_executor_drain_tier2();
	zend_tpde_layout_training_flush();
	zend_native_compile_trace_request_end("request_shutdown");
	zend_native_executor_request_state.active = false;
	zend_native_executor_request_state.pending_opcodes = NULL;
	zend_native_executor_request_state.pending_dispatch = NULL;
	zend_native_executor_deactivate_compiler();
	if (zend_native_executor_request_state.dispatch_active) {
		zend_hash_destroy(
			&zend_native_executor_request_state.owners);
		zend_hash_destroy(
			&zend_native_executor_request_state.owners_by_file);
		zend_hash_destroy(
			&zend_native_executor_request_state.external_entries);
		zend_hash_destroy(
			&zend_native_executor_request_state.dispatch);
		zend_native_executor_request_state.dispatch_active = false;
	}
	if (zend_native_executor_request_state.lookup_indexes_active) {
		zend_hash_destroy(
			&zend_native_executor_request_state
				.request_generations_by_opcodes);
		zend_hash_destroy(
			&zend_native_executor_request_state
				.request_generations_by_root);
	}
	zend_native_executor_destroy_generations(
		&zend_native_executor_request_state.request_generations);
	zend_native_executor_release_request_leases();
	if (zend_native_executor_request_state.lookup_indexes_active) {
		zend_hash_destroy(
			&zend_native_executor_request_state.leased_generations);
		zend_native_executor_request_state.lookup_indexes_active = false;
	}
	zend_native_executor_release_request_epoch();
	zend_native_call_resolution_cache_destroy();
	zend_native_compile_trace_request_end("request_end");
}

bool zend_native_executor_take_layout_restart(void)
{
	return zend_tpde_layout_take_restart();
}

void zend_native_executor_enable_automatic_layout(void)
{
	zend_native_executor_automatic_layout = true;
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
	bool was_enabled = zend_native_runtime_source_probe_enabled();

	zend_native_runtime_set_source_probe(probe, context);
	if (zend_native_executor_installed
			&& was_enabled != (probe != NULL)) {
		zend_native_executor_invalidate();
	}
}

void zend_native_executor_set_frame_probe(
	zend_native_frame_probe_t probe, void *context)
{
	bool changed = zend_native_executor_frame_probe != probe
		|| zend_native_executor_frame_probe_context != context;

	zend_native_executor_frame_probe = probe;
	zend_native_executor_frame_probe_context = context;
	if (zend_native_executor_installed && changed) {
		zend_native_executor_invalidate();
	}
}

void zend_native_executor_begin_preload_capture(void)
{
	ZEND_ASSERT(zend_native_executor_installed);
	ZEND_ASSERT(!zend_native_executor_preload_capture_state.active);
	zend_hash_init(
		&zend_native_executor_preload_capture_state.roots,
		32, NULL, NULL, false);
	zend_native_executor_preload_capture_state.active = true;
}

void zend_native_executor_end_preload_capture(void)
{
	if (!zend_native_executor_preload_capture_state.active) {
		return;
	}
	zend_native_executor_preload_capture_state.active = false;
	zend_hash_destroy(
		&zend_native_executor_preload_capture_state.roots);
}

static bool zend_native_executor_compile_captured_op_array(
	zend_native_compiler *compiler, zend_op_array *op_array,
	zend_native_compile_diagnostic *diagnostic, uint32_t depth,
	uint32_t *selected_count)
{
	uint32_t index;
	bool selected;

	if (compiler == NULL || op_array == NULL || selected_count == NULL
			|| depth > 64) {
		return false;
	}
	selected = zend_native_executor_preload_capture_state.active
		&& zend_hash_index_exists(
				&zend_native_executor_preload_capture_state.roots,
				(zend_ulong) (uintptr_t) op_array);
	if (selected) {
		if (*selected_count == UINT32_MAX
				|| zend_native_compiler_compile(
				compiler, op_array, NULL, 0,
				diagnostic) == FAILURE) {
			return false;
		}
		(*selected_count)++;
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (!zend_native_executor_compile_captured_op_array(
				compiler, op_array->dynamic_func_defs[index],
				diagnostic, depth + 1, selected_count)) {
			return false;
		}
	}
	return true;
}

static bool zend_native_executor_compile_captured_class(
	zend_native_compiler *compiler, zend_class_entry *class_entry,
	zend_native_compile_diagnostic *diagnostic,
	uint32_t *selected_count)
{
	zend_function *function;
	zend_property_info *property_info;
	uint32_t hook_index;

	if (compiler == NULL || class_entry == NULL
			|| selected_count == NULL) {
		return false;
	}
	ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_executor_compile_captured_op_array(
					compiler, &function->op_array,
					diagnostic, 0, selected_count)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	if (class_entry->num_hooked_props == 0) {
		return true;
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
					&& !zend_native_executor_compile_captured_op_array(
						compiler, &function->op_array,
						diagnostic, 0, selected_count)) {
				return false;
			}
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static bool zend_native_executor_compile_captured_script(
	zend_native_compiler *compiler, zend_script *script,
	zend_native_compile_diagnostic *diagnostic,
	uint32_t *selected_count)
{
	zend_function *function;
	zend_class_entry *class_entry;

	if (selected_count == NULL) {
		return false;
	}
	if (!zend_native_executor_preload_capture_state.active) {
		return true;
	}
	if (!zend_native_executor_compile_captured_op_array(
			compiler, &script->main_op_array, diagnostic, 0,
			selected_count)) {
		return false;
	}
	ZEND_HASH_FOREACH_PTR(&script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_executor_compile_captured_op_array(
					compiler, &function->op_array,
					diagnostic, 0, selected_count)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&script->class_table, class_entry) {
		if (class_entry != NULL
				&& class_entry->type == ZEND_USER_CLASS
				&& !zend_native_executor_compile_captured_class(
					compiler, class_entry, diagnostic,
					selected_count)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

/*
 * Content-addressed bundles. An application may rewrite a script with the
 * same content in every request (a cache file such as Symfony's
 * PhpFilesAdapter writes for Doctrine's query cache); OPcache compiles each
 * new version again, and so did the native compiler. A script that declares
 * no class and no named function is keyed by a hash of its op_arrays (main
 * code and closures: opcodes, literals, variables, parameters, live ranges,
 * try/catch regions); its bundle, which then holds the closures too, is kept
 * per process and attached to a later script with the same key instead of
 * compiling it again. Imports bind by source ordinal and validate every
 * reference, as for a bundle the file cache loads.
 */
#define ZEND_NATIVE_CONTENT_BUNDLES_MAX 128
#define ZEND_NATIVE_CONTENT_BUNDLES_BYTES (16u * 1024u * 1024u)

typedef struct _zend_native_content_key {
	uint64_t a;
	uint64_t b;
} zend_native_content_key;

typedef struct _zend_native_content_bundle {
	zend_native_content_key key;
	uint32_t flags;
	uint64_t training_session;
	size_t size;
	unsigned char *bytes;
} zend_native_content_bundle;

static zend_native_content_bundle
	zend_native_content_bundles[ZEND_NATIVE_CONTENT_BUNDLES_MAX];
static uint32_t zend_native_content_bundle_count;
static size_t zend_native_content_bundle_bytes;

static zend_always_inline void zend_native_content_mix(
	zend_native_content_key *key, uint64_t value)
{
	key->a = (key->a ^ value) * UINT64_C(0x100000001b3);
	key->a ^= key->a >> 29;
	key->b = (key->b + value) * UINT64_C(0x9e3779b97f4a7c15);
	key->b ^= key->b >> 31;
}

static void zend_native_content_mix_bytes(
	zend_native_content_key *key, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	uint64_t word;

	zend_native_content_mix(key, length);
	while (length >= 8) {
		memcpy(&word, bytes, 8);
		zend_native_content_mix(key, word);
		bytes += 8;
		length -= 8;
	}
	word = 0;
	memcpy(&word, bytes, length);
	zend_native_content_mix(key, word);
}

static void zend_native_content_mix_string(
	zend_native_content_key *key, const zend_string *string)
{
	if (string == NULL) {
		zend_native_content_mix(key, UINT64_C(0xfeedfacecafebeef));
		return;
	}
	zend_native_content_mix_bytes(key, ZSTR_VAL(string), ZSTR_LEN(string));
}

static bool zend_native_content_mix_zval(
	zend_native_content_key *key, const zval *value, uint32_t depth)
{
	zend_native_content_mix(key, Z_TYPE_P(value));
	switch (Z_TYPE_P(value)) {
		case IS_UNDEF:
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
			return true;
		case IS_LONG:
			zend_native_content_mix(key, (uint64_t) Z_LVAL_P(value));
			return true;
		case IS_DOUBLE: {
			uint64_t bits;

			memcpy(&bits, &Z_DVAL_P(value), sizeof(bits));
			zend_native_content_mix(key, bits);
			return true;
		}
		case IS_STRING:
			zend_native_content_mix_string(key, Z_STR_P(value));
			return true;
		case IS_ARRAY: {
			const HashTable *table = Z_ARRVAL_P(value);
			zend_ulong index;
			zend_string *name;
			zval *element;

			if (depth > 32) {
				return false;
			}
			zend_native_content_mix(key, zend_hash_num_elements(table));
			ZEND_HASH_FOREACH_KEY_VAL(table, index, name, element) {
				if (name != NULL) {
					zend_native_content_mix_string(key, name);
				} else {
					zend_native_content_mix(key, index);
				}
				if (!zend_native_content_mix_zval(key, element, depth + 1)) {
					return false;
				}
			} ZEND_HASH_FOREACH_END();
			return true;
		}
		default:
			/* Constant expressions and anything else keep compiling. */
			return false;
	}
}

static bool zend_native_content_mix_type(
	zend_native_content_key *key, const zend_type *type)
{
	if (ZEND_TYPE_HAS_LIST(*type)) {
		return false;
	}
	zend_native_content_mix(key, ZEND_TYPE_FULL_MASK(*type));
	if (ZEND_TYPE_HAS_NAME(*type)) {
		zend_native_content_mix_string(key, ZEND_TYPE_NAME(*type));
	}
	return true;
}

static bool zend_native_content_mix_op_array(
	zend_native_content_key *key, const zend_op_array *op_array,
	uint32_t depth)
{
	uint32_t index;
	uint32_t arg_count;

	if (op_array == NULL || depth > 16 || op_array->scope != NULL
			|| op_array->attributes != NULL) {
		return false;
	}
	zend_native_content_mix(key, op_array->fn_flags);
	zend_native_content_mix(key, ((uint64_t) op_array->num_args << 32)
		| op_array->required_num_args);
	zend_native_content_mix(key, ((uint64_t) op_array->last << 32)
		| (uint32_t) op_array->T);
	zend_native_content_mix(key, ((uint64_t) (uint32_t) op_array->last_var
		<< 32) | (uint32_t) op_array->last_literal);
	zend_native_content_mix(key, ((uint64_t) (uint32_t) op_array->cache_size
		<< 32) | op_array->num_dynamic_func_defs);
	zend_native_content_mix(key, ((uint64_t) op_array->last_live_range << 32)
		| (uint32_t) op_array->last_try_catch);
	zend_native_content_mix(key, ((uint64_t) op_array->line_start << 32)
		| op_array->line_end);
	zend_native_content_mix_string(key, op_array->function_name);
	zend_native_content_mix_string(key, op_array->filename);
	for (index = 0; index < op_array->last; index++) {
		const zend_op *op = &op_array->opcodes[index];

		zend_native_content_mix(key, (uint64_t) op->opcode
			| ((uint64_t) op->op1_type << 8)
			| ((uint64_t) op->op2_type << 16)
			| ((uint64_t) op->result_type << 24)
			| ((uint64_t) op->extended_value << 32));
		zend_native_content_mix(key, ((uint64_t) op->op1.num << 32)
			| op->op2.num);
		zend_native_content_mix(key, ((uint64_t) op->result.num << 32)
			| op->lineno);
	}
	for (index = 0; index < (uint32_t) op_array->last_literal; index++) {
		if (!zend_native_content_mix_zval(
				key, &op_array->literals[index], 0)) {
			return false;
		}
	}
	for (index = 0; index < (uint32_t) op_array->last_var; index++) {
		zend_native_content_mix_string(key, op_array->vars[index]);
	}
	arg_count = op_array->num_args
		+ ((op_array->fn_flags & ZEND_ACC_VARIADIC) != 0 ? 1 : 0);
	if (op_array->arg_info != NULL) {
		const zend_arg_info *info = op_array->arg_info;
		uint32_t first = 0;

		if ((op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) != 0) {
			info--;
			arg_count++;
		}
		for (index = first; index < arg_count; index++) {
			zend_native_content_mix_string(key, info[index].name);
			if (!zend_native_content_mix_type(key, &info[index].type)) {
				return false;
			}
		}
	} else if (arg_count != 0) {
		return false;
	}
	for (index = 0; index < op_array->last_live_range; index++) {
		const zend_live_range *range = &op_array->live_range[index];

		zend_native_content_mix(key, range->var);
		zend_native_content_mix(key, ((uint64_t) range->start << 32)
			| range->end);
	}
	for (index = 0; index < (uint32_t) op_array->last_try_catch; index++) {
		const zend_try_catch_element *element =
			&op_array->try_catch_array[index];

		zend_native_content_mix(key, ((uint64_t) element->try_op << 32)
			| element->catch_op);
		zend_native_content_mix(key, ((uint64_t) element->finally_op << 32)
			| element->finally_end);
	}
	if (op_array->static_variables != NULL) {
		zval variables;

		ZVAL_ARR(&variables, op_array->static_variables);
		if (!zend_native_content_mix_zval(key, &variables, 0)) {
			return false;
		}
	} else {
		zend_native_content_mix(key, 0);
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (!zend_native_content_mix_op_array(
				key, op_array->dynamic_func_defs[index], depth + 1)) {
			return false;
		}
	}
	return true;
}

static bool zend_native_content_key_of(
	const zend_script *script, zend_native_content_key *key)
{
	key->a = UINT64_C(0xcbf29ce484222325);
	key->b = UINT64_C(0x6a09e667f3bcc909);
	if (zend_hash_num_elements(&script->function_table) != 0
			|| zend_hash_num_elements(&script->class_table) != 0
			|| zend_native_executor_preload_capture_state.active) {
		return false;
	}
	zend_native_content_mix(key, ZEND_NATIVE_RUNTIME_ABI_VERSION);
	return zend_native_content_mix_op_array(key, &script->main_op_array, 0);
}

static const zend_native_content_bundle *zend_native_content_bundle_find(
	const zend_native_content_key *key)
{
	const uint32_t flags = zend_native_executor_bundle_flags();
	const uint64_t session = zend_tpde_layout_training_session();

	for (uint32_t index = 0; index < zend_native_content_bundle_count;
			index++) {
		const zend_native_content_bundle *entry =
			&zend_native_content_bundles[index];

		if (entry->key.a == key->a && entry->key.b == key->b
				&& entry->flags == flags
				&& entry->training_session == session) {
			return entry;
		}
	}
	return NULL;
}

static void zend_native_content_bundle_store(
	const zend_native_content_key *key, const unsigned char *bytes,
	size_t size)
{
	zend_native_content_bundle *entry;

	if (size > ZEND_NATIVE_CONTENT_BUNDLES_BYTES / 4) {
		return;
	}
	if (zend_native_content_bundle_count == ZEND_NATIVE_CONTENT_BUNDLES_MAX
			|| zend_native_content_bundle_bytes + size
				> ZEND_NATIVE_CONTENT_BUNDLES_BYTES) {
		/* Start over: the cache serves scripts rewritten every request. */
		for (uint32_t index = 0; index < zend_native_content_bundle_count;
				index++) {
			free(zend_native_content_bundles[index].bytes);
		}
		zend_native_content_bundle_count = 0;
		zend_native_content_bundle_bytes = 0;
	}
	entry = &zend_native_content_bundles[zend_native_content_bundle_count];
	entry->bytes = malloc(size);
	if (entry->bytes == NULL) {
		return;
	}
	memcpy(entry->bytes, bytes, size);
	entry->key = *key;
	entry->flags = zend_native_executor_bundle_flags();
	entry->training_session = zend_tpde_layout_training_session();
	entry->size = size;
	zend_native_content_bundle_count++;
	zend_native_content_bundle_bytes += size;
}

/* Every closure of a content-keyed script joins its bundle. */
static bool zend_native_executor_compile_closures(
	zend_native_compiler *compiler, zend_op_array *op_array,
	zend_native_compile_diagnostic *diagnostic, uint32_t depth)
{
	for (uint32_t index = 0; index < op_array->num_dynamic_func_defs;
			index++) {
		zend_op_array *closure = op_array->dynamic_func_defs[index];

		if (depth > 16
				|| zend_native_compiler_compile(
					compiler, closure, NULL, 0, diagnostic) == FAILURE
				|| !zend_native_executor_compile_closures(
					compiler, closure, diagnostic, depth + 1)) {
			return false;
		}
	}
	return true;
}

static void zend_native_executor_attach_bundle(
	zend_script *script, const unsigned char *bytes, size_t size)
{
	zend_native_opcache_bundle *bundle = emalloc(
		zend_native_executor_bundle_allocation_size(size));

	bundle->magic = ZEND_NATIVE_OPCACHE_BUNDLE_MAGIC;
	bundle->format = ZEND_NATIVE_OPCACHE_BUNDLE_FORMAT;
	bundle->storage = ZEND_NATIVE_OPCACHE_BUNDLE_HEAP;
	bundle->flags = zend_native_executor_bundle_flags();
	bundle->training_session = zend_tpde_layout_training_session();
	bundle->size = size;
	memcpy(bundle->bytes, bytes, size);
	script->main_op_array.reserved[zend_native_executor_bundle_rid] =
		bundle;
}

static zend_result zend_native_executor_prepare_script_impl(
	zend_script *script,
	bool compile_main,
	zend_native_compile_diagnostic *product_diagnostic)
{
	zend_native_compiler_config config;
	zend_native_compile_diagnostic diagnostic;
	zend_native_compiler *compiler;
	zend_native_opcache_bundle *bundle;
	unsigned char *bytes = NULL;
	size_t size = 0;
	size_t allocation_size;
	uint32_t selected_count = 0;
	zend_native_content_key content_key;
	bool content_keyed;

	if (script == NULL || zend_native_executor_bundle_rid < 0) {
		return FAILURE;
	}
	if (zend_native_executor_bundle(&script->main_op_array) != NULL) {
		bundle = zend_native_executor_bundle(&script->main_op_array);
		if (zend_native_executor_bundle_compatible(bundle)
				|| bundle->storage
					!= ZEND_NATIVE_OPCACHE_BUNDLE_HEAP) {
			return SUCCESS;
		}
		zend_native_executor_discard_bundle(&script->main_op_array);
	}
	content_keyed = compile_main
		&& zend_native_content_key_of(script, &content_key);
	if (content_keyed) {
		const zend_native_content_bundle *cached =
			zend_native_content_bundle_find(&content_key);

		if (cached != NULL) {
			zend_native_executor_attach_bundle(
				script, cached->bytes, cached->size);
			return SUCCESS;
		}
	}
	memset(&config, 0, sizeof(config));
	memset(&diagnostic, 0, sizeof(diagnostic));
	config.script = script;
	config.target = zend_native_executor_target();
	config.frame_probe = zend_native_executor_frame_probe;
	config.frame_probe_context =
		zend_native_executor_frame_probe_context;
	config.source_probe = zend_native_runtime_source_probe_enabled();
	config.defer_publication = true;
	compiler = zend_native_compiler_create(&config, &diagnostic);
	if (compiler == NULL) {
		if (product_diagnostic != NULL) {
			*product_diagnostic = diagnostic;
		}
		return FAILURE;
	}
	zend_native_compile_trace_reason = "opcache-prepare";
	if ((compile_main
				&& zend_native_compiler_compile(
					compiler, &script->main_op_array, NULL, 0,
					&diagnostic) == FAILURE)
			|| (content_keyed
				&& !zend_native_executor_compile_closures(
					compiler, &script->main_op_array, &diagnostic, 0))
			|| !zend_native_executor_compile_captured_script(
				compiler, script, &diagnostic, &selected_count)) {
		zend_native_compile_trace_reason = "execute";
		if (product_diagnostic != NULL) {
			*product_diagnostic = diagnostic;
		}
		zend_native_compiler_destroy(compiler);
		return FAILURE;
	}
	zend_native_compile_trace_reason = "execute";
	if (!compile_main && selected_count == 0) {
		zend_native_compiler_destroy(compiler);
		return SUCCESS;
	}
	if (zend_native_compiler_serialize_bundle(
				compiler, &bytes, &size, &diagnostic) == FAILURE) {
		if (product_diagnostic != NULL) {
			*product_diagnostic = diagnostic;
		}
		zend_native_compiler_destroy(compiler);
		return FAILURE;
	}
	zend_native_compiler_destroy(compiler);
	if (content_keyed) {
		zend_native_content_bundle_store(&content_key, bytes, size);
	}
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
	bundle->flags = zend_native_executor_bundle_flags();
	bundle->training_session = zend_tpde_layout_training_session();
	bundle->size = size;
	memcpy(bundle->bytes, bytes, size);
	zend_native_compiler_bundle_destroy(bytes);
	script->main_op_array.reserved[zend_native_executor_bundle_rid] =
		bundle;
	return SUCCESS;
}

zend_result zend_native_executor_prepare_script(
	zend_script *script,
	zend_native_compile_diagnostic *diagnostic)
{
	return zend_native_executor_prepare_script_impl(
		script, true, diagnostic);
}

zend_result zend_native_executor_prepare_preloaded_script(
	zend_script *script,
	zend_native_compile_diagnostic *diagnostic)
{
	return zend_native_executor_prepare_script_impl(
		script, false, diagnostic);
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
				(void *) script) == NULL
			|| (op_array->filename != NULL
				&& zend_hash_index_update_ptr(
					&zend_native_executor_request_state.owners_by_file,
					(zend_ulong) (uintptr_t) op_array->filename,
					(void *) script) == NULL)) {
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
	zend_function *func;
	bool generator_frame;

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
	if ((execute_data->func->common.fn_flags
			& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
		zval *return_value = execute_data->return_value;

		if (!zend_native_call_normalize_trampoline(execute_data, NULL)) {
			EG(current_execute_data) = previous;
			return;
		}
		if (execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
			status = zend_native_internal_call_execute_top(execute_data);
			EG(current_execute_data) = previous;
			if (status == ZEND_NATIVE_BAILOUT) {
				zend_bailout();
			}
			return;
		}
		ZEND_ASSERT(execute_data->func->type == ZEND_USER_FUNCTION);
		/*
		 * execute_ex() callers have already made the trampoline frame current.
		 * Reinitializing that frame without restoring its caller would make the
		 * frame its own prev_execute_data and leave a cycle for shutdown GC.
		 */
		EG(current_execute_data) = previous;
		zend_init_func_execute_data(
			execute_data, &execute_data->func->op_array, return_value);
		ZEND_ADD_CALL_FLAG(execute_data, ZEND_CALL_TOP);
		ZEND_OBSERVER_FCALL_BEGIN(execute_data);
		previous = execute_data->prev_execute_data;
	}
	if (UNEXPECTED(!zend_native_executor_capture_preload_root(
			&execute_data->func->op_array))) {
		zend_throw_error(NULL,
			"Native preload root capture failed");
		EG(current_execute_data) = previous;
		return;
	}
	/*
	 * A generator that closes during execution frees its heap frame. Keep
	 * everything read after execution out of that frame.
	 */
	func = execute_data->func;
	generator_frame =
		(ZEND_CALL_INFO(execute_data) & ZEND_CALL_GENERATOR) != 0;
	entry_cell = NULL;
	if ((ZEND_CALL_INFO(execute_data) & ZEND_CALL_GENERATOR) != 0
			&& execute_data->return_value != NULL) {
		zend_generator *generator =
			(zend_generator *) execute_data->return_value;

		/* A suspended generator pins the immutable entry that created its
		 * heap frame. Resume it directly instead of resolving against the
		 * currently active caller generation, which may be a different root. */
		if (generator->native_entry_cell != NULL
				&& generator->native_entry_generation
					== generator->native_entry_cell->generation
				&& generator->native_entry_cell->function
					== execute_data->func) {
			entry_cell = generator->native_entry_cell;
		}
	}
	if (entry_cell == NULL
			&& zend_native_executor_request_state.execution_depth != 0) {
		entry_cell = zend_native_reentry_resolve(execute_data->func);
	}
	if (entry_cell != NULL
			&& (code = zend_native_entry_cell_load(entry_cell)) != NULL) {
		if (entry_cell->frame_probe != NULL) {
			entry_cell->frame_probe(
				entry_cell->frame_probe_context, previous,
				execute_data);
		}
		/* Only the code and the message start are read back. */
		diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_OK;
		diagnostic.message[0] = '\0';
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
			&& zend_native_op_array_identity_matches(
				&dispatch->op_array_identity,
				&execute_data->func->op_array)
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
			diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_OK;
			diagnostic.message[0] = '\0';
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
				generation = zend_native_executor_find_owner_generation(
					execute_data->func);
			}
			if (generation == NULL) {
				generation =
					zend_native_executor_find_function_generation(
						execute_data->func);
			}
		}
	} else {
		generation = zend_native_executor_find_root_generation(
			&execute_data->func->op_array);
		if (generation == NULL) {
			generation = zend_native_executor_find_function_generation(
				execute_data->func);
		}
		/* A closure of a cached script, entered from C (a shutdown
		 * function, an error handler): its copied op_array is mutable, but
		 * its opcodes are the script's, which the persistent generation
		 * indexed, as for an external reentry. */
		if (generation == NULL
				&& (zend_native_executor_bundle(
						&execute_data->func->op_array) != NULL
					|| (execute_data->func->common.fn_flags
						& ZEND_ACC_CLOSURE) != 0)) {
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
	if (!generation->persistent
			&& !zend_native_executor_sync_request_generation(generation)) {
		status = ZEND_NATIVE_EXCEPTION;
		snprintf(diagnostic.message, sizeof(diagnostic.message), "%s",
			"Native generation function index allocation failed");
	}
	entry_cell = zend_native_compiler_lookup(generation->compiler, func);
	if (entry_cell != NULL) {
		zend_native_executor_bind_dispatch(
			generation, &func->op_array, entry_cell);
	}
	zend_native_executor_release_completed_main_generation(
		generation, &func->op_array);
complete:
	if (status == ZEND_NATIVE_BAILOUT) {
		EG(current_execute_data) = previous;
		zend_bailout();
	}
	if (EG(exception) != NULL
			&& !generator_frame
			&& zend_native_runtime_source_probe_enabled()
			&& EG(opline_before_exception) != NULL
			&& EG(opline_before_exception)
				>= func->op_array.opcodes
			&& EG(opline_before_exception)
				< func->op_array.opcodes + func->op_array.last) {
		uint32_t source_position = (uint32_t) (EG(opline_before_exception)
			- func->op_array.opcodes);

		EG(current_execute_data) = execute_data;
		zend_native_runtime_source_probe(source_position);
	}
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) == NULL) {
		zend_throw_error(NULL, "%s",
			diagnostic.message[0] != '\0'
				? diagnostic.message
				: "Native userland execution failed");
	}
	EG(current_execute_data) = previous;
}
