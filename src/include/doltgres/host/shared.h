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
 * Shared memory (host's shared.rs, design item 14's S2): records a storage
 * crate declares by a shape of typed parts, reached only through host's
 * doors by part. In C a shape is a table of its constants and functions, and
 * a part's marker type is its index; the order of locks the Rust's types
 * hold the compiler to is what the C's callers keep, which the mapping
 * table's rows record as lost.
 */
/* lib.h is the umbrella, whose order of its modules' parts holds whichever header comes first. */
#include "doltgres/host/lib.h"

#ifndef DOLTGRES_HOST_SHARED_H
#define DOLTGRES_HOST_SHARED_H

#include "doltgres/std/atomic.h"
#include "doltgres/std/str.h"
#include "doltgres/std/tuple.h"

#define HOST_SHARED_BLOBS 4
#define HOST_SHARED_ARRAYS 4

/*
 * HostSharedLimits are the numbers Postgres fixes before the fork's shared
 * memory is sized, which a pool's count reads.
 */
typedef struct HostSharedLimits
{
	size_t		processes;
	size_t		delta_pages;
	size_t		relation_locks;
} HostSharedLimits;

/* HostSharedShapeOps is the Shape trait's constants: a record's id and its counts of each kind of part. */
typedef struct HostSharedShapeOps
{
	uint64		id;
	size_t		locks;
	size_t		flags;
	size_t		atomics;
	const size_t *blobs;
	size_t		blobs_len;
	const std_TupleUsizeUsize_t *arrays;
	size_t		arrays_len;
	size_t		lists;
} HostSharedShapeOps;

struct HostIoIo;
struct HostError;

/* HostSharedPoolOps is the Pool trait: a shape, its records' name, its count, and its reclaim. */
typedef struct HostSharedPoolOps
{
	HostSharedShapeOps shape;
	StdStr		name;
	size_t		(*count) (const HostSharedLimits *limits);
	bool		(*reclaim) (struct HostIoIo *io, bool *result, struct HostError *error);
} HostSharedPoolOps;

/* HostSharedAgedOps is the Aged trait: a shape, its records' name, the aged shapes they name, and its count. */
typedef struct HostSharedAgedOps
{
	HostSharedShapeOps shape;
	StdStr		name;
	const uint64 *names;
	size_t		names_len;
	size_t		(*count) (const HostSharedLimits *limits);
} HostSharedAgedOps;

/* HostSharedSingleOps is the Single trait: a shape and the count of its run of words. */
typedef struct HostSharedSingleOps
{
	HostSharedShapeOps shape;
	size_t		(*words) (const HostSharedLimits *limits);
} HostSharedSingleOps;

/* HostSharedProcSlotsOps is the ProcSlots trait: a slot of words for each process. */
typedef struct HostSharedProcSlotsOps
{
	uint64		id;
	size_t		words;
	bool		kept;
} HostSharedProcSlotsOps;

/*
 * HostSharedAt is the place of one of a process slot's words, below the
 * slot's count of words by construction: only At::of, a constant function,
 * makes one, so a place a caller computes is checked where it is made.
 */
typedef struct HostSharedAt
{
	size_t		index;
} HostSharedAt;

/* HostSharedFormTag is Form's variants: how the server keeps a kind. */
typedef enum HostSharedFormTag
{
	HOST_SHARED_FORM_POOL,
	HOST_SHARED_FORM_SINGLE,
	HOST_SHARED_FORM_AGED,
	HOST_SHARED_FORM_PROCESS,
} HostSharedFormTag;

/* HostSharedForm is how the server keeps a kind. */
typedef struct HostSharedForm
{
	HostSharedFormTag tag;
	union
	{
		size_t		(*pool) (const HostSharedLimits *limits);
		struct
		{
			size_t		(*run) (const HostSharedLimits *limits);
		}			single;
		struct
		{
			size_t		(*count) (const HostSharedLimits *limits);
			const uint64 *names;
			size_t		names_len;
		}			aged;
		struct
		{
			bool		kept;
		}			process;
	}			u;
} HostSharedForm;

