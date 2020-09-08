#ifndef TARANTOOL_BOX_KEY_DEF_API_H_INCLUDED
#define TARANTOOL_BOX_KEY_DEF_API_H_INCLUDED
/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#include <stdint.h>
#include "trivia/util.h"

typedef struct tuple box_tuple_t;

/** \cond public */

typedef struct key_def box_key_def_t;

/** Key part definition flags. */
enum {
	BOX_KEY_PART_DEF_IS_NULLABLE_SHIFT = 0,
	BOX_KEY_PART_DEF_IS_NULLABLE_MASK =
		1 << BOX_KEY_PART_DEF_IS_NULLABLE_SHIFT,
};

/**
 * It is recommended to verify size of <box_key_part_def_t>
 * against this constant on the module side at build time.
 * Example:
 *
 * | #if !defined(__cplusplus) && !defined(static_assert)
 * | #define static_assert _Static_assert
 * | #endif
 * |
 * | (slash)*
 * |  * Verify that <box_key_part_def_t> has the same size when
 * |  * compiled within tarantool and within the module.
 * |  *
 * |  * It is important, because the module allocates an array of key
 * |  * parts and passes it to <box_key_def_new_ex>() tarantool
 * |  * function.
 * |  *(slash)
 * | static_assert(sizeof(box_key_part_def_t) == BOX_KEY_PART_DEF_T_SIZE,
 * |               "sizeof(box_key_part_def_t)");
 *
 * This snippet is not part of module.h, because portability of
 * static_assert() / _Static_assert() is dubious. It should be
 * decision of a module author how portable its code should be.
 */
enum {
	BOX_KEY_PART_DEF_T_SIZE = 64,
};

/**
 * Public representation of a key part definition.
 *
 * Usage: Allocate an array of such key parts, initialize each
 * key part (call <box_key_part_def_create>() and set necessary
 * fields), pass the array into <box_key_def_new_ex>() function.
 *
 * The idea of separation from internal <struct key_part_def> is
 * to provide stable API and ABI for modules.
 *
 * New fields may be added into the end of the structure in later
 * tarantool versions. Also new flags may be introduced within
 * <flags> field. <collation> cannot be changed to a union (to
 * reuse for some other value), because it is verified even for
 * a non-string key part by <box_key_def_new_ex>().
 *
 * Fields that are unknown at given tarantool version are ignored.
 */
typedef union PACKED {
	struct {
		/** Index of a tuple field (zero based). */
		uint32_t fieldno;
		/** Flags, e.g. nullability. */
		uint32_t flags;
		/** Type of the tuple field. */
		const char *field_type;
		/** Collation name for string comparisons. */
		const char *collation;
		/**
		 * JSON path to point a nested field.
		 *
		 * Example:
		 *
		 * tuple: [1, {"foo": "bar"}]
		 * key parts: [
		 *     {
		 *         "fieldno": 2,
		 *         "type": "string",
		 *         "path": "foo"
		 *     }
		 * ]
		 *
		 * => key: ["bar"]
		 *
		 * Note: When the path is given, <field_type>
		 * means type of the nested field.
		 */
		const char *path;
	};
	/**
	 * Padding to guarantee certain size across different
	 * tarantool versions.
	 */
	char padding[BOX_KEY_PART_DEF_T_SIZE];
} box_key_part_def_t;

/**
 * Create key definition with given field numbers and field types.
 *
 * May be used for tuple format creation and/or tuple comparison.
 *
 * \sa <box_key_def_new_ex>().
 *
 * \param fields array with key field identifiers
 * \param types array with key field types (see enum field_type)
 * \param part_count the number of key fields
 * \returns a new key definition object
 */
API_EXPORT box_key_def_t *
box_key_def_new(uint32_t *fields, uint32_t *types, uint32_t part_count);

/**
 * Initialize a key part with default values.
 *
 * All trailing padding bytes are set to zero.
 *
 * All unknown <flags> bits are set to zero.
 */
API_EXPORT void
box_key_part_def_create(box_key_part_def_t *part);

/**
 * Create a key_def from given key parts.
 *
 * Unlike <box_key_def_new>() this function allows to define
 * nullability, collation and other options for each key part.
 *
 * <box_key_part_def_t> fields that are unknown at given tarantool
 * version are ignored. The same for unknown <flags> bits.
 */
API_EXPORT box_key_def_t *
box_key_def_new_ex(box_key_part_def_t *parts, uint32_t part_count);

/**
 * Delete key definition
 *
 * \param key_def key definition to delete
 */
API_EXPORT void
box_key_def_delete(box_key_def_t *key_def);

/**
 * Dump key part definitions of given key_def.
 *
 * The function allocates key parts and storage for pointer fields
 * (e.g. JSON paths) on the box region.
 * @sa <box_region_truncate>().
 */
API_EXPORT box_key_part_def_t *
box_key_def_dump_parts(const box_key_def_t *key_def, uint32_t *part_count_ptr);

/**
 * Check that tuple fields match with given key definition.
 *
 * @param key_def Key definition.
 * @param tuple Tuple to validate.
 *
 * @retval 0  The tuple is valid.
 * @retval -1 The tuple is invalid.
 */
API_EXPORT int
box_tuple_validate_key_parts(box_key_def_t *key_def, box_tuple_t *tuple);

/**
 * Compare tuples using the key definition.
 * @param tuple_a first tuple
 * @param tuple_b second tuple
 * @param key_def key definition
 * @retval 0  if key_fields(tuple_a) == key_fields(tuple_b)
 * @retval <0 if key_fields(tuple_a) < key_fields(tuple_b)
 * @retval >0 if key_fields(tuple_a) > key_fields(tuple_b)
 */
API_EXPORT int
box_tuple_compare(box_tuple_t *tuple_a, box_tuple_t *tuple_b,
		  box_key_def_t *key_def);

/**
 * @brief Compare tuple with key using the key definition.
 * @param tuple tuple
 * @param key key with MessagePack array header
 * @param key_def key definition
 *
 * @retval 0  if key_fields(tuple) == parts(key)
 * @retval <0 if key_fields(tuple) < parts(key)
 * @retval >0 if key_fields(tuple) > parts(key)
 */
API_EXPORT int
box_tuple_compare_with_key(box_tuple_t *tuple_a, const char *key_b,
			   box_key_def_t *key_def);

/** \endcond public */

/*
 * Size of the structure should remain the same across all
 * tarantool versions in order to allow to allocate an array of
 * them.
 */
static_assert(sizeof(box_key_part_def_t) == BOX_KEY_PART_DEF_T_SIZE,
	      "sizeof(box_key_part_def_t)");

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_KEY_API_DEF_H_INCLUDED */
