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
 * The records of host's shared seams (design item 14's S2) in the fork's
 * fixed region: after the control record, each kind's place and counts, the
 * processes' epoch slots, the directory's words, one for each pooled record,
 * and every kind's records, each its LWLocks, its flags (a holder and a
 * condition variable each), its atomics, its blobs (a length and the bytes of
 * its largest size), and its arrays (a count, a name, and the entries). A
 * kind of words for each process is a record for each process, with the
 * words as its atomics. The postmaster lays all of it out once and
 * initializes every lock and condition variable; a process reads each kind's
 * place from the region and reaches a record by its kind and its number among
 * the kind's records. Every index is checked against the kind's counts before
 * it is used, so no caller can reach past a record's parts. What a directory
 * word and an array's name mean is host's: this module loads and swaps them.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/shmem/records.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "pg_calls.h"

#define RUST_FILE "shmem" STD_SEP "src" STD_SEP "records.rs"

/* SHMEM_RECORDS_FLAG_WAIT is the name of the wait event of a wait for another process's flag. */
static const char SHMEM_RECORDS_FLAG_WAIT[] = "versioned flag";

/* SHMEM_RECORDS_LINE is a cache line, which every part of the region and every record starts on. */
#define SHMEM_RECORDS_LINE ((size_t) 64)

/* SHMEM_RECORDS_HEAD is the size of what an array holds before its entries: its count and its name. */
#define SHMEM_RECORDS_HEAD (2 * sizeof(pg_atomic_uint64))

/* ShmemRecordsSpot is a kind's place and counts as the region holds them. */
typedef struct ShmemRecordsSpot
{
	uint64		id;
	/* at is the offset in the region of the kind's first record. */
	uint64		at;
	/* size is the size of one record. */
	uint64		size;
	/* first is the directory slot of the kind's first record, for a pool. */
	uint32		first;
	uint32		count;
	uint32		locks;
	uint32		flags;
	uint32		atomics;
	uint32		blobs;
	/* maxes gives each blob its largest size. */
	uint32		maxes[SHMEM_RECORDS_BLOBS];
	uint32		arrays;
	/* entries gives each array its count of entries. */
	uint32		entries[SHMEM_RECORDS_ARRAYS];
	/* widths gives each array the size of one entry. */
	uint32		widths[SHMEM_RECORDS_ARRAYS];
	ShmemRecordsForm form;
} ShmemRecordsSpot;

/* ShmemRecordsPlaced is a kind in the region: its spot, and the hand its makes sweep from. */
typedef struct ShmemRecordsPlaced
{
	ShmemRecordsSpot spot;
	pg_atomic_uint64 hand;
} ShmemRecordsPlaced;

/*
 * ShmemRecordsFlagSlot is a record's flag: the holder's proc number plus one,
 * or zero, and the condition variable its waiters sleep on.
 */
typedef struct ShmemRecordsFlagSlot
{
	pg_atomic_uint32 holder;
	ConditionVariable cv;
} ShmemRecordsFlagSlot;

/* ShmemRecordsLayout is where a record's parts begin, and its size. */
typedef struct ShmemRecordsLayout
{
	size_t		flags;
	size_t		atomics;
	/* blobs is where each blob begins: its length, then its bytes. */
	size_t		blobs[SHMEM_RECORDS_BLOBS];

	/*
	 * arrays is where each array begins: its count, its name, then its
	 * entries.
	 */
	size_t		arrays[SHMEM_RECORDS_ARRAYS];
	size_t		size;
} ShmemRecordsLayout;

static ShmemRecordsLayout shmem_records_layout(const ShmemRecordsSpot *spot);

/* shmem_records_Spot_of returns a kind's counts and its record's size, with no place yet. */
static ShmemRecordsSpot
shmem_records_Spot_of(const ShmemRecordsKind *kind)
{
	uint32		maxes[SHMEM_RECORDS_BLOBS] = {0};
	uint32		entries[SHMEM_RECORDS_ARRAYS] = {0};
	uint32		widths[SHMEM_RECORDS_ARRAYS] = {0};
	ShmemRecordsSpot spot;
	ShmemRecordsLayout layout;

	if (!(kind->blobs_len <= SHMEM_RECORDS_BLOBS))
		STD_PANIC(113, 9, "a shape declares more blobs than a record holds");
	if (!(kind->arrays_len <= SHMEM_RECORDS_ARRAYS))
		STD_PANIC(114, 9, "a shape declares more arrays than a record holds");
	for (size_t i = 0; i < SHMEM_RECORDS_BLOBS && i < kind->blobs_len; i++)
		maxes[i] = (uint32) kind->blobs[i];
	for (size_t i = 0; i < SHMEM_RECORDS_ARRAYS && i < kind->arrays_len; i++)
	{
		entries[i] = (uint32) kind->arrays[i].f0;
		widths[i] = (uint32) kind->arrays[i].f1;
	}
	spot.id = kind->id;
	spot.at = 0;
	spot.size = 0;
	spot.first = 0;
	spot.count = (uint32) kind->count;
	spot.locks = (uint32) kind->locks;
	spot.flags = (uint32) kind->flags;
	spot.atomics = (uint32) kind->atomics;
	spot.blobs = (uint32) kind->blobs_len;
	memcpy(spot.maxes, maxes, sizeof(maxes));
	spot.arrays = (uint32) kind->arrays_len;
	memcpy(spot.entries, entries, sizeof(entries));
	memcpy(spot.widths, widths, sizeof(widths));
	spot.form = kind->form;
	layout = shmem_records_layout(&spot);
	spot.size = (uint64) layout.size;
	return spot;
}

/* shmem_records_layout_closure_0 is after: where the next kind of part begins, past a count of parts of a size. */
static size_t
shmem_records_layout_closure_0(void *captures, size_t at, uint32 count, size_t each)
{
	size_t		tmp_1 = STD_MUL(usize, (size_t) count, each, 173, 60);
	size_t		tmp_2 = STD_ADD(usize, at, tmp_1, 173, 55);

	(void) captures;
	return std_next_multiple_of_usize(tmp_2, 16);
}

/*
 * shmem_records_layout returns where the parts of a record with a kind's
 * counts begin, its locks first, each kind of part on a 16-byte boundary and
 * each blob and array on an 8-byte one, and the record's size, in whole cache
 * lines.
 */
static ShmemRecordsLayout
shmem_records_layout(const ShmemRecordsSpot *spot)
{
	ShmemRecordsLayout result;
	size_t		flags = shmem_records_layout_closure_0(NULL, 0, spot->locks, sizeof(LWLock));
	size_t		atomics = shmem_records_layout_closure_0(NULL, flags, spot->flags, sizeof(ShmemRecordsFlagSlot));
	size_t		at = shmem_records_layout_closure_0(NULL, atomics, spot->atomics, sizeof(pg_atomic_uint64));
	size_t		blobs[SHMEM_RECORDS_BLOBS] = {0};
	size_t		arrays[SHMEM_RECORDS_ARRAYS] = {0};

	for (size_t i = 0; i < SHMEM_RECORDS_BLOBS && i < (size_t) spot->blobs; i++)
	{
		uint32		max = spot->maxes[i];
		size_t		tmp_1;
		size_t		tmp_2;

		blobs[i] = at;
		tmp_1 = STD_ADD(usize, at, sizeof(uint64), 180, 15);
		tmp_2 = STD_ADD(usize, tmp_1, (size_t) max, 180, 15);
		at = std_next_multiple_of_usize(tmp_2, 8);
	}
	for (size_t i = 0; i < SHMEM_RECORDS_ARRAYS && i < (size_t) spot->arrays; i++)
	{
		uint32		count = spot->entries[i];
		uint32		width = spot->widths[i];
		size_t		tmp_1;
		size_t		tmp_2;
		size_t		tmp_3;

		arrays[i] = at;
		tmp_1 = STD_ADD(usize, at, SHMEM_RECORDS_HEAD, 186, 15);
		tmp_2 = STD_MUL(usize, (size_t) count, (size_t) width, 186, 27);
		tmp_3 = STD_ADD(usize, tmp_1, tmp_2, 186, 15);
		at = std_next_multiple_of_usize(tmp_3, 8);
	}
	result.flags = flags;
	result.atomics = atomics;
	memcpy(result.blobs, blobs, sizeof(blobs));
	memcpy(result.arrays, arrays, sizeof(arrays));
	result.size = std_max_usize(std_next_multiple_of_usize(at, SHMEM_RECORDS_LINE), SHMEM_RECORDS_LINE);
	return result;
}

/* SHMEM_RECORDS_PLACED is the offset in the region of the kinds' places: after the control record, on a cache line. */
#define SHMEM_RECORDS_PLACED ((size_t) TYPEALIGN(SHMEM_RECORDS_LINE, sizeof(ShmemControl)))

/* ShmemRecordsPlan is where the region's parts go for the server's kinds and processes. */
typedef struct ShmemRecordsPlan
{
	/* epochs is the offset of the processes' epoch slots. */
	size_t		epochs;
	/* directory is the offset of the directory's words. */
	size_t		directory;
	/* slots is the number of the directory's slots. */
	size_t		slots;
	/* spots is each kind's place and counts, a Vec<Spot>. */
	StdVec		spots;
	/* size is the region's size. */
	size_t		size;
} ShmemRecordsPlan;

/* shmem_records_Plan_drop is Plan's drop glue: its spots. */
static void
shmem_records_Plan_drop(ShmemRecordsPlan *self)
{
	std_vec_free_Checked(&self->spots, sizeof(ShmemRecordsSpot), _Alignof(ShmemRecordsSpot));
}

/* shmem_records_plan_frame_0_t is plan's state across its pad: its arguments, its result, and its spots. */
typedef struct shmem_records_plan_frame_0_t
{
	const ShmemRecordsKind *kinds;
	size_t		kinds_len;
	size_t		procs;
	ShmemRecordsPlan *result;
	StdVec		spots;
	bool		spots_live;
} shmem_records_plan_frame_0_t;

/* shmem_records_plan_body_0 is plan's body. */
static void
shmem_records_plan_body_0(void *frame_)
{
	shmem_records_plan_frame_0_t *frame = frame_;
	size_t		tmp_1 = STD_MUL(usize, frame->kinds_len, sizeof(ShmemRecordsPlaced), 211, 28);
	size_t		tmp_2 = STD_ADD(usize, SHMEM_RECORDS_PLACED, tmp_1, 211, 19);
	size_t		epochs = std_next_multiple_of_usize(tmp_2, SHMEM_RECORDS_LINE);
	size_t		tmp_3 = STD_MUL(usize, frame->procs, sizeof(pg_atomic_uint64), 212, 31);
	size_t		tmp_4 = STD_ADD(usize, epochs, tmp_3, 212, 22);
	size_t		directory = std_next_multiple_of_usize(tmp_4, SHMEM_RECORDS_LINE);
	size_t		slots = 0;
	size_t		at;
	size_t		first = 0;

	for (size_t i = 0, i_len = frame->kinds_len; i < i_len; i++)
	{
		const ShmemRecordsKind *kind = &frame->kinds[i];

		if (kind->form == SHMEM_RECORDS_FORM_POOL || kind->form == SHMEM_RECORDS_FORM_AGED)
			slots = std_add_usize(slots, kind->count, STD_ACCUM_FILE, 206, 1);
	}
	{
		size_t		tmp_5 = STD_MUL(usize, slots, sizeof(pg_atomic_uint64), 215, 31);
		size_t		tmp_6 = STD_ADD(usize, directory, tmp_5, 215, 19);

		at = std_next_multiple_of_usize(tmp_6, SHMEM_RECORDS_LINE);
	}
	std_vec_with_capacity_Checked(&frame->spots, frame->kinds_len, sizeof(ShmemRecordsSpot),
								  _Alignof(ShmemRecordsSpot));
	frame->spots_live = true;
	for (size_t i = 0, i_len = frame->kinds_len; i < i_len; i++)
	{
		const ShmemRecordsKind *kind = &frame->kinds[i];
		uint64		tmp_7 = (uint64) at;
		uint32		tmp_8 = (uint32) first;
		ShmemRecordsSpot spot = shmem_records_Spot_of(kind);
		size_t		tmp_9;

		spot.at = tmp_7;
		spot.first = tmp_8;
		if (kind->form == SHMEM_RECORDS_FORM_POOL || kind->form == SHMEM_RECORDS_FORM_AGED)
			first = STD_ADD(usize, first, kind->count, 221, 13);
		tmp_9 = STD_MUL(usize, kind->count, (size_t) spot.size, 223, 15);
		at = STD_ADD(usize, at, tmp_9, 223, 9);
		std_vec_push_Checked(&frame->spots, &spot, sizeof(ShmemRecordsSpot), _Alignof(ShmemRecordsSpot));
	}
	frame->spots_live = false;
	frame->result->epochs = epochs;
	frame->result->directory = directory;
	frame->result->slots = slots;
	frame->result->spots = frame->spots;
	frame->result->size = at;
}