/* HostSharedKind is a line of the server's table of kinds: a shape and how the server keeps it. */
typedef struct HostSharedKind
{
	uint64		id;
	size_t		locks;
	size_t		flags;
	size_t		atomics;
	const size_t *blobs;
	size_t		blobs_len;
	const std_TupleUsizeUsize_t *arrays;
	size_t		arrays_len;
	HostSharedForm form;
} HostSharedKind;

/* HostSharedOpTag is Op's variants: an operation on an atomic. */
typedef enum HostSharedOpTag
{
	HOST_SHARED_OP_LOAD,
	HOST_SHARED_OP_ADD,
	HOST_SHARED_OP_EXCHANGE,
	HOST_SHARED_OP_LOAD_ACQUIRE,
} HostSharedOpTag;

/*
 * HostSharedOp is an operation on an atomic: a load, an add, an exchange from
 * a value to another, or a load after every read before it.
 */
typedef struct HostSharedOp
{
	HostSharedOpTag tag;
	union
	{
		uint64		add;
		struct
		{
			uint64		f0;
			uint64		f1;
		}			exchange;
	}			u;
} HostSharedOp;

/* HostSharedLookedTag is Looked's variants. */
typedef enum HostSharedLookedTag
{
	HOST_SHARED_LOOKED_MINE,
	HOST_SHARED_LOOKED_THEIRS,
	HOST_SHARED_LOOKED_USED,
} HostSharedLookedTag;

/* HostSharedLooked is what a host's look at a record finds. */
typedef struct HostSharedLooked
{
	HostSharedLookedTag tag;
	union
	{
		uint64		mine;
	}			u;
} HostSharedLooked;

/* The module's handles and guards, and its functions, which take lib.h's types, follow its include. */
#include "doltgres/host/lib.h"

/*
 * HostSharedOwned is a pooled record one process owns, its maker or the one
 * claimant after it was published: only it frees the record. One dropped
 * without a free or a publish gives the record up at once: a record never
 * published is freed, and one claimed is published again.
 */
typedef struct HostSharedOwned
{
	HostHost	host;
	uint64		place;
} HostSharedOwned;

/* HostSharedHeld is a published record a process opened, which keeps its slot until the handle drops. */
typedef struct HostSharedHeld
{
	HostHost	host;
	uint64		place;
} HostSharedHeld;

/*
 * HostSharedReclaim is a published record no process held when a reclaim took
 * it to look at, which it frees only if no process opened the record since.
 */
typedef struct HostSharedReclaim
{
	HostHost	host;
	uint64		place;
} HostSharedReclaim;

/* HostSharedLookTag is Look's variants. */
typedef enum HostSharedLookTag
{
	HOST_SHARED_LOOK_MINE,
	HOST_SHARED_LOOK_THEIRS,
	HOST_SHARED_LOOK_USED,
} HostSharedLookTag;

/* HostSharedLook is what a reclaim's look at a record finds. */
typedef struct HostSharedLook
{
	HostSharedLookTag tag;
	union
	{
		HostSharedReclaim mine;
	}			u;
} HostSharedLook;

/*
 * HostSharedRecord is a record of a shape reached while the handle it came
 * from lives: its place, which every process reads the same way.
 */
typedef struct HostSharedRecord
{
	uint64		place;
} HostSharedRecord;

/*
 * HostSharedHeldPlace is the place of a record a hold keeps open, which
 * Held's place gives out and its caller uses only while that hold lives: its
 * adds take no lock, allocate nothing, and return nothing.
 */
typedef struct HostSharedHeldPlace
{
	uint64		place;
} HostSharedHeldPlace;

/* HostSharedHeldPlaceOption is an Option<HeldPlace>, which PlaceCell's get returns. */
typedef struct HostSharedHeldPlaceOption
{
	bool		has;
	HostSharedHeldPlace place;
} HostSharedHeldPlaceOption;

/*
 * HostSharedPlaceCell holds a HeldPlace, or none, which its process reads
 * with no lock and no result, in a commit's critical section or as it exits,
 * and uses only while the hold the place came from lives: one past the place,
 * zero for none.
 */
typedef struct HostSharedPlaceCell
{
	StdAtomicU64 place;
} HostSharedPlaceCell;

/*
 * HostSharedNamed is an atomic of a record that takes a page's name at the
 * page's link, the atomic's index in place of its marker.
 */
