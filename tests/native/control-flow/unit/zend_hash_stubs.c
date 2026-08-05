#include <stdlib.h>

#include "Zend/zend_hash.h"
#include "Zend/zend_operators.h"

void *zend_hash_find_ptr_lc(const HashTable *table, zend_string *key)
{
	zend_string *candidate_key;
	void *candidate;

	if (table == NULL || key == NULL) {
		return NULL;
	}
	ZEND_HASH_FOREACH_STR_KEY_PTR(table, candidate_key, candidate) {
		if (candidate_key != NULL && ZSTR_LEN(candidate_key) == ZSTR_LEN(key)
				&& zend_binary_strcasecmp(
					ZSTR_VAL(candidate_key), ZSTR_LEN(candidate_key),
					ZSTR_VAL(key), ZSTR_LEN(key)) == 0) {
			return candidate;
		}
	} ZEND_HASH_FOREACH_END();
	return NULL;
}
