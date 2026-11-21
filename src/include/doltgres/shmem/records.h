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
 * The records of host's shared seams (shmem's records.rs, design item 14's
 * S2) in the fork's fixed region.
 */
#ifndef DOLTGRES_SHMEM_RECORDS_H
#define DOLTGRES_SHMEM_RECORDS_H

#ifndef FRONTEND

#include "doltgres/shmem/lib.h"
#include "doltgres/std/tuple.h"

/* SHMEM_RECORDS_BLOBS is the most blobs a record holds. */
#define SHMEM_RECORDS_BLOBS ((size_t) 4)

/* SHMEM_RECORDS_ARRAYS is the most arrays a record holds. */
#define SHMEM_RECORDS_ARRAYS ((size_t) 4)

/* ShmemRecordsForm is how the server keeps a kind's records. */
typedef enum ShmemRecordsForm
{
	/* SHMEM_RECORDS_FORM_SINGLE is one record. */
	SHMEM_RECORDS_FORM_SINGLE = 0,

	/*
	 * SHMEM_RECORDS_FORM_POOL is a pool of records, each with a slot in the
	 * directory.
	 */
	SHMEM_RECORDS_FORM_POOL = 1,

	/*
	 * SHMEM_RECORDS_FORM_PROCESS is a record for each process, whose atomics
	 * are the process's words.
	 */
	SHMEM_RECORDS_FORM_PROCESS = 2,

	/*
	 * SHMEM_RECORDS_FORM_KEPT is a record for each process whose words the
	 * process's exit leaves as they are.
	 */
	SHMEM_RECORDS_FORM_KEPT = 3,

	/*
	 * SHMEM_RECORDS_FORM_AGED is a pool of records the delta's epochs free,
	 * each with a slot in the directory whose word has the delta's page
	 * directory's layout, and whose last atomic is the epoch of its
	 * retirement.
	 */
	SHMEM_RECORDS_FORM_AGED = 4,
} ShmemRecordsForm;

/* ShmemRecordsKind is a kind of records the server keeps: a shape's counts and how many records of it. */
typedef struct ShmemRecordsKind
{
	/* id is the shape's id. */
	uint64		id;
	/* form is how the server keeps the records. */
	ShmemRecordsForm form;
	/* count is the number of records. */
	size_t		count;
	/* locks is the number of a record's locks. */
	size_t		locks;
	/* flags is the number of a record's flags. */
	size_t		flags;
	/* atomics is the number of a record's atomics. */
	size_t		atomics;

	/*
	 * blobs gives each of a record's blobs its largest size, at most
	 * SHMEM_RECORDS_BLOBS of them.
	 */
	const size_t *blobs;
	size_t		blobs_len;

	/*
	 * arrays gives each of a record's arrays its count of entries and an
	 * entry's size, at most SHMEM_RECORDS_ARRAYS of them.
	 */
	const std_TupleUsizeUsize_t *arrays;
	size_t		arrays_len;
} ShmemRecordsKind;

/* ShmemRecordsOpTag is Op's variants. */
typedef enum ShmemRecordsOpTag
{
	SHMEM_RECORDS_OP_LOAD,
	SHMEM_RECORDS_OP_ADD,
	SHMEM_RECORDS_OP_EXCHANGE,
	SHMEM_RECORDS_OP_LOAD_ACQUIRE,
} ShmemRecordsOpTag;

/*
 * ShmemRecordsOp is an operation on an atomic: a load, an add, an exchange of
 * the second value for the first, or a load after a read barrier.
 */
typedef struct ShmemRecordsOp
{
	ShmemRecordsOpTag tag;
	union
	{
		uint64		add;
		struct
		{
			uint64		f0;
			uint64		f1;
		}			exchange;
	}			u;
} ShmemRecordsOp;

extern size_t shmem_records_size(const ShmemRecordsKind *kinds, size_t kinds_len, size_t procs);
extern bool shmem_records_initialize(const PgMayRaise *raise, ShmemControl *control, int32 tranche,
									 const ShmemRecordsKind *kinds, size_t kinds_len, PgPgError *error);