typedef struct HostSharedNamed
{
	HostSharedRecord record;
	size_t		atomic;
} HostSharedNamed;

/* HostSharedPast is an atomic of a record that takes a name at a link one past it, so that zero names nothing. */
typedef struct HostSharedPast
{
	HostSharedNamed named;
} HostSharedPast;

/* HostSharedRunWord is a word of a single record's run, an atomic that takes a page's name at the page's link. */
typedef struct HostSharedRunWord
{
	HostSharedRecord record;
	/* atomic is the word's index among the record's atomics. */
	size_t		atomic;
} HostSharedRunWord;

/* HostSharedGuard is a held lock under which no file I/O runs, in a mode, which lends its taker's lock scope. */
typedef struct HostSharedGuard
{
	HostHost	host;
	uint64		place;

	/*
	 * index is the lock's place among the record's locks, which the guard
	 * releases.
	 */
	size_t		index;
	HostIoHeld *held;
	StdAny		raise;
} HostSharedGuard;

/*
 * HostSharedLongGuard is a lock held across I/O, in a mode, which lends a
 * capability that does file I/O but never waits.
 */
typedef struct HostSharedLongGuard
{
	HostHost	host;
	uint64		place;

	/*
	 * index is the lock's place among the record's locks, which the guard
	 * releases.
	 */
	size_t		index;
	HostIoHeld *held;
	StdAny		raise;
} HostSharedLongGuard;

/*
 * HostSharedLongIo is a long lock's capability: it does file I/O and takes
 * later locks of the same record, short and long, but never waits on a flag.
 */
typedef struct HostSharedLongIo
{
	uint64		place;
	HostIoHeld *held;
	StdAny		raise;
} HostSharedLongIo;

/* HostSharedFlagGuard is a set flag, which lends a capability for the long operation it marks. */
typedef struct HostSharedFlagGuard
{
	HostHost	host;
	uint64		place;

	/*
	 * index is the flag's place among the record's flags, which the guard
	 * clears.
	 */
	size_t		index;
	HostIoHeld *held;
	StdAny		raise;
} HostSharedFlagGuard;

/*
 * HostSharedFlagIo is a flag's capability: it does file I/O, checks
 * interrupts, takes later flags, and takes any lock of the same record, but
 * lends no raw Io.
 */
typedef struct HostSharedFlagIo
{
	uint64		place;
	HostIoHeld *held;
	StdAny		raise;
} HostSharedFlagIo;

/* HostSharedEpochs is the fork-wide epoch lock, held exclusively, never with a record's lock. */
typedef struct HostSharedEpochs
{
	HostHost	host;
} HostSharedEpochs;

/* HostSharedReader is a reader's epoch, published until it drops, which keeps what the reader may see from going. */
typedef struct HostSharedReader
{
	HostHost	host;
	uint64		epoch;
} HostSharedReader;

/* HostSharedMine is the running process's handle of its own slot of words, which stays with the process that got it. */
typedef struct HostSharedMine
{
	HostHost	host;
	uint64		place;
} HostSharedMine;

/*
 * HostSharedTheirs is another process's slot of a kind of words, found once,
 * so a store into one of its words can't fail.
 */
typedef struct HostSharedTheirs
{
	/* host is the host whose process the slot is of. */
	HostHost	host;
	uint64		place;
} HostSharedTheirs;

/*
 * HostSharedListed is a kind of words with its position in the server's table
 * of kinds, which only at makes, in a constant.
 */
typedef struct HostSharedListed
{
	size_t		index;
} HostSharedListed;

/* HostSharedWithinRaisesOps is within::Raises' table: the capability of pg's a view borrows. */
typedef struct HostSharedWithinRaisesOps
{
	StdAny		(*raise) (void *self);
} HostSharedWithinRaisesOps;

/* HostSharedWithinOps is Within's table, its supertrait's alone. */
typedef struct HostSharedWithinOps
{
	HostSharedWithinRaisesOps raises;
} HostSharedWithinOps;

/* HostSharedWithin is a view that implements Within. */
typedef struct HostSharedWithin
{
	const HostSharedWithinOps *ops;
	void	   *self;
} HostSharedWithin;