/* shmem_records_plan_pad_0 is plan's pad: its spots, while they are live. */
static void
shmem_records_plan_pad_0(void *frame_)
{
	shmem_records_plan_frame_0_t *frame = frame_;

	if (frame->spots_live)
		std_vec_free_Checked(&frame->spots, sizeof(ShmemRecordsSpot), _Alignof(ShmemRecordsSpot));
}

/*
 * shmem_records_plan lays the region out: the kinds' places, the epoch slots,
 * the directory, and each kind's records, each part on a cache line.
 */
static ShmemRecordsPlan
shmem_records_plan(const ShmemRecordsKind *kinds, size_t kinds_len, size_t procs)
{
	ShmemRecordsPlan result;
	shmem_records_plan_frame_0_t frame = {.kinds = kinds, .kinds_len = kinds_len, .procs = procs, .result = &result};

	std_landing(&frame, shmem_records_plan_body_0, shmem_records_plan_pad_0);
	return result;
}

/* shmem_records_size returns the region's size for the server's kinds and processes. */
size_t
shmem_records_size(const ShmemRecordsKind *kinds, size_t kinds_len, size_t procs)
{
	ShmemRecordsPlan tmp_1 = shmem_records_plan(kinds, kinds_len, procs);
	size_t		size = tmp_1.size;

	shmem_records_Plan_drop(&tmp_1);
	return size;
}

/* shmem_records_internal returns the fork's internal error with a message. */
static PgPgError
shmem_records_internal(const char *message, size_t message_len)
{
	StdString	owned;

	std_slice_to_vec_Checked(&owned, message, message_len, 1, 1);
	return pg_PgError_fork((const uint8 *) "XX000", &owned);
}

/* SHMEM_RECORDS_INTERNAL is shmem_records_internal of a literal message. */
#define SHMEM_RECORDS_INTERNAL(literal) shmem_records_internal((literal), sizeof(literal) - 1)

/* shmem_records_control returns the region's control record. */
static ShmemControl *
shmem_records_control(void)
{
	return (ShmemControl *) pg_calls_shmem_base();
}

/*
 * shmem_records_part returns the address in the region of the item at an
 * index among items of a size that begin at an offset.
 */
static void *
shmem_records_part(size_t offset, size_t index, size_t size)
{
	uint8	   *base = (uint8 *) pg_calls_shmem_base();
	size_t		tmp_1 = STD_MUL(usize, index, size, 246, 57);
	size_t		tmp_2 = STD_ADD(usize, offset, tmp_1, 246, 48);

	return STD_PTR_AT(base, std_wrapping_add_usize(0, tmp_2), 1);
}

/* ShmemRecordsMapped is a record in this process's mapping of the region, with its kind's counts and its layout. */
typedef struct ShmemRecordsMapped
{
	uint8	   *base;
	ShmemRecordsSpot spot;
	ShmemRecordsLayout layout;
} ShmemRecordsMapped;

/* shmem_records_Mapped_of returns the record at an index among a kind's records, refusing an index past the kind's count. */
static ShmemRecordsMapped
shmem_records_Mapped_of(const ShmemRecordsSpot *spot, size_t index)
{
	ShmemRecordsMapped result;
	size_t		tmp_1;

	if (!(index < (size_t) spot->count))
		STD_PANIC(419, 9, "a shared record's number is past its kind's count");
	tmp_1 = STD_MUL(usize, index, (size_t) spot->size, 420, 53);
	result.base = (uint8 *) shmem_records_part((size_t) spot->at, tmp_1, 1);
	result.spot = *spot;
	result.layout = shmem_records_layout(spot);
	return result;
}

/*
 * shmem_records_Mapped_part returns the address of the part of a size at an
 * index of a kind of part, refusing an index past the kind's count, which
 * only a shape whose markers break its own counts could pass.
 */
static void *
shmem_records_Mapped_part(const ShmemRecordsMapped *self, size_t at, uint32 count, size_t index, size_t size)
{
	size_t		tmp_1;
	size_t		tmp_2;

	if (!(index < (size_t) count))
		STD_PANIC(426, 9, "a shared record's part index is past its shape's count");
	tmp_1 = STD_MUL(usize, index, size, 427, 37);
	tmp_2 = STD_ADD(usize, at, tmp_1, 427, 32);
	return STD_PTR_AT(self->base, tmp_2, 1);
}

/* shmem_records_Mapped_lock returns a lock's address. */
static LWLock *
shmem_records_Mapped_lock(const ShmemRecordsMapped *self, size_t index)
{
	return shmem_records_Mapped_part(self, 0, self->spot.locks, index, sizeof(LWLock));
}

/* shmem_records_Mapped_flag returns a flag's address. */
static ShmemRecordsFlagSlot *
shmem_records_Mapped_flag(const ShmemRecordsMapped *self, size_t index)
{
	return shmem_records_Mapped_part(self, self->layout.flags, self->spot.flags, index, sizeof(ShmemRecordsFlagSlot));
}

/* shmem_records_Mapped_atomic returns an atomic's address. */
static pg_atomic_uint64 *
shmem_records_Mapped_atomic(const ShmemRecordsMapped *self, size_t index)
{
	return shmem_records_Mapped_part(self, self->layout.atomics, self->spot.atomics, index, sizeof(pg_atomic_uint64));
}

/* shmem_records_Mapped_blob returns the address of a blob's length, which its bytes follow. */
static uint64 *
shmem_records_Mapped_blob(const ShmemRecordsMapped *self, size_t index)
{
	if (!(index < (size_t) self->spot.blobs))
		STD_PANIC(447, 9, "a shared record's part index is past its shape's count");
	std_index_check(index, SHMEM_RECORDS_BLOBS, RUST_FILE, 448, 32);
	return STD_PTR_AT(self->base, self->layout.blobs[index], 1);
}

/* shmem_records_Mapped_array returns the address of an array's count, which its name and then its entries follow. */
static pg_atomic_uint64 *
shmem_records_Mapped_array(const ShmemRecordsMapped *self, size_t index)
{
	if (!(index < (size_t) self->spot.arrays))
		STD_PANIC(453, 9, "a shared record's part index is past its shape's count");
	std_index_check(index, SHMEM_RECORDS_ARRAYS, RUST_FILE, 454, 32);
	return STD_PTR_AT(self->base, self->layout.arrays[index], 1);
}

/* shmem_records_Mapped_entries returns the address of an array's first entry. */
static uint8 *
shmem_records_Mapped_entries(const ShmemRecordsMapped *self, size_t array)
{
	return STD_PTR_AT(shmem_records_Mapped_array(self, array), SHMEM_RECORDS_HEAD, 1);
}

/* shmem_records_Mapped_entry returns the address of an entry of an array, or nothing for an entry past the array's count. */
static uint8 *
shmem_records_Mapped_entry(const ShmemRecordsMapped *self, size_t array, uint64 entry)
{
	uint8	   *first = shmem_records_Mapped_entries(self, array);

	std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 465, 28);
	if (entry < (uint64) self->spot.entries[array])
	{
		size_t		tmp_1;

		std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 466, 58);
		tmp_1 = STD_MUL(usize, (size_t) entry, (size_t) self->spot.widths[array], 466, 41);
		return STD_PTR_AT(first, tmp_1, 1);
	}
	return NULL;
}

/* shmem_records_record returns the record at an index among a kind's records. */
static ShmemRecordsMapped shmem_records_record(size_t kind, uint32 index);

/* shmem_records_initialize_frame_0_t is initialize's state across its pad: its arguments and the plan. */
typedef struct shmem_records_initialize_frame_0_t
{
	const PgMayRaise *raise;
	ShmemControl *control;
	int32		tranche;
	const ShmemRecordsKind *kinds;
	size_t		kinds_len;
	PgPgError  *error;
	bool		done;
	ShmemRecordsPlan plan;
	bool		plan_live;
} shmem_records_initialize_frame_0_t;

/* shmem_records_initialize_body_0 is initialize's body. */
static void
shmem_records_initialize_body_0(void *frame_)
{
	shmem_records_initialize_frame_0_t *frame = frame_;
	ShmemControl *control = frame->control;
	size_t		procs = (size_t) pg_calls_max_procs();
	uint32		flag_wait;

	frame->plan = shmem_records_plan(frame->kinds, frame->kinds_len, procs);
	frame->plan_live = true;
	if (!pg_calls_wait_event_new(frame->raise, SHMEM_RECORDS_FLAG_WAIT, &flag_wait, frame->error))
	{
		frame->done = false;
		frame->plan_live = false;
		shmem_records_Plan_drop(&frame->plan);
		return;
	}
	pg_calls_lwlock_initialize(&control->epoch_lock, frame->tranche);
	control->epoch = 1;
	control->procs = (uint32) procs;
	control->tranche = frame->tranche;
	control->flag_wait = flag_wait;
	control->kinds = (uint32) frame->kinds_len;
	control->slots = (uint32) frame->plan.slots;
	control->epochs = (uint64) frame->plan.epochs;
	control->directory = (uint64) frame->plan.directory;
	for (size_t slot = 0, slot_end = procs; slot < slot_end; slot = std_forward_unchecked_usize_Checked(slot, 1))
		pg_calls_atomic_init_u64(shmem_records_part(frame->plan.epochs, slot, sizeof(pg_atomic_uint64)), 0);
	for (size_t slot = 0, slot_end = frame->plan.slots; slot < slot_end;
		 slot = std_forward_unchecked_usize_Checked(slot, 1))
		pg_calls_atomic_init_u64(shmem_records_part(frame->plan.directory, slot, sizeof(pg_atomic_uint64)), 0);
	for (size_t i = 0, i_len = frame->plan.spots.len, count = 0; i < i_len; i++)
	{
		size_t		kind = count;
		const ShmemRecordsSpot *spot = (const ShmemRecordsSpot *) STD_PTR_AT(frame->plan.spots.ptr, i,
																			 sizeof(ShmemRecordsSpot));
		ShmemRecordsPlaced *placed;

		count = std_Enumerate_count_step(count);
		placed = shmem_records_part(SHMEM_RECORDS_PLACED, kind, sizeof(ShmemRecordsPlaced));
		placed->spot = *spot;
		pg_calls_atomic_init_u64(&placed->hand, 0);
		for (size_t index = 0, index_end = (size_t) spot->count; index < index_end;
			 index = std_forward_unchecked_usize_Checked(index, 1))
		{
			ShmemRecordsMapped record = shmem_records_Mapped_of(spot, index);

			for (size_t lock = 0, lock_end = (size_t) spot->locks; lock < lock_end;
				 lock = std_forward_unchecked_usize_Checked(lock, 1))
				pg_calls_lwlock_initialize(shmem_records_Mapped_lock(&record, lock), frame->tranche);
			for (size_t flag = 0, flag_end = (size_t) spot->flags; flag < flag_end;
				 flag = std_forward_unchecked_usize_Checked(flag, 1))
			{
				pg_calls_atomic_init_u32(&shmem_records_Mapped_flag(&record, flag)->holder, 0);
				pg_calls_cv_init(&shmem_records_Mapped_flag(&record, flag)->cv);
			}
			for (size_t atomic = 0, atomic_end = (size_t) spot->atomics; atomic < atomic_end;
				 atomic = std_forward_unchecked_usize_Checked(atomic, 1))
				pg_calls_atomic_init_u64(shmem_records_Mapped_atomic(&record, atomic), 0);
			for (size_t blob = 0, blob_end = (size_t) spot->blobs; blob < blob_end;
				 blob = std_forward_unchecked_usize_Checked(blob, 1))
				*shmem_records_Mapped_blob(&record, blob) = 0;
			for (size_t array = 0, array_end = (size_t) spot->arrays; array < array_end;
				 array = std_forward_unchecked_usize_Checked(array, 1))
			{
				uint32		tmp_1;

				pg_calls_atomic_init_u64(shmem_records_Mapped_array(&record, array), 0);
				pg_calls_atomic_init_u64((pg_atomic_uint64 *) STD_PTR_AT(shmem_records_Mapped_array(&record, array), 1,
																		 sizeof(pg_atomic_uint64)), 0);
				std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 306, 59);
				std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 306, 81);
				tmp_1 = STD_MUL(u32, spot->entries[array], spot->widths[array], 306, 59);
				std_ptr_write_bytes(shmem_records_Mapped_entries(&record, array), 0, (size_t) tmp_1, 1, 1);
			}
		}
	}
	frame->plan_live = false;
	shmem_records_Plan_drop(&frame->plan);
	frame->done = true;
}

