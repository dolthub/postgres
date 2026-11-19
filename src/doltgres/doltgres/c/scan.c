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
} DoltgresScanTurnTag;

/*
 * DoltgresScanTurn is Turn: what a turn of a read in the scan's record
 * leaves it, a row, another turn, the end, a compared table's run to make
 * by its order, or two candidates to compare by it.
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
 * tid range unit's first and last tuple ids, and the delta's split keys (a
 * Vec<Vec<u8>>).
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
	std_vec_free_Checked(&scan->keyed, DOLTGRES_SCAN_BYTES);
	trees_KeyOrder_drop(&scan->order);
	doltgres_scan_option_drop(&scan->bound);
	doltgres_scan_option_drop(&scan->seek);
	if (scan->has_parallel)
		doltgres_scan_vecs_drop(&scan->parallel.splits);
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
	*result_0 = page.f0;
	*result_1 = page.f1;
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
	scan.folded = base == NULL ? 0 : base->folded;
	scan.epochs = frame->epochs;
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
 * whose base, or no base (NULL), is in a table of a keyed layout, an object
 * id, and a tree's order, which it takes, and answers its handle; its first
 * read gives the storage's first row its snapshot sees.
 */
bool
doltgres_scan_scan_begin(HostIoIo *io, TreesStoreNodeStore store, uint64 blocks, const TxnBaseBase *base,
						 StdVec *keyed, uint32 table, TreesKeyOrder *order, HostOwnedHandle *result,
						 HostError *error)
{
	doltgres_scan_begin_frame_t frame = {.io = io, .store = store, .blocks = blocks, .base = base, .keyed = *keyed,
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
 * group's key it moves from, the base's entry, the delta's hold, the key a
 * seek left, and the delta's key and versions, each while live.
 */
typedef struct doltgres_scan_advance_frame_t
{
	HostIoIo   *io;
	const DoltgresScanStores *stores;
	const DoltgresScanUnder *under;
	DoltgresScanScan *scan;
	DoltgresScanDirection way;
	doltgres_OptionVecU8_t from;
	doltgres_scan_base_read_t entry;
	HostDeltaHold hold;
	bool		hold_live;
	doltgres_OptionVecU8_t at;
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
	doltgres_scan_option_drop(&frame->at);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->hold_live = false;
	doltgres_scan_entry_drop(&frame->entry);
	doltgres_scan_option_drop(&frame->from);
}

/* doltgres_scan_advance_body finds the nearer of the base's next entry and the delta's next key and makes its group. */
static void
doltgres_scan_advance_body(void *frame_)
{
	doltgres_scan_advance_frame_t *frame = frame_;
	DoltgresScanScan *scan = frame->scan;
	DoltgresScanDirection way = frame->way;
	bool		entries = scan->index && !scan->records;
	StdVec		key;
	bool		has_copies = false;
	DoltgresScanCopies copies;
	StdVec		versions;

	if (scan->has_group)
	{
		frame->from.has = true;
		frame->from.v = scan->group.key;
		std_vec_free_Checked(&scan->group.base, DOLTGRES_SCAN_BYTES);
		std_ptr_drop_in_place_slice(scan->group.rows.ptr, scan->group.rows.len, sizeof(DoltgresScanFound),
									doltgres_scan_Found_drop);
		std_vec_free_Checked(&scan->group.rows, DOLTGRES_SCAN_FOUND);
		scan->has_group = false;
	}
	frame->entry.scan = scan;
	frame->entry.keyless = scan->keyed.len == 0;
	frame->entry.entries = entries ? &scan->order : NULL;
	frame->entry.way = way;
	frame->entry.error = frame->error;
	frame->stores->ops->with(frame->stores->self, frame->io, &scan->reader, doltgres_scan_advance_closure_1,
							 &frame->entry);
	if (!frame->entry.ok)
		return;
	if (!frame->from.has && entries && way == DOLTGRES_SCAN_DIRECTION_BACKWARD)
	{
		uint8		record = TXN_ENTRY_RECORD;

		std_slice_to_vec_Checked(&frame->from.v, &record, 1, DOLTGRES_SCAN_BYTES);
		frame->from.has = true;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (scan->seek.has)
	{
		frame->at = scan->seek;
		scan->seek.has = false;
		if (!txn_visible_versions(frame->io, &frame->hold, frame->under->state, frame->at.v.ptr, frame->at.v.len,
								  scan->folded, &versions, frame->error))
			return;
		if (versions.len == 0)
		{
			txn_memtable_version_versions_drop(&versions, 0);
			if (!txn_visible_next(frame->io, &frame->hold, frame->under->state, true, frame->at.v.ptr,
								  frame->at.v.len, doltgres_scan_toward(way), scan->folded, &frame->has_delta,
								  &frame->delta, frame->error))
				return;
			doltgres_scan_option_drop(&frame->at);
		}
		else
		{
			frame->delta.f0 = frame->at.v;
			frame->delta.f1 = versions;
			frame->at.has = false;
			frame->has_delta = true;
		}
	}
	else if (!txn_visible_next(frame->io, &frame->hold, frame->under->state, frame->from.has, frame->from.v.ptr,
							   frame->from.has ? frame->from.v.len : 0, doltgres_scan_toward(way), scan->folded,
							   &frame->has_delta, &frame->delta, frame->error))
		return;
	if (frame->has_delta && entries && frame->delta.f0.len > 0 &&
		((const uint8 *) frame->delta.f0.ptr)[0] == TXN_ENTRY_RECORD)
		doltgres_scan_delta_drop(&frame->has_delta, &frame->delta);
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
 * scan starts over.
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
 * doltgres_scan_chosen_frame_t is chosen's state across its pad: the choice,
 * the delta's hold, and the key's versions, while live.
 */
typedef struct doltgres_scan_chosen_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	DoltgresScanScan *scan;
	DoltgresScanDirection way;
	bool		delta_key;
	StdVec		key;
	bool		key_live;
	bool		has_copies;
	DoltgresScanCopies copies;
	HostDeltaHold hold;
	bool		hold_live;
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
 * group takes the delta's key, and makes the group.
 */
static void
doltgres_scan_chosen_body(void *frame_)
{
	doltgres_scan_chosen_frame_t *frame = frame_;
	DoltgresScanScan *scan = frame->scan;
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
 * group's.
 */
static bool
doltgres_scan_chosen(HostIoIo *io, const DoltgresScanUnder *under, DoltgresScanScan *scan,
					 DoltgresScanPicked *picked, DoltgresScanDirection way, HostError *error)
{
	doltgres_scan_chosen_frame_t frame = {.io = io, .under = under, .scan = scan, .way = way, .key_live = true,
	.error = error};
	bool		has_at;
	size_t		at = 0;
	bool		delta_key;

	switch (picked->tag)
	{
		case DOLTGRES_SCAN_PICKED_BASE:
			frame.key = picked->base_key;
			frame.copies = picked->base_copies;
			frame.has_copies = true;
			has_at = picked->has_before;
			at = picked->before;
			delta_key = false;
			break;
		case DOLTGRES_SCAN_PICKED_DELTA:
			frame.key = picked->delta_key;
			has_at = true;
			at = picked->delta_at;
			delta_key = true;
			break;
		case DOLTGRES_SCAN_PICKED_BOTH:
		default:
			std_vec_free_Checked(&picked->base_key, DOLTGRES_SCAN_BYTES);
			frame.key = picked->delta_key;
			frame.copies = picked->base_copies;
			frame.has_copies = true;
			has_at = true;
			at = picked->delta_at;
			delta_key = true;
			break;
	}
	if (scan->has_run)
	{
		if (has_at && (delta_key || way == DOLTGRES_SCAN_DIRECTION_BACKWARD))
			scan->run.split = at + 1;
		else if (has_at)
			scan->run.split = at;
		else if (way == DOLTGRES_SCAN_DIRECTION_FORWARD)
			scan->run.split = scan->run.keys.len;
		else
			scan->run.split = 0;
	}
	doltgres_scan_took(&scan->place, frame.has_copies);
	frame.delta_key = delta_key;
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

	if (seeked)
		doltgres_scan_option_drop(&scan->seek);
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
	if (!doltgres_scan_chosen(frame->io, frame->under, scan, &picked, way, frame->error))
		return;
	frame->result->tag = DOLTGRES_SCAN_TURN_AGAIN;
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
 * in the record: the choice, the read's state, and its way.
 */
typedef struct doltgres_scan_chose_t
{
	DoltgresScanPicked picked;
	const DoltgresScanUnder *under;
	DoltgresScanDirection way;
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

	return doltgres_scan_chosen(io, chose->under, scan, &chose->picked, chose->way, error);
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
		if (step.tag == DOLTGRES_SCAN_TURN_RUN)
		{
			doltgres_scan_TupleOptionVecU8VecVecU8_t looked;
			doltgres_scan_run_set_t set = {0};

			frame->order = step.u.run;
			frame->order_live = true;
			if (!doltgres_scan_run(frame->io, frame->under, &frame->order, &frame->keys, frame->error))
				return;
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
 * code.
 */
static bool
doltgres_scan_run(HostIoIo *io, const DoltgresScanUnder *under, const TreesKeyOrder *order, StdVec *result,
				  HostError *error)
{
	doltgres_scan_run_frame_t frame = {.io = io, .under = under, .order = order, .result = result, .error = error};

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
 * top-level xid, its raw command, what its stamp shows above it, and what a
 * Dirty read found running; made counts the fields it has made, which an
 * unwind frees.
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
 * run, seek, page, sample, and unit cleared.
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
		scan->parallel.has_unit = scan->parallel.has_ranged = false;
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

/* doltgres_scan_placed_captures_t is placed's arguments in a pin scope: the tree, the key, the way, and the order. */
typedef struct doltgres_scan_placed_captures_t
{
	TreesStoreTree tree;
	const uint8 *from;
	size_t		from_len;
	DoltgresScanDirection way;
	const TreesKeyOrder *order;
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
		frame->result->tag = DOLTGRES_SCAN_PLACE_AT;
		frame->cursor_live = false;
		trees_cursor_Cursor_keep(&frame->cursor, &frame->result->u.at.cursor);
		frame->result->u.at.way = captures->way;
		frame->result->u.at.taken = false;
	}
	else
		*frame->result = doltgres_scan_off(captures->way);
	frame->ok = true;
}

/*
 * doltgres_scan_placed answers a base's place at a key in its tree's order:
 * its first entry not below the key going forward, or its last not above it
 * going backward, or off its end that way.
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
	const ChunksTrailerStamp *stamp = NULL;

	if (!doltgres_scan_seek_exact(frame, &paired->root, paired->key.ptr, paired->key.len, &paired->order, &at))
		return;
	if (!at)
	{
		*frame->error = doltgres_scan_resolve_closure_0(paired->tuple_id);
		return;
	}
	if (!trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, &frame->stamps, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->stamps_live = true;
	if (!doltgres_scan_copies(&frame->cursor, paired->keyed.len == 0, &count, &tuple, frame->error))
		return;
	if (frame->stamps.len != count)
	{
		*frame->error = doltgres_scan_uneven();
		return;
	}
	for (size_t i = 0; i < frame->stamps.len && stamp == NULL; i++)
		if (STD_VEC_AT(&frame->stamps, ChunksTrailerStamp, i).tuple == paired->tuple_id)
			stamp = &STD_VEC_AT(&frame->stamps, ChunksTrailerStamp, i);
	if (stamp == NULL)
	{
		*frame->error = doltgres_scan_resolve_closure_0(paired->tuple_id);
		return;
	}
	if (!trees_cursor_Cursor_values(&frame->cursor, frame->io, frame->pin, &frame->values, &failed))
	{
		doltgres_scan_storage_failed(&failed, frame->error);
		return;
	}
	frame->values_live = true;
	std_slice_to_vec_Checked(&frame->tuple, tuple.ptr, tuple.len, DOLTGRES_SCAN_BYTES);
	frame->tuple_live = true;
	frame->bytes_live = true;
	doltgres_scan_value_bytes(frame->values.ptr, frame->values.len, &frame->bytes);
	*row = (DoltgresScanRow)
	{
		.table = paired->table, .stamp = *stamp, .tuple = frame->tuple, .values = frame->bytes
	};
	frame->tuple_live = frame->bytes_live = false;
	row->transform = paired->transform;
	row->keyed = paired->keyed;
	row->key = paired->key;
	std_vec_new(&paired->transform, DOLTGRES_SCAN_BYTES);
	std_vec_new(&paired->keyed, DOLTGRES_SCAN_BYTES);
	std_vec_new(&paired->key, DOLTGRES_SCAN_BYTES);
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
	if (!txn_memtable_tuples_get(frame->io, under->state, tuple, &frame->written.has, &frame->written.v,
								 frame->error))
		return;
	if (!frame->written.has && frame->trees.has_tuple_ids &&
		!doltgres_scan_pair(frame->io, frame->store, frame->trees.tuple_ids, frame->tuple_id, &frame->paired,
							frame->error))
		return;
	key = frame->written.has ? &frame->written.v : frame->paired.has ? &frame->paired.v : NULL;
	if (key == NULL)
	{
		frame->ok = true;
		return;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_slice_row_versions(frame->io, &frame->hold, under->state, key->ptr, key->len, tuple,
										 frame->trees.folded, &slice, frame->error))
		return;
	frame->versions = slice.versions;
	frame->versions_live = true;
	std_vec_new(&slice.versions, sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion));
	txn_memtable_slice_Slice_drop(&slice);
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
			.order = frame->layout.order, .transform = frame->transform
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
 * It takes the layout.
 */
bool
doltgres_scan_fetch(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
					DoltgresScanEpochs *epochs, DoltgresScanTrees trees, DoltgresScanLayout *layout, uint64 tuple_id,
					bool *has_result, DoltgresScanRow *result, HostError *error)
{
	doltgres_scan_fetch_frame_t frame = {.io = io, .store = store, .under = under, .epochs = epochs, .trees = trees,
		.layout = *layout, .layout_live = true, .tuple_id = tuple_id, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_fetch_body, doltgres_scan_fetch_pad);
	doltgres_scan_fetch_pad(&frame);
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

	if (frame->trees.has_root)
	{
		doltgres_scan_fetch_key_captures_t captures = {frame->trees.root, frame->key, frame->key_len,
		&frame->layout.order, frame->layout.keyed.len == 0};

		if (!trees_pin_pin_ResultOptionCopies(frame->io, frame->store, doltgres_scan_fetch_key_closure_0, &captures,
											  NULL, &frame->copied, frame->error))
			return;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_visible_versions(frame->io, &frame->hold, frame->under->state, frame->key, frame->key_len,
							  frame->trees.folded, &versions, frame->error))
		return;
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
 * leave seen, then the delta's rows it sees, oldest first. It takes the
 * layout.
 */
bool
doltgres_scan_fetch_key(HostIoIo *io, TreesStoreNodeStore store, const DoltgresScanUnder *under,
						DoltgresScanEpochs *epochs, DoltgresScanTrees trees, DoltgresScanLayout *layout,
						const uint8 *key, size_t key_len, StdVec *result, HostError *error)
{
	doltgres_scan_fetch_key_frame_t frame = {.io = io, .store = store, .under = under, .epochs = epochs,
		.trees = trees, .layout = *layout, .layout_live = true, .key = key, .key_len = key_len, .result = result,
	.error = error};

	std_landing(&frame, doltgres_scan_fetch_key_body, doltgres_scan_fetch_key_pad);
	doltgres_scan_fetch_key_pad(&frame);
	return frame.ok;
}

/* doltgres_scan_keyed_frame_t is keyed's state across its pad: the delta's key of the tuple id, while live. */
typedef struct doltgres_scan_keyed_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	HostSharedRecord state;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
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

/* doltgres_scan_keyed_body reads the delta's and the base's keys of the tuple id. */
static void
doltgres_scan_keyed_body(void *frame_)
{
	doltgres_scan_keyed_frame_t *frame = frame_;
	HostRow		tuple = {frame->tuple_id};
	doltgres_OptionVecU8_t paired = {0};

	if (!txn_memtable_tuples_get(frame->io, frame->state, tuple, &frame->written.has, &frame->written.v,
								 frame->error))
		return;
	if (frame->has_tuple_ids &&
		!doltgres_scan_pair(frame->io, frame->store, frame->tuple_ids, frame->tuple_id, &paired, frame->error))
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
 * doltgres_scan_keyed answers the row key a tuple id names in a storage,
 * the delta's tuple id table's, else the base's tuple id tree's, read
 * through a store, with whether the base holds the row, or none for a tuple
 * id no layer holds.
 */
bool
doltgres_scan_keyed(HostIoIo *io, TreesStoreNodeStore store, HostSharedRecord state, bool has_tuple_ids,
					TreesStoreTree tuple_ids, uint64 tuple_id, bool *has_result, StdVec *result_0, bool *result_1,
					HostError *error)
{
	doltgres_scan_keyed_frame_t frame = {io, store, state, has_tuple_ids, tuple_ids, tuple_id, {0}, has_result,
	result_0, result_1, error, false};

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
	*result_0 = range.f0;
	*result_1 = range.f1;
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
 * of the record, or none.
 */
typedef struct doltgres_scan_OptionChosen_t
{
	bool		has;
	uint64		tuple_id;
	DoltgresScanTrees trees;
	DoltgresScanLayout layout;
	DoltgresScanEpochs epochs;
} doltgres_scan_OptionChosen_t;

/* DOLTGRES_SCAN_CHOSEN_DROP drops a choice a call left. */
#define DOLTGRES_SCAN_CHOSEN_DROP(place) \
	do { \
		if ((place)->has) \
		{ \
			doltgres_scan_Layout_drop(&(place)->layout); \
			doltgres_scan_Epochs_drop(&(place)->epochs); \
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

	result->has = false;
	if (!has_range)
		return true;
	if (!scan->has_written)
	{
		if (!doltgres_scan_written(io, read->under, scan->folded, &scan->written, error))
			return false;
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
		scan->has_tree, scan->tree, scan->has_tuple_ids, scan->tuple_ids, scan->folded
	};
	result->tuple_id = scan->given;
	result->epochs = scan->epochs;
	doltgres_scan_Epochs_Default_default(&scan->epochs);
	result->has = true;
	return true;
}

/* doltgres_scan_epochs_put_drop drops the epochs of a put that never ran. */
static void
doltgres_scan_epochs_put_drop(void *epochs)
{
	doltgres_scan_Epochs_drop(epochs);
}

/* doltgres_scan_next_in_range_closure_1 is next_in_range's put of the epochs back in the record. */
static bool
doltgres_scan_next_in_range_closure_1(void *captures, HostIoIo *io, DoltgresScanScan *scan, HostError *error)
{
	doltgres_scan_Epochs_drop(&scan->epochs);
	scan->epochs = *(DoltgresScanEpochs *) captures;
	return true;
}

/*
 * doltgres_scan_next_in_range_frame_t is next_in_range's state across its pad:
 * the choice's epochs and the fetched row, while live.
 */
typedef struct doltgres_scan_next_in_range_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const DoltgresScanUnder *under;
	HostOwnedHandle handle;
	DoltgresScanDirection way;
	DoltgresScanEpochs epochs;
	bool		epochs_live;
	bool		has_row;
	DoltgresScanRow row;
	bool	   *has_result;
	DoltgresScanRow *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_next_in_range_frame_t;

/* doltgres_scan_next_in_range_pad drops what next_in_range holds. */
static void
doltgres_scan_next_in_range_pad(void *frame_)
{
	doltgres_scan_next_in_range_frame_t *frame = frame_;

	if (frame->has_row)
		doltgres_scan_Row_drop(&frame->row);
	frame->has_row = false;
	if (frame->epochs_live)
		doltgres_scan_Epochs_drop(&frame->epochs);
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

		if (!host_owned_TableDoltgresScanScan_withOptionChosen(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
															   doltgres_scan_next_in_range_closure_0, &read, NULL,
															   &chosen, frame->error))
			return;
		if (!chosen.has)
		{
			*frame->has_result = false;
			frame->ok = true;
			return;
		}
		frame->epochs = chosen.epochs;
		frame->epochs_live = true;
		if (!doltgres_scan_fetch(frame->io, frame->store, frame->under, &frame->epochs, chosen.trees, &chosen.layout,
								 chosen.tuple_id, &frame->has_row, &frame->row, frame->error))
			return;
		frame->epochs_live = false;
		if (!host_owned_TableDoltgresScanScan_withUnit(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
													   doltgres_scan_next_in_range_closure_1, &frame->epochs,
													   doltgres_scan_epochs_put_drop, frame->error))
			return;
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

/* doltgres_scan_internal returns an internal error of the fork's with a literal message. */
static HostError
doltgres_scan_internal(const char *message)
{
	StdString text;

	std_string_from_Checked(&text, message, strlen(message));
	return host_Error_fork((const uint8 *) "XX000", &text);
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
	scan.page = doltgres_scan_ended();
	frame->reader_live = frame->shared_live = frame->keyed_live = frame->order_live = false;
	frame->ok = host_owned_TableDoltgresScanScan_make(&DOLTGRES_SCAN_SCANS, frame->io, scan, frame->result,
													  frame->error);
}

/*
 * doltgres_scan_begin_parallel makes a parallel scan of the base the leader
 * shared, in a table of a keyed layout, an object id, and a tree's order,
 * each of which it takes, and answers its handle; its first read asks for
 * its first unit.
 */
bool
doltgres_scan_begin_parallel(HostIoIo *io, DoltgresScanShared *shared, StdVec *keyed, uint32 table,
							 TreesKeyOrder *order, HostOwnedHandle *result, HostError *error)
{
	doltgres_scan_begin_parallel_frame_t frame = {.io = io, .shared = *shared, .shared_live = true, .keyed = *keyed,
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
	doltgres_scan_bounds_captures_t captures = {.scan = scan, .tree = scan->tree, .unit = frame->unit,
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
	if (!scan->has_tree || captures.entries == 0)
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
 * doltgres_scan_unit_keys_frame_t is unit_keys' state across its pad: the
 * run's keys not yet looked at and the ones kept, while live.
 */
typedef struct doltgres_scan_unit_keys_frame_t
{
	HostIoIo   *io;
	const DoltgresScanUnder *under;
	const DoltgresScanBounds *bounds;
	StdVec		keys;
	size_t		next;
	bool		keys_live;
	StdVec	   *result;
	bool		result_live;
	HostError  *error;
	bool		ok;
} doltgres_scan_unit_keys_frame_t;

/* doltgres_scan_unit_keys_pad drops the keys not yet looked at and the ones kept. */
static void
doltgres_scan_unit_keys_pad(void *frame_)
{
	doltgres_scan_unit_keys_frame_t *frame = frame_;

	if (frame->result_live)
		doltgres_scan_vecs_drop(frame->result);
	frame->result_live = false;
	if (frame->keys_live)
	{
		for (size_t i = frame->next; i < frame->keys.len; i++)
			std_vec_free_Checked(&STD_VEC_AT(&frame->keys, StdVec, i), DOLTGRES_SCAN_BYTES);
		std_vec_free_Checked(&frame->keys, DOLTGRES_SCAN_VEC);
	}
	frame->keys_live = false;
}

/* doltgres_scan_unit_keys_body reads the run and keeps its keys within the unit's bounds. */
static void
doltgres_scan_unit_keys_body(void *frame_)
{
	doltgres_scan_unit_keys_frame_t *frame = frame_;
	const DoltgresScanBounds *bounds = frame->bounds;

	std_vec_new(frame->result, DOLTGRES_SCAN_VEC);
	if (!bounds->low.has && !bounds->high.has && !bounds->alone)
	{
		frame->ok = true;
		return;
	}
	frame->result_live = true;
	if (!doltgres_scan_run(frame->io, frame->under, &bounds->order, &frame->keys, frame->error))
		return;
	frame->keys_live = true;
	while (frame->next < frame->keys.len)
	{
		StdVec		key = STD_VEC_AT(&frame->keys, StdVec, frame->next);
		StdOrdering ordering;
		bool		at_or_past = true;
		bool		before = true;

		if (bounds->low.has)
		{
			if (!doltgres_scan_total(frame->io, &bounds->order, &key, &bounds->low.v, &ordering, frame->error))
				return;
			at_or_past = ordering != STD_ORDERING_LESS;
		}
		if (bounds->high.has)
		{
			if (!doltgres_scan_total(frame->io, &bounds->order, &key, &bounds->high.v, &ordering, frame->error))
				return;
			before = ordering == STD_ORDERING_LESS;
		}
		if (at_or_past && before)
		{
			std_vec_push_Checked(frame->result, &key, DOLTGRES_SCAN_VEC);
			frame->next++;
		}
		else
		{
			frame->next++;
			std_vec_free_Checked(&key, DOLTGRES_SCAN_BYTES);
		}
	}
	frame->result_live = false;
	frame->ok = true;
}

/*
 * doltgres_scan_unit_keys answers a parallel unit's delta keys in the
 * table's order: every key at or past its low bound and before its high
 * one, with ties broken by the keys' bytes, or every key when its bounds say
 * the unit reads them all alone.
 */
static bool
doltgres_scan_unit_keys(HostIoIo *io, const DoltgresScanUnder *under, const DoltgresScanBounds *bounds,
						StdVec *result, HostError *error)
{
	doltgres_scan_unit_keys_frame_t frame = {.io = io, .under = under, .bounds = bounds, .result = result,
	.error = error};

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

	return doltgres_scan_chosen(io, chose->under, scan, &chose->picked, DOLTGRES_SCAN_DIRECTION_FORWARD, error);
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
	DoltgresScanStep *result;
	HostError  *error;
	bool		ok;
} doltgres_scan_next_parallel_frame_t;

/* doltgres_scan_next_parallel_pad drops what a turn holds. */
static void
doltgres_scan_next_parallel_pad(void *frame_)
{
	doltgres_scan_next_parallel_frame_t *frame = frame_;

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

	for (;;)
	{
		DoltgresScanTurn step;

		if (!host_owned_TableDoltgresScanScan_withOptionBounds(&DOLTGRES_SCAN_SCANS, frame->io, frame->handle,
															   doltgres_scan_next_parallel_closure_0,
															   (void *) &frame->stores, NULL, &frame->begun,
															   frame->error))
			return;
		if (frame->begun.has)
		{
			if (!doltgres_scan_unit_keys(frame->io, frame->under, &frame->begun.v, &frame->keys, frame->error))
				return;
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
		if (step.tag == DOLTGRES_SCAN_TURN_RUN)
		{
			frame->order = step.u.run;
			frame->order_live = true;
			if (!doltgres_scan_run(frame->io, frame->under, &frame->order, &frame->keys, frame->error))
				return;
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

#endif							/* FRONTEND */