/* HostSharedPausesOps is Pauses' table, its supertrait's alone. */
typedef struct HostSharedPausesOps
{
	HostSharedWithinRaisesOps raises;
} HostSharedPausesOps;

/* HostSharedPauses is a view that implements Pauses. */
typedef struct HostSharedPauses
{
	const HostSharedPausesOps *ops;
	void	   *self;
} HostSharedPauses;

/* Each view's tables of Within and Pauses. */
extern const HostSharedWithinOps HOST_SHARED_IO_WITHIN_OPS;
extern const HostSharedWithinOps HOST_SHARED_GUARD_WITHIN_OPS;
extern const HostSharedWithinOps HOST_SHARED_LONG_GUARD_WITHIN_OPS;
extern const HostSharedPausesOps HOST_SHARED_IO_PAUSES_OPS;
extern const HostSharedPausesOps HOST_SHARED_REGISTERING_PAUSES_OPS;
extern const HostSharedPausesOps HOST_SHARED_KEY_IO_PAUSES_OPS;
extern const HostSharedPausesOps HOST_SHARED_LONG_IO_PAUSES_OPS;
extern const HostSharedPausesOps HOST_SHARED_FLAG_IO_PAUSES_OPS;
extern const HostSharedPausesOps HOST_SHARED_GUARD_PAUSES_OPS;
extern const HostSharedPausesOps HOST_SHARED_LONG_GUARD_PAUSES_OPS;
extern const HostIoIoDoorsOps HOST_SHARED_LONG_IO_IO_DOORS_OPS;
extern const HostIoIoDoorsOps HOST_SHARED_FLAG_IO_IO_DOORS_OPS;

/* The coercions of a view to a trait it implements, which Rust makes implicitly. */
#define HOST_SHARED_IO_WITHIN(io) ((HostSharedWithin) {&HOST_SHARED_IO_WITHIN_OPS, (io)})
#define HOST_SHARED_GUARD_WITHIN(g) ((HostSharedWithin) {&HOST_SHARED_GUARD_WITHIN_OPS, (g)})
#define HOST_SHARED_LONG_GUARD_WITHIN(g) ((HostSharedWithin) {&HOST_SHARED_LONG_GUARD_WITHIN_OPS, (g)})
#define HOST_SHARED_IO_PAUSES(io) ((HostSharedPauses) {&HOST_SHARED_IO_PAUSES_OPS, (io)})
#define HOST_SHARED_REGISTERING_PAUSES(r) ((HostSharedPauses) {&HOST_SHARED_REGISTERING_PAUSES_OPS, (r)})
#define HOST_SHARED_KEY_IO_PAUSES(k) ((HostSharedPauses) {&HOST_SHARED_KEY_IO_PAUSES_OPS, (k)})
#define HOST_SHARED_LONG_IO_PAUSES(l) ((HostSharedPauses) {&HOST_SHARED_LONG_IO_PAUSES_OPS, (l)})
#define HOST_SHARED_FLAG_IO_PAUSES(f) ((HostSharedPauses) {&HOST_SHARED_FLAG_IO_PAUSES_OPS, (f)})
#define HOST_SHARED_GUARD_PAUSES(g) ((HostSharedPauses) {&HOST_SHARED_GUARD_PAUSES_OPS, (g)})
#define HOST_SHARED_LONG_GUARD_PAUSES(g) ((HostSharedPauses) {&HOST_SHARED_LONG_GUARD_PAUSES_OPS, (g)})
#define HOST_SHARED_LONG_IO_IO_DOORS(l) ((HostIoIoDoors) {&HOST_SHARED_LONG_IO_IO_DOORS_OPS, (l)})
#define HOST_SHARED_FLAG_IO_IO_DOORS(f) ((HostIoIoDoors) {&HOST_SHARED_FLAG_IO_IO_DOORS_OPS, (f)})