/* shmem_records_initialize_pad_0 is initialize's pad: the plan, while it is live. */
static void
shmem_records_initialize_pad_0(void *frame_)
{
	shmem_records_initialize_frame_0_t *frame = frame_;

	if (frame->plan_live)
		shmem_records_Plan_drop(&frame->plan);
}

/*
 * shmem_records_initialize lays the region out for the kinds, in the
 * postmaster: the fork-wide epochs, the directory with every slot free, and
 * every record with its locks and condition variables initialized and its
 * other parts zero. control is the region's control record, in a region of
 * shmem_records_size's bytes for these kinds, which nothing else uses yet.
 */
bool
shmem_records_initialize(const PgMayRaise *raise, ShmemControl *control, int32 tranche, const ShmemRecordsKind *kinds,
						 size_t kinds_len, PgPgError *error)
{
	shmem_records_initialize_frame_0_t frame = {.raise = raise, .control = control, .tranche = tranche,
	.kinds = kinds, .kinds_len = kinds_len, .error = error};

	std_landing(&frame, shmem_records_initialize_body_0, shmem_records_initialize_pad_0);
	return frame.done;
}

/*
 * shmem_records_spot returns a kind's place and counts, refusing a kind the
 * region doesn't hold, which only a caller that passes a kind this module
 * didn't give it could ask for.
 */
static ShmemRecordsSpot
shmem_records_spot(size_t kind)
{
	ShmemControl *control = shmem_records_control();

	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 321, 24);
	STD_CHECK_NULL_DEREF(control, 321, 24);
	if (!(kind < (size_t) control->kinds))
		STD_PANIC(321, 9, "a shared record's kind is past the region's kinds");
	return ((ShmemRecordsPlaced *) shmem_records_part(SHMEM_RECORDS_PLACED, kind, sizeof(ShmemRecordsPlaced)))->spot;
}

/* shmem_records_kinds returns the number of the region's kinds. */
size_t
shmem_records_kinds(void)
{
	ShmemControl *control = shmem_records_control();

	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 330, 14);
	STD_CHECK_NULL_DEREF(control, 330, 14);
	return (size_t) control->kinds;
}

/*
 * shmem_records_aged returns the directory's first slot of an aged kind's
 * records and their count, or nothing for a kind that isn't aged.
 */
bool
shmem_records_aged(size_t kind, std_TupleU32U32_t *result)
{
	ShmemRecordsSpot spot = shmem_records_spot(kind);

	if (spot.form == SHMEM_RECORDS_FORM_AGED)
	{
		result->f0 = spot.first;
		result->f1 = spot.count;
		return true;
	}
	return false;
}

/*
 * shmem_records_kind returns the number of the kind of a shape's id among the
 * region's kinds, or nothing when the server's table doesn't list it.
 */
bool
shmem_records_kind(uint64 id, size_t *result)
{
	ShmemControl *control = shmem_records_control();
	size_t		kinds;

	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 345, 26);
	STD_CHECK_NULL_DEREF(control, 345, 26);
	kinds = (size_t) control->kinds;
	for (size_t kind = 0, kind_end = kinds; kind < kind_end; kind = std_forward_unchecked_usize_Checked(kind, 1))
	{
		if (shmem_records_spot(kind).id == id)
		{
			*result = kind;
			return true;
		}
	}
	return false;
}

/* shmem_records_count returns the number of a kind's records. */
uint32
shmem_records_count(size_t kind)
{
	return shmem_records_spot(kind).count;
}

/* shmem_records_atomics returns the number of a kind's record's atomics, a single record's run included. */
size_t
shmem_records_atomics(size_t kind)
{
	return (size_t) shmem_records_spot(kind).atomics;
}

/* shmem_records_form returns how the server keeps a kind's records. */
ShmemRecordsForm
shmem_records_form(size_t kind)
{
	return shmem_records_spot(kind).form;
}

/*
 * shmem_records_pool returns the directory's first slot of a kind's records
 * and their count, or nothing for a kind that isn't a pool.
 */
bool
shmem_records_pool(size_t kind, std_TupleU32U32_t *result)
{
	ShmemRecordsSpot spot = shmem_records_spot(kind);

	if (spot.form == SHMEM_RECORDS_FORM_POOL)
	{
		result->f0 = spot.first;
		result->f1 = spot.count;
		return true;
	}
	return false;
}

/*
 * shmem_records_slot returns the directory's word of a slot, refusing a slot
 * past the directory, which only a caller that didn't check its location's
 * slot against its pool could ask for.
 */
static pg_atomic_uint64 *
shmem_records_slot(uint32 slot)
{
	ShmemControl *control = shmem_records_control();

	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 377, 24);
	STD_CHECK_NULL_DEREF(control, 377, 24);
	if (!(slot < control->slots))
		STD_PANIC(377, 9, "a shared record's slot is past the directory");
	control = shmem_records_control();
	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 378, 14);
	STD_CHECK_NULL_DEREF(control, 378, 14);
	return shmem_records_part((size_t) control->directory, (size_t) slot, sizeof(pg_atomic_uint64));
}

/* shmem_records_word loads the directory's word of a slot. */
uint64
shmem_records_word(uint32 at)
{
	return pg_calls_atomic_read_u64(shmem_records_slot(at));
}

/*
 * shmem_records_swap stores a word in a slot of the directory if the slot
 * still holds the word its caller loaded, reporting whether it did.
 */
bool
shmem_records_swap(uint32 at, uint64 loaded, uint64 word)
{
	uint64		seen = loaded;

	pg_calls_atomic_exchange_u64(shmem_records_slot(at), &seen, word);
	return seen == loaded;
}

/* shmem_records_hand returns the number among a pool's records at which a make's sweep starts, and moves the pool's hand on. */
uint32
shmem_records_hand(size_t kind)
{
	ShmemRecordsSpot spot = shmem_records_spot(kind);
	ShmemRecordsPlaced *placed = shmem_records_part(SHMEM_RECORDS_PLACED, kind, sizeof(ShmemRecordsPlaced));
	uint64		hand = pg_calls_atomic_add_u64(&placed->hand, 1);

	return (uint32) STD_REM(u64, hand, (uint64) spot.count, 406, 6);
}

static ShmemRecordsMapped
shmem_records_record(size_t kind, uint32 index)
{
	ShmemRecordsSpot spot = shmem_records_spot(kind);

	return shmem_records_Mapped_of(&spot, (size_t) index);
}

/* SHMEM_RECORDS_READERS_LOCK is READERS' RwLock, which guards SHMEM_RECORDS_READERS. */
static StdRwLock SHMEM_RECORDS_READERS_LOCK = STD_RW_LOCK_NEW;

/* SHMEM_RECORDS_READERS are this process's live readers' epochs, a Vec<u64>, whose oldest its slot publishes. */
static StdVec SHMEM_RECORDS_READERS = STD_VEC_NEW;

/* SHMEM_RECORDS_FLAGS_LOCK is FLAGS' RwLock, which guards SHMEM_RECORDS_FLAGS. */
static StdRwLock SHMEM_RECORDS_FLAGS_LOCK = STD_RW_LOCK_NEW;

/* SHMEM_RECORDS_FLAGS are the flags this process holds, a Vec of std_TupleUsizeU32Usize_t, which its exit clears. */
static StdVec SHMEM_RECORDS_FLAGS = STD_VEC_NEW;

/*
 * shmem_records_taken takes one of this process's own lists, which no other
 * thread uses and no call holding it reenters, so the take never waits; a
 * poisoned list keeps what it holds.
 */
static StdRwLockWriteGuard
shmem_records_taken(StdRwLock *list, void *data)
{
	StdRwLockWriteGuard guard;

	switch (std_rw_lock_try_write(list, data, &guard))
	{
		case STD_TRY_LOCK_OK:
			return guard;
		case STD_TRY_LOCK_POISONED:
			return guard;
		case STD_TRY_LOCK_WOULD_BLOCK:
			STD_PANIC(488, 42, "a process's own list was taken while it held it");
	}
	pg_unreachable();
}

/*
 * shmem_records_clear zeroes the atomics, empties the blobs, and zeroes the
 * arrays' entries of a record its maker just took, which no other process
 * reaches yet.
 */
void
shmem_records_clear(size_t kind, uint32 index)
{
	ShmemRecordsMapped record = shmem_records_record(kind, index);

	for (size_t atomic = 0, atomic_end = (size_t) record.spot.atomics; atomic < atomic_end;
		 atomic = std_forward_unchecked_usize_Checked(atomic, 1))
		pg_calls_atomic_write_u64(shmem_records_Mapped_atomic(&record, atomic), 0);
	for (size_t blob = 0, blob_end = (size_t) record.spot.blobs; blob < blob_end;
		 blob = std_forward_unchecked_usize_Checked(blob, 1))
		*shmem_records_Mapped_blob(&record, blob) = 0;
	for (size_t array = 0, array_end = (size_t) record.spot.arrays; array < array_end;
		 array = std_forward_unchecked_usize_Checked(array, 1))
	{
		uint32		tmp_1;

		std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 506, 51);
		std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 506, 80);
		tmp_1 = STD_MUL(u32, record.spot.entries[array], record.spot.widths[array],
						506, 51);
		std_ptr_write_bytes(shmem_records_Mapped_entries(&record, array), 0, (size_t) tmp_1, 1, 1);
	}
}

/* shmem_records_lock takes a record's lock, refusing one this process holds. */
bool
shmem_records_lock(const PgMayRaise *raise, size_t kind, uint32 index, size_t lock, bool exclusive, PgPgError *error)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);
	LWLock	   *held = shmem_records_Mapped_lock(&tmp_1, lock);

	if (pg_calls_lwlock_held(held))
	{
		*error = SHMEM_RECORDS_INTERNAL("a backend took a shared lock it already holds");
		return false;
	}
	return pg_calls_lwlock_acquire(raise, held, exclusive ? LW_EXCLUSIVE : LW_SHARED, error);
}

/* shmem_records_unlock releases a record's lock this process holds. */
void
shmem_records_unlock(size_t kind, uint32 index, size_t lock)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);

	pg_calls_lwlock_release(shmem_records_Mapped_lock(&tmp_1, lock));
}

