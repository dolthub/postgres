/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The fork's tuple table slot (access' slot.rs): it holds a row as its
 * table's tree stores it, with the primary key's columns beside it, and
 * deforms it on demand into values in a buffer the slot owns.
 */
#ifndef DOLTGRES_ACCESS_SLOT_H
#define DOLTGRES_ACCESS_SLOT_H

#ifndef FRONTEND
#include "doltgres/access/heaptuple.h"
#include "doltgres/access/key.h"
#include "doltgres/access/transform.h"
#include "doltgres/pg/lib.h"
#include "pg_calls.h"

/* access_TupleU32U16_t is a (u32, u16): a ctid's block and its offset. */
typedef struct access_TupleU32U16_t
{
	uint32		f0;
	uint16		f1;
} access_TupleU32U16_t;

/*
 * AccessSlotVersion is Version: a stored row's version as the slot keeps
 * it, its writer's and its deleter's full xids, the deleter's zero for
 * none, and their commands, with its table's object id and its ctid.
 */
typedef struct AccessSlotVersion
{
	uint64		writer;
	uint32		command;
	uint64		deleter;
	uint32		deleter_command;
	uint32		table;
	access_TupleU32U16_t ctid;
} AccessSlotVersion;

/* AccessSlotOutside is Outside: a stored row's values' ids out of line, four bytes each, and their TOAST relation. */
typedef struct AccessSlotOutside
{
	StdBytes	ids;
	uint32		toast;
} AccessSlotOutside;

/*
 * AccessSlotChunk is Chunk: an overflow chunk a deform stopped at, its
 * address, the field it holds, and the values out of line before it.
 */
typedef struct AccessSlotChunk
{
	uint8		address[32];
	size_t		field;
	size_t		before;
} AccessSlotChunk;

extern void access_slot_keyed(const access_TupleUsizeOrderKeyType_t *key, size_t key_len, StdVec *result);
extern bool access_slot_key_columns(const uint8 *keyed, size_t keyed_len, StdVec *result, PgPgError *error);
extern bool access_slot_columns(const PgSlot *slot, StdVec *result, PgPgError *error);
extern void access_slot_identity(const PgSlot *slot, uint32 *result_0, access_TupleU32U16_t *result_1);
extern bool access_slot_ctid(uint64 tuple_id, uint16 most_offset, access_TupleU32U16_t *result, PgPgError *error);
extern bool access_slot_store(const PgMayRaise *raise, PgSlot *slot, const uint8 *keyed, size_t keyed_len,
							  const uint8 *key, size_t key_len, const uint8 *tuple, size_t tuple_len,
							  AccessSlotOutside outside, const uint8 *transform, size_t transform_len,
							  const AccessSlotVersion *version, PgPgError *error);
extern bool access_slot_getsomeattrs(const PgSlot *slot, int32 natts, bool *has_result, AccessSlotChunk *result,
									 PgPgError *error);
extern bool access_slot_overflowed(const PgSlot *slot, int32 natts, const AccessSlotChunk *chunk, const uint8 *bytes,
								   size_t bytes_len, bool *has_result, AccessSlotChunk *result, PgPgError *error);
extern bool access_slot_getsysattr(const PgMayRaise *raise, PgSlot *slot, int32 attnum, PgIsNull *isnull,
								   PgDatum *result, PgPgError *error);
extern bool access_slot_copy(const PgMayRaise *raise, PgSlot *slot, PgSlot *source, PgPgError *error);
#ifdef DOLTGRES_TEST_ACCESS
extern void test_access_slot_tests_a_key_columns_bytes_hold_its_position_order_and_type(void);
extern void test_access_slot_tests_a_tuple_id_splits_into_its_block_and_offset(void);
extern void test_access_slot_tests_a_deform_that_meets_no_overflow_chunk_leaves_its_chunk_as_it_was(void);
#endif
#endif

#endif							/* DOLTGRES_ACCESS_SLOT_H */