extern bool host_pause(HostSharedPauses at, const char *point, HostError *error);
extern bool host_injected(HostIoIoDoors io, const char *point, bool *result, HostError *error);
extern size_t host_shared_Single_words(const HostSharedLimits *limits);
extern HostSharedKind host_shared_Kind_pool(const HostSharedPoolOps *s);
extern HostSharedKind host_shared_Kind_aged(const HostSharedAgedOps *s);
extern HostSharedKind host_shared_Kind_single(const HostSharedSingleOps *s);
extern HostSharedKind host_shared_Kind_slots(const HostSharedProcSlotsOps *p);
extern bool host_shared_ordered(const HostSharedKind *kinds, size_t kinds_len);
extern void host_shared_array(size_t entry_size, size_t count, size_t *out_0, size_t *out_1);
extern bool host_shared_distinct(const size_t *indices, size_t indices_len, size_t count);
extern bool host_shared_make(const HostSharedPoolOps *s, HostIoIo *io, HostSharedOwned *result,
							 HostError *error);
extern bool host_shared_claim(HostSharedWithin within, const HostSharedPoolOps *s, uint64 location,
							  bool *has_result, HostSharedOwned *result, HostError *error);
extern void host_shared_free(HostSharedOwned *owned);
extern HostSharedRecord host_shared_Owned_record(const HostSharedOwned *self);
extern uint64 host_shared_Owned_publish(HostSharedOwned *self);
extern void host_shared_Owned_drop(HostSharedOwned *self);
extern StdAny host_shared_Io_Raises_raise(void *self);
extern StdAny host_shared_Registering_Raises_raise(void *self);
extern StdAny host_shared_Guard_Raises_raise(void *self);
extern StdAny host_shared_LongGuard_Raises_raise(void *self);
extern StdAny host_shared_KeyIo_Raises_raise(void *self);
extern StdAny host_shared_LongIo_Raises_raise(void *self);
extern StdAny host_shared_FlagIo_Raises_raise(void *self);
#if defined(USE_INJECTION_POINTS) || defined(DOLTGRES_HOST_TESTING)
extern StdAny host_shared_raising(HostSharedPauses at);
#endif
extern bool host_shared_open(HostSharedWithin within, const HostSharedPoolOps *s, uint64 location,
							 bool *has_result, HostSharedHeld *result, HostError *error);
extern HostSharedRecord host_shared_Held_record(const HostSharedHeld *self);
extern HostSharedHeldPlace host_shared_Held_place(const HostSharedHeld *self);
extern HostSharedHeldPlace host_shared_HeldPlace_Clone_clone(const HostSharedHeldPlace *self);
extern void host_shared_HeldPlace_add(HostSharedHeldPlace self, size_t a, uint64 value);
extern uint64 host_shared_HeldPlace_exchange(HostSharedHeldPlace self, size_t a, uint64 old, uint64 new_);
extern void host_shared_PlaceCell_Default_default(HostSharedPlaceCell *result);
extern void host_shared_PlaceCell_set(HostSharedPlaceCell *self, bool has_place, HostSharedHeldPlace place);
extern HostSharedHeldPlaceOption host_shared_PlaceCell_get(HostSharedPlaceCell *self);
extern void host_shared_Held_drop(HostSharedHeld *self);
extern bool host_shared_reclaim(HostSharedWithin within, const HostSharedPoolOps *s, uint64 location,
								HostSharedLook *result, HostError *error);
extern HostSharedRecord host_shared_Reclaim_record(const HostSharedReclaim *self);
extern bool host_shared_Reclaim_free(HostSharedReclaim *self);
extern void host_shared_Reclaim_drop(HostSharedReclaim *self);
extern void host_shared_Look_drop(HostSharedLook *self);
extern bool host_shared_sweep(HostSharedWithin within, const HostSharedPoolOps *s, size_t a,
							  StdVec *result, HostError *error);
extern bool host_shared_gone(const HostSharedPoolOps *s, uint64 location, bool *result,
							 HostError *error);
extern bool host_shared_ready(void);
extern bool host_shared_single(const HostSharedSingleOps *s, HostSharedRecord *result,
							   HostError *error);
extern HostSharedRecord host_shared_Record_at(uint64 place);
extern bool host_shared_Record_lock(HostSharedRecord self, HostIoTakesShort io, size_t l,
									HostSharedGuard *result, HostError *error);
extern bool host_shared_Record_lock_shared(HostSharedRecord self, HostIoTakesShort io, size_t l,
										   HostSharedGuard *result, HostError *error);