/* shmem_records_room_closure_0 is refused: the out-of-memory error of a flag the list has no room for. */
static PgPgError
shmem_records_room_closure_0(void *captures)
{
	StdString	message;

	(void) captures;
	std_slice_to_vec_Checked(&message, "out of memory", sizeof("out of memory") - 1, 1, 1);
	return pg_PgError_fork((const uint8 *) "53200", &message);
}

/* shmem_records_room_frame_0_t is room's state across its pad: the flags' guard. */
typedef struct shmem_records_room_frame_0_t
{
	bool		reserved;
	StdRwLockWriteGuard guard;
	bool		guard_live;
} shmem_records_room_frame_0_t;

/* shmem_records_room_body_0 is room's take of the flags' list and its reserve of one more. */
static void
shmem_records_room_body_0(void *frame_)
{
	shmem_records_room_frame_0_t *frame = frame_;

	frame->guard = shmem_records_taken(&SHMEM_RECORDS_FLAGS_LOCK, &SHMEM_RECORDS_FLAGS);
	frame->guard_live = true;
	frame->reserved = std_vec_try_reserve(frame->guard.data, 1, sizeof(std_TupleUsizeU32Usize_t),
										  _Alignof(std_TupleUsizeU32Usize_t));
	frame->guard_live = false;
	std_RwLockWriteGuard_drop(&frame->guard);
}

/* shmem_records_room_pad_0 is room's pad: the flags' guard, while it is live. */
static void
shmem_records_room_pad_0(void *frame_)
{
	shmem_records_room_frame_0_t *frame = frame_;

	if (frame->guard_live)
		std_RwLockWriteGuard_drop(&frame->guard);
}

/*
 * shmem_records_room makes room in this process's list of flags for one
 * more, so that listing a flag it has set can't fail. The fork's tests have it
 * fail as the allocation does, through an injection point.
 */
static bool
shmem_records_room(const PgMayRaise *raise, PgPgError *error)
{
	shmem_records_room_frame_0_t frame = {0};
	bool		attached;

	std_landing(&frame, shmem_records_room_body_0, shmem_records_room_pad_0);
	if (!frame.reserved)
	{
		*error = shmem_records_room_closure_0(NULL);
		return false;
	}
	if (!pg_calls_injection_point_attached(raise, "doltgres-shared-flag-refused", &attached, error))
		return false;
	if (attached)
	{
		*error = shmem_records_room_closure_0(NULL);
		return false;
	}
	return true;
}

/* shmem_records_flag_take_frame_0_t is flag_take's state across its pad: the flag it lists and the list's guard. */
typedef struct shmem_records_flag_take_frame_0_t
{
	std_TupleUsizeU32Usize_t held;
	StdRwLockWriteGuard flags;
	bool		flags_live;
} shmem_records_flag_take_frame_0_t;

/* shmem_records_flag_take_body_0 is flag_take's push of the flag it took onto the process's list. */
static void
shmem_records_flag_take_body_0(void *frame_)
{
	shmem_records_flag_take_frame_0_t *frame = frame_;

	frame->flags = shmem_records_taken(&SHMEM_RECORDS_FLAGS_LOCK, &SHMEM_RECORDS_FLAGS);
	frame->flags_live = true;
	std_vec_push_Checked(frame->flags.data, &frame->held, sizeof(frame->held), _Alignof(std_TupleUsizeU32Usize_t));
	frame->flags_live = false;
	std_RwLockWriteGuard_drop(&frame->flags);
}

/* shmem_records_flag_take_pad_0 is flag_take's pad: the list's guard while it is live. */
static void
shmem_records_flag_take_pad_0(void *frame_)
{
	shmem_records_flag_take_frame_0_t *frame = frame_;

	if (frame->flags_live)
		std_RwLockWriteGuard_drop(&frame->flags);
}

/*
 * shmem_records_flag_take sets a record's flag, waiting interruptibly while
 * another process holds it, and refusing one this process holds. It makes
 * room for the flag in the process's list before it sets it, so nothing after
 * the set can fail and leave a flag that no exit clears.
 */
bool
shmem_records_flag_take(const PgMayRaise *raise, size_t kind, uint32 index, size_t flag, PgPgError *error)
{
	ShmemRecordsMapped tmp_1;
	ShmemRecordsFlagSlot *slot;
	uint32		me;
	ShmemControl *control;
	uint32		wait;

	if (!shmem_records_room(raise, error))
		return false;
	tmp_1 = shmem_records_record(kind, index);
	slot = shmem_records_Mapped_flag(&tmp_1, flag);
	me = STD_ADD(u32, (uint32) pg_calls_my_proc_number(), 1, 548, 14);
	control = shmem_records_control();
	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 551, 25);
	STD_CHECK_NULL_DEREF(control, 551, 25);
	wait = control->flag_wait;
	for (;;)
	{
		uint32		seen = 0;

		pg_calls_atomic_exchange_u32(&slot->holder, &seen, me);
		if (seen == 0 || seen == me)
		{
			pg_calls_cv_cancel();
			if (seen == me)
			{
				*error = SHMEM_RECORDS_INTERNAL("a backend set a shared flag it already holds");
				return false;
			}
			{
				shmem_records_flag_take_frame_0_t frame = {.held = {kind, index, flag}};

				std_landing(&frame, shmem_records_flag_take_body_0, shmem_records_flag_take_pad_0);
			}
			return true;
		}
		if (!pg_calls_cv_sleep(raise, &slot->cv, wait, error))
			return false;
	}
}

/* shmem_records_flag_clear_closure_0 is retain's predicate: whether a held flag isn't the one cleared. */
static bool
shmem_records_flag_clear_closure_0(void *captures, const void *element)
{
	const std_TupleUsizeU32Usize_t *cleared = captures;
	const std_TupleUsizeU32Usize_t *held = element;

	return !(held->f0 == cleared->f0 && held->f1 == cleared->f1 && held->f2 == cleared->f2);
}

/* shmem_records_flag_clear clears a record's flag this process holds and wakes its waiters. */
void
shmem_records_flag_clear(size_t kind, uint32 index, size_t flag)
{
	ShmemRecordsMapped tmp_2;
	ShmemRecordsFlagSlot *slot;

	{
		StdRwLockWriteGuard tmp_1 = shmem_records_taken(&SHMEM_RECORDS_FLAGS_LOCK, &SHMEM_RECORDS_FLAGS);
		std_TupleUsizeU32Usize_t cleared = {kind, index, flag};

		std_vec_retain_Checked(tmp_1.data, sizeof(std_TupleUsizeU32Usize_t), _Alignof(std_TupleUsizeU32Usize_t), NULL,
							   shmem_records_flag_clear_closure_0, &cleared);
		std_RwLockWriteGuard_drop(&tmp_1);
	}
	tmp_2 = shmem_records_record(kind, index);
	slot = shmem_records_Mapped_flag(&tmp_2, flag);
	pg_calls_atomic_write_u32(&slot->holder, 0);
	pg_calls_cv_broadcast(&slot->cv);
}

/* shmem_records_atomic operates on a record's atomic, returning its value before. */
uint64
shmem_records_atomic(size_t kind, uint32 index, size_t atomic, ShmemRecordsOp op)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);
	pg_atomic_uint64 *at = shmem_records_Mapped_atomic(&tmp_1, atomic);

	switch (op.tag)
	{
		case SHMEM_RECORDS_OP_LOAD:
			return pg_calls_atomic_read_u64(at);
		case SHMEM_RECORDS_OP_ADD:
			return pg_calls_atomic_add_u64(at, (int64) op.u.add);
		case SHMEM_RECORDS_OP_EXCHANGE:
			{
				uint64		seen = op.u.exchange.f0;

				pg_calls_atomic_exchange_u64(at, &seen, op.u.exchange.f1);
				return seen;
			}
	}
	pg_unreachable();
}

/* shmem_records_blob_read returns the bytes of a record's blob, under the blob's lock, which its caller holds. */
void
shmem_records_blob_read(size_t kind, uint32 index, size_t blob, StdVec *result)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);
	uint64	   *length = shmem_records_Mapped_blob(&tmp_1, blob);
	uint8	   *bytes = STD_PTR_AT(length, 1, sizeof(uint64));
	size_t		len = (size_t) *length;

	std_slice_from_raw_parts_check(bytes, len, 1, 1, RUST_FILE, 617, 14);
	std_slice_to_vec_Checked(result, bytes, len, 1, 1);
}

/*
 * shmem_records_blob_write writes the bytes of a record's blob in place,
 * under the blob's lock held exclusively, which its caller holds, refusing
 * bytes longer than the blob's largest size with nothing written. Nothing
 * between the length's store and the copy's end raises or checks for
 * interrupts.
 */
bool
shmem_records_blob_write(size_t kind, uint32 index, size_t blob, const uint8 *bytes, size_t bytes_len,
						 PgPgError *error)
{
	ShmemRecordsMapped record = shmem_records_record(kind, index);
	uint64	   *length = shmem_records_Mapped_blob(&record, blob);

	std_index_check(blob, SHMEM_RECORDS_BLOBS, RUST_FILE, 626, 22);
	if (bytes_len > (size_t) record.spot.maxes[blob])
	{
		*error = SHMEM_RECORDS_INTERNAL("a shared blob's bytes are longer than its largest size");
		return false;
	}
	*length = (uint64) bytes_len;
	std_ptr_copy_nonoverlapping_Checked(bytes, STD_PTR_AT(length, 1, sizeof(uint64)), bytes_len, 1, 1);
	return true;
}

/* shmem_records_entry_read_closure_0 is ok_or_else's error: an entry past an array's count. */
static PgPgError
shmem_records_entry_read_closure_0(void *captures)
{
	(void) captures;
	return SHMEM_RECORDS_INTERNAL("a shared array's entry is past its count");
}

/*
 * shmem_records_entry_read returns the bytes of an entry of a record's array,
 * under the array's lock, which its caller holds, refusing an entry past the
 * array's count.
 */
bool
shmem_records_entry_read(size_t kind, uint32 index, size_t array, size_t entry, StdVec *result, PgPgError *error)
{
	ShmemRecordsMapped record = shmem_records_record(kind, index);
	uint8	   *at = shmem_records_Mapped_entry(&record, array, (uint64) entry);
	size_t		width;

	if (at == NULL)
	{
		*error = shmem_records_entry_read_closure_0(NULL);
		return false;
	}
	std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 647, 49);
	width = (size_t) record.spot.widths[array];
	std_slice_from_raw_parts_check(at, width, 1, 1, RUST_FILE, 647, 17);
	std_slice_to_vec_Checked(result, at, width, 1, 1);
	return true;
}

/* shmem_records_entry_write_closure_0 is ok_or_else's error: an entry past an array's count. */
static PgPgError
shmem_records_entry_write_closure_0(void *captures)
{
	(void) captures;
	return SHMEM_RECORDS_INTERNAL("a shared array's entry is past its count");
}

/*
 * shmem_records_entry_write writes the bytes of an entry of a record's array
 * in place, under the array's lock held exclusively, which its caller holds,
 * refusing an entry past the array's count and bytes of another length than
 * an entry's with nothing written.
 */
bool
shmem_records_entry_write(size_t kind, uint32 index, size_t array, size_t entry, const uint8 *bytes,
						  size_t bytes_len, PgPgError *error)
{
	ShmemRecordsMapped record = shmem_records_record(kind, index);
	uint8	   *at = shmem_records_Mapped_entry(&record, array, (uint64) entry);

	if (at == NULL)
	{
		*error = shmem_records_entry_write_closure_0(NULL);
		return false;
	}
	std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 656, 23);
	if (bytes_len != (size_t) record.spot.widths[array])
	{
		*error = SHMEM_RECORDS_INTERNAL("a shared array's entry is of another length than its bytes");
		return false;
	}
	std_ptr_copy_nonoverlapping_Checked(bytes, at, bytes_len, 1, 1);
	return true;
}

/* shmem_records_slots_name loads the name of a record's slots array. */
uint64
shmem_records_slots_name(size_t kind, uint32 index, size_t array)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);

	return pg_calls_atomic_read_u64((pg_atomic_uint64 *) STD_PTR_AT(shmem_records_Mapped_array(&tmp_1, array), 1,
																	sizeof(pg_atomic_uint64)));
}