extern size_t shmem_records_kinds(void);
extern bool shmem_records_aged(size_t kind, std_TupleU32U32_t *result);
extern bool shmem_records_kind(uint64 id, size_t *result);
extern uint32 shmem_records_count(size_t kind);
extern size_t shmem_records_atomics(size_t kind);
extern ShmemRecordsForm shmem_records_form(size_t kind);
extern bool shmem_records_pool(size_t kind, std_TupleU32U32_t *result);
extern uint64 shmem_records_word(uint32 at);
extern bool shmem_records_swap(uint32 at, uint64 loaded, uint64 word);
extern uint32 shmem_records_hand(size_t kind);
extern uint64 shmem_records_retired(size_t kind);
extern bool shmem_records_retired_swap(size_t kind, uint64 loaded, uint64 word);
extern void shmem_records_clear(size_t kind, uint32 index);
extern bool shmem_records_lock(const PgMayRaise *raise, size_t kind, uint32 index, size_t lock, bool exclusive,
							   PgPgError *error);
extern void shmem_records_unlock(size_t kind, uint32 index, size_t lock);
extern bool shmem_records_flag_take(const PgMayRaise *raise, size_t kind, uint32 index, size_t flag,
									PgPgError *error);
extern void shmem_records_flag_clear(size_t kind, uint32 index, size_t flag);
extern uint64 shmem_records_atomic(size_t kind, uint32 index, size_t atomic, ShmemRecordsOp op);
extern void shmem_records_blob_read(size_t kind, uint32 index, size_t blob, StdVec *result);
extern bool shmem_records_blob_write(size_t kind, uint32 index, size_t blob, const uint8 *bytes, size_t bytes_len,
									 PgPgError *error);
extern bool shmem_records_entry_read(size_t kind, uint32 index, size_t array, size_t entry, StdVec *result,
									 PgPgError *error);
extern bool shmem_records_entry_write(size_t kind, uint32 index, size_t array, size_t entry, const uint8 *bytes,
									  size_t bytes_len, PgPgError *error);
extern uint64 shmem_records_slots_name(size_t kind, uint32 index, size_t array);
extern bool shmem_records_slots_rename(size_t kind, uint32 index, size_t array, uint64 loaded, uint64 name);
extern void shmem_records_slots_empty(size_t kind, uint32 index, size_t array);
extern bool shmem_records_slots_claim(const PgMayRaise *raise, size_t kind, uint32 index, size_t array,
									  const uint8 *bytes, size_t bytes_len, bool *result, PgPgError *error);
extern void shmem_records_slots_read(size_t kind, uint32 index, size_t array, StdVec *result);
extern bool shmem_records_word_load(size_t kind, uint32 proc, size_t word, uint64 *result, PgPgError *error);
extern void shmem_records_word_store(size_t kind, size_t word, uint64 value);
extern bool shmem_records_epochs_lock(const PgMayRaise *raise, PgPgError *error);
extern uint64 shmem_records_epochs_retiring(void);
extern bool shmem_records_epochs_retiring_now(uint64 *result);
extern uint64 shmem_records_epochs_passing(void);
extern uint64 shmem_records_epochs_current(void);
extern void shmem_records_epochs_unlock(void);
extern uint64 shmem_records_epochs_advance(void);
extern uint64 shmem_records_epochs_oldest(void);
extern bool shmem_records_epochs_read(const PgMayRaise *raise, uint64 *result, PgPgError *error);
extern void shmem_records_epochs_end(uint64 epoch);
extern size_t shmem_records_epochs_paused(void);
extern void shmem_records_epochs_resumed(size_t count);
extern uint32 shmem_records_proc_number(void);
extern void shmem_records_clear_flags(void);
extern void shmem_records_release(void);
extern void shmem_records_zero_words(void);

#ifdef DOLTGRES_TEST_SHMEM
extern void test_shmem_records_tests_a_records_parts_do_not_overlap(void);
extern void test_shmem_records_tests_a_process_has_a_record_of_its_words(void);
extern void test_shmem_records_tests_the_region_is_the_sum_of_its_kinds(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_SHMEM_RECORDS_H */