extern bool host_shared_Record_lock_long(HostSharedRecord self, HostIoTakesShort io, size_t l,
										 HostSharedLongGuard *result, HostError *error);
extern bool host_shared_Record_lock_long_shared(HostSharedRecord self, HostIoIo *io, size_t l,
												HostSharedLongGuard *result, HostError *error);
extern bool host_shared_Record_flag(HostSharedRecord self, HostIoTakesShort io, size_t f,
									HostSharedFlagGuard *result, HostError *error);
extern uint64 host_shared_Record_atomic(HostSharedRecord self, size_t a, HostSharedOp op);
extern bool host_shared_Record_atomic_in(HostSharedRecord self, size_t a_first, size_t a_count,
										 size_t number, HostSharedOp op, uint64 *result,
										 HostError *error);
extern HostSharedNamed host_shared_Record_names(HostSharedRecord self, size_t a);
extern HostSharedPast host_shared_Record_names_past(HostSharedRecord self, size_t a);
extern void host_shared_Named_store(const HostSharedNamed *self, uint64 value);
extern void host_shared_Past_store(const HostSharedPast *self, uint64 name);
extern bool host_shared_Past_load(const HostSharedPast *self, uint64 *result);
extern bool host_shared_Past_clear(const HostSharedPast *self, uint64 name);
extern void host_shared_stored(uint64 place, size_t atomic, uint64 value);
extern bool host_shared_Record_word(HostSharedRecord self, const HostSharedSingleOps *s, size_t index,
									HostSharedRunWord *result, HostError *error);
extern uint64 host_shared_RunWord_load(const HostSharedRunWord *self);
extern void host_shared_RunWord_store(const HostSharedRunWord *self, uint64 value);
extern uint64 host_shared_RunWord_compare_exchange(const HostSharedRunWord *self, uint64 expected,
												   uint64 value);
extern uint64 host_shared_RunWord_fetch_add(const HostSharedRunWord *self, uint64 value);
extern bool host_shared_Record_mark(HostSharedRecord self, size_t i, HostSharedLongGuard *barrier,
									bool *result, HostError *error);
extern bool host_shared_Record_named(HostSharedRecord self, size_t i, uint64 *result);
extern bool host_shared_Record_unmark(HostSharedRecord self, size_t i,
									  HostSharedLongGuard *barrier, HostError *error);
extern bool host_shared_Record_read(HostSharedRecord self, size_t i, HostSharedLongGuard *barrier,
									StdVec *result, HostError *error);
extern bool host_shared_Record_claim(HostSharedRecord self, size_t i,
									 const HostSharedLongGuard *barrier, const uint8 *bytes,
									 size_t bytes_len, bool *result, HostError *error);
extern HostIoHeld *host_shared_Guard_held(HostSharedGuard *self);
extern uint64 host_shared_Guard_place(const HostSharedGuard *self);
extern HostSharedRecord host_shared_Guard_record(const HostSharedGuard *self);
extern bool host_shared_Guard_lock(HostSharedGuard *self, size_t n, HostSharedGuard *result,
								   HostError *error);
extern bool host_shared_Guard_lock_shared(HostSharedGuard *self, size_t n,
										  HostSharedGuard *result, HostError *error);
extern StdVec host_shared_Guard_read_blob(HostSharedGuard *self, size_t i);
extern bool host_shared_Guard_entry(HostSharedGuard *self, size_t i, size_t index,
									StdVec *result, HostError *error);
extern bool host_shared_GuardExclusive_write_blob(HostSharedGuard *self, size_t i, const uint8 *bytes,
												  size_t bytes_len, HostError *error);
extern bool host_shared_GuardExclusive_set_entry(HostSharedGuard *self, size_t i, size_t index,
												 const uint8 *bytes, size_t bytes_len, HostError *error);
extern void host_shared_Guard_drop(HostSharedGuard *self);
extern HostSharedLongIo host_shared_LongGuard_io(HostSharedLongGuard *self);
extern StdVec host_shared_LongGuard_read_blob(HostSharedLongGuard *self, size_t i);
extern bool host_shared_LongGuard_write_blob(HostSharedLongGuard *self, size_t i,
											 const uint8 *bytes, size_t bytes_len,
											 HostError *error);