/*
 * shmem_records_slots_rename stores a name in a record's slots array if the
 * array still holds the name its caller loaded, reporting whether it did.
 */
bool
shmem_records_slots_rename(size_t kind, uint32 index, size_t array, uint64 loaded, uint64 name)
{
	uint64		seen = loaded;
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);

	pg_calls_atomic_exchange_u64((pg_atomic_uint64 *) STD_PTR_AT(shmem_records_Mapped_array(&tmp_1, array), 1,
																 sizeof(pg_atomic_uint64)), &seen, name);
	return seen == loaded;
}

/*
 * shmem_records_slots_empty zeroes the count of a record's slots array, for
 * the mark that just named its record, which holds that record's barrier
 * exclusively, so no claim runs.
 */
void
shmem_records_slots_empty(size_t kind, uint32 index, size_t array)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, index);

	pg_calls_atomic_write_u64(shmem_records_Mapped_array(&tmp_1, array), 0);
}

#ifdef USE_INJECTION_POINTS
/*
 * shmem_records_paused runs the tests' injection point between a claim's add
 * and its write. A server built without injection points has nothing there,
 * so nothing between the two can raise.
 */
static bool
shmem_records_paused(const PgMayRaise *raise, PgPgError *error)
{
	return pg_calls_injection_point(raise, "doltgres-shared-claiming", error);
}
#else
/* shmem_records_paused is nothing in a server built without injection points. */
static bool
shmem_records_paused(const PgMayRaise *raise, PgPgError *error)
{
	(void) raise;
	(void) error;
	return true;
}
#endif

/*
 * shmem_records_slots_claim adds an entry to a record's slots array and
 * writes its bytes, reporting whether it did: an array whose count is at its
 * entries takes none, and neither does a claim whose add wins an index past
 * them. Its caller holds the barrier of the record the array names, shared.
 * The tests' pause between the add and the write fails the claim only after
 * the entry is written, so an index that was won always holds its bytes.
 */
bool
shmem_records_slots_claim(const PgMayRaise *raise, size_t kind, uint32 index, size_t array, const uint8 *bytes,
						  size_t bytes_len, bool *result, PgPgError *error)
{
	ShmemRecordsMapped record = shmem_records_record(kind, index);
	pg_atomic_uint64 *count = shmem_records_Mapped_array(&record, array);
	uint64		won;
	PgPgError	paused_error;
	bool		paused;
	uint8	   *at;

	std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 713, 24);
	if (!(bytes_len == (size_t) record.spot.widths[array]))
		STD_PANIC(712, 5, "a shared slot's bytes are of another length than its array's entries");
	std_index_check(array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 718, 59);
	if (pg_calls_atomic_read_u64(count) >= (uint64) record.spot.entries[array])
	{
		*result = false;
		return true;
	}
	won = pg_calls_atomic_add_u64(count, 1);
	paused = shmem_records_paused(raise, &paused_error);
	at = shmem_records_Mapped_entry(&record, array, won);
	if (at == NULL)
	{
		if (!paused)
		{
			*error = paused_error;
			return false;
		}
		*result = false;
		return true;
	}
	std_ptr_copy_nonoverlapping_Checked(bytes, at, bytes_len, 1, 1);
	if (!paused)
	{
		*error = paused_error;
		return false;
	}
	*result = true;
	return true;
}

/* shmem_records_slots_read_frame_0_t is slots_read's state across its pad: its arguments and the entries made so far. */
typedef struct shmem_records_slots_read_frame_0_t
{
	size_t		kind;
	uint32		index;
	size_t		array;
	StdVec	   *result;
	StdVec		entries;
	bool		entries_live;
} shmem_records_slots_read_frame_0_t;

/* shmem_records_slots_read_closure_0 is map's closure: an entry's bytes. */
static void
shmem_records_slots_read_closure_0(const uint8 *first, size_t width, size_t entry, StdVec *result)
{
	size_t		tmp_1 = STD_MUL(usize, entry, width, 748, 78);
	const uint8 *at = STD_PTR_AT(first, tmp_1, 1);

	std_slice_from_raw_parts_check(at, width, 1, 1, RUST_FILE, 748, 31);
	std_slice_to_vec_Checked(result, at, width, 1, 1);
}

/* shmem_records_slots_read_body_0 is slots_read's body. */
static void
shmem_records_slots_read_body_0(void *frame_)
{
	shmem_records_slots_read_frame_0_t *frame = frame_;
	ShmemRecordsMapped record = shmem_records_record(frame->kind, frame->index);
	uint8	   *first = shmem_records_Mapped_entries(&record, frame->array);
	size_t		width;
	uint64		count;
	size_t		len;

	std_index_check(frame->array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 739, 50);
	width = (size_t) record.spot.widths[frame->array];
	count = pg_calls_atomic_read_u64(shmem_records_Mapped_array(&record, frame->array));
	std_index_check(frame->array, SHMEM_RECORDS_ARRAYS, RUST_FILE, 742, 83);
	count = std_min_u64(count, (uint64) record.spot.entries[frame->array]);
	len = (size_t) count;
	std_vec_with_capacity_Checked(&frame->entries, len, sizeof(StdVec), _Alignof(StdVec));
	frame->entries_live = true;
	for (size_t entry = 0; entry < len; entry = std_forward_unchecked_usize_Checked(entry, 1))
	{
		shmem_records_slots_read_closure_0(first, width, entry, (StdVec *) STD_PTR_AT(frame->entries.ptr, entry,
																					  sizeof(StdVec)));
		frame->entries.len = std_wrapping_add_usize(frame->entries.len, 1);
	}
	frame->entries_live = false;
	*frame->result = frame->entries;
}

/* shmem_records_slots_read_pad_0 is slots_read's pad: the entries made so far, each, and then their vector. */
static void
shmem_records_slots_read_pad_0(void *frame_)
{
	shmem_records_slots_read_frame_0_t *frame = frame_;

	if (frame->entries_live)
	{
		for (size_t i = 0, i_len = frame->entries.len; i < i_len; i++)
			std_vec_free_Checked((StdVec *) STD_PTR_AT(frame->entries.ptr, i, sizeof(StdVec)), 1, 1);
		std_vec_free_Checked(&frame->entries, sizeof(StdVec), _Alignof(StdVec));
	}
}

/*
 * shmem_records_slots_read returns the entries of a record's slots array
 * below its count, a Vec<Vec<u8>>. Its caller holds the barrier of the record
 * the array names, exclusively.
 */
void
shmem_records_slots_read(size_t kind, uint32 index, size_t array, StdVec *result)
{
	shmem_records_slots_read_frame_0_t frame = {.kind = kind, .index = index, .array = array, .result = result};

	std_landing(&frame, shmem_records_slots_read_body_0, shmem_records_slots_read_pad_0);
}

/* shmem_records_word_load loads a word of a process, refusing a process number past the server's processes. */
bool
shmem_records_word_load(size_t kind, uint32 proc, size_t word, uint64 *result, PgPgError *error)
{
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};

	if (proc >= shmem_records_spot(kind).count)
	{
		*error = SHMEM_RECORDS_INTERNAL("a process's number is past the server's processes");
		return false;
	}
	*result = shmem_records_atomic(kind, proc, word, load);
	return true;
}

/* shmem_records_word_store stores a word of this process. */
void
shmem_records_word_store(size_t kind, size_t word, uint64 value)
{
	ShmemRecordsMapped tmp_1 = shmem_records_record(kind, shmem_records_proc_number());

	pg_calls_atomic_write_u64(shmem_records_Mapped_atomic(&tmp_1, word), value);
}

/* shmem_records_epoch_lock returns the fork-wide epoch lock. */
static LWLock *
shmem_records_epoch_lock(void)
{
	return &shmem_records_control()->epoch_lock;
}

/* shmem_records_epochs_lock takes the epoch lock exclusively, refusing it when this process holds it. */
bool
shmem_records_epochs_lock(const PgMayRaise *raise, PgPgError *error)
{
	if (pg_calls_lwlock_held(shmem_records_epoch_lock()))
	{
		*error = SHMEM_RECORDS_INTERNAL("a backend took the epoch lock it already holds");
		return false;
	}
	return pg_calls_lwlock_acquire(raise, shmem_records_epoch_lock(), LW_EXCLUSIVE, error);
}

/*
 * shmem_records_epochs_retiring takes the epoch lock exclusively for a
 * retirement of the delta's pages, a step that can't fail and whose caller
 * holds no lock of the fork's, and returns the current epoch.
 */
uint64
shmem_records_epochs_retiring(void)
{
	pg_calls_lwlock_acquire_sure(shmem_records_epoch_lock(), LW_EXCLUSIVE);
	return shmem_records_control()->epoch;
}

/*
 * shmem_records_epochs_retiring_now takes the epoch lock exclusively when no
 * process holds it, for a free pass's raise of the kept stamp, which waits on
 * no retirement held between its stores, and returns the current epoch, or
 * nothing while the lock is held (bugs row 124).
 */
bool
shmem_records_epochs_retiring_now(uint64 *result)
{
	if (pg_calls_lwlock_try_sure(shmem_records_epoch_lock(), LW_EXCLUSIVE))
	{
		*result = shmem_records_control()->epoch;
		return true;
	}
	return false;
}

/*
 * shmem_records_epochs_passing returns the oldest epoch a live reader started
 * at, or the current one, for a free pass of the delta's pages, under its own
 * shared take of the epoch lock, which can't fail. A reader that starts
 * meanwhile starts at the current epoch, which is no older than the answer.
 */
uint64
shmem_records_epochs_passing(void)
{
	uint64		oldest;

	pg_calls_lwlock_acquire_sure(shmem_records_epoch_lock(), LW_SHARED);
	oldest = shmem_records_epochs_oldest();
	shmem_records_epochs_unlock();
	return oldest;
}

/* shmem_records_epochs_current returns the current epoch under its own shared take of the epoch lock, which can't fail. */
uint64
shmem_records_epochs_current(void)
{
	uint64		epoch;

	pg_calls_lwlock_acquire_sure(shmem_records_epoch_lock(), LW_SHARED);
	epoch = shmem_records_control()->epoch;
	shmem_records_epochs_unlock();
	return epoch;
}

/* shmem_records_epochs_unlock releases the epoch lock. */
void
shmem_records_epochs_unlock(void)
{
	pg_calls_lwlock_release(shmem_records_epoch_lock());
}

/* shmem_records_epochs_advance returns the current epoch and advances it, under the epoch lock held exclusively. */
uint64
shmem_records_epochs_advance(void)
{
	uint64		epoch = shmem_records_control()->epoch;

	shmem_records_control()->epoch = STD_ADD(u64, epoch, 1, 846, 45);
	return epoch;
}

/* shmem_records_slots returns the processes' epoch slots and their number. */
static void
shmem_records_slots(pg_atomic_uint64 **out_0, size_t *out_1)
{
	ShmemControl *control = shmem_records_control();

	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 855, 20);
	STD_CHECK_NULL_DEREF(control, 855, 20);
	*out_0 = shmem_records_part((size_t) control->epochs, 0, sizeof(pg_atomic_uint64));
	control = shmem_records_control();
	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 855, 54);
	STD_CHECK_NULL_DEREF(control, 855, 54);
	*out_1 = (size_t) control->procs;
}

/* shmem_records_epochs_oldest returns the oldest epoch a live reader started at, or the current one, under the epoch lock. */
uint64
shmem_records_epochs_oldest(void)
{
	pg_atomic_uint64 *slots;
	size_t		procs;
	uint64		current;

	shmem_records_slots(&slots, &procs);
	current = shmem_records_control()->epoch;
	for (size_t slot = 0, slot_end = procs; slot < slot_end; slot = std_forward_unchecked_usize_Checked(slot, 1))
	{
		uint64		epoch = pg_calls_atomic_read_u64((pg_atomic_uint64 *) STD_PTR_AT(slots, slot,
																					 sizeof(pg_atomic_uint64)));

		if (epoch != 0)
			current = std_min_u64(current, epoch);
	}
	return current;
}

