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
 * The fork's table scans (doltgres' scan.rs, design item 13, the access
 * map's E): each an owned record a scan's descriptor names by its handle,
 * reading a storage's base through trees' cursor, kept between the calls
 * that read its rows, the base's entries and the delta's row versions
 * merged in the table's order, and a comparator's comparisons made with no
 * record lent, since they run user code.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/access/transform.h"
#include "doltgres/doltgres/scan.h"
#include "doltgres/host/delta.h"
#include "doltgres/pghost/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/entry.h"
#include "doltgres/txn/memtable/slice.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/state.h"

#ifdef DOLTGRES_TEST_DOLTGRES
#include "doltgres/repo/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/test.h"
#include "doltgres/trees/build.h"
#include "doltgres/txn/change.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/insert.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/written.h"
#endif

#define RUST_FILE "doltgres" STD_SEP "src" STD_SEP "scan.rs"

/* The element layouts of the scan's vectors. */
#define DOLTGRES_SCAN_BYTES 1, 1
#define DOLTGRES_SCAN_VEC sizeof(StdVec), _Alignof(StdVec)
#define DOLTGRES_SCAN_U32 sizeof(uint32), _Alignof(uint32)
#define DOLTGRES_SCAN_U64 sizeof(uint64), _Alignof(uint64)
#define DOLTGRES_SCAN_STAMP sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp)
#define DOLTGRES_SCAN_LIVE sizeof(TxnVisibleLive), _Alignof(TxnVisibleLive)
#define DOLTGRES_SCAN_COMPOSED sizeof(doltgres_OptionVecU8_t), _Alignof(doltgres_OptionVecU8_t)
#define DOLTGRES_SCAN_TRANSFORM sizeof(AccessTransformTransform), _Alignof(AccessTransformTransform)
#define DOLTGRES_SCAN_ROW sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow)
#define DOLTGRES_SCAN_FOUND sizeof(DoltgresScanFound), _Alignof(DoltgresScanFound)
#define DOLTGRES_SCAN_XID sizeof(HostXid), _Alignof(HostXid)

/*
 * DoltgresScanFound is Found: a row of a key's group, its tuple, none for a
 * copy of the base entry's, its values' ids, its read's transform, its row
 * stamp, its writer's top-level xid and command, its changer, and what a
 * Dirty read found running.
 */
typedef struct DoltgresScanFound
{
	bool		has_tuple;
	StdVec		tuple;
	StdVec		values;
	StdVec		transform;
	ChunksTrailerStamp stamp;
	uint64		top;
	uint32		command;
	bool		has_changer;
	DoltgresScanChanger changer;
	TxnVisibleDirty dirty;
} DoltgresScanFound;

/*
 * DoltgresScanGroup is Group: the rows a scan's snapshot sees of one key, a
 * Vec<Found> in a forward read's order, with the key, the base entry's
 * tuple, and the row the last read gave, none before the group's first read.
 */
typedef struct DoltgresScanGroup
{
	StdVec		key;
	StdVec		base;
	StdVec		rows;
	bool		has_at;
	size_t		at;
} DoltgresScanGroup;

/* DoltgresScanPlaceTag is Place's variants. */
typedef enum DoltgresScanPlaceTag
{
	DOLTGRES_SCAN_PLACE_START,
	DOLTGRES_SCAN_PLACE_AT,
	DOLTGRES_SCAN_PLACE_OFF,
} DoltgresScanPlaceTag;

/*
 * DoltgresScanPlace is Place: where a scan is in its base, not begun, off
 * its end one way, or at an entry the last advance reached going a way,
 * with whether that entry joined the group it made.
 */
typedef struct DoltgresScanPlace
{
	DoltgresScanPlaceTag tag;
	union
	{
		struct
		{
			TreesCursorKeptCursor cursor;
			DoltgresScanDirection way;
			bool		taken;
		}			at;
		DoltgresScanDirection off;
	}			u;
} DoltgresScanPlace;

/*
 * DoltgresScanRun is Run: a compared table's delta keys, a Vec<Vec<u8>> in
 * the table's order, and how many of them are at or before the scan's place.
 */
typedef struct DoltgresScanRun
{
	StdVec		keys;
	size_t		split;
} DoltgresScanRun;

/*
 * DoltgresScanCopies is Copies: a base entry's rows copied out of its leaf,
 * its tuple, its copies' row stamps (a Vec<Stamp>), and its value ids (a
 * Vec<u32>).
 */
typedef struct DoltgresScanCopies
{
	StdVec		tuple;
	StdVec		stamps;
	StdVec		values;
} DoltgresScanCopies;

/* DoltgresScanTurnTag is Turn's variants. */
typedef enum DoltgresScanTurnTag
{
	DOLTGRES_SCAN_TURN_ROW,
	DOLTGRES_SCAN_TURN_AGAIN,
	DOLTGRES_SCAN_TURN_END,
	DOLTGRES_SCAN_TURN_RUN,
	DOLTGRES_SCAN_TURN_COMPARE,
	DOLTGRES_SCAN_TURN_MOVED,
} DoltgresScanTurnTag;

/*
 * DoltgresScanTurn is Turn: what a turn of a read in the scan's record
 * leaves it, a row, another turn, the end, a compared table's run to make
 * by its order, two candidates to compare by it, or an install's move of the
 * storage's base during the turn's read of the delta, which changed nothing
 * in the record: the scan moves to the newest base (reseated) and takes the
 * turn again.
 */
typedef struct DoltgresScanTurn
{
	DoltgresScanTurnTag tag;
	union
	{
		DoltgresScanRow row;
		TreesKeyOrder run;
		struct
		{
			StdVec		base_key;
			DoltgresScanCopies base_copies;
			StdVec		delta_key;
			size_t		delta_at;
			TreesKeyOrder order;
		}			compare;
	}			u;
} DoltgresScanTurn;

/*
 * DoltgresScanBounds is Bounds: what a parallel unit's delta keys are read
 * between, its low bound, its high bound, the table's order, and whether
 * the unit reads every delta key alone.
 */
typedef struct DoltgresScanBounds
{
	doltgres_OptionVecU8_t low;
	doltgres_OptionVecU8_t high;
	TreesKeyOrder order;
	bool		alone;
} DoltgresScanBounds;

/* DoltgresScanPickedTag is Picked's variants. */
typedef enum DoltgresScanPickedTag
{
	DOLTGRES_SCAN_PICKED_BASE,
	DOLTGRES_SCAN_PICKED_DELTA,
	DOLTGRES_SCAN_PICKED_BOTH,
} DoltgresScanPickedTag;

/*
 * DoltgresScanPicked is Picked: a compared table's next group's candidates
 * as chosen, the base's entry alone with the place in the run of the delta's
 * key it came before, if any, the delta's key alone, or both.
 */
typedef struct DoltgresScanPicked
{
	DoltgresScanPickedTag tag;
	/* base_key and base_copies are the base's entry, for Base and Both. */
	StdVec		base_key;
	DoltgresScanCopies base_copies;
	bool		has_before;
	size_t		before;

	/*
	 * delta_key and delta_at are the delta's key and its place in the run,
	 * for Delta and Both.
	 */
	StdVec		delta_key;
	size_t		delta_at;
} DoltgresScanPicked;

/* DoltgresScanSampled is Sampled: a sample scan's block and its rows the snapshot sees, a Vec<Row>. */
typedef struct DoltgresScanSampled
{
	uint32		block;
	StdVec		rows;
} DoltgresScanSampled;

/* DoltgresScanPageTag is Page's variants. */
typedef enum DoltgresScanPageTag
{
	DOLTGRES_SCAN_PAGE_EXACT,
	DOLTGRES_SCAN_PAGE_LOSSY,
	DOLTGRES_SCAN_PAGE_ENDED,
} DoltgresScanPageTag;

/*
 * DoltgresScanPage is Page: where a bitmap scan is in its page, the tuple
 * ids left of an exact page (a Vec<u64> taken from its front), a lossy
 * page, or ended.
 */
typedef struct DoltgresScanPage
{
	DoltgresScanPageTag tag;
	StdVec		tuple_ids;
	size_t		front;
} DoltgresScanPage;

/*
 * DoltgresScanUnit is Unit: a parallel scan's unit, its number and its
 * entries' first ordinal and the one past its last.
 */
typedef struct DoltgresScanUnit
{
	uint64		number;
	uint64		first;
	uint64		end;
} DoltgresScanUnit;

/*
 * DoltgresScanParallel is Parallel: a parallel scan's part of its record,
 * the base's count of entries, the count of units, the unit being read, a
 * tid range unit's first and last tuple ids, the delta's split keys (a
 * Vec<Vec<u8>>), the table tree of the base the leader shared, whose
 * entries' ordinals every worker's units split whatever newer base a worker
 * moved to (rebased), and the unit's low and high keys (bounds), which a
 * worker on a newer base than the shared one finds its unit's place and stop
 * in.
 */
typedef struct DoltgresScanParallel
{
	uint64		entries;
	uint64		units;
	bool		has_unit;
	DoltgresScanUnit unit;
	bool		has_ranged;
	uint64		ranged_first;
	uint64		ranged_last;
	StdVec		splits;
	bool		has_shared;
	TreesStoreTree shared;
	doltgres_OptionVecU8_t low;
	doltgres_OptionVecU8_t high;

	/*
	 * kept is the delta's run in the table's order (a Vec<Vec<u8>>), read at
	 * the scan's generation kept_at, which each unit takes its keys from
	 * (bugs row 309).
	 */
	bool		has_kept;
	StdVec		kept;
	uint64		kept_at;
} DoltgresScanParallel;

/*
 * DoltgresScanPaired is Paired: a tuple id and its row's key, with the
 * table's tree, keyed layout, object id, tree's order, and a base row's
 * transform.
 */
typedef struct DoltgresScanPaired
{
	uint64		tuple_id;
	StdVec		key;
	bool		has_root;
	TreesStoreTree root;
	StdVec		keyed;
	uint32		table;
	TreesKeyOrder order;
	StdVec		transform;

	/*
	 * resolved is the memo of the fetch's scan, or its call, which the pair
	 * doesn't own.
	 */
	DoltgresScanResolved *resolved;
} DoltgresScanPaired;

/* DoltgresScanScan is Scan: a table scan's record, its fields as the Rust's, in its order. */
struct DoltgresScanScan
{
	HostSharedReader reader;
	bool		has_group;
	DoltgresScanGroup group;
	DoltgresScanPlace place;
	bool		has_run;
	DoltgresScanRun run;
	/* written is the delta's sorted tuple ids, a Vec<u64>. */
	bool		has_written;
	StdVec		written;
	bool		has_given;
	uint64		given;
	bool		has_tree;
	TreesStoreTree tree;
	uint64		folded;
	DoltgresScanEpochs epochs;

	/*
	 * resolved is the memo of the base leaf the scan's fetches last resolved
	 * a row in.
	 */
	DoltgresScanResolved resolved;
	/* generation is the base generation the scan's base was read at. */
	uint64		generation;
	uint64		blocks;
	StdVec		keyed;
	uint32		table;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	TreesKeyOrder order;
	bool		has_range;
	uint64		range_first;
	uint64		range_last;
	doltgres_OptionVecU8_t bound;
	doltgres_OptionVecU8_t seek;
	bool		index;
	bool		records;
	bool		has_parallel;
	DoltgresScanParallel parallel;
	DoltgresScanPage page;
	bool		has_sample;
	DoltgresScanSampled sample;
};

/* doltgres_scan_vecs_drop frees a Vec<Vec<u8>>. */
static void
doltgres_scan_vecs_drop(StdVec *vecs)
{
	for (size_t i = 0; i < vecs->len; i++)
		std_vec_free_Checked(&STD_VEC_AT(vecs, StdVec, i), DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(vecs, DOLTGRES_SCAN_VEC);
}

/* doltgres_scan_option_drop frees an Option<Vec<u8>>'s bytes. */
static void
doltgres_scan_option_drop(doltgres_OptionVecU8_t *self)
{
	if (self->has)
		std_vec_free_Checked(&self->v, DOLTGRES_SCAN_BYTES);
	self->has = false;
}

/* doltgres_scan_Live_drop is Live's drop glue: its pending transform's bytes freed. */
static void
doltgres_scan_Live_drop(void *live)
{
	txn_base_Pending_drop(&((TxnVisibleLive *) live)->f0);
}

/* doltgres_scan_composed_drop is Option<Vec<u8>>'s drop glue, a composed transform's. */
static void
doltgres_scan_composed_drop(void *composed)
{
	doltgres_scan_option_drop(composed);
}

/* doltgres_scan_Epochs_Default_default is Epochs' derived Default: epoch 0 with no chain. */
void
doltgres_scan_Epochs_Default_default(DoltgresScanEpochs *result)
{
	*result = (DoltgresScanEpochs)
	{
		0
	};
	std_vec_new(&result->chain, DOLTGRES_SCAN_LIVE);
	std_vec_new(&result->composed, DOLTGRES_SCAN_COMPOSED);
}

/* doltgres_scan_Epochs_drop frees the epochs' chain and composed transforms. */
void
doltgres_scan_Epochs_drop(DoltgresScanEpochs *self)
{
	std_ptr_drop_in_place_slice(self->chain.ptr, self->chain.len, sizeof(TxnVisibleLive), doltgres_scan_Live_drop);
	std_vec_free_Checked(&self->chain, DOLTGRES_SCAN_LIVE);
	std_ptr_drop_in_place_slice(self->composed.ptr, self->composed.len, sizeof(doltgres_OptionVecU8_t),
								doltgres_scan_composed_drop);
	std_vec_free_Checked(&self->composed, DOLTGRES_SCAN_COMPOSED);
}

/*
 * doltgres_scan_Epochs_of answers the epochs of a storage's base by its
 * epoch and its chain tree, which its live pending transforms are read from;
 * a storage with no base has epoch 0 and no chain.
 */
bool
doltgres_scan_Epochs_of(HostIoIo *io, TreesStoreNodeStore store, uint32 epoch, bool has_chain, TreesStoreTree chain,
						DoltgresScanEpochs *result, HostError *error)
{
	StdVec		pending;

	if (!txn_base_chain_of(io, store, has_chain, chain, &pending, error))
		return false;
	*result = (DoltgresScanEpochs)
	{
		.base = epoch
	};
	if (!txn_visible_live(io, &pending, &result->chain, error))
		return false;
	std_vec_new(&result->composed, DOLTGRES_SCAN_COMPOSED);
	return true;
}

/* doltgres_scan_internal returns an internal error of the fork's with a literal message. */
static HostError
doltgres_scan_internal(const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return host_Error_fork((const uint8 *) "XX000", &text);
}

/*
 * doltgres_scan_Epochs_moved takes the epochs of a newer base a read moved to
 * (rebased, Trees_newer), which it takes, keeping the reader's epoch and its
 * layout's check; a newer base whose epoch or chain doesn't reach the
 * reader's epoch is an internal error, as a row past it is.
 */
static bool
doltgres_scan_Epochs_moved(DoltgresScanEpochs *self, DoltgresScanEpochs *newer, HostError *error)
{
	size_t		end = 0;

	if (self->has_end)
	{
		size_t		reached = (size_t) self->base + self->end;

		if (reached < newer->base || reached - newer->base > newer->chain.len)
		{
			doltgres_scan_Epochs_drop(newer);
			*error = doltgres_scan_internal("a newer base is past its reader's epoch");
			return false;
		}
		end = reached - newer->base;
	}
	newer->has_end = self->has_end;
	newer->end = end;
	newer->checked = self->checked;
	doltgres_scan_Epochs_drop(self);
	*self = *newer;
	return true;
}

/* doltgres_scan_Epochs_based answers a base's epoch and chain tree, which of reads, epoch 0 and no chain for none. */
void
doltgres_scan_Epochs_based(const TxnBaseBase *base, uint32 *result_0, bool *has_result_1, TreesStoreTree *result_1)
{
	*result_0 = base == NULL ? 0 : base->epoch;
	*has_result_1 = base != NULL && base->has_chain;
	if (*has_result_1)
		*result_1 = base->chain;
}

/*
 * doltgres_scan_Epochs_transform_frame_t is transform's state across its
 * pad: the epochs, the start of the transforms it composes and their end,
 * the decoded transforms (a Vec<Transform>), the composed one, and the
 * encoded result, each while live.
 */
typedef struct doltgres_scan_Epochs_transform_frame_t
{
	DoltgresScanEpochs *self;
	size_t		from;
	size_t		end;
	StdVec		decoded;
	bool		decoded_live;
	bool		has_composed;
	AccessTransformTransform composed;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_scan_Epochs_transform_frame_t;

/* doltgres_scan_Transform_drop is Transform's drop glue, an element of a Vec<Transform>'s. */
static void
doltgres_scan_Transform_drop(void *transform)
{
	access_transform_Transform_drop(transform);
}

/* doltgres_scan_Epochs_transform_pad frees what an unwound composition held. */
static void
doltgres_scan_Epochs_transform_pad(void *frame_)
{
	doltgres_scan_Epochs_transform_frame_t *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(frame->result, DOLTGRES_SCAN_BYTES);
	frame->result_live = false;
	if (frame->has_composed)
		access_transform_Transform_drop(&frame->composed);
	frame->has_composed = false;
	if (frame->decoded_live)
	{
		std_ptr_drop_in_place_slice(frame->decoded.ptr, frame->decoded.len, sizeof(AccessTransformTransform),
									doltgres_scan_Transform_drop);
		std_vec_free_Checked(&frame->decoded, DOLTGRES_SCAN_TRANSFORM);
	}
	frame->decoded_live = false;
}

/*
 * doltgres_scan_Epochs_transform_body decodes the pending transforms past the
 * epoch, composes them, and keeps the result.
 */
static void
doltgres_scan_Epochs_transform_body(void *frame_)
{
	doltgres_scan_Epochs_transform_frame_t *frame = frame_;
	DoltgresScanEpochs *self = frame->self;
	doltgres_OptionVecU8_t *kept;
	PgPgError	failed;

	std_vec_with_capacity_Checked(&frame->decoded, self->chain.len - frame->from, DOLTGRES_SCAN_TRANSFORM);
	frame->decoded_live = true;
	for (size_t i = frame->from; i < frame->end; i++)
	{
		const TxnBasePending *pending = &STD_VEC_AT(&self->chain, TxnVisibleLive, i).f0;
		AccessTransformTransform decoded;

		if (!access_transform_Transform_decode(pending->transform.ptr, pending->transform.len, &decoded, &failed))
		{
			*frame->error = pghost_held(&failed);
			return;
		}
		std_vec_push_Checked(&frame->decoded, &decoded, DOLTGRES_SCAN_TRANSFORM);
	}
	if (!access_transform_compose(frame->decoded.ptr, frame->decoded.len, &frame->has_composed, &frame->composed,
								  &failed))
	{
		*frame->error = pghost_held(&failed);
		return;
	}
	if (frame->has_composed)
	{
		access_transform_Transform_encode(&frame->composed, frame->result);
		frame->result_live = true;
		access_transform_Transform_drop(&frame->composed);
		frame->has_composed = false;
	}
	else
	{
		std_vec_new(frame->result, DOLTGRES_SCAN_BYTES);
		frame->result_live = true;
	}
	kept = &STD_VEC_AT(&self->composed, doltgres_OptionVecU8_t, frame->from);
	std_vec_clone(&kept->v, frame->result, DOLTGRES_SCAN_BYTES);
	kept->has = true;
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_Epochs_transform answers the encoded transform a row at an
 * epoch takes to the reader's, empty for a row at the reader's, composed
 * from the pending transforms past its epoch and kept for the next row of
 * the epoch; a row past the reader's epoch is an internal error.
 */
bool
doltgres_scan_Epochs_transform(DoltgresScanEpochs *self, uint32 epoch, StdVec *result, HostError *error)
{
	size_t		end = self->has_end ? self->end : self->chain.len;
	doltgres_scan_Epochs_transform_frame_t frame = {.self = self, .end = end, .result = result, .error = error};

	if (epoch < self->base || (size_t) (epoch - self->base) > end)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("a row's epoch "), STD_FMT_ARG(0),
		STD_FMT_LIT(" is past its reader's")};
		StdFmtArg	args[] = {{&epoch, std_u32_Display_fmt}};
		StdFmtArguments message = {pieces, lengthof(pieces), args};
		StdString text;

		std_fmt_format_Checked(&text, &message);
		*error = host_Error_fork((const uint8 *) "XX000", &text);
		return false;
	}
	frame.from = epoch - self->base;
	if (frame.from == end)
	{
		std_vec_new(result, DOLTGRES_SCAN_BYTES);
		return true;
	}
	if (self->composed.len < self->chain.len)
	{
		doltgres_OptionVecU8_t none = {0};

		std_vec_resize(&self->composed, self->chain.len, &none, DOLTGRES_SCAN_COMPOSED);
	}
	if (STD_VEC_AT(&self->composed, doltgres_OptionVecU8_t, frame.from).has)
	{
		std_vec_clone(result, &STD_VEC_AT(&self->composed, doltgres_OptionVecU8_t, frame.from).v,
					  DOLTGRES_SCAN_BYTES);
		return true;
	}
	std_landing(&frame, doltgres_scan_Epochs_transform_body, doltgres_scan_Epochs_transform_pad);
	doltgres_scan_Epochs_transform_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_Epochs_of_version answers the encoded transform a delta version's row takes, by its epoch. */
static bool
doltgres_scan_Epochs_of_version(DoltgresScanEpochs *self, HostIoIo *io, TxnMemtableVersionVersion *version,
								StdVec *result, HostError *error)
{
	uint32		epoch;

	if (!txn_visible_epoch(io, self->base, self->chain.ptr, self->chain.len, version, &epoch, error))
		return false;
	return doltgres_scan_Epochs_transform(self, epoch, result, error);
}

/*
 * doltgres_scan_Epochs_of_written answers the encoded transform the row a
 * writer wrote takes, by its top-level xid and command and its commit's end,
 * none for a writer that hasn't committed.
 */
bool
doltgres_scan_Epochs_of_written(DoltgresScanEpochs *self, uint64 xid, uint32 command, bool has_end, uint64 end,
								StdVec *result, HostError *error)
{
	return doltgres_scan_Epochs_transform(self, txn_visible_epoch_after(self->base, self->chain.ptr, self->chain.len,
																		xid, command, has_end, end), result, error);
}

/* doltgres_scan_Epochs_base returns the epoch of the base's rows. */
uint32
doltgres_scan_Epochs_base(const DoltgresScanEpochs *self)
{
	return self->base;
}

/* doltgres_scan_Found_drop frees a found row's bytes. */
static void
doltgres_scan_Found_drop(void *found_)
{
	DoltgresScanFound *found = found_;

	if (found->has_tuple)
		std_vec_free_Checked(&found->tuple, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&found->values, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&found->transform, DOLTGRES_SCAN_BYTES);
}

/* doltgres_scan_Group_drop frees a group's key, base tuple, and rows. */
static void
doltgres_scan_Group_drop(DoltgresScanGroup *self)
{
	std_vec_free_Checked(&self->key, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->base, DOLTGRES_SCAN_BYTES);
	std_ptr_drop_in_place_slice(self->rows.ptr, self->rows.len, sizeof(DoltgresScanFound), doltgres_scan_Found_drop);
	std_vec_free_Checked(&self->rows, DOLTGRES_SCAN_FOUND);
}

/* doltgres_scan_Place_drop drops a place's kept cursor. */
static void
doltgres_scan_Place_drop(DoltgresScanPlace *self)
{
	if (self->tag == DOLTGRES_SCAN_PLACE_AT)
		trees_cursor_KeptCursor_drop(&self->u.at.cursor);
}

/* doltgres_scan_Place_set replaces a place, dropping the one it held, as an assignment does. */
static void
doltgres_scan_Place_set(DoltgresScanPlace *self, DoltgresScanPlace place)
{
	doltgres_scan_Place_drop(self);
	*self = place;
}

/* doltgres_scan_off returns Place::Off of a way. */
static DoltgresScanPlace
doltgres_scan_off(DoltgresScanDirection way)
{
	DoltgresScanPlace place = {.tag = DOLTGRES_SCAN_PLACE_OFF};

	place.u.off = way;
	return place;
}

/* doltgres_scan_Copies_drop frees a base entry's copied rows. */
static void
doltgres_scan_Copies_drop(DoltgresScanCopies *self)
{
	std_vec_free_Checked(&self->tuple, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->stamps, DOLTGRES_SCAN_STAMP);
	std_vec_free_Checked(&self->values, DOLTGRES_SCAN_U32);
}

/* doltgres_scan_Row_drop frees a row's bytes. */
void
doltgres_scan_Row_drop(DoltgresScanRow *self)
{
	std_vec_free_Checked(&self->keyed, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->key, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->tuple, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->values, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->transform, DOLTGRES_SCAN_BYTES);
}

/* doltgres_scan_Row_drop_in_place is Row's drop glue, an element of a Vec<Row>'s. */
static void
doltgres_scan_Row_drop_in_place(void *row)
{
	doltgres_scan_Row_drop(row);
}

/* doltgres_scan_rows_drop frees a Vec<Row>. */
static void
doltgres_scan_rows_drop(StdVec *rows)
{
	std_ptr_drop_in_place_slice(rows->ptr, rows->len, sizeof(DoltgresScanRow), doltgres_scan_Row_drop_in_place);
	std_vec_free_Checked(rows, DOLTGRES_SCAN_ROW);
}

/* doltgres_scan_Turn_drop frees what a turn holds. */
static void
doltgres_scan_Turn_drop(DoltgresScanTurn *self)
{
	switch (self->tag)
	{
		case DOLTGRES_SCAN_TURN_ROW:
			doltgres_scan_Row_drop(&self->u.row);
			break;
		case DOLTGRES_SCAN_TURN_RUN:
			trees_KeyOrder_drop(&self->u.run);
			break;
		case DOLTGRES_SCAN_TURN_COMPARE:
			std_vec_free_Checked(&self->u.compare.base_key, DOLTGRES_SCAN_BYTES);
			doltgres_scan_Copies_drop(&self->u.compare.base_copies);
			std_vec_free_Checked(&self->u.compare.delta_key, DOLTGRES_SCAN_BYTES);
			trees_KeyOrder_drop(&self->u.compare.order);
			break;
		case DOLTGRES_SCAN_TURN_AGAIN:
		case DOLTGRES_SCAN_TURN_END:
		case DOLTGRES_SCAN_TURN_MOVED:
			break;
	}
}

/* doltgres_scan_Bounds_drop frees a unit's bounds and its order. */
static void
doltgres_scan_Bounds_drop(DoltgresScanBounds *self)
{
	doltgres_scan_option_drop(&self->low);
	doltgres_scan_option_drop(&self->high);
	trees_KeyOrder_drop(&self->order);
}

/* doltgres_scan_Picked_drop frees the candidates a choice holds. */
static void
doltgres_scan_Picked_drop(DoltgresScanPicked *self)
{
	if (self->tag != DOLTGRES_SCAN_PICKED_DELTA)
	{
		std_vec_free_Checked(&self->base_key, DOLTGRES_SCAN_BYTES);
		doltgres_scan_Copies_drop(&self->base_copies);
	}
	if (self->tag != DOLTGRES_SCAN_PICKED_BASE)
		std_vec_free_Checked(&self->delta_key, DOLTGRES_SCAN_BYTES);
}

/* doltgres_scan_Page_drop frees an exact page's tuple ids. */
static void
doltgres_scan_Page_drop(DoltgresScanPage *self)
{
	if (self->tag == DOLTGRES_SCAN_PAGE_EXACT)
		std_vec_free_Checked(&self->tuple_ids, DOLTGRES_SCAN_U64);
}

/* doltgres_scan_ended returns Page::Ended. */
static DoltgresScanPage
doltgres_scan_ended(void)
{
	DoltgresScanPage page = {.tag = DOLTGRES_SCAN_PAGE_ENDED};

	return page;
}

/* doltgres_scan_Layout_drop frees a layout's keyed layout and order. */
void
doltgres_scan_Layout_drop(DoltgresScanLayout *self)
{
	std_vec_free_Checked(&self->keyed, DOLTGRES_SCAN_BYTES);
	trees_KeyOrder_drop(&self->order);
}

/* doltgres_scan_Shared_drop frees a shared base's split keys and epochs. */
void
doltgres_scan_Shared_drop(DoltgresScanShared *self)
{
	doltgres_scan_vecs_drop(&self->splits);
	doltgres_scan_Epochs_drop(&self->epochs);
}

/* doltgres_scan_Step_drop frees a step's row. */
void
doltgres_scan_Step_drop(DoltgresScanStep *self)
{
	if (self->tag == DOLTGRES_SCAN_STEP_ROW)
		doltgres_scan_Row_drop(&self->row);
}

/* doltgres_scan_Paired_drop frees a pair's key, keyed layout, order, and transform. */
static void
doltgres_scan_Paired_drop(DoltgresScanPaired *self)
{
	std_vec_free_Checked(&self->key, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&self->keyed, DOLTGRES_SCAN_BYTES);
	trees_KeyOrder_drop(&self->order);
	std_vec_free_Checked(&self->transform, DOLTGRES_SCAN_BYTES);
}

/* doltgres_scan_Resolved_drop drops a memo of a base leaf: its stamps and its kept cursor, when it holds them. */
void
doltgres_scan_Resolved_drop(DoltgresScanResolved *self)
{
	trees_cursor_LeafStamps_drop(&self->stamps);
	if (self->has_kept)
		trees_cursor_KeptCursor_drop(&self->kept);
	self->has_kept = false;
}

/* doltgres_scan_Scan_drop drops a scan's record, its fields in their order, its reader first. */
static void
doltgres_scan_Scan_drop(void *record)
{
	DoltgresScanScan *scan = record;

	host_shared_Reader_drop(&scan->reader);
	if (scan->has_group)
		doltgres_scan_Group_drop(&scan->group);
	doltgres_scan_Place_drop(&scan->place);
	if (scan->has_run)
		doltgres_scan_vecs_drop(&scan->run.keys);
	if (scan->has_written)
		std_vec_free_Checked(&scan->written, DOLTGRES_SCAN_U64);
	doltgres_scan_Epochs_drop(&scan->epochs);
	doltgres_scan_Resolved_drop(&scan->resolved);
	std_vec_free_Checked(&scan->keyed, DOLTGRES_SCAN_BYTES);
	trees_KeyOrder_drop(&scan->order);
	doltgres_scan_option_drop(&scan->bound);
	doltgres_scan_option_drop(&scan->seek);
	if (scan->has_parallel)
	{
		doltgres_scan_vecs_drop(&scan->parallel.splits);
		doltgres_scan_option_drop(&scan->parallel.low);
		doltgres_scan_option_drop(&scan->parallel.high);
		if (scan->parallel.has_kept)
			doltgres_scan_vecs_drop(&scan->parallel.kept);
	}
	doltgres_scan_Page_drop(&scan->page);
	if (scan->has_sample)
		doltgres_scan_rows_drop(&scan->sample.rows);
}

/* DOLTGRES_SCAN_SCANS is SCANS, the backend's table scans. */
HostOwnedTable DOLTGRES_SCAN_SCANS = HOST_OWNED_TABLE_NEW;

/* doltgres_scan_Scan_Owned_table is Scan's Owned::table, SCANS. */
static HostOwnedTable *
doltgres_scan_Scan_Owned_table(void)
{
	return &DOLTGRES_SCAN_SCANS;
}

/* doltgres_scan_OptionTupleU32U16_t is an Option<(u32, u16)>, a sample's block and its largest offset. */
typedef struct doltgres_scan_OptionTupleU32U16_t
{
	bool		has;
	uint32		f0;
	uint16		f1;
} doltgres_scan_OptionTupleU32U16_t;

/* doltgres_scan_OptionRow_t is an Option<Row>. */
typedef struct doltgres_scan_OptionRow_t
{
	bool		has;
	DoltgresScanRow v;
} doltgres_scan_OptionRow_t;

/* DOLTGRES_SCAN_OPTION_ROW_DROP drops an Option<Row> a call left. */
#define DOLTGRES_SCAN_OPTION_ROW_DROP(place) \
	do { \
		if ((place)->has) \
			doltgres_scan_Row_drop(&(place)->v); \
	} while (0)

/* Scan's Table, host's owned Table<Scan>, with its kind's glue, extern as txn state's Holds' is. */
#define HOST_OWNED_SCOPE extern
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at (). */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS Unit
#define HOST_OWNED_WITH_UNIT
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at u32, for sample_next. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS U32
#define HOST_OWNED_WITH_T uint32
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at Option<(u32, u16)>, for sample_page. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS OptionTupleU32U16
#define HOST_OWNED_WITH_T doltgres_scan_OptionTupleU32U16_t
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at Option<Row>, for sample_take. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS OptionRow
#define HOST_OWNED_WITH_T doltgres_scan_OptionRow_t
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_OPTION_ROW_DROP(place)
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at Bitmap, for bitmap_step. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS Bitmap
#define HOST_OWNED_WITH_T DoltgresScanBitmap
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at u64, for blocks. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS U64
#define HOST_OWNED_WITH_T uint64
#include "doltgres/host/owned_template.h"

/* Scan's Table's with at bool, for tid_valid, unchecked, and is_parallel. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS Bool
#define HOST_OWNED_WITH_T bool
#include "doltgres/host/owned_template.h"

/* doltgres_scan_sample_next_closure_0 is sample_next's call: the sample's next block in order. */
static bool
doltgres_scan_sample_next_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, uint32 *result,
									HostError *error)
{
	*result = scan->has_sample ? std_saturating_add_u32(scan->sample.block, 1) : 0;
	return true;
}

/*
 * doltgres_scan_sample_next answers the block a sample scan reads next in
 * order: the first, or the one after its block.
 */
bool
doltgres_scan_sample_next(HostIoIo *io, HostOwnedHandle handle, uint32 *result, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withU32(&DOLTGRES_SCAN_SCANS, io, handle,
													doltgres_scan_sample_next_closure_0, NULL, NULL, result, error);
}

/* doltgres_scan_sample_block_captures_t is sample_block's call's captures: the block, if any, and its rows. */
typedef struct doltgres_scan_sample_block_captures_t
{
	bool		has_block;
	uint32		block;
	StdVec		rows;
} doltgres_scan_sample_block_captures_t;

/* doltgres_scan_sample_block_drop frees the rows of a block whose call never ran. */
static void
doltgres_scan_sample_block_drop(void *captures)
{
	doltgres_scan_sample_block_captures_t *block = captures;

	if (block->has_block)
		doltgres_scan_rows_drop(&block->rows);
}

/* doltgres_scan_sample_block_closure_0 is sample_block's call: the scan's sample replaced by the block. */
static bool
doltgres_scan_sample_block_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_sample_block_captures_t *block = captures;

	if (scan->has_sample)
		doltgres_scan_rows_drop(&scan->sample.rows);
	scan->has_sample = block->has_block;
	if (block->has_block)
	{
		scan->sample.block = block->block;
		scan->sample.rows = block->rows;
	}
	return true;
}

/*
 * doltgres_scan_sample_block starts a sample scan's block with its rows the
 * snapshot sees, a Vec<Row> it takes, or ends its sample with none.
 */
bool
doltgres_scan_sample_block(HostIoIo *io, HostOwnedHandle handle, bool has_block, uint32 block, StdVec *rows,
						   HostError *error)
{
	doltgres_scan_sample_block_captures_t captures = {.has_block = has_block, .block = block};

	if (has_block)
		captures.rows = *rows;
	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_sample_block_closure_0, &captures,
													 doltgres_scan_sample_block_drop, error);
}

/* doltgres_scan_sample_page_closure_0 is sample_page's call: the sample's block and its rows' largest offset. */
static bool
doltgres_scan_sample_page_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									doltgres_scan_OptionTupleU32U16_t *result, HostError *error)
{
	uint16		most = 0;

	result->has = scan->has_sample;
	if (!scan->has_sample)
		return true;
	for (size_t i = 0; i < scan->sample.rows.len; i++)
		most = Max(most, (uint16) (STD_VEC_AT(&scan->sample.rows, DoltgresScanRow, i).stamp.tuple & 0xffff));
	result->f0 = scan->sample.block;
	result->f1 = most;
	return true;
}

/*
 * doltgres_scan_sample_page answers a sample scan's block and its rows'
 * largest offset, the page's last line pointer heap's sampling methods are
 * given, or none.
 */
bool
doltgres_scan_sample_page(HostIoIo *io, HostOwnedHandle handle, bool *has_result, uint32 *result_0, uint16 *result_1,
						  HostError *error)
{
	doltgres_scan_OptionTupleU32U16_t page;

	if (!host_owned_TableDoltgresScanScan_withOptionTupleU32U16(&DOLTGRES_SCAN_SCANS, io, handle,
																doltgres_scan_sample_page_closure_0, NULL, NULL, &page,
																error))
		return false;
	*has_result = page.has;
	if (page.has)
	{
		*result_0 = page.f0;
		*result_1 = page.f1;
	}
	return true;
}

/* doltgres_scan_sample_take_closure_0 is sample_take's call: the sample's row of the offset taken out. */
static bool
doltgres_scan_sample_take_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									doltgres_scan_OptionRow_t *result, HostError *error)
{
	uint16		offset = *(const uint16 *) captures;

	result->has = false;
	if (!scan->has_sample)
		return true;
	for (size_t at = 0; at < scan->sample.rows.len; at++)
		if ((STD_VEC_AT(&scan->sample.rows, DoltgresScanRow, at).stamp.tuple & 0xffff) == offset)
		{
			std_vec_swap_remove(&scan->sample.rows, at, &result->v, DOLTGRES_SCAN_ROW);
			result->has = true;
			break;
		}
	return true;
}

/*
 * doltgres_scan_sample_take takes a sample scan's row of an offset of its
 * block, none for an offset no row the snapshot sees has.
 */
bool
doltgres_scan_sample_take(HostIoIo *io, HostOwnedHandle handle, uint16 offset, bool *has_result,
						  DoltgresScanRow *result, HostError *error)
{
	doltgres_scan_OptionRow_t taken;

	if (!host_owned_TableDoltgresScanScan_withOptionRow(&DOLTGRES_SCAN_SCANS, io, handle,
														doltgres_scan_sample_take_closure_0, &offset, NULL, &taken,
														error))
		return false;
	*has_result = taken.has;
	if (taken.has)
		*result = taken.v;
	return true;
}

/*
 * doltgres_scan_bitmap_page_captures_t is bitmap_page's call's captures: the
 * page's first tid and its offsets, if exact.
 */
typedef struct doltgres_scan_bitmap_page_captures_t
{
	uint64		first;
	bool		has_offsets;
	const uint16 *offsets;
	size_t		offsets_len;
} doltgres_scan_bitmap_page_captures_t;

/* doltgres_scan_bitmap_page_closure_0 is bitmap_page's call: the scan's page replaced by the bitmap's. */
static bool
doltgres_scan_bitmap_page_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_bitmap_page_captures_t *page = captures;
	DoltgresScanPage next = {0};

	if (page->has_offsets)
	{
		next.tag = DOLTGRES_SCAN_PAGE_EXACT;
		std_vec_with_capacity_Checked(&next.tuple_ids, page->offsets_len, DOLTGRES_SCAN_U64);
		for (size_t i = 0; i < page->offsets_len; i++)
		{
			uint64		tuple_id = page->first | page->offsets[i];

			std_vec_push_Checked(&next.tuple_ids, &tuple_id, DOLTGRES_SCAN_U64);
		}
	}
	else
	{
		scan->has_range = true;
		scan->range_first = page->first | 1;
		scan->range_last = page->first | 0xffff;
		scan->has_given = false;
		next.tag = DOLTGRES_SCAN_PAGE_LOSSY;
	}
	doltgres_scan_Page_drop(&scan->page);
	scan->page = next;
	return true;
}

/*
 * doltgres_scan_bitmap_page starts a bitmap scan's page, an exact page's
 * offsets as its tuple ids or, with no offsets, a lossy page's every tid as
 * the scan's tid range, which starts the range's read over.
 */
bool
doltgres_scan_bitmap_page(HostIoIo *io, HostOwnedHandle handle, uint32 block, bool has_offsets,
						  const uint16 *offsets, size_t offsets_len, HostError *error)
{
	doltgres_scan_bitmap_page_captures_t captures = {(uint64) block << 16, has_offsets, offsets, offsets_len};

	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_bitmap_page_closure_0, &captures, NULL, error);
}

/* doltgres_scan_bitmap_step_closure_0 is bitmap_step's call: the page's next step, the exact page's next tid taken. */
static bool
doltgres_scan_bitmap_step_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									DoltgresScanBitmap *result, HostError *error)
{
	DoltgresScanPage *page = &scan->page;

	result->tag = DOLTGRES_SCAN_BITMAP_PAGE;
	if (page->tag == DOLTGRES_SCAN_PAGE_EXACT && page->front < page->tuple_ids.len)
	{
		result->tag = DOLTGRES_SCAN_BITMAP_TID;
		result->tid = STD_VEC_AT(&page->tuple_ids, uint64, page->front++);
	}
	else if (page->tag == DOLTGRES_SCAN_PAGE_LOSSY)
		result->tag = DOLTGRES_SCAN_BITMAP_LOSSY;
	return true;
}

/* doltgres_scan_bitmap_step answers a bitmap scan's next step, taking the exact page's next tuple id. */
bool
doltgres_scan_bitmap_step(HostIoIo *io, HostOwnedHandle handle, DoltgresScanBitmap *result, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withBitmap(&DOLTGRES_SCAN_SCANS, io, handle,
													   doltgres_scan_bitmap_step_closure_0, NULL, NULL, result,
													   error);
}

/* doltgres_scan_bitmap_ended_closure_0 is bitmap_ended's call: the page ended. */
static bool
doltgres_scan_bitmap_ended_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_Page_drop(&scan->page);
	scan->page = doltgres_scan_ended();
	return true;
}

/* doltgres_scan_bitmap_ended ends a bitmap scan's lossy page once its tid range gives no more rows. */
bool
doltgres_scan_bitmap_ended(HostIoIo *io, HostOwnedHandle handle, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_bitmap_ended_closure_0, NULL, NULL, error);
}

/* doltgres_scan_blocks_closure_0 is blocks' call: the scan's count of blocks. */
static bool
doltgres_scan_blocks_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, uint64 *result,
							   HostError *error)
{
	*result = scan->blocks;
	return true;
}

/* doltgres_scan_blocks answers the storage's count of blocks when a scan began. */
bool
doltgres_scan_blocks(HostIoIo *io, HostOwnedHandle handle, uint64 *result, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withU64(&DOLTGRES_SCAN_SCANS, io, handle, doltgres_scan_blocks_closure_0,
													NULL, NULL, result, error);
}

/* doltgres_scan_Direction_Debug_fmt is Direction's derived Debug. */
bool
doltgres_scan_Direction_Debug_fmt(const void *value, StdFormatter *f)
{
	if (*(const DoltgresScanDirection *) value == DOLTGRES_SCAN_DIRECTION_FORWARD)
		return std_fmt_write_str(f, "Forward", 7);
	return std_fmt_write_str(f, "Backward", 8);
}

/* doltgres_scan_step moves a cursor one entry in a direction. */
static bool
doltgres_scan_step(HostIoIo *io, TreesPinPin *pin, TreesCursorCursor *cursor, DoltgresScanDirection way,
				   HostError *error)
{
	ChunksErrorError failed;
	bool		ok = way == DOLTGRES_SCAN_DIRECTION_FORWARD ? trees_cursor_Cursor_next(cursor, io, pin, &failed) :
		trees_cursor_Cursor_prev(cursor, io, pin, &failed);

	if (!ok)
		*error = txn_base_storage(&failed);
	return ok;
}

/*
 * doltgres_scan_copies answers the count of copies and the tuple of the row
 * of the entry a cursor is at: a keyless table's entry counts its row's
 * copies, and any other entry is one row.
 */
static bool
doltgres_scan_copies(const TreesCursorCursor *cursor, bool keyless, uint64 *count, StdBytes *tuple, HostError *error)
{
	StdBytes	value = trees_cursor_Cursor_value(cursor);
	ChunksErrorError failed;

	if (!keyless)
	{
		*count = 1;
		*tuple = value;
		return true;
	}
	if (!txn_counted_keyless_parts(value.ptr, value.len, count, tuple, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	return true;
}

/*
 * doltgres_scan_uneven returns the error of a stored entry whose stamps differ
 * in number from its copies, data corruption.
 */
static HostError
doltgres_scan_uneven(void)
{
	static const char text[] = "a stored row's stamps differ in number from its copies";
	StdString	message;

	std_string_from_Checked(&message, text, sizeof(text) - 1);
	return host_Error_fork((const uint8 *) "XX001", &message);
}

/*
 * doltgres_scan_Scan_entries returns a compared run's delta keys a read of
 * the scan takes, a Vec<Vec<u8>> it takes: for a read of an index's
 * entries, those before its records, and otherwise every key.
 */
static void
doltgres_scan_Scan_entries(const DoltgresScanScan *self, StdVec *keys)
{
	size_t		kept = 0;

	if (!self->index || self->records)
		return;
	for (size_t i = 0; i < keys->len; i++)
	{
		StdVec	   *key = &STD_VEC_AT(keys, StdVec, i);

		if (key->len > 0 && ((const uint8 *) key->ptr)[0] == TXN_ENTRY_RECORD)
			std_vec_free_Checked(key, DOLTGRES_SCAN_BYTES);
		else
			STD_VEC_AT(keys, StdVec, kept++) = *key;
	}
	keys->len = kept;
}

static bool doltgres_scan_base_next(HostIoIo *io, TreesStoreNodeStore store, bool has_tree, TreesStoreTree tree,
									bool keyless, bool index, const TreesKeyOrder *entries,
									DoltgresScanPlace *place, const doltgres_OptionVecU8_t *bound,
									DoltgresScanDirection way, bool *has_result, StdVec *result_key,
									DoltgresScanCopies *result_copies, HostError *error);
static bool doltgres_scan_grouped(HostIoIo *io, const DoltgresScanUnder *under, DoltgresScanEpochs *epochs,
								  StdVec *key, bool has_copies, DoltgresScanCopies *copies, StdVec *versions,
								  DoltgresScanGroup *result, HostError *error);
static bool doltgres_scan_run(HostIoIo *io, const DoltgresScanUnder *under, const TreesKeyOrder *order,
							  StdVec *result, HostError *error);
static bool doltgres_scan_split(HostIoIo *io, const StdVec *keys, const uint8 *from, size_t from_len,
								DoltgresScanDirection way, const TreesKeyOrder *order, size_t *result,
								HostError *error);
static bool doltgres_scan_pick(HostIoIo *io, StdVec *base_key, DoltgresScanCopies *base_copies, StdVec *delta_key,
							   size_t delta_at, const TreesKeyOrder *order, DoltgresScanDirection way,
							   DoltgresScanPicked *result, HostError *error);
static bool doltgres_scan_reseated(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
								   HostOwnedHandle handle, DoltgresScanDirection way, HostError *error);

/* doltgres_scan_Repo_Stores_with is the repository's Stores::with: the call lent the repository's reader of nodes. */
static void
doltgres_scan_Repo_Stores_with(const void *self, HostIoIo *io, const HostSharedReader *reader,
							   void (*call) (void *captures, HostIoIo *io, TreesStoreNodeStore store), void *captures)
{
	TreesStoreReader store = repo_Repo_reader(self, HOST_CACHE_PLACEMENT_SERVER, reader);

	call(captures, io, TREES_STORE_READER_NODE_STORE(&store));
}

/* DOLTGRES_SCAN_REPO_STORES_OPS is the backend's repository's Stores. */
const DoltgresScanStoresOps DOLTGRES_SCAN_REPO_STORES_OPS = {doltgres_scan_Repo_Stores_with};

/*
 * doltgres_scan_row_frame_t is a row's making's state across its pad: the
 * row, and how many of its owned fields are made.
 */
typedef struct doltgres_scan_row_frame_t
{
	DoltgresScanRow *row;
	const uint8 *keyed;
	size_t		keyed_len;
	const uint8 *key;
	size_t		key_len;
	const uint8 *tuple;
	size_t		tuple_len;
	const StdVec *values;
	const StdVec *transform;
	int			made;
} doltgres_scan_row_frame_t;

/* doltgres_scan_row_body copies the row's owned fields out, in the struct's order. */
static void
doltgres_scan_row_body(void *frame_)
{
	doltgres_scan_row_frame_t *frame = frame_;

	std_slice_to_vec_Checked(&frame->row->keyed, frame->keyed, frame->keyed_len, DOLTGRES_SCAN_BYTES);
	frame->made = 1;
	std_slice_to_vec_Checked(&frame->row->key, frame->key, frame->key_len, DOLTGRES_SCAN_BYTES);
	frame->made = 2;
	std_slice_to_vec_Checked(&frame->row->tuple, frame->tuple, frame->tuple_len, DOLTGRES_SCAN_BYTES);
	frame->made = 3;
	std_vec_clone(&frame->row->values, frame->values, DOLTGRES_SCAN_BYTES);
	frame->made = 4;
	std_vec_clone(&frame->row->transform, frame->transform, DOLTGRES_SCAN_BYTES);
	frame->made = 5;
}

/* doltgres_scan_row_pad frees the fields an unwound copy made. */
static void
doltgres_scan_row_pad(void *frame_)
{
	doltgres_scan_row_frame_t *frame = frame_;
	StdVec	   *fields[] = {&frame->row->keyed, &frame->row->key, &frame->row->tuple, &frame->row->values,
	&frame->row->transform};

	for (int i = frame->made; i > 0; i--)
		std_vec_free_Checked(fields[i - 1], DOLTGRES_SCAN_BYTES);
}

/*
 * doltgres_scan_Found_row makes the row a read gives of a found row of a
 * key in a table of a keyed layout and an object id, the base entry's tuple
 * for a copy of the base's.
 */
static void
doltgres_scan_Found_row(const DoltgresScanFound *self, const StdVec *keyed, uint32 table, const uint8 *key,
						size_t key_len, const StdVec *base, DoltgresScanRow *result)
{
	const StdVec *tuple = self->has_tuple ? &self->tuple : base;
	doltgres_scan_row_frame_t frame = {result, keyed->ptr, keyed->len, key, key_len, tuple->ptr, tuple->len,
	&self->values, &self->transform};

	std_landing(&frame, doltgres_scan_row_body, doltgres_scan_row_pad);
	result->table = table;
	result->stamp = self->stamp;
	result->top = self->top;
	result->command = self->command;
	result->has_changer = self->has_changer;
	result->changer = self->changer;
	result->dirty = self->dirty;
}

/*
 * doltgres_scan_begin_frame_t is begin's state across its pad: the keyed
 * layout and order it took, its epochs, and its reader, while live.
 */
typedef struct doltgres_scan_begin_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	uint64		blocks;
	const TxnBaseBase *base;
	uint64		generation;
	StdVec		keyed;
	bool		keyed_live;
	uint32		table;
	TreesKeyOrder order;
	bool		order_live;
	DoltgresScanEpochs epochs;
	bool		epochs_live;
	HostSharedReader reader;
	bool		reader_live;
	HostOwnedHandle *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_begin_frame_t;

/* doltgres_scan_begin_pad drops what begin still holds, its reader first, as an early return drops it. */
static void
doltgres_scan_begin_pad(void *frame_)
{
	doltgres_scan_begin_frame_t *frame = frame_;

	if (frame->reader_live)
		host_shared_Reader_drop(&frame->reader);
	frame->reader_live = false;
	if (frame->epochs_live)
		doltgres_scan_Epochs_drop(&frame->epochs);
	frame->epochs_live = false;
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->order_live = false;
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, DOLTGRES_SCAN_BYTES);
	frame->keyed_live = false;
}

/* doltgres_scan_begin_body reads the base's epochs, takes a reader, and makes the scan's record. */
static void
doltgres_scan_begin_body(void *frame_)
{
	doltgres_scan_begin_frame_t *frame = frame_;
	const TxnBaseBase *base = frame->base;
	DoltgresScanScan scan = {0};
	uint32		epoch;
	bool		has_chain;
	TreesStoreTree chain;

	doltgres_scan_Epochs_based(base, &epoch, &has_chain, &chain);
	if (!doltgres_scan_Epochs_of(frame->io, frame->store, epoch, has_chain, chain, &frame->epochs, frame->error))
		return;
	frame->epochs_live = true;
	if (!host_shared_read(frame->io, &frame->reader, frame->error))
		return;
	frame->reader_live = true;
	scan.reader = frame->reader;
	scan.place.tag = DOLTGRES_SCAN_PLACE_START;
	scan.has_tree = base != NULL && base->has_root;
	if (scan.has_tree)
		scan.tree = base->root;
	scan.folded = base == NULL ? 0 : base->read;
	scan.epochs = frame->epochs;
	scan.generation = frame->generation;
	scan.blocks = frame->blocks;
	scan.keyed = frame->keyed;
	scan.table = frame->table;
	scan.has_tuple_ids = base != NULL && base->has_tuple_ids;
	if (scan.has_tuple_ids)
		scan.tuple_ids = base->tuple_ids;
	scan.order = frame->order;
	scan.index = base != NULL && base->has_index;
	scan.page = doltgres_scan_ended();
	frame->reader_live = frame->epochs_live = frame->keyed_live = frame->order_live = false;
	frame->ok = host_owned_TableDoltgresScanScan_make(&DOLTGRES_SCAN_SCANS, frame->io, scan, frame->result,
													  frame->error);
}

/*
 * doltgres_scan_scan_begin makes a scan of a storage of a count of blocks,
 * whose base, or no base (NULL), read at a base generation or after it, is in
 * a table of a keyed layout, an object id, and a tree's order, which it
 * takes, and answers its handle; its first read gives the storage's first
 * row its snapshot sees.
 */
bool
doltgres_scan_scan_begin(HostIoIo *io, TreesStoreNodeStore store, uint64 blocks, const TxnBaseBase *base,
						 uint64 generation, StdVec *keyed, uint32 table, TreesKeyOrder *order,
						 HostOwnedHandle *result, HostError *error)
{
	doltgres_scan_begin_frame_t frame = {.io = io, .store = store, .blocks = blocks, .base = base,
		.generation = generation, .keyed = *keyed,
	.keyed_live = true, .table = table, .order = *order, .order_live = true, .result = result, .error = error};

	std_landing(&frame, doltgres_scan_begin_body, doltgres_scan_begin_pad);
	doltgres_scan_begin_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_given answers the row after the last read's in its key's group
 * the way a read goes, or false past the group's end.
 */
static bool
doltgres_scan_given(DoltgresScanScan *scan, DoltgresScanDirection way, DoltgresScanRow *result)
{
	DoltgresScanGroup *group = &scan->group;
	size_t		at;

	if (!scan->has_group)
		return false;
	if (!group->has_at && way == DOLTGRES_SCAN_DIRECTION_FORWARD)
		at = 0;
	else if (!group->has_at)
	{
		if (group->rows.len == 0)
			return false;
		at = group->rows.len - 1;
	}
	else if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
		at = group->at + 1;
	else
	{
		if (group->at == 0)
			return false;
		at = group->at - 1;
	}
	if (at >= group->rows.len)
		return false;
	group->has_at = true;
	group->at = at;
	doltgres_scan_Found_row(&STD_VEC_AT(&group->rows, DoltgresScanFound, at), &scan->keyed, scan->table,
							group->key.ptr, group->key.len, &group->base, result);
	return true;
}

/* doltgres_scan_past reports whether a key is past another the way a read goes. */
static bool
doltgres_scan_past(const StdVec *key, const StdVec *other, DoltgresScanDirection way)
{
	StdOrdering ordering = std_bytes_Ord_cmp(key->ptr, key->len, other->ptr, other->len);

	return way == DOLTGRES_SCAN_DIRECTION_FORWARD ? ordering == STD_ORDERING_GREATER : ordering == STD_ORDERING_LESS;
}

/* doltgres_scan_toward returns a read's direction as the delta's walk names it. */
static TxnMemtableTableDirection
doltgres_scan_toward(DoltgresScanDirection way)
{
	return way == DOLTGRES_SCAN_DIRECTION_FORWARD ? TXN_MEMTABLE_TABLE_DIRECTION_FORWARD :
		TXN_MEMTABLE_TABLE_DIRECTION_BACKWARD;
}

/* doltgres_scan_took records in the base's place whether the entry it is at joined the group the advance made. */
static void
doltgres_scan_took(DoltgresScanPlace *place, bool joined)
{
	if (place->tag == DOLTGRES_SCAN_PLACE_AT)
		place->u.at.taken = joined;
}

/*
 * doltgres_scan_base_read_t is a read of the base's next entry under the
 * scan's reader, a Stores::with call's captures: what it reads, and its
 * answer.
 */
typedef struct doltgres_scan_base_read_t
{
	DoltgresScanScan *scan;
	bool		keyless;
	const TreesKeyOrder *entries;
	DoltgresScanDirection way;
	bool		ok;
	bool		has_entry;
	StdVec		key;
	DoltgresScanCopies copies;
	HostError  *error;
} doltgres_scan_base_read_t;

/*
 * doltgres_scan_base_read reads the base's next entry past the scan's place,
 * advance's and gather's store calls' body.
 */
static void
doltgres_scan_base_read(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_base_read_t *read = captures;
	DoltgresScanScan *scan = read->scan;

	read->ok = doltgres_scan_base_next(io, store, scan->has_tree, scan->tree, read->keyless, scan->index,
									   read->entries, &scan->place, &scan->bound, read->way, &read->has_entry,
									   &read->key, &read->copies, read->error);
}

/* doltgres_scan_advance_closure_1 is advance's store call, the base's next entry read under the scan's reader. */
static void
doltgres_scan_advance_closure_1(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_base_read(captures, io, store);
}

/* doltgres_scan_gather_closure_2 is gather's store call, the base's next entry read under the scan's reader. */
static void
doltgres_scan_gather_closure_2(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_base_read(captures, io, store);
}

/*
 * doltgres_scan_advance_frame_t is advance's state across its pad: the
 * base's entry, the delta's hold, and the delta's key and versions, each
 * while live.
 */
typedef struct doltgres_scan_advance_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	DoltgresScanScan *scan;
	DoltgresScanDirection way;
	doltgres_scan_base_read_t entry;
	HostDeltaHold hold;
	bool		hold_live;
	bool		has_delta;
	txn_TupleVecU8VecVersion_t delta;
	DoltgresScanTurn *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_advance_frame_t;

/* doltgres_scan_entry_drop frees a base read's entry, when it has one. */
static void
doltgres_scan_entry_drop(doltgres_scan_base_read_t *entry)
{
	if (entry->has_entry)
	{
		std_vec_free_Checked(&entry->key, DOLTGRES_SCAN_BYTES);
		doltgres_scan_Copies_drop(&entry->copies);
	}
	entry->has_entry = false;
}

/* doltgres_scan_delta_drop frees the delta's key and its versions' buffer, when there is one. */
static void
doltgres_scan_delta_drop(bool *has_delta, txn_TupleVecU8VecVersion_t *delta)
{
	if (*has_delta)
	{
		std_vec_free_Checked(&delta->f0, DOLTGRES_SCAN_BYTES);
		txn_memtable_version_versions_drop(&delta->f1, 0);
	}
	*has_delta = false;
}

/* doltgres_scan_advance_pad drops what advance holds, in the Rust's reverse order. */
static void
doltgres_scan_advance_pad(void *frame_)
{
	doltgres_scan_advance_frame_t *frame = frame_;

	doltgres_scan_delta_drop(&frame->has_delta, &frame->delta);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	doltgres_scan_entry_drop(&frame->entry);
}

/* doltgres_scan_advance_body finds the nearer of the base's next entry and the delta's next key and makes its group. */
static void
doltgres_scan_advance_body(void *frame_)
{
	doltgres_scan_advance_frame_t *frame = frame_;
	DoltgresScanScan *scan = frame->scan;
	DoltgresScanDirection way = frame->way;
	bool		entries = scan->index && !scan->records;
	uint8		record = TXN_ENTRY_RECORD;
	const uint8 *from = NULL;
	size_t		from_len = 0;
	StdVec		key;
	bool		has_copies = false;
	DoltgresScanCopies copies;
	StdVec		versions;

	if (scan->has_group)
	{
		from = scan->group.key.ptr;
		from_len = scan->group.key.len;
	}
	else if (entries && way == DOLTGRES_SCAN_DIRECTION_BACKWARD)
	{
		from = &record;
		from_len = 1;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (scan->seek.has)
	{
		if (!txn_visible_versions(frame->io, &frame->hold, frame->under->state, scan->seek.v.ptr, scan->seek.v.len,
								  scan->folded, &versions, frame->error))
			return;
		if (versions.len == 0)
		{
			txn_memtable_version_versions_drop(&versions, 0);
			if (!txn_visible_next(frame->io, &frame->hold, frame->under->state, true, scan->seek.v.ptr,
								  scan->seek.v.len, doltgres_scan_toward(way), scan->folded, &frame->has_delta,
								  &frame->delta, frame->error))
				return;
		}
		else
		{
			std_vec_clone(&frame->delta.f0, &scan->seek.v, DOLTGRES_SCAN_BYTES);
			frame->delta.f1 = versions;
			frame->has_delta = true;
		}
	}
	else if (!txn_visible_next(frame->io, &frame->hold, frame->under->state, from != NULL, from, from_len,
							   doltgres_scan_toward(way), scan->folded, &frame->has_delta, &frame->delta,
							   frame->error))
		return;
	if (frame->has_delta && entries && frame->delta.f0.len > 0 &&
		((const uint8 *) frame->delta.f0.ptr)[0] == TXN_ENTRY_RECORD)
		doltgres_scan_delta_drop(&frame->has_delta, &frame->delta);
	if (txn_state_moved_past(frame->under->state, scan->generation))
	{
		frame->result->tag = DOLTGRES_SCAN_TURN_MOVED;
		frame->ok = true;
		return;
	}
	if (scan->has_group)
		doltgres_scan_Group_drop(&scan->group);
	scan->has_group = false;
	doltgres_scan_option_drop(&scan->seek);
	frame->entry.scan = scan;
	frame->entry.keyless = scan->keyed.len == 0;
	frame->entry.entries = entries ? &scan->order : NULL;
	frame->entry.way = way;
	frame->entry.error = frame->error;
	frame->stores->ops->with(frame->stores->self, frame->io, &scan->reader, doltgres_scan_advance_closure_1,
							 &frame->entry);
	if (!frame->entry.ok)
		return;
	if (!frame->entry.has_entry && !frame->has_delta)
	{
		DoltgresScanPlace start = {.tag = DOLTGRES_SCAN_PLACE_START};

		doltgres_scan_Place_set(&scan->place, start);
		frame->result->tag = DOLTGRES_SCAN_TURN_END;
		frame->ok = true;
		return;
	}
	if (frame->entry.has_entry && frame->has_delta &&
		std_bytes_Ord_cmp(frame->entry.key.ptr, frame->entry.key.len, frame->delta.f0.ptr,
						  frame->delta.f0.len) == STD_ORDERING_EQUAL)
	{
		key = frame->entry.key;
		copies = frame->entry.copies;
		has_copies = true;
		versions = frame->delta.f1;
		std_vec_free_Checked(&frame->delta.f0, DOLTGRES_SCAN_BYTES);
	}
	else if (frame->entry.has_entry && frame->has_delta && doltgres_scan_past(&frame->entry.key, &frame->delta.f0, way))
	{
		key = frame->delta.f0;
		versions = frame->delta.f1;
		doltgres_scan_entry_drop(&frame->entry);
	}
	else if (frame->entry.has_entry)
	{
		key = frame->entry.key;
		copies = frame->entry.copies;
		has_copies = true;
		std_vec_new(&versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
		doltgres_scan_delta_drop(&frame->has_delta, &frame->delta);
	}
	else
	{
		key = frame->delta.f0;
		versions = frame->delta.f1;
	}
	frame->entry.has_entry = false;
	frame->has_delta = false;
	doltgres_scan_took(&scan->place, has_copies);
	if (!doltgres_scan_grouped(frame->io, frame->under, &scan->epochs, &key, has_copies, &copies, &versions,
							   &scan->group, frame->error))
		return;
	scan->has_group = true;
	frame->result->tag = DOLTGRES_SCAN_TURN_AGAIN;
	frame->ok = true;
}

/*
 * doltgres_scan_advance moves a byte-ordered table's scan to its next key
 * the way a read goes and makes that key's group: the nearer of the base's
 * next entry and the delta's next key with a version past the group's key,
 * by their bytes, one group when they are the same key. With neither, the
 * scan starts over. It reads the delta first and, when an install moved the
 * storage's base since the scan took its own (txn_state_moved_past),
 * changes nothing and answers Moved; so the delta it pairs with its base is
 * the delta before any newer base's drop.
 */
static bool
doltgres_scan_advance(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
					  DoltgresScanScan *scan, DoltgresScanDirection way, DoltgresScanTurn *result, HostError *error)
{
	doltgres_scan_advance_frame_t frame = {.io = io, .stores = stores, .under = under, .scan = scan, .way = way,
	.result = result, .error = error};

	std_landing(&frame, doltgres_scan_advance_body, doltgres_scan_advance_pad);
	doltgres_scan_advance_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_chosen_frame_t is chosen's state across its pad: the
 * choice, with where the run's split goes, the delta's hold, and the key's
 * versions, while live, and whether an install moved the base.
 */
typedef struct doltgres_scan_chosen_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	DoltgresScanScan *scan;
	DoltgresScanDirection way;
	bool		has_at;
	size_t		at;
	bool		delta_key;
	StdVec		key;
	bool		key_live;
	bool		has_copies;
	DoltgresScanCopies copies;
	HostDeltaHold hold;
	bool		hold_live;
	bool	   *moved;
	HostError  *error;
	bool		ok;
} doltgres_scan_chosen_frame_t;

/* doltgres_scan_chosen_pad drops what chosen holds. */
static void
doltgres_scan_chosen_pad(void *frame_)
{
	doltgres_scan_chosen_frame_t *frame = frame_;

	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	if (frame->has_copies)
		doltgres_scan_Copies_drop(&frame->copies);
	frame->has_copies = false;
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, DOLTGRES_SCAN_BYTES);
	frame->key_live = false;
}

/*
 * doltgres_scan_chosen_body reads the delta's versions of the key, when the
 * group takes the delta's key, and, unless an install moved the base, moves
 * the run's split and makes the group.
 */
static void
doltgres_scan_chosen_body(void *frame_)
{
	doltgres_scan_chosen_frame_t *frame = frame_;
	DoltgresScanScan *scan = frame->scan;
	DoltgresScanDirection way = frame->way;
	StdVec		versions;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (frame->delta_key)
	{
		if (!txn_visible_versions(frame->io, &frame->hold, frame->under->state, frame->key.ptr, frame->key.len,
								  scan->folded, &versions, frame->error))
			return;
	}
	else
		std_vec_new(&versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	*frame->moved = txn_state_moved_past(frame->under->state, scan->generation);
	if (*frame->moved)
	{
		txn_memtable_version_versions_drop(&versions, 0);
		frame->ok = true;
		return;
	}
	doltgres_scan_option_drop(&scan->seek);
	if (scan->has_run)
	{
		if (frame->has_at && (frame->delta_key || way == DOLTGRES_SCAN_DIRECTION_BACKWARD))
			scan->run.split = frame->at + 1;
		else if (frame->has_at)
			scan->run.split = frame->at;
		else if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
			scan->run.split = scan->run.keys.len;
		else
			scan->run.split = 0;
	}
	doltgres_scan_took(&scan->place, frame->has_copies);
	frame->key_live = false;
	if (!doltgres_scan_grouped(frame->io, frame->under, &scan->epochs, &frame->key, frame->has_copies,
							   &frame->copies, &versions, &scan->group, frame->error))
	{
		frame->has_copies = false;
		return;
	}
	frame->has_copies = false;
	scan->has_group = true;
	frame->ok = true;
}

/*
 * doltgres_scan_chosen makes a compared table's next group from its
 * candidates as chosen, which it takes, the delta's key's versions read
 * again by the key, and moves the run's split past the keys at or before the
 * group's. It reads the delta key's versions first and, when an install
 * moved the storage's base since the scan took its own
 * (txn_state_moved_past), changes nothing and answers moved, as advance
 * answers Moved.
 */
static bool
doltgres_scan_chosen(HostIoIo *io, const DoltgresScanUnder *under, DoltgresScanScan *scan,
					 DoltgresScanPicked *picked, DoltgresScanDirection way, bool *moved, HostError *error)
{
	doltgres_scan_chosen_frame_t frame = {.io = io, .under = under, .scan = scan, .way = way, .key_live = true,
	.moved = moved, .error = error};

	switch (picked->tag)
	{
		case DOLTGRES_SCAN_PICKED_BASE:
			frame.key = picked->base_key;
			frame.copies = picked->base_copies;
			frame.has_copies = true;
			frame.has_at = picked->has_before;
			frame.at = picked->before;
			break;
		case DOLTGRES_SCAN_PICKED_DELTA:
			frame.key = picked->delta_key;
			frame.has_at = true;
			frame.at = picked->delta_at;
			frame.delta_key = true;
			break;
		case DOLTGRES_SCAN_PICKED_BOTH:
		default:
			std_vec_free_Checked(&picked->base_key, DOLTGRES_SCAN_BYTES);
			frame.key = picked->delta_key;
			frame.copies = picked->base_copies;
			frame.has_copies = true;
			frame.has_at = true;
			frame.at = picked->delta_at;
			frame.delta_key = true;
			break;
	}
	std_landing(&frame, doltgres_scan_chosen_body, doltgres_scan_chosen_pad);
	doltgres_scan_chosen_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_gather_frame_t is gather's state across its pad: the run's
 * next key and the base's entry, each while live.
 */
typedef struct doltgres_scan_gather_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	DoltgresScanScan *scan;
	DoltgresScanDirection way;
	bool		has_delta;
	StdVec		delta_key;
	size_t		delta_at;
	doltgres_scan_base_read_t entry;
	DoltgresScanTurn *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_gather_frame_t;

/* doltgres_scan_gather_pad drops what gather holds. */
static void
doltgres_scan_gather_pad(void *frame_)
{
	doltgres_scan_gather_frame_t *frame = frame_;

	doltgres_scan_entry_drop(&frame->entry);
	if (frame->has_delta)
		std_vec_free_Checked(&frame->delta_key, DOLTGRES_SCAN_BYTES);
	frame->has_delta = false;
}

/*
 * doltgres_scan_gather_body takes the run's next key and the base's next entry
 * and makes, or asks to compare, the next group.
 */
static void
doltgres_scan_gather_body(void *frame_)
{
	doltgres_scan_gather_frame_t *frame = frame_;
	DoltgresScanScan *scan = frame->scan;
	DoltgresScanRun *run = &scan->run;
	DoltgresScanDirection way = frame->way;
	bool		seeked = !scan->has_group && scan->seek.has;
	bool		has_at;
	size_t		at = 0;
	DoltgresScanPicked picked = {0};
	bool		moved;

	if (!scan->has_group && seeked && way == DOLTGRES_SCAN_DIRECTION_FORWARD)
	{
		has_at = run->split < run->keys.len;
		at = run->split;
	}
	else if (!scan->has_group && seeked)
	{
		has_at = run->split > 0;
		at = run->split - 1;
	}
	else if (!scan->has_group && way == DOLTGRES_SCAN_DIRECTION_FORWARD)
		has_at = run->keys.len > 0;
	else if (!scan->has_group)
	{
		has_at = run->keys.len > 0;
		at = run->keys.len - 1;
	}
	else if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
	{
		has_at = run->split < run->keys.len;
		at = run->split;
	}
	else
	{
		has_at = run->split > 0;
		at = run->split - 1;
		if (has_at)
		{
			const StdVec *last = &STD_VEC_AT(&run->keys, StdVec, at);

			if (std_bytes_Ord_cmp(last->ptr, last->len, scan->group.key.ptr, scan->group.key.len) ==
				STD_ORDERING_EQUAL)
			{
				has_at = at > 0;
				at = at - 1;
			}
		}
	}
	if (has_at)
	{
		std_vec_clone(&frame->delta_key, &STD_VEC_AT(&run->keys, StdVec, at), DOLTGRES_SCAN_BYTES);
		frame->delta_at = at;
		frame->has_delta = true;
	}
	frame->entry.scan = scan;
	frame->entry.keyless = scan->keyed.len == 0;
	frame->entry.entries = scan->index && !scan->records ? &scan->order : NULL;
	frame->entry.way = way;
	frame->entry.error = frame->error;
	frame->stores->ops->with(frame->stores->self, frame->io, &scan->reader, doltgres_scan_gather_closure_2,
							 &frame->entry);
	if (!frame->entry.ok)
		return;
	if (!frame->entry.has_entry && !frame->has_delta)
	{
		DoltgresScanPlace start = {.tag = DOLTGRES_SCAN_PLACE_START};

		if (scan->has_group)
			doltgres_scan_Group_drop(&scan->group);
		scan->has_group = false;
		doltgres_scan_Place_set(&scan->place, start);
		doltgres_scan_option_drop(&scan->seek);
		frame->result->tag = DOLTGRES_SCAN_TURN_END;
		frame->ok = true;
		return;
	}
	if (frame->entry.has_entry && frame->has_delta)
	{
		trees_KeyOrder_Clone_clone(&scan->order, &frame->result->u.compare.order);
		frame->result->tag = DOLTGRES_SCAN_TURN_COMPARE;
		frame->result->u.compare.base_key = frame->entry.key;
		frame->result->u.compare.base_copies = frame->entry.copies;
		frame->result->u.compare.delta_key = frame->delta_key;
		frame->result->u.compare.delta_at = frame->delta_at;
		frame->entry.has_entry = frame->has_delta = false;
		frame->ok = true;
		return;
	}
	if (frame->entry.has_entry)
	{
		picked.tag = DOLTGRES_SCAN_PICKED_BASE;
		picked.base_key = frame->entry.key;
		picked.base_copies = frame->entry.copies;
	}
	else
	{
		picked.tag = DOLTGRES_SCAN_PICKED_DELTA;
		picked.delta_key = frame->delta_key;
		picked.delta_at = frame->delta_at;
	}
	frame->entry.has_entry = frame->has_delta = false;
	if (!doltgres_scan_chosen(frame->io, frame->under, scan, &picked, way, &moved, frame->error))
		return;
	frame->result->tag = moved ? DOLTGRES_SCAN_TURN_MOVED : DOLTGRES_SCAN_TURN_AGAIN;
	frame->ok = true;
}

/*
 * doltgres_scan_gather is a compared table's advance in the scan's record:
 * it asks for the delta's run when the scan has none, and otherwise takes
 * the base's next entry and the run's next key the way a read goes, past the
 * group's, and makes the group of the one there is, or gives both for a
 * comparison outside the record, or starts the scan over with neither.
 */
static bool
doltgres_scan_gather(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
					 DoltgresScanScan *scan, DoltgresScanDirection way, DoltgresScanTurn *result, HostError *error)
{
	doltgres_scan_gather_frame_t frame = {.io = io, .stores = stores, .under = under, .scan = scan, .way = way,
	.result = result, .error = error};

	if (!scan->has_run)
	{
		result->tag = DOLTGRES_SCAN_TURN_RUN;
		trees_KeyOrder_Clone_clone(&scan->order, &result->u.run);
		return true;
	}
	std_landing(&frame, doltgres_scan_gather_body, doltgres_scan_gather_pad);
	doltgres_scan_gather_pad(&frame);
	return frame.ok;
}

/* DOLTGRES_SCAN_TURN_DROP drops a Turn a call left. */
#define DOLTGRES_SCAN_TURN_DROP(place) doltgres_scan_Turn_drop(place)

/* Scan's Table's with at Turn, for next's and next_parallel's turns. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS Turn
#define HOST_OWNED_WITH_T DoltgresScanTurn
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_TURN_DROP(place)
#include "doltgres/host/owned_template.h"

/* doltgres_scan_TupleOptionVecU8VecVecU8_t is an (Option<Vec<u8>>, Vec<Vec<u8>>): a seek's key and a run's keys. */
typedef struct doltgres_scan_TupleOptionVecU8VecVecU8_t
{
	doltgres_OptionVecU8_t f0;
	StdVec		f1;
} doltgres_scan_TupleOptionVecU8VecVecU8_t;

/* DOLTGRES_SCAN_SEEK_KEYS_DROP drops a seek's key and a run's keys a call left. */
#define DOLTGRES_SCAN_SEEK_KEYS_DROP(place) \
	do { \
		doltgres_scan_option_drop(&(place)->f0); \
		doltgres_scan_vecs_drop(&(place)->f1); \
	} while (0)

/* Scan's Table's with at (Option<Vec<u8>>, Vec<Vec<u8>>), for next's run. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS TupleOptionVecU8VecVecU8
#define HOST_OWNED_WITH_T doltgres_scan_TupleOptionVecU8VecVecU8_t
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_SEEK_KEYS_DROP(place)
#include "doltgres/host/owned_template.h"

/* doltgres_scan_generation_closure_0 is a read's look in the record: the base generation the scan took its base at. */
static bool
doltgres_scan_generation_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, uint64 *result,
								   HostError *error)
{
	*result = scan->generation;
	return true;
}

/* doltgres_scan_turn_t is a read's turn's captures: the store's source, what the read is under, and its way. */
typedef struct doltgres_scan_turn_t
{
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	DoltgresScanDirection way;
} doltgres_scan_turn_t;

/* doltgres_scan_next_closure_0 is next's turn in the record: the group's next row, or else an advance or a gather. */
static bool
doltgres_scan_next_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, DoltgresScanTurn *result,
							 HostError *error)
{
	doltgres_scan_turn_t *turn = captures;

	if (doltgres_scan_given(scan, turn->way, &result->u.row))
	{
		result->tag = DOLTGRES_SCAN_TURN_ROW;
		return true;
	}
	if (trees_KeyOrder_is_bytes(&scan->order))
		return doltgres_scan_advance(io, turn->stores, turn->under, scan, turn->way, result, error);
	return doltgres_scan_gather(io, turn->stores, turn->under, scan, turn->way, result, error);
}

/* doltgres_scan_keys_drop frees the keys of a call that never ran. */
static void
doltgres_scan_keys_drop(void *keys)
{
	doltgres_scan_vecs_drop(keys);
}

/* doltgres_scan_next_closure_1 is next's look in the record: its seek's key, cloned, and the run's keys it reads. */
static bool
doltgres_scan_next_closure_1(void *captures, HostIoIo *io, DoltgresScanScan *scan,
							 doltgres_scan_TupleOptionVecU8VecVecU8_t *result, HostError *error)
{
	StdVec	   *keys = captures;

	result->f0.has = scan->seek.has;
	if (scan->seek.has)
		std_vec_clone(&result->f0.v, &scan->seek.v, DOLTGRES_SCAN_BYTES);
	doltgres_scan_Scan_entries(scan, keys);
	result->f1 = *keys;
	return true;
}

/* doltgres_scan_run_set_t is the captures of a call that puts a run in the record: its keys and its split. */
typedef struct doltgres_scan_run_set_t
{
	StdVec		keys;
	size_t		split;
} doltgres_scan_run_set_t;

/* doltgres_scan_run_set_drop frees the run of a call that never ran. */
static void
doltgres_scan_run_set_drop(void *captures)
{
	doltgres_scan_vecs_drop(&((doltgres_scan_run_set_t *) captures)->keys);
}

/* doltgres_scan_next_closure_2 is next's put of the run in the record, which drops the run it held. */
static bool
doltgres_scan_next_closure_2(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_run_set_t *set = captures;

	if (scan->has_run)
		doltgres_scan_vecs_drop(&scan->run.keys);
	scan->run.keys = set->keys;
	scan->run.split = set->split;
	scan->has_run = true;
	return true;
}

/*
 * doltgres_scan_chose_t is the captures of a call that makes the chosen group
 * in the record: the choice, the read's state, its way, and whether an
 * install moved the base.
 */
typedef struct doltgres_scan_chose_t
{
	DoltgresScanPicked picked;
	const DoltgresScanUnder *under;
	DoltgresScanDirection way;
	bool		moved;
} doltgres_scan_chose_t;

/* doltgres_scan_chose_drop frees the choice of a call that never ran. */
static void
doltgres_scan_chose_drop(void *captures)
{
	doltgres_scan_Picked_drop(&((doltgres_scan_chose_t *) captures)->picked);
}

/* doltgres_scan_next_closure_3 is next's making of the chosen group in the record. */
static bool
doltgres_scan_next_closure_3(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_chose_t *chose = captures;

	return doltgres_scan_chosen(io, chose->under, scan, &chose->picked, chose->way, &chose->moved, error);
}

/*
 * doltgres_scan_next_frame_t is next's state across its pad: a turn's
 * order, the run's keys, and the seek's key, each while live.
 */
typedef struct doltgres_scan_next_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	DoltgresScanDirection way;
	TreesKeyOrder order;
	bool		order_live;
	StdVec		keys;
	bool		keys_live;
	doltgres_OptionVecU8_t from;
	bool	   *has_result;
	DoltgresScanRow *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_next_frame_t;

/* doltgres_scan_next_pad drops what a turn holds. */
static void
doltgres_scan_next_pad(void *frame_)
{
	doltgres_scan_next_frame_t *frame = frame_;

	doltgres_scan_option_drop(&frame->from);
	if (frame->keys_live)
		doltgres_scan_vecs_drop(&frame->keys);
	frame->keys_live = false;
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->order_live = false;
}

/*
 * doltgres_scan_next_body turns the read until it gives a row or the end,
 * making runs and comparisons outside the record.
 */
static void
doltgres_scan_next_body(void *frame_)
{
	doltgres_scan_next_frame_t *frame = frame_;
	doltgres_scan_turn_t turn = {frame->stores, frame->under, frame->way};

	for (;;)
	{
		DoltgresScanTurn step;

		if (!host_owned_TableDoltgresScanScan_withTurn(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
													   doltgres_scan_next_closure_0, &turn, NULL, &step,
													   frame->error))
			return;
		if (step.tag == DOLTGRES_SCAN_TURN_ROW || step.tag == DOLTGRES_SCAN_TURN_END)
		{
			*frame->has_result = step.tag == DOLTGRES_SCAN_TURN_ROW;
			if (*frame->has_result)
				*frame->result = step.u.row;
			frame->ok = true;
			return;
		}
		if (step.tag == DOLTGRES_SCAN_TURN_MOVED)
		{
			if (!doltgres_scan_reseated(frame->io, frame->stores, frame->under, frame->handle, frame->way,
										frame->error))
				return;
		}
		else if (step.tag == DOLTGRES_SCAN_TURN_RUN)
		{
			doltgres_scan_TupleOptionVecU8VecVecU8_t looked;
			doltgres_scan_run_set_t set = {0};
			uint64		generation;

			frame->order = step.u.run;
			frame->order_live = true;
			if (!doltgres_scan_run(frame->io, frame->under, &frame->order, &frame->keys, frame->error))
				return;
			frame->keys_live = true;
			if (!host_owned_TableDoltgresScanScan_withU64(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														  doltgres_scan_generation_closure_0, NULL, NULL, &generation,
														  frame->error))
				return;
			if (txn_state_moved_past(frame->under->state, generation))
			{
				doltgres_scan_next_pad(frame);
				if (!doltgres_scan_reseated(frame->io, frame->stores, frame->under, frame->handle, frame->way,
											frame->error))
					return;
				continue;
			}
			frame->keys_live = false;
			if (!host_owned_TableDoltgresScanScan_withTupleOptionVecU8VecVecU8(&DOLTGRES_SCAN_SCANS, frame->io,
																			   frame->handle,
																			   doltgres_scan_next_closure_1,
																			   &frame->keys, doltgres_scan_keys_drop,
																			   &looked, frame->error))
				return;
			frame->from = looked.f0;
			frame->keys = looked.f1;
			frame->keys_live = true;
			if (frame->from.has &&
				!doltgres_scan_split(frame->io, &frame->keys, frame->from.v.ptr, frame->from.v.len, frame->way,
									 &frame->order, &set.split, frame->error))
				return;
			set.keys = frame->keys;
			frame->keys_live = false;
			if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														   doltgres_scan_next_closure_2, &set,
														   doltgres_scan_run_set_drop, frame->error))
				return;
			doltgres_scan_next_pad(frame);
		}
		else if (step.tag == DOLTGRES_SCAN_TURN_COMPARE)
		{
			doltgres_scan_chose_t chose = {.under = frame->under, .way = frame->way};

			frame->order = step.u.compare.order;
			frame->order_live = true;
			if (!doltgres_scan_pick(frame->io, &step.u.compare.base_key, &step.u.compare.base_copies,
									&step.u.compare.delta_key, step.u.compare.delta_at, &frame->order, frame->way,
									&chose.picked, frame->error))
				return;
			if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														   doltgres_scan_next_closure_3, &chose,
														   doltgres_scan_chose_drop, frame->error))
				return;
			doltgres_scan_next_pad(frame);
			if (chose.moved &&
				!doltgres_scan_reseated(frame->io, frame->stores, frame->under, frame->handle, frame->way,
										frame->error))
				return;
		}
	}
}

/*
 * doltgres_scan_next reads the scan's next row in a direction under its
 * snapshot and answers it, or nothing when there is none: the row after the
 * last read's that way among its key's rows, or else the first row of the
 * next key that way, the base's and the delta's keys merged in the table's
 * order, or the first row in that direction when the scan starts over. A
 * read that finds no row starts the scan over.
 */
bool
doltgres_scan_next(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
				   HostOwnedHandle handle, DoltgresScanDirection way, bool *has_result, DoltgresScanRow *result,
				   HostError *error)
{
	doltgres_scan_next_frame_t frame = {.io = io, .stores = stores, .under = under, .handle = handle, .way = way,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, doltgres_scan_next_body, doltgres_scan_next_pad);
	doltgres_scan_next_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_run_frame_t is run's state across its pad: the keys read so far and the delta's hold, while live. */
typedef struct doltgres_scan_run_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	const TreesKeyOrder *order;
	StdVec		keys;
	bool		keys_live;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		positions;
	bool		positions_live;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_scan_run_frame_t;

/* doltgres_scan_taken_drop frees a Vec<Option<Vec<u8>>> whose taken keys are NULL. */
static void
doltgres_scan_taken_drop(StdVec *keys)
{
	for (size_t i = 0; i < keys->len; i++)
	{
		StdVec	   *key = &STD_VEC_AT(keys, StdVec, i);

		if (key->ptr != NULL)
			std_vec_free_Checked(key, DOLTGRES_SCAN_BYTES);
	}
	std_vec_free_Checked(keys, DOLTGRES_SCAN_VEC);
}

/* doltgres_scan_run_pad drops what run holds. */
static void
doltgres_scan_run_pad(void *frame_)
{
	doltgres_scan_run_frame_t *frame = frame_;

	if (frame->positions_live)
		std_vec_free_Checked(&frame->positions, sizeof(size_t), _Alignof(size_t));
	frame->positions_live = false;
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	if (frame->result_live)
		doltgres_scan_vecs_drop(frame->result);
	frame->result_live = false;
	if (frame->keys_live)
		doltgres_scan_taken_drop(&frame->keys);
	frame->keys_live = false;
}

/* doltgres_scan_run_body reads the delta's keys in byte order and sorts them by the table's order. */
static void
doltgres_scan_run_body(void *frame_)
{
	doltgres_scan_run_frame_t *frame = frame_;
	StdVec		refs;
	ChunksErrorError failed;
	bool		sorted;

	std_vec_new(&frame->keys, DOLTGRES_SCAN_VEC);
	frame->keys_live = true;
	for (;;)
	{
		const StdVec *after = frame->keys.len > 0 ? &STD_VEC_AT(&frame->keys, StdVec, frame->keys.len - 1) : NULL;
		bool		some;
		txn_TupleVecU8VecVersion_t next;

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_memtable_table_next(frame->io, &frame->hold, frame->under->state, after != NULL,
									 after == NULL ? NULL : after->ptr, after == NULL ? 0 : after->len,
									 TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &some, &next, frame->error))
			return;
		if (!some)
		{
			frame->hold_live = false;
			host_delta_Hold_drop(&frame->hold);
			break;
		}
		txn_memtable_version_versions_drop(&next.f1, 0);
		std_vec_push_Checked(&frame->keys, &next.f0, DOLTGRES_SCAN_VEC);
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	std_vec_with_capacity_Checked(&refs, frame->keys.len, sizeof(StdBytes), _Alignof(StdBytes));
	for (size_t i = 0; i < frame->keys.len; i++)
	{
		const StdVec *key = &STD_VEC_AT(&frame->keys, StdVec, i);
		StdBytes	ref = {key->ptr, key->len};

		std_vec_push_Checked(&refs, &ref, sizeof(StdBytes), _Alignof(StdBytes));
	}
	sorted = trees_sorted_positions(frame->io, refs.ptr, refs.len, frame->order, &frame->positions, &failed);
	std_vec_free_Checked(&refs, sizeof(StdBytes), _Alignof(StdBytes));
	if (!sorted)
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->positions_live = true;
	std_vec_new(frame->result, DOLTGRES_SCAN_VEC);
	frame->result_live = true;
	for (size_t i = 0; i < frame->positions.len; i++)
	{
		size_t		at = STD_VEC_AT(&frame->positions, size_t, i);
		StdVec	   *key;

		std_index_check(at, frame->keys.len, RUST_FILE, 717, 46);
		key = &STD_VEC_AT(&frame->keys, StdVec, at);
		if (key->ptr == NULL)
			continue;
		std_vec_push_Checked(frame->result, key, DOLTGRES_SCAN_VEC);
		key->ptr = NULL;
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_run reads every key of a compared table's delta that has a
 * version, in byte order, and sorts them by the table's order with ties
 * broken by their bytes, so that keys its comparator finds equal make
 * adjacent groups; it runs with no record lent, since the comparator is user
 * code. It pauses at doltgres-scan-run first, so a case counts its reads.
 */
static bool
doltgres_scan_run(HostIoIo *io, const DoltgresScanUnder *under, const TreesKeyOrder *order, StdVec *result,
				  HostError *error)
{
	doltgres_scan_run_frame_t frame = {.io = io, .under = under, .order = order, .result = result, .error = error};

	if (!host_pause(HOST_SHARED_IO_PAUSES(io), "doltgres-scan-run", error))
		return false;
	std_landing(&frame, doltgres_scan_run_body, doltgres_scan_run_pad);
	doltgres_scan_run_pad(&frame);
	return frame.ok;
}

/* trees' pin at Result<Option<(Vec<u8>, Copies)>, Error>, for base_next. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionTupleVecU8Copies
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *has_result, StdVec *result_key, DoltgresScanCopies *result_copies, \
	HostError *error
#define TREES_PIN_RESULT_FIELDS bool *has_result; StdVec *result_key; DoltgresScanCopies *result_copies; \
	HostError *error;
#define TREES_PIN_RESULT_INIT .has_result = has_result, .result_key = result_key, .result_copies = result_copies, \
	.error = error
#define TREES_PIN_RESULT_ARGS , frame->has_result, frame->result_key, frame->result_copies, frame->error
#include "doltgres/trees/pin_template.h"

/* trees' pin at Result<Place, Error>, for seek's and seek_compared's placed. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultPlace
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , DoltgresScanPlace *result, HostError *error
#define TREES_PIN_RESULT_FIELDS DoltgresScanPlace *result; HostError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * doltgres_scan_base_next_closure_0_captures_t is base_next's pin scope's
 * captures: the base's tree, how its entries read, the scan's place, its
 * bound, and the way.
 */
typedef struct doltgres_scan_base_next_closure_0_captures_t
{
	TreesStoreTree tree;
	bool		keyless;
	bool		index;
	const TreesKeyOrder *entries;
	DoltgresScanPlace *place;
	const doltgres_OptionVecU8_t *bound;
	DoltgresScanDirection way;
} doltgres_scan_base_next_closure_0_captures_t;

/*
 * doltgres_scan_base_next_closure_0_frame_t is the pin scope's state across
 * its pad: its cursor and the entry's stamps, values, and key, while live.
 */
typedef struct doltgres_scan_base_next_closure_0_frame_t
{
	doltgres_scan_base_next_closure_0_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	StdVec		stamps;
	bool		stamps_live;
	StdVec		values;
	bool		values_live;
	StdVec		key;
	bool		key_live;
	bool	   *has_result;
	StdVec	   *result_key;
	DoltgresScanCopies *result_copies;
	HostError  *error;
	bool		ok;
} doltgres_scan_base_next_closure_0_frame_t;

/* doltgres_scan_base_next_closure_0_pad drops what the scope holds. */
static void
doltgres_scan_base_next_closure_0_pad(void *frame_)
{
	doltgres_scan_base_next_closure_0_frame_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(&frame->key, DOLTGRES_SCAN_BYTES);
	frame->key_live = false;
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, DOLTGRES_SCAN_U32);
	frame->values_live = false;
	if (frame->stamps_live)
		std_vec_free_Checked(&frame->stamps, DOLTGRES_SCAN_STAMP);
	frame->stamps_live = false;
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->cursor_live = false;
}

/* doltgres_scan_storage_failed answers false with a storage error made of a trees error. */
static bool
doltgres_scan_storage_failed(ChunksErrorError *failed, HostError *error)
{
	*error = txn_base_storage(failed);
	return false;
}

/* doltgres_scan_base_next_closure_0_body steps the place's cursor and copies out the entry it reaches. */
static void
doltgres_scan_base_next_closure_0_body(void *frame_)
{
	doltgres_scan_base_next_closure_0_frame_t *frame = frame_;
	doltgres_scan_base_next_closure_0_captures_t *captures = frame->captures;
	DoltgresScanPlace old = *captures->place;
	DoltgresScanDirection way = captures->way;
	ChunksErrorError failed;
	StdBytes	key;
	StdBytes	tuple;
	bool		record;

	*captures->place = doltgres_scan_off(way);
	*frame->has_result = false;
	if (old.tag == DOLTGRES_SCAN_PLACE_AT)
	{
		if (!trees_cursor_KeptCursor_resume(&old.u.at.cursor, frame->io, frame->pin, &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
		if ((old.u.at.taken || old.u.at.way != way) &&
			!doltgres_scan_step(frame->io, frame->pin, &frame->cursor, way, frame->error))
			return;
	}
	else if (old.tag == DOLTGRES_SCAN_PLACE_OFF && old.u.off == way)
	{
		frame->ok = true;
		return;
	}
	else if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
	{
		if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree),
									   &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
	}
	else if (captures->entries == NULL)
	{
		if (!trees_cursor_Cursor_last(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree),
									  &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
	}
	else
	{
		uint8		records = TXN_ENTRY_RECORD;

		if (!trees_cursor_Cursor_seek_past(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree), &records,
										   1, captures->entries, &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
		if (trees_cursor_Cursor_is_valid(&frame->cursor))
		{
			if (!trees_cursor_Cursor_prev(&frame->cursor, frame->io, frame->pin, &failed))
			{
				doltgres_scan_storage_failed(&failed, frame->error);
				return;
			}
		}
		else
		{
			TreesCursorCursor last;

			if (!trees_cursor_Cursor_last(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree), &last,
										  &failed))
			{
				doltgres_scan_storage_failed(&failed, frame->error);
				return;
			}
			trees_cursor_Cursor_drop(&frame->cursor);
			frame->cursor = last;
		}
	}
	if (!trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		frame->ok = true;
		return;
	}
	key = trees_cursor_Cursor_key(&frame->cursor);
	record = captures->entries != NULL && key.len > 0 && key.ptr[0] == TXN_ENTRY_RECORD;
	if (record ||
		(captures->bound->has && captures->bound->v.len == key.len &&
		 memcmp(captures->bound->v.ptr, key.ptr, key.len) == 0))
	{
		frame->ok = true;
		return;
	}
	if (!trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, &frame->stamps, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->stamps_live = true;
	if (captures->index)
	{
		if (!txn_entry_based(trees_cursor_Cursor_value(&frame->cursor).ptr,
							 trees_cursor_Cursor_value(&frame->cursor).len, frame->stamps.ptr, frame->stamps.len,
							 captures->keyless, &tuple, frame->error))
			return;
	}
	else
	{
		uint64		count;

		if (!doltgres_scan_copies(&frame->cursor, captures->keyless, &count, &tuple, frame->error))
			return;
		if (frame->stamps.len != count)
		{
			*frame->error = doltgres_scan_uneven();
			return;
		}
	}
	if (captures->index && !captures->keyless)
	{
		ChunksTrailerStamp zero = {0};

		std_vec_push_Checked(&frame->stamps, &zero, DOLTGRES_SCAN_STAMP);
	}
	if (!trees_cursor_Cursor_values(&frame->cursor, frame->io, frame->pin, &frame->values, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->values_live = true;
	std_slice_to_vec_Checked(&frame->key, key.ptr, key.len, DOLTGRES_SCAN_BYTES);
	frame->key_live = true;
	std_slice_to_vec_Checked(&frame->result_copies->tuple, tuple.ptr, tuple.len, DOLTGRES_SCAN_BYTES);
	frame->result_copies->stamps = frame->stamps;
	frame->result_copies->values = frame->values;
	*frame->result_key = frame->key;
	frame->key_live = frame->stamps_live = frame->values_live = false;
	frame->cursor_live = false;
	captures->place->tag = DOLTGRES_SCAN_PLACE_AT;
	trees_cursor_Cursor_keep(&frame->cursor, &captures->place->u.at.cursor);
	captures->place->u.at.way = way;
	captures->place->u.at.taken = false;
	*frame->has_result = true;
	frame->ok = true;
}

/* doltgres_scan_base_next_closure_0 is base_next's pin scope: the base's next entry past the place. */
static bool
doltgres_scan_base_next_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *has_result,
								  StdVec *result_key, DoltgresScanCopies *result_copies, HostError *error)
{
	doltgres_scan_base_next_closure_0_frame_t frame = {.captures = captures, .io = io, .pin = pin,
	.has_result = has_result, .result_key = result_key, .result_copies = result_copies, .error = error};

	std_landing(&frame, doltgres_scan_base_next_closure_0_body, doltgres_scan_base_next_closure_0_pad);
	doltgres_scan_base_next_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_base_next answers the next entry of a base's tree, or of
 * none, past the base's place the way a read goes, its key and rows copied
 * out under the leaf's pins, with the place left at it, or none off the
 * base's end that way. An entry whose stamps differ in number from its
 * copies is data corruption. An index's entry is read as the index fold
 * reads it, and a read of an index's entries (NULL entries for none) ends
 * before its records.
 */
static bool
doltgres_scan_base_next(HostIoIo *io, TreesStoreNodeStore store, bool has_tree, TreesStoreTree tree, bool keyless,
						bool index, const TreesKeyOrder *entries, DoltgresScanPlace *place,
						const doltgres_OptionVecU8_t *bound, DoltgresScanDirection way, bool *has_result,
						StdVec *result_key, DoltgresScanCopies *result_copies, HostError *error)
{
	doltgres_scan_base_next_closure_0_captures_t captures = {tree, keyless, index, entries, place, bound, way};

	if (!has_tree)
	{
		*has_result = false;
		return true;
	}
	return trees_pin_pin_ResultOptionTupleVecU8Copies(io, store, doltgres_scan_base_next_closure_0, &captures, NULL,
													  has_result, result_key, result_copies, error);
}

/*
 * doltgres_scan_stamped returns what a row's stamp shows of its changer and
 * its commander, NULL for none, or false for no changer.
 */
static bool
doltgres_scan_stamped(TxnMemtableVersionVersion *changer, TxnMemtableVersionVersion *commander,
					  DoltgresScanChanger *result)
{
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;

	result->command = DOLTGRES_SCAN_INVALID_COMMAND;
	if (commander != NULL)
	{
		record = txn_memtable_version_Version_record(commander);
		result->command = txn_memtable_version_Record_ended(&record);
	}
	if (changer == NULL)
		return false;
	record = txn_memtable_version_Version_record(changer);
	result->xid = txn_memtable_version_Record_writer(&record).xid;
	op = txn_memtable_version_Record_op(&record);
	result->locks = op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW;
	return true;
}

/* doltgres_scan_Changer_Debug_fmt is Changer's derived Debug. */
bool
doltgres_scan_Changer_Debug_fmt(const void *value, StdFormatter *f)
{
	const DoltgresScanChanger *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Changer", 7);

	std_DebugStruct_field(&s, "xid", 3, &self->xid, std_u64_Debug_fmt);
	std_DebugStruct_field(&s, "command", 7, &self->command, std_u32_Debug_fmt);
	std_DebugStruct_field(&s, "locks", 5, &self->locks, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* doltgres_scan_Changer_PartialEq_eq is Changer's derived PartialEq, field by field. */
bool
doltgres_scan_Changer_PartialEq_eq(const DoltgresScanChanger *self, const DoltgresScanChanger *other)
{
	return self->xid == other->xid && self->command == other->command && self->locks == other->locks;
}

/* doltgres_scan_value_bytes collects a row's value ids out of line, four bytes little-endian each. */
static void
doltgres_scan_value_bytes(const uint32 *ids, size_t len, StdVec *result)
{
	std_vec_new(result, DOLTGRES_SCAN_BYTES);
	for (size_t i = 0; i < len; i++)
	{
		uint8		bytes[4];

		std_u32_to_le_bytes(ids[i], bytes);
		std_vec_extend_from_slice_Checked(result, bytes, 4, DOLTGRES_SCAN_BYTES);
	}
}

/*
 * doltgres_scan_found makes a delta row version a snapshot sees a group's
 * row: the value its version puts, the transform its read takes by its
 * epoch, its writer's xid and its tuple id as its stamp, its writer's
 * top-level xid, its raw command, what its stamp shows above it or its update
 * carried, and what a Dirty read found running; made counts the fields it has
 * made, which an unwind frees.
 */
static bool
doltgres_scan_found(HostIoIo *io, DoltgresScanEpochs *epochs, const TxnVisibleSeenRow *seen,
					DoltgresScanFound *result, int *made, HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(seen->row.version);
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
	TxnMemtableVersionValues values = txn_memtable_version_PutRef_values(&seen->row.put);
	uint32		id;

	std_slice_to_vec_Checked(&result->tuple, seen->row.put.value.ptr, seen->row.put.value.len, DOLTGRES_SCAN_BYTES);
	result->has_tuple = true;
	*made = 1;
	std_vec_new(&result->values, DOLTGRES_SCAN_BYTES);
	*made = 2;
	while (txn_memtable_version_Values_Iterator_next(&values, &id))
	{
		uint8		bytes[4];

		std_u32_to_le_bytes(id, bytes);
		std_vec_extend_from_slice_Checked(&result->values, bytes, 4, DOLTGRES_SCAN_BYTES);
	}
	if (!doltgres_scan_Epochs_of_version(epochs, io, seen->row.version, &result->transform, error))
		return false;
	*made = 3;
	result->stamp.txn = writer.xid;
	result->stamp.tuple = seen->row.put.tuple.f0;
	result->top = writer.has_top ? writer.top : writer.xid;
	result->command = txn_memtable_version_Version_raw(seen->row.version);
	result->has_changer = doltgres_scan_stamped(seen->changer, seen->commander, &result->changer);
	if (!result->has_changer && seen->row.put.has_carried)
	{
		result->has_changer = true;
		result->changer.xid = seen->row.put.carried;
		result->changer.locks = true;
	}
	result->dirty = seen->dirty;
	return true;
}

/*
 * doltgres_scan_grouped_frame_t is grouped's state across its pad: the key,
 * the base entry's copies, the versions, the snapshot's seen rows, the
 * group's rows so far, and the base rows' transform, each while live.
 */
typedef struct doltgres_scan_grouped_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	DoltgresScanEpochs *epochs;
	StdVec		key;
	bool		key_live;
	bool		has_copies;
	DoltgresScanCopies copies;
	StdVec		versions;
	bool		versions_live;
	TxnVisibleSeen seen;
	bool		seen_live;
	StdVec		rows;
	bool		rows_live;
	StdVec		transform;
	bool		transform_live;
	StdVec		base;
	bool		base_live;
	DoltgresScanFound found;
	int			found_made;
	DoltgresScanGroup *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_grouped_frame_t;

/* doltgres_scan_grouped_pad drops what grouped holds. */
static void
doltgres_scan_grouped_pad(void *frame_)
{
	doltgres_scan_grouped_frame_t *frame = frame_;

	if (frame->found_made > 2)
		std_vec_free_Checked(&frame->found.transform, DOLTGRES_SCAN_BYTES);
	if (frame->found_made > 1)
		std_vec_free_Checked(&frame->found.values, DOLTGRES_SCAN_BYTES);
	if (frame->found_made > 0 && frame->found.has_tuple)
		std_vec_free_Checked(&frame->found.tuple, DOLTGRES_SCAN_BYTES);
	frame->found_made = 0;
	if (frame->base_live)
		std_vec_free_Checked(&frame->base, DOLTGRES_SCAN_BYTES);
	frame->base_live = false;
	if (frame->transform_live)
		std_vec_free_Checked(&frame->transform, DOLTGRES_SCAN_BYTES);
	frame->transform_live = false;
	if (frame->rows_live)
	{
		std_ptr_drop_in_place_slice(frame->rows.ptr, frame->rows.len, sizeof(DoltgresScanFound),
									doltgres_scan_Found_drop);
		std_vec_free_Checked(&frame->rows, DOLTGRES_SCAN_FOUND);
	}
	frame->rows_live = false;
	if (frame->seen_live)
		txn_visible_Seen_drop(&frame->seen);
	frame->seen_live = false;
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->versions_live = false;
	if (frame->has_copies)
		doltgres_scan_Copies_drop(&frame->copies);
	frame->has_copies = false;
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, DOLTGRES_SCAN_BYTES);
	frame->key_live = false;
}

/* doltgres_scan_conflicts_push records a writer a serializable read's version conflicts out with. */
static void
doltgres_scan_conflicts_push(const DoltgresScanUnder *under, HostXid xid)
{
	std_vec_push_Checked(under->serial.conflicts, &xid, DOLTGRES_SCAN_XID);
}

/* doltgres_scan_grouped_body sees the key's versions and makes its group's rows, the base's copies first. */
static void
doltgres_scan_grouped_body(void *frame_)
{
	doltgres_scan_grouped_frame_t *frame = frame_;
	const DoltgresScanUnder *under = frame->under;
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, under->snapshot};
	const TxnVisibleSerial *serial = under->has_serial ? &under->serial.read : NULL;

	if (!txn_visible_seen(frame->io, rule, frame->versions.ptr, frame->versions.len, serial, &frame->seen,
						  frame->error))
		return;
	frame->seen_live = true;
	if (under->has_serial)
		for (size_t i = 0; i < frame->seen.conflicts.len; i++)
			doltgres_scan_conflicts_push(under, STD_VEC_AT(&frame->seen.conflicts, txn_TupleRowXid_t, i).f1);
	std_vec_new(&frame->rows, DOLTGRES_SCAN_FOUND);
	frame->rows_live = true;
	if (frame->has_copies)
	{
		if (!doltgres_scan_Epochs_transform(frame->epochs, frame->epochs->base, &frame->transform, frame->error))
			return;
		frame->transform_live = true;
		for (size_t i = 0; i < frame->copies.stamps.len; i++)
		{
			ChunksTrailerStamp stamp = STD_VEC_AT(&frame->copies.stamps, ChunksTrailerStamp, i);
			HostRow		tuple = {stamp.tuple};
			bool		kept;
			TxnVisibleDirty dirty;
			TxnMemtableVersionVersion *changer;
			TxnMemtableVersionVersion *commander;

			if (!txn_visible_Seen_base(&frame->seen, frame->io, rule, tuple, &kept, &dirty, &changer, &commander,
									   frame->error))
				return;
			if (under->has_serial)
			{
				bool		conflicts;
				HostXid		xid;

				if (!txn_visible_Seen_base_conflict(&frame->seen, frame->io, &under->serial.read, tuple, stamp.txn,
													kept, &conflicts, &xid, frame->error))
					return;
				if (conflicts)
					doltgres_scan_conflicts_push(under, xid);
			}
			if (!kept)
				continue;
			frame->found = (DoltgresScanFound)
			{
				0
			};
			frame->found_made = 2;
			doltgres_scan_value_bytes(frame->copies.values.ptr, frame->copies.values.len, &frame->found.values);
			std_vec_clone(&frame->found.transform, &frame->transform, DOLTGRES_SCAN_BYTES);
			frame->found_made = 3;
			frame->found.stamp = stamp;
			frame->found.has_changer = doltgres_scan_stamped(changer, commander, &frame->found.changer);
			frame->found.dirty = dirty;
			std_vec_push_Checked(&frame->rows, &frame->found, DOLTGRES_SCAN_FOUND);
			frame->found_made = 0;
		}
		frame->base = frame->copies.tuple;
		frame->base_live = true;
		std_vec_free_Checked(&frame->copies.stamps, DOLTGRES_SCAN_STAMP);
		std_vec_free_Checked(&frame->copies.values, DOLTGRES_SCAN_U32);
		frame->has_copies = false;
		frame->transform_live = false;
		std_vec_free_Checked(&frame->transform, DOLTGRES_SCAN_BYTES);
	}
	else
	{
		std_vec_new(&frame->base, DOLTGRES_SCAN_BYTES);
		frame->base_live = true;
	}
	for (size_t i = frame->seen.rows.len; i > 0; i--)
	{
		frame->found = (DoltgresScanFound)
		{
			0
		};
		if (!doltgres_scan_found(frame->io, frame->epochs, &STD_VEC_AT(&frame->seen.rows, TxnVisibleSeenRow, i - 1),
								 &frame->found, &frame->found_made, frame->error))
			return;
		std_vec_push_Checked(&frame->rows, &frame->found, DOLTGRES_SCAN_FOUND);
		frame->found_made = 0;
	}
	frame->result->key = frame->key;
	frame->result->base = frame->base;
	frame->result->rows = frame->rows;
	frame->result->has_at = false;
	frame->key_live = frame->base_live = frame->rows_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_grouped makes a key's group of the rows a snapshot sees,
 * taking the key, the base entry's copies, and the versions: the base
 * entry's copies the key's versions in the delta leave seen, then the
 * delta's row versions it sees, oldest first.
 */
static bool
doltgres_scan_grouped(HostIoIo *io, const DoltgresScanUnder *under, DoltgresScanEpochs *epochs, StdVec *key,
					  bool has_copies, DoltgresScanCopies *copies, StdVec *versions, DoltgresScanGroup *result,
					  HostError *error)
{
	doltgres_scan_grouped_frame_t frame = {.io = io, .under = under, .epochs = epochs, .key = *key, .key_live = true,
	.has_copies = has_copies, .versions = *versions, .versions_live = true, .result = result, .error = error};

	if (has_copies)
		frame.copies = *copies;
	std_landing(&frame, doltgres_scan_grouped_body, doltgres_scan_grouped_pad);
	doltgres_scan_grouped_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_rescan_closure_0 is rescan's call: the scan's place, group,
 * run, seek, page, sample, unit, and kept run cleared.
 */
static bool
doltgres_scan_rescan_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	DoltgresScanPlace start = {.tag = DOLTGRES_SCAN_PLACE_START};

	if (scan->has_group)
		doltgres_scan_Group_drop(&scan->group);
	scan->has_group = false;
	doltgres_scan_Place_set(&scan->place, start);
	if (scan->has_run)
		doltgres_scan_vecs_drop(&scan->run.keys);
	scan->has_run = false;
	doltgres_scan_option_drop(&scan->bound);
	doltgres_scan_option_drop(&scan->seek);
	doltgres_scan_Page_drop(&scan->page);
	scan->page = doltgres_scan_ended();
	if (scan->has_sample)
		doltgres_scan_rows_drop(&scan->sample.rows);
	scan->has_sample = false;
	if (scan->has_written)
		std_vec_free_Checked(&scan->written, DOLTGRES_SCAN_U64);
	scan->has_written = false;
	scan->records = false;
	if (scan->has_parallel)
	{
		scan->parallel.has_unit = scan->parallel.has_ranged = false;
		if (scan->parallel.has_kept)
			doltgres_scan_vecs_drop(&scan->parallel.kept);
		scan->parallel.has_kept = false;
	}
	return true;
}

/*
 * doltgres_scan_scan_rescan starts a scan over, so its next read gives the
 * base's first row in that read's direction, or, for a parallel scan, asks
 * for a unit again.
 */
bool
doltgres_scan_scan_rescan(HostIoIo *io, HostOwnedHandle handle, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle, doltgres_scan_rescan_closure_0,
													 NULL, NULL, error);
}

/*
 * doltgres_scan_placed_captures_t is placed's arguments in a pin scope: the
 * tree, the key, the way, the order, and whether the key is a group's, which
 * a newer base's place takes when the tree holds it (reseated).
 */
typedef struct doltgres_scan_placed_captures_t
{
	TreesStoreTree tree;
	const uint8 *from;
	size_t		from_len;
	DoltgresScanDirection way;
	const TreesKeyOrder *order;
	bool		grouped;
} doltgres_scan_placed_captures_t;

/* doltgres_scan_placed_frame_t is placed's state across its pad: its cursor, while live. */
typedef struct doltgres_scan_placed_frame_t
{
	doltgres_scan_placed_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	DoltgresScanPlace *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_placed_frame_t;

/* doltgres_scan_placed_pad drops the cursor. */
static void
doltgres_scan_placed_pad(void *frame_)
{
	doltgres_scan_placed_frame_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->cursor_live = false;
}

/* doltgres_scan_placed_body seeks the key the way given and keeps the cursor it finds. */
static void
doltgres_scan_placed_body(void *frame_)
{
	doltgres_scan_placed_frame_t *frame = frame_;
	doltgres_scan_placed_captures_t *captures = frame->captures;
	TreesStoreNamed tree = TREES_STORE_TREE_NAMED(&captures->tree);
	ChunksErrorError failed;

	if (captures->way == DOLTGRES_SCAN_DIRECTION_FORWARD)
	{
		if (!trees_cursor_Cursor_seek(frame->io, frame->pin, tree, captures->from, captures->from_len,
									  captures->order, &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
	}
	else
	{
		if (!trees_cursor_Cursor_seek_past(frame->io, frame->pin, tree, captures->from, captures->from_len,
										   captures->order, &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
		if (trees_cursor_Cursor_is_valid(&frame->cursor))
		{
			if (!trees_cursor_Cursor_prev(&frame->cursor, frame->io, frame->pin, &failed))
			{
				doltgres_scan_storage_failed(&failed, frame->error);
				return;
			}
		}
		else
		{
			TreesCursorCursor last;

			if (!trees_cursor_Cursor_last(frame->io, frame->pin, tree, &last, &failed))
			{
				doltgres_scan_storage_failed(&failed, frame->error);
				return;
			}
			trees_cursor_Cursor_drop(&frame->cursor);
			frame->cursor = last;
		}
	}
	if (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
		bool		taken = captures->grouped &&
			std_bytes_Ord_cmp(key.ptr, key.len, captures->from, captures->from_len) == STD_ORDERING_EQUAL;

		frame->result->tag = DOLTGRES_SCAN_PLACE_AT;
		frame->cursor_live = false;
		trees_cursor_Cursor_keep(&frame->cursor, &frame->result->u.at.cursor);
		frame->result->u.at.way = captures->way;
		frame->result->u.at.taken = taken;
	}
	else
		*frame->result = doltgres_scan_off(captures->way);
	frame->ok = true;
}

/*
 * doltgres_scan_placed answers a base's place at a key in its tree's order:
 * its first entry not below the key going forward, or its last not above it
 * going backward, or off its end that way; at a group's key the tree holds,
 * the place is taken, so the next advance steps past it whichever way it
 * goes, as an advance leaves a nearer entry its group didn't take untaken.
 */
static bool
doltgres_scan_placed(void *captures, HostIoIo *io, TreesPinPin *pin, DoltgresScanPlace *result, HostError *error)
{
	doltgres_scan_placed_frame_t frame = {.captures = captures, .io = io, .pin = pin, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_placed_body, doltgres_scan_placed_pad);
	doltgres_scan_placed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_place_read_t is a seek's placing under a reader, a
 * Stores::with call's captures: placed's arguments and its answer.
 */
typedef struct doltgres_scan_place_read_t
{
	doltgres_scan_placed_captures_t placed;
	DoltgresScanPlace place;
	HostError  *error;
	bool		ok;
} doltgres_scan_place_read_t;

/*
 * doltgres_scan_place_read finds the base's place at the key in a pin scope,
 * seek's and seek_compared's store calls' body.
 */
static void
doltgres_scan_place_read(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_place_read_t *read = captures;

	read->ok = trees_pin_pin_ResultPlace(io, store, doltgres_scan_placed, &read->placed, NULL, &read->place,
										 read->error);
}

/* doltgres_scan_Trees_of returns the trees of a base, or of none (NULL), read at a base generation or after it. */
DoltgresScanTrees
doltgres_scan_Trees_of(const TxnBaseBase *base, uint64 generation)
{
	DoltgresScanTrees trees = {.generation = generation};

	if (base == NULL)
		return trees;
	trees.has_root = base->has_root;
	if (trees.has_root)
		trees.root = base->root;
	trees.has_tuple_ids = base->has_tuple_ids;
	if (trees.has_tuple_ids)
		trees.tuple_ids = base->tuple_ids;
	trees.folded = base->read;
	return trees;
}

/*
 * DoltgresScanNewest is what a read whose base an install moved reads of its
 * storage's newest base: whether the generation moved, the base, none for a
 * storage with no base, the generation it read it at, and its epochs, each
 * while live.
 */
typedef struct DoltgresScanNewest
{
	bool		moved;
	bool		some;
	TxnBaseBase base;
	uint64		now;
	DoltgresScanEpochs epochs;
	bool		epochs_live;
} DoltgresScanNewest;

/* doltgres_scan_Kept_drop is Kept's drop glue: what a scan's fetches keep between its rows. */
void
doltgres_scan_Kept_drop(void *record)
{
	DoltgresScanKept *self = record;

	txn_base_Key_drop(&self->key);
	if (self->live)
		doltgres_scan_Epochs_drop(&self->epochs);
	self->live = false;
	self->key.tag = TXN_BASE_KEY_NONE;
}

/* doltgres_scan_Newest_drop frees a newest base's read, its epochs and its base. */
static void
doltgres_scan_Newest_drop(DoltgresScanNewest *self)
{
	if (self->epochs_live)
		doltgres_scan_Epochs_drop(&self->epochs);
	self->epochs_live = false;
	if (self->some)
		txn_base_Base_drop(&self->base);
	self->some = false;
}

/*
 * doltgres_scan_newest reads a storage's newest base for a snapshot to read,
 * as readable gives it, with the generation it read it at and its epochs,
 * its chain read through a store, once an install moved the generation past
 * one a read took its base at, and nothing while it hasn't, which costs one
 * load of the generation.
 */
static bool
doltgres_scan_newest(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under, uint64 generation,
					 DoltgresScanNewest *result, HostError *error)
{
	uint32		epoch;
	bool		has_chain;
	TreesStoreTree chain;

	if (!txn_state_readable_since(io, under->state, under->snapshot, generation, &result->moved, &result->some,
								  &result->base, &result->now, error))
		return false;
	if (!result->moved)
		return true;
	doltgres_scan_Epochs_based(result->some ? &result->base : NULL, &epoch, &has_chain, &chain);
	if (!doltgres_scan_Epochs_of(io, store, epoch, has_chain, chain, &result->epochs, error))
		return false;
	result->epochs_live = true;
	return true;
}

/* doltgres_scan_Trees_newer_frame_t is Trees_newer's state across its pad: the newest base's read. */
typedef struct doltgres_scan_Trees_newer_frame_t
{
	DoltgresScanTrees *self;
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	DoltgresScanEpochs *epochs;
	DoltgresScanNewest newest;
	HostError  *error;
	bool		ok;
} doltgres_scan_Trees_newer_frame_t;

/* doltgres_scan_Trees_newer_pad frees the newest base's read. */
static void
doltgres_scan_Trees_newer_pad(void *frame_)
{
	doltgres_scan_Newest_drop(&((doltgres_scan_Trees_newer_frame_t *) frame_)->newest);
}

/* doltgres_scan_Trees_newer_body reads the newest base and moves the read's trees and epochs to it. */
static void
doltgres_scan_Trees_newer_body(void *frame_)
{
	doltgres_scan_Trees_newer_frame_t *frame = frame_;
	DoltgresScanNewest *newest = &frame->newest;

	if (!doltgres_scan_newest(frame->io, frame->store, frame->under, frame->self->generation, newest, frame->error))
		return;
	if (newest->moved)
	{
		newest->epochs_live = false;
		if (!doltgres_scan_Epochs_moved(frame->epochs, &newest->epochs, frame->error))
			return;
		*frame->self = doltgres_scan_Trees_of(newest->some ? &newest->base : NULL, newest->now);
	}
	frame->ok = true;
}

/*
 * doltgres_scan_Trees_newer moves the trees of a read whose base an install
 * moved to its storage's newest base, as readable gives it for the read's
 * snapshot, with the generation it read it at, and the read's epochs to
 * that base's (Epochs_moved), leaving them as they are while the base
 * hasn't moved.
 */
static bool
doltgres_scan_Trees_newer(DoltgresScanTrees *self, HostIoIo *io, TreesStoreNodeStore store,
						  const DoltgresScanUnder *under, DoltgresScanEpochs *epochs, HostError *error)
{
	doltgres_scan_Trees_newer_frame_t frame = {.self = self, .io = io, .store = store, .under = under,
	.epochs = epochs, .error = error};

	std_landing(&frame, doltgres_scan_Trees_newer_body, doltgres_scan_Trees_newer_pad);
	doltgres_scan_Trees_newer_pad(&frame);
	return frame.ok;
}

/*
 * DoltgresScanRebased is Rebased: what a move of a scan to its storage's
 * newest base leaves its place to go on from: whether it moved, the new
 * base's tree, the key a read goes on from, with whether it is its group's
 * key, gone past, or a seek's, gone to, none for a scan not begun, and the
 * table's order, cloned for the comparisons outside the record, while live.
 */
typedef struct DoltgresScanRebased
{
	bool		moved;
	bool		has_tree;
	TreesStoreTree tree;
	doltgres_OptionVecU8_t from;
	bool		grouped;
	TreesKeyOrder order;
	bool		order_live;
} DoltgresScanRebased;

/* doltgres_scan_Rebased_drop frees a move's key and order, each while live. */
static void
doltgres_scan_Rebased_drop(DoltgresScanRebased *self)
{
	doltgres_scan_option_drop(&self->from);
	if (self->order_live)
		trees_KeyOrder_drop(&self->order);
	self->order_live = self->moved = false;
}

/* doltgres_scan_rebased_put_t is the captures of rebased's put in the record: the newest base's read and the move. */
typedef struct doltgres_scan_rebased_put_t
{
	DoltgresScanNewest *newest;
	DoltgresScanRebased *result;
} doltgres_scan_rebased_put_t;

/*
 * doltgres_scan_rebased_closure_0 is rebased's put in the record: the newest
 * base's trees, read point, epochs, and generation, the tid range read's
 * delta tuple ids cleared, and the key the scan goes on from.
 */
static bool
doltgres_scan_rebased_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_rebased_put_t *put = captures;
	DoltgresScanNewest *newest = put->newest;
	DoltgresScanRebased *result = put->result;
	DoltgresScanTrees trees = doltgres_scan_Trees_of(newest->some ? &newest->base : NULL, newest->now);

	newest->epochs_live = false;
	if (!doltgres_scan_Epochs_moved(&scan->epochs, &newest->epochs, error))
		return false;
	scan->has_tree = trees.has_root;
	scan->tree = trees.root;
	scan->has_tuple_ids = trees.has_tuple_ids;
	scan->tuple_ids = trees.tuple_ids;
	scan->folded = trees.folded;
	scan->generation = trees.generation;
	if (scan->has_written)
		std_vec_free_Checked(&scan->written, DOLTGRES_SCAN_U64);
	scan->has_written = false;
	result->moved = true;
	result->has_tree = scan->has_tree;
	result->tree = scan->tree;
	result->grouped = scan->has_group;
	if (scan->has_group)
		std_vec_clone(&result->from.v, &scan->group.key, DOLTGRES_SCAN_BYTES);
	else if (scan->seek.has)
		std_vec_clone(&result->from.v, &scan->seek.v, DOLTGRES_SCAN_BYTES);
	result->from.has = scan->has_group || scan->seek.has;
	trees_KeyOrder_Clone_clone(&scan->order, &result->order);
	result->order_live = true;
	return true;
}

/* doltgres_scan_rebased_frame_t is rebased's state across its pad: the newest base's read. */
typedef struct doltgres_scan_rebased_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	DoltgresScanNewest newest;
	DoltgresScanRebased *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_rebased_frame_t;

/* doltgres_scan_rebased_pad frees the newest base's read. */
static void
doltgres_scan_rebased_pad(void *frame_)
{
	doltgres_scan_Newest_drop(&((doltgres_scan_rebased_frame_t *) frame_)->newest);
}

/* doltgres_scan_rebased_body reads the newest base once the generation moved and puts it in the record. */
static void
doltgres_scan_rebased_body(void *frame_)
{
	doltgres_scan_rebased_frame_t *frame = frame_;
	doltgres_scan_rebased_put_t put = {&frame->newest, frame->result};
	uint64		generation;

	*frame->result = (DoltgresScanRebased)
	{
		.moved = false
	};
	if (!host_owned_TableDoltgresScanScan_withU64(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
												  doltgres_scan_generation_closure_0, NULL, NULL, &generation,
												  frame->error) ||
		!doltgres_scan_newest(frame->io, frame->store, frame->under, generation, &frame->newest, frame->error))
		return;
	frame->ok = !frame->newest.moved ||
		host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
												  doltgres_scan_rebased_closure_0, &put, NULL, frame->error);
}

/*
 * doltgres_scan_rebased moves a scan to its storage's newest base when an
 * install moved the base since the scan took its own (bugs rows 240 and
 * 241), at a read's start and never inside one: the record takes the new
 * base's trees, read point, epochs (Epochs_moved), and generation, the
 * epochs' chain read through a store, and a tid range read's delta tuple ids
 * are collected again at that point by its next read. Every live snapshot
 * may read the newest base (txn_state_readable), and its read point passes
 * over the delta's versions it holds, so a read going on from its group's
 * key reads nothing twice and misses nothing. It answers what the caller's
 * place goes on from, or that the base hasn't moved, which costs one load of
 * the generation.
 */
static bool
doltgres_scan_rebased(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
					  HostOwnedHandle handle, DoltgresScanRebased *result, HostError *error)
{
	doltgres_scan_rebased_frame_t frame = {.io = io, .store = store, .under = under, .handle = handle,
	.result = result, .error = error};

	std_landing(&frame, doltgres_scan_rebased_body, doltgres_scan_rebased_pad);
	doltgres_scan_rebased_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_rebase_read_t is a move to the newest base under a reader, a
 * Stores::with call's captures: rebased's arguments and its answer.
 */
typedef struct doltgres_scan_rebase_read_t
{
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	DoltgresScanRebased *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_rebase_read_t;

/* doltgres_scan_rebase_read moves the scan to the newest base, reseated's and next_parallel's store calls' body. */
static void
doltgres_scan_rebase_read(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_rebase_read_t *read = captures;

	read->ok = doltgres_scan_rebased(io, store, read->under, read->handle, read->result, read->error);
}

/* doltgres_scan_reseated_closure_0 is reseated's put of the place in the record. */
static bool
doltgres_scan_reseated_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_Place_set(&scan->place, *(DoltgresScanPlace *) captures);
	return true;
}

/* doltgres_scan_place_put_drop drops the place of a put that never ran. */
static void
doltgres_scan_place_put_drop(void *captures)
{
	doltgres_scan_Place_drop(captures);
}

/* doltgres_scan_reseated_frame_t is reseated's state across its pad: its reader and the move, while live. */
typedef struct doltgres_scan_reseated_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	DoltgresScanDirection way;
	HostSharedReader reading;
	bool		reading_live;
	DoltgresScanRebased rebasing;
	DoltgresScanPlace place;
	HostError  *error;
	bool		ok;
} doltgres_scan_reseated_frame_t;

/* doltgres_scan_reseated_pad drops what reseated holds. */
static void
doltgres_scan_reseated_pad(void *frame_)
{
	doltgres_scan_reseated_frame_t *frame = frame_;

	doltgres_scan_Rebased_drop(&frame->rebasing);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	frame->reading_live = false;
}

/* doltgres_scan_reseated_body moves the scan to the newest base and puts its place in the new base's tree. */
static void
doltgres_scan_reseated_body(void *frame_)
{
	doltgres_scan_reseated_frame_t *frame = frame_;
	DoltgresScanRebased *rebasing = &frame->rebasing;
	doltgres_scan_rebase_read_t rebase = {frame->under, frame->handle, rebasing, frame->error};

	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		return;
	frame->reading_live = true;
	frame->stores->ops->with(frame->stores->self, frame->io, &frame->reading, doltgres_scan_rebase_read, &rebase);
	if (!rebase.ok)
		return;
	if (!rebasing->moved)
	{
		frame->ok = true;
		return;
	}
	if (!rebasing->from.has)
		frame->place.tag = DOLTGRES_SCAN_PLACE_START;
	else if (rebasing->has_tree)
	{
		doltgres_scan_place_read_t read = {{rebasing->tree, rebasing->from.v.ptr, rebasing->from.v.len, frame->way,
		&rebasing->order, rebasing->grouped}, .error = frame->error};

		frame->stores->ops->with(frame->stores->self, frame->io, &frame->reading, doltgres_scan_place_read, &read);
		if (!read.ok)
			return;
		frame->place = read.place;
	}
	else
		frame->place = doltgres_scan_off(frame->way);
	frame->ok = host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														  doltgres_scan_reseated_closure_0, &frame->place,
														  doltgres_scan_place_put_drop, frame->error);
}

/*
 * doltgres_scan_reseated moves a scan whose read found an install moved its
 * storage's base (txn_state_moved_past) to the newest base (rebased) and
 * puts its place in the new base's tree, outside the record, since a
 * compared tree's order runs user code: past its group's key (placed's
 * grouped place), at a seek's key, or, for a scan not begun, whose read may
 * have stepped its place in the old tree, at the new tree's start.
 */
static bool
doltgres_scan_reseated(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
					   HostOwnedHandle handle, DoltgresScanDirection way, HostError *error)
{
	doltgres_scan_reseated_frame_t frame = {.io = io, .stores = stores, .under = under, .handle = handle,
	.way = way, .error = error};

	std_landing(&frame, doltgres_scan_reseated_body, doltgres_scan_reseated_pad);
	doltgres_scan_reseated_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_seek_closure_1_0 is seek's store call, the base's place at the key found under the scan's reader. */
static void
doltgres_scan_seek_closure_1_0(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_place_read(captures, io, store);
}

/*
 * doltgres_scan_seek_compared_closure_1 is seek_compared's store call, the
 * base's place at the key found under its own reader.
 */
static void
doltgres_scan_seek_compared_closure_1(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_place_read(captures, io, store);
}

/* doltgres_scan_OptionKeyOrder_t is an Option<KeyOrder>. */
typedef struct doltgres_scan_OptionKeyOrder_t
{
	bool		has;
	TreesKeyOrder v;
} doltgres_scan_OptionKeyOrder_t;

/* DOLTGRES_SCAN_OPTION_KEY_ORDER_DROP drops an Option<KeyOrder> a call left. */
#define DOLTGRES_SCAN_OPTION_KEY_ORDER_DROP(place) \
	do { \
		if ((place)->has) \
			trees_KeyOrder_drop(&(place)->v); \
	} while (0)

/* Scan's Table's with at Option<KeyOrder>, for seek. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS OptionKeyOrder
#define HOST_OWNED_WITH_T doltgres_scan_OptionKeyOrder_t
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_OPTION_KEY_ORDER_DROP(place)
#include "doltgres/host/owned_template.h"

/* doltgres_scan_TupleOptionTreeOptionRun_t is an (Option<Tree>, Option<Run>): a scan's tree and its run, taken. */
typedef struct doltgres_scan_TupleOptionTreeOptionRun_t
{
	bool		has_tree;
	TreesStoreTree tree;
	bool		has_run;
	DoltgresScanRun run;
} doltgres_scan_TupleOptionTreeOptionRun_t;

/* DOLTGRES_SCAN_TREE_RUN_DROP drops the run a call left. */
#define DOLTGRES_SCAN_TREE_RUN_DROP(place) \
	do { \
		if ((place)->has_run) \
			doltgres_scan_vecs_drop(&(place)->run.keys); \
	} while (0)

/* Scan's Table's with at (Option<Tree>, Option<Run>), for seek_compared. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS TupleOptionTreeOptionRun
#define HOST_OWNED_WITH_T doltgres_scan_TupleOptionTreeOptionRun_t
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_TREE_RUN_DROP(place)
#include "doltgres/host/owned_template.h"

/* doltgres_scan_seek_closure_0 is seek's look in the record: the order cloned out of it when a comparator orders it. */
static bool
doltgres_scan_seek_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
							 doltgres_scan_OptionKeyOrder_t *result, HostError *error)
{
	result->has = !trees_KeyOrder_is_bytes(&scan->order);
	if (result->has)
		trees_KeyOrder_Clone_clone(&scan->order, &result->v);
	return true;
}

/* doltgres_scan_seek_t is a seek's captures: the store's source, the key, and the way. */
typedef struct doltgres_scan_seek_t
{
	const DoltgresScanStores *stores;
	const uint8 *from;
	size_t		from_len;
	DoltgresScanDirection way;
} doltgres_scan_seek_t;

/* doltgres_scan_seek_records returns whether a seek's key is an index's record. */
static bool
doltgres_scan_seek_records(const uint8 *from, size_t from_len)
{
	return from_len > 0 && from[0] == TXN_ENTRY_RECORD;
}

/* doltgres_scan_seek_closure_1 is seek's placing in the record of a byte-ordered tree. */
static bool
doltgres_scan_seek_closure_1(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_seek_t *seek = captures;
	DoltgresScanPlace place = doltgres_scan_off(seek->way);

	if (scan->has_group)
		doltgres_scan_Group_drop(&scan->group);
	scan->has_group = false;
	if (scan->has_tree)
	{
		static const TreesKeyOrder bytes = {.tag = TREES_KEY_ORDER_BYTES};
		doltgres_scan_place_read_t read = {{scan->tree, seek->from, seek->from_len, seek->way, &bytes},
		.error = error};

		seek->stores->ops->with(seek->stores->self, io, &scan->reader, doltgres_scan_seek_closure_1_0, &read);
		if (!read.ok)
			return false;
		place = read.place;
	}
	doltgres_scan_Place_set(&scan->place, place);
	doltgres_scan_option_drop(&scan->seek);
	std_slice_to_vec_Checked(&scan->seek.v, seek->from, seek->from_len, DOLTGRES_SCAN_BYTES);
	scan->seek.has = true;
	scan->records = doltgres_scan_seek_records(seek->from, seek->from_len);
	return true;
}

/* doltgres_scan_seek_compared_closure_0 is seek_compared's take from the record: its tree and its run. */
static bool
doltgres_scan_seek_compared_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									  doltgres_scan_TupleOptionTreeOptionRun_t *result, HostError *error)
{
	result->has_tree = scan->has_tree;
	result->tree = scan->tree;
	result->has_run = scan->has_run;
	result->run = scan->run;
	scan->has_run = false;
	return true;
}

/* doltgres_scan_seek_put_t is the captures of seek_compared's put in the record: the place, the run, and the key. */
typedef struct doltgres_scan_seek_put_t
{
	DoltgresScanPlace place;
	bool		has_run;
	DoltgresScanRun run;
	const uint8 *from;
	size_t		from_len;
} doltgres_scan_seek_put_t;

/* doltgres_scan_seek_put_drop drops the place and run of a put that never ran. */
static void
doltgres_scan_seek_put_drop(void *captures)
{
	doltgres_scan_seek_put_t *put = captures;

	if (put->has_run)
		doltgres_scan_vecs_drop(&put->run.keys);
	doltgres_scan_Place_drop(&put->place);
}

/*
 * doltgres_scan_seek_compared_closure_2 is seek_compared's put in the record:
 * the group cleared and the place, run, and seek set.
 */
static bool
doltgres_scan_seek_compared_closure_2(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_seek_put_t *put = captures;

	if (scan->has_group)
		doltgres_scan_Group_drop(&scan->group);
	scan->has_group = false;
	doltgres_scan_Place_set(&scan->place, put->place);
	if (scan->has_run)
		doltgres_scan_vecs_drop(&scan->run.keys);
	scan->has_run = put->has_run;
	scan->run = put->run;
	doltgres_scan_option_drop(&scan->seek);
	std_slice_to_vec_Checked(&scan->seek.v, put->from, put->from_len, DOLTGRES_SCAN_BYTES);
	scan->seek.has = true;
	scan->records = doltgres_scan_seek_records(put->from, put->from_len);
	return true;
}

/*
 * doltgres_scan_seek_compared_frame_t is seek_compared's state across its
 * pad: the run it took, its reader, and the place it found, each while live.
 */
typedef struct doltgres_scan_seek_compared_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	HostOwnedHandle handle;
	const uint8 *from;
	size_t		from_len;
	DoltgresScanDirection way;
	const TreesKeyOrder *order;
	doltgres_scan_TupleOptionTreeOptionRun_t taken;
	HostSharedReader reading;
	bool		reading_live;
	doltgres_scan_seek_put_t put;
	bool		place_live;
	bool		run_live;
	HostError  *error;
	bool		ok;
} doltgres_scan_seek_compared_frame_t;

/* doltgres_scan_seek_compared_pad drops what seek_compared holds. */
static void
doltgres_scan_seek_compared_pad(void *frame_)
{
	doltgres_scan_seek_compared_frame_t *frame = frame_;

	if (frame->run_live)
		doltgres_scan_vecs_drop(&frame->put.run.keys);
	frame->run_live = false;
	if (frame->place_live)
		doltgres_scan_Place_drop(&frame->put.place);
	frame->place_live = false;
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	frame->reading_live = false;
	if (frame->taken.has_run)
		doltgres_scan_vecs_drop(&frame->taken.run.keys);
	frame->taken.has_run = false;
}

/* doltgres_scan_seek_compared_body places the base and splits the run with no record lent, then puts them back. */
static void
doltgres_scan_seek_compared_body(void *frame_)
{
	doltgres_scan_seek_compared_frame_t *frame = frame_;

	if (!host_owned_TableDoltgresScanScan_withTupleOptionTreeOptionRun(&DOLTGRES_SCAN_SCANS, frame->io,
																	   frame->handle,
																	   doltgres_scan_seek_compared_closure_0, NULL,
																	   NULL, &frame->taken, frame->error))
		return;
	if (frame->taken.has_tree)
	{
		doltgres_scan_place_read_t read = {{frame->taken.tree, frame->from, frame->from_len, frame->way,
		frame->order}, .error = frame->error};

		if (!host_shared_read(frame->io, &frame->reading, frame->error))
			return;
		frame->reading_live = true;
		frame->stores->ops->with(frame->stores->self, frame->io, &frame->reading,
								 doltgres_scan_seek_compared_closure_1, &read);
		if (!read.ok)
			return;
		frame->put.place = read.place;
		frame->place_live = true;
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	else
	{
		frame->put.place = doltgres_scan_off(frame->way);
		frame->place_live = true;
	}
	if (frame->taken.has_run)
	{
		if (!doltgres_scan_split(frame->io, &frame->taken.run.keys, frame->from, frame->from_len, frame->way,
								 frame->order, &frame->put.run.split, frame->error))
			return;
		frame->put.run.keys = frame->taken.run.keys;
		frame->put.has_run = true;
		frame->run_live = true;
		frame->taken.has_run = false;
	}
	frame->put.from = frame->from;
	frame->put.from_len = frame->from_len;
	frame->place_live = false;
	frame->run_live = false;
	if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
												   doltgres_scan_seek_compared_closure_2, &frame->put,
												   doltgres_scan_seek_put_drop, frame->error))
		return;
	frame->ok = true;
}

/*
 * doltgres_scan_seek_compared is seek for a tree ordered by a comparator:
 * the base's place found through a reader of its own, and the delta's run,
 * once read, split at the key, each found with no record lent and put back
 * after.
 */
static bool
doltgres_scan_seek_compared(HostIoIo *io, const DoltgresScanStores *stores, HostOwnedHandle handle,
							const uint8 *from, size_t from_len, DoltgresScanDirection way,
							const TreesKeyOrder *order, HostError *error)
{
	doltgres_scan_seek_compared_frame_t frame = {.io = io, .stores = stores, .handle = handle, .from = from,
	.from_len = from_len, .way = way, .order = order, .error = error};

	std_landing(&frame, doltgres_scan_seek_compared_body, doltgres_scan_seek_compared_pad);
	doltgres_scan_seek_compared_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_seek_frame_t is seek's state across its pad: the comparator's
 * order cloned out of the record, while live.
 */
typedef struct doltgres_scan_seek_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	HostOwnedHandle handle;
	const uint8 *from;
	size_t		from_len;
	DoltgresScanDirection way;
	doltgres_scan_OptionKeyOrder_t compared;
	HostError  *error;
	bool		ok;
} doltgres_scan_seek_frame_t;

/* doltgres_scan_seek_pad drops the cloned order. */
static void
doltgres_scan_seek_pad(void *frame_)
{
	doltgres_scan_seek_frame_t *frame = frame_;

	DOLTGRES_SCAN_OPTION_KEY_ORDER_DROP(&frame->compared);
	frame->compared.has = false;
}

/* doltgres_scan_seek_body places the scan at the key, outside the record for a comparator's tree. */
static void
doltgres_scan_seek_body(void *frame_)
{
	doltgres_scan_seek_frame_t *frame = frame_;
	doltgres_scan_seek_t seek = {frame->stores, frame->from, frame->from_len, frame->way};

	if (!host_owned_TableDoltgresScanScan_withOptionKeyOrder(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
															 doltgres_scan_seek_closure_0, NULL, NULL,
															 &frame->compared, frame->error))
		return;
	if (frame->compared.has)
	{
		frame->ok = doltgres_scan_seek_compared(frame->io, frame->stores, frame->handle, frame->from,
												frame->from_len, frame->way, &frame->compared.v, frame->error);
		return;
	}
	frame->ok = host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														  doltgres_scan_seek_closure_1, &seek, NULL, frame->error);
}

/*
 * doltgres_scan_seek puts a scan at a key, so its next read the way given
 * starts at the first key at or past it that way: the base's place at its
 * first entry not below the key going forward, or its last not above it
 * going backward, and the delta's versions of the key itself read before the
 * keys past it. A seek to an index's record reads on through the records.
 */
bool
doltgres_scan_seek(HostIoIo *io, const DoltgresScanStores *stores, HostOwnedHandle handle, const uint8 *from,
				   size_t from_len, DoltgresScanDirection way, HostError *error)
{
	doltgres_scan_seek_frame_t frame = {.io = io, .stores = stores, .handle = handle, .from = from,
	.from_len = from_len, .way = way, .error = error};

	std_landing(&frame, doltgres_scan_seek_body, doltgres_scan_seek_pad);
	doltgres_scan_seek_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_split answers how many of a compared run's keys come before
 * a key the way a read goes from it: those below it going forward, and
 * those at or below it going backward.
 */
static bool
doltgres_scan_split(HostIoIo *io, const StdVec *keys, const uint8 *from, size_t from_len, DoltgresScanDirection way,
					const TreesKeyOrder *order, size_t *result, HostError *error)
{
	size_t		low = 0;
	size_t		high = keys->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		const StdVec *key = &STD_VEC_AT(keys, StdVec, mid);
		StdOrdering ordering;
		ChunksErrorError failed;
		bool		before;

		if (!trees_KeyOrder_compare(order, io, key->ptr, key->len, from, from_len, &ordering, &failed))
			return doltgres_scan_storage_failed(&failed, error);
		before = way == DOLTGRES_SCAN_DIRECTION_FORWARD ? ordering == STD_ORDERING_LESS :
			ordering != STD_ORDERING_GREATER;
		if (before)
			low = mid + 1;
		else
			high = mid;
	}
	*result = low;
	return true;
}

/* doltgres_scan_scan_end ends a scan, dropping its record and its cursor. */
bool
doltgres_scan_scan_end(HostIoIo *io, HostOwnedHandle handle, HostError *error)
{
	return host_owned_TableDoltgresScanScan_end(&DOLTGRES_SCAN_SCANS, io, handle, error);
}

/* doltgres_scan_tid_valid_captures_t is tid_valid's call's captures: the tuple id and a page's most offset. */
typedef struct doltgres_scan_tid_valid_captures_t
{
	uint64		tuple_id;
	uint16		most_offset;
} doltgres_scan_tid_valid_captures_t;

/* doltgres_scan_tid_valid_closure_0 is tid_valid's call: the tuple id's block and offset checked against the scan's. */
static bool
doltgres_scan_tid_valid_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, bool *result,
								  HostError *error)
{
	doltgres_scan_tid_valid_captures_t *tid = captures;
	uint64		offset = tid->tuple_id & 0xffff;

	*result = tid->tuple_id >> 16 < scan->blocks && offset >= 1 && offset <= tid->most_offset;
	return true;
}

/*
 * doltgres_scan_tid_valid answers whether a tuple id is one a scan's storage
 * can hold: its block below the storage's count of blocks when the scan
 * began, and its offset from 1 to a page's most tuples.
 */
bool
doltgres_scan_tid_valid(HostIoIo *io, HostOwnedHandle handle, uint64 tuple_id, uint16 most_offset, bool *result,
						HostError *error)
{
	doltgres_scan_tid_valid_captures_t captures = {tuple_id, most_offset};

	return host_owned_TableDoltgresScanScan_withBool(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_tid_valid_closure_0, &captures, NULL, result,
													 error);
}

/* doltgres_scan_total answers how two keys order in a table's order with ties broken by their bytes. */
static bool
doltgres_scan_total(HostIoIo *io, const TreesKeyOrder *order, const StdVec *a, const StdVec *b, StdOrdering *result,
					HostError *error)
{
	ChunksErrorError failed;

	if (!trees_KeyOrder_compare(order, io, a->ptr, a->len, b->ptr, b->len, result, &failed))
		return doltgres_scan_storage_failed(&failed, error);
	if (*result == STD_ORDERING_EQUAL)
		*result = std_bytes_Ord_cmp(a->ptr, a->len, b->ptr, b->len);
	return true;
}

/* doltgres_scan_pick_frame_t is pick's state across its pad: the two candidates it took, while live. */
typedef struct doltgres_scan_pick_frame_t
{
	HostIoIo   *io;
	const TreesKeyOrder *order;
	DoltgresScanDirection way;
	DoltgresScanPicked *result;
	bool		live;
	HostError  *error;
	bool		ok;
} doltgres_scan_pick_frame_t;

/* doltgres_scan_pick_pad drops the candidates. */
static void
doltgres_scan_pick_pad(void *frame_)
{
	doltgres_scan_pick_frame_t *frame = frame_;

	if (frame->live)
		doltgres_scan_Picked_drop(frame->result);
	frame->live = false;
}

/* doltgres_scan_pick_body compares the candidates' keys and keeps the nearer, or both. */
static void
doltgres_scan_pick_body(void *frame_)
{
	doltgres_scan_pick_frame_t *frame = frame_;
	DoltgresScanPicked *picked = frame->result;
	DoltgresScanDirection way = frame->way;
	StdOrdering ordering;

	if (!doltgres_scan_total(frame->io, frame->order, &picked->base_key, &picked->delta_key, &ordering,
							 frame->error))
		return;
	if (ordering == STD_ORDERING_EQUAL)
		picked->tag = DOLTGRES_SCAN_PICKED_BOTH;
	else if ((ordering == STD_ORDERING_LESS && way == DOLTGRES_SCAN_DIRECTION_FORWARD) ||
			 (ordering == STD_ORDERING_GREATER && way == DOLTGRES_SCAN_DIRECTION_BACKWARD))
	{
		std_vec_free_Checked(&picked->delta_key, DOLTGRES_SCAN_BYTES);
		picked->tag = DOLTGRES_SCAN_PICKED_BASE;
		picked->has_before = true;
		picked->before = picked->delta_at;
	}
	else
	{
		std_vec_free_Checked(&picked->base_key, DOLTGRES_SCAN_BYTES);
		doltgres_scan_Copies_drop(&picked->base_copies);
		picked->tag = DOLTGRES_SCAN_PICKED_DELTA;
	}
	frame->live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_pick chooses between a compared merge's two candidates,
 * which it takes, the way a read goes: the nearer in the table's order with
 * ties broken by bytes, both when they are one key.
 */
static bool
doltgres_scan_pick(HostIoIo *io, StdVec *base_key, DoltgresScanCopies *base_copies, StdVec *delta_key,
				   size_t delta_at, const TreesKeyOrder *order, DoltgresScanDirection way, DoltgresScanPicked *result,
				   HostError *error)
{
	doltgres_scan_pick_frame_t frame = {.io = io, .order = order, .way = way, .result = result, .live = true,
	.error = error};

	*result = (DoltgresScanPicked)
	{
		.tag = DOLTGRES_SCAN_PICKED_BOTH, .base_key = *base_key, .base_copies = *base_copies,
			.delta_key = *delta_key, .delta_at = delta_at
	};
	std_landing(&frame, doltgres_scan_pick_body, doltgres_scan_pick_pad);
	doltgres_scan_pick_pad(&frame);
	return frame.ok;
}

/* trees' pin at Result<Option<Vec<u8>>, Error>, for pair. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionVecU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , doltgres_OptionVecU8_t *result, HostError *error
#define TREES_PIN_RESULT_FIELDS doltgres_OptionVecU8_t *result; HostError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* trees' pin at Result<Row, Error>, for resolve. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultRow
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , DoltgresScanRow *result, HostError *error
#define TREES_PIN_RESULT_FIELDS DoltgresScanRow *result; HostError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* doltgres_scan_OptionCopies_t is an Option<Copies>. */
typedef struct doltgres_scan_OptionCopies_t
{
	bool		has;
	DoltgresScanCopies v;
} doltgres_scan_OptionCopies_t;

/* trees' pin at Result<Option<Copies>, Error>, for fetch_key. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionCopies
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , doltgres_scan_OptionCopies_t *result, HostError *error
#define TREES_PIN_RESULT_FIELDS doltgres_scan_OptionCopies_t *result; HostError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* doltgres_scan_OptionU64_t is an Option<u64>. */
typedef struct doltgres_scan_OptionU64_t
{
	bool		has;
	uint64		v;
} doltgres_scan_OptionU64_t;

/* trees' pin at Result<Option<u64>, Error>, for base_tid. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionU64
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , doltgres_scan_OptionU64_t *result, HostError *error
#define TREES_PIN_RESULT_FIELDS doltgres_scan_OptionU64_t *result; HostError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * doltgres_scan_cursor_frame_t is a pin scope's state across its pad: its
 * captures, its cursor, and the stamps, values, tuple, and bytes it reads
 * out, each while live.
 */
typedef struct doltgres_scan_cursor_frame_t
{
	void	   *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	StdVec		stamps;
	bool		stamps_live;
	StdVec		values;
	bool		values_live;
	StdVec		tuple;
	bool		tuple_live;
	StdVec		bytes;
	bool		bytes_live;
	void	   *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_cursor_frame_t;

/* doltgres_scan_cursor_pad drops a pin scope's stamps and cursor. */
static void
doltgres_scan_cursor_pad(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, DOLTGRES_SCAN_BYTES);
	frame->bytes_live = false;
	if (frame->tuple_live)
		std_vec_free_Checked(&frame->tuple, DOLTGRES_SCAN_BYTES);
	frame->tuple_live = false;
	if (frame->values_live)
		std_vec_free_Checked(&frame->values, DOLTGRES_SCAN_U32);
	frame->values_live = false;
	if (frame->stamps_live)
		std_vec_free_Checked(&frame->stamps, DOLTGRES_SCAN_STAMP);
	frame->stamps_live = false;
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->cursor_live = false;
}

/* doltgres_scan_cursor_scope runs a pin scope's body with its cursor framed. */
static bool
doltgres_scan_cursor_scope(void *captures, HostIoIo *io, TreesPinPin *pin, void *result, HostError *error,
						   void (*body) (void *frame))
{
	doltgres_scan_cursor_frame_t frame = {.captures = captures, .io = io, .pin = pin, .result = result,
	.error = error};

	std_landing(&frame, body, doltgres_scan_cursor_pad);
	doltgres_scan_cursor_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_seek_exact seeks a key in a tree's order and reports whether the cursor is valid at that very key. */
static bool
doltgres_scan_seek_exact(doltgres_scan_cursor_frame_t *frame, TreesStoreTree *tree, const uint8 *key,
						 size_t key_len, const TreesKeyOrder *order, bool *at)
{
	ChunksErrorError failed;
	StdBytes	found;

	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(tree), key, key_len, order,
								  &frame->cursor, &failed))
		return doltgres_scan_storage_failed(&failed, frame->error);
	frame->cursor_live = true;
	*at = trees_cursor_Cursor_is_valid(&frame->cursor);
	if (*at)
	{
		found = trees_cursor_Cursor_key(&frame->cursor);
		*at = found.len == key_len && memcmp(found.ptr, key, key_len) == 0;
	}
	return true;
}

/* doltgres_scan_pair_captures_t is pair's pin scope's captures: the tuple id tree and the tuple id's key. */
typedef struct doltgres_scan_pair_captures_t
{
	TreesStoreTree tuple_ids;
	uint8		id[6];
} doltgres_scan_pair_captures_t;

/* doltgres_scan_pair_closure_0_body seeks the tuple id's key and copies out the row key its pair names. */
static void
doltgres_scan_pair_closure_0_body(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;
	doltgres_scan_pair_captures_t *captures = frame->captures;
	doltgres_OptionVecU8_t *result = frame->result;
	static const TreesKeyOrder bytes = {.tag = TREES_KEY_ORDER_BYTES};
	bool		at;
	StdBytes	key;
	uint32		heap_bytes;

	if (!doltgres_scan_seek_exact(frame, &captures->tuple_ids, captures->id, sizeof(captures->id), &bytes, &at))
		return;
	result->has = at;
	if (at)
	{
		StdBytes	value = trees_cursor_Cursor_value(&frame->cursor);

		if (!txn_base_pair_parts(value.ptr, value.len, &key, &heap_bytes, frame->error))
			return;
		std_slice_to_vec_Checked(&result->v, key.ptr, key.len, DOLTGRES_SCAN_BYTES);
	}
	frame->ok = true;
}

/* doltgres_scan_pair_closure_0 is pair's pin scope. */
static bool
doltgres_scan_pair_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, doltgres_OptionVecU8_t *result,
							 HostError *error)
{
	return doltgres_scan_cursor_scope(captures, io, pin, result, error, doltgres_scan_pair_closure_0_body);
}

/*
 * doltgres_scan_pair answers the row key a base's tuple id tree pairs with a
 * tuple id, read through a store, or none.
 */
static bool
doltgres_scan_pair(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tuple_ids, uint64 tuple_id,
				   doltgres_OptionVecU8_t *result, HostError *error)
{
	doltgres_scan_pair_captures_t captures = {.tuple_ids = tuple_ids};

	txn_base_tuple_id_key(tuple_id, captures.id);
	return trees_pin_pin_ResultOptionVecU8(io, store, doltgres_scan_pair_closure_0, &captures, NULL, result, error);
}

/*
 * doltgres_scan_resolve_closure_0 is resolve's lost: the error of the tuple
 * id's row missing from its table's tree, data corruption.
 */
static HostError
doltgres_scan_resolve_closure_0(uint64 tuple_id)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the row of tuple id "), STD_FMT_ARG(0),
	STD_FMT_LIT(" is missing from its table's tree")};
	StdFmtArg	args[] = {{&tuple_id, std_u64_Display_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};
	StdString text;

	std_fmt_format_Checked(&text, &message);
	return host_Error_fork((const uint8 *) "XX001", &text);
}

/*
 * doltgres_scan_resolved_seek puts the frame's cursor at the pair's key in
 * its tree: in the memo's kept leaf when the memo kept a cursor sought from
 * the same root and that leaf holds the key, else by a seek from the root,
 * answering whether the tree holds the key.
 */
static bool
doltgres_scan_resolved_seek(doltgres_scan_cursor_frame_t *frame, DoltgresScanPaired *paired, bool *at)
{
	DoltgresScanResolved *resolved = paired->resolved;
	ChunksErrorError failed;

	*at = false;
	if (resolved->has_kept)
	{
		bool		same = resolved->kept.path.len > 0 &&
			trees_store_Tree_PartialEq_eq(&STD_VEC_AT(&resolved->kept.path, TreesCursorFrame, 0).tree, &paired->root);

		resolved->has_kept = false;
		if (!same)
			trees_cursor_KeptCursor_drop(&resolved->kept);
		else
		{
			if (!trees_cursor_KeptCursor_resume(&resolved->kept, frame->io, frame->pin, &frame->cursor, &failed))
				return doltgres_scan_storage_failed(&failed, frame->error);
			frame->cursor_live = true;
			if (!trees_cursor_Cursor_find_in_node(&frame->cursor, frame->io, paired->key.ptr, paired->key.len,
												  &paired->order, at, &failed))
				return doltgres_scan_storage_failed(&failed, frame->error);
			if (*at)
				return true;
			frame->cursor_live = false;
			trees_cursor_Cursor_drop(&frame->cursor);
		}
	}
	return doltgres_scan_seek_exact(frame, &paired->root, paired->key.ptr, paired->key.len, &paired->order, at);
}

/* doltgres_scan_resolve_closure_1_body seeks the pair's key and makes the row of its copy that holds the tuple id. */
static void
doltgres_scan_resolve_closure_1_body(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;
	DoltgresScanPaired *paired = frame->captures;
	DoltgresScanRow *row = frame->result;
	ChunksErrorError failed;
	bool		at;
	uint64		count;
	StdBytes	tuple;
	uint64		copies;
	bool		some;
	ChunksTrailerStamp stamp;

	if (!doltgres_scan_resolved_seek(frame, paired, &at))
		return;
	if (!at)
	{
		*frame->error = doltgres_scan_resolve_closure_0(paired->tuple_id);
		return;
	}
	if (!trees_cursor_LeafStamps_stamp_of(&paired->resolved->stamps, frame->io, frame->pin, &frame->cursor, paired->tuple_id,
										  &copies, &some, &stamp, &frame->values, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->values_live = true;
	if (!doltgres_scan_copies(&frame->cursor, paired->keyed.len == 0, &count, &tuple, frame->error))
		return;
	if (copies != count)
	{
		*frame->error = doltgres_scan_uneven();
		return;
	}
	if (!some)
	{
		*frame->error = doltgres_scan_resolve_closure_0(paired->tuple_id);
		return;
	}
	std_slice_to_vec_Checked(&frame->tuple, tuple.ptr, tuple.len, DOLTGRES_SCAN_BYTES);
	frame->tuple_live = true;
	frame->bytes_live = true;
	doltgres_scan_value_bytes(frame->values.ptr, frame->values.len, &frame->bytes);
	*row = (DoltgresScanRow)
	{
		.table = paired->table, .stamp = stamp, .tuple = frame->tuple, .values = frame->bytes
	};
	frame->tuple_live = frame->bytes_live = false;
	row->transform = paired->transform;
	row->keyed = paired->keyed;
	row->key = paired->key;
	std_vec_new(&paired->transform, DOLTGRES_SCAN_BYTES);
	std_vec_new(&paired->keyed, DOLTGRES_SCAN_BYTES);
	std_vec_new(&paired->key, DOLTGRES_SCAN_BYTES);
	frame->cursor_live = false;
	trees_cursor_Cursor_keep(&frame->cursor, &paired->resolved->kept);
	paired->resolved->has_kept = true;
	frame->ok = true;
}

/* doltgres_scan_Paired_drop_fn is a pair's drop as a pin scope's captures' drop. */
static void
doltgres_scan_Paired_drop_fn(void *paired)
{
	doltgres_scan_Paired_drop(paired);
}

/* doltgres_scan_resolve_closure_1 is resolve's pin scope. */
static bool
doltgres_scan_resolve_closure_1(void *captures, HostIoIo *io, TreesPinPin *pin, DoltgresScanRow *result,
								HostError *error)
{
	return doltgres_scan_cursor_scope(captures, io, pin, result, error, doltgres_scan_resolve_closure_1_body);
}

/*
 * doltgres_scan_resolve answers the row a pair of the tuple id tree names,
 * which it takes, read through a store: its key sought in the table's tree
 * in that tree's order, and the copy of the key's entry whose stamp holds
 * the tuple id. A key the table's tree lacks, an entry with no copy of the
 * id, and an entry whose stamps differ in number from its copies are data
 * corruption.
 */
static bool
doltgres_scan_resolve(HostIoIo *io, TreesStoreNodeStore store, DoltgresScanPaired *paired, DoltgresScanRow *result,
					  HostError *error)
{
	if (!paired->has_root)
	{
		*error = doltgres_scan_resolve_closure_0(paired->tuple_id);
		doltgres_scan_Paired_drop(paired);
		return false;
	}
	return trees_pin_pin_ResultRow(io, store, doltgres_scan_resolve_closure_1, paired, doltgres_scan_Paired_drop_fn,
								   result, error);
}

/*
 * doltgres_scan_fetch_frame_t is fetch's state across its pad: the delta's
 * and the base's keys of the tuple id, the delta's hold, the key's versions,
 * what the snapshot sees of them, the layout it took, a found row, the base
 * row's transform, and the base's row, each while live.
 */
typedef struct doltgres_scan_fetch_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	DoltgresScanEpochs *epochs;
	DoltgresScanResolved *resolved;
	DoltgresScanTrees trees;
	DoltgresScanLayout layout;
	bool		layout_live;
	uint64		tuple_id;
	doltgres_OptionVecU8_t written;
	doltgres_OptionVecU8_t paired;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	TxnVisibleSeen seen;
	bool		seen_live;
	DoltgresScanFound found;
	int			found_made;
	StdVec		transform;
	bool		transform_live;
	DoltgresScanRow row;
	bool		row_live;
	bool	   *has_result;
	DoltgresScanRow *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_fetch_frame_t;

/* doltgres_scan_found_unmade frees a found row's made fields. */
static void
doltgres_scan_found_unmade(DoltgresScanFound *found, int *made)
{
	if (*made > 2)
		std_vec_free_Checked(&found->transform, DOLTGRES_SCAN_BYTES);
	if (*made > 1)
		std_vec_free_Checked(&found->values, DOLTGRES_SCAN_BYTES);
	if (*made > 0 && found->has_tuple)
		std_vec_free_Checked(&found->tuple, DOLTGRES_SCAN_BYTES);
	*made = 0;
}

/* doltgres_scan_fetch_pad drops what fetch holds. */
static void
doltgres_scan_fetch_pad(void *frame_)
{
	doltgres_scan_fetch_frame_t *frame = frame_;

	if (frame->row_live)
		doltgres_scan_Row_drop(&frame->row);
	frame->row_live = false;
	if (frame->transform_live)
		std_vec_free_Checked(&frame->transform, DOLTGRES_SCAN_BYTES);
	frame->transform_live = false;
	doltgres_scan_found_unmade(&frame->found, &frame->found_made);
	if (frame->seen_live)
		txn_visible_Seen_drop(&frame->seen);
	frame->seen_live = false;
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	frame->versions_live = false;
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	doltgres_scan_option_drop(&frame->paired);
	doltgres_scan_option_drop(&frame->written);
	if (frame->layout_live)
		doltgres_scan_Layout_drop(&frame->layout);
	frame->layout_live = false;
}

/* doltgres_scan_fetch_body finds the tuple id's row among the delta's versions or else the base's copies. */
static void
doltgres_scan_fetch_body(void *frame_)
{
	doltgres_scan_fetch_frame_t *frame = frame_;
	const DoltgresScanUnder *under = frame->under;
	TxnVisibleRule rule = {&TXN_VISIBLE_SNAPSHOT_RULE_OPS, under->snapshot};
	HostRow		tuple = {frame->tuple_id};
	const StdVec *key;
	TxnMemtableSliceSlice slice;
	bool		kept;
	TxnVisibleDirty dirty;
	TxnMemtableVersionVersion *changer;
	TxnMemtableVersionVersion *commander;
	DoltgresScanPaired paired;

	*frame->has_result = false;
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	for (;;)
	{
		if (!txn_memtable_tuples_get(frame->io, under->state, tuple, &frame->written.has, &frame->written.v,
									 frame->error))
			return;
		if (!frame->written.has && frame->trees.has_tuple_ids &&
			!doltgres_scan_pair(frame->io, frame->store, frame->trees.tuple_ids, frame->tuple_id, &frame->paired,
								frame->error))
			return;
		key = frame->written.has ? &frame->written.v : frame->paired.has ? &frame->paired.v : NULL;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-fetch-keyed", frame->error))
			return;
		if (key != NULL)
		{
			if (!txn_memtable_slice_row_versions(frame->io, &frame->hold, under->state, key->ptr, key->len, tuple,
												 frame->trees.folded, &slice, frame->error))
				return;
			frame->versions = slice.versions;
			frame->versions_live = true;
			std_vec_new(&slice.versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
			txn_memtable_slice_Slice_drop(&slice);
		}
		if (!txn_state_moved_past(under->state, frame->trees.generation))
			break;
		if (frame->versions_live)
			txn_memtable_version_versions_drop(&frame->versions, 0);
		frame->versions_live = false;
		doltgres_scan_option_drop(&frame->paired);
		doltgres_scan_option_drop(&frame->written);
		if (!doltgres_scan_Trees_newer(&frame->trees, frame->io, frame->store, under, frame->epochs, frame->error))
			return;
	}
	if (key == NULL)
	{
		frame->ok = true;
		return;
	}
	if (!txn_visible_seen(frame->io, rule, frame->versions.ptr, frame->versions.len,
						  under->has_serial ? &under->serial.read : NULL, &frame->seen, frame->error))
		return;
	frame->seen_live = true;
	if (under->has_serial)
		for (size_t i = 0; i < frame->seen.conflicts.len; i++)
		{
			const txn_TupleRowXid_t *conflict = &STD_VEC_AT(&frame->seen.conflicts, txn_TupleRowXid_t, i);

			if (conflict->f0.f0 == frame->tuple_id)
				doltgres_scan_conflicts_push(under, conflict->f1);
		}
	for (size_t i = 0; i < frame->seen.rows.len; i++)
	{
		const TxnVisibleSeenRow *seen = &STD_VEC_AT(&frame->seen.rows, TxnVisibleSeenRow, i);
		static const StdVec empty = STD_VEC_NEW;

		if (seen->row.put.tuple.f0 != frame->tuple_id)
			continue;
		if (!doltgres_scan_found(frame->io, frame->epochs, seen, &frame->found, &frame->found_made, frame->error))
			return;
		doltgres_scan_Found_row(&frame->found, &frame->layout.keyed, frame->layout.table, key->ptr, key->len, &empty,
								frame->result);
		*frame->has_result = true;
		frame->ok = true;
		return;
	}
	if (!frame->paired.has && frame->written.has && frame->trees.has_tuple_ids &&
		!doltgres_scan_pair(frame->io, frame->store, frame->trees.tuple_ids, frame->tuple_id, &frame->paired,
							frame->error))
		return;
	if (!frame->paired.has)
	{
		frame->ok = true;
		return;
	}
	if (!txn_visible_Seen_base(&frame->seen, frame->io, rule, tuple, &kept, &dirty, &changer, &commander,
							   frame->error))
		return;
	if (!kept && !under->has_serial)
	{
		frame->ok = true;
		return;
	}
	if (!doltgres_scan_Epochs_transform(frame->epochs, frame->epochs->base, &frame->transform, frame->error))
		return;
	frame->transform_live = true;
	paired = (DoltgresScanPaired)
	{
		.tuple_id = frame->tuple_id, .key = frame->paired.v, .has_root = frame->trees.has_root,
			.root = frame->trees.root, .keyed = frame->layout.keyed, .table = frame->layout.table,
			.order = frame->layout.order, .transform = frame->transform, .resolved = frame->resolved
	};
	frame->paired.has = frame->layout_live = frame->transform_live = false;
	if (!doltgres_scan_resolve(frame->io, frame->store, &paired, &frame->row, frame->error))
		return;
	frame->row_live = true;
	if (under->has_serial)
	{
		bool		conflicts;
		HostXid		xid;

		if (!txn_visible_Seen_base_conflict(&frame->seen, frame->io, &under->serial.read, tuple, frame->row.stamp.txn,
											kept, &conflicts, &xid, frame->error))
			return;
		if (conflicts)
			doltgres_scan_conflicts_push(under, xid);
	}
	if (kept)
	{
		frame->row.has_changer = doltgres_scan_stamped(changer, commander, &frame->row.changer);
		frame->row.dirty = dirty;
		*frame->result = frame->row;
		frame->row_live = false;
		*frame->has_result = true;
	}
	frame->ok = true;
}

/*
 * doltgres_scan_fetch answers the row version with a tuple id of a storage
 * under a snapshot, read through a store, or nothing when the snapshot
 * doesn't see it or the storage has none: the key the delta's tuple id table
 * or the base's tuple id tree pairs with the id, then the key's row version
 * with the id among those the snapshot sees in the delta, or else the base's
 * copy with the id, unless its ender in the delta ended it for the snapshot.
 * Once it read the delta, a read whose base an install moved
 * (txn_state_moved_past) reads again from the newest base, so it never pairs
 * a base with the delta a newer base's drop changed; a case holds it inside
 * its delta read, between its tuple id's lookup and its versions' read
 * (doltgres-fetch-keyed). It takes the layout, and leaves trees at the base it
 * read, so a caller that keeps them reads from there next (bugs row 478).
 */
bool
doltgres_scan_fetch(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
					DoltgresScanEpochs *epochs, DoltgresScanResolved *resolved, DoltgresScanTrees *trees,
					DoltgresScanLayout *layout, uint64 tuple_id, bool *has_result, DoltgresScanRow *result,
					HostError *error)
{
	doltgres_scan_fetch_frame_t frame = {.io = io, .store = store, .under = under, .epochs = epochs,
		.resolved = resolved, .trees = *trees,
		.layout = *layout, .layout_live = true, .tuple_id = tuple_id, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_fetch_body, doltgres_scan_fetch_pad);
	doltgres_scan_fetch_pad(&frame);
	if (frame.ok)
		*trees = frame.trees;
	return frame.ok;
}

/*
 * doltgres_scan_fetch_key_captures_t is fetch_key's pin scope's captures: the
 * table's tree, the key, its order, and whether the table is keyless.
 */
typedef struct doltgres_scan_fetch_key_captures_t
{
	TreesStoreTree root;
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
	bool		keyless;
} doltgres_scan_fetch_key_captures_t;

/* doltgres_scan_fetch_key_closure_0_body seeks the key and copies out its entry's rows. */
static void
doltgres_scan_fetch_key_closure_0_body(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;
	doltgres_scan_fetch_key_captures_t *captures = frame->captures;
	doltgres_scan_OptionCopies_t *result = frame->result;
	ChunksErrorError failed;
	bool		at;
	uint64		count;
	StdBytes	tuple;

	result->has = false;
	if (!doltgres_scan_seek_exact(frame, &captures->root, captures->key, captures->key_len, captures->order, &at))
		return;
	if (!at)
	{
		frame->ok = true;
		return;
	}
	if (!trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, &frame->stamps, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->stamps_live = true;
	if (!doltgres_scan_copies(&frame->cursor, captures->keyless, &count, &tuple, frame->error))
		return;
	if (frame->stamps.len != count)
	{
		*frame->error = doltgres_scan_uneven();
		return;
	}
	std_slice_to_vec_Checked(&frame->tuple, tuple.ptr, tuple.len, DOLTGRES_SCAN_BYTES);
	frame->tuple_live = true;
	if (!trees_cursor_Cursor_values(&frame->cursor, frame->io, frame->pin, &frame->values, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	result->has = true;
	result->v.tuple = frame->tuple;
	result->v.stamps = frame->stamps;
	result->v.values = frame->values;
	frame->tuple_live = frame->stamps_live = false;
	frame->ok = true;
}

/* doltgres_scan_fetch_key_closure_0 is fetch_key's pin scope. */
static bool
doltgres_scan_fetch_key_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, doltgres_scan_OptionCopies_t *result,
								  HostError *error)
{
	return doltgres_scan_cursor_scope(captures, io, pin, result, error, doltgres_scan_fetch_key_closure_0_body);
}

/*
 * doltgres_scan_fetch_key_frame_t is fetch_key's state across its pad: the
 * layout it took, the base entry's copies, the delta's hold, the key's
 * versions, its group, and the rows made so far, each while live.
 */
typedef struct doltgres_scan_fetch_key_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	DoltgresScanEpochs *epochs;
	DoltgresScanTrees trees;
	DoltgresScanLayout layout;
	bool		layout_live;
	const uint8 *key;
	size_t		key_len;
	doltgres_scan_OptionCopies_t copied;
	HostDeltaHold hold;
	bool		hold_live;
	DoltgresScanGroup group;
	bool		group_live;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_scan_fetch_key_frame_t;

/* doltgres_scan_fetch_key_pad drops what fetch_key holds. */
static void
doltgres_scan_fetch_key_pad(void *frame_)
{
	doltgres_scan_fetch_key_frame_t *frame = frame_;

	if (frame->result_live)
		doltgres_scan_rows_drop(frame->result);
	frame->result_live = false;
	if (frame->group_live)
		doltgres_scan_Group_drop(&frame->group);
	frame->group_live = false;
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	if (frame->copied.has)
		doltgres_scan_Copies_drop(&frame->copied.v);
	frame->copied.has = false;
	if (frame->layout_live)
		doltgres_scan_Layout_drop(&frame->layout);
	frame->layout_live = false;
}

/* doltgres_scan_fetch_key_body reads the base's copies and the delta's versions of the key and makes their rows. */
static void
doltgres_scan_fetch_key_body(void *frame_)
{
	doltgres_scan_fetch_key_frame_t *frame = frame_;
	StdVec		versions;
	StdVec		key;
	bool		has_copies;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	for (;;)
	{
		if (frame->trees.has_root)
		{
			doltgres_scan_fetch_key_captures_t captures = {frame->trees.root, frame->key, frame->key_len,
			&frame->layout.order, frame->layout.keyed.len == 0};

			if (!trees_pin_pin_ResultOptionCopies(frame->io, frame->store, doltgres_scan_fetch_key_closure_0,
												  &captures, NULL, &frame->copied, frame->error))
				return;
		}
		if (!txn_visible_versions(frame->io, &frame->hold, frame->under->state, frame->key, frame->key_len,
								  frame->trees.folded, &versions, frame->error))
			return;
		if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-fetch-key-read", frame->error))
		{
			txn_memtable_version_versions_drop(&versions, 0);
			return;
		}
		if (!txn_state_moved_past(frame->under->state, frame->trees.generation))
			break;
		txn_memtable_version_versions_drop(&versions, 0);
		if (frame->copied.has)
			doltgres_scan_Copies_drop(&frame->copied.v);
		frame->copied.has = false;
		if (!doltgres_scan_Trees_newer(&frame->trees, frame->io, frame->store, frame->under, frame->epochs,
									   frame->error))
			return;
	}
	std_slice_to_vec_Checked(&key, frame->key, frame->key_len, DOLTGRES_SCAN_BYTES);
	has_copies = frame->copied.has;
	frame->copied.has = false;
	if (!doltgres_scan_grouped(frame->io, frame->under, frame->epochs, &key, has_copies, &frame->copied.v, &versions,
							   &frame->group, frame->error))
		return;
	frame->group_live = true;
	std_vec_with_capacity_Checked(frame->result, frame->group.rows.len, DOLTGRES_SCAN_ROW);
	frame->result_live = true;
	for (size_t i = 0; i < frame->group.rows.len; i++)
	{
		DoltgresScanRow row;

		doltgres_scan_Found_row(&STD_VEC_AT(&frame->group.rows, DoltgresScanFound, i), &frame->layout.keyed,
								frame->layout.table, frame->key, frame->key_len, &frame->group.base, &row);
		std_vec_push_Checked(frame->result, &row, DOLTGRES_SCAN_ROW);
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_fetch_key answers the rows a snapshot sees of a row key in a
 * storage, read through a store, in the order a scan's group gives them: the
 * copies of the base's entry of the key that the key's versions in the delta
 * leave seen, then the delta's rows it sees, oldest first. Once it read the
 * delta, a read whose base an install moved reads again from the newest
 * base, as fetch does. It takes the layout, and leaves trees at the base it
 * read, as fetch does.
 */
bool
doltgres_scan_fetch_key(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
						DoltgresScanEpochs *epochs, DoltgresScanTrees *trees, DoltgresScanLayout *layout,
						const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	doltgres_scan_fetch_key_frame_t frame = {.io = io, .store = store, .under = under, .epochs = epochs,
		.trees = *trees, .layout = *layout, .layout_live = true, .key = key, .key_len = key_len, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_fetch_key_body, doltgres_scan_fetch_key_pad);
	doltgres_scan_fetch_key_pad(&frame);
	if (frame.ok)
		*trees = frame.trees;
	return frame.ok;
}

/* doltgres_scan_keyed_frame_t is keyed's state across its pad: the delta's key of the tuple id, while live. */
typedef struct doltgres_scan_keyed_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	uint64		tuple_id;
	doltgres_OptionVecU8_t written;
	bool	   *has_result;
	StdVec	   *result_0;
	bool	   *result_1;
	HostError  *error;
	bool		ok;
} doltgres_scan_keyed_frame_t;

/* doltgres_scan_keyed_pad drops the delta's key. */
static void
doltgres_scan_keyed_pad(void *frame_)
{
	doltgres_scan_option_drop(&((doltgres_scan_keyed_frame_t *) frame_)->written);
}

/* doltgres_scan_keyed_body reads the delta's key of the tuple id, then the newest base's. */
static void
doltgres_scan_keyed_body(void *frame_)
{
	doltgres_scan_keyed_frame_t *frame = frame_;
	HostRow		tuple = {frame->tuple_id};
	doltgres_OptionVecU8_t paired = {0};
	bool		some;
	TxnBaseBase base;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;

	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-tuple-keyed", frame->error) ||
		!txn_memtable_tuples_get(frame->io, host_shared_Held_record(frame->held), tuple, &frame->written.has,
								 &frame->written.v, frame->error) ||
		!txn_state_newest(frame->io, frame->held, &some, &base, frame->error))
		return;
	has_tuple_ids = some && base.has_tuple_ids;
	if (has_tuple_ids)
		tuple_ids = base.tuple_ids;
	if (some)
		txn_base_Base_drop(&base);
	if (has_tuple_ids &&
		!doltgres_scan_pair(frame->io, frame->store, tuple_ids, frame->tuple_id, &paired, frame->error))
		return;
	*frame->result_1 = paired.has;
	*frame->has_result = frame->written.has || paired.has;
	if (frame->written.has)
	{
		*frame->result_0 = frame->written.v;
		frame->written.has = false;
		doltgres_scan_option_drop(&paired);
	}
	else if (paired.has)
		*frame->result_0 = paired.v;
	frame->ok = true;
}

/*
 * doltgres_scan_keyed answers the row key a tuple id names in a held
 * storage, the delta's tuple id table's, else the newest base's tuple id
 * tree's, read through a store, with whether that base holds the row, or
 * none for a tuple id no layer holds. It reads the base after the delta, so
 * a row a fold's drop takes out of the delta after the delta's read is in
 * the base it reads (bugs rows 240 and 315); it first pauses at
 * doltgres-tuple-keyed.
 */
bool
doltgres_scan_keyed(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint64 tuple_id,
					bool *has_result, StdVec *result_0, bool *result_1, HostError *error)
{
	doltgres_scan_keyed_frame_t frame = {io, store, held, tuple_id, {0}, has_result, result_0, result_1, error,
	false};

	std_landing(&frame, doltgres_scan_keyed_body, doltgres_scan_keyed_pad);
	doltgres_scan_keyed_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_based reports whether a storage's newest base holds the row
 * of a tuple id, its tuple id tree pairing it, read through a store.
 */
bool
doltgres_scan_based(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, uint64 tuple_id,
					bool *result, HostError *error)
{
	bool		some;
	TxnBaseBase base;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	doltgres_OptionVecU8_t paired;

	if (!txn_state_newest(io, held, &some, &base, error))
		return false;
	has_tuple_ids = some && base.has_tuple_ids;
	if (has_tuple_ids)
		tuple_ids = base.tuple_ids;
	if (some)
		txn_base_Base_drop(&base);
	*result = false;
	if (!has_tuple_ids)
		return true;
	if (!doltgres_scan_pair(io, store, tuple_ids, tuple_id, &paired, error))
		return false;
	*result = paired.has;
	doltgres_scan_option_drop(&paired);
	return true;
}

/*
 * doltgres_scan_end_before_captures_t is end_before's call's captures: the
 * transform's top-level xid, command, and subtransaction.
 */
typedef struct doltgres_scan_end_before_captures_t
{
	uint64		xid;
	uint32		command;
	uint64		sub;
} doltgres_scan_end_before_captures_t;

/*
 * doltgres_scan_end_before_closure_0 is end_before's call: the epochs stopped
 * before the transform, their composed ones cleared.
 */
static bool
doltgres_scan_end_before_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_end_before_captures_t *before = captures;
	DoltgresScanEpochs *epochs = &scan->epochs;

	for (size_t at = 0; at < epochs->chain.len; at++)
	{
		const TxnBasePending *pending = &STD_VEC_AT(&epochs->chain, TxnVisibleLive, at).f0;

		if (pending->xid == before->xid && pending->command == before->command && pending->sub == before->sub)
		{
			epochs->has_end = true;
			epochs->end = at;
			std_vec_clear(&epochs->composed, sizeof(doltgres_OptionVecU8_t), doltgres_scan_composed_drop);
			return true;
		}
	}
	{
		static const char text[] = "a scan's layout names a transform its chain lacks";
		StdString	message;

		std_string_from_Checked(&message, text, sizeof(text) - 1);
		*error = host_Error_fork((const uint8 *) "XX000", &message);
	}
	return false;
}

/*
 * doltgres_scan_end_before stops a scan's epoch before a pending transform
 * of its storage's chain, by its top-level xid, command, and
 * subtransaction's xid, so its rows read in the layout that transform's
 * table had before it.
 */
bool
doltgres_scan_end_before(HostIoIo *io, HostOwnedHandle handle, uint64 xid, uint32 command, uint64 sub,
						 HostError *error)
{
	doltgres_scan_end_before_captures_t captures = {xid, command, sub};

	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_end_before_closure_0, &captures, NULL, error);
}

/* doltgres_scan_unchecked_closure_0 is unchecked's call: whether the layout was unchecked, marking it checked. */
static bool
doltgres_scan_unchecked_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, bool *result,
								  HostError *error)
{
	*result = !scan->epochs.checked;
	scan->epochs.checked = true;
	return true;
}

/*
 * doltgres_scan_unchecked reports whether a scan's slot's layout is still to
 * be checked against the transforms its transaction chained, once, marking
 * it checked.
 */
bool
doltgres_scan_unchecked(HostIoIo *io, HostOwnedHandle handle, bool *result, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withBool(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_unchecked_closure_0, NULL, NULL, result, error);
}

/* doltgres_scan_set_range_closure_0 is set_range's call: the range set and the read started over. */
static bool
doltgres_scan_set_range_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	const uint64 *range = captures;

	scan->has_range = true;
	scan->range_first = range[0];
	scan->range_last = range[1];
	scan->has_given = false;
	return true;
}

/*
 * doltgres_scan_set_range sets the tuple ids a scan's tid range reads give,
 * the first and the last, and starts the scan over, as heap's
 * heap_set_tidrange does.
 */
bool
doltgres_scan_set_range(HostIoIo *io, HostOwnedHandle handle, uint64 first, uint64 last, HostError *error)
{
	uint64		range[2] = {first, last};

	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_set_range_closure_0, range, NULL, error);
}

/* doltgres_scan_OptionTupleU64U64_t is an Option<(u64, u64)>, a tid range. */
typedef struct doltgres_scan_OptionTupleU64U64_t
{
	bool		has;
	uint64		f0;
	uint64		f1;
} doltgres_scan_OptionTupleU64U64_t;

/* Scan's Table's with at Option<(u64, u64)>, for range. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS OptionTupleU64U64
#define HOST_OWNED_WITH_T doltgres_scan_OptionTupleU64U64_t
#include "doltgres/host/owned_template.h"

/* doltgres_scan_range_closure_0 is range's call: the scan's tid range. */
static bool
doltgres_scan_range_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
							  doltgres_scan_OptionTupleU64U64_t *result, HostError *error)
{
	result->has = scan->has_range;
	result->f0 = scan->range_first;
	result->f1 = scan->range_last;
	return true;
}

/* doltgres_scan_range answers the tuple ids a scan's tid range reads give, the first and the last, or none. */
bool
doltgres_scan_range(HostIoIo *io, HostOwnedHandle handle, bool *has_result, uint64 *result_0, uint64 *result_1,
					HostError *error)
{
	doltgres_scan_OptionTupleU64U64_t range;

	if (!host_owned_TableDoltgresScanScan_withOptionTupleU64U64(&DOLTGRES_SCAN_SCANS, io, handle,
																doltgres_scan_range_closure_0, NULL, NULL, &range,
																error))
		return false;
	*has_result = range.has;
	if (range.has)
	{
		*result_0 = range.f0;
		*result_1 = range.f1;
	}
	return true;
}

/* doltgres_scan_u64_lt is the fn item u64's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
doltgres_scan_u64_lt(void *captures, const void *a, const void *b)
{
	return *(const uint64 *) a < *(const uint64 *) b;
}

/* written's sort of the delta's tuple ids: sort_unstable at u64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT doltgres_scan_u64_lt
#include "doltgres/std/sort_template.h"

/* doltgres_scan_u64_eq is u64's PartialEq, which written's dedup takes. */
static bool
doltgres_scan_u64_eq(const void *self, const void *other)
{
	return *(const uint64 *) self == *(const uint64 *) other;
}

/*
 * doltgres_scan_written_frame_t is written's state across its pad: the tuple
 * ids so far, the key after which it reads, and the delta's hold, while live.
 */
typedef struct doltgres_scan_written_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	uint64		folded;
	StdVec	   *result;
	bool		result_live;
	doltgres_OptionVecU8_t after;
	HostDeltaHold hold;
	bool		hold_live;
	bool		has_next;
	txn_TupleVecU8VecVersion_t next;
	HostError  *error;
	bool		ok;
} doltgres_scan_written_frame_t;

/* doltgres_scan_written_pad drops what written holds. */
static void
doltgres_scan_written_pad(void *frame_)
{
	doltgres_scan_written_frame_t *frame = frame_;

	doltgres_scan_delta_drop(&frame->has_next, &frame->next);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	doltgres_scan_option_drop(&frame->after);
	if (frame->result_live)
		std_vec_free_Checked(frame->result, DOLTGRES_SCAN_U64);
	frame->result_live = false;
}

/* doltgres_scan_written_body walks the delta's keys and collects their row versions' tuple ids. */
static void
doltgres_scan_written_body(void *frame_)
{
	doltgres_scan_written_frame_t *frame = frame_;

	std_vec_new(frame->result, DOLTGRES_SCAN_U64);
	frame->result_live = true;
	for (;;)
	{
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_visible_next(frame->io, &frame->hold, frame->under->state, frame->after.has, frame->after.v.ptr,
							  frame->after.has ? frame->after.v.len : 0, TXN_MEMTABLE_TABLE_DIRECTION_FORWARD,
							  frame->folded, &frame->has_next, &frame->next, frame->error))
			return;
		if (!frame->has_next)
		{
			frame->hold_live = false;
			host_delta_Hold_drop(&frame->hold);
			break;
		}
		for (size_t i = 0; i < frame->next.f1.len; i++)
		{
			TxnVisibleRowVersion row;

			if (txn_visible_RowVersion_of(&STD_VEC_AT(&frame->next.f1, TxnMemtableVersionVersion, i), &row))
				std_vec_push_Checked(frame->result, &row.put.tuple.f0, DOLTGRES_SCAN_U64);
		}
		doltgres_scan_option_drop(&frame->after);
		frame->after.has = true;
		frame->after.v = frame->next.f0;
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
		frame->has_next = false;
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	std_slice_sort_unstable_U64(frame->result->ptr, frame->result->len);
	std_vec_dedup(frame->result, DOLTGRES_SCAN_U64, doltgres_scan_u64_eq, NULL);
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_written answers the tuple ids of the delta's row versions
 * the base folded at a fold point didn't apply, sorted, read key by key with
 * no lock held between, as a tid range read's first read collects them once
 * for every range the scan reads.
 */
static bool
doltgres_scan_written(HostIoIo *io, const DoltgresScanUnder *under, uint64 folded, StdVec *result, HostError *error)
{
	doltgres_scan_written_frame_t frame = {.io = io, .under = under, .folded = folded, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_written_body, doltgres_scan_written_pad);
	doltgres_scan_written_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_base_tid_captures_t is base_tid's pin scope's captures: the
 * tuple id tree, the id it starts from, the range, and the way.
 */
typedef struct doltgres_scan_base_tid_captures_t
{
	TreesStoreTree tuple_ids;
	uint64		from;
	uint64		first;
	uint64		last;
	DoltgresScanDirection way;
} doltgres_scan_base_tid_captures_t;

/*
 * doltgres_scan_base_tid_closure_0_body seeks the start the way given and
 * answers the tuple id there, if in the range.
 */
static void
doltgres_scan_base_tid_closure_0_body(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;
	doltgres_scan_base_tid_captures_t *captures = frame->captures;
	doltgres_scan_OptionU64_t *result = frame->result;
	static const TreesKeyOrder bytes = {.tag = TREES_KEY_ORDER_BYTES};
	TreesStoreNamed tree = TREES_STORE_TREE_NAMED(&captures->tuple_ids);
	ChunksErrorError failed;
	uint8		id[6];
	StdBytes	key;
	uint64		tuple_id;

	result->has = false;
	txn_base_tuple_id_key(captures->from, id);
	if (captures->way == DOLTGRES_SCAN_DIRECTION_FORWARD)
	{
		if (!trees_cursor_Cursor_seek(frame->io, frame->pin, tree, id, sizeof(id), &bytes, &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
	}
	else
	{
		if (!trees_cursor_Cursor_seek_past(frame->io, frame->pin, tree, id, sizeof(id), &bytes, &frame->cursor,
										   &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
		if (trees_cursor_Cursor_is_valid(&frame->cursor))
		{
			if (!doltgres_scan_step(frame->io, frame->pin, &frame->cursor, captures->way, frame->error))
				return;
		}
		else
		{
			TreesCursorCursor last;

			if (!trees_cursor_Cursor_last(frame->io, frame->pin, tree, &last, &failed))
			{
				doltgres_scan_storage_failed(&failed, frame->error);
				return;
			}
			trees_cursor_Cursor_drop(&frame->cursor);
			frame->cursor = last;
		}
	}
	if (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		key = trees_cursor_Cursor_key(&frame->cursor);
		if (!txn_base_tuple_id_of(key.ptr, key.len, &tuple_id, frame->error))
			return;
		result->has = tuple_id >= captures->first && tuple_id <= captures->last;
		result->v = tuple_id;
	}
	frame->ok = true;
}

/* doltgres_scan_base_tid_closure_0 is base_tid's pin scope. */
static bool
doltgres_scan_base_tid_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, doltgres_scan_OptionU64_t *result,
								 HostError *error)
{
	return doltgres_scan_cursor_scope(captures, io, pin, result, error, doltgres_scan_base_tid_closure_0_body);
}

/*
 * doltgres_scan_base_tid answers the base's nearest tuple id past one a read
 * gave the way it goes, or the range's first that way with none, read from
 * the base's tuple id tree through a store, or none when no id of the tree
 * is left in the range.
 */
static bool
doltgres_scan_base_tid(HostIoIo *io, TreesStoreNodeStore store, TreesStoreTree tuple_ids, bool has_given,
					   uint64 given, uint64 first, uint64 last, DoltgresScanDirection way,
					   doltgres_scan_OptionU64_t *result, HostError *error)
{
	doltgres_scan_base_tid_captures_t captures = {.tuple_ids = tuple_ids, .first = first, .last = last, .way = way};

	if (!has_given)
		captures.from = way == DOLTGRES_SCAN_DIRECTION_FORWARD ? first : last;
	else if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
		captures.from = given + 1;
	else if (given > 0)
		captures.from = given - 1;
	else
	{
		result->has = false;
		return true;
	}
	return trees_pin_pin_ResultOptionU64(io, store, doltgres_scan_base_tid_closure_0, &captures, NULL, result, error);
}

/* doltgres_scan_below is delta_tid's first partition_point predicate: a tuple id below the start. */
static bool
doltgres_scan_below(void *captures, const void *tuple_id)
{
	return *(const uint64 *) tuple_id < *(const uint64 *) captures;
}

/* doltgres_scan_at_or_below is delta_tid's second partition_point predicate: a tuple id at or below the end. */
static bool
doltgres_scan_at_or_below(void *captures, const void *tuple_id)
{
	return *(const uint64 *) tuple_id <= *(const uint64 *) captures;
}

/*
 * doltgres_scan_delta_tid answers the delta's nearest tuple id in a range
 * past one a read gave the way it goes, or the range's first that way with
 * none, from the range's sorted tuple ids, or false for none.
 */
static bool
doltgres_scan_delta_tid(const uint64 *written, size_t written_len, bool has_given, uint64 given, uint64 first,
						uint64 last, DoltgresScanDirection way, uint64 *result)
{
	size_t		at;

	if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
	{
		uint64		from = has_given ? given + 1 : first;

		at = std_slice_partition_point(written, written_len, sizeof(uint64), doltgres_scan_below, &from, NULL);
		if (at >= written_len)
			return false;
		*result = written[at];
	}
	else
	{
		uint64		to;

		if (has_given && given == 0)
			return false;
		to = has_given ? given - 1 : last;
		at = std_slice_partition_point(written, written_len, sizeof(uint64), doltgres_scan_at_or_below, &to, NULL);
		if (at == 0)
			return false;
		*result = written[at - 1];
	}
	return *result >= first && *result <= last;
}

/*
 * doltgres_scan_OptionChosen_t is next_in_range's choice in the record: a
 * tuple id, the trees and layout its fetch reads, and the epochs moved out
 * of the record, or none, with whether an install moved the base while it
 * collected the delta's tuple ids, which the read moves to (rebased) before
 * it chooses again. The tuple ids it chooses from are the base's and the
 * delta's from before any newer base's drop, so none is missed; each fetch
 * reads its row from the newest base (fetch), and a read whose fetch found a
 * newer base moves to it once, so its later fetches start there.
 */
typedef struct doltgres_scan_OptionChosen_t
{
	bool		has;
	bool		moved;
	uint64		tuple_id;
	DoltgresScanTrees trees;
	DoltgresScanLayout layout;
	DoltgresScanEpochs epochs;
	DoltgresScanResolved resolved;
} doltgres_scan_OptionChosen_t;

/* DOLTGRES_SCAN_CHOSEN_DROP drops a choice a call left. */
#define DOLTGRES_SCAN_CHOSEN_DROP(place) \
	do { \
		if ((place)->has) \
		{ \
			doltgres_scan_Layout_drop(&(place)->layout); \
			doltgres_scan_Epochs_drop(&(place)->epochs); \
			doltgres_scan_Resolved_drop(&(place)->resolved); \
		} \
	} while (0)

/* Scan's Table's with at Option<(u64, Trees, Layout, Epochs)>, for next_in_range. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS OptionChosen
#define HOST_OWNED_WITH_T doltgres_scan_OptionChosen_t
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_CHOSEN_DROP(place)
#include "doltgres/host/owned_template.h"

/* doltgres_scan_in_range_t is next_in_range's captures: the store, what the read is under, and its way. */
typedef struct doltgres_scan_in_range_t
{
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	DoltgresScanDirection way;
} doltgres_scan_in_range_t;

/*
 * doltgres_scan_next_in_range_closure_0 is next_in_range's choice in the
 * record: the nearest tuple id past the last read's.
 */
static bool
doltgres_scan_next_in_range_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									  doltgres_scan_OptionChosen_t *result, HostError *error)
{
	doltgres_scan_in_range_t *read = captures;
	bool		has_range = scan->has_parallel ? scan->parallel.has_ranged : scan->has_range;
	uint64		first = scan->has_parallel ? scan->parallel.ranged_first : scan->range_first;
	uint64		last = scan->has_parallel ? scan->parallel.ranged_last : scan->range_last;
	doltgres_scan_OptionU64_t base = {0};
	bool		has_delta;
	uint64		delta = 0;

	result->has = result->moved = false;
	if (!has_range)
		return true;
	if (!scan->has_written)
	{
		if (!doltgres_scan_written(io, read->under, scan->folded, &scan->written, error))
			return false;
		if (txn_state_moved_past(read->under->state, scan->generation))
		{
			std_vec_free_Checked(&scan->written, DOLTGRES_SCAN_U64);
			result->moved = true;
			return true;
		}
		scan->has_written = true;
	}
	if (scan->has_tuple_ids &&
		!doltgres_scan_base_tid(io, read->store, scan->tuple_ids, scan->has_given, scan->given, first, last,
								read->way, &base, error))
		return false;
	has_delta = doltgres_scan_delta_tid(scan->written.ptr, scan->written.len, scan->has_given, scan->given, first,
										last, read->way, &delta);
	if (base.has && has_delta)
		scan->given = read->way == DOLTGRES_SCAN_DIRECTION_FORWARD ? Min(base.v, delta) : Max(base.v, delta);
	else
		scan->given = base.has ? base.v : delta;
	scan->has_given = base.has || has_delta;
	if (!scan->has_given)
		return true;
	std_vec_clone(&result->layout.keyed, &scan->keyed, DOLTGRES_SCAN_BYTES);
	result->layout.table = scan->table;
	trees_KeyOrder_Clone_clone(&scan->order, &result->layout.order);
	result->trees = (DoltgresScanTrees)
	{
		scan->has_tree, scan->tree, scan->has_tuple_ids, scan->tuple_ids, scan->folded, scan->generation
	};
	result->tuple_id = scan->given;
	result->epochs = scan->epochs;
	doltgres_scan_Epochs_Default_default(&scan->epochs);
	result->resolved = scan->resolved;
	scan->resolved = (DoltgresScanResolved)
	{
		0
	};
	result->has = true;
	return true;
}

/*
 * doltgres_scan_next_in_range_frame_t is next_in_range's state across its pad:
 * the choice's epochs and memo of a base leaf's stamps, and the fetched row,
 * while live.
 */
typedef struct doltgres_scan_next_in_range_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	DoltgresScanDirection way;
	DoltgresScanEpochs epochs;
	DoltgresScanResolved resolved;
	bool		epochs_live;
	bool		has_row;
	DoltgresScanRow row;
	bool	   *has_result;
	DoltgresScanRow *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_next_in_range_frame_t;

/* doltgres_scan_epochs_put_drop drops the epochs and memo of a put that never ran. */
static void
doltgres_scan_epochs_put_drop(void *frame_)
{
	doltgres_scan_next_in_range_frame_t *frame = frame_;

	doltgres_scan_Epochs_drop(&frame->epochs);
	doltgres_scan_Resolved_drop(&frame->resolved);
}

/* doltgres_scan_next_in_range_closure_1 is next_in_range's put of the epochs and memo back in the record. */
static bool
doltgres_scan_next_in_range_closure_1(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_next_in_range_frame_t *frame = captures;

	doltgres_scan_Epochs_drop(&scan->epochs);
	scan->epochs = frame->epochs;
	doltgres_scan_Resolved_drop(&scan->resolved);
	scan->resolved = frame->resolved;
	return true;
}

/* doltgres_scan_next_in_range_pad drops what next_in_range holds. */
static void
doltgres_scan_next_in_range_pad(void *frame_)
{
	doltgres_scan_next_in_range_frame_t *frame = frame_;

	if (frame->has_row)
		doltgres_scan_Row_drop(&frame->row);
	frame->has_row = false;
	if (frame->epochs_live)
		doltgres_scan_epochs_put_drop(frame);
	frame->epochs_live = false;
}

/* doltgres_scan_next_in_range_body chooses tuple ids in the record and fetches each outside it until one is seen. */
static void
doltgres_scan_next_in_range_body(void *frame_)
{
	doltgres_scan_next_in_range_frame_t *frame = frame_;
	doltgres_scan_in_range_t read = {frame->store, frame->under, frame->way};

	for (;;)
	{
		doltgres_scan_OptionChosen_t chosen;
		DoltgresScanRebased rebasing;

		if (!host_owned_TableDoltgresScanScan_withOptionChosen(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
															   doltgres_scan_next_in_range_closure_0, &read, NULL,
															   &chosen, frame->error))
			return;
		if (chosen.moved)
		{
			if (!doltgres_scan_rebased(frame->io, frame->store, frame->under, frame->handle, &rebasing, frame->error))
				return;
			doltgres_scan_Rebased_drop(&rebasing);
			continue;
		}
		if (!chosen.has)
		{
			*frame->has_result = false;
			frame->ok = true;
			return;
		}
		frame->epochs = chosen.epochs;
		frame->resolved = chosen.resolved;
		frame->epochs_live = true;
		if (!doltgres_scan_fetch(frame->io, frame->store, frame->under, &frame->epochs, &frame->resolved, &chosen.trees,
								 &chosen.layout, chosen.tuple_id, &frame->has_row, &frame->row, frame->error))
			return;
		frame->epochs_live = false;
		if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
													   doltgres_scan_next_in_range_closure_1, frame,
													   doltgres_scan_epochs_put_drop, frame->error))
			return;
		if (txn_state_moved_past(frame->under->state, chosen.trees.generation))
		{
			if (!doltgres_scan_rebased(frame->io, frame->store, frame->under, frame->handle, &rebasing, frame->error))
				return;
			doltgres_scan_Rebased_drop(&rebasing);
		}
		if (frame->has_row)
		{
			*frame->has_result = true;
			*frame->result = frame->row;
			frame->has_row = false;
			frame->ok = true;
			return;
		}
	}
}

/*
 * doltgres_scan_next_in_range reads the scan's next row of its tid range in
 * a direction under its snapshot, through a store, and answers it, or
 * nothing when there is none: the row version with the nearest tuple id past
 * the last read's that way, of the base's tuple id tree and of the delta's
 * tuple ids in the range, fetched as fetch fetches one and passed over when
 * the snapshot doesn't see it. The fetch runs with the record put back, its
 * epochs moved out until it is done.
 */
bool
doltgres_scan_next_in_range(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
							HostOwnedHandle handle, DoltgresScanDirection way, bool *has_result,
							DoltgresScanRow *result, HostError *error)
{
	doltgres_scan_next_in_range_frame_t frame = {.io = io, .store = store, .under = under, .handle = handle,
	.way = way, .has_result = has_result, .result = result, .error = error};

	std_landing(&frame, doltgres_scan_next_in_range_body, doltgres_scan_next_in_range_pad);
	doltgres_scan_next_in_range_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_splits_captures_t is splits' arguments in its landing frame: the range starts and the result. */
typedef struct doltgres_scan_splits_captures_t
{
	const StdVec *starts;
	size_t		starts_len;
	StdVec	   *result;
} doltgres_scan_splits_captures_t;

/* doltgres_scan_splits_pad frees the split keys of an unwound collection. */
static void
doltgres_scan_splits_pad(void *frame_)
{
	std_vec_free_Checked(((doltgres_scan_splits_captures_t *) frame_)->result, DOLTGRES_SCAN_BYTES);
}

/* doltgres_scan_splits_body picks the starts spread evenly and writes each that fits the budget. */
static void
doltgres_scan_splits_body(void *frame_)
{
	doltgres_scan_splits_captures_t *frame = frame_;
	StdVec	   *out = frame->result;
	size_t		len = 0;
	size_t		picked;
	size_t		walked = 0;
	size_t		next = 0;

	for (size_t i = 0; i < frame->starts_len; i++)
		if (frame->starts[i].len > 0)
			len++;
	picked = Min(len, DOLTGRES_SCAN_SPLITS_MOST);
	for (size_t i = 0; i < picked; i++)
	{
		size_t		at = i * len / picked;
		const StdVec *key;
		uint8		bytes[2];

		for (;; next++)
			if (frame->starts[next].len > 0 && walked++ == at)
				break;
		key = &frame->starts[next++];
		if (key->len > UINT16_MAX || out->len + 2 + key->len > DOLTGRES_SCAN_SPLITS_BUDGET)
			continue;
		std_u16_to_le_bytes((uint16) key->len, bytes);
		std_vec_extend_from_slice_Checked(out, bytes, 2, DOLTGRES_SCAN_BYTES);
		std_vec_extend_from_slice_Checked(out, key->ptr, key->len, DOLTGRES_SCAN_BYTES);
	}
}

/*
 * doltgres_scan_splits writes the split keys a leader stores for a storage
 * with no base, from the delta's range starts in byte order: up to
 * DOLTGRES_SCAN_SPLITS_MOST of them, spread evenly, each its length in two
 * bytes, low byte first, then its bytes, leaving out the empty first start
 * and a key that doesn't fit the budget's bytes left.
 */
void
doltgres_scan_splits(const StdVec *starts, size_t starts_len, StdVec *result)
{
	doltgres_scan_splits_captures_t frame = {starts, starts_len, result};

	std_vec_new(result, DOLTGRES_SCAN_BYTES);
	std_landing(&frame, doltgres_scan_splits_body, doltgres_scan_splits_pad);
}

/* doltgres_scan_split_keys_frame_t is split_keys' state across its pad: the keys read so far, and the bytes left. */
typedef struct doltgres_scan_split_keys_frame_t
{
	StdVec	   *keys;
	const uint8 *bytes;
	size_t		bytes_len;
	HostError  *error;
	bool		ok;
} doltgres_scan_split_keys_frame_t;

/* doltgres_scan_split_keys_pad frees the split keys of an unwound read. */
static void
doltgres_scan_split_keys_pad(void *frame_)
{
	doltgres_scan_vecs_drop(((doltgres_scan_split_keys_frame_t *) frame_)->keys);
}

/* doltgres_scan_split_keys_body reads each key's two-byte length and its bytes. */
static void
doltgres_scan_split_keys_body(void *frame_)
{
	doltgres_scan_split_keys_frame_t *frame = frame_;

	while (frame->bytes_len >= 2)
	{
		size_t		len = frame->bytes[0] | (size_t) frame->bytes[1] << 8;
		StdVec		key;

		if (frame->bytes_len - 2 < len)
		{
			*frame->error = doltgres_scan_internal("a split key is cut short");
			return;
		}
		std_slice_to_vec_Checked(&key, frame->bytes + 2, len, DOLTGRES_SCAN_BYTES);
		std_vec_push_Checked(frame->keys, &key, DOLTGRES_SCAN_VEC);
		frame->bytes += 2 + len;
		frame->bytes_len -= 2 + len;
	}
	if (frame->bytes_len != 0)
	{
		*frame->error = doltgres_scan_internal("a split key's length is cut short");
		return;
	}
	frame->ok = true;
}

/*
 * doltgres_scan_split_keys reads the split keys splits stored, refusing
 * bytes cut short as an internal error, since only the leader writes them.
 */
bool
doltgres_scan_split_keys(const uint8 *bytes, size_t bytes_len, StdVec *result, HostError *error)
{
	doltgres_scan_split_keys_frame_t frame = {result, bytes, bytes_len, error, false};

	std_vec_new(result, DOLTGRES_SCAN_VEC);
	std_landing(&frame, doltgres_scan_split_keys_body, doltgres_scan_split_keys_pad);
	if (!frame.ok)
		doltgres_scan_vecs_drop(result);
	return frame.ok;
}

/*
 * doltgres_scan_begin_parallel_frame_t is begin_parallel's state across its
 * pad: what it took, and its reader, while live.
 */
typedef struct doltgres_scan_begin_parallel_frame_t
{
	HostIoIo   *io;
	DoltgresScanShared shared;
	bool		shared_live;
	uint64		generation;
	StdVec		keyed;
	bool		keyed_live;
	uint32		table;
	TreesKeyOrder order;
	bool		order_live;
	HostSharedReader reader;
	bool		reader_live;
	HostOwnedHandle *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_begin_parallel_frame_t;

/* doltgres_scan_begin_parallel_pad drops what begin_parallel still holds. */
static void
doltgres_scan_begin_parallel_pad(void *frame_)
{
	doltgres_scan_begin_parallel_frame_t *frame = frame_;

	if (frame->reader_live)
		host_shared_Reader_drop(&frame->reader);
	frame->reader_live = false;
	if (frame->shared_live)
		doltgres_scan_Shared_drop(&frame->shared);
	frame->shared_live = false;
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->order_live = false;
	if (frame->keyed_live)
		std_vec_free_Checked(&frame->keyed, DOLTGRES_SCAN_BYTES);
	frame->keyed_live = false;
}

/* doltgres_scan_begin_parallel_body takes a reader and makes the parallel scan's record. */
static void
doltgres_scan_begin_parallel_body(void *frame_)
{
	doltgres_scan_begin_parallel_frame_t *frame = frame_;
	DoltgresScanShared *shared = &frame->shared;
	DoltgresScanScan scan = {0};

	if (!host_shared_read(frame->io, &frame->reader, frame->error))
		return;
	frame->reader_live = true;
	scan.reader = frame->reader;
	scan.place.tag = DOLTGRES_SCAN_PLACE_START;
	scan.has_tree = shared->has_tree;
	scan.tree = shared->tree;
	scan.folded = shared->folded;
	scan.epochs = shared->epochs;
	scan.generation = frame->generation;
	scan.blocks = shared->has_tree ? (shared->fresh_end >> 16) + 1 : 0;
	scan.keyed = frame->keyed;
	scan.table = frame->table;
	scan.has_tuple_ids = shared->has_tuple_ids;
	scan.tuple_ids = shared->tuple_ids;
	scan.order = frame->order;
	scan.has_parallel = true;
	scan.parallel.entries = shared->entries;
	scan.parallel.units = shared->units;
	scan.parallel.splits = shared->splits;
	scan.parallel.has_shared = shared->has_tree;
	scan.parallel.shared = shared->tree;
	scan.page = doltgres_scan_ended();
	frame->reader_live = frame->shared_live = frame->keyed_live = frame->order_live = false;
	frame->ok = host_owned_TableDoltgresScanScan_make(&DOLTGRES_SCAN_SCANS, frame->io, scan, frame->result,
													  frame->error);
}

/*
 * doltgres_scan_begin_parallel makes a parallel scan of the base the leader
 * shared, the storage's newest at a base generation or none newer, in a
 * table of a keyed layout, an object id, and a tree's order, each of which
 * it takes, and answers its handle; its first read asks for its first unit.
 */
bool
doltgres_scan_begin_parallel(HostIoIo *io, DoltgresScanShared *shared, uint64 generation, StdVec *keyed,
							 uint32 table, TreesKeyOrder *order, HostOwnedHandle *result, HostError *error)
{
	doltgres_scan_begin_parallel_frame_t frame = {.io = io, .shared = *shared, .shared_live = true,
		.generation = generation, .keyed = *keyed,
		.keyed_live = true, .table = table, .order = *order, .order_live = true, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_begin_parallel_body, doltgres_scan_begin_parallel_pad);
	doltgres_scan_begin_parallel_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_is_parallel_closure_0 is is_parallel's call: whether the scan has a parallel part. */
static bool
doltgres_scan_is_parallel_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, bool *result,
									HostError *error)
{
	*result = scan->has_parallel;
	return true;
}

/* doltgres_scan_is_parallel reports whether a scan reads a parallel scan's units. */
bool
doltgres_scan_is_parallel(HostIoIo *io, HostOwnedHandle handle, bool *result, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withBool(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_is_parallel_closure_0, NULL, NULL, result, error);
}

/*
 * doltgres_scan_unit_range answers the entries a parallel scan's unit
 * covers, its first ordinal and the one past its last: unit b of the units
 * covers b * entries / units up to (b + 1) * entries / units.
 */
static void
doltgres_scan_unit_range(uint64 unit, uint64 units, uint64 entries, uint64 *first, uint64 *end)
{
	StdU128		wide_entries = std_u128_from_u64(entries);
	StdU128		wide_units = std_u128_from_u64(units);

	*first = std_div_u128(std_mul_u128(std_u128_from_u64(unit), wide_entries, RUST_FILE, 1727, 24), wide_units,
						  RUST_FILE, 1727, 24).lo;
	*end = std_div_u128(std_mul_u128(std_u128_from_u64(unit + 1), wide_entries, RUST_FILE, 1727, 24), wide_units,
						RUST_FILE, 1727, 24).lo;
}

/* doltgres_scan_start_unit_closure_0 is start_unit's call: the unit's entries set and the merge started over. */
static bool
doltgres_scan_start_unit_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	uint64		unit = *(const uint64 *) captures;
	DoltgresScanPlace start = {.tag = DOLTGRES_SCAN_PLACE_START};

	if (!scan->has_parallel)
	{
		*error = doltgres_scan_internal("a serial scan was given a unit");
		return false;
	}
	doltgres_scan_unit_range(unit, scan->parallel.units, scan->parallel.entries, &scan->parallel.unit.first,
							 &scan->parallel.unit.end);
	scan->parallel.unit.number = unit;
	scan->parallel.has_unit = true;
	if (scan->has_group)
		doltgres_scan_Group_drop(&scan->group);
	scan->has_group = false;
	doltgres_scan_Place_set(&scan->place, start);
	if (scan->has_run)
		doltgres_scan_vecs_drop(&scan->run.keys);
	scan->has_run = false;
	doltgres_scan_option_drop(&scan->bound);
	return true;
}

/*
 * doltgres_scan_start_unit starts a parallel sequential scan's unit, the
 * entries unit_range gives it, so the scan's next read begins the unit's
 * merge.
 */
bool
doltgres_scan_start_unit(HostIoIo *io, HostOwnedHandle handle, uint64 unit, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_start_unit_closure_0, &unit, NULL, error);
}

/*
 * doltgres_scan_TupleOptionVecU8OptionVecU8_t is an (Option<Vec<u8>>,
 * Option<Vec<u8>>): a unit's low and high bounds.
 */
typedef struct doltgres_scan_TupleOptionVecU8OptionVecU8_t
{
	doltgres_OptionVecU8_t f0;
	doltgres_OptionVecU8_t f1;
} doltgres_scan_TupleOptionVecU8OptionVecU8_t;

/* trees' pin at Result<(Option<Vec<u8>>, Option<Vec<u8>>), Error>, for bounds. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultTupleOptionVecU8OptionVecU8
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , doltgres_scan_TupleOptionVecU8OptionVecU8_t *result, HostError *error
#define TREES_PIN_RESULT_FIELDS doltgres_scan_TupleOptionVecU8OptionVecU8_t *result; HostError *error;
#define TREES_PIN_RESULT_INIT .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/*
 * doltgres_scan_bounds_captures_t is bounds' pin scope's captures: the scan,
 * the base's tree, its count of entries, and the unit.
 */
typedef struct doltgres_scan_bounds_captures_t
{
	DoltgresScanScan *scan;
	TreesStoreTree tree;
	uint64		entries;
	DoltgresScanUnit unit;
	doltgres_scan_TupleOptionVecU8OptionVecU8_t bounds;
	HostError  *error;
	bool		ok;
} doltgres_scan_bounds_captures_t;

/*
 * doltgres_scan_bounds_closure_2_0_body reads the next unit's first key and
 * the unit's own, and places the base at the unit's first entry.
 */
static void
doltgres_scan_bounds_closure_2_0_body(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;
	doltgres_scan_bounds_captures_t *captures = frame->captures;
	doltgres_scan_TupleOptionVecU8OptionVecU8_t *result = frame->result;
	TreesStoreNamed tree = TREES_STORE_TREE_NAMED(&captures->tree);
	DoltgresScanUnit unit = captures->unit;
	ChunksErrorError failed;

	if (unit.end < captures->entries)
	{
		StdBytes	key;

		if (!trees_cursor_Cursor_at_ordinal(frame->io, frame->pin, tree, unit.end, &frame->cursor, &failed))
		{
			doltgres_scan_storage_failed(&failed, frame->error);
			return;
		}
		frame->cursor_live = true;
		key = trees_cursor_Cursor_key(&frame->cursor);
		std_slice_to_vec_Checked(&frame->tuple, key.ptr, key.len, DOLTGRES_SCAN_BYTES);
		frame->tuple_live = true;
		frame->cursor_live = false;
		trees_cursor_Cursor_drop(&frame->cursor);
	}
	if (!trees_cursor_Cursor_at_ordinal(frame->io, frame->pin, tree, unit.first, &frame->cursor, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->cursor_live = true;
	result->f0.has = unit.number > 0 && trees_cursor_Cursor_is_valid(&frame->cursor);
	if (result->f0.has)
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);

		std_slice_to_vec_Checked(&frame->bytes, key.ptr, key.len, DOLTGRES_SCAN_BYTES);
		frame->bytes_live = true;
	}
	if (unit.first < unit.end && trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		DoltgresScanPlace at = {.tag = DOLTGRES_SCAN_PLACE_AT};

		frame->cursor_live = false;
		trees_cursor_Cursor_keep(&frame->cursor, &at.u.at.cursor);
		at.u.at.way = DOLTGRES_SCAN_DIRECTION_FORWARD;
		doltgres_scan_Place_set(&captures->scan->place, at);
	}
	else
		doltgres_scan_Place_set(&captures->scan->place, doltgres_scan_off(DOLTGRES_SCAN_DIRECTION_FORWARD));
	result->f1.has = frame->tuple_live;
	result->f1.v = frame->tuple;
	result->f0.v = frame->bytes;
	frame->tuple_live = frame->bytes_live = false;
	frame->ok = true;
}

/* doltgres_scan_bounds_closure_2_0 is bounds' pin scope. */
static bool
doltgres_scan_bounds_closure_2_0(void *captures, HostIoIo *io, TreesPinPin *pin,
								 doltgres_scan_TupleOptionVecU8OptionVecU8_t *result, HostError *error)
{
	return doltgres_scan_cursor_scope(captures, io, pin, result, error, doltgres_scan_bounds_closure_2_0_body);
}

/* doltgres_scan_bounds_closure_2 is bounds' store call: the pin scope run with the store. */
static void
doltgres_scan_bounds_closure_2(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_bounds_captures_t *bounds = captures;

	bounds->ok = trees_pin_pin_ResultTupleOptionVecU8OptionVecU8(io, store, doltgres_scan_bounds_closure_2_0, bounds,
																 NULL, &bounds->bounds, bounds->error);
}

/*
 * doltgres_scan_bounds_closure_1 is bounds' key: a clone of the split key of a
 * segment between the first and the last.
 */
static doltgres_OptionVecU8_t
doltgres_scan_bounds_closure_1(const StdVec *splits, uint64 segment, uint64 segments)
{
	doltgres_OptionVecU8_t key = {segment > 0 && segment < segments};

	if (key.has)
		std_vec_clone(&key.v, &STD_VEC_AT(splits, StdVec, segment - 1), DOLTGRES_SCAN_BYTES);
	return key;
}

/* doltgres_scan_edges keeps a unit's bounds as its edges in the scan's parallel part, cloned. */
static void
doltgres_scan_edges(DoltgresScanParallel *parallel, const DoltgresScanBounds *bounds)
{
	doltgres_scan_option_drop(&parallel->low);
	doltgres_scan_option_drop(&parallel->high);
	parallel->low.has = bounds->low.has;
	if (bounds->low.has)
		std_vec_clone(&parallel->low.v, &bounds->low.v, DOLTGRES_SCAN_BYTES);
	parallel->high.has = bounds->high.has;
	if (bounds->high.has)
		std_vec_clone(&parallel->high.v, &bounds->high.v, DOLTGRES_SCAN_BYTES);
}

/*
 * doltgres_scan_bounds_frame_t is bounds' state across its pad: the scan, the
 * unit, the store's source, and the bounds being made.
 */
typedef struct doltgres_scan_bounds_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	DoltgresScanScan *scan;
	DoltgresScanUnit unit;
	DoltgresScanBounds *result;
	bool		live;
	HostError  *error;
	bool		ok;
} doltgres_scan_bounds_frame_t;

/* doltgres_scan_bounds_pad drops the bounds an unwound or failed making held. */
static void
doltgres_scan_bounds_pad(void *frame_)
{
	doltgres_scan_bounds_frame_t *frame = frame_;

	if (frame->live)
		doltgres_scan_Bounds_drop(frame->result);
	frame->live = false;
}

/*
 * doltgres_scan_bounds_body makes the unit's bounds from the split keys, or
 * from the base's entries at the unit's ends.
 */
static void
doltgres_scan_bounds_body(void *frame_)
{
	doltgres_scan_bounds_frame_t *frame = frame_;
	DoltgresScanScan *scan = frame->scan;
	DoltgresScanBounds *result = frame->result;
	doltgres_scan_bounds_captures_t captures = {.scan = scan, .tree = scan->parallel.shared, .unit = frame->unit,
	.error = frame->error};

	*result = (DoltgresScanBounds)
	{
		.alone = false
	};
	trees_KeyOrder_Clone_clone(&scan->order, &result->order);
	frame->live = true;
	if (!scan->has_parallel)
	{
		*frame->error = doltgres_scan_internal("a serial scan was read as a parallel one");
		return;
	}
	captures.entries = scan->parallel.entries;
	if (!scan->parallel.has_shared || captures.entries == 0)
	{
		const StdVec *splits = &scan->parallel.splits;
		uint64		segments = (uint64) splits->len + 1;
		uint64		units = Max(scan->parallel.units, 1);
		uint64		from = frame->unit.number * segments / units;
		uint64		to = (frame->unit.number + 1) * segments / units;

		if (from < to)
		{
			result->low = doltgres_scan_bounds_closure_1(splits, from, segments);
			result->high = doltgres_scan_bounds_closure_1(splits, Min(to, segments), segments);
		}
		else
		{
			result->low.has = result->high.has = true;
			std_vec_new(&result->low.v, DOLTGRES_SCAN_BYTES);
			std_vec_new(&result->high.v, DOLTGRES_SCAN_BYTES);
		}
		result->alone = !result->low.has && !result->high.has;
		doltgres_scan_edges(&scan->parallel, result);
		frame->live = false;
		frame->ok = true;
		return;
	}
	frame->stores->ops->with(frame->stores->self, frame->io, &scan->reader, doltgres_scan_bounds_closure_2,
							 &captures);
	if (!captures.ok)
		return;
	result->low = captures.bounds.f0;
	result->high = captures.bounds.f1;
	doltgres_scan_option_drop(&scan->bound);
	if (result->high.has)
		std_vec_clone(&scan->bound.v, &result->high.v, DOLTGRES_SCAN_BYTES);
	scan->bound.has = result->high.has;
	doltgres_scan_edges(&scan->parallel, result);
	frame->live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_bounds begins a parallel unit's merge in the scan's record:
 * its first entry's key, none for unit 0, and the next unit's first entry's
 * key, none for the last, at which its base read stops; the base's place at
 * its first entry; and what unit_keys takes. With an empty base, the units
 * divide the delta's keys by the leader's split keys.
 */
static bool
doltgres_scan_bounds(HostIoIo *io, const DoltgresScanStores *stores, DoltgresScanScan *scan, DoltgresScanUnit unit,
					 DoltgresScanBounds *result, HostError *error)
{
	doltgres_scan_bounds_frame_t frame = {.io = io, .stores = stores, .scan = scan, .unit = unit, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_bounds_body, doltgres_scan_bounds_pad);
	doltgres_scan_bounds_pad(&frame);
	return frame.ok;
}

/*
 * doltgres_scan_Kept_t is a parallel worker's kept run outside its record:
 * whether it has one, its keys (a Vec<Vec<u8>>), and the scan's generation
 * it was read at, which a take sets to the record's generation when it finds
 * none to give.
 */
typedef struct doltgres_scan_Kept_t
{
	bool		has;
	StdVec		keys;
	uint64		at;
} doltgres_scan_Kept_t;

/*
 * doltgres_scan_kept_take_closure_0 is kept_take's call: the record's kept run
 * moved out when it was read at the scan's generation, and dropped when it
 * wasn't, since a rebase or a rescan left it behind.
 */
static bool
doltgres_scan_kept_take_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_Kept_t *kept = captures;

	kept->has = false;
	kept->at = scan->generation;
	if (!scan->has_parallel || !scan->parallel.has_kept)
		return true;
	scan->parallel.has_kept = false;
	if (scan->parallel.kept_at != scan->generation)
	{
		doltgres_scan_vecs_drop(&scan->parallel.kept);
		return true;
	}
	kept->has = true;
	kept->keys = scan->parallel.kept;
	return true;
}

/* doltgres_scan_kept_put_closure_0 is kept_put's call: the kept run moved back into the record. */
static bool
doltgres_scan_kept_put_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_Kept_t *kept = captures;

	if (!scan->has_parallel)
		return true;
	scan->parallel.kept = kept->keys;
	scan->parallel.kept_at = kept->at;
	scan->parallel.has_kept = true;
	kept->has = false;
	return true;
}

/*
 * doltgres_scan_unit_keys_frame_t is unit_keys' state across its pad: the
 * kept run while it is out of the record, and the keys kept, while live.
 */
typedef struct doltgres_scan_unit_keys_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	const DoltgresScanBounds *bounds;
	doltgres_scan_Kept_t kept;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_scan_unit_keys_frame_t;

/* doltgres_scan_unit_keys_pad drops the kept run while it is out of the record, and the keys kept. */
static void
doltgres_scan_unit_keys_pad(void *frame_)
{
	doltgres_scan_unit_keys_frame_t *frame = frame_;

	if (frame->result_live)
		doltgres_scan_vecs_drop(frame->result);
	frame->result_live = false;
	if (frame->kept.has)
		doltgres_scan_vecs_drop(&frame->kept.keys);
	frame->kept.has = false;
}

/*
 * doltgres_scan_unit_keys_at returns the place in the kept run of its first
 * key at or past a bound in the table's order, by a binary search, since the
 * run is in that order.
 */
static bool
doltgres_scan_unit_keys_at(doltgres_scan_unit_keys_frame_t *frame, const StdVec *bound, size_t *result)
{
	size_t		low = 0;
	size_t		high = frame->kept.keys.len;

	while (low < high)
	{
		size_t		middle = low + (high - low) / 2;
		StdOrdering ordering;

		if (!doltgres_scan_total(frame->io, &frame->bounds->order, &STD_VEC_AT(&frame->kept.keys, StdVec, middle),
								 bound, &ordering, frame->error))
			return false;
		if (ordering == STD_ORDERING_LESS)
			low = middle + 1;
		else
			high = middle;
	}
	*result = low;
	return true;
}

/*
 * doltgres_scan_unit_keys_body takes the kept run out of the record, reading
 * the run when the record keeps none at the scan's generation, copies out its
 * keys within the unit's bounds, and puts the run back.
 */
static void
doltgres_scan_unit_keys_body(void *frame_)
{
	doltgres_scan_unit_keys_frame_t *frame = frame_;
	const DoltgresScanBounds *bounds = frame->bounds;
	size_t		first = 0;
	size_t		end;

	std_vec_new(frame->result, DOLTGRES_SCAN_VEC);
	if (!bounds->low.has && !bounds->high.has && !bounds->alone)
	{
		frame->ok = true;
		return;
	}
	frame->result_live = true;
	if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
												   doltgres_scan_kept_take_closure_0, &frame->kept, NULL, frame->error))
		return;
	if (!frame->kept.has)
	{
		if (!doltgres_scan_run(frame->io, frame->under, &bounds->order, &frame->kept.keys, frame->error))
			return;
		frame->kept.has = true;
	}
	end = frame->kept.keys.len;
	if ((bounds->low.has && !doltgres_scan_unit_keys_at(frame, &bounds->low.v, &first)) ||
		(bounds->high.has && !doltgres_scan_unit_keys_at(frame, &bounds->high.v, &end)))
		return;
	for (size_t i = first; i < end; i++)
	{
		const StdVec *key = &STD_VEC_AT(&frame->kept.keys, StdVec, i);
		StdVec		copy;

		std_slice_to_vec_Checked(&copy, key->ptr, key->len, DOLTGRES_SCAN_BYTES);
		std_vec_push_Checked(frame->result, &copy, DOLTGRES_SCAN_VEC);
	}
	if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
												   doltgres_scan_kept_put_closure_0, &frame->kept, NULL, frame->error))
		return;
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_unit_keys answers a parallel unit's delta keys in the
 * table's order: every key at or past its low bound and before its high
 * one, with ties broken by the keys' bytes, or every key when its bounds say
 * the unit reads them all alone. It takes them from the run the worker keeps,
 * read once for each generation the scan reads at, so a scan's units read the
 * delta's run once and not once each (bugs row 309).
 */
static bool
doltgres_scan_unit_keys(HostIoIo *io, const DoltgresScanUnder *under, HostOwnedHandle handle,
						const DoltgresScanBounds *bounds, StdVec *result, HostError *error)
{
	doltgres_scan_unit_keys_frame_t frame = {.io = io, .under = under, .handle = handle, .bounds = bounds,
	.result = result, .error = error};

	std_landing(&frame, doltgres_scan_unit_keys_body, doltgres_scan_unit_keys_pad);
	doltgres_scan_unit_keys_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_OptionBounds_t is an Option<Bounds>. */
typedef struct doltgres_scan_OptionBounds_t
{
	bool		has;
	DoltgresScanBounds v;
} doltgres_scan_OptionBounds_t;

/* DOLTGRES_SCAN_OPTION_BOUNDS_DROP drops an Option<Bounds> a call left. */
#define DOLTGRES_SCAN_OPTION_BOUNDS_DROP(place) \
	do { \
		if ((place)->has) \
			doltgres_scan_Bounds_drop(&(place)->v); \
	} while (0)

/* Scan's Table's with at Option<Bounds>, for next_parallel. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS OptionBounds
#define HOST_OWNED_WITH_T doltgres_scan_OptionBounds_t
#define HOST_OWNED_WITH_T_DROP(place) DOLTGRES_SCAN_OPTION_BOUNDS_DROP(place)
#include "doltgres/host/owned_template.h"

/*
 * doltgres_scan_next_parallel_closure_0 is next_parallel's look in the record:
 * a begun unit's bounds, when it has no run yet.
 */
static bool
doltgres_scan_next_parallel_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									  doltgres_scan_OptionBounds_t *result, HostError *error)
{
	result->has = scan->has_parallel && scan->parallel.has_unit && !scan->has_run;
	if (!result->has)
		return true;
	return doltgres_scan_bounds(io, *(const DoltgresScanStores **) captures, scan, scan->parallel.unit, &result->v,
								error);
}

/* doltgres_scan_run_put puts a run of keys in the record at split 0, dropping the run it held. */
static void
doltgres_scan_run_put(DoltgresScanScan *scan, StdVec *keys)
{
	if (scan->has_run)
		doltgres_scan_vecs_drop(&scan->run.keys);
	scan->run.keys = *keys;
	scan->run.split = 0;
	scan->has_run = true;
}

/* doltgres_scan_next_parallel_closure_1 is next_parallel's put of the unit's keys in the record. */
static bool
doltgres_scan_next_parallel_closure_1(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_run_put(scan, captures);
	return true;
}

/*
 * doltgres_scan_next_parallel_closure_2 is next_parallel's turn in the record:
 * the end with no unit, the group's next row, or a gather forward.
 */
static bool
doltgres_scan_next_parallel_closure_2(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									  DoltgresScanTurn *result, HostError *error)
{
	doltgres_scan_turn_t *turn = captures;

	if (!scan->has_parallel || !scan->parallel.has_unit)
	{
		result->tag = DOLTGRES_SCAN_TURN_END;
		return true;
	}
	if (doltgres_scan_given(scan, DOLTGRES_SCAN_DIRECTION_FORWARD, &result->u.row))
	{
		result->tag = DOLTGRES_SCAN_TURN_ROW;
		return true;
	}
	return doltgres_scan_gather(io, turn->stores, turn->under, scan, DOLTGRES_SCAN_DIRECTION_FORWARD, result,
								error);
}

/* doltgres_scan_next_parallel_closure_3 is next_parallel's end of the unit in the record. */
static bool
doltgres_scan_next_parallel_closure_3(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	if (scan->has_parallel)
		scan->parallel.has_unit = false;
	return true;
}

/* doltgres_scan_next_parallel_closure_4 is next_parallel's put of the delta's run in the record. */
static bool
doltgres_scan_next_parallel_closure_4(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_run_put(scan, captures);
	return true;
}

/* doltgres_scan_next_parallel_closure_5 is next_parallel's making of the chosen group in the record, forward. */
static bool
doltgres_scan_next_parallel_closure_5(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_chose_t *chose = captures;

	return doltgres_scan_chosen(io, chose->under, scan, &chose->picked, DOLTGRES_SCAN_DIRECTION_FORWARD,
								&chose->moved, error);
}

/* doltgres_scan_sought_captures_t is sought's pin scope's captures: the tree, the key, and its order. */
typedef struct doltgres_scan_sought_captures_t
{
	TreesStoreTree tree;
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} doltgres_scan_sought_captures_t;

/* doltgres_scan_sought_closure_0_body seeks the key and copies out the key of the first entry at or past it. */
static void
doltgres_scan_sought_closure_0_body(void *frame_)
{
	doltgres_scan_cursor_frame_t *frame = frame_;
	doltgres_scan_sought_captures_t *captures = frame->captures;
	doltgres_OptionVecU8_t *result = frame->result;
	ChunksErrorError failed;
	StdBytes	key;

	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&captures->tree), captures->key,
								  captures->key_len, captures->order, &frame->cursor, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->cursor_live = true;
	result->has = trees_cursor_Cursor_is_valid(&frame->cursor);
	if (result->has)
	{
		key = trees_cursor_Cursor_key(&frame->cursor);
		std_slice_to_vec_Checked(&result->v, key.ptr, key.len, DOLTGRES_SCAN_BYTES);
	}
	frame->ok = true;
}

/* doltgres_scan_sought_closure_0 is sought's pin scope. */
static bool
doltgres_scan_sought_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, doltgres_OptionVecU8_t *result,
							   HostError *error)
{
	return doltgres_scan_cursor_scope(captures, io, pin, result, error, doltgres_scan_sought_closure_0_body);
}

/* doltgres_scan_sought_read_t is a stop's read under a reader, a Stores::with call's captures. */
typedef struct doltgres_scan_sought_read_t
{
	doltgres_scan_sought_captures_t sought;
	doltgres_OptionVecU8_t *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_sought_read_t;

/* doltgres_scan_sought_read reads the key of a tree's first entry at or past a key going forward, in a pin scope. */
static void
doltgres_scan_sought_read(void *captures, HostIoIo *io, TreesStoreNodeStore store)
{
	doltgres_scan_sought_read_t *read = captures;

	read->ok = trees_pin_pin_ResultOptionVecU8(io, store, doltgres_scan_sought_closure_0, &read->sought, NULL,
											   read->result, read->error);
}

/*
 * DoltgresScanUnitPlacing is what unit_placed takes out of a parallel scan's
 * record: whether the worker's tree is a newer base's than the shared one,
 * that tree, its group's key, its unit's edges, and the table's order, each
 * cloned and each while live, and the place and stop it puts back.
 */
typedef struct DoltgresScanUnitPlacing
{
	bool		moved;
	bool		has_tree;
	TreesStoreTree tree;
	doltgres_OptionVecU8_t group;
	doltgres_OptionVecU8_t low;
	doltgres_OptionVecU8_t high;
	TreesKeyOrder order;
	bool		order_live;
	DoltgresScanPlace place;
	doltgres_OptionVecU8_t bound;
} DoltgresScanUnitPlacing;

/* doltgres_scan_unit_placed_closure_0 is unit_placed's look in the record: a moved worker's tree, group, and edges. */
static bool
doltgres_scan_unit_placed_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	DoltgresScanUnitPlacing *placing = captures;
	const DoltgresScanParallel *parallel = &scan->parallel;

	if (!scan->has_parallel || !parallel->has_unit ||
		(scan->has_tree == parallel->has_shared &&
		 (!scan->has_tree || trees_store_Tree_PartialEq_eq(&scan->tree, &parallel->shared))))
		return true;
	placing->moved = true;
	placing->has_tree = scan->has_tree;
	placing->tree = scan->tree;
	if (scan->has_group)
		std_vec_clone(&placing->group.v, &scan->group.key, DOLTGRES_SCAN_BYTES);
	placing->group.has = scan->has_group;
	if (parallel->low.has)
		std_vec_clone(&placing->low.v, &parallel->low.v, DOLTGRES_SCAN_BYTES);
	placing->low.has = parallel->low.has;
	if (parallel->high.has)
		std_vec_clone(&placing->high.v, &parallel->high.v, DOLTGRES_SCAN_BYTES);
	placing->high.has = parallel->high.has;
	trees_KeyOrder_Clone_clone(&scan->order, &placing->order);
	placing->order_live = true;
	return true;
}

/* doltgres_scan_unit_placed_closure_1 is unit_placed's put of the place and the stop in the record. */
static bool
doltgres_scan_unit_placed_closure_1(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	DoltgresScanUnitPlacing *placing = captures;

	doltgres_scan_Place_set(&scan->place, placing->place);
	doltgres_scan_option_drop(&scan->bound);
	scan->bound = placing->bound;
	placing->place.tag = DOLTGRES_SCAN_PLACE_START;
	placing->bound.has = false;
	return true;
}

/* doltgres_scan_unit_placed_frame_t is unit_placed's state across its pad: its reader and what it took, while live. */
typedef struct doltgres_scan_unit_placed_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	HostOwnedHandle handle;
	HostSharedReader reading;
	bool		reading_live;
	DoltgresScanUnitPlacing placing;
	HostError  *error;
	bool		ok;
} doltgres_scan_unit_placed_frame_t;

/* doltgres_scan_unit_placed_pad drops what unit_placed holds. */
static void
doltgres_scan_unit_placed_pad(void *frame_)
{
	doltgres_scan_unit_placed_frame_t *frame = frame_;
	DoltgresScanUnitPlacing *placing = &frame->placing;

	doltgres_scan_option_drop(&placing->bound);
	doltgres_scan_Place_drop(&placing->place);
	placing->place.tag = DOLTGRES_SCAN_PLACE_START;
	if (placing->order_live)
		trees_KeyOrder_drop(&placing->order);
	doltgres_scan_option_drop(&placing->high);
	doltgres_scan_option_drop(&placing->low);
	doltgres_scan_option_drop(&placing->group);
	placing->order_live = placing->moved = false;
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	frame->reading_live = false;
}

/* doltgres_scan_unit_placed_body finds a moved worker's place and stop in its unit in the newer base's tree. */
static void
doltgres_scan_unit_placed_body(void *frame_)
{
	doltgres_scan_unit_placed_frame_t *frame = frame_;
	DoltgresScanUnitPlacing *placing = &frame->placing;
	const doltgres_OptionVecU8_t *from;

	placing->place.tag = DOLTGRES_SCAN_PLACE_START;
	if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
												   doltgres_scan_unit_placed_closure_0, placing, NULL, frame->error))
		return;
	if (!placing->moved)
	{
		frame->ok = true;
		return;
	}
	from = placing->group.has ? &placing->group : &placing->low;
	if (!placing->has_tree)
		placing->place = doltgres_scan_off(DOLTGRES_SCAN_DIRECTION_FORWARD);
	else
	{
		if (!host_shared_read(frame->io, &frame->reading, frame->error))
			return;
		frame->reading_live = true;
		if (from->has)
		{
			doltgres_scan_place_read_t read = {{placing->tree, from->v.ptr, from->v.len,
			DOLTGRES_SCAN_DIRECTION_FORWARD, &placing->order, placing->group.has}, .error = frame->error};

			frame->stores->ops->with(frame->stores->self, frame->io, &frame->reading, doltgres_scan_place_read,
									 &read);
			if (!read.ok)
				return;
			placing->place = read.place;
		}
		if (placing->high.has)
		{
			doltgres_scan_sought_read_t read = {{placing->tree, placing->high.v.ptr, placing->high.v.len,
			&placing->order}, &placing->bound, frame->error};

			frame->stores->ops->with(frame->stores->self, frame->io, &frame->reading, doltgres_scan_sought_read,
									 &read);
			if (!read.ok)
				return;
		}
	}
	frame->ok = host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														  doltgres_scan_unit_placed_closure_1, placing, NULL,
														  frame->error);
}

/*
 * doltgres_scan_unit_placed puts a parallel scan's worker that moved to a
 * newer base than the shared one (rebased) at its place in its unit in the
 * newer base's tree, outside the record, since a compared tree's order runs
 * user code: past its group's key once it has one, as a serial read goes on,
 * or else at the unit's low key, or the tree's start for unit 0; and stops it
 * at the first entry at or past the unit's high key, which the newer tree
 * need not hold, so each key is read by the unit whose edges hold it. A
 * worker on the shared base, or with no unit, is left as it is.
 */
static bool
doltgres_scan_unit_placed(HostIoIo *io, const DoltgresScanStores *stores, HostOwnedHandle handle, HostError *error)
{
	doltgres_scan_unit_placed_frame_t frame = {.io = io, .stores = stores, .handle = handle, .error = error};

	std_landing(&frame, doltgres_scan_unit_placed_body, doltgres_scan_unit_placed_pad);
	doltgres_scan_unit_placed_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_rebasing_frame_t is rebasing's state across its pad: its reader and the move, while live. */
typedef struct doltgres_scan_rebasing_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	HostSharedReader reading;
	bool		reading_live;
	DoltgresScanRebased rebasing;
	bool	   *moved;
	HostError  *error;
	bool		ok;
} doltgres_scan_rebasing_frame_t;

/* doltgres_scan_rebasing_pad drops what rebasing holds. */
static void
doltgres_scan_rebasing_pad(void *frame_)
{
	doltgres_scan_rebasing_frame_t *frame = frame_;

	doltgres_scan_Rebased_drop(&frame->rebasing);
	if (frame->reading_live)
		host_shared_Reader_drop(&frame->reading);
	frame->reading_live = false;
}

/* doltgres_scan_rebasing_body moves the worker to the newest base under its reader. */
static void
doltgres_scan_rebasing_body(void *frame_)
{
	doltgres_scan_rebasing_frame_t *frame = frame_;
	doltgres_scan_rebase_read_t rebase = {frame->under, frame->handle, &frame->rebasing, frame->error};

	if (!host_shared_read(frame->io, &frame->reading, frame->error))
		return;
	frame->reading_live = true;
	frame->stores->ops->with(frame->stores->self, frame->io, &frame->reading, doltgres_scan_rebase_read, &rebase);
	*frame->moved = rebase.ok && frame->rebasing.moved;
	frame->ok = rebase.ok;
}

/*
 * doltgres_scan_rebasing moves a parallel scan's worker to its storage's
 * newest base under a reader of its own (rebased) and answers whether it
 * moved.
 */
static bool
doltgres_scan_rebasing(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
					   HostOwnedHandle handle, bool *moved, HostError *error)
{
	doltgres_scan_rebasing_frame_t frame = {.io = io, .stores = stores, .under = under, .handle = handle,
	.moved = moved, .error = error};

	std_landing(&frame, doltgres_scan_rebasing_body, doltgres_scan_rebasing_pad);
	doltgres_scan_rebasing_pad(&frame);
	return frame.ok;
}


/*
 * doltgres_scan_next_parallel_frame_t is next_parallel's state across its pad:
 * a unit's bounds, a turn's order, and a run's keys, while live.
 */
typedef struct doltgres_scan_next_parallel_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	doltgres_scan_OptionBounds_t begun;
	TreesKeyOrder order;
	bool		order_live;
	StdVec		keys;
	bool		keys_live;
	DoltgresScanStep *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_next_parallel_frame_t;

/* doltgres_scan_next_parallel_pad drops what a turn holds. */
static void
doltgres_scan_next_parallel_pad(void *frame_)
{
	doltgres_scan_next_parallel_frame_t *frame = frame_;

	if (frame->keys_live)
		doltgres_scan_vecs_drop(&frame->keys);
	frame->keys_live = false;
	if (frame->order_live)
		trees_KeyOrder_drop(&frame->order);
	frame->order_live = false;
	DOLTGRES_SCAN_OPTION_BOUNDS_DROP(&frame->begun);
	frame->begun.has = false;
}

/* doltgres_scan_next_parallel_body turns the unit's merge until it gives a row or the unit ends. */
static void
doltgres_scan_next_parallel_body(void *frame_)
{
	doltgres_scan_next_parallel_frame_t *frame = frame_;
	doltgres_scan_turn_t turn = {frame->stores, frame->under, DOLTGRES_SCAN_DIRECTION_FORWARD};
	bool		moved = false;

	for (;;)
	{
		DoltgresScanTurn step;
		uint64		generation;

		if (!host_owned_TableDoltgresScanScan_withOptionBounds(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
															   doltgres_scan_next_parallel_closure_0,
															   (void *) &frame->stores, NULL, &frame->begun,
															   frame->error))
			return;
		if (moved || frame->begun.has)
		{
			if (!doltgres_scan_unit_placed(frame->io, frame->stores, frame->handle, frame->error))
				return;
			moved = false;
		}
		if (frame->begun.has)
		{
			if (!doltgres_scan_unit_keys(frame->io, frame->under, frame->handle, &frame->begun.v, &frame->keys,
										 frame->error))
				return;
			frame->keys_live = true;
			if (!host_owned_TableDoltgresScanScan_withU64(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														  doltgres_scan_generation_closure_0, NULL, NULL, &generation,
														  frame->error))
				return;
			if (txn_state_moved_past(frame->under->state, generation))
			{
				doltgres_scan_next_parallel_pad(frame);
				if (!doltgres_scan_rebasing(frame->io, frame->stores, frame->under, frame->handle, &moved,
											frame->error))
					return;
				continue;
			}
			frame->keys_live = false;
			if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														   doltgres_scan_next_parallel_closure_1, &frame->keys,
														   doltgres_scan_keys_drop, frame->error))
				return;
			doltgres_scan_next_parallel_pad(frame);
		}
		if (!host_owned_TableDoltgresScanScan_withTurn(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
													   doltgres_scan_next_parallel_closure_2, &turn, NULL, &step,
													   frame->error))
			return;
		if (step.tag == DOLTGRES_SCAN_TURN_ROW)
		{
			frame->result->tag = DOLTGRES_SCAN_STEP_ROW;
			frame->result->row = step.u.row;
			frame->ok = true;
			return;
		}
		if (step.tag == DOLTGRES_SCAN_TURN_END)
		{
			if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														   doltgres_scan_next_parallel_closure_3, NULL, NULL,
														   frame->error))
				return;
			frame->result->tag = DOLTGRES_SCAN_STEP_UNIT;
			frame->ok = true;
			return;
		}
		if (step.tag == DOLTGRES_SCAN_TURN_MOVED)
		{
			if (!doltgres_scan_rebasing(frame->io, frame->stores, frame->under, frame->handle, &moved, frame->error))
				return;
		}
		else if (step.tag == DOLTGRES_SCAN_TURN_RUN)
		{
			frame->order = step.u.run;
			frame->order_live = true;
			if (!doltgres_scan_run(frame->io, frame->under, &frame->order, &frame->keys, frame->error))
				return;
			frame->keys_live = true;
			if (!host_owned_TableDoltgresScanScan_withU64(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														  doltgres_scan_generation_closure_0, NULL, NULL, &generation,
														  frame->error))
				return;
			if (txn_state_moved_past(frame->under->state, generation))
			{
				doltgres_scan_next_parallel_pad(frame);
				if (!doltgres_scan_rebasing(frame->io, frame->stores, frame->under, frame->handle, &moved,
											frame->error))
					return;
				continue;
			}
			frame->keys_live = false;
			if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														   doltgres_scan_next_parallel_closure_4, &frame->keys,
														   doltgres_scan_keys_drop, frame->error))
				return;
			doltgres_scan_next_parallel_pad(frame);
		}
		else if (step.tag == DOLTGRES_SCAN_TURN_COMPARE)
		{
			doltgres_scan_chose_t chose = {.under = frame->under, .way = DOLTGRES_SCAN_DIRECTION_FORWARD};

			frame->order = step.u.compare.order;
			frame->order_live = true;
			if (!doltgres_scan_pick(frame->io, &step.u.compare.base_key, &step.u.compare.base_copies,
									&step.u.compare.delta_key, step.u.compare.delta_at, &frame->order,
									DOLTGRES_SCAN_DIRECTION_FORWARD, &chose.picked, frame->error))
				return;
			if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
														   doltgres_scan_next_parallel_closure_5, &chose,
														   doltgres_scan_chose_drop, frame->error))
				return;
			doltgres_scan_next_parallel_pad(frame);
			if (chose.moved &&
				!doltgres_scan_rebasing(frame->io, frame->stores, frame->under, frame->handle, &moved, frame->error))
				return;
		}
	}
}

/*
 * doltgres_scan_next_parallel reads a parallel sequential scan's next row
 * under its snapshot, forward, and gives it, or the need of the next unit
 * once the unit's rows are read. A unit's rows are the merge of its base
 * entries with the delta's keys at or past its first entry's key and before
 * the next unit's, so each delta key is read by one unit.
 */
bool
doltgres_scan_next_parallel(HostIoIo *io, const DoltgresScanStores *stores, const DoltgresScanUnder *under,
							HostOwnedHandle handle, DoltgresScanStep *result, HostError *error)
{
	doltgres_scan_next_parallel_frame_t frame = {.io = io, .stores = stores, .under = under, .handle = handle,
	.result = result, .error = error};

	std_landing(&frame, doltgres_scan_next_parallel_body, doltgres_scan_next_parallel_pad);
	doltgres_scan_next_parallel_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_TupleU32U32_t is a (u32, u32): a range's first block and its count of blocks. */
typedef struct doltgres_scan_TupleU32U32_t
{
	uint32		f0;
	uint32		f1;
} doltgres_scan_TupleU32U32_t;

/* Scan's Table's with at (u32, u32), for range_blocks. */
#define HOST_OWNED_SCOPE static
#define HOST_OWNED_DECLARE
#define HOST_OWNED_DEFINE
#define HOST_OWNED_PREFIX doltgres_scan_Scan
#define HOST_OWNED_RS DoltgresScanScan
#define HOST_OWNED_R DoltgresScanScan
#define HOST_OWNED_R_DROP doltgres_scan_Scan_drop
#define HOST_OWNED_KIND 0
#define HOST_OWNED_TABLE_FN doltgres_scan_Scan_Owned_table
#define HOST_OWNED_WITH_TS TupleU32U32
#define HOST_OWNED_WITH_T doltgres_scan_TupleU32U32_t
#include "doltgres/host/owned_template.h"

/* doltgres_scan_range_blocks_closure_0 is range_blocks' call: the range's ends within the storage's blocks. */
static bool
doltgres_scan_range_blocks_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan,
									 doltgres_scan_TupleU32U32_t *result, HostError *error)
{
	uint64		blocks = *(const uint64 *) captures;
	uint64		low;
	uint64		high;

	result->f0 = result->f1 = 0;
	if (!scan->has_range)
		return true;
	low = Max(scan->range_first, 1);
	high = Min(scan->range_last, std_saturating_sub_u64(blocks, 1) << 16 | 0xffff);
	if (blocks > 0 && low <= high)
	{
		result->f0 = (uint32) (low >> 16);
		result->f1 = (uint32) ((high >> 16) - (low >> 16) + 1);
	}
	return true;
}

/*
 * doltgres_scan_range_blocks answers the blocks a parallel tid range scan's
 * units are drawn from, the first and how many, as heap's heap_set_tidrange
 * limits them: its range's ends within the storage's blocks, and none for an
 * empty range.
 */
bool
doltgres_scan_range_blocks(HostIoIo *io, HostOwnedHandle handle, uint64 blocks, uint32 *result_0, uint32 *result_1,
						   HostError *error)
{
	doltgres_scan_TupleU32U32_t range;

	if (!host_owned_TableDoltgresScanScan_withTupleU32U32(&DOLTGRES_SCAN_SCANS, io, handle,
														  doltgres_scan_range_blocks_closure_0, &blocks, NULL, &range,
														  error))
		return false;
	*result_0 = range.f0;
	*result_1 = range.f1;
	return true;
}

/*
 * doltgres_scan_start_range_unit_closure_0 is start_range_unit's call: the
 * scan's range within the block, the read started over.
 */
static bool
doltgres_scan_start_range_unit_closure_0(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	uint64		block = *(const uint64 *) captures;
	uint64		first = scan->has_range ? scan->range_first : 1;
	uint64		last = scan->has_range ? scan->range_last : 0;

	if (!scan->has_parallel)
	{
		*error = doltgres_scan_internal("a serial scan was given a unit");
		return false;
	}
	scan->parallel.has_ranged = true;
	scan->parallel.ranged_first = Max(first, block << 16);
	scan->parallel.ranged_last = Min(last, block << 16 | 0xffff);
	scan->has_given = false;
	return true;
}

/*
 * doltgres_scan_start_range_unit starts a parallel tid range scan's unit, a
 * ctid block: the scan's range within the block, read as next_in_range reads
 * a range, which gives no row past it.
 */
bool
doltgres_scan_start_range_unit(HostIoIo *io, HostOwnedHandle handle, uint64 block, HostError *error)
{
	return host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, io, handle,
													 doltgres_scan_start_range_unit_closure_0, &block, NULL, error);
}

#ifdef DOLTGRES_TEST_DOLTGRES

/* DOLTGRES_SCAN_TESTS_DATABASE is DATABASE, the database of the cases' storage. */
#define DOLTGRES_SCAN_TESTS_DATABASE 9400

/* DOLTGRES_SCAN_TESTS_UNWRAP is Result::unwrap's panic text. */
#define DOLTGRES_SCAN_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* doltgres_scan_tests_MemoryStore_Stores_with is MemoryStore's Stores: the call over the store itself. */
static void
doltgres_scan_tests_MemoryStore_Stores_with(const void *self, HostIoIo *io, const HostSharedReader *reader,
											void (*call) (void *captures, HostIoIo *io, TreesStoreNodeStore store),
											void *captures)
{
	call(captures, io, TREES_STORE_MEMORY_STORE_NODE_STORE((TreesStoreMemoryStore *) self));
}

/* DOLTGRES_SCAN_TESTS_MEMORY_STORE_STORES_OPS is MemoryStore's Stores, the cases' store. */
static const DoltgresScanStoresOps DOLTGRES_SCAN_TESTS_MEMORY_STORE_STORES_OPS = {
doltgres_scan_tests_MemoryStore_Stores_with};

/* doltgres_scan_tests_stores lends a MemoryStore as the Stores a read takes. */
static DoltgresScanStores
doltgres_scan_tests_stores(const TreesStoreMemoryStore *store)
{
	DoltgresScanStores stores = {&DOLTGRES_SCAN_TESTS_MEMORY_STORE_STORES_OPS, store};

	return stores;
}

/* doltgres_scan_tests_storage returns the cases' storage. */
static HostStorage
doltgres_scan_tests_storage(void)
{
	HostStorage storage = {HOST_PG_DEFAULT, DOLTGRES_SCAN_TESTS_DATABASE, 16384};

	return storage;
}

/* doltgres_scan_tests_trees returns a base's root, its tuple id tree, and its fold point, as a fetch reads them. */
static DoltgresScanTrees
doltgres_scan_tests_trees(const TxnBaseBase *base)
{
	return doltgres_scan_Trees_of(base, 0);
}

/*
 * doltgres_scan_tests_layout returns a fetch's layout of a table of a keyed
 * layout and an object id, in bytes' order.
 */
static DoltgresScanLayout
doltgres_scan_tests_layout(const uint8 *keyed, size_t keyed_len, uint32 table)
{
	DoltgresScanLayout layout = {.table = table, .order = {.tag = TREES_KEY_ORDER_BYTES}};

	std_slice_to_vec_Checked(&layout.keyed, keyed, keyed_len, DOLTGRES_SCAN_BYTES);
	return layout;
}

/* doltgres_scan_tests_made returns the cases' storage's state, made new with nothing in its delta. */
static HostSharedHeld
doltgres_scan_tests_made(HostIoIo *io)
{
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostStorage storage = doltgres_scan_tests_storage();
	ChunksErrorError created;
	HostError	failed;

	if (!repo_Repo_create(io, DOLTGRES_SCAN_TESTS_DATABASE, HOST_PG_DEFAULT, &repo, &created))
		STD_RESULT_EXPECT_FAILED(1791, 91, DOLTGRES_SCAN_TESTS_UNWRAP, &created, chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, &reading, &failed))
		STD_RESULT_EXPECT_FAILED(1792, 46, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	if (!txn_state_state(io, &repo, &reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &failed))
		STD_RESULT_EXPECT_FAILED(1793, 85, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	return held;
}

/*
 * doltgres_scan_tests_based returns a base record of a table's tree and a
 * tuple id tree whose fresh end is a tuple id.
 */
static TxnBaseBase
doltgres_scan_tests_based(TreesStoreTree root, const TreesStoreTree *tuple_ids, uint64 fresh_end)
{
	TxnBaseBase base = {.has_root = true, .root = root, .fresh_end = fresh_end};

	base.has_tuple_ids = tuple_ids != NULL;
	if (tuple_ids != NULL)
		base.tuple_ids = *tuple_ids;
	base.key.tag = TXN_BASE_KEY_NONE;
	return base;
}

/* doltgres_scan_tests_finished finishes a case's builder and names its root as a base's record would decode it. */
static TreesStoreTree
doltgres_scan_tests_finished(TreesBuildBuilder *builder, HostIoIo *io, uint32 line, uint32 col)
{
	TreesStoreFresh fresh;
	TreesStoreTreeRef root;
	ChunksErrorError failed;

	if (!trees_build_Builder_finish(builder, io, &TREES_STORE_MEMORY_STORE_BRANDED_OPS, &fresh, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
	root = trees_store_Fresh_as_ref(&fresh);
	return trees_store_Tree_decoded(root.address, root.locator);
}

/* doltgres_scan_tests_add adds a case's entry to its builder, which unwraps at a line and column. */
static void
doltgres_scan_tests_add(TreesBuildBuilder *builder, HostIoIo *io, TreesBuildEntry *entry, uint32 line, uint32 col)
{
	ChunksErrorError failed;

	if (!trees_build_Builder_add(builder, io, entry, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, chunks_error_Error_Debug_fmt);
}

/*
 * doltgres_scan_tests_pairs builds a tuple id tree of pairs of a tuple id and
 * a row's key, each row of 32 heap bytes, in tuple id order, and returns its
 * root's name; it takes the keys.
 */
static TreesStoreTree
doltgres_scan_tests_pairs(HostIoIo *io, TreesStoreMemoryStore *store, const uint64 *tuple_ids, StdVec *keys,
						  size_t len)
{
	TreesBuildBuilder builder;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(store), 256, &builder);
	for (size_t i = 0; i < len; i++)
	{
		uint8		key[6];
		StdVec		bytes;
		StdVec		value;
		TreesBuildEntry entry;

		txn_base_tuple_id_key(tuple_ids[i], key);
		std_slice_to_vec_Checked(&bytes, key, sizeof(key), DOLTGRES_SCAN_BYTES);
		txn_base_pair_value(keys[i].ptr, keys[i].len, 32, &value);
		std_vec_free_Checked(&keys[i], DOLTGRES_SCAN_BYTES);
		trees_build_Entry_new_VecU8VecU8(&bytes, &value, &entry);
		doltgres_scan_tests_add(&builder, io, &entry, 1823, 107);
	}
	return doltgres_scan_tests_finished(&builder, io, 1825, 53);
}

/* doltgres_scan_tests_be4 returns a u32's four bytes, high byte first, as a Vec<u8>. */
static StdVec
doltgres_scan_tests_be4(uint32 n)
{
	uint8		bytes[4] = {(uint8) (n >> 24), (uint8) (n >> 16), (uint8) (n >> 8), (uint8) n};
	StdVec		out;

	std_slice_to_vec_Checked(&out, bytes, 4, DOLTGRES_SCAN_BYTES);
	return out;
}

/* doltgres_scan_tests_le4 returns a u32's four bytes, low byte first, as a Vec<u8>. */
static StdVec
doltgres_scan_tests_le4(uint32 n)
{
	uint8		bytes[4] = {(uint8) n, (uint8) (n >> 8), (uint8) (n >> 16), (uint8) (n >> 24)};
	StdVec		out;

	std_slice_to_vec_Checked(&out, bytes, 4, DOLTGRES_SCAN_BYTES);
	return out;
}

/*
 * doltgres_scan_tests_built builds a tree of the rows numbered from one
 * number up to another in a store, each keyed by its number big-endian with
 * its number's square as its value and stamped by its number plus one as
 * its tuple id, and returns its root's name as a base's record would decode
 * it; its adds and its finish unwrap at a line and column each.
 */
static TreesStoreTree
doltgres_scan_tests_built(HostIoIo *io, TreesStoreMemoryStore *store, uint32 from, uint32 to, uint32 add_line,
						  uint32 add_col, uint32 finished_line, uint32 finished_col)
{
	TreesBuildBuilder builder;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(store), 256, &builder);
	for (uint32 n = from; n < to; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(n);
		StdVec		value = doltgres_scan_tests_le4(n * n);
		ChunksTrailerStamp stamp = {3, (uint64) n + 1};
		TreesBuildEntry entry;

		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		std_vec_push_Checked(&entry.stamps, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		doltgres_scan_tests_add(&builder, io, &entry, add_line, add_col);
	}
	return doltgres_scan_tests_finished(&builder, io, finished_line, finished_col);
}

/*
 * doltgres_scan_tests_base builds a tree of rows rows in a store, as built
 * builds the rows from 0, and returns its root's name.
 */
static TreesStoreTree
doltgres_scan_tests_base(HostIoIo *io, TreesStoreMemoryStore *store, uint32 rows)
{
	return doltgres_scan_tests_built(io, store, 0, rows, 1837, 105, 1839, 53);
}

/* doltgres_scan_tests_next reads a scan's next row, which unwraps at a line and column. */
static bool
doltgres_scan_tests_next(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						 HostOwnedHandle handle, DoltgresScanDirection way, DoltgresScanRow *row, uint32 line,
						 uint32 col)
{
	DoltgresScanStores stores = doltgres_scan_tests_stores(store);
	HostError	failed;
	bool		has;

	if (!doltgres_scan_next(io, &stores, under, handle, way, &has, row, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return has;
}

/*
 * doltgres_scan_tests_read reads every row a scan has left in a direction,
 * one call a row, and returns each one, a Vec<Row>, whose key and tuple the
 * cases compare.
 */
static StdVec
doltgres_scan_tests_read(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						 HostOwnedHandle handle, DoltgresScanDirection way)
{
	StdVec		rows;
	DoltgresScanRow row;

	std_vec_new(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	while (doltgres_scan_tests_next(io, store, under, handle, way, &row, 1852, 67))
		std_vec_push_Checked(&rows, &row, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	return rows;
}

/* doltgres_scan_tests_Row_drop is Row's drop glue, an element of read's answer. */
static void
doltgres_scan_tests_Row_drop(void *row)
{
	doltgres_scan_Row_drop(row);
}

/* doltgres_scan_tests_rows_drop drops a read's rows. */
static void
doltgres_scan_tests_rows_drop(StdVec *rows)
{
	std_vec_drop_Checked(rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow), doltgres_scan_tests_Row_drop);
}

/* doltgres_scan_tests_bytes_is reports whether a Vec<u8> holds a byte string. */
static bool
doltgres_scan_tests_bytes_is(const StdVec *bytes, const uint8 *expected, size_t len)
{
	return bytes->len == len && (len == 0 || memcmp(bytes->ptr, expected, len) == 0);
}

/*
 * doltgres_scan_tests_is_expected reports whether a row is base's row n: its
 * key n big-endian and its tuple n's square.
 */
static bool
doltgres_scan_tests_is_expected(const DoltgresScanRow *row, uint32 n)
{
	uint8		key[4] = {(uint8) (n >> 24), (uint8) (n >> 16), (uint8) (n >> 8), (uint8) n};
	uint32		square = n * n;
	uint8		tuple[4] = {(uint8) square, (uint8) (square >> 8), (uint8) (square >> 16), (uint8) (square >> 24)};

	return doltgres_scan_tests_bytes_is(&row->key, key, 4) && doltgres_scan_tests_bytes_is(&row->tuple, tuple, 4);
}

/*
 * doltgres_scan_tests_expected reports whether rows are expected(n)'s rows,
 * base's rows from first, ascending or descending, as assert_eq! compares
 * them with expected's.
 */
static bool
doltgres_scan_tests_expected(const StdVec *rows, uint32 first, uint32 count, bool reversed)
{
	if (rows->len != count)
		return false;
	for (uint32 i = 0; i < count; i++)
	{
		uint32		n = reversed ? first + count - 1 - i : first + i;

		if (!doltgres_scan_tests_is_expected(&STD_VEC_AT(rows, DoltgresScanRow, i), n))
			return false;
	}
	return true;
}

/* doltgres_scan_tests_begin begins a scan over a base in bytes' order, which unwraps at a line and column. */
static HostOwnedHandle
doltgres_scan_tests_begin(HostIoIo *io, TreesStoreMemoryStore *store, uint64 blocks, const TxnBaseBase *base,
						  const uint8 *keyed, size_t keyed_len, uint32 table, uint32 line, uint32 col)
{
	StdVec		layout;
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	HostOwnedHandle handle;
	HostError	failed;

	std_slice_to_vec_Checked(&layout, keyed, keyed_len, DOLTGRES_SCAN_BYTES);
	if (!doltgres_scan_scan_begin(io, TREES_STORE_MEMORY_STORE_NODE_STORE(store), blocks, base, 0, &layout, table,
								  &order, &handle, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return handle;
}

/* doltgres_scan_tests_end ends a scan, which unwraps at a line and column. */
static void
doltgres_scan_tests_end(HostIoIo *io, HostOwnedHandle handle, uint32 line, uint32 col)
{
	HostError	failed;

	if (!doltgres_scan_scan_end(io, handle, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
}

/* doltgres_scan_tests_case_t is a case's actor's captures: the test's capability. */
typedef struct doltgres_scan_tests_case_t
{
	HostIoIo   *io;
} doltgres_scan_tests_case_t;

/* doltgres_scan_tests_run runs a case's actor as actor 1 over the test's capability, as sim::as_actor(1, ..) does. */
static void
doltgres_scan_tests_run(void (*actor) (void *captures))
{
	SimcTestIo	test_io = simc_io();
	doltgres_scan_tests_case_t c = {&test_io.io};

	simc_as_actor(1, actor, &c);
	simc_TestIo_drop(&test_io);
}

/* doltgres_scan_tests_rescan starts a scan over, which unwraps at a line and column. */
static void
doltgres_scan_tests_rescan(HostIoIo *io, HostOwnedHandle handle, uint32 line, uint32 col)
{
	HostError	failed;

	if (!doltgres_scan_scan_rescan(io, handle, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
}

/*
 * doltgres_scan_tests_one reads a scan's next row and answers whether there
 * was one, dropping it, which unwraps at a line and column.
 */
static bool
doltgres_scan_tests_one(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						HostOwnedHandle handle, DoltgresScanDirection way, uint32 line, uint32 col)
{
	DoltgresScanRow row;
	bool		has = doltgres_scan_tests_next(io, store, under, handle, way, &row, line, col);

	if (has)
		doltgres_scan_Row_drop(&row);
	return has;
}

/*
 * doltgres_scan_tests_fails reads a scan's next row and reports whether the
 * read failed with an SQLSTATE, or with any error when the SQLSTATE is NULL.
 */
static bool
doltgres_scan_tests_fails(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						  HostOwnedHandle handle, DoltgresScanDirection way, const char *sqlstate)
{
	DoltgresScanStores stores = doltgres_scan_tests_stores(store);
	DoltgresScanRow row;
	HostError	failed;
	bool		has;
	bool		matches;

	if (doltgres_scan_next(io, &stores, under, handle, way, &has, &row, &failed))
	{
		if (has)
			doltgres_scan_Row_drop(&row);
		return false;
	}
	matches = sqlstate == NULL || memcmp(host_Error_sqlstate(&failed), sqlstate, 5) == 0;
	host_Error_drop(&failed);
	return matches;
}

/* doltgres_scan_tests_under_t is a case's store, state, snapshot, and what a read reads under. */
typedef struct doltgres_scan_tests_under_t
{
	TreesStoreMemoryStore store;
	HostSharedHeld held;
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
} doltgres_scan_tests_under_t;

/* doltgres_scan_tests_under makes a case's store and state and an Mvcc snapshot of the simulation's running xids. */
static void
doltgres_scan_tests_under(HostIoIo *io, doltgres_scan_tests_under_t *result)
{
	result->store = trees_store_MemoryStore_new();
	result->held = doltgres_scan_tests_made(io);
	result->taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	result->snapshot = simc_Taken_lent(&result->taken);
	result->under.state = host_shared_Held_record(&result->held);
	result->under.snapshot = &result->snapshot;
	result->under.has_serial = false;
}

/* doltgres_scan_tests_under_drop drops a case's snapshot, state, and store, as the case's end drops them. */
static void
doltgres_scan_tests_under_drop(doltgres_scan_tests_under_t *self)
{
	simc_Taken_drop(&self->taken);
	host_shared_Held_drop(&self->held);
	trees_store_MemoryStore_drop(&self->store);
}

/* test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TxnBaseBase based;
	HostOwnedHandle handle;
	StdVec		rows;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_base(io, &u.store, 3000);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 1878, 105);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1879, 13, doltgres_scan_tests_expected(&rows, 0, 3000, false),
			   "read(io, &store, &under, handle, Direction::Forward) == expected(3000)");
	doltgres_scan_tests_rows_drop(&rows);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1880, 13, doltgres_scan_tests_expected(&rows, 0, 3000, false),
			   "read(io, &store, &under, handle, Direction::Forward) == expected(3000)");
	doltgres_scan_tests_rows_drop(&rows);
	STD_ASSERT(1881, 13, doltgres_scan_tests_one(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, 1881,
												 74),
			   "next(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
	doltgres_scan_tests_rescan(io, handle, 1882, 32);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1883, 13, rows.len == 3000, "read(io, &store, &under, handle, Direction::Forward).len() == 3000");
	doltgres_scan_tests_rows_drop(&rows);
	doltgres_scan_tests_end(io, handle, 1884, 29);
	STD_ASSERT(1885, 13, doltgres_scan_tests_fails(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, NULL),
			   "next(io, &store, &under, handle, Direction::Forward).is_err()");
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls:
 * the fork's own: a scan gives every row of a base in key order, one call a
 * row across its leaves, then no row, after which it starts over, as heap's
 * scan does once it ends; a rescan starts it over; and its end leaves no
 * record its handle names.
 */
void
test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_scan_reads_a_bases_rows_in_order_across_calls_closure_0);
}

/* test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TxnBaseBase based;
	HostOwnedHandle handle;
	StdVec		rows;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_base(io, &u.store, 3000);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 1904, 105);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(1907, 13, doltgres_scan_tests_expected(&rows, 0, 3000, true),
			   "read(io, &store, &under, handle, Direction::Backward) == reversed");
	doltgres_scan_tests_rows_drop(&rows);
	for (int i = 0; i < 3; i++)
		STD_ASSERT(1909, 17, doltgres_scan_tests_one(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD,
													 1909, 78),
				   "next(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(1911, 13, doltgres_scan_tests_expected(&rows, 0, 2, true),
			   "read(io, &store, &under, handle, Direction::Backward) == expected(2).into_iter().rev().collect::<Vec<_>>()");
	doltgres_scan_tests_rows_drop(&rows);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1915, 13, doltgres_scan_tests_expected(&rows, 0, 3000, false),
			   "read(io, &store, &under, handle, Direction::Forward) == expected(3000)");
	doltgres_scan_tests_rows_drop(&rows);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(1916, 13, doltgres_scan_tests_expected(&rows, 0, 3000, true),
			   "read(io, &store, &under, handle, Direction::Backward) == reversed");
	doltgres_scan_tests_rows_drop(&rows);
	doltgres_scan_tests_end(io, handle, 1917, 29);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does: the
 * fork's own, after heap's scan: a backward scan gives a base's rows in
 * reverse key order across its leaves; a read that turns back gives the row
 * before the last one read; and a backward read after a forward one found no
 * row starts from the last row.
 */
void
test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_scan_reads_backward_and_turns_as_heaps_does_closure_0);
}

/* test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 zero[4] = {0};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TxnBaseBase based;
	HostOwnedHandle handle;
	HostOwnedHandle one;
	HostOwnedHandle other;
	DoltgresScanRow first;
	StdVec		rows;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_base(io, &u.store, 0);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 1934, 106);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1935, 13, rows.len == 0, "read(io, &store, &under, handle, Direction::Forward).is_empty()");
	doltgres_scan_tests_rows_drop(&rows);
	doltgres_scan_tests_end(io, handle, 1936, 29);
	tree = doltgres_scan_tests_base(io, &u.store, 10);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	one = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 1939, 96);
	other = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 1940, 96);
	if (!doltgres_scan_tests_next(io, &u.store, &u.under, one, DOLTGRES_SCAN_DIRECTION_FORWARD, &first, 1942, 75))
		STD_OPTION_UNWRAP_FAILED(1942, 84);
	STD_ASSERT(1943, 13, doltgres_scan_tests_bytes_is(&first.key, zero, 4), "first.key == 0u32.to_be_bytes()");
	doltgres_scan_Row_drop(&first);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, other, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1944, 13, doltgres_scan_tests_expected(&rows, 0, 10, false),
			   "read(io, &store, &under, other, Direction::Forward) == expected(10)");
	doltgres_scan_tests_rows_drop(&rows);
	rows = doltgres_scan_tests_read(io, &u.store, &u.under, one, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1945, 13, doltgres_scan_tests_expected(&rows, 1, 9, false),
			   "read(io, &store, &under, one, Direction::Forward) == expected(10)[1..]");
	doltgres_scan_tests_rows_drop(&rows);
	doltgres_scan_tests_end(io, one, 1946, 26);
	doltgres_scan_tests_end(io, other, 1947, 28);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places: the
 * fork's own: a scan of an empty base gives no row, and two scans of one base
 * each keep their own place.
 */
void
test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_scans_of_one_base_keep_their_own_places_closure_0);
}

/*
 * doltgres_scan_tests_keyless builds a keyless table's base of entries, each
 * its key's byte, its count of copies, and its copies' tuple ids, its tuple
 * the key's byte alone, and returns its root's name.
 */
static TreesStoreTree
doltgres_scan_tests_keyless(HostIoIo *io, TreesStoreMemoryStore *store, const uint8 *bytes, const uint64 *counts,
							const uint64 *const *tuples, const size_t *tuples_len, size_t len)
{
	TreesBuildBuilder builder;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(store), 256, &builder);
	for (size_t i = 0; i < len; i++)
	{
		StdVec		key;
		StdVec		value;
		TreesBuildEntry entry;

		std_vec_u8_from_elem(&key, bytes[i], 16);
		txn_counted_keyless_value(counts[i], &bytes[i], 1, &value);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		for (size_t k = 0; k < tuples_len[i]; k++)
		{
			ChunksTrailerStamp stamp = {5, tuples[i][k]};

			std_vec_push_Checked(&entry.stamps, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		}
		doltgres_scan_tests_add(&builder, io, &entry, 1959, 56);
	}
	return doltgres_scan_tests_finished(&builder, io, 1961, 53);
}

/* doltgres_scan_tests_pair_t is a (Vec<u8>, u64): a row's key or tuple, and its tuple id. */
typedef struct doltgres_scan_tests_pair_t
{
	StdVec		bytes;
	uint64		tuple;
} doltgres_scan_tests_pair_t;

/* doltgres_scan_tests_pairs_drop drops a case's pairs. */
static void
doltgres_scan_tests_pairs_drop(StdVec *rows)
{
	for (size_t i = 0; i < rows->len; i++)
		std_vec_free_Checked(&STD_VEC_AT(rows, doltgres_scan_tests_pair_t, i).bytes, DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(rows, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
}

/*
 * doltgres_scan_tests_copies_read reads every row a scan has left in a
 * direction and returns each one's tuple and tuple id.
 */
static StdVec
doltgres_scan_tests_copies_read(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
								HostOwnedHandle handle, DoltgresScanDirection way)
{
	StdVec		rows;
	DoltgresScanRow row;

	std_vec_new(&rows, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
	while (doltgres_scan_tests_next(io, store, under, handle, way, &row, 1974, 67))
	{
		doltgres_scan_tests_pair_t copy = {row.tuple, row.stamp.tuple};

		row.tuple = (StdVec) STD_VEC_NEW;
		doltgres_scan_Row_drop(&row);
		std_vec_push_Checked(&rows, &copy, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
	}
	return rows;
}

/* doltgres_scan_tests_copies reports whether copies_read's pairs are one-byte tuples with their tuple ids, in order. */
static bool
doltgres_scan_tests_copies(const StdVec *rows, const uint8 *bytes, const uint64 *tuple_ids, size_t len)
{
	if (rows->len != len)
		return false;
	for (size_t i = 0; i < len; i++)
	{
		const doltgres_scan_tests_pair_t *row = &STD_VEC_AT(rows, doltgres_scan_tests_pair_t, i);

		if (!doltgres_scan_tests_bytes_is(&row->bytes, &bytes[i], 1) || row->tuple != tuple_ids[i])
			return false;
	}
	return true;
}

/* test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 bytes[] = {1, 2};
	static const uint64 counts[] = {2, 1};
	static const uint64 first[] = {10, 11};
	static const uint64 second[] = {12};
	static const uint64 *const tuples[] = {first, second};
	static const size_t tuples_len[] = {2, 1};
	static const uint8 forward[] = {1, 1, 2};
	static const uint64 forward_ids[] = {10, 11, 12};
	static const uint8 backward[] = {2, 1, 1};
	static const uint64 backward_ids[] = {12, 11, 10};
	static const uint8 short_bytes[] = {3};
	static const uint64 short_counts[] = {2};
	static const uint64 short_first[] = {13};
	static const uint64 *const short_tuples[] = {short_first};
	static const size_t short_len[] = {1};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TxnBaseBase based;
	HostOwnedHandle handle;
	DoltgresScanRow row;
	StdVec		rows;
	bool		has;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_keyless(io, &u.store, bytes, counts, tuples, tuples_len, 2);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, NULL, 0, 0, 1995, 108);
	rows = doltgres_scan_tests_copies_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(1996, 13, doltgres_scan_tests_copies(&rows, forward, forward_ids, 3),
			   "copies_read(io, &store, &under, handle, Direction::Forward) == [(vec![1], 10), (vec![1], 11), (vec![2], 12)]");
	doltgres_scan_tests_pairs_drop(&rows);
	rows = doltgres_scan_tests_copies_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2000, 13, doltgres_scan_tests_copies(&rows, backward, backward_ids, 3),
			   "copies_read(io, &store, &under, handle, Direction::Backward) == [(vec![2], 12), (vec![1], 11), (vec![1], 10)]");
	doltgres_scan_tests_pairs_drop(&rows);
	has = doltgres_scan_tests_next(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, &row, 2005, 70);
	STD_ASSERT(2004, 13, has && row.stamp.tuple == 10,
			   "next(io, &store, &under, handle, Direction::Forward).unwrap().map(|row| row.stamp.tuple) == Some(10)");
	if (has)
		doltgres_scan_Row_drop(&row);
	doltgres_scan_tests_rescan(io, handle, 2008, 32);
	rows = doltgres_scan_tests_copies_read(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2009, 13, rows.len == 3, "copies_read(io, &store, &under, handle, Direction::Forward).len() == 3");
	doltgres_scan_tests_pairs_drop(&rows);
	doltgres_scan_tests_end(io, handle, 2010, 29);
	tree = doltgres_scan_tests_keyless(io, &u.store, short_bytes, short_counts, short_tuples, short_len, 1);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, NULL, 0, 0, 2012, 109);
	STD_ASSERT(2014, 13, doltgres_scan_tests_fails(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD,
												   "XX001"),
			   "read.is_err_and(|error| error.sqlstate() == b\"XX001\")");
	doltgres_scan_tests_end(io, handle, 2015, 29);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp:
 * PLAN's format section, a keyless table's entry: a scan gives a keyless
 * entry's row once a copy, copy i with the entry's stamp i, forward and
 * backward; a rescan in the middle of an entry's copies starts over; and an
 * entry whose stamps differ in number from its copies is data corruption.
 */
void
test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_keyless_entry_gives_each_copy_with_its_own_stamp_closure_0);
}

/*
 * doltgres_scan_tests_fetch fetches a row by its tuple id under fresh epochs,
 * answering whether it read without error, the error then in failed; it
 * takes the layout.
 */
static bool
doltgres_scan_tests_fetch(HostIoIo *io, TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						  const TxnBaseBase *base, DoltgresScanLayout layout, uint64 tuple_id, bool *has,
						  DoltgresScanRow *row, HostError *failed)
{
	DoltgresScanEpochs epochs;
	DoltgresScanResolved resolved = {0};
	DoltgresScanTrees trees;
	bool		ok;

	doltgres_scan_Epochs_Default_default(&epochs);
	trees = doltgres_scan_tests_trees(base);
	ok = doltgres_scan_fetch(io, TREES_STORE_MEMORY_STORE_NODE_STORE(store), under, &epochs, &resolved, &trees,
							 &layout, tuple_id, has, row, failed);
	doltgres_scan_Resolved_drop(&resolved);
	doltgres_scan_Epochs_drop(&epochs);
	return ok;
}

/* test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint32 ns[] = {0, 1, 1500, 2999};
	static const uint64 nones[] = {0, 3001, UINT64CONST(1) << 16 | 1};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TreesStoreTree ids;
	TreesStoreTree lost_ids;
	TxnBaseBase fetched;
	TxnBaseBase unpaired;
	TxnBaseBase lost;
	uint64		tuple_ids[3000];
	StdVec		keys[3000];
	uint64		lost_tuple_ids[] = {1, 2};
	StdVec		lost_keys[2];
	DoltgresScanRow row;
	HostError	failed;
	bool		has;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_base(io, &u.store, 3000);
	for (uint32 n = 0; n < 3000; n++)
	{
		tuple_ids[n] = (uint64) n + 1;
		keys[n] = doltgres_scan_tests_be4(n);
	}
	ids = doltgres_scan_tests_pairs(io, &u.store, tuple_ids, keys, 3000);
	fetched = doltgres_scan_tests_based(tree, &ids, 1);
	for (size_t i = 0; i < lengthof(ns); i++)
	{
		uint32		n = ns[i];
		uint8		key[4] = {(uint8) (n >> 24), (uint8) (n >> 16), (uint8) (n >> 8), (uint8) n};
		uint32		square = n * n;
		uint8		tuple[4] = {(uint8) square, (uint8) (square >> 8), (uint8) (square >> 16),
		(uint8) (square >> 24)};

		if (!doltgres_scan_tests_fetch(io, &u.store, &u.under, &fetched, doltgres_scan_tests_layout(keyed, 1, 7),
									   (uint64) n + 1, &has, &row, &failed))
			STD_RESULT_EXPECT_FAILED(2046, 18, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(2047, 18);
		STD_ASSERT(2048, 17, doltgres_scan_tests_bytes_is(&row.key, key, 4) &&
				   doltgres_scan_tests_bytes_is(&row.tuple, tuple, 4) && row.stamp.tuple == (uint64) n + 1 &&
				   row.table == 7,
				   "(row.key, row.tuple, row.stamp.tuple, row.table) == (n.to_be_bytes().to_vec(), (n * n).to_le_bytes().to_vec(), u64::from(n) + 1, 7)");
		doltgres_scan_Row_drop(&row);
	}
	for (size_t i = 0; i < lengthof(nones); i++)
	{
		if (!doltgres_scan_tests_fetch(io, &u.store, &u.under, &fetched, doltgres_scan_tests_layout(keyed, 1, 7),
									   nones[i], &has, &row, &failed))
			STD_RESULT_EXPECT_FAILED(2056, 26, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
		if (has)
			doltgres_scan_Row_drop(&row);
		STD_ASSERT(2054, 17, !has,
				   "fetch(io, &store, (&under, &mut Epochs::default()), trees(&fetched), layout(vec![1], 7), none).unwrap().is_none()");
	}
	unpaired = doltgres_scan_tests_based(tree, NULL, 1);
	if (!doltgres_scan_tests_fetch(io, &u.store, &u.under, &unpaired, doltgres_scan_tests_layout(keyed, 1, 7), 1, &has,
								   &row, &failed))
		STD_RESULT_EXPECT_FAILED(2063, 22, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	if (has)
		doltgres_scan_Row_drop(&row);
	STD_ASSERT(2061, 13, !has,
			   "fetch(io, &store, (&under, &mut Epochs::default()), trees(&unpaired), layout(vec![1], 7), 1).unwrap().is_none()");
	lost_keys[0] = doltgres_scan_tests_be4(9999);
	lost_keys[1] = doltgres_scan_tests_be4(3);
	lost_ids = doltgres_scan_tests_pairs(io, &u.store, lost_tuple_ids, lost_keys, 2);
	lost = doltgres_scan_tests_based(tree, &lost_ids, 1);
	for (uint64 tuple_id = 1; tuple_id <= 2; tuple_id++)
	{
		bool		ok = doltgres_scan_tests_fetch(io, &u.store, &u.under, &lost, doltgres_scan_tests_layout(keyed, 1, 7),
												   tuple_id, &has, &row, &failed);
		bool		corrupt = !ok && memcmp(host_Error_sqlstate(&failed), "XX001", 5) == 0;

		if (ok && has)
			doltgres_scan_Row_drop(&row);
		if (!ok)
			host_Error_drop(&failed);
		STD_ASSERT(2072, 17, corrupt, "read.is_err_and(|error| error.sqlstate() == b\"XX001\")");
	}
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names: PLAN
 * item 2, resolving a tid over a base: a fetch gives the row whose stamp
 * holds a tuple id, across the tuple id tree's leaves and the table's; a
 * tuple id the tree lacks, or any over a base with no tuple id tree, gives
 * none; and a pair naming a key the table's tree lacks, or a key whose entry
 * has no copy of the id, is data corruption.
 */
void
test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_fetch_gives_the_row_its_tuple_id_names_closure_0);
}

/* test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 bytes[] = {1, 2};
	static const uint64 counts[] = {2, 1};
	static const uint64 first[] = {10, 11};
	static const uint64 second[] = {12};
	static const uint64 *const tuples[] = {first, second};
	static const size_t tuples_len[] = {2, 1};
	static const uint64 fetches[] = {10, 11, 12};
	static const uint8 fetched_bytes[] = {1, 1, 2};
	static const uint8 short_bytes[] = {3};
	static const uint64 short_counts[] = {2};
	static const uint64 short_first[] = {13};
	static const uint64 *const short_tuples[] = {short_first};
	static const size_t short_len[] = {1};
	static const char uneven[] = "a stored row's stamps differ in number from its copies";
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TreesStoreTree ids;
	TxnBaseBase fetched;
	TxnBaseBase short_base;
	uint64		id_tuples[] = {10, 11, 12};
	StdVec		id_keys[3];
	uint64		short_ids_tuples[] = {13};
	StdVec		short_keys[1];
	DoltgresScanRow row;
	HostError	failed;
	StdStr		message;
	bool		has;
	bool		ok;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_keyless(io, &u.store, bytes, counts, tuples, tuples_len, 2);
	std_vec_u8_from_elem(&id_keys[0], 1, 16);
	std_vec_u8_from_elem(&id_keys[1], 1, 16);
	std_vec_u8_from_elem(&id_keys[2], 2, 16);
	ids = doltgres_scan_tests_pairs(io, &u.store, id_tuples, id_keys, 3);
	fetched = doltgres_scan_tests_based(tree, &ids, 1);
	for (size_t i = 0; i < lengthof(fetches); i++)
	{
		uint8		key[16];

		memset(key, fetched_bytes[i], sizeof(key));
		if (!doltgres_scan_tests_fetch(io, &u.store, &u.under, &fetched, doltgres_scan_tests_layout(NULL, 0, 0),
									   fetches[i], &has, &row, &failed))
			STD_RESULT_EXPECT_FAILED(2103, 18, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(2104, 18);
		STD_ASSERT(2105, 17, doltgres_scan_tests_bytes_is(&row.key, key, 16) &&
				   doltgres_scan_tests_bytes_is(&row.tuple, &fetched_bytes[i], 1) && row.stamp.tuple == fetches[i],
				   "(row.key, row.tuple, row.stamp.tuple) == (vec![byte; 16], vec![byte], tuple_id)");
		doltgres_scan_Row_drop(&row);
	}
	tree = doltgres_scan_tests_keyless(io, &u.store, short_bytes, short_counts, short_tuples, short_len, 1);
	std_vec_u8_from_elem(&short_keys[0], 3, 16);
	ids = doltgres_scan_tests_pairs(io, &u.store, short_ids_tuples, short_keys, 1);
	short_base = doltgres_scan_tests_based(tree, &ids, 1);
	ok = doltgres_scan_tests_fetch(io, &u.store, &u.under, &short_base, doltgres_scan_tests_layout(NULL, 0, 0), 13,
								   &has, &row, &failed);
	if (ok && has)
		doltgres_scan_Row_drop(&row);
	if (!ok)
		message = host_Error_message(&failed);
	STD_ASSERT(2117, 13, !ok && message.len == sizeof(uneven) - 1 && memcmp(message.ptr, uneven, message.len) == 0,
			   "read.is_err_and(|error| error.message() == uneven().message())");
	host_Error_drop(&failed);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id:
 * PLAN's format section, a keyless table's entry: each copy of a keyless
 * entry fetches by its own tuple id, giving the stamp the id names, and a
 * fetch from an entry whose stamps differ in number from its copies is data
 * corruption, as the scan says it is.
 */
void
test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_each_keyless_copy_fetches_by_its_own_tuple_id_closure_0);
}

/* doltgres_scan_tests_tid_valid answers whether a tuple id is valid for a scan, which unwraps at a line and column. */
static bool
doltgres_scan_tests_tid_valid(HostIoIo *io, HostOwnedHandle handle, uint64 tuple_id, uint32 line, uint32 col)
{
	HostError	failed;
	bool		valid;

	if (!doltgres_scan_tid_valid(io, handle, tuple_id, 291, &valid, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return valid;
}

/* test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint64 tuple_ids[] = {1, UINT64CONST(1) << 16 | 291, UINT64CONST(2) << 16 | 1, 0,
	UINT64CONST(1) << 16 | 292};
	static const bool valid[] = {true, true, false, false, false};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	TreesStoreTree tree = doltgres_scan_tests_base(io, &store, 1);
	TxnBaseBase based = doltgres_scan_tests_based(tree, NULL, UINT64CONST(1) << 16 | 5);
	HostOwnedHandle handle = doltgres_scan_tests_begin(io, &store, 2, &based, keyed, 1, 0, 2133, 106);

	for (size_t i = 0; i < lengthof(tuple_ids); i++)
		STD_ASSERT(2137, 17, doltgres_scan_tests_tid_valid(io, handle, tuple_ids[i], 2137, 65) == valid[i],
				   "tid_valid(io, handle, tuple_id, 291).unwrap() == valid");
	doltgres_scan_tests_end(io, handle, 2139, 29);
	handle = doltgres_scan_tests_begin(io, &store, 0, NULL, keyed, 1, 0, 2140, 82);
	STD_ASSERT(2141, 13, !doltgres_scan_tests_tid_valid(io, handle, 1, 2141, 52), "!tid_valid(io, handle, 1, 291).unwrap()");
	doltgres_scan_tests_end(io, handle, 2142, 29);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks:
 * the fork's own, after heap's check against the relation's blocks: a tuple
 * id is valid for a scan when its block is below the storage's blocks when
 * it began and its offset is from 1 to a page's most tuples, and none is for
 * a scan of no blocks.
 */
void
test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_tuple_id_is_valid_within_the_storages_blocks_closure_0);
}

/* doltgres_scan_tests_set_range sets a scan's tid range, which unwraps at a line and column. */
static void
doltgres_scan_tests_set_range(HostIoIo *io, HostOwnedHandle handle, uint64 first, uint64 last, uint32 line,
							  uint32 col)
{
	HostError	failed;

	if (!doltgres_scan_set_range(io, handle, first, last, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
}

/* doltgres_scan_tests_in_range reads a tid range's next row, which unwraps at a line and column. */
static bool
doltgres_scan_tests_in_range(HostIoIo *io, TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
							 HostOwnedHandle handle, DoltgresScanDirection way, DoltgresScanRow *row, uint32 line,
							 uint32 col)
{
	HostError	failed;
	bool		has;

	if (!doltgres_scan_next_in_range(io, TREES_STORE_MEMORY_STORE_NODE_STORE(store), under, handle, way, &has, row,
									 &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return has;
}

/*
 * doltgres_scan_tests_ranged reads every row a scan's tid range has left in a
 * direction and returns each one's tuple id.
 */
static StdVec
doltgres_scan_tests_ranged(HostIoIo *io, TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						   HostOwnedHandle handle, DoltgresScanDirection way)
{
	StdVec		ids;
	DoltgresScanRow row;

	std_vec_new(&ids, sizeof(uint64), _Alignof(uint64));
	while (doltgres_scan_tests_in_range(io, store, under, handle, way, &row, 2149, 76))
	{
		std_vec_push_Checked(&ids, &row.stamp.tuple, sizeof(uint64), _Alignof(uint64));
		doltgres_scan_Row_drop(&row);
	}
	return ids;
}

/* doltgres_scan_tests_ids_are reports whether tuple ids run from first to last, either way, and frees them. */
static bool
doltgres_scan_tests_ids_are(StdVec *ids, uint64 first, uint64 last)
{
	bool		descending = first > last;
	uint64		count = (descending ? first - last : last - first) + 1;
	bool		same = ids->len == count;

	for (uint64 i = 0; same && i < count; i++)
		same = STD_VEC_AT(ids, uint64, i) == (descending ? first - i : first + i);
	std_vec_free_Checked(ids, sizeof(uint64), _Alignof(uint64));
	return same;
}

/* doltgres_scan_tests_none frees tuple ids and reports whether there were none. */
static bool
doltgres_scan_tests_none(StdVec *ids)
{
	bool		empty = ids->len == 0;

	std_vec_free_Checked(ids, sizeof(uint64), _Alignof(uint64));
	return empty;
}

/* doltgres_scan_tests_paired builds base's tuple id tree of rows rows, each tuple id its row's number plus one. */
static TreesStoreTree
doltgres_scan_tests_paired(HostIoIo *io, TreesStoreMemoryStore *store, uint32 rows)
{
	uint64	   *tuple_ids = palloc(sizeof(uint64) * rows);
	StdVec	   *keys = palloc(sizeof(StdVec) * rows);
	TreesStoreTree tree;

	for (uint32 n = 0; n < rows; n++)
	{
		tuple_ids[n] = (uint64) n + 1;
		keys[n] = doltgres_scan_tests_be4(n);
	}
	tree = doltgres_scan_tests_pairs(io, store, tuple_ids, keys, rows);
	pfree(keys);
	pfree(tuple_ids);
	return tree;
}

/* test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TreesStoreTree ids;
	TxnBaseBase based;
	HostOwnedHandle handle;
	StdVec		read;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_base(io, &u.store, 3000);
	ids = doltgres_scan_tests_paired(io, &u.store, 3000);
	based = doltgres_scan_tests_based(tree, &ids, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 2173, 110);
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2174, 13, doltgres_scan_tests_none(&read), "ranged(io, &store, &under, handle, Direction::Forward).is_empty()");
	doltgres_scan_tests_set_range(io, handle, 100, 2500, 2175, 46);
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2177, 13, doltgres_scan_tests_ids_are(&read, 100, 2500),
			   "ranged(io, &store, &under, handle, Direction::Forward) == forward");
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2178, 13, doltgres_scan_tests_ids_are(&read, 100, 2500),
			   "ranged(io, &store, &under, handle, Direction::Forward) == forward");
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2180, 13, doltgres_scan_tests_ids_are(&read, 2500, 100),
			   "ranged(io, &store, &under, handle, Direction::Backward) == backward");
	for (int i = 0; i < 3; i++)
	{
		DoltgresScanRow row;

		STD_ASSERT(2182, 17, doltgres_scan_tests_in_range(io, &u.store, &u.under, handle,
														  DOLTGRES_SCAN_DIRECTION_FORWARD, &row, 2182, 87),
				   "next_in_range(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
		doltgres_scan_Row_drop(&row);
	}
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2184, 13, doltgres_scan_tests_ids_are(&read, 101, 100),
			   "ranged(io, &store, &under, handle, Direction::Backward) == [101, 100]");
	doltgres_scan_tests_set_range(io, handle, 2990, 9999, 2185, 47);
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2186, 13, doltgres_scan_tests_ids_are(&read, 3000, 2990),
			   "ranged(io, &store, &under, handle, Direction::Backward) == (2990..=3000).rev().collect::<Vec<_>>()");
	doltgres_scan_tests_set_range(io, handle, 5, 3, 2190, 41);
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2191, 13, doltgres_scan_tests_none(&read), "ranged(io, &store, &under, handle, Direction::Forward).is_empty()");
	read = doltgres_scan_tests_ranged(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2192, 13, doltgres_scan_tests_none(&read), "ranged(io, &store, &under, handle, Direction::Backward).is_empty()");
	doltgres_scan_tests_end(io, handle, 2193, 29);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order:
 * the fork's own, after heap's heap_getnextslot_tidrange: a tid range read
 * gives the rows whose tuple ids are in the range in tuple id order across
 * the tuple id tree's leaves, forward and backward; a read that turns back
 * gives the row before the last one read; a read after the range's end
 * starts it over; a range whose first id is above its last, or a scan with
 * no range set, gives no row.
 */
void
test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_tid_range_read_gives_its_rows_in_tuple_id_order_closure_0);
}

/* test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	uint64		tuple_ids[] = {1, 2};
	StdVec		keys[2];
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TreesStoreTree ids;
	TxnBaseBase based;
	HostOwnedHandle handle;
	DoltgresScanRow row;
	HostError	failed;
	bool		has;
	bool		ok;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_base(io, &u.store, 10);
	keys[0] = doltgres_scan_tests_be4(0);
	keys[1] = doltgres_scan_tests_be4(9999);
	ids = doltgres_scan_tests_pairs(io, &u.store, tuple_ids, keys, 2);
	based = doltgres_scan_tests_based(tree, &ids, 1);
	handle = doltgres_scan_tests_begin(io, &u.store, 1, &based, keyed, 1, 0, 2213, 110);
	doltgres_scan_tests_set_range(io, handle, 0, 10, 2214, 42);
	STD_ASSERT(2215, 13, doltgres_scan_tests_in_range(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD,
													  &row, 2215, 83),
			   "next_in_range(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
	doltgres_scan_Row_drop(&row);
	ok = doltgres_scan_next_in_range(io, TREES_STORE_MEMORY_STORE_NODE_STORE(&u.store), &u.under, handle,
									 DOLTGRES_SCAN_DIRECTION_FORWARD, &has, &row, &failed);
	if (ok && has)
		doltgres_scan_Row_drop(&row);
	STD_ASSERT(2217, 13, !ok && memcmp(host_Error_sqlstate(&failed), "XX001", 5) == 0,
			   "read.is_err_and(|error| error.sqlstate() == b\"XX001\")");
	host_Error_drop(&failed);
	doltgres_scan_tests_end(io, handle, 2218, 29);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption:
 * PLAN item 2, resolving a tid: a pair of the range whose key the table's
 * tree lacks is data corruption, after the pairs before it read.
 */
void
test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_tid_range_pair_the_table_lacks_is_data_corruption_closure_0);
}

/*
 * test_doltgres_scan_tests_a_parallel_scans_units_partition_the_entries:
 * PLAN item 5's parallel scans: a parallel scan's units partition a tree's
 * entries in key order, each entry in one unit, whether the entries are
 * fewer than the units, as many, or more, and at counts past 64 bits'
 * product.
 */
void
test_doltgres_scan_tests_a_parallel_scans_units_partition_the_entries(void)
{
	static const uint64 cases[][2] = {{0, 5}, {3, 7}, {7, 7}, {3000, 7}, {1, 1}, {PG_UINT64_MAX, 3}};

	for (size_t i = 0; i < lengthof(cases); i++)
	{
		uint64		entries = cases[i][0];
		uint64		units = cases[i][1];
		uint64		next = 0;

		for (uint64 unit = 0; unit < units; unit++)
		{
			uint64		first;
			uint64		past;

			doltgres_scan_unit_range(unit, units, entries, &first, &past);
			STD_ASSERT(2230, 17, first == next, "{entries} entries, unit {unit} of {units}: first == next");
			STD_ASSERT(2231, 17, past >= first, "past >= first");
			next = past;
		}
		STD_ASSERT(2234, 13, next == entries, "{entries} entries in {units} units: next == entries");
	}
}

/*
 * doltgres_scan_tests_unit_order orders a parallel scan's pairs by key, then
 * tuple id, as a sort of the (key, id) tuples does.
 */
static StdOrdering
doltgres_scan_tests_unit_order(void *captures, const void *a, const void *b)
{
	const doltgres_scan_tests_pair_t *x = a;
	const doltgres_scan_tests_pair_t *y = b;
	StdOrdering order = std_bytes_Ord_cmp(x->bytes.ptr, x->bytes.len, y->bytes.ptr, y->bytes.len);

	if (order != STD_ORDERING_EQUAL)
		return order;
	return x->tuple < y->tuple ? STD_ORDERING_LESS : (x->tuple > y->tuple ? STD_ORDERING_GREATER : STD_ORDERING_EQUAL);
}

/* doltgres_scan_tests_step reads a parallel scan's next step, which unwraps at a line and column. */
static DoltgresScanStep
doltgres_scan_tests_step(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						 HostOwnedHandle handle, uint32 line, uint32 col)
{
	DoltgresScanStores stores = doltgres_scan_tests_stores(store);
	DoltgresScanStep step;
	HostError	failed;

	if (!doltgres_scan_next_parallel(io, &stores, under, handle, &step, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return step;
}

/*
 * doltgres_scan_tests_read_units reads a parallel scan's units in an order,
 * each to its end, and returns each row's key and tuple id.
 */
static StdVec
doltgres_scan_tests_read_units(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
							   HostOwnedHandle handle, const uint64 *units, size_t len)
{
	StdVec		rows;

	std_vec_new(&rows, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
	for (size_t i = 0; i < len; i++)
	{
		HostError	failed;
		DoltgresScanStep step;

		if (!doltgres_scan_start_unit(io, handle, units[i], &failed))
			STD_RESULT_EXPECT_FAILED(2248, 42, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
		while ((step = doltgres_scan_tests_step(io, store, under, handle, 2249, 80)).tag == DOLTGRES_SCAN_STEP_ROW)
		{
			doltgres_scan_tests_pair_t row = {step.row.key, step.row.stamp.tuple};

			step.row.key = (StdVec) STD_VEC_NEW;
			doltgres_scan_Row_drop(&step.row);
			std_vec_push_Checked(&rows, &row, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
		}
	}
	return rows;
}

/* doltgres_scan_tests_unit_is reports whether a parallel scan's row is base's row n with its tuple id. */
static bool
doltgres_scan_tests_unit_is(const doltgres_scan_tests_pair_t *row, uint32 n)
{
	uint8		key[4] = {(uint8) (n >> 24), (uint8) (n >> 16), (uint8) (n >> 8), (uint8) n};

	return doltgres_scan_tests_bytes_is(&row->bytes, key, 4) && row->tuple == (uint64) n + 1;
}

/* doltgres_scan_tests_begin_parallel begins a parallel scan over a shared base in bytes' order, which it takes. */
static HostOwnedHandle
doltgres_scan_tests_begin_parallel(HostIoIo *io, DoltgresScanShared *shared, const uint8 *keyed, size_t keyed_len,
								   uint32 line, uint32 col)
{
	StdVec		layout;
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_BYTES};
	HostOwnedHandle handle;
	HostError	failed;

	std_slice_to_vec_Checked(&layout, keyed, keyed_len, DOLTGRES_SCAN_BYTES);
	if (!doltgres_scan_begin_parallel(io, shared, 0, &layout, 0, &order, &handle, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return handle;
}

/*
 * doltgres_scan_tests_shared returns a parallel scan's share of a tree of
 * entries in units, no tuple id tree, and no splits.
 */
static DoltgresScanShared
doltgres_scan_tests_shared(TreesStoreTree tree, uint64 entries, uint64 units)
{
	DoltgresScanShared shared = {.has_tree = true, .tree = tree, .fresh_end = 1, .entries = entries, .units = units};

	std_vec_new(&shared.splits, DOLTGRES_SCAN_BYTES);
	doltgres_scan_Epochs_Default_default(&shared.epochs);
	return shared;
}

/* <[(Vec<u8>, u64)]>::sort, which the parallel case sorts its rows by. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TestsPair
#define STD_SORT_T doltgres_scan_tests_pair_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TestsPair
#define STD_STABLE_SORT_T doltgres_scan_tests_pair_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY
#include "doltgres/std/stable_sort_template.h"

/* test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint64 order[] = {3, 0, 6, 1, 5, 2, 4};
	static const uint64 middle_unit[] = {2};
	static const uint64 copies_order[] = {1, 0};
	static const uint8 bytes[] = {1, 2, 3};
	static const uint64 counts[] = {2, 1, 3};
	static const uint64 first_copies[] = {10, 11};
	static const uint64 second_copies[] = {12};
	static const uint64 third_copies[] = {13, 14, 15};
	static const uint64 *const tuples[] = {first_copies, second_copies, third_copies};
	static const size_t tuples_len[] = {2, 1, 3};
	static const uint64 ids[] = {12, 13, 14, 15, 10, 11};
	doltgres_scan_tests_under_t u;
	DoltgresScanShared shared;
	HostOwnedHandle handle;
	DoltgresScanStep step;
	StdVec		rows;
	uint64		first;
	uint64		past;
	bool		same;

	doltgres_scan_tests_under(io, &u);
	shared = doltgres_scan_tests_shared(doltgres_scan_tests_base(io, &u.store, 3000), 3000, 7);
	handle = doltgres_scan_tests_begin_parallel(io, &shared, keyed, 1, 2281, 82);
	step = doltgres_scan_tests_step(io, &u.store, &u.under, handle, 2282, 72);
	if (step.tag == DOLTGRES_SCAN_STEP_ROW)
		doltgres_scan_Row_drop(&step.row);
	STD_ASSERT(2282, 13, step.tag == DOLTGRES_SCAN_STEP_UNIT,
			   "matches!(next_parallel(io, &store, &under, handle).unwrap(), Step::Unit)");
	rows = doltgres_scan_tests_read_units(io, &u.store, &u.under, handle, order, lengthof(order));
	std_slice_sort_by_TestsPair(rows.ptr, rows.len, doltgres_scan_tests_unit_order, NULL, NULL);
	same = rows.len == 3000;
	for (uint32 n = 0; same && n < 3000; n++)
		same = doltgres_scan_tests_unit_is(&STD_VEC_AT(&rows, doltgres_scan_tests_pair_t, n), n);
	doltgres_scan_tests_pairs_drop(&rows);
	STD_ASSERT(2287, 13, same, "rows == expected");
	doltgres_scan_unit_range(2, 7, 3000, &first, &past);
	rows = doltgres_scan_tests_read_units(io, &u.store, &u.under, handle, middle_unit, 1);
	same = rows.len == past - first;
	for (uint64 i = 0; same && i < rows.len; i++)
		same = doltgres_scan_tests_unit_is(&STD_VEC_AT(&rows, doltgres_scan_tests_pair_t, i), (uint32) (first + i));
	doltgres_scan_tests_pairs_drop(&rows);
	STD_ASSERT(2290, 13, same, "middle == expected[first as usize..past as usize]");
	doltgres_scan_tests_end(io, handle, 2291, 29);
	shared = doltgres_scan_tests_shared(doltgres_scan_tests_keyless(io, &u.store, bytes, counts, tuples, tuples_len, 3),
										3, 2);
	handle = doltgres_scan_tests_begin_parallel(io, &shared, NULL, 0, 2303, 85);
	rows = doltgres_scan_tests_read_units(io, &u.store, &u.under, handle, copies_order, lengthof(copies_order));
	same = rows.len == lengthof(ids);
	for (size_t i = 0; same && i < rows.len; i++)
		same = STD_VEC_AT(&rows, doltgres_scan_tests_pair_t, i).tuple == ids[i];
	doltgres_scan_tests_pairs_drop(&rows);
	STD_ASSERT(2305, 13, same, "ids == [12, 13, 14, 15, 10, 11]");
	doltgres_scan_tests_end(io, handle, 2306, 29);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once: PLAN
 * item 5's parallel scans: a parallel scan's units, read in any order, give
 * each of the base's rows once, each unit its entries in key order from the
 * ordinal its seek finds, across the tree's leaves; a keyless entry's copies
 * stay with their entry's unit; and a scan with no unit asks for one.
 */
void
test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_parallel_scans_units_give_each_row_once_closure_0);
}

/* doltgres_scan_tests_range_blocks answers a tid range's first block and count, which unwraps at a line and column. */
static void
doltgres_scan_tests_range_blocks(HostIoIo *io, HostOwnedHandle handle, uint64 blocks, uint32 *first, uint32 *count,
								 uint32 line, uint32 col)
{
	HostError	failed;

	if (!doltgres_scan_range_blocks(io, handle, blocks, first, count, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
}

/* test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint64 cases[][5] = {
		{5, UINT64CONST(1) << 16 | 7, 3, 0, 2},
		{UINT64CONST(1) << 16, UINT64CONST(1) << 16 | 3, 3, 1, 1},
		{UINT64CONST(1) << 16 | 1, UINT64CONST(9) << 16 | 1, 3, 1, 2},
		{UINT64CONST(5) << 16 | 1, UINT64CONST(6) << 16 | 1, 3, 0, 0},
		{UINT64CONST(2) << 16 | 1, UINT64CONST(1) << 16 | 1, 3, 0, 0},
		{1, UINT64CONST(1) << 16 | 1, 0, 0, 0},
	};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostOwnedHandle handle = doltgres_scan_tests_begin(io, &store, 1, NULL, keyed, 1, 0, 2320, 95);
	uint32		first;
	uint32		count;

	trees_store_MemoryStore_drop(&store);
	doltgres_scan_tests_range_blocks(io, handle, 3, &first, &count, 2321, 52);
	STD_ASSERT(2321, 13, first == 0 && count == 0, "range_blocks(io, handle, 3).unwrap() == (0, 0)");
	for (size_t i = 0; i < lengthof(cases); i++)
	{
		doltgres_scan_tests_set_range(io, handle, cases[i][0], cases[i][1], 2330, 52);
		doltgres_scan_tests_range_blocks(io, handle, cases[i][2], &first, &count, 2332, 54);
		STD_ASSERT(2331, 17, first == cases[i][3] && count == cases[i][4],
				   "{first:#x} to {last:#x}, {blocks} blocks: range_blocks(io, handle, blocks).unwrap() == expected");
	}
	doltgres_scan_tests_end(io, handle, 2337, 29);
}

/*
 * test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps: the
 * fork's own, after heap's heap_set_tidrange: a parallel tid range scan's
 * units are drawn from its range's blocks within the storage's, the first
 * and how many: a range within the blocks gives its own, one from offset 0
 * starts at its block, one past the last block is cut at it, and one past
 * the end, one whose first is above its last, one never set, and any over a
 * storage of no blocks give none.
 */
void
test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_parallel_tid_ranges_blocks_are_heaps_closure_0);
}

/*
 * doltgres_scan_tests_pushed pushes a version of a key into a case's delta by
 * a writer at a command; it takes the op.
 */
static void
doltgres_scan_tests_pushed(HostIoIo *io, const HostSharedHeld *held, const uint8 *key, size_t key_len, HostXid writer,
						   uint32 command, TxnMemtableOp *op)
{
	TxnMemtableVersionChange change = {
		.writer = {.xid = writer.f0, .command = command},
		.op = op,
		.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE,
		.shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND},
	};
	TxnMemtableVersionHandle handle;
	HostError	failed;

	if (!txn_memtable_table_push(io, host_shared_Held_record(held), key, key_len, &change, &handle, &failed))
		STD_RESULT_EXPECT_FAILED(2351, 54, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	txn_memtable_Op_drop(op);
}

/* doltgres_scan_tests_put returns a version's put of a tuple with a tuple id. */
static TxnMemtablePut
doltgres_scan_tests_put(const uint8 *tuple, size_t tuple_len, uint64 tuple_id)
{
	TxnMemtablePut put = {.tuple = {tuple_id}, .heap_bytes = 32};

	std_slice_to_vec_Checked(&put.value, tuple, tuple_len, DOLTGRES_SCAN_BYTES);
	std_vec_new(&put.addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	std_vec_new(&put.values, sizeof(uint32), _Alignof(uint32));
	return put;
}

/*
 * doltgres_scan_tests_put_op returns the op that puts a tuple with a tuple
 * id, as Op::Put(put(..)) does.
 */
static TxnMemtableOp
doltgres_scan_tests_put_op(const char *tuple, uint64 tuple_id)
{
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_PUT};

	op.u.put = doltgres_scan_tests_put((const uint8 *) tuple, strlen(tuple), tuple_id);
	return op;
}

/*
 * doltgres_scan_tests_shown_t is Shown: a row a case reads, its key, its
 * tuple, its tuple id, its writer and command, and its changer.
 */
typedef struct doltgres_scan_tests_shown_t
{
	StdVec		key;
	StdVec		tuple;
	uint64		tuple_id;
	uint64		txn;
	uint32		command;
	bool		has_changer;
	uint64		changer_xid;
	uint32		changer_command;
} doltgres_scan_tests_shown_t;

/* doltgres_scan_tests_shown_push pushes a Shown of a key, a tuple, a tuple id, a writer and command, and a changer. */
static void
doltgres_scan_tests_shown_push(StdVec *rows, const uint8 *key, size_t key_len, const uint8 *tuple, size_t tuple_len,
							   uint64 tuple_id, uint64 txn, uint32 command, bool has_changer, uint64 changer_xid,
							   uint32 changer_command)
{
	doltgres_scan_tests_shown_t row = {.tuple_id = tuple_id, .txn = txn, .command = command,
	.has_changer = has_changer, .changer_xid = changer_xid, .changer_command = changer_command};

	std_slice_to_vec_Checked(&row.key, key, key_len, DOLTGRES_SCAN_BYTES);
	std_slice_to_vec_Checked(&row.tuple, tuple, tuple_len, DOLTGRES_SCAN_BYTES);
	std_vec_push_Checked(rows, &row, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
}

/* doltgres_scan_tests_shown_drop drops a case's Shown rows. */
static void
doltgres_scan_tests_shown_drop(StdVec *rows)
{
	for (size_t i = 0; i < rows->len; i++)
	{
		doltgres_scan_tests_shown_t *row = &STD_VEC_AT(rows, doltgres_scan_tests_shown_t, i);

		std_vec_free_Checked(&row->key, DOLTGRES_SCAN_BYTES);
		std_vec_free_Checked(&row->tuple, DOLTGRES_SCAN_BYTES);
	}
	std_vec_free_Checked(rows, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
}

/* doltgres_scan_tests_shown_eq reports whether two cases' Shown rows are equal, as assert_eq! compares them. */
static bool
doltgres_scan_tests_shown_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t i = 0; i < a->len; i++)
	{
		const doltgres_scan_tests_shown_t *x = &STD_VEC_AT(a, doltgres_scan_tests_shown_t, i);
		const doltgres_scan_tests_shown_t *y = &STD_VEC_AT(b, doltgres_scan_tests_shown_t, i);

		if (!doltgres_scan_tests_bytes_is(&x->key, y->key.ptr, y->key.len) ||
			!doltgres_scan_tests_bytes_is(&x->tuple, y->tuple.ptr, y->tuple.len) || x->tuple_id != y->tuple_id ||
			x->txn != y->txn || x->command != y->command || x->has_changer != y->has_changer ||
			(x->has_changer && (x->changer_xid != y->changer_xid || x->changer_command != y->changer_command)))
			return false;
	}
	return true;
}

/* doltgres_scan_tests_shown reads every row a scan has left in a direction under a snapshot. */
static StdVec
doltgres_scan_tests_shown(HostIoIo *io, const TreesStoreMemoryStore *store, const DoltgresScanUnder *under,
						  HostOwnedHandle handle, DoltgresScanDirection way)
{
	StdVec		rows;
	DoltgresScanRow row;

	std_vec_new(&rows, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	while (doltgres_scan_tests_next(io, store, under, handle, way, &row, 2371, 67))
	{
		doltgres_scan_tests_shown_t shown = {row.key, row.tuple, row.stamp.tuple, row.stamp.txn, row.command,
		row.has_changer, row.changer.xid, row.changer.command};

		row.key = (StdVec) STD_VEC_NEW;
		row.tuple = (StdVec) STD_VEC_NEW;
		doltgres_scan_Row_drop(&row);
		std_vec_push_Checked(&rows, &shown, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	}
	return rows;
}

/* doltgres_scan_tests_reversed returns the first len of rows in reverse order, cloned. */
static StdVec
doltgres_scan_tests_reversed(const StdVec *rows, size_t len)
{
	StdVec		out;

	std_vec_new(&out, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	for (size_t i = len; i > 0; i--)
	{
		const doltgres_scan_tests_shown_t *row = &STD_VEC_AT(rows, doltgres_scan_tests_shown_t, i - 1);

		doltgres_scan_tests_shown_push(&out, row->key.ptr, row->key.len, row->tuple.ptr, row->tuple.len, row->tuple_id,
									   row->txn, row->command, row->has_changer, row->changer_xid,
									   row->changer_command);
	}
	return out;
}

/* test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 zero[] = {0, 0, 0, 0};
	static const uint8 one[] = {0, 0, 0, 1};
	static const uint8 two[] = {0, 0, 0, 2};
	static const uint8 three[] = {0, 0, 0, 3};
	static const uint8 five[] = {0, 0, 0, 5};
	static const uint8 between[] = {0, 0, 0, 1, 0};
	static const uint8 square_one[] = {1, 0, 0, 0};
	static const uint8 square_two[] = {4, 0, 0, 0};
	static const uint8 square_three[] = {9, 0, 0, 0};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesStoreTree tree = doltgres_scan_tests_base(io, &store, 4);
	HostXid		writer = simc_begin();
	TxnMemtableOp op;
	TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
	SimcTaken	running_taken;
	SimcTaken	after_taken;
	HostSnapshot running;
	HostSnapshot after;
	TxnBaseBase based;
	HostOwnedHandle handle;
	DoltgresScanUnder under;
	StdVec		merged;
	StdVec		based_rows;
	StdVec		rows;
	StdVec		expected;
	uint64		w;

	op = doltgres_scan_tests_put_op("five", 100);
	doltgres_scan_tests_pushed(io, &held, five, 4, writer, 2, &op);
	doltgres_scan_tests_pushed(io, &held, one, 4, writer, 0, &deleted);
	op = doltgres_scan_tests_put_op("between", 102);
	doltgres_scan_tests_pushed(io, &held, between, 5, writer, 1, &op);
	op = doltgres_scan_tests_put_op("two", 101);
	doltgres_scan_tests_pushed(io, &held, two, 4, writer, 1, &op);
	running_taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	simc_commit(writer);
	after_taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	running = simc_Taken_lent(&running_taken);
	after = simc_Taken_lent(&after_taken);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &store, 1, &based, keyed, 1, 0, 2413, 105);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &after;
	under.has_serial = false;
	w = writer.f0;
	std_vec_new(&merged, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	doltgres_scan_tests_shown_push(&merged, zero, 4, zero, 4, 1, 3, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, between, 5, (const uint8 *) "between", 7, 102, w, 1, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, two, 4, (const uint8 *) "two", 3, 101, w, 1, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, three, 4, square_three, 4, 4, 3, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, five, 4, (const uint8 *) "five", 4, 100, w, 2, false, 0, 0);
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2423, 13, doltgres_scan_tests_shown_eq(&rows, &merged),
			   "shown(io, &store, &under, handle, Direction::Forward) == merged");
	doltgres_scan_tests_shown_drop(&rows);
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	expected = doltgres_scan_tests_reversed(&merged, merged.len);
	STD_ASSERT(2424, 13, doltgres_scan_tests_shown_eq(&rows, &expected),
			   "shown(io, &store, &under, handle, Direction::Backward) == reversed(&merged)");
	doltgres_scan_tests_shown_drop(&expected);
	doltgres_scan_tests_shown_drop(&rows);
	for (int i = 0; i < 3; i++)
		STD_ASSERT(2426, 17, doltgres_scan_tests_one(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, 2426,
													 78),
				   "next(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	expected = doltgres_scan_tests_reversed(&merged, 2);
	STD_ASSERT(2428, 13, doltgres_scan_tests_shown_eq(&rows, &expected),
			   "shown(io, &store, &under, handle, Direction::Backward) == reversed(&merged[..2])");
	doltgres_scan_tests_shown_drop(&expected);
	doltgres_scan_tests_shown_drop(&rows);
	under.snapshot = &running;
	doltgres_scan_tests_rescan(io, handle, 2430, 32);
	std_vec_new(&based_rows, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	doltgres_scan_tests_shown_push(&based_rows, zero, 4, zero, 4, 1, 3, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&based_rows, one, 4, square_one, 4, 2, 3, 0, true, w, 0);
	doltgres_scan_tests_shown_push(&based_rows, two, 4, square_two, 4, 3, 3, 0, true, w, 1);
	doltgres_scan_tests_shown_push(&based_rows, three, 4, square_three, 4, 4, 3, 0, false, 0, 0);
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2437, 13, doltgres_scan_tests_shown_eq(&rows, &based_rows),
			   "shown(io, &store, &under, handle, Direction::Forward) == based_rows");
	doltgres_scan_tests_shown_drop(&rows);
	doltgres_scan_tests_end(io, handle, 2438, 29);
	doltgres_scan_tests_shown_drop(&based_rows);
	doltgres_scan_tests_shown_drop(&merged);
	simc_Taken_drop(&after_taken);
	simc_Taken_drop(&running_taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order:
 * step (5)'s decision 3, the scan's merge: a read gives the base's rows and
 * the delta's in key order, each key's rows the snapshot sees, forward and
 * backward and across a turn. A committed delete hides a base row and a
 * committed update shows its new row in the base row's place; a key only the
 * delta holds comes in its order. A snapshot taken while the writer ran sees
 * the base's rows alone, each the writer changed showing it as its xmax (bugs
 * row 60), and the delta's rows show their writer and command.
 */
void
test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_scan_merges_the_deltas_rows_with_the_bases_in_key_order_closure_0);
}

/*
 * doltgres_scan_tests_keys_t is a seek case's captures: its store, its
 * scan, and what its reads read under, as its keys closure takes them.
 */
typedef struct doltgres_scan_tests_keys_t
{
	const TreesStoreMemoryStore *store;
	HostOwnedHandle handle;
	const DoltgresScanUnder *under;
} doltgres_scan_tests_keys_t;

/*
 * doltgres_scan_tests_keys_are seeks a case's scan to a key a way, reads on,
 * and reports whether the keys it read are the keys expected, in order.
 */
static bool
doltgres_scan_tests_keys_are(HostIoIo *io, const doltgres_scan_tests_keys_t *c, const uint8 *from, size_t from_len,
							 DoltgresScanDirection way, const uint8 *const *keys, const size_t *lens, size_t len)
{
	DoltgresScanStores stores = doltgres_scan_tests_stores(c->store);
	HostError	failed;
	StdVec		rows;
	bool		same;

	if (!doltgres_scan_seek(io, &stores, c->handle, from, from_len, way, &failed))
		STD_RESULT_EXPECT_FAILED(2466, 53, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	rows = doltgres_scan_tests_read(io, c->store, c->under, c->handle, way);
	same = rows.len == len;
	for (size_t i = 0; same && i < len; i++)
		same = doltgres_scan_tests_bytes_is(&STD_VEC_AT(&rows, DoltgresScanRow, i).key, keys[i], lens[i]);
	doltgres_scan_tests_rows_drop(&rows);
	return same;
}

/* test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 zero[] = {0, 0, 0, 0};
	static const uint8 one[] = {0, 0, 0, 1};
	static const uint8 two[] = {0, 0, 0, 2};
	static const uint8 three[] = {0, 0, 0, 3};
	static const uint8 four[] = {0, 0, 0, 4};
	static const uint8 five[] = {0, 0, 0, 5};
	static const uint8 nine[] = {0, 0, 0, 9};
	static const uint8 between[] = {0, 0, 0, 1, 0};
	static const uint8 short_key[] = {0};
	static const uint8 square_three[] = {9, 0, 0, 0};
	static const uint8 *const on_two[] = {two, three, five};
	static const size_t on_two_lens[] = {4, 4, 4};
	static const uint8 *const on_between[] = {between, two, three, five};
	static const size_t on_between_lens[] = {5, 4, 4, 4};
	static const uint8 *const on_five[] = {five};
	static const size_t on_five_lens[] = {4};
	static const uint8 *const back_two[] = {two, between, zero};
	static const size_t back_two_lens[] = {4, 5, 4};
	static const uint8 *const back_four[] = {three, two, between, zero};
	static const size_t back_four_lens[] = {4, 4, 5, 4};
	static const uint8 *const back_between[] = {between, zero};
	static const size_t back_between_lens[] = {5, 4};
	static const uint8 *const back_zero[] = {zero};
	static const size_t back_zero_lens[] = {4};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesStoreTree tree = doltgres_scan_tests_base(io, &store, 4);
	HostXid		writer = simc_begin();
	TxnMemtableOp op;
	TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
	SimcTaken	after_taken;
	HostSnapshot after;
	TxnBaseBase based;
	DoltgresScanUnder under;
	doltgres_scan_tests_keys_t c;
	DoltgresScanStores stores;
	HostError	failed;
	StdVec		rows;

	op = doltgres_scan_tests_put_op("five", 100);
	doltgres_scan_tests_pushed(io, &held, five, 4, writer, 2, &op);
	doltgres_scan_tests_pushed(io, &held, one, 4, writer, 0, &deleted);
	op = doltgres_scan_tests_put_op("between", 102);
	doltgres_scan_tests_pushed(io, &held, between, 5, writer, 1, &op);
	simc_commit(writer);
	after_taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	after = simc_Taken_lent(&after_taken);
	based = doltgres_scan_tests_based(tree, NULL, 1);
	c.store = &store;
	c.handle = doltgres_scan_tests_begin(io, &store, 1, &based, keyed, 1, 0, 2463, 105);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &after;
	under.has_serial = false;
	c.under = &under;
	STD_ASSERT(2470, 13, doltgres_scan_tests_keys_are(io, &c, two, 4, DOLTGRES_SCAN_DIRECTION_FORWARD, on_two,
													  on_two_lens, 3),
			   "keys(io, &key(2), Direction::Forward) == vec![key(2), key(3), key(5)]");
	STD_ASSERT(2471, 13, doltgres_scan_tests_keys_are(io, &c, between, 5, DOLTGRES_SCAN_DIRECTION_FORWARD, on_between,
													  on_between_lens, 4),
			   "keys(io, &between, Direction::Forward) == vec![between.clone(), key(2), key(3), key(5)]");
	STD_ASSERT(2472, 13, doltgres_scan_tests_keys_are(io, &c, five, 4, DOLTGRES_SCAN_DIRECTION_FORWARD, on_five,
													  on_five_lens, 1),
			   "keys(io, &key(5), Direction::Forward) == vec![key(5)]");
	STD_ASSERT(2473, 13, doltgres_scan_tests_keys_are(io, &c, one, 4, DOLTGRES_SCAN_DIRECTION_FORWARD, on_between,
													  on_between_lens, 4),
			   "keys(io, &key(1), Direction::Forward) == vec![between.clone(), key(2), key(3), key(5)]");
	STD_ASSERT(2474, 13, doltgres_scan_tests_keys_are(io, &c, nine, 4, DOLTGRES_SCAN_DIRECTION_FORWARD, NULL, NULL, 0),
			   "keys(io, &key(9), Direction::Forward) == Vec::<Vec<u8>>::new()");
	STD_ASSERT(2475, 13, doltgres_scan_tests_keys_are(io, &c, two, 4, DOLTGRES_SCAN_DIRECTION_BACKWARD, back_two,
													  back_two_lens, 3),
			   "keys(io, &key(2), Direction::Backward) == vec![key(2), between.clone(), key(0)]");
	STD_ASSERT(2476, 13, doltgres_scan_tests_keys_are(io, &c, four, 4, DOLTGRES_SCAN_DIRECTION_BACKWARD, back_four,
													  back_four_lens, 4),
			   "keys(io, &key(4), Direction::Backward) == vec![key(3), key(2), between.clone(), key(0)]");
	STD_ASSERT(2477, 13, doltgres_scan_tests_keys_are(io, &c, between, 5, DOLTGRES_SCAN_DIRECTION_BACKWARD,
													  back_between, back_between_lens, 2),
			   "keys(io, &between, Direction::Backward) == vec![between.clone(), key(0)]");
	STD_ASSERT(2478, 13, doltgres_scan_tests_keys_are(io, &c, zero, 4, DOLTGRES_SCAN_DIRECTION_BACKWARD, back_zero,
													  back_zero_lens, 1),
			   "keys(io, &[0, 0, 0, 0], Direction::Backward) == vec![key(0)]");
	STD_ASSERT(2479, 13, doltgres_scan_tests_keys_are(io, &c, short_key, 1, DOLTGRES_SCAN_DIRECTION_BACKWARD, NULL, NULL,
													  0),
			   "keys(io, &[0], Direction::Backward) == Vec::<Vec<u8>>::new()");
	stores = doltgres_scan_tests_stores(&store);
	if (!doltgres_scan_seek(io, &stores, c.handle, three, 4, DOLTGRES_SCAN_DIRECTION_FORWARD, &failed))
		STD_RESULT_EXPECT_FAILED(2480, 67, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	rows = doltgres_scan_tests_read(io, &store, &under, c.handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	std_index_check(0, rows.len, RUST_FILE, 2481, 24);
	STD_ASSERT(2481, 13, doltgres_scan_tests_bytes_is(&STD_VEC_AT(&rows, DoltgresScanRow, 0).key, three, 4) &&
			   doltgres_scan_tests_bytes_is(&STD_VEC_AT(&rows, DoltgresScanRow, 0).tuple, square_three, 4),
			   "read(io, &store, &under, handle, Direction::Forward)[0] == (key(3), square(3))");
	doltgres_scan_tests_rows_drop(&rows);
	doltgres_scan_tests_end(io, c.handle, 2482, 29);
	simc_Taken_drop(&after_taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way:
 * the btree's part 2, slice 6: a seek puts a scan at the first key at or past
 * a key the way it reads, the base's entries and the delta's keys merged from
 * there, the delta's own key at the seek included, and a key neither holds
 * started from the nearest past it; a seek past every key gives no row.
 */
void
test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_seek_reads_on_from_the_first_key_at_or_past_it_that_way_closure_0);
}

/*
 * doltgres_scan_tests_entries builds a tree of three entries, each keyed by
 * its number big-endian with its number's byte as its value and the stamps
 * given, and returns its root's name.
 */
static TreesStoreTree
doltgres_scan_tests_entries(HostIoIo *io, TreesStoreMemoryStore *store, const ChunksTrailerStamp *stamps,
							size_t stamps_len)
{
	TreesBuildBuilder builder;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(store), 256, &builder);
	for (uint32 n = 0; n < 3; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(n);
		StdVec		value;
		uint8		byte = (uint8) n;
		TreesBuildEntry entry;

		std_slice_to_vec_Checked(&value, &byte, 1, DOLTGRES_SCAN_BYTES);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		for (size_t i = 0; i < stamps_len; i++)
			std_vec_push_Checked(&entry.stamps, &stamps[i], sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		doltgres_scan_tests_add(&builder, io, &entry, 2501, 80);
	}
	return doltgres_scan_tests_finished(&builder, io, 2503, 61);
}

/* test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const ChunksTrailerStamp stamp = {3, 1};
	doltgres_scan_tests_under_t u;
	TreesStoreTree tree;
	TreesStoreTree stamped;
	TxnBaseBase index;
	HostOwnedHandle handle;
	StdVec		rows;
	StdVec		expected;

	doltgres_scan_tests_under(io, &u);
	tree = doltgres_scan_tests_entries(io, &u.store, NULL, 0);
	index = doltgres_scan_tests_based(tree, NULL, 1);
	index.has_index = true;
	index.index = 1;
	handle = doltgres_scan_tests_begin(io, &u.store, 0, &index, keyed, 1, 0, 2511, 90);
	rows = doltgres_scan_tests_shown(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	std_vec_new(&expected, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	for (uint32 n = 0; n < 3; n++)
	{
		uint8		key[4] = {(uint8) (n >> 24), (uint8) (n >> 16), (uint8) (n >> 8), (uint8) n};
		uint8		byte = (uint8) n;

		doltgres_scan_tests_shown_push(&expected, key, 4, &byte, 1, 0, 0, 0, false, 0, 0);
	}
	STD_ASSERT(2515, 13, doltgres_scan_tests_shown_eq(&rows, &expected), "rows == expected");
	doltgres_scan_tests_end(io, handle, 2516, 29);
	stamped = doltgres_scan_tests_entries(io, &u.store, &stamp, 1);
	index = doltgres_scan_tests_based(stamped, NULL, 1);
	index.has_index = true;
	index.index = 1;
	handle = doltgres_scan_tests_begin(io, &u.store, 0, &index, keyed, 1, 0, 2519, 90);
	STD_ASSERT(2521, 13, doltgres_scan_tests_fails(io, &u.store, &u.under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD,
												   "XX001"),
			   "read.is_err_and(|error| error.sqlstate() == b\"XX001\")");
	doltgres_scan_tests_end(io, handle, 2522, 29);
	doltgres_scan_tests_shown_drop(&expected);
	doltgres_scan_tests_shown_drop(&rows);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero:
 * the btree's part 2, slice 6 (agreed with TxnPF 9): a keyed table's index
 * entry, which keeps no stamp in its base, is read as the index fold reads
 * it, one copy stamped {0, 0}, and a stamp on such an entry is data
 * corruption.
 */
void
test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_keyed_tables_index_entry_is_one_copy_stamped_zero_closure_0);
}

/*
 * test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies_closure_0
 * is the case's actor.
 */
static void
test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 bytes[] = {1, 2};
	static const uint64 counts[] = {2, 1};
	static const uint64 first[] = {10, 11};
	static const uint64 second[] = {12};
	static const uint64 *const tuples[] = {first, second};
	static const size_t tuples_len[] = {2, 1};
	static const uint8 ones[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
	static const uint8 added[] = {1};
	static const uint8 forward[] = {1, 1, 2};
	static const uint64 forward_ids[] = {11, 13, 12};
	static const uint8 backward[] = {2, 1, 1};
	static const uint64 backward_ids[] = {12, 13, 11};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesStoreTree tree = doltgres_scan_tests_keyless(io, &store, bytes, counts, tuples, tuples_len, 2);
	HostXid		writer = simc_begin();
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_REMOVE, .u.remove = {.f0 = {10}}};
	TxnMemtableOp added_op = {.tag = TXN_MEMTABLE_OP_ADD};
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
	TxnBaseBase based;
	HostOwnedHandle handle;
	StdVec		rows;

	doltgres_scan_tests_pushed(io, &held, ones, 16, writer, 0, &op);
	added_op.u.add = doltgres_scan_tests_put(added, 1, 13);
	doltgres_scan_tests_pushed(io, &held, ones, 16, writer, 1, &added_op);
	simc_commit(writer);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &store, 1, &based, NULL, 0, 0, 2544, 108);
	rows = doltgres_scan_tests_copies_read(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2546, 13, doltgres_scan_tests_copies(&rows, forward, forward_ids, 3),
			   "copies_read(io, &store, &under, handle, Direction::Forward) == rows");
	doltgres_scan_tests_pairs_drop(&rows);
	rows = doltgres_scan_tests_copies_read(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2547, 13, doltgres_scan_tests_copies(&rows, backward, backward_ids, 3),
			   "copies_read(io, &store, &under, handle, Direction::Backward) == reversed(&rows)");
	doltgres_scan_tests_pairs_drop(&rows);
	doltgres_scan_tests_end(io, handle, 2548, 29);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies:
 * step (5)'s decision 3 over a keyless table: a key's rows are its base
 * entry's copies the delta leaves, a copy a committed removal names hidden,
 * then the copies the delta adds, forward and backward.
 */
void
test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_keyless_keys_rows_are_its_base_copies_the_delta_leaves_then_its_added_copies_closure_0);
}

/*
 * doltgres_scan_tests_dirty_is reports whether a Dirty read's found running is
 * a writer as its xmin or its xmax alone.
 */
static bool
doltgres_scan_tests_dirty_is(const TxnVisibleDirty *dirty, bool has_xmin, bool has_xmax, HostXid writer)
{
	return dirty->has_xmin == has_xmin && (!has_xmin || dirty->xmin.f0 == writer.f0) &&
		dirty->has_xmax == has_xmax && (!has_xmax || dirty->xmax.f0 == writer.f0) && !dirty->has_token;
}

/* test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 zero[] = {0, 0, 0, 0};
	static const uint8 one[] = {0, 0, 0, 1};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesStoreTree tree = doltgres_scan_tests_base(io, &store, 1);
	HostXid		writer = simc_begin();
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_DELETE};
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
	TxnBaseBase based;
	HostOwnedHandle handle;
	DoltgresScanRow row;
	uint64		ids[2];
	TxnVisibleDirty found[2];
	size_t		len = 0;

	doltgres_scan_tests_pushed(io, &held, zero, 4, writer, 0, &op);
	op = doltgres_scan_tests_put_op("new", 50);
	doltgres_scan_tests_pushed(io, &held, one, 4, writer, 0, &op);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_DIRTY, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	based = doltgres_scan_tests_based(tree, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &store, 1, &based, keyed, 1, 0, 2569, 105);
	while (doltgres_scan_tests_next(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, &row, 2571, 88))
	{
		if (len < lengthof(ids))
		{
			ids[len] = row.stamp.tuple;
			found[len] = row.dirty;
		}
		len++;
		doltgres_scan_Row_drop(&row);
	}
	STD_ASSERT(2576, 13, len == 2 && ids[0] == 1 && doltgres_scan_tests_dirty_is(&found[0], false, true, writer) &&
			   ids[1] == 50 && doltgres_scan_tests_dirty_is(&found[1], true, false, writer),
			   "found == [(1, deleting), (50, inserting)]");
	doltgres_scan_tests_end(io, handle, 2577, 29);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row:
 * step (5)'s decision 2 through a scan: a Dirty read gives a base row a
 * running transaction deletes with that deleter as what it found running,
 * and a row a running transaction inserted with that writer.
 */
void
test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_dirty_scan_gives_what_it_found_running_of_each_row_closure_0);
}

/*
 * doltgres_scan_tests_taken inserts a row as a case does, taking its key's
 * stripe with a test's Waits and no resolve, and answers its tuple id; it
 * takes the row.
 */
static bool
doltgres_scan_tests_taken(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, TxnInsertInsert *row,
						  const TxnInsertRoom *room, const HostSharedHeld *toast, const TxnValuesEntry *toast_entries,
						  size_t toast_entries_len, uint64 *result, HostError *error)
{
	HostIoWaits waits = host_io_Waits_testing();
	TxnInsertKeyed keyed = {.stripe = txn_change_stripe(&row->storage, row->key.ptr, row->key.len, true)};
	TxnInsertInserted inserted;
	TxnInsertInsertedRef other = {&inserted, {NULL, std_unit_Debug_fmt}};
	StdFmtPiece pieces[] = {STD_FMT_LIT("a case's insert gave "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&other, txn_insert_Inserted_Debug_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};
	StdString	message;

	if (!txn_insert_insert(io, store, held, row, room, NULL, toast, toast_entries, toast_entries_len, keyed, &waits,
						   &inserted, error))
		return false;
	if (inserted.tag == TXN_INSERT_INSERTED_TOOK)
	{
		*result = inserted.u.took.tuple;
		return true;
	}
	std_fmt_format_Checked(&message, &arguments);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/* doltgres_scan_tests_room returns a case's room of a row's bytes on a page of 8168 bytes and 291 tuples. */
static TxnInsertRoom
doltgres_scan_tests_room(uint64 bytes)
{
	TxnInsertRoom room = {.bytes = bytes, .page = 8168, .most = 291};

	return room;
}

/* doltgres_scan_tests_insert returns a case's insert into table 7 of a key and a tuple by a writer at command zero. */
static TxnInsertInsert
doltgres_scan_tests_insert(StdVec key, const uint8 *tuple, size_t tuple_len, HostXid writer)
{
	TxnInsertInsert row = {.storage = doltgres_scan_tests_storage(), .table = 7, .key = key,
	.put = doltgres_scan_tests_put(tuple, tuple_len, 0), .writer = {.xid = writer.f0}, .logged = true};

	std_vec_new(&row.kept, sizeof(size_t), _Alignof(size_t));
	return row;
}

/* doltgres_scan_tests_inserted inserts a case's row, which unwraps at a line and column, and returns its tuple id. */
static uint64
doltgres_scan_tests_inserted(HostIoIo *io, TreesStoreMemoryStore *store, const HostSharedHeld *held,
							 TxnInsertInsert *row, const TxnInsertRoom *room, uint32 line, uint32 col)
{
	HostError	failed;
	uint64		tuple_id;

	if (!doltgres_scan_tests_taken(io, TREES_STORE_MEMORY_STORE_NODE_STORE(store), held, row, room, NULL, NULL, 0,
								   &tuple_id, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return tuple_id;
}

/* doltgres_scan_tests_ended ends the simulation's transactions' writes, which unwraps at a line and column. */
static void
doltgres_scan_tests_ended(HostIoIo *io, uint32 line, uint32 col)
{
	HostError	failed;

	if (!txn_written_ended(io, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
}

/*
 * doltgres_scan_tests_stamped builds a tree of three entries, each keyed by
 * its number big-endian with its number's byte as its value and stamped by
 * transaction 3 with its number plus one in block 1, and returns its root's
 * name, its builder unwrapping at its lines.
 */
static TreesStoreTree
doltgres_scan_tests_stamped(HostIoIo *io, TreesStoreMemoryStore *store, uint32 add_line, uint32 add_col,
							uint32 finish_line, uint32 finish_col)
{
	TreesBuildBuilder builder;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(store), 256, &builder);
	for (uint32 n = 0; n < 3; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(n);
		StdVec		value;
		uint8		byte = (uint8) n;
		ChunksTrailerStamp stamp = {3, UINT64CONST(1) << 16 | ((uint64) n + 1)};
		TreesBuildEntry entry;

		std_slice_to_vec_Checked(&value, &byte, 1, DOLTGRES_SCAN_BYTES);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		std_vec_push_Checked(&entry.stamps, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		doltgres_scan_tests_add(&builder, io, &entry, add_line, add_col);
	}
	return doltgres_scan_tests_finished(&builder, io, finish_line, finish_col);
}

/* doltgres_scan_tests_stamped_ids builds the tuple id tree of stamped's three entries, each its block 1 tuple id. */
static TreesStoreTree
doltgres_scan_tests_stamped_ids(HostIoIo *io, TreesStoreMemoryStore *store)
{
	uint64		tuple_ids[3];
	StdVec		keys[3];

	for (uint32 n = 0; n < 3; n++)
	{
		tuple_ids[n] = UINT64CONST(1) << 16 | ((uint64) n + 1);
		keys[n] = doltgres_scan_tests_be4(n);
	}
	return doltgres_scan_tests_pairs(io, store, tuple_ids, keys, 3);
}

/*
 * doltgres_scan_tests_fetch_is fetches a row by its tuple id under a snapshot
 * and reports whether it is the tuple, stamp, and changer expected, or only
 * the tuple and the stamp's tuple id when whole is false, or none when the
 * tuple expected is NULL.
 */
static bool
doltgres_scan_tests_fetch_is(HostIoIo *io, TreesStoreMemoryStore *store, const HostSharedHeld *held,
							 const TxnBaseBase *fetched, const HostSnapshot *snapshot, uint64 tuple_id,
							 const uint8 *tuple, size_t tuple_len, ChunksTrailerStamp stamp, bool whole,
							 bool has_changer, uint64 changer_xid, uint32 changer_command)
{
	static const uint8 keyed[] = {1};
	DoltgresScanUnder under = {host_shared_Held_record(held), snapshot};
	DoltgresScanRow row;
	HostError	failed;
	bool		has;
	bool		same;

	if (!doltgres_scan_tests_fetch(io, store, &under, fetched, doltgres_scan_tests_layout(keyed, 1, 7), tuple_id, &has,
								   &row, &failed))
		STD_RESULT_EXPECT_FAILED(2633, 26, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	if (!has)
		return tuple == NULL;
	same = tuple != NULL && doltgres_scan_tests_bytes_is(&row.tuple, tuple, tuple_len) &&
		row.stamp.tuple == stamp.tuple && (!whole || (row.stamp.txn == stamp.txn && row.has_changer == has_changer &&
													  (!has_changer || (row.changer.xid == changer_xid &&
																		row.changer.command == changer_command))));
	doltgres_scan_Row_drop(&row);
	return same;
}

/* test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 one[] = {0, 0, 0, 1};
	static const uint8 seven[] = {0, 0, 0, 7};
	static const uint8 zero_tuple[] = {0};
	static const uint8 one_tuple[] = {1};
	static const uint8 text[] = "inserted";
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesStoreTree tree = doltgres_scan_tests_stamped(io, &store, 2597, 60, 2599, 57);
	TreesStoreTree ids = doltgres_scan_tests_stamped_ids(io, &store);
	TxnBaseBase fetched = doltgres_scan_tests_based(tree, &ids, UINT64CONST(2) << 16 | 1);
	HostXid		writer = simc_begin();
	TxnInsertInsert row = doltgres_scan_tests_insert(doltgres_scan_tests_be4(7), text, sizeof(text) - 1, writer);
	TxnInsertRoom room = doltgres_scan_tests_room(32);
	TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
	TxnMemtableOp again;
	uint64		inserted = doltgres_scan_tests_inserted(io, &store, &held, &row, &room, 2623, 71);
	SimcTaken	running_taken;
	SimcTaken	after_taken;
	SimcTaken	any_taken;
	HostSnapshot running;
	HostSnapshot after;
	HostSnapshot any;
	HostXid		second;
	uint64		w;
	ChunksTrailerStamp stamp;

	doltgres_scan_tests_pushed(io, &held, one, 4, writer, 1, &deleted);
	running_taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	simc_commit(writer);
	after_taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	running = simc_Taken_lent(&running_taken);
	after = simc_Taken_lent(&after_taken);
	w = writer.f0;
	stamp.txn = w;
	stamp.tuple = inserted;
	STD_ASSERT(2637, 13, doltgres_scan_tests_fetch_is(io, &store, &held, &fetched, &after, inserted, text,
													  sizeof(text) - 1, stamp, true, false, 0, 0),
			   "read(io, &after, inserted) == Some((b\"inserted\".to_vec(), Stamp { txn: w, tuple: inserted }, None))");
	STD_ASSERT(2641, 13, doltgres_scan_tests_fetch_is(io, &store, &held, &fetched, &running, inserted, NULL, 0, stamp,
													  true, false, 0, 0),
			   "read(io, &running, inserted) == None");
	STD_ASSERT(2642, 13, doltgres_scan_tests_fetch_is(io, &store, &held, &fetched, &after, UINT64CONST(1) << 16 | 2,
													  NULL, 0, stamp, true, false, 0, 0),
			   "read(io, &after, 1 << 16 | 2) == None");
	stamp.txn = 3;
	stamp.tuple = UINT64CONST(1) << 16 | 2;
	STD_ASSERT(2644, 13, doltgres_scan_tests_fetch_is(io, &store, &held, &fetched, &running, UINT64CONST(1) << 16 | 2,
													  one_tuple, 1, stamp, true, true, w, 1),
			   "read(io, &running, 1 << 16 | 2) == deleted");
	stamp.tuple = UINT64CONST(1) << 16 | 1;
	STD_ASSERT(2645, 13, doltgres_scan_tests_fetch_is(io, &store, &held, &fetched, &after, UINT64CONST(1) << 16 | 1,
													  zero_tuple, 1, stamp, true, false, 0, 0),
			   "read(io, &after, 1 << 16 | 1) == Some((vec![0], Stamp { txn: 3, tuple: 1 << 16 | 1 }, None))");
	second = simc_begin();
	again = doltgres_scan_tests_put_op("again", UINT64CONST(2) << 16 | 9);
	doltgres_scan_tests_pushed(io, &held, seven, 4, second, 0, &again);
	any_taken = simc_snapshot(HOST_SNAPSHOT_KIND_ANY, false);
	any = simc_Taken_lent(&any_taken);
	stamp.txn = w;
	stamp.tuple = inserted;
	STD_ASSERT(2650, 13, doltgres_scan_tests_fetch_is(io, &store, &held, &fetched, &any, inserted, text,
													  sizeof(text) - 1, stamp, false, false, 0, 0),
			   "older == Some((b\"inserted\".to_vec(), inserted))");
	doltgres_scan_tests_ended(io, 2651, 37);
	simc_Taken_drop(&any_taken);
	simc_Taken_drop(&after_taken);
	simc_Taken_drop(&running_taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id:
 * step (5)'s tid fetch: a row an insert wrote fetches by the tuple id the
 * insert took, once its writer's commit is in the snapshot and not before; a
 * base row a committed delete ended fetches as none, and under a snapshot
 * taken while the delete ran it fetches with the deleter as its xmax; a base
 * row nothing changed fetches as it is.
 */
void
test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_fetch_reads_the_deltas_rows_and_the_bases_by_tuple_id_closure_0);
}

/*
 * <doltgres_scan_tests_Reversed as KeyComparator>::compare orders keys by
 * their first four bytes, largest first, so keys alike in those bytes
 * compare equal.
 */
static bool
doltgres_scan_tests_Reversed_KeyComparator_compare(void *self, HostIoHeld *held, const uint8 *a, size_t a_len,
												   const uint8 *b, size_t b_len, StdOrdering *result,
												   ChunksErrorError *error)
{
	std_range_to_check(4, b_len, RUST_FILE, 2661, 16);
	std_range_to_check(4, a_len, RUST_FILE, 2661, 28);
	*result = std_bytes_Ord_cmp(b, 4, a, 4);
	return true;
}

/* DOLTGRES_SCAN_TESTS_REVERSED_TYPE is Reversed's Any identity, a unit struct's. */
static const StdTypeId DOLTGRES_SCAN_TESTS_REVERSED_TYPE = {"doltgres::scan::tests::Reversed", 0, 1, NULL};

/* DOLTGRES_SCAN_TESTS_REVERSED_KEY_COMPARATOR_OPS is Reversed's KeyComparator. */
static const TreesKeyComparatorOps DOLTGRES_SCAN_TESTS_REVERSED_KEY_COMPARATOR_OPS = {
&DOLTGRES_SCAN_TESTS_REVERSED_TYPE, doltgres_scan_tests_Reversed_KeyComparator_compare};

/* test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 zero[] = {0, 0, 0, 0};
	static const uint8 one[] = {0, 0, 0, 1};
	static const uint8 two[] = {0, 0, 0, 2};
	static const uint8 three[] = {0, 0, 0, 3};
	static const uint8 five[] = {0, 0, 0, 5};
	static const uint8 tie[] = {0, 0, 0, 1, 0};
	static const uint8 one_tuple[] = {1};
	static const uint8 three_tuple[] = {3};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesKeyOrder order = {.tag = TREES_KEY_ORDER_COMPARE};
	TreesShape	shape = {0};
	TreesBuildBuilder builder;
	TreesStoreTree tree;
	HostXid		writer;
	TxnMemtableOp op;
	TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
	TxnBaseBase based;
	StdVec		layout;
	HostOwnedHandle handle;
	HostError	failed;
	StdVec		merged;
	StdVec		rows;
	StdVec		expected;
	uint64		w;

	order.u.compare.inner = std_arc_new(NULL, 0, 1);
	order.u.compare.ops = &DOLTGRES_SCAN_TESTS_REVERSED_KEY_COMPARATOR_OPS;
	trees_KeyOrder_Clone_clone(&order, &shape.order);
	trees_build_Builder_shaped(TREES_STORE_MEMORY_STORE_NODE_STORE(&store), 256, &shape, &builder);
	trees_KeyOrder_drop(&shape.order);
	for (uint32 n = 4; n > 0; n--)
	{
		StdVec		key = doltgres_scan_tests_be4(n - 1);
		StdVec		value;
		uint8		byte = (uint8) (n - 1);
		ChunksTrailerStamp stamp = {3, n};
		TreesBuildEntry entry;

		std_slice_to_vec_Checked(&value, &byte, 1, DOLTGRES_SCAN_BYTES);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		std_vec_push_Checked(&entry.stamps, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		doltgres_scan_tests_add(&builder, io, &entry, 2684, 109);
	}
	tree = doltgres_scan_tests_finished(&builder, io, 2686, 57);
	writer = simc_begin();
	op = doltgres_scan_tests_put_op("five", 100);
	doltgres_scan_tests_pushed(io, &held, five, 4, writer, 0, &op);
	op = doltgres_scan_tests_put_op("tie", 102);
	doltgres_scan_tests_pushed(io, &held, tie, 5, writer, 0, &op);
	op = doltgres_scan_tests_put_op("two", 101);
	doltgres_scan_tests_pushed(io, &held, two, 4, writer, 0, &op);
	doltgres_scan_tests_pushed(io, &held, zero, 4, writer, 0, &deleted);
	simc_commit(writer);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	based = doltgres_scan_tests_based(tree, NULL, 1);
	std_slice_to_vec_Checked(&layout, keyed, 1, DOLTGRES_SCAN_BYTES);
	if (!doltgres_scan_scan_begin(io, TREES_STORE_MEMORY_STORE_NODE_STORE(&store), 1, &based, 0, &layout, 0, &order,
								  &handle, &failed))
		STD_RESULT_EXPECT_FAILED(2697, 95, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	w = writer.f0;
	std_vec_new(&merged, sizeof(doltgres_scan_tests_shown_t), _Alignof(doltgres_scan_tests_shown_t));
	doltgres_scan_tests_shown_push(&merged, five, 4, (const uint8 *) "five", 4, 100, w, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, three, 4, three_tuple, 1, 4, 3, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, two, 4, (const uint8 *) "two", 3, 101, w, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, one, 4, one_tuple, 1, 2, 3, 0, false, 0, 0);
	doltgres_scan_tests_shown_push(&merged, tie, 5, (const uint8 *) "tie", 3, 102, w, 0, false, 0, 0);
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2706, 13, doltgres_scan_tests_shown_eq(&rows, &merged),
			   "shown(io, &store, &under, handle, Direction::Forward) == merged");
	doltgres_scan_tests_shown_drop(&rows);
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	expected = doltgres_scan_tests_reversed(&merged, merged.len);
	STD_ASSERT(2707, 13, doltgres_scan_tests_shown_eq(&rows, &expected),
			   "shown(io, &store, &under, handle, Direction::Backward) == reversed(&merged)");
	doltgres_scan_tests_shown_drop(&expected);
	doltgres_scan_tests_shown_drop(&rows);
	for (int i = 0; i < 3; i++)
		STD_ASSERT(2709, 17, doltgres_scan_tests_one(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, 2709,
													 78),
				   "next(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
	rows = doltgres_scan_tests_shown(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	expected = doltgres_scan_tests_reversed(&merged, 2);
	STD_ASSERT(2711, 13, doltgres_scan_tests_shown_eq(&rows, &expected),
			   "shown(io, &store, &under, handle, Direction::Backward) == reversed(&merged[..2])");
	doltgres_scan_tests_shown_drop(&expected);
	doltgres_scan_tests_shown_drop(&rows);
	doltgres_scan_tests_end(io, handle, 2712, 29);
	doltgres_scan_tests_shown_drop(&merged);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order:
 * step (5)'s decision 3 over a compared table: a read merges the base's rows
 * with a run of the delta's keys in the table's order, comparing with no
 * record lent, forward, backward, and across a turn; a key the comparator
 * finds equal to a base key but whose bytes differ is a group of its own
 * beside it, after it by its bytes; and a committed update and delete change
 * the base's rows in their places.
 */
void
test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_compared_scan_merges_the_deltas_keys_in_the_tables_order_closure_0);
}

/* doltgres_scan_tests_ids_is reports whether tuple ids are the ids expected, in order, and frees them. */
static bool
doltgres_scan_tests_ids_is(StdVec *ids, const uint64 *expected, size_t len)
{
	bool		same = ids->len == len;

	for (size_t i = 0; same && i < len; i++)
		same = STD_VEC_AT(ids, uint64, i) == expected[i];
	std_vec_free_Checked(ids, sizeof(uint64), _Alignof(uint64));
	return same;
}

/*
 * test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases_closure_0
 * is the case's actor.
 */
static void
test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 one[] = {0, 0, 0, 1};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesStoreTree tree = doltgres_scan_tests_stamped(io, &store, 2733, 22, 2735, 57);
	TreesStoreTree ids = doltgres_scan_tests_stamped_ids(io, &store);
	TxnInsertRoom room = doltgres_scan_tests_room(32);
	HostXid		writer = simc_begin();
	HostXid		running;
	TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
	uint64		inserted[2];
	uint64		unseen;
	uint64		forward[4];
	uint64		backward[4];
	uint64		turned[2];
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
	TxnBaseBase based;
	HostOwnedHandle handle;
	StdVec		read;

	for (uint32 n = 0; n < 2; n++)
	{
		uint8		byte = (uint8) n;
		TxnInsertInsert row = doltgres_scan_tests_insert(doltgres_scan_tests_be4(10 + n), &byte, 1, writer);

		inserted[n] = doltgres_scan_tests_inserted(io, &store, &held, &row, &room, 2759, 86);
	}
	doltgres_scan_tests_pushed(io, &held, one, 4, writer, 1, &deleted);
	simc_commit(writer);
	running = simc_begin();
	{
		uint8		byte = 2;
		TxnInsertInsert row = doltgres_scan_tests_insert(doltgres_scan_tests_be4(12), &byte, 1, running);

		unseen = doltgres_scan_tests_inserted(io, &store, &held, &row, &room, 2763, 81);
	}
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	based = doltgres_scan_tests_based(tree, &ids, UINT64CONST(2) << 16 | 1);
	handle = doltgres_scan_tests_begin(io, &store, 2, &based, keyed, 1, 0, 2768, 111);
	doltgres_scan_tests_set_range(io, handle, 1, UINT64CONST(1) << 16 | 3, 2769, 51);
	forward[0] = inserted[0];
	forward[1] = inserted[1];
	forward[2] = UINT64CONST(1) << 16 | 1;
	forward[3] = UINT64CONST(1) << 16 | 3;
	for (int i = 0; i < 4; i++)
		backward[i] = forward[3 - i];
	turned[0] = inserted[1];
	turned[1] = inserted[0];
	STD_ASSERT(2771, 13, unseen != forward[0] && unseen != forward[1] && unseen != forward[2] && unseen != forward[3],
			   "!forward.contains(&unseen)");
	read = doltgres_scan_tests_ranged(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2772, 13, doltgres_scan_tests_ids_is(&read, forward, 4),
			   "ranged(io, &store, &under, handle, Direction::Forward) == forward");
	read = doltgres_scan_tests_ranged(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2773, 13, doltgres_scan_tests_ids_is(&read, backward, 4),
			   "ranged(io, &store, &under, handle, Direction::Backward) == reversed(&forward)");
	for (int i = 0; i < 3; i++)
	{
		DoltgresScanRow row;

		STD_ASSERT(2775, 17, doltgres_scan_tests_in_range(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD,
														  &row, 2775, 87),
				   "next_in_range(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
		doltgres_scan_Row_drop(&row);
	}
	read = doltgres_scan_tests_ranged(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(2777, 13, doltgres_scan_tests_ids_is(&read, turned, 2),
			   "ranged(io, &store, &under, handle, Direction::Backward) == [inserted[1], inserted[0]]");
	doltgres_scan_tests_set_range(io, handle, UINT64CONST(1) << 16 | 2, UINT64CONST(1) << 16 | 2, 2778, 61);
	read = doltgres_scan_tests_ranged(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	STD_ASSERT(2779, 13, doltgres_scan_tests_none(&read),
			   "ranged(io, &store, &under, handle, Direction::Forward).is_empty()");
	doltgres_scan_tests_end(io, handle, 2780, 29);
	doltgres_scan_tests_ended(io, 2781, 37);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases:
 * step (5)'s tid range over the delta: a range read gives the base's rows and
 * the rows the delta's inserts wrote in tuple id order, forward and backward
 * and across a turn, passing over a base row a committed delete ended and a
 * row a running transaction inserted, and leaving out tuple ids past the
 * range.
 */
void
test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_tid_range_read_merges_the_deltas_tuple_ids_with_the_bases_closure_0);
}

/* doltgres_scan_tests_pair_push pushes a pair of a u32 key big-endian and a tuple id. */
static void
doltgres_scan_tests_pair_push(StdVec *pairs, uint32 n, uint64 tuple)
{
	doltgres_scan_tests_pair_t pair = {doltgres_scan_tests_be4(n), tuple};

	std_vec_push_Checked(pairs, &pair, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
}

/* doltgres_scan_tests_pairs_eq reports whether two cases' pairs are equal, as assert_eq! compares them. */
static bool
doltgres_scan_tests_pairs_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t i = 0; i < a->len; i++)
	{
		const doltgres_scan_tests_pair_t *x = &STD_VEC_AT(a, doltgres_scan_tests_pair_t, i);
		const doltgres_scan_tests_pair_t *y = &STD_VEC_AT(b, doltgres_scan_tests_pair_t, i);

		if (!doltgres_scan_tests_bytes_is(&x->bytes, y->bytes.ptr, y->bytes.len) || x->tuple != y->tuple)
			return false;
	}
	return true;
}

/* test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint32 puts[] = {0, 1, 5, 7, 11, 13};
	static const uint8 two[] = {0, 0, 0, 2};
	static const uint8 eight[] = {0, 0, 0, 8};
	static const uint64 order[] = {2, 0, 1};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TreesBuildBuilder builder;
	TreesStoreTree tree;
	HostXid		writer;
	TxnMemtableOp op;
	TxnMemtableOp deleted = {.tag = TXN_MEMTABLE_OP_DELETE};
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
	DoltgresScanShared shared;
	HostOwnedHandle handle;
	StdVec		rows;
	StdVec		expected;
	bool		same;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(&store), 256, &builder);
	for (uint32 n = 1; n < 7; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(2 * n);
		StdVec		value;
		uint8		byte = (uint8) n;
		ChunksTrailerStamp stamp = {3, n};
		TreesBuildEntry entry;

		std_slice_to_vec_Checked(&value, &byte, 1, DOLTGRES_SCAN_BYTES);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		std_vec_push_Checked(&entry.stamps, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		doltgres_scan_tests_add(&builder, io, &entry, 2802, 105);
	}
	tree = doltgres_scan_tests_finished(&builder, io, 2804, 57);
	writer = simc_begin();
	for (size_t i = 0; i < lengthof(puts); i++)
	{
		uint8		key[4] = {(uint8) (puts[i] >> 24), (uint8) (puts[i] >> 16), (uint8) (puts[i] >> 8),
		(uint8) puts[i]};
		uint8		byte = (uint8) puts[i];

		op.tag = TXN_MEMTABLE_OP_PUT;
		op.u.put = doltgres_scan_tests_put(&byte, 1, 100 + (uint64) puts[i]);
		doltgres_scan_tests_pushed(io, &held, key, 4, writer, 0, &op);
	}
	op = doltgres_scan_tests_put_op("two", 200);
	doltgres_scan_tests_pushed(io, &held, two, 4, writer, 0, &op);
	doltgres_scan_tests_pushed(io, &held, eight, 4, writer, 0, &deleted);
	simc_commit(writer);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	shared = doltgres_scan_tests_shared(tree, 6, 3);
	handle = doltgres_scan_tests_begin_parallel(io, &shared, keyed, 1, 2826, 82);
	rows = doltgres_scan_tests_read_units(io, &store, &under, handle, order, lengthof(order));
	std_slice_sort_by_TestsPair(rows.ptr, rows.len, doltgres_scan_tests_unit_order, NULL, NULL);
	std_vec_new(&expected, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
	for (size_t i = 0; i < lengthof(puts); i++)
		doltgres_scan_tests_pair_push(&expected, puts[i], 100 + (uint64) puts[i]);
	doltgres_scan_tests_pair_push(&expected, 2, 200);
	doltgres_scan_tests_pair_push(&expected, 4, 2);
	doltgres_scan_tests_pair_push(&expected, 6, 3);
	doltgres_scan_tests_pair_push(&expected, 10, 5);
	doltgres_scan_tests_pair_push(&expected, 12, 6);
	std_slice_sort_by_TestsPair(expected.ptr, expected.len, doltgres_scan_tests_unit_order, NULL, NULL);
	same = doltgres_scan_tests_pairs_eq(&rows, &expected);
	STD_ASSERT(2833, 13, same, "rows == expected");
	doltgres_scan_tests_end(io, handle, 2834, 29);
	doltgres_scan_tests_pairs_drop(&expected);
	doltgres_scan_tests_pairs_drop(&rows);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once:
 * step (5)'s decision 8, a parallel scan over base and delta: each unit
 * merges its base entries with the delta's keys at or past its first entry's
 * key and before the next unit's, unit 0 taking the delta's keys below every
 * unit's, so the units read in any order give each row the snapshot sees
 * once: a key only the delta holds, a base row a committed update replaced,
 * and none of a base row a committed delete ended.
 */
void
test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_parallel_scans_units_merge_the_delta_each_key_once_closure_0);
}

/* doltgres_scan_tests_split splits starts and reads back their split keys, which unwraps at a line and column. */
static StdVec
doltgres_scan_tests_split(const StdVec *starts, size_t len, uint32 line, uint32 col)
{
	StdVec		bytes;
	StdVec		keys;
	HostError	failed;

	doltgres_scan_splits(starts, len, &bytes);
	if (!doltgres_scan_split_keys(bytes.ptr, bytes.len, &keys, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	std_vec_free_Checked(&bytes, DOLTGRES_SCAN_BYTES);
	return keys;
}

/* doltgres_scan_tests_keys_eq reports whether split keys are the keys expected, in order. */
static bool
doltgres_scan_tests_keys_eq(const StdVec *keys, const StdVec *expected, size_t len)
{
	if (keys->len != len)
		return false;
	for (size_t i = 0; i < len; i++)
		if (!doltgres_scan_tests_bytes_is(&STD_VEC_AT(keys, StdVec, i), expected[i].ptr, expected[i].len))
			return false;
	return true;
}

/*
 * test_doltgres_scan_tests_a_leaders_split_keys_are_its_range_starts_spread_within_the_budget:
 * CommPF 7's ruling (b) on decision 8's empty base: the leader's split keys
 * are up to 64 of the delta's range starts, spread evenly, the empty first
 * start left out, each fitting the 16 KB budget's bytes left, so a key longer
 * than what is left is passed over and none fit when every start is longer
 * than the budget; they read back as written.
 */
void
test_doltgres_scan_tests_a_leaders_split_keys_are_its_range_starts_spread_within_the_budget(void)
{
	StdVec		starts[201];
	StdVec		expected[64];
	StdVec		longs[3];
	StdVec		fitting_expected[2];
	StdVec		picked;
	StdVec		fitting;
	StdVec		none;

	std_vec_new(&starts[0], DOLTGRES_SCAN_BYTES);
	for (uint32 n = 0; n < 200; n++)
		starts[n + 1] = doltgres_scan_tests_be4(n);
	picked = doltgres_scan_tests_split(starts, 201, 2845, 107);
	for (size_t i = 0; i < 64; i++)
		expected[i] = starts[1 + i * 200 / 64];
	STD_ASSERT(2847, 9, doltgres_scan_tests_keys_eq(&picked, expected, 64), "picked == expected");
	trees_VecVecU8_drop(&picked);
	for (size_t i = 0; i < lengthof(starts); i++)
		std_vec_free_Checked(&starts[i], DOLTGRES_SCAN_BYTES);
	std_vec_u8_from_elem(&longs[0], 1, 9000);
	std_vec_u8_from_elem(&longs[1], 2, 9000);
	std_vec_u8_from_elem(&longs[2], 3, 100);
	fitting = doltgres_scan_tests_split(longs, 3, 2849, 90);
	fitting_expected[0] = longs[0];
	fitting_expected[1] = longs[2];
	STD_ASSERT(2850, 9, doltgres_scan_tests_keys_eq(&fitting, fitting_expected, 2),
			   "fitting == [long(1, 9000), long(3, 100)]");
	trees_VecVecU8_drop(&fitting);
	for (size_t i = 0; i < lengthof(longs); i++)
		std_vec_free_Checked(&longs[i], DOLTGRES_SCAN_BYTES);
	std_vec_u8_from_elem(&longs[0], 1, 20000);
	std_vec_u8_from_elem(&longs[1], 2, 20000);
	none = doltgres_scan_tests_split(longs, 2, 2851, 72);
	STD_ASSERT(2851, 9, none.len == 0,
			   "split_keys(&splits(&[long(1, 20000), long(2, 20000)])).unwrap().is_empty()");
	trees_VecVecU8_drop(&none);
	std_vec_free_Checked(&longs[0], DOLTGRES_SCAN_BYTES);
	std_vec_free_Checked(&longs[1], DOLTGRES_SCAN_BYTES);
}

/* doltgres_scan_tests_range_starts returns the delta's range starts, which unwraps at a line and column. */
static StdVec
doltgres_scan_tests_range_starts(HostIoIo *io, const HostSharedHeld *held, uint32 line, uint32 col)
{
	StdVec		starts;
	HostError	failed;

	if (!txn_memtable_table_range_starts(io, host_shared_Held_record(held), &starts, &failed))
		STD_RESULT_EXPECT_FAILED(line, col, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	return starts;
}

/*
 * test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits_closure_0
 * is the case's actor.
 */
static void
test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	static const uint8 tuple[] = {1};
	static const uint64 unit_counts[] = {4, 3};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	HostXid		writer = simc_begin();
	uint32		count = 2 * 32 * 1024 + 100;
	uint32		first = 32 * 1024 + 50;
	TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_PUT};
	StdVec		starts;
	StdVec		leaders;
	StdVec		after;
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;

	for (uint32 n = 0; n < first; n++)
	{
		uint32		k = 2 * n;
		uint8		key[4] = {(uint8) (k >> 24), (uint8) (k >> 16), (uint8) (k >> 8), (uint8) k};

		op.u.put = doltgres_scan_tests_put(tuple, 1, 1 + (uint64) n);
		doltgres_scan_tests_pushed(io, &held, key, 4, writer, 0, &op);
	}
	starts = doltgres_scan_tests_range_starts(io, &held, 2872, 65);
	STD_ASSERT_MSG(2873, 13, starts.len > 1, "the delta's first range didn't split");
	leaders = doltgres_scan_tests_split(starts.ptr, starts.len, 2874, 56);
	for (uint32 n = first; n < count; n++)
	{
		uint32		k = 2 * (n - first) + 1;
		uint8		key[4] = {(uint8) (k >> 24), (uint8) (k >> 16), (uint8) (k >> 8), (uint8) k};

		op.u.put = doltgres_scan_tests_put(tuple, 1, 1 + (uint64) n);
		doltgres_scan_tests_pushed(io, &held, key, 4, writer, 0, &op);
	}
	simc_commit(writer);
	after = doltgres_scan_tests_range_starts(io, &held, 2880, 56);
	STD_ASSERT_MSG(2879, 13, after.len > starts.len, "no split after the leader's");
	trees_VecVecU8_drop(&after);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	for (size_t c = 0; c < lengthof(unit_counts); c++)
	{
		uint64		units = unit_counts[c];
		DoltgresScanShared shared = {.units = units};
		HostOwnedHandle handle;
		uint64	   *order = palloc(sizeof(uint64) * units);
		StdVec		rows;
		bool		same;

		if (c == 0)
		{
			std_vec_new(&shared.splits, sizeof(StdVec), _Alignof(StdVec));
			for (size_t i = 0; i < leaders.len; i++)
			{
				const StdVec *leader = &STD_VEC_AT(&leaders, StdVec, i);
				StdVec		copy;

				std_slice_to_vec_Checked(&copy, leader->ptr, leader->len, DOLTGRES_SCAN_BYTES);
				std_vec_push_Checked(&shared.splits, &copy, sizeof(StdVec), _Alignof(StdVec));
			}
		}
		else
			std_vec_new(&shared.splits, sizeof(StdVec), _Alignof(StdVec));
		doltgres_scan_Epochs_Default_default(&shared.epochs);
		handle = doltgres_scan_tests_begin_parallel(io, &shared, keyed, 1, 2897, 86);
		for (uint64 i = 0; i < units; i++)
			order[i] = units - 1 - i;
		rows = doltgres_scan_tests_read_units(io, &store, &under, handle, order, units);
		pfree(order);
		for (size_t i = 0; i < rows.len; i++)
			std_vec_free_Checked(&STD_VEC_AT(&rows, doltgres_scan_tests_pair_t, i).bytes, DOLTGRES_SCAN_BYTES);
		for (size_t i = 0; i < rows.len; i++)
			STD_VEC_AT(&rows, uint64, i) = STD_VEC_AT(&rows, doltgres_scan_tests_pair_t, i).tuple;
		std_slice_sort_unstable_U64(rows.ptr, rows.len);
		same = rows.len == count;
		for (uint32 i = 0; same && i < count; i++)
			same = STD_VEC_AT(&rows, uint64, i) == (uint64) i + 1;
		std_vec_free_Checked(&rows, sizeof(doltgres_scan_tests_pair_t), _Alignof(doltgres_scan_tests_pair_t));
		STD_ASSERT(2903, 17, same, "{units} units: ids == (1..=u64::from(count)).collect::<Vec<_>>()");
		doltgres_scan_tests_end(io, handle, 2904, 33);
	}
	trees_VecVecU8_drop(&leaders);
	trees_VecVecU8_drop(&starts);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits:
 * CommPF 7's ruling (b): a parallel scan over an empty base divides the
 * delta's keys among its units by the split keys the leader read at the
 * scan's initialization, so each key is read once even after the delta's
 * ranges split again before the units are read, and with no split key unit
 * 0 reads every key.
 */
void
test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_parallel_scan_over_an_empty_base_reads_each_delta_key_once_by_the_leaders_splits_closure_0);
}

/* test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	TxnInsertRoom room = doltgres_scan_tests_room(1032);
	HostXid		writer = simc_begin();
	uint64		tuple_ids[280];
	uint64		most = 0;
	uint64		blocks;
	uint64		before;
	uint64		holds;
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under;
	HostOwnedHandle handle;
	StdVec		read;
	bool		same;

	for (uint32 n = 0; n < 280; n++)
	{
		uint8		byte = (uint8) n;
		TxnInsertInsert row = doltgres_scan_tests_insert(doltgres_scan_tests_be4(n), &byte, 1, writer);

		tuple_ids[n] = doltgres_scan_tests_inserted(io, &store, &held, &row, &room, 2941, 64);
	}
	simc_commit(writer);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	under.has_serial = false;
	for (size_t i = 0; i < lengthof(tuple_ids); i++)
		most = Max(most, tuple_ids[i]);
	blocks = most >> 16;
	handle = doltgres_scan_tests_begin(io, &store, STD_ADD(u64, blocks, 1, 2949, 44), NULL, keyed, 1, 0, 2949, 91);
	before = simc_delta_holds();
	std_vec_new(&read, sizeof(uint64), _Alignof(uint64));
	for (uint64 block = 0; block <= blocks; block++)
	{
		StdVec		ranged;

		doltgres_scan_tests_set_range(io, handle, block << 16 | 1, block << 16 | 0xffff, 2953, 78);
		ranged = doltgres_scan_tests_ranged(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
		for (size_t i = 0; i < ranged.len; i++)
			std_vec_push_Checked(&read, &STD_VEC_AT(&ranged, uint64, i), sizeof(uint64), _Alignof(uint64));
		std_vec_free_Checked(&ranged, sizeof(uint64), _Alignof(uint64));
	}
	holds = STD_SUB(u64, simc_delta_holds(), before, 2956, 25);
	same = blocks == 39 && read.len == lengthof(tuple_ids);
	for (size_t i = 0; same && i < read.len; i++)
		same = STD_VEC_AT(&read, uint64, i) == tuple_ids[i];
	std_vec_free_Checked(&read, sizeof(uint64), _Alignof(uint64));
	STD_ASSERT(2957, 13, same, "(blocks, read) == (39, tuple_ids)");
	STD_ASSERT(2958, 13, holds == (280 + 1) + 2 * 280, "holds == (280 + 1) + 2 * 280");
	doltgres_scan_tests_end(io, handle, 2959, 29);
	doltgres_scan_tests_ended(io, 2960, 37);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once: bugs
 * row 123: ANALYZE's sample reads a tid range a block at a time, and the
 * scan walks the delta's tuple ids once for every range it reads, not once a
 * block, so the holds the reads take grow with the rows, not with the rows
 * times the blocks.
 */
void
test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_sample_of_many_blocks_walks_the_delta_once_closure_0);
}

/* test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostOwnedHandle handle = doltgres_scan_tests_begin(io, &store, 0, NULL, keyed, 1, 0, __LINE__, 5);
	HostError	failed;
	bool		has = true;
	uint32		block = 0x5EED;
	uint16		most = 0x5EED;
	uint64		first = 0x5EED;
	uint64		last = 0x5EED;

	if (!doltgres_scan_sample_page(io, handle, &has, &block, &most, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !has && block == 0x5EED && most == 0x5EED,
			   "a scan with no sample answers none and leaves its block and offset as they were");
	has = true;
	if (!doltgres_scan_range(io, handle, &has, &first, &last, &failed))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !has && first == 0x5EED && last == 0x5EED,
			   "a scan with no tid range answers none and leaves its first and last as they were");
	doltgres_scan_tests_end(io, handle, __LINE__, 5);
	trees_store_MemoryStore_drop(&store);
}

/*
 * test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were:
 * a scan that holds no sample page and no tid range answers none for each
 * and leaves its caller's values as the caller had them (bugs row 288).
 */
void
test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_scan_with_no_sample_or_range_leaves_its_answers_as_they_were_closure_0);
}

/*
 * test_doltgres_scan_tests_a_scan_moves_to_a_newer_base_at_its_next_read_and_gives_every_row_once_closure_0
 * is the case's actor.
 */
static void
test_doltgres_scan_tests_a_scan_moves_to_a_newer_base_at_its_next_read_and_gives_every_row_once_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	static const uint8 keyed[] = {1};
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	HostSharedHeld held = doltgres_scan_tests_made(io);
	HostSharedRecord state = host_shared_Held_record(&held);
	TreesStoreTree older = doltgres_scan_tests_base(io, &store, 4);
	TreesStoreTree newer = doltgres_scan_tests_base(io, &store, 8);
	HostXid		writer = simc_begin();
	TxnStatusOutcome committed = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 100};
	TxnStatusRoomed roomed;
	bool		took;
	SimcTaken	after_taken;
	HostSnapshot after;
	TxnBaseBase based;
	TxnBaseBase installed;
	uint64		generation;
	HostOwnedHandle handle;
	DoltgresScanUnder under;
	StdVec		rows;
	StdVec		rest;
	HostError	error;

	for (uint32 n = 4; n < 8; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(n);
		StdVec		tuple = doltgres_scan_tests_le4(n * n);
		TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_PUT};

		op.u.put = doltgres_scan_tests_put(tuple.ptr, tuple.len, (uint64) n + 1);
		doltgres_scan_tests_pushed(io, &held, key.ptr, key.len, writer, 0, &op);
		std_vec_free_Checked(&tuple, DOLTGRES_SCAN_BYTES);
		std_vec_free_Checked(&key, DOLTGRES_SCAN_BYTES);
	}
	if (!txn_status_take(io, writer, committed, &roomed, &took, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	simc_commit(writer);
	after_taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	after = simc_Taken_lent(&after_taken);
	generation = txn_state_generation(state);
	based = doltgres_scan_tests_based(older, NULL, 1);
	handle = doltgres_scan_tests_begin(io, &store, 1, &based, keyed, 1, 0, __LINE__, 0);
	under.state = state;
	under.snapshot = &after;
	under.has_serial = false;
	std_vec_new(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	for (int i = 0; i < 6; i++)
	{
		DoltgresScanRow row;

		STD_ASSERT(__LINE__, 0,
				   doltgres_scan_tests_next(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, &row,
											__LINE__, 0),
				   "next(io, &store, &under, handle, Direction::Forward).unwrap().is_some()");
		std_vec_push_Checked(&rows, &row, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	}
	installed = doltgres_scan_tests_based(newer, NULL, 1);
	installed.folded = 200;
	if (!txn_state_installed(HOST_IO_IO_TAKES_SHORT(io), &held, generation, &installed, NULL, NULL, &took, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, took, "state::installed(io, &held, generation, &installed).unwrap()");
	for (uint32 n = 4; n < 8; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(n);
		StdVec		versions;
		StdVec		handles;
		StdVec		removed;
		HostDeltaHold hold;

		if (!host_delta_hold(io, &hold, &error) ||
			!txn_memtable_table_versions(io, &hold, state, key.ptr, key.len, &versions, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
		std_vec_new(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
		for (size_t v = 0; v < versions.len; v++)
			std_vec_push_Checked(&handles, &STD_VEC_AT(&versions, TxnMemtableVersionVersion, v).handle,
								 sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
		txn_memtable_version_versions_drop(&versions, 0);
		host_delta_Hold_drop(&hold);
		if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, state, key.ptr, key.len, handles.ptr,
										handles.len, &removed, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
		std_vec_free_Checked(&removed, sizeof(bool), _Alignof(bool));
		std_vec_free_Checked(&handles, sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle));
		std_vec_free_Checked(&key, DOLTGRES_SCAN_BYTES);
	}
	rest = doltgres_scan_tests_read(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD);
	for (size_t i = 0; i < rest.len; i++)
		std_vec_push_Checked(&rows, &STD_VEC_AT(&rest, DoltgresScanRow, i), sizeof(DoltgresScanRow),
							 _Alignof(DoltgresScanRow));
	std_vec_free_Checked(&rest, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	STD_ASSERT(__LINE__, 0, doltgres_scan_tests_expected(&rows, 0, 8, false), "every row once across the install");
	doltgres_scan_tests_rows_drop(&rows);
	rows = doltgres_scan_tests_read(io, &store, &under, handle, DOLTGRES_SCAN_DIRECTION_BACKWARD);
	STD_ASSERT(__LINE__, 0, doltgres_scan_tests_expected(&rows, 0, 8, true),
			   "read(io, &store, &under, handle, Direction::Backward) == reversed(&expected(8))");
	doltgres_scan_tests_rows_drop(&rows);
	doltgres_scan_tests_end(io, handle, __LINE__, 0);
	simc_Taken_drop(&after_taken);
	host_shared_Held_drop(&held);
	trees_store_MemoryStore_drop(&store);
}

/*
 * Bugs rows 240 and 241: a scan that has read part of its base and of the
 * delta when an install gives the storage a newer base holding the delta's
 * committed rows, and a drop takes them out of the delta, moves to the newer
 * base at its next read and goes on past its group's key, so it gives every
 * row once, and its read back after gives them all once the other way. A
 * C-only case.
 */
void
test_doltgres_scan_tests_a_scan_moves_to_a_newer_base_at_its_next_read_and_gives_every_row_once(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_a_scan_moves_to_a_newer_base_at_its_next_read_and_gives_every_row_once_closure_0);
}

/* DOLTGRES_SCAN_TESTS_HELD_DATABASE is the held reader case's database. */
#define DOLTGRES_SCAN_TESTS_HELD_DATABASE 9631

/*
 * doltgres_scan_tests_held_t is the held reader case's actors' captures: the
 * store, the older and newer bases' trees, the base generation the reader
 * takes its base at, and the rows the reader read.
 */
typedef struct doltgres_scan_tests_held_t
{
	TreesStoreMemoryStore *store;
	TreesStoreTree older;
	TreesStoreTree newer;
	uint64		generation;
	StdVec	   *rows;
} doltgres_scan_tests_held_t;

/* doltgres_scan_tests_held_storage returns the held reader case's storage. */
static HostStorage
doltgres_scan_tests_held_storage(void)
{
	HostStorage storage = {HOST_PG_DEFAULT, DOLTGRES_SCAN_TESTS_HELD_DATABASE, 16384};

	return storage;
}

/* doltgres_scan_tests_held_state opens the held reader case's repository and its storage's state. */
static bool
doltgres_scan_tests_held_state(HostIoIo *io, HostSharedHeld *result, HostError *error)
{
	HostStorage storage = doltgres_scan_tests_held_storage();
	RepoRepo	repo;
	HostSharedReader reading;
	ChunksErrorError failed;
	bool		ok;

	if (!repo_Repo_open(io, DOLTGRES_SCAN_TESTS_HELD_DATABASE, HOST_PG_DEFAULT, &repo, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	ok = host_shared_read(io, &reading, error);
	if (ok)
	{
		ok = txn_state_state(io, &repo, &reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, result,
							 error);
		host_shared_Reader_drop(&reading);
	}
	repo_Repo_drop(&repo);
	return ok;
}

/* doltgres_scan_tests_held_reader is the held reader case's reader: it reads the scan of the older base to its end. */
static bool
doltgres_scan_tests_held_reader(void *captures, HostIoIo *io, HostError *error)
{
	const doltgres_scan_tests_held_t *c = captures;
	static const uint8 keyed[] = {1};
	TxnBaseBase based = doltgres_scan_tests_based(c->older, NULL, 1);
	DoltgresScanStores stores = doltgres_scan_tests_stores(c->store);
	DoltgresScanLayout layout = doltgres_scan_tests_layout(keyed, 1, 0);
	HostSharedHeld held;
	SimcTaken	taken;
	HostSnapshot snapshot;
	DoltgresScanUnder under = {.has_serial = false};
	HostOwnedHandle handle;
	bool		ok;

	if (!doltgres_scan_tests_held_state(io, &held, error))
	{
		doltgres_scan_Layout_drop(&layout);
		return false;
	}
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	snapshot = simc_Taken_lent(&taken);
	under.state = host_shared_Held_record(&held);
	under.snapshot = &snapshot;
	ok = doltgres_scan_scan_begin(io, TREES_STORE_MEMORY_STORE_NODE_STORE(c->store), 1, &based, c->generation,
								  &layout.keyed, 0, &layout.order, &handle, error);
	for (;;)
	{
		DoltgresScanRow row;
		bool		has;

		if (!ok)
			break;
		ok = doltgres_scan_next(io, &stores, &under, handle, DOLTGRES_SCAN_DIRECTION_FORWARD, &has, &row, error);
		if (!ok || !has)
			break;
		std_vec_push_Checked(c->rows, &row, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	}
	ok = ok && doltgres_scan_scan_end(io, handle, error);
	simc_Taken_drop(&taken);
	host_shared_Held_drop(&held);
	return ok;
}

/* doltgres_scan_tests_held_folder is the held reader case's folder: it installs the newer base and drops the spills. */
static bool
doltgres_scan_tests_held_folder(void *captures, HostIoIo *io, HostError *error)
{
	const doltgres_scan_tests_held_t *c = captures;
	TxnBaseBase installed = doltgres_scan_tests_based(c->newer, NULL, 1);
	HostSharedHeld held;
	bool		took = false;
	bool		ok;

	installed.folded = 200;
	if (!doltgres_scan_tests_held_state(io, &held, error))
		return false;
	ok = txn_state_installed(HOST_IO_IO_TAKES_SHORT(io), &held, c->generation, &installed, NULL, NULL, &took, error);
	STD_ASSERT(__LINE__, 0, !ok || took, "state::installed(io, &held, generation, &installed)?");
	ok = ok && txn_spill_folded(io, host_shared_Held_record(&held), 200, error);
	host_shared_Held_drop(&held);
	return ok;
}

/*
 * Bugs rows 240 and 241's check after the delta read: a scan's first read,
 * held inside its read of the delta before it reads the spill files that
 * hold the delta's committed rows, all below its base's first entry, while
 * an install gives the storage a newer base holding them and a drop takes
 * their files out, finds once it read the delta that the base moved, moves
 * to the newer base, and gives every row once; with the check before the
 * delta read it pairs its base with the dropped delta and misses them. A
 * C-only case.
 */
void
test_doltgres_scan_tests_a_scan_held_inside_its_delta_read_across_an_install_and_drop_gives_every_row_once(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostStorage storage = doltgres_scan_tests_held_storage();
	TreesStoreMemoryStore store = trees_store_MemoryStore_new();
	StdVec		rows;
	doltgres_scan_tests_held_t captures = {.store = &store, .rows = &rows};
	SimcActor	reader = {doltgres_scan_tests_held_reader, &captures};
	SimcActor	folder = {doltgres_scan_tests_held_folder, &captures};
	SimcStep	steps[2] = {{"folder", 6, PG_UINT64_MAX}, {"reader", 6, PG_UINT64_MAX}};
	SimcEnding	returned = {.tag = SIMC_ENDING_RETURNED};
	TxnStatusOutcome committed = {.tag = TXN_STATUS_OUTCOME_COMMITTED, .u.committed = 100};
	TxnStatusRoomed roomed;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedHeld held;
	HostXid		writer;
	ChunksErrorError created;
	HostError	error;
	SimcRun		run;
	SimcReport	report;
	bool		spilled;
	bool		took;
	bool		has_count;
	uint64		count;

	captures.older = doltgres_scan_tests_built(io, &store, 8, 12, __LINE__, 0, __LINE__, 0);
	captures.newer = doltgres_scan_tests_built(io, &store, 0, 12, __LINE__, 0, __LINE__, 0);
	if (!repo_Repo_create(io, DOLTGRES_SCAN_TESTS_HELD_DATABASE, HOST_PG_DEFAULT, &repo, &created))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &created, chunks_error_Error_Debug_fmt);
	if (!host_shared_read(io, &reading, &error) ||
		!txn_state_state(io, &repo, &reading, &storage, TXN_STATE_PERSISTENCE_PERMANENT, false, NULL, &held, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	writer = simc_begin();
	for (uint32 n = 0; n < 8; n++)
	{
		StdVec		key = doltgres_scan_tests_be4(n);
		StdVec		tuple = doltgres_scan_tests_le4(n * n);
		TxnMemtableOp op = {.tag = TXN_MEMTABLE_OP_PUT};

		op.u.put = doltgres_scan_tests_put(tuple.ptr, tuple.len, (uint64) n + 1);
		doltgres_scan_tests_pushed(io, &held, key.ptr, key.len, writer, 0, &op);
		std_vec_free_Checked(&tuple, DOLTGRES_SCAN_BYTES);
		std_vec_free_Checked(&key, DOLTGRES_SCAN_BYTES);
	}
	if (!txn_spill_spilled(io, &storage, TXN_WRITTEN_OF_ROWS, writer.f0, &spilled, &error) ||
		!txn_status_take(io, writer, committed, &roomed, &took, &error) ||
		!txn_status_add(io, writer, 8, &has_count, &count, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 0, DOLTGRES_SCAN_TESTS_UNWRAP, &error, host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 0, spilled, "spill::spilled(io, &storage, Of::Rows, writer.0).unwrap()");
	simc_commit(writer);
	captures.generation = txn_state_generation(host_shared_Held_record(&held));
	host_shared_Held_drop(&held);
	host_shared_Reader_drop(&reading);
	repo_Repo_drop(&repo);
	std_vec_new(&rows, sizeof(DoltgresScanRow), _Alignof(DoltgresScanRow));
	run = simc_Run_new(5);
	simc_Run_actor(&run, "reader", 6, reader);
	simc_Run_actor(&run, "folder", 6, folder);
	simc_Run_until(&run, "reader", 6, "doltgres-spill-slots-read");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 0, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ended");
	for (size_t k = 0; k < report.outcome.u.ended.len; k++)
		STD_ASSERT(__LINE__, 0,
				   simc_Ending_PartialEq_eq(&STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, k).f1,
											&returned),
				   "every actor returned Ok(())");
	STD_ASSERT(__LINE__, 0, doltgres_scan_tests_expected(&rows, 0, 12, false),
			   "every row once across the install and drop");
	doltgres_scan_tests_rows_drop(&rows);
	simc_Report_drop(&report);
	trees_store_MemoryStore_drop(&store);
	simc_TestIo_drop(&test_io);
}

/* DOLTGRES_SCAN_TESTS_SPREAD is how many rows the memo case spreads over its four keyless entries. */
#define DOLTGRES_SCAN_TESTS_SPREAD 4000

/*
 * doltgres_scan_tests_spread builds a keyless table's base of four entries
 * whose copies take tuple ids in turn, as a statement inserting four values
 * in turn writes them, so that each copy is a run of its own, the copy of
 * tuple id t of transaction txn plus t / 1000, each entry's row the key's
 * byte plus a shift, with its tuple id tree.
 */
static TxnBaseBase
doltgres_scan_tests_spread(HostIoIo *io, TreesStoreMemoryStore *store, uint64 txn, uint8 shift)
{
	static uint64 tuple_ids[DOLTGRES_SCAN_TESTS_SPREAD];
	static StdVec keys[DOLTGRES_SCAN_TESTS_SPREAD];
	TreesBuildBuilder builder;
	TreesStoreTree tree;
	TreesStoreTree ids;

	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(store), 256, &builder);
	for (uint8 byte = 1; byte <= 4; byte++)
	{
		StdVec		key;
		StdVec		value;
		TreesBuildEntry entry;
		uint8		row = byte + shift;

		std_vec_u8_from_elem(&key, byte, 16);
		txn_counted_keyless_value(DOLTGRES_SCAN_TESTS_SPREAD / 4, &row, 1, &value);
		trees_build_Entry_new_VecU8VecU8(&key, &value, &entry);
		for (uint64 tuple = byte; tuple <= DOLTGRES_SCAN_TESTS_SPREAD; tuple += 4)
		{
			ChunksTrailerStamp stamp = {txn + tuple / 1000, tuple};

			std_vec_push_Checked(&entry.stamps, &stamp, sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp));
		}
		doltgres_scan_tests_add(&builder, io, &entry, __LINE__, 9);
	}
	tree = doltgres_scan_tests_finished(&builder, io, __LINE__, 9);
	for (size_t i = 0; i < DOLTGRES_SCAN_TESTS_SPREAD; i++)
	{
		tuple_ids[i] = i + 1;
		std_vec_u8_from_elem(&keys[i], (uint8) (i % 4 + 1), 16);
	}
	ids = doltgres_scan_tests_pairs(io, store, tuple_ids, keys, DOLTGRES_SCAN_TESTS_SPREAD);
	return doltgres_scan_tests_based(tree, &ids, 1);
}

/* doltgres_scan_tests_memo_fetch fetches a tuple id of a spread base through a memo, asserting its row's key, row, and stamp at a line. */
static void
doltgres_scan_tests_memo_fetch(HostIoIo *io, doltgres_scan_tests_under_t *u, const TxnBaseBase *base,
							   DoltgresScanEpochs *epochs, DoltgresScanResolved *resolved, uint64 tuple, uint64 txn,
							   uint8 shift, uint32 line)
{
	DoltgresScanLayout layout = doltgres_scan_tests_layout(NULL, 0, 0);
	uint8		key[16];
	uint8		byte = (uint8) ((tuple - 1) % 4 + 1);
	uint8		expected = byte + shift;
	DoltgresScanRow row;
	HostError	failed;
	bool		has;
	DoltgresScanTrees trees;

	memset(key, byte, sizeof(key));
	trees = doltgres_scan_tests_trees(base);
	if (!doltgres_scan_fetch(io, TREES_STORE_MEMORY_STORE_NODE_STORE(&u->store), &u->under, epochs, resolved, &trees,
							 &layout, tuple, &has, &row, &failed))
		STD_RESULT_EXPECT_FAILED(line, 13, DOLTGRES_SCAN_TESTS_UNWRAP, &failed, host_Error_Debug_fmt);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(line, 13);
	STD_ASSERT(line, 9,
			   doltgres_scan_tests_bytes_is(&row.key, key, sizeof(key)) &&
			   doltgres_scan_tests_bytes_is(&row.tuple, &expected, 1) && row.stamp.tuple == tuple &&
			   row.stamp.txn == txn + tuple / 1000,
			   "(row.key, row.tuple, row.stamp) == (vec![byte; 16], vec![byte + shift], (txn + tuple / 1000, tuple))");
	doltgres_scan_Row_drop(&row);
}

/* test_doltgres_scan_tests_fetches_through_one_memo_give_each_keyless_copys_own_stamp_closure_0 is the case's actor. */
static void
test_doltgres_scan_tests_fetches_through_one_memo_give_each_keyless_copys_own_stamp_closure_0(void *captures)
{
	HostIoIo   *io = ((doltgres_scan_tests_case_t *) captures)->io;
	doltgres_scan_tests_under_t u;
	DoltgresScanEpochs epochs;
	DoltgresScanResolved resolved = {0};
	TxnBaseBase first;
	TxnBaseBase second;

	doltgres_scan_tests_under(io, &u);
	doltgres_scan_Epochs_Default_default(&epochs);
	first = doltgres_scan_tests_spread(io, &u.store, 100, 0);
	for (uint64 tuple = 1; tuple <= DOLTGRES_SCAN_TESTS_SPREAD; tuple++)
		doltgres_scan_tests_memo_fetch(io, &u, &first, &epochs, &resolved, tuple, 100, 0, __LINE__);
	second = doltgres_scan_tests_spread(io, &u.store, 900, 10);
	for (uint64 tuple = 1; tuple <= DOLTGRES_SCAN_TESTS_SPREAD; tuple += 333)
		doltgres_scan_tests_memo_fetch(io, &u, &second, &epochs, &resolved, tuple, 900, 10, __LINE__);
	doltgres_scan_tests_memo_fetch(io, &u, &first, &epochs, &resolved, 7, 100, 0, __LINE__);
	doltgres_scan_Resolved_drop(&resolved);
	doltgres_scan_Epochs_drop(&epochs);
	doltgres_scan_tests_under_drop(&u);
}

/*
 * test_doltgres_scan_tests_fetches_through_one_memo_give_each_keyless_copys_own_stamp:
 * bugs row 346: fetches of every tuple id of a keyless table whose copies
 * take tuple ids in turn, through one memo of the leaf they last resolved a
 * row in, each give the copy's own key and stamp; a second base of the same
 * keys, other rows and other transactions' copies, fetched through the same
 * memo, gives its own rows and stamps, never the first base's kept leaf's,
 * and the first again its own.
 */
void
test_doltgres_scan_tests_fetches_through_one_memo_give_each_keyless_copys_own_stamp(void)
{
	doltgres_scan_tests_run(test_doltgres_scan_tests_fetches_through_one_memo_give_each_keyless_copys_own_stamp_closure_0);
}

#endif							/* DOLTGRES_TEST_DOLTGRES */

#endif							/* FRONTEND */