extern void host_shared_LongGuard_drop(HostSharedLongGuard *self);
extern bool host_shared_LongIo_lock(HostSharedLongIo *self, size_t n, HostSharedGuard *result,
									HostError *error);
extern bool host_shared_LongIo_lock_shared(HostSharedLongIo *self, size_t n,
										   HostSharedGuard *result, HostError *error);
extern bool host_shared_LongIo_lock_long(HostSharedLongIo *self, size_t n,
										 HostSharedLongGuard *result, HostError *error);
extern bool host_shared_LongIo_lock_long_shared(HostSharedLongIo *self, size_t n,
												HostSharedLongGuard *result, HostError *error);
extern HostSharedFlagIo host_shared_FlagGuard_io(HostSharedFlagGuard *self);
extern void host_shared_FlagGuard_drop(HostSharedFlagGuard *self);
extern bool host_shared_FlagIo_check(HostSharedFlagIo *self, HostError *error);
extern const void *host_shared_FlagIo_Deref_deref(const void *self);
extern void *host_shared_FlagIo_DerefMut_deref_mut(void *self);
extern void host_shared_FlagIo_Sealed_parts(void *self, HostIoHeld **out_0, StdAny *out_1);
extern bool host_shared_FlagIo_flag(HostSharedFlagIo *self, size_t g, HostSharedFlagGuard *result,
									HostError *error);
extern bool host_shared_FlagIo_lock(HostSharedFlagIo *self, size_t l, HostSharedGuard *result,
									HostError *error);
extern bool host_shared_FlagIo_lock_shared(HostSharedFlagIo *self, size_t l,
										   HostSharedGuard *result, HostError *error);
extern bool host_shared_FlagIo_lock_long(HostSharedFlagIo *self, size_t l,
										 HostSharedLongGuard *result, HostError *error);
extern bool host_shared_epochs(HostIoIo *io, HostSharedEpochs *result, HostError *error);
extern uint64 host_shared_Epochs_advance(HostSharedEpochs *self);
extern uint64 host_shared_Epochs_oldest(HostSharedEpochs *self);
extern void host_shared_Epochs_drop(HostSharedEpochs *self);
extern bool host_shared_read(HostIoIo *io, HostSharedReader *result, HostError *error);
extern uint64 host_shared_Reader_epoch(const HostSharedReader *self);
extern void host_shared_Reader_drop(HostSharedReader *self);
extern uint32 host_shared_proc_number(void);
extern bool host_shared_mine(HostIoTakesShort io, const HostSharedProcSlotsOps *p,
							 HostSharedMine *result, HostError *error);
extern void host_shared_Mine_store(const HostSharedMine *self, size_t w, uint64 value);
extern uint64 host_shared_Mine_load(const HostSharedMine *self, size_t w);
extern void host_shared_Mine_store_at(const HostSharedMine *self, HostSharedAt at, uint64 value);
extern uint64 host_shared_Mine_load_at(const HostSharedMine *self, HostSharedAt at);
extern uint64 host_shared_Mine_exchange(const HostSharedMine *self, size_t w, uint64 loaded, uint64 value);
extern uint64 host_shared_Mine_id(const HostSharedMine *self);
extern void host_shared_store_own(const HostSharedProcSlotsOps *p, const HostSharedListed *listed,
								  size_t w, uint64 value);
extern bool host_shared_processes(const HostSharedProcSlotsOps *p, StdVec *result,
								  HostError *error);
extern bool host_shared_load(const HostSharedProcSlotsOps *p, size_t w, uint32 proc, uint64 *result,
							 HostError *error);
extern bool host_shared_load_at(const HostSharedProcSlotsOps *p, uint32 proc, HostSharedAt at, uint64 *result,
								HostError *error);
extern bool host_shared_theirs(const HostSharedProcSlotsOps *p, uint32 proc, HostSharedTheirs *result,
							   HostError *error);
extern uint64 host_shared_Theirs_exchange(const HostSharedTheirs *self, size_t w, uint64 loaded, uint64 value);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_shared_tests_a_table_lists_an_aged_kind_before_what_it_names(void);
#endif

#endif							/* DOLTGRES_HOST_SHARED_H */