/* shmem_records_publish writes this process's oldest live reader's epoch, or zero, into its slot. */
static void
shmem_records_publish(const uint64 *readers, size_t readers_len)
{
	pg_atomic_uint64 *slots;
	size_t		procs;
	size_t		me;
	uint64		oldest = 0;

	shmem_records_slots(&slots, &procs);
	me = (size_t) pg_calls_my_proc_number();
	if (!(me < procs))
		STD_PANIC(876, 5, "a process's number is below the number of slots");
	for (size_t i = 0, i_len = readers_len; i < i_len; i++)
		oldest = i == 0 ? readers[i] : std_min_u64(oldest, readers[i]);
	pg_calls_atomic_write_u64((pg_atomic_uint64 *) STD_PTR_AT(slots, me, sizeof(pg_atomic_uint64)), oldest);
}

/* shmem_records_epochs_read_frame_0_t is epochs_read's state across its pad: the readers' guard. */
typedef struct shmem_records_epochs_read_frame_0_t
{
	uint64		epoch;
	StdRwLockWriteGuard readers;
	bool		readers_live;
} shmem_records_epochs_read_frame_0_t;

/* shmem_records_epochs_read_body_0 is epochs_read's push of its epoch and the publish of the oldest. */
static void
shmem_records_epochs_read_body_0(void *frame_)
{
	shmem_records_epochs_read_frame_0_t *frame = frame_;
	StdVec	   *readers;

	frame->readers = shmem_records_taken(&SHMEM_RECORDS_READERS_LOCK, &SHMEM_RECORDS_READERS);
	frame->readers_live = true;
	readers = frame->readers.data;
	std_vec_push_Checked(readers, &frame->epoch, sizeof(uint64), _Alignof(uint64));
	shmem_records_publish(readers->ptr, readers->len);
	frame->readers_live = false;
	std_RwLockWriteGuard_drop(&frame->readers);
}

/* shmem_records_epochs_read_pad_0 is epochs_read's pad: the readers' guard, while it is live. */
static void
shmem_records_epochs_read_pad_0(void *frame_)
{
	shmem_records_epochs_read_frame_0_t *frame = frame_;

	if (frame->readers_live)
		std_RwLockWriteGuard_drop(&frame->readers);
}

/*
 * shmem_records_epochs_read starts a reader of this process at the current
 * epoch, published in its slot under the epoch lock, shared.
 */
bool
shmem_records_epochs_read(const PgMayRaise *raise, uint64 *result, PgPgError *error)
{
	shmem_records_epochs_read_frame_0_t frame = {0};

	if (!pg_calls_lwlock_acquire(raise, shmem_records_epoch_lock(), LW_SHARED, error))
		return false;
	frame.epoch = shmem_records_control()->epoch;
	std_landing(&frame, shmem_records_epochs_read_body_0, shmem_records_epochs_read_pad_0);
	shmem_records_epochs_unlock();
	*result = frame.epoch;
	return true;
}

/* shmem_records_epochs_end_frame_0_t is epochs_end's state across its pad: the epoch and the readers' guard. */
typedef struct shmem_records_epochs_end_frame_0_t
{
	uint64		epoch;
	StdRwLockWriteGuard readers;
	bool		readers_live;
} shmem_records_epochs_end_frame_0_t;

/*
 * shmem_records_epochs_end_closure_0 is renewed: the place of the oldest
 * entry newer than the epoch, the first of equal ones, or nothing.
 */
static bool
shmem_records_epochs_end_closure_0(const StdVec *readers, uint64 epoch, size_t *result)
{
	bool		found = false;
	uint64		least = 0;

	for (size_t at = 0, at_end = readers->len; at < at_end; at = std_forward_unchecked_usize_Checked(at, 1))
	{
		uint64		reader;

		std_index_check(at, readers->len, RUST_FILE, 903, 54);
		reader = *(const uint64 *) STD_PTR_AT(readers->ptr, at, sizeof(uint64));
		if (reader > epoch)
		{
			uint64		key;

			std_index_check(at, readers->len, RUST_FILE, 903, 92);
			key = *(const uint64 *) STD_PTR_AT(readers->ptr, at, sizeof(uint64));
			if (!found || least > key)
			{
				found = true;
				least = key;
				*result = at;
			}
		}
	}
	return found;
}

/* shmem_records_epochs_end_body_0 is epochs_end's body. */
static void
shmem_records_epochs_end_body_0(void *frame_)
{
	shmem_records_epochs_end_frame_0_t *frame = frame_;
	StdVec	   *readers;
	bool		has_at = false;
	size_t		at = 0;

	frame->readers = shmem_records_taken(&SHMEM_RECORDS_READERS_LOCK, &SHMEM_RECORDS_READERS);
	frame->readers_live = true;
	readers = frame->readers.data;
	for (size_t i = 0, i_len = readers->len; i < i_len; i++)
	{
		if (*(const uint64 *) STD_PTR_AT(readers->ptr, i, sizeof(uint64)) == frame->epoch)
		{
			has_at = true;
			at = i;
			break;
		}
	}
	if (!has_at)
		has_at = shmem_records_epochs_end_closure_0(readers, frame->epoch, &at);
	if (has_at)
	{
		uint64		removed;

		std_vec_swap_remove(readers, at, &removed, sizeof(uint64), _Alignof(uint64));
	}
	shmem_records_publish(readers->ptr, readers->len);
	frame->readers_live = false;
	std_RwLockWriteGuard_drop(&frame->readers);
}

/* shmem_records_epochs_end_pad_0 is epochs_end's pad: the readers' guard, while it is live. */
static void
shmem_records_epochs_end_pad_0(void *frame_)
{
	shmem_records_epochs_end_frame_0_t *frame = frame_;

	if (frame->readers_live)
		std_RwLockWriteGuard_drop(&frame->readers);
}

/*
 * shmem_records_epochs_end ends a reader of this process: its epoch's entry,
 * or else, for a reader a wait at the delta's budget renewed
 * (shmem_records_epochs_resumed), the oldest entry newer than its epoch, so
 * the list keeps one entry per live reader.
 */
void
shmem_records_epochs_end(uint64 epoch)
{
	shmem_records_epochs_end_frame_0_t frame = {.epoch = epoch};

	std_landing(&frame, shmem_records_epochs_end_body_0, shmem_records_epochs_end_pad_0);
}

/* shmem_records_epochs_paused_frame_0_t is epochs_paused's state across its pad: its count and the readers' guard. */
typedef struct shmem_records_epochs_paused_frame_0_t
{
	size_t		count;
	StdRwLockWriteGuard readers;
	bool		readers_live;
} shmem_records_epochs_paused_frame_0_t;

/* shmem_records_epochs_paused_body_0 is epochs_paused's body. */
static void
shmem_records_epochs_paused_body_0(void *frame_)
{
	shmem_records_epochs_paused_frame_0_t *frame = frame_;
	StdVec	   *readers;

	frame->readers = shmem_records_taken(&SHMEM_RECORDS_READERS_LOCK, &SHMEM_RECORDS_READERS);
	frame->readers_live = true;
	readers = frame->readers.data;
	frame->count = readers->len;
	std_vec_clear(readers, sizeof(uint64), NULL);
	shmem_records_publish(readers->ptr, readers->len);
	frame->readers_live = false;
	std_RwLockWriteGuard_drop(&frame->readers);
}

/* shmem_records_epochs_paused_pad_0 is epochs_paused's pad: the readers' guard, while it is live. */
static void
shmem_records_epochs_paused_pad_0(void *frame_)
{
	shmem_records_epochs_paused_frame_0_t *frame = frame_;

	if (frame->readers_live)
		std_RwLockWriteGuard_drop(&frame->readers);
}

/*
 * shmem_records_epochs_paused gives up the epochs of all this process's
 * readers for a wait at the delta's budget (the budget wait text, draft 2),
 * so the wait keeps nothing it waits for: its slot publishes none, and it
 * returns how many readers it gave up, which shmem_records_epochs_resumed
 * starts again.
 */
size_t
shmem_records_epochs_paused(void)
{
	shmem_records_epochs_paused_frame_0_t frame = {0};

	std_landing(&frame, shmem_records_epochs_paused_body_0, shmem_records_epochs_paused_pad_0);
	return frame.count;
}

/* shmem_records_epochs_resumed_frame_0_t is epochs_resumed's state across its pad: the count, the epoch, and the readers' guard. */
typedef struct shmem_records_epochs_resumed_frame_0_t
{
	size_t		count;
	uint64		epoch;
	StdRwLockWriteGuard readers;
	bool		readers_live;
} shmem_records_epochs_resumed_frame_0_t;

/* shmem_records_epochs_resumed_body_0 is epochs_resumed's resize of the list and its publish. */
static void
shmem_records_epochs_resumed_body_0(void *frame_)
{
	shmem_records_epochs_resumed_frame_0_t *frame = frame_;
	StdVec	   *readers;

	frame->readers = shmem_records_taken(&SHMEM_RECORDS_READERS_LOCK, &SHMEM_RECORDS_READERS);
	frame->readers_live = true;
	readers = frame->readers.data;
	std_vec_resize(readers, frame->count, &frame->epoch, sizeof(uint64), _Alignof(uint64));
	shmem_records_publish(readers->ptr, readers->len);
	frame->readers_live = false;
	std_RwLockWriteGuard_drop(&frame->readers);
}

/* shmem_records_epochs_resumed_pad_0 is epochs_resumed's pad: the readers' guard, while it is live. */
static void
shmem_records_epochs_resumed_pad_0(void *frame_)
{
	shmem_records_epochs_resumed_frame_0_t *frame = frame_;

	if (frame->readers_live)
		std_RwLockWriteGuard_drop(&frame->readers);
}

/*
 * shmem_records_epochs_resumed starts again, at the current epoch, the
 * readers a wait gave up, under the epoch lock shared, which can't fail; the
 * list kept their room, so it takes no memory.
 */
void
shmem_records_epochs_resumed(size_t count)
{
	shmem_records_epochs_resumed_frame_0_t frame = {.count = count};

	pg_calls_lwlock_acquire_sure(shmem_records_epoch_lock(), LW_SHARED);
	frame.epoch = shmem_records_control()->epoch;
	std_landing(&frame, shmem_records_epochs_resumed_body_0, shmem_records_epochs_resumed_pad_0);
	shmem_records_epochs_unlock();
}

/* shmem_records_proc_number returns this process's number. */
uint32
shmem_records_proc_number(void)
{
	return (uint32) pg_calls_my_proc_number();
}

/* shmem_records_clear_flags_frame_0_t is clear_flags's state across its pad: the flags taken from the list. */
typedef struct shmem_records_clear_flags_frame_0_t
{
	StdVec		held;
	bool		held_live;
	size_t		at;
} shmem_records_clear_flags_frame_0_t;

/* shmem_records_clear_flags_body_0 clears each flag taken from the list, consuming them. */
static void
shmem_records_clear_flags_body_0(void *frame_)
{
	shmem_records_clear_flags_frame_0_t *frame = frame_;

	while (frame->at < frame->held.len)
	{
		std_TupleUsizeU32Usize_t held = *(const std_TupleUsizeU32Usize_t *) STD_PTR_AT(frame->held.ptr, frame->at,
																					   sizeof(std_TupleUsizeU32Usize_t));

		frame->at = std_wrapping_add_usize(frame->at, 1);
		shmem_records_flag_clear(held.f0, held.f1, held.f2);
	}
	frame->held_live = false;
	std_vec_free_Checked(&frame->held, sizeof(std_TupleUsizeU32Usize_t), _Alignof(std_TupleUsizeU32Usize_t));
}

/* shmem_records_clear_flags_pad_0 is clear_flags's pad: the IntoIter's buffer, while it is live. */
static void
shmem_records_clear_flags_pad_0(void *frame_)
{
	shmem_records_clear_flags_frame_0_t *frame = frame_;

	if (frame->held_live)
		std_vec_free_Checked(&frame->held, sizeof(std_TupleUsizeU32Usize_t), _Alignof(std_TupleUsizeU32Usize_t));
}

/*
 * shmem_records_clear_flags clears the flags this process holds and wakes
 * their waiters, before each of its exit works and at its release, since a
 * process ended by a FATAL or pg_terminate_backend leaves without unwinding
 * the frames whose drops would.
 */
void
shmem_records_clear_flags(void)
{
	shmem_records_clear_flags_frame_0_t frame = {0};

	{
		StdRwLockWriteGuard tmp_1 = shmem_records_taken(&SHMEM_RECORDS_FLAGS_LOCK, &SHMEM_RECORDS_FLAGS);
		StdVec	   *list = tmp_1.data;

		frame.held = *list;
		std_vec_new(list, sizeof(std_TupleUsizeU32Usize_t), _Alignof(std_TupleUsizeU32Usize_t));
		std_RwLockWriteGuard_drop(&tmp_1);
	}
	frame.held_live = true;
	std_landing(&frame, shmem_records_clear_flags_body_0, shmem_records_clear_flags_pad_0);
}

/* shmem_records_release_frame_0_t is release's state across its pad: the readers' guard. */
typedef struct shmem_records_release_frame_0_t
{
	StdRwLockWriteGuard readers;
	bool		readers_live;
} shmem_records_release_frame_0_t;

/* shmem_records_release_body_0 is release's body past its clear of the flags. */
static void
shmem_records_release_body_0(void *frame_)
{
	shmem_records_release_frame_0_t *frame = frame_;
	StdVec	   *list;

	frame->readers = shmem_records_taken(&SHMEM_RECORDS_READERS_LOCK, &SHMEM_RECORDS_READERS);
	frame->readers_live = true;
	list = frame->readers.data;
	if (!(list->len == 0))
	{
		std_vec_clear(list, sizeof(uint64), NULL);
		shmem_records_publish(list->ptr, list->len);
	}
	frame->readers_live = false;
	std_RwLockWriteGuard_drop(&frame->readers);
}

/* shmem_records_release_pad_0 is release's pad: the readers' guard while it is live. */
static void
shmem_records_release_pad_0(void *frame_)
{
	shmem_records_release_frame_0_t *frame = frame_;

	if (frame->readers_live)
		std_RwLockWriteGuard_drop(&frame->readers);
}

/* shmem_records_release clears the flags this process holds and its reader slot, from its exit callback. */
void
shmem_records_release(void)
{
	shmem_records_release_frame_0_t frame = {0};

	shmem_records_clear_flags();
	std_landing(&frame, shmem_records_release_body_0, shmem_records_release_pad_0);
}

#ifdef USE_INJECTION_POINTS
/*
 * shmem_records_between is where the fork's tests hold an exit between two
 * stores of its zeroing of a kind's words, at a point named by the kind's id
 * in 16 hexadecimal digits, so a test holds the zeroing of its own kind
 * whatever other kinds the server lists.
 */
static void
shmem_records_between(uint64 id)
{
	char		name[] = "doltgres-shared-zeroing-0000000000000000";
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 24, i_len = 40, count = 0; i < i_len; i++)
	{
		size_t		at = count;
		char	   *digit = &name[i];
		size_t		tmp_1;

		count = std_Enumerate_count_step(count);
		tmp_1 = STD_MUL(usize, 4, at, 969, 52);
		{
			size_t		tmp_2 = STD_SUB(usize, 60, tmp_1, 969, 47);
			uint64		tmp_3 = STD_SHR(u64, id, tmp_2, 969, 40);
			size_t		tmp_4 = (size_t) (tmp_3 & 15);

			std_index_check(tmp_4, 16, RUST_FILE, 969, 18);
			*digit = digits[tmp_4];
		}
	}
	pg_calls_zeroing(name);
}
#else
/* shmem_records_between is nothing in a server built without injection points. */
static void
shmem_records_between(uint64 id)
{
	(void) id;
}
#endif

/*
 * shmem_records_zero_words zeroes this process's words of every kind but a
 * kept one, each kind's lowest index first, from its exit callback, after its
 * flags are cleared and its handles given up, passing over a kind whose words
 * all read zero, which this process never stored.
 */
void
shmem_records_zero_words(void)
{
	ShmemControl *control = shmem_records_control();
	ShmemRecordsOp load = {SHMEM_RECORDS_OP_LOAD};
	size_t		kinds;
	uint32		proc;

	STD_CHECK_ALIGNED(control, _Alignof(ShmemControl), 986, 26);
	STD_CHECK_NULL_DEREF(control, 986, 26);
	kinds = (size_t) control->kinds;
	proc = shmem_records_proc_number();
	for (size_t kind = 0, kind_end = kinds; kind < kind_end; kind = std_forward_unchecked_usize_Checked(kind, 1))
	{
		size_t		words;
		bool		zero = true;

		if (!(shmem_records_form(kind) == SHMEM_RECORDS_FORM_PROCESS))
			continue;
		words = (size_t) shmem_records_spot(kind).atomics;
		for (size_t word = 0, word_end = words; word < word_end; word = std_forward_unchecked_usize_Checked(word, 1))
		{
			if (!(shmem_records_atomic(kind, proc, word, load) == 0))
			{
				zero = false;
				break;
			}
		}
		if (zero)
			continue;
		for (size_t word = 0, word_end = words; word < word_end; word = std_forward_unchecked_usize_Checked(word, 1))
		{
			if (word != 0)
				shmem_records_between(shmem_records_spot(kind).id);
			shmem_records_word_store(kind, word, 0);
		}
	}
}

#ifdef DOLTGRES_TEST_SHMEM

/* SHMEM_RECORDS_TESTS_KIND_BLOBS are the test kind's blobs' largest sizes. */
static const size_t SHMEM_RECORDS_TESTS_KIND_BLOBS[] = {5, 100};

/* SHMEM_RECORDS_TESTS_KIND_ARRAYS are the test kind's arrays' counts and entry sizes. */
static const std_TupleUsizeUsize_t SHMEM_RECORDS_TESTS_KIND_ARRAYS[] = {{3, 5}, {2, 48}};

/* shmem_records_tests_kind returns a kind of a count of records with two locks, a flag, three atomics, two blobs, and two arrays. */
static ShmemRecordsKind
shmem_records_tests_kind(uint64 id, ShmemRecordsForm form, size_t count)
{
	return (ShmemRecordsKind)
	{
		id, form, count, 2, 1, 3, SHMEM_RECORDS_TESTS_KIND_BLOBS,
			lengthof(SHMEM_RECORDS_TESTS_KIND_BLOBS), SHMEM_RECORDS_TESTS_KIND_ARRAYS,
			lengthof(SHMEM_RECORDS_TESTS_KIND_ARRAYS)
	};
}

/*
 * test_shmem_records_tests_a_records_parts_do_not_overlap: a record's parts
 * follow one another without overlapping, each kind of part on a 16-byte
 * boundary and each blob and array on an 8-byte one, in whole cache lines;
 * an array is its count, its name, and its entries.
 */
void
test_shmem_records_tests_a_records_parts_do_not_overlap(void)
{
	ShmemRecordsKind tmp_1 = shmem_records_tests_kind(1, SHMEM_RECORDS_FORM_SINGLE, 1);
	ShmemRecordsSpot tmp_2 = shmem_records_Spot_of(&tmp_1);
	ShmemRecordsLayout layout = shmem_records_layout(&tmp_2);

	STD_ASSERT(1016, 9, layout.flags >= (size_t) (2 * sizeof(LWLock)), "layout.flags >= 2 * size_of::<ffi::LWLock>()");
	STD_ASSERT(1017, 9,
			   layout.atomics >= STD_ADD(usize, layout.flags, sizeof(ShmemRecordsFlagSlot), 1017, 35),
			   "layout.atomics >= layout.flags + size_of::<FlagSlot>()");
	STD_ASSERT(1018, 9,
			   layout.blobs[0] >= STD_ADD(usize, layout.atomics, (size_t) (3 * sizeof(pg_atomic_uint64)), 1018, 36),
			   "layout.blobs[0] >= layout.atomics + 3 * size_of::<ffi::pg_atomic_uint64>()");
	STD_ASSERT(1019, 9,
			   layout.blobs[1] >= STD_ADD(usize, STD_ADD(usize, layout.blobs[0], sizeof(uint64), 1019, 36), 5, 1019, 36),
			   "layout.blobs[1] >= layout.blobs[0] + size_of::<u64>() + 5");
	STD_ASSERT(1020, 9,
			   layout.arrays[0] >= STD_ADD(usize, STD_ADD(usize, layout.blobs[1], sizeof(uint64), 1020, 37), 100, 1020,
										   37),
			   "layout.arrays[0] >= layout.blobs[1] + size_of::<u64>() + 100");
	STD_ASSERT(1021, 9,
			   layout.arrays[1] >= STD_ADD(usize, STD_ADD(usize, layout.arrays[0], SHMEM_RECORDS_HEAD, 1021, 37),
										   (size_t) (3 * 5), 1021, 37),
			   "layout.arrays[1] >= layout.arrays[0] + HEAD + 3 * 5");
	STD_ASSERT(1022, 9,
			   layout.size >= STD_ADD(usize, STD_ADD(usize, layout.arrays[1], SHMEM_RECORDS_HEAD, 1022, 32),
									  (size_t) (2 * 48), 1022, 32),
			   "layout.size >= layout.arrays[1] + HEAD + 2 * 48");
	{
		size_t		left_0 = STD_REM_FOLDED(usize, layout.flags, 16);
		size_t		left_1 = STD_REM_FOLDED(usize, layout.atomics, 16);
		size_t		left_2 = STD_REM_FOLDED(usize, layout.blobs[0], 8);
		size_t		left_3 = STD_REM_FOLDED(usize, layout.blobs[1], 8);
		size_t		right_0 = 0;
		StdTupleRef left = STD_TUPLE(4, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_usize_Debug_fmt),
									 STD_ITEM(&left_2, std_usize_Debug_fmt), STD_ITEM(&left_3, std_usize_Debug_fmt));
		StdTupleRef right = STD_TUPLE(4, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_0, std_usize_Debug_fmt),
									  STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_0, std_usize_Debug_fmt));

		STD_ASSERT_EQ(1023, 9, left_0 == 0 && left_1 == 0 && left_2 == 0 && left_3 == 0, &left, std_tuple_Debug_fmt,
					  &right, std_tuple_Debug_fmt);
	}
	{
		size_t		left_0 = STD_REM_FOLDED(usize, layout.arrays[0], 8);
		size_t		left_1 = STD_REM_FOLDED(usize, layout.arrays[1], 8);
		size_t		left_2 = SHMEM_RECORDS_HEAD;
		size_t		right_0 = 0;
		size_t		right_2 = 16;
		StdTupleRef left = STD_TUPLE(3, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_usize_Debug_fmt),
									 STD_ITEM(&left_2, std_usize_Debug_fmt));
		StdTupleRef right = STD_TUPLE(3, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_0, std_usize_Debug_fmt),
									  STD_ITEM(&right_2, std_usize_Debug_fmt));

		STD_ASSERT_EQ(1024, 9, left_0 == 0 && left_1 == 0 && left_2 == right_2, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		size_t		left = STD_REM_FOLDED(usize, layout.size, SHMEM_RECORDS_LINE);
		size_t		right = 0;

		STD_ASSERT_EQ(1025, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	{
		ShmemRecordsKind bare = {2, SHMEM_RECORDS_FORM_SINGLE, 1, 0, 0, 0, STD_DANGLING, 0, STD_DANGLING, 0};
		ShmemRecordsSpot tmp_3 = shmem_records_Spot_of(&bare);

		{
			uint64		left = tmp_3.size;
			uint64		right = (uint64) SHMEM_RECORDS_LINE;

			STD_ASSERT_EQ(1028, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
		}
	}
}

/* shmem_records_tests_VecSpot_Index_index is Vec<Spot>'s Index of the plan's spots: the spot at an index, checked at the indexing's line and column. */
static const ShmemRecordsSpot *
shmem_records_tests_VecSpot_Index_index(const ShmemRecordsPlan *plan, size_t index, uint32 line, uint32 col)
{
	std_index_check(index, plan->spots.len, RUST_FILE, line, col);
	return &STD_VEC_AT(&plan->spots, ShmemRecordsSpot, index);
}

/* test_shmem_records_tests_a_process_has_a_record_of_its_words_frame_0_t is the test's state across its pad: the plan. */
typedef struct test_shmem_records_tests_a_process_has_a_record_of_its_words_frame_0_t
{
	ShmemRecordsPlan plan;
	bool		plan_live;
} test_shmem_records_tests_a_process_has_a_record_of_its_words_frame_0_t;

/* test_shmem_records_tests_a_process_has_a_record_of_its_words_pad_0 is the test's pad: the plan while it is live. */
static void
test_shmem_records_tests_a_process_has_a_record_of_its_words_pad_0(void *frame_)
{
	test_shmem_records_tests_a_process_has_a_record_of_its_words_frame_0_t *frame = frame_;

	if (frame->plan_live)
		shmem_records_Plan_drop(&frame->plan);
}

/* test_shmem_records_tests_a_process_has_a_record_of_its_words_body_0 is the test's body. */
static void
test_shmem_records_tests_a_process_has_a_record_of_its_words_body_0(void *frame_)
{
	test_shmem_records_tests_a_process_has_a_record_of_its_words_frame_0_t *frame = frame_;
	ShmemRecordsKind words = {9, SHMEM_RECORDS_FORM_PROCESS, 100, 0, 0, 7, STD_DANGLING, 0, STD_DANGLING, 0};
	ShmemRecordsKind kinds[2];
	ShmemRecordsLayout record;

	kinds[0] = shmem_records_tests_kind(1, SHMEM_RECORDS_FORM_POOL, 10);
	kinds[1] = words;
	frame->plan = shmem_records_plan(kinds, lengthof(kinds), 100);
	frame->plan_live = true;
	record = shmem_records_layout(shmem_records_tests_VecSpot_Index_index(&frame->plan, 1, 1038, 30));
	{
		size_t		left_0 = record.atomics;
		size_t		left_1 = record.size;
		size_t		right_0 = 0;
		size_t		right_1 = SHMEM_RECORDS_LINE;
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_usize_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_1, std_usize_Debug_fmt));

		STD_ASSERT_EQ(1039, 9, left_0 == right_0 && left_1 == right_1, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		uint64		tmp_0 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 1, 1040, 20)->at;
		uint64		tmp_1 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1040, 38)->at;
		uint64		tmp_2 = STD_MUL(u64, 10, shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1040, 62)->size, 1040, 57);

		{
			uint64		left = tmp_0;
			uint64		right = STD_ADD(u64, tmp_1, tmp_2, 1040, 38);

			STD_ASSERT_EQ(1040, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
		}
	}
	{
		size_t		tmp_0 = frame->plan.size;
		size_t		tmp_1 = (size_t) shmem_records_tests_VecSpot_Index_index(&frame->plan, 1, 1041, 31)->at;

		{
			size_t		left = tmp_0;
			size_t		right = STD_ADD(usize, tmp_1, (size_t) (100 * SHMEM_RECORDS_LINE), 1041, 31);

			STD_ASSERT_EQ(1041, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		}
	}
	{
		size_t		left = frame->plan.slots;
		size_t		right = 10;

		STD_ASSERT_EQ(1042, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	frame->plan_live = false;
	shmem_records_Plan_drop(&frame->plan);
}

/*
 * test_shmem_records_tests_a_process_has_a_record_of_its_words: a kind of
 * words for each process is a record for each process, whose atomics are
 * the words, each process's on a cache line of its own.
 */
void
test_shmem_records_tests_a_process_has_a_record_of_its_words(void)
{
	test_shmem_records_tests_a_process_has_a_record_of_its_words_frame_0_t frame = {0};

	std_landing(&frame, test_shmem_records_tests_a_process_has_a_record_of_its_words_body_0,
				test_shmem_records_tests_a_process_has_a_record_of_its_words_pad_0);
}

/* test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_frame_0_t is the test's state across its pad: the plan. */
typedef struct test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_frame_0_t
{
	ShmemRecordsPlan plan;
	bool		plan_live;
} test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_frame_0_t;

/* test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_pad_0 is the test's pad: the plan while it is live. */
static void
test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_pad_0(void *frame_)
{
	test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_frame_0_t *frame = frame_;

	if (frame->plan_live)
		shmem_records_Plan_drop(&frame->plan);
}

/* test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_body_0 is the test's body. */
static void
test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_body_0(void *frame_)
{
	test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_frame_0_t *frame = frame_;
	ShmemRecordsKind kinds[3];
	size_t		record;

	kinds[0] = shmem_records_tests_kind(1, SHMEM_RECORDS_FORM_POOL, 10);
	kinds[1] = shmem_records_tests_kind(2, SHMEM_RECORDS_FORM_SINGLE, 1);
	kinds[2] = shmem_records_tests_kind(3, SHMEM_RECORDS_FORM_POOL, 7);
	frame->plan = shmem_records_plan(kinds, lengthof(kinds), 100);
	frame->plan_live = true;
	{
		ShmemRecordsSpot tmp_1 = shmem_records_Spot_of(&kinds[0]);

		record = (size_t) tmp_1.size;
	}
	{
		size_t		left = (size_t) (SHMEM_RECORDS_PLACED % SHMEM_RECORDS_LINE);
		size_t		right = 0;

		STD_ASSERT_EQ(1053, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	STD_ASSERT(1054, 9, SHMEM_RECORDS_PLACED >= sizeof(ShmemControl), "PLACED >= size_of::<Control>()");
	STD_ASSERT(1055, 9, frame->plan.epochs >= (size_t) (SHMEM_RECORDS_PLACED + 3 * sizeof(ShmemRecordsPlaced)),
			   "plan.epochs >= PLACED + 3 * size_of::<Placed>()");
	STD_ASSERT(1056, 9, frame->plan.directory >= STD_ADD(usize, frame->plan.epochs, (size_t) (100 * 8), 1056, 35),
			   "plan.directory >= plan.epochs + 100 * 8");
	{
		size_t		left = frame->plan.slots;
		size_t		right = 17;

		STD_ASSERT_EQ(1057, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	{
		size_t		tmp_1 = (size_t) shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1058, 17)->at;

		STD_ASSERT(1058, 9, tmp_1 >= STD_ADD(usize, frame->plan.directory, (size_t) (17 * 8), 1058, 46),
				   "plan.spots[0].at as usize >= plan.directory + 17 * 8");
	}
	{
		uint64		tmp_0 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 1, 1059, 20)->at;
		uint64		tmp_1 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1059, 38)->at;
		uint64		tmp_2 = STD_MUL(u64, 10, (uint64) record, 1059, 57);

		{
			uint64		left = tmp_0;
			uint64		right = STD_ADD(u64, tmp_1, tmp_2, 1059, 38);

			STD_ASSERT_EQ(1059, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
		}
	}
	{
		uint64		tmp_0 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 2, 1060, 20)->at;
		uint64		tmp_1 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 1, 1060, 38)->at;

		{
			uint64		left = tmp_0;
			uint64		right = STD_ADD(u64, tmp_1, (uint64) record, 1060, 38);

			STD_ASSERT_EQ(1060, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
		}
	}
	{
		size_t		tmp_0 = frame->plan.size;
		size_t		tmp_1 = (size_t) shmem_records_tests_VecSpot_Index_index(&frame->plan, 2, 1061, 31)->at;
		size_t		tmp_2 = STD_MUL(usize, 7, record, 1061, 59);

		{
			size_t		left = tmp_0;
			size_t		right = STD_ADD(usize, tmp_1, tmp_2, 1061, 31);

			STD_ASSERT_EQ(1061, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
		}
	}
	{
		uint32		left_0 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1062, 21)->first;
		uint32		left_1 = shmem_records_tests_VecSpot_Index_index(&frame->plan, 2, 1062, 42)->first;
		uint32		right_0 = 0;
		uint32		right_1 = 10;
		StdTupleRef left = STD_TUPLE(2, STD_ITEM(&left_0, std_u32_Debug_fmt), STD_ITEM(&left_1, std_u32_Debug_fmt));
		StdTupleRef right = STD_TUPLE(2, STD_ITEM(&right_0, std_u32_Debug_fmt), STD_ITEM(&right_1, std_u32_Debug_fmt));

		STD_ASSERT_EQ(1062, 9, left_0 == right_0 && left_1 == right_1, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		bool		held = shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1063, 17)->form == SHMEM_RECORDS_FORM_POOL;

		if (held)
			held = shmem_records_tests_VecSpot_Index_index(&frame->plan, 1, 1063, 53)->form == SHMEM_RECORDS_FORM_SINGLE;
		STD_ASSERT(1063, 9, held, "plan.spots[0].form == Form::Pool && plan.spots[1].form == Form::Single");
	}
	{
		size_t		left_0 = STD_REM_FOLDED(usize, frame->plan.epochs, SHMEM_RECORDS_LINE);
		size_t		left_1 = STD_REM_FOLDED(usize, frame->plan.directory, SHMEM_RECORDS_LINE);
		size_t		left_2 = STD_REM_FOLDED(usize, (size_t) shmem_records_tests_VecSpot_Index_index(&frame->plan, 0, 1064, 64)->at,
											SHMEM_RECORDS_LINE);
		size_t		right_0 = 0;
		StdTupleRef left = STD_TUPLE(3, STD_ITEM(&left_0, std_usize_Debug_fmt), STD_ITEM(&left_1, std_usize_Debug_fmt),
									 STD_ITEM(&left_2, std_usize_Debug_fmt));
		StdTupleRef right = STD_TUPLE(3, STD_ITEM(&right_0, std_usize_Debug_fmt), STD_ITEM(&right_0, std_usize_Debug_fmt),
									  STD_ITEM(&right_0, std_usize_Debug_fmt));

		STD_ASSERT_EQ(1064, 9, left_0 == 0 && left_1 == 0 && left_2 == 0, &left, std_tuple_Debug_fmt, &right,
					  std_tuple_Debug_fmt);
	}
	{
		size_t		left = shmem_records_size(kinds, lengthof(kinds), 100);
		size_t		right = frame->plan.size;

		STD_ASSERT_EQ(1065, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	{
		size_t		left = shmem_records_size(STD_DANGLING, 0, 100);
		size_t		right = std_next_multiple_of_usize((size_t) (SHMEM_RECORDS_PLACED + 100 * 8), SHMEM_RECORDS_LINE);

		STD_ASSERT_EQ(1066, 9, left == right, &left, std_usize_Debug_fmt, &right, std_usize_Debug_fmt);
	}
	frame->plan_live = false;
	shmem_records_Plan_drop(&frame->plan);
}

/*
 * test_shmem_records_tests_the_region_is_the_sum_of_its_kinds: the region
 * holds the control record, the kinds' places, a slot for each process, a
 * word for each pooled record, and each kind's count of records, in that
 * order and without overlapping; a single record has no slot in the
 * directory, and each pool's slots follow the pool's before it.
 */
void
test_shmem_records_tests_the_region_is_the_sum_of_its_kinds(void)
{
	test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_frame_0_t frame = {0};

	std_landing(&frame, test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_body_0,
				test_shmem_records_tests_the_region_is_the_sum_of_its_kinds_pad_0);
}

#endif							/* DOLTGRES_TEST_SHMEM */

#endif							/* FRONTEND */
