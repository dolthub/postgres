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
 * The template of chunks' index.rs generics that hold a type parameter by
 * value (the port text's section 2): Unread at an error E, the lookups
 * first_in_run and first_in_index at a value V and an error E, and Run and
 * AddressIndex at V. An instance defines the parameters of the parts it
 * needs and includes this file; a part is generated only when its own
 * parameters are defined, and every parameter is undefined at the end.
 *
 * CHUNKS_INDEX_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * CHUNKS_INDEX_DECLARE generates the instance's types and declarations, and
 * CHUNKS_INDEX_DEFINE its definitions, in a file that defines RUST_FILE as
 * index.rs's.
 *
 * At E: CHUNKS_INDEX_ES spells E, and CHUNKS_INDEX_E is E's C form.
 *
 * At V: CHUNKS_INDEX_VS spells V, CHUNKS_INDEX_V is V's C form, and
 * CHUNKS_INDEX_ENTRY is the C form of the tuple (Address, V); a defining
 * instance also defines CHUNKS_INDEX_V_LEN as V's Fixed::LEN,
 * CHUNKS_INDEX_V_EQ(a, b) as V's PartialEq on pointers to values,
 * CHUNKS_INDEX_RUNS_OPS as the name of AddressIndex<V>'s Runs table, and
 * CHUNKS_INDEX_SORT_BY_KEY as the stable sort_by_key of entries by their
 * address, and V's Fixed is chunks_index_<V>_Fixed_write and _read.
 */

/* CHUNKS_INDEX_CAT pastes two tokens after expanding them. */
#define CHUNKS_INDEX_CAT_(a, b) a##b
#define CHUNKS_INDEX_CAT(a, b) CHUNKS_INDEX_CAT_(a, b)

/* CHUNKS_INDEX_DEF is CHUNKS_INDEX_SCOPE's storage class for a definition: static, or none for extern. */
#define CHUNKS_INDEX_DEF_static static
#define CHUNKS_INDEX_DEF_extern
#define CHUNKS_INDEX_DEF CHUNKS_INDEX_CAT(CHUNKS_INDEX_DEF_, CHUNKS_INDEX_SCOPE)

#ifdef CHUNKS_INDEX_ES

/* CHUNKS_INDEX_UNREAD is Unread<E>. */
#define CHUNKS_INDEX_UNREAD CHUNKS_INDEX_CAT(ChunksIndexUnread, CHUNKS_INDEX_ES)

#ifdef CHUNKS_INDEX_DECLARE

/*
 * Unread<E> is why a lookup in an index's blocks ended without an answer: a
 * read of a block failed, or the block at a position of a run doesn't fit
 * the run's fences.
 */
typedef struct CHUNKS_INDEX_UNREAD
{
	ChunksIndexUnreadTag tag;
	union
	{
		CHUNKS_INDEX_E source;
		struct
		{
			size_t		f0;
			size_t		f1;
		}			unfit;
	}			u;
}			CHUNKS_INDEX_UNREAD;

#endif							/* CHUNKS_INDEX_DECLARE */
#endif							/* CHUNKS_INDEX_ES */

#if defined(CHUNKS_INDEX_VS) && defined(CHUNKS_INDEX_ES)

/* CHUNKS_INDEX_LOOKUP names the lookups' instance at V and E. */
#define CHUNKS_INDEX_LOOKUP(name) \
	CHUNKS_INDEX_CAT(CHUNKS_INDEX_CAT(chunks_index_##name##_, CHUNKS_INDEX_VS), CHUNKS_INDEX_ES)

#ifdef CHUNKS_INDEX_DECLARE
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_LOOKUP(first_in_run) (ChunksIndexSource source, void *with, size_t run,
														   const ChunksIndexFences *fences,
														   const ChunksAddressAddress *key, bool *has_result,
														   size_t *result_0, CHUNKS_INDEX_V * result_1,
														   CHUNKS_INDEX_UNREAD * error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_LOOKUP(first_in_index) (ChunksIndexSource source, void *with,
															 const ChunksIndexFences *const *fences,
															 size_t fences_len, const ChunksAddressAddress *key,
															 bool *has_result, std_TupleUsizeUsize_t *result_0,
															 CHUNKS_INDEX_V * result_1,
															 CHUNKS_INDEX_UNREAD * error);
#endif							/* CHUNKS_INDEX_DECLARE */

#ifdef CHUNKS_INDEX_DEFINE

/* The first_in_run frame, body, and pad of the lookups' instance. */
#define CHUNKS_INDEX_FIRST_IN_RUN_FRAME CHUNKS_INDEX_CAT(CHUNKS_INDEX_LOOKUP(first_in_run), _frame_0_t)
#define CHUNKS_INDEX_FIRST_IN_RUN_BODY CHUNKS_INDEX_CAT(CHUNKS_INDEX_LOOKUP(first_in_run), _body_0)
#define CHUNKS_INDEX_FIRST_IN_RUN_PAD CHUNKS_INDEX_CAT(CHUNKS_INDEX_LOOKUP(first_in_run), _pad_0)

/*
 * The state of first_in_run across its pad: its arguments, its results, and
 * the block it holds.
 */
typedef struct CHUNKS_INDEX_FIRST_IN_RUN_FRAME
{
	ChunksIndexSource source;
	void	   *with;
	size_t		run;
	const ChunksIndexFences *fences;
	const ChunksAddressAddress *key;
	bool	   *has_result;
	size_t	   *result_0;
	CHUNKS_INDEX_V *result_1;
	CHUNKS_INDEX_UNREAD *error;
	StdVec		entries;
	bool		entries_live;
	bool		ok;
}			CHUNKS_INDEX_FIRST_IN_RUN_FRAME;

/* first_in_run's body. */
static void
CHUNKS_INDEX_FIRST_IN_RUN_BODY(void *frame_)
{
	CHUNKS_INDEX_FIRST_IN_RUN_FRAME *frame = frame_;
	size_t		keys_len;
	const ChunksAddressAddress *keys = chunks_index_Fences_keys(frame->fences, &keys_len);
	size_t		range_0;
	size_t		range_1;
	size_t		b;

	chunks_index_block_range(keys, keys_len, frame->key, &range_0, &range_1);
	b = range_0;
	while (b < range_1)
	{
		std_TupleUsizeUsize_t question = {frame->run, b};
		CHUNKS_INDEX_E source_error;
		size_t		at;

		if (!frame->source.ops->ask(frame->source.self, frame->with, &question, &frame->entries, &source_error))
		{
			frame->error->tag = CHUNKS_INDEX_UNREAD_SOURCE;
			frame->error->u.source = source_error;
			return;
		}
		frame->entries_live = true;
		if (!chunks_index_block_fits(sizeof(CHUNKS_INDEX_ENTRY), frame->entries.ptr, frame->entries.len, keys,
									 keys_len, b))
		{
			frame->error->tag = CHUNKS_INDEX_UNREAD_UNFIT;
			frame->error->u.unfit.f0 = frame->run;
			frame->error->u.unfit.f1 = b;
			frame->entries_live = false;
			std_vec_free_Checked(&frame->entries, sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY));
			return;
		}
		if (chunks_index_first_with(sizeof(CHUNKS_INDEX_ENTRY), frame->entries.ptr, frame->entries.len, frame->key,
									&at))
		{
			std_index_check(at, frame->entries.len, RUST_FILE, 542, 32);
			*frame->result_1 = STD_VEC_AT(&frame->entries, CHUNKS_INDEX_ENTRY, at).f1;
			*frame->result_0 = b;
			*frame->has_result = true;
			frame->ok = true;
			frame->entries_live = false;
			std_vec_free_Checked(&frame->entries, sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY));
			return;
		}
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY));
		b = STD_ADD(usize, b, 1, 548, 9);
	}
	*frame->has_result = false;
	frame->ok = true;
}

/* first_in_run's pad: the block, while it holds it. */
static void
CHUNKS_INDEX_FIRST_IN_RUN_PAD(void *frame_)
{
	CHUNKS_INDEX_FIRST_IN_RUN_FRAME *frame = frame_;

	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY));
}

/*
 * The lookups' first_in_run: the value of the first entry with a key in one
 * run of an index, whose blocks a source reads by the run's and the block's
 * positions, with its block's position, reading only the blocks the run's
 * fences say can hold the key and checking that each fits them, or none when
 * no block that fits holds the key.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_LOOKUP(first_in_run) (ChunksIndexSource source, void *with, size_t run,
								   const ChunksIndexFences *fences, const ChunksAddressAddress *key,
								   bool *has_result, size_t *result_0, CHUNKS_INDEX_V * result_1,
								   CHUNKS_INDEX_UNREAD * error)
{
	CHUNKS_INDEX_FIRST_IN_RUN_FRAME frame = {.source = source, .with = with, .run = run, .fences = fences,
	.key = key, .has_result = has_result, .result_0 = result_0, .result_1 = result_1, .error = error};

	std_landing(&frame, CHUNKS_INDEX_FIRST_IN_RUN_BODY, CHUNKS_INDEX_FIRST_IN_RUN_PAD);
	return frame.ok;
}

/*
 * The lookups' first_in_index: the value of the first entry with a key in
 * the newest run of an index that holds one, the runs' fences in the runs'
 * order, oldest first, with the run's and the block's positions it was read
 * at, or none when every run lacks the key.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_LOOKUP(first_in_index) (ChunksIndexSource source, void *with, const ChunksIndexFences *const *fences,
									 size_t fences_len, const ChunksAddressAddress *key, bool *has_result,
									 std_TupleUsizeUsize_t *result_0, CHUNKS_INDEX_V * result_1,
									 CHUNKS_INDEX_UNREAD * error)
{
	size_t		r = fences_len;

	while (r > 0)
	{
		bool		has;
		size_t		b;
		CHUNKS_INDEX_V value;

		r = STD_SUB(usize, r, 1, 614, 9);
		std_index_check(r, fences_len, RUST_FILE, 616, 45);
		if (!CHUNKS_INDEX_LOOKUP(first_in_run) (source, with, r, fences[r], key, &has, &b, &value, error))
			return false;
		if (has)
		{
			result_0->f0 = r;
			result_0->f1 = b;
			*result_1 = value;
			*has_result = true;
			return true;
		}
	}
	*has_result = false;
	return true;
}

#undef CHUNKS_INDEX_FIRST_IN_RUN_FRAME
#undef CHUNKS_INDEX_FIRST_IN_RUN_BODY
#undef CHUNKS_INDEX_FIRST_IN_RUN_PAD
#endif							/* CHUNKS_INDEX_DEFINE */

#undef CHUNKS_INDEX_LOOKUP
#endif							/* CHUNKS_INDEX_VS && CHUNKS_INDEX_ES */

#ifdef CHUNKS_INDEX_VS

/* CHUNKS_INDEX_RUN is Run<V>, and CHUNKS_INDEX_RUN_NAME names its methods. */
#define CHUNKS_INDEX_RUN CHUNKS_INDEX_CAT(ChunksIndexRun, CHUNKS_INDEX_VS)
#define CHUNKS_INDEX_RUN_NAME(name) CHUNKS_INDEX_CAT(CHUNKS_INDEX_CAT(chunks_index_Run, CHUNKS_INDEX_VS), _##name)

/* CHUNKS_INDEX_INDEX is AddressIndex<V>, and CHUNKS_INDEX_INDEX_NAME names its methods. */
#define CHUNKS_INDEX_INDEX CHUNKS_INDEX_CAT(ChunksIndexAddressIndex, CHUNKS_INDEX_VS)
#define CHUNKS_INDEX_INDEX_NAME(name) \
	CHUNKS_INDEX_CAT(CHUNKS_INDEX_CAT(chunks_index_AddressIndex, CHUNKS_INDEX_VS), _##name)

/* CHUNKS_INDEX_FN names a generic function's instance at V. */
#define CHUNKS_INDEX_FN(name) CHUNKS_INDEX_CAT(chunks_index_##name##_, CHUNKS_INDEX_VS)

/* CHUNKS_INDEX_FIXED names V's Fixed methods. */
#define CHUNKS_INDEX_FIXED(name) CHUNKS_INDEX_CAT(CHUNKS_INDEX_CAT(chunks_index_, CHUNKS_INDEX_VS), _Fixed_##name)

/* CHUNKS_INDEX_ARC_RUN names the glue of Arc<Run<V>>. */
#define CHUNKS_INDEX_ARC_RUN(name) CHUNKS_INDEX_CAT(CHUNKS_INDEX_CAT(chunks_index_ArcRun, CHUNKS_INDEX_VS), _##name)

#ifdef CHUNKS_INDEX_DECLARE

/* Run<V> is an open run file of a value for each address. */
typedef struct CHUNKS_INDEX_RUN
{
	StdPathBuf	path;
	ChunksIndexBacking backing;
	uint64		count;
	/* fences is the first address of each block, in order. */
	ChunksIndexFences fences;

	/*
	 * at is where the run's bytes begin in its file, past its start for the
	 * address table a pack holds.
	 */
	uint64		at;
}			CHUNKS_INDEX_RUN;

/*
 * AddressIndex<V> is a set of runs, newest last, which a clone shares, so
 * that a reader searches it outside the lock it is kept under.
 */
typedef struct CHUNKS_INDEX_INDEX
{
	/* runs is a Vec<Arc<Run<V>>>. */
	StdVec		runs;
}			CHUNKS_INDEX_INDEX;

CHUNKS_INDEX_SCOPE size_t CHUNKS_INDEX_FN(entry) (void);
CHUNKS_INDEX_SCOPE size_t CHUNKS_INDEX_FN(per_block) (void);
CHUNKS_INDEX_SCOPE void CHUNKS_INDEX_ARC_RUN(Clone_clone) (const void *self, void *result);
CHUNKS_INDEX_SCOPE void CHUNKS_INDEX_ARC_RUN(drop) (void *self);
CHUNKS_INDEX_SCOPE void CHUNKS_INDEX_ARC_RUN(new) (void *run, void *result);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_INDEX_NAME(Runs_block) (const void *self, void *with, size_t run, size_t block,
															 StdVec *result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(write) (HostIoIoDoors io, const char *path, size_t path_len,
													  const CHUNKS_INDEX_ENTRY * entries, size_t entries_len,
													  CHUNKS_INDEX_RUN * result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(in_memory) (HostIoIo *io, const char *path, size_t path_len,
														  const CHUNKS_INDEX_ENTRY * entries, size_t entries_len,
														  CHUNKS_INDEX_RUN * result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(decode) (HostIoReadDoors io, const char *path, size_t path_len,
													   uint64 at, StdVec *bytes, CHUNKS_INDEX_RUN * result,
													   ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(encode) (const CHUNKS_INDEX_ENTRY * entries, size_t entries_len,
													   StdVec *result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(open) (HostIoReadDoors io, const char *path, size_t path_len,
													 CHUNKS_INDEX_RUN * result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE const char *CHUNKS_INDEX_RUN_NAME(path) (const CHUNKS_INDEX_RUN * self, size_t *result_len);
CHUNKS_INDEX_SCOPE uint64 CHUNKS_INDEX_RUN_NAME(len) (const CHUNKS_INDEX_RUN * self);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(is_empty) (const CHUNKS_INDEX_RUN * self);
CHUNKS_INDEX_SCOPE size_t CHUNKS_INDEX_RUN_NAME(per_block) (void);
CHUNKS_INDEX_SCOPE size_t CHUNKS_INDEX_RUN_NAME(blocks) (const CHUNKS_INDEX_RUN * self);
CHUNKS_INDEX_SCOPE ChunksAddressAddress CHUNKS_INDEX_RUN_NAME(fence) (const CHUNKS_INDEX_RUN * self, size_t i);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(block) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io, size_t i,
													  StdVec *result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE ChunksErrorError CHUNKS_INDEX_RUN_NAME(damaged) (const CHUNKS_INDEX_RUN * self, size_t i,
																	const char *holds, size_t holds_len);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(get) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io,
													const ChunksAddressAddress *address,
													bool (*keep) (void *captures, const CHUNKS_INDEX_V * value),
													void *keep_captures, void (*keep_drop) (void *),
													bool *has_result, CHUNKS_INDEX_V * result,
													ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(range) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io,
													  const ChunksAddressAddress *low,
													  const ChunksAddressAddress *high, StdVec *result,
													  ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_RUN_NAME(entries) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io,
														StdVec *result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE void CHUNKS_INDEX_RUN_NAME(drop) (void *self);
CHUNKS_INDEX_SCOPE CHUNKS_INDEX_INDEX CHUNKS_INDEX_INDEX_NAME(Default_default) (void);
CHUNKS_INDEX_SCOPE CHUNKS_INDEX_INDEX CHUNKS_INDEX_INDEX_NAME(Clone_clone) (const CHUNKS_INDEX_INDEX * self);
CHUNKS_INDEX_SCOPE CHUNKS_INDEX_INDEX CHUNKS_INDEX_INDEX_NAME(new) (StdVec *runs);
CHUNKS_INDEX_SCOPE CHUNKS_INDEX_INDEX CHUNKS_INDEX_INDEX_NAME(shared) (StdVec *runs);
CHUNKS_INDEX_SCOPE void CHUNKS_INDEX_INDEX_NAME(add) (CHUNKS_INDEX_INDEX * self, CHUNKS_INDEX_RUN * run);
CHUNKS_INDEX_SCOPE StdArcInner *const *CHUNKS_INDEX_INDEX_NAME(runs) (const CHUNKS_INDEX_INDEX * self,
																	  size_t *result_len);
CHUNKS_INDEX_SCOPE StdVec CHUNKS_INDEX_INDEX_NAME(fences) (const CHUNKS_INDEX_INDEX * self);
CHUNKS_INDEX_SCOPE ChunksErrorError CHUNKS_INDEX_INDEX_NAME(unread) (const CHUNKS_INDEX_INDEX * self,
																	 ChunksIndexUnreadError *unread);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_INDEX_NAME(get) (const CHUNKS_INDEX_INDEX * self, HostIoReadDoors io,
													  const ChunksAddressAddress *address, bool *has_result,
													  CHUNKS_INDEX_V * result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_INDEX_NAME(all) (const CHUNKS_INDEX_INDEX * self, HostIoReadDoors io,
													  const ChunksAddressAddress *address, StdVec *result,
													  ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_INDEX_NAME(with_prefix) (const CHUNKS_INDEX_INDEX * self, HostIoIo *io,
															  const char *prefix, size_t prefix_len,
															  StdVec *result, ChunksErrorError *error);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_INDEX_NAME(to_merge) (const CHUNKS_INDEX_INDEX * self, size_t *result);
CHUNKS_INDEX_SCOPE bool CHUNKS_INDEX_INDEX_NAME(merged) (const CHUNKS_INDEX_INDEX * self, HostIoReadDoors io,
														 size_t from, size_t to, StdVec *result,
														 ChunksErrorError *error);
CHUNKS_INDEX_SCOPE StdVec CHUNKS_INDEX_INDEX_NAME(remove) (CHUNKS_INDEX_INDEX * self,
														   bool (*pick) (void *captures,
																		 const CHUNKS_INDEX_RUN * run),
														   void *pick_captures, void (*pick_drop) (void *));
CHUNKS_INDEX_SCOPE StdVec CHUNKS_INDEX_INDEX_NAME(replace) (CHUNKS_INDEX_INDEX * self, size_t from, size_t to,
															CHUNKS_INDEX_RUN * run);
CHUNKS_INDEX_SCOPE void CHUNKS_INDEX_INDEX_NAME(drop) (void *self);

#endif							/* CHUNKS_INDEX_DECLARE */

#ifdef CHUNKS_INDEX_DEFINE

/* CHUNKS_INDEX_ENTRIES is the size and alignment of the tuple (Address, V), a vector's element. */
#define CHUNKS_INDEX_ENTRIES sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY)

/* CHUNKS_INDEX_ARCS is the size and alignment of an Arc<Run<V>>, a vector's element. */
#define CHUNKS_INDEX_ARCS sizeof(StdArcInner *), _Alignof(StdArcInner *)

/* CHUNKS_INDEX_FIRST_IN_INDEX is the lookups' first_in_index at V and Error, which AddressIndex<V>'s get runs. */
#define CHUNKS_INDEX_FIRST_IN_INDEX CHUNKS_INDEX_CAT(CHUNKS_INDEX_CAT(chunks_index_first_in_index_, CHUNKS_INDEX_VS), Error)

/* The frames, bodies, and pads of Run<V>'s and AddressIndex<V>'s methods that own values across a call that can panic. */
#define CHUNKS_INDEX_WRITE_FRAME CHUNKS_INDEX_RUN_NAME(write_frame_0_t)
#define CHUNKS_INDEX_ENCODE_FRAME CHUNKS_INDEX_RUN_NAME(encode_frame_0_t)
#define CHUNKS_INDEX_OPEN_FRAME CHUNKS_INDEX_RUN_NAME(open_frame_0_t)
#define CHUNKS_INDEX_READ_FRAME CHUNKS_INDEX_RUN_NAME(read_frame_0_t)
#define CHUNKS_INDEX_BLOCK_FRAME CHUNKS_INDEX_RUN_NAME(block_frame_0_t)
#define CHUNKS_INDEX_DAMAGED_FRAME CHUNKS_INDEX_RUN_NAME(damaged_frame_0_t)
#define CHUNKS_INDEX_GET_FRAME CHUNKS_INDEX_RUN_NAME(get_frame_0_t)
#define CHUNKS_INDEX_RANGE_FRAME CHUNKS_INDEX_RUN_NAME(range_frame_0_t)
#define CHUNKS_INDEX_RUN_ENTRIES_FRAME CHUNKS_INDEX_RUN_NAME(entries_frame_0_t)
#define CHUNKS_INDEX_ADD_FRAME CHUNKS_INDEX_INDEX_NAME(add_frame_0_t)
#define CHUNKS_INDEX_FENCES_FRAME CHUNKS_INDEX_INDEX_NAME(fences_frame_0_t)
#define CHUNKS_INDEX_INDEX_GET_FRAME CHUNKS_INDEX_INDEX_NAME(get_frame_0_t)
#define CHUNKS_INDEX_ALL_FRAME CHUNKS_INDEX_INDEX_NAME(all_frame_0_t)
#define CHUNKS_INDEX_WITH_PREFIX_FRAME CHUNKS_INDEX_INDEX_NAME(with_prefix_frame_0_t)
#define CHUNKS_INDEX_MERGED_FRAME CHUNKS_INDEX_INDEX_NAME(merged_frame_0_t)
#define CHUNKS_INDEX_REMOVE_FRAME CHUNKS_INDEX_INDEX_NAME(remove_frame_0_t)
#define CHUNKS_INDEX_REPLACE_FRAME CHUNKS_INDEX_INDEX_NAME(replace_frame_0_t)

/* entry returns the size of an entry of a run of V: the address and the value. */
CHUNKS_INDEX_DEF size_t
CHUNKS_INDEX_FN(entry) (void)
{
	return 32 + CHUNKS_INDEX_V_LEN;
}

/* per_block returns the number of entries in a block of a run of V. */
CHUNKS_INDEX_DEF size_t
CHUNKS_INDEX_FN(per_block) (void)
{
	return STD_DIV(usize, CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, CHUNKS_INDEX_FN(entry) (), 763, 5);
}

/* ArcRun_Clone_clone is Arc<Run<V>>'s Clone: another reference to the run. */
CHUNKS_INDEX_DEF void
CHUNKS_INDEX_ARC_RUN(Clone_clone) (const void *self, void *result)
{
	*(StdArcInner **) result = std_arc_clone(*(StdArcInner *const *) self);
}

/* ArcRun_drop is Arc<Run<V>>'s drop glue: the run's, when the last reference goes. */
CHUNKS_INDEX_DEF void
CHUNKS_INDEX_ARC_RUN(drop) (void *self)
{
	std_arc_drop(*(StdArcInner **) self, sizeof(CHUNKS_INDEX_RUN), _Alignof(CHUNKS_INDEX_RUN), CHUNKS_INDEX_RUN_NAME(drop));
}

/* ArcRun_new is Arc::<Run<V>>::new as a function value: the run moved into a new Arc. */
CHUNKS_INDEX_DEF void
CHUNKS_INDEX_ARC_RUN(new) (void *run, void *result)
{
	*(StdArcInner **) result = std_arc_new_dropping(run, sizeof(CHUNKS_INDEX_RUN), _Alignof(CHUNKS_INDEX_RUN),
													CHUNKS_INDEX_RUN_NAME(drop));
}

/* AddressIndex<V>'s Runs::block: the decoded block at a position of the run at a position, read through the with's doors. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_INDEX_NAME(Runs_block) (const void *self_, void *with, size_t run, size_t block, StdVec *result,
									 ChunksErrorError *error)
{
	const		CHUNKS_INDEX_INDEX *self = self_;
	const		CHUNKS_INDEX_RUN *at;

	std_index_check(run, self->runs.len, RUST_FILE, 702, 18);
	at = std_arc_Deref_deref(STD_VEC_AT(&self->runs, StdArcInner *, run), _Alignof(CHUNKS_INDEX_RUN));
	return CHUNKS_INDEX_RUN_NAME(block) (at, *(HostIoReadDoors *) with, block, result, error);
}

/* CHUNKS_INDEX_RUNS_OPS is AddressIndex<V>'s Runs table. */
CHUNKS_INDEX_DEF const ChunksIndexRunsOps CHUNKS_INDEX_RUNS_OPS = {CHUNKS_INDEX_INDEX_NAME(Runs_block)};

static bool CHUNKS_INDEX_RUN_NAME(read) (HostIoReadDoors io, const char *path, size_t path_len, size_t size,
										 ChunksIndexBacking *backing, CHUNKS_INDEX_RUN * result,
										 ChunksErrorError *error);

/*
 * The state of encode across its pad: its arguments, its result and error,
 * and the bytes it writes until they are its result.
 */
typedef struct CHUNKS_INDEX_ENCODE_FRAME
{
	const		CHUNKS_INDEX_ENTRY *entries;
	size_t		entries_len;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		out_live;
	bool		ok;
}			CHUNKS_INDEX_ENCODE_FRAME;

/* encode's body. */
static void
CHUNKS_INDEX_RUN_NAME(encode_body_0) (void *frame_)
{
	CHUNKS_INDEX_ENCODE_FRAME *frame = frame_;
	StdVec	   *out = frame->result;
	StdWindows	windows = std_slice_windows(frame->entries, frame->entries_len, 2, RUST_FILE, 827, 35);
	const void *window;
	size_t		window_len;
	bool		sorted = true;
	size_t		blocks;
	size_t		chunk_size;
	StdChunks	chunks;
	const void *block;
	size_t		block_len;
	size_t		fences_at;
	uint8		le[8];

	while (std_slice_Windows_next(&windows, sizeof(CHUNKS_INDEX_ENTRY), &window, &window_len))
	{
		const		CHUNKS_INDEX_ENTRY *w = window;

		std_index_check(0, window_len, RUST_FILE, 827, 54);
		std_index_check(1, window_len, RUST_FILE, 827, 64);
		if (!chunks_address_Address_PartialOrd_le(&w[0].f0, &w[1].f0))
		{
			sorted = false;
			break;
		}
	}
	if (!sorted)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("I1d: a run's entries are written unsorted")};
		StdFmtArguments arguments = {pieces, 1, NULL};

		frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&frame->error->u.invariant, &arguments);
		return;
	}
	blocks = std_div_ceil_usize(frame->entries_len, CHUNKS_INDEX_FN(per_block) (), RUST_FILE, 828, 36);
	std_vec_with_capacity_Checked(out, STD_ADD(usize, STD_ADD(usize, 8, STD_MUL(usize, blocks, CHUNKS_INDEX_BLOCK + 32,
																				829, 46),
															  829, 42),
											   CHUNKS_INDEX_FOOTER, 829, 42),
								  1, 1);
	frame->out_live = true;
	std_vec_extend_from_slice_Checked(out, CHUNKS_INDEX_MAGIC, sizeof(CHUNKS_INDEX_MAGIC), 1, 1);
	chunk_size = CHUNKS_INDEX_FN(per_block) ();
	std_slice_chunks_check(chunk_size, RUST_FILE, 831, 30);
	chunks = std_slice_chunks(frame->entries, frame->entries_len, chunk_size);
	while (std_slice_Chunks_next(&chunks, sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY), &block, &block_len))
	{
		const		CHUNKS_INDEX_ENTRY *entries = block;
		size_t		start = out->len;
		uint8		zero = 0;

		for (size_t i = 0, i_len = block_len; i < i_len; i++)
		{
			std_vec_extend_from_slice_Checked(out, entries[i].f0.f0, sizeof(entries[i].f0.f0), 1, 1);
			CHUNKS_INDEX_FIXED(write) (&entries[i].f1, out);
		}
		std_vec_resize(out, STD_SUB(usize, STD_ADD(usize, start, CHUNKS_INDEX_BLOCK, 837, 24), CHUNKS_INDEX_SUM, 837,
									24),
					   &zero, 1, 1);
		std_range_from_check(start, out->len, RUST_FILE, 838, 54);
		std_u64_to_le_bytes(XXH3_64bits(STD_PTR_AT(out->ptr, start, 1), std_wrapping_sub_usize(out->len, start)), le);
		std_vec_extend_from_slice_Checked(out, le, sizeof(le), 1, 1);
	}
	fences_at = out->len;
	chunk_size = CHUNKS_INDEX_FN(per_block) ();
	std_slice_chunks_check(chunk_size, RUST_FILE, 842, 30);
	chunks = std_slice_chunks(frame->entries, frame->entries_len, chunk_size);
	while (std_slice_Chunks_next(&chunks, sizeof(CHUNKS_INDEX_ENTRY), _Alignof(CHUNKS_INDEX_ENTRY), &block, &block_len))
	{
		const		CHUNKS_INDEX_ENTRY *entries = block;

		std_index_check(0, block_len, RUST_FILE, 843, 36);
		std_vec_extend_from_slice_Checked(out, entries[0].f0.f0, sizeof(entries[0].f0.f0), 1, 1);
	}
	std_u64_to_le_bytes((uint64) frame->entries_len, le);
	std_vec_extend_from_slice_Checked(out, le, sizeof(le), 1, 1);
	std_range_from_check(fences_at, out->len, RUST_FILE, 846, 55);
	std_u64_to_le_bytes(XXH3_64bits(STD_PTR_AT(out->ptr, fences_at, 1), std_wrapping_sub_usize(out->len, fences_at)),
						le);
	std_vec_extend_from_slice_Checked(out, le, sizeof(le), 1, 1);
	std_vec_extend_from_slice_Checked(out, CHUNKS_INDEX_MAGIC, sizeof(CHUNKS_INDEX_MAGIC), 1, 1);
	frame->out_live = false;
	frame->ok = true;
}

/* encode's pad: the bytes, while it writes them. */
static void
CHUNKS_INDEX_RUN_NAME(encode_pad_0) (void *frame_)
{
	CHUNKS_INDEX_ENCODE_FRAME *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(frame->result, 1, 1);
}

/* encode returns the bytes of a run of entries sorted by address. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(encode) (const CHUNKS_INDEX_ENTRY * entries, size_t entries_len, StdVec *result,
							   ChunksErrorError *error)
{
	CHUNKS_INDEX_ENCODE_FRAME frame = {.entries = entries, .entries_len = entries_len, .result = result,
	.error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(encode_body_0), CHUNKS_INDEX_RUN_NAME(encode_pad_0));
	return frame.ok;
}

/*
 * The state of write across its pad: its arguments, its result and error,
 * the encoded bytes, and in tests SHORT_WRITE's backend value, its lock's
 * guard, and the directory it takes.
 */
typedef struct CHUNKS_INDEX_WRITE_FRAME
{
	HostIoIoDoors io;
	const char *path;
	size_t		path_len;
	const		CHUNKS_INDEX_ENTRY *entries;
	size_t		entries_len;
	CHUNKS_INDEX_RUN *result;
	ChunksErrorError *error;
#ifdef DOLTGRES_TEST_CHUNKS
	HostLocalLocal short_write;
	bool		short_write_live;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	chunks_OptionPathBuf_t taken;
	bool		taken_live;
#endif
	StdVec		bytes;
	bool		bytes_live;
	bool		ok;
}			CHUNKS_INDEX_WRITE_FRAME;

/* write's body. */
static void
CHUNKS_INDEX_RUN_NAME(write_body_0) (void *frame_)
{
	CHUNKS_INDEX_WRITE_FRAME *frame = frame_;
	bool		wrote;

#ifdef DOLTGRES_TEST_CHUNKS
	{
		HostIoHeld *held;
		chunks_OptionPathBuf_t *dir;

		frame->short_write = host_local_BackendLocal_get(&CHUNKS_INDEX_SHORT_WRITE.local, &CHUNKS_INDEX_SHORT_WRITE_TYPE);
		frame->short_write_live = true;
		held = frame->io.ops->read_doors.deref_mut.deref_mut(frame->io.self);
		frame->guard = host_lock_IoMutex_lock((HostLockIoMutex *) host_local_Local_Deref_deref(&frame->short_write,
																							   &CHUNKS_INDEX_SHORT_WRITE_TYPE),
											  &CHUNKS_INDEX_SHORT_WRITE_LOCK_TYPE, held);
		frame->guard_live = true;
		dir = host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
		frame->taken.has = false;
		if (dir->has)
		{
			StdStr		parent;

			if (std_path_parent(frame->path, frame->path_len, &parent) &&
				std_path_PartialEq_eq(parent.ptr, parent.len, dir->value.bytes.ptr, dir->value.bytes.len))
			{
				frame->taken = *dir;
				dir->has = false;
			}
		}
		frame->taken_live = true;
		if (frame->taken.has)
		{
			size_t		kept = std_saturating_sub_usize(frame->entries_len, 1);

			std_range_to_check(kept, frame->entries_len, RUST_FILE, 805, 32);
			frame->entries_len = kept;
		}
		frame->taken_live = false;
		if (frame->taken.has)
			std_vec_free_Checked(&frame->taken.value.bytes, 1, 1);
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
		frame->short_write_live = false;
		host_local_Local_drop(&frame->short_write, &CHUNKS_INDEX_SHORT_WRITE_TYPE);
	}
#endif
	if (!CHUNKS_INDEX_RUN_NAME(encode) (frame->entries, frame->entries_len, &frame->bytes, frame->error))
		return;
	frame->bytes_live = true;
	wrote = chunks_file_write_file(frame->io, frame->path, frame->path_len, frame->bytes.ptr, frame->bytes.len,
								   frame->error);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
	if (!wrote)
		return;
	frame->ok = CHUNKS_INDEX_RUN_NAME(open) (HOST_IO_IO_DOORS_READ_DOORS(frame->io), frame->path, frame->path_len,
											 frame->result, frame->error);
}

/* write's pad: the encoded bytes, and in tests the taken directory, the lock's guard, and the backend value. */
static void
CHUNKS_INDEX_RUN_NAME(write_pad_0) (void *frame_)
{
	CHUNKS_INDEX_WRITE_FRAME *frame = frame_;

	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
#ifdef DOLTGRES_TEST_CHUNKS
	if (frame->taken_live && frame->taken.has)
		std_vec_free_Checked(&frame->taken.value.bytes, 1, 1);
	if (frame->guard_live)
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	if (frame->short_write_live)
		host_local_Local_drop(&frame->short_write, &CHUNKS_INDEX_SHORT_WRITE_TYPE);
#endif
}

/* write writes a run of entries sorted by address to the path and opens it. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(write) (HostIoIoDoors io, const char *path, size_t path_len, const CHUNKS_INDEX_ENTRY * entries,
							  size_t entries_len, CHUNKS_INDEX_RUN * result, ChunksErrorError *error)
{
	CHUNKS_INDEX_WRITE_FRAME frame = {.io = io, .path = path, .path_len = path_len, .entries = entries,
	.entries_len = entries_len, .result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(write_body_0), CHUNKS_INDEX_RUN_NAME(write_pad_0));
	return frame.ok;
}

/*
 * in_memory returns a run of entries sorted by address kept in memory under
 * the path its file would have, for a process that changes no file.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(in_memory) (HostIoIo *io, const char *path, size_t path_len, const CHUNKS_INDEX_ENTRY * entries,
								  size_t entries_len, CHUNKS_INDEX_RUN * result, ChunksErrorError *error)
{
	ChunksIndexBacking backing = {.tag = CHUNKS_INDEX_BACKING_MEMORY};

	if (!CHUNKS_INDEX_RUN_NAME(encode) (entries, entries_len, &backing.u.memory, error))
		return false;
	return CHUNKS_INDEX_RUN_NAME(read) (HOST_IO_IO_READ_DOORS(io), path, path_len, backing.u.memory.len, &backing,
										result, error);
}

/*
 * decode returns the run whose bytes another file holds at an offset, as a
 * pack holds its own run, named by that file, taking the bytes.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(decode) (HostIoReadDoors io, const char *path, size_t path_len, uint64 at, StdVec *bytes,
							   CHUNKS_INDEX_RUN * result, ChunksErrorError *error)
{
	ChunksIndexBacking backing = {.tag = CHUNKS_INDEX_BACKING_MEMORY, .u.memory = *bytes};

	if (!CHUNKS_INDEX_RUN_NAME(read) (io, path, path_len, bytes->len, &backing, result, error))
		return false;
	result->at = at;
	return true;
}

/* The state of open across its pad: its arguments, its result and error, and the file it opens. */
typedef struct CHUNKS_INDEX_OPEN_FRAME
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	CHUNKS_INDEX_RUN *result;
	ChunksErrorError *error;
	HostFile	file;
	bool		file_live;
	bool		ok;
}			CHUNKS_INDEX_OPEN_FRAME;

/* open's body. */
static void
CHUNKS_INDEX_RUN_NAME(open_body_0) (void *frame_)
{
	CHUNKS_INDEX_OPEN_FRAME *frame = frame_;
	HostError	host_error;
	uint64		size;
	ChunksIndexBacking backing = {.tag = CHUNKS_INDEX_BACKING_FILE};

	if (!host_open_to_read(frame->io, frame->path, frame->path_len, &frame->file, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	frame->file_live = true;
	if (!host_size(frame->io, &frame->file, &size, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		frame->file_live = false;
		host_File_drop(&frame->file);
		return;
	}
	backing.u.file = frame->file;
	frame->file_live = false;
	frame->ok = CHUNKS_INDEX_RUN_NAME(read) (frame->io, frame->path, frame->path_len, (size_t) size, &backing,
											 frame->result, frame->error);
}

/* open's pad: the file, while it holds it. */
static void
CHUNKS_INDEX_RUN_NAME(open_pad_0) (void *frame_)
{
	CHUNKS_INDEX_OPEN_FRAME *frame = frame_;

	if (frame->file_live)
		host_File_drop(&frame->file);
}

/* open opens a run file, reading its fences. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(open) (HostIoReadDoors io, const char *path, size_t path_len, CHUNKS_INDEX_RUN * result,
							 ChunksErrorError *error)
{
	CHUNKS_INDEX_OPEN_FRAME frame = {.io = io, .path = path, .path_len = path_len, .result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(open_body_0), CHUNKS_INDEX_RUN_NAME(open_pad_0));
	return frame.ok;
}

/* read's corrupt, its capture the run's path: the error of a run whose bytes no writer of a run writes. */
static ChunksErrorError
CHUNKS_INDEX_RUN_NAME(read_closure_0) (void *captures)
{
	const StdStr *path = captures;
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("address index run "), STD_FMT_ARG(0), STD_FMT_LIT(" is corrupt")};
	StdFmtArg	args[] = {{path, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, 3, args};

	std_fmt_format_Checked(&message, &arguments);
	return chunks_error_Error_corrupt(&message);
}

/*
 * The state of read across its pad: its arguments, its result and error,
 * where the run's bytes are, which it owns until the run does, its footer
 * and its tail, and the fences it reads.
 */
typedef struct CHUNKS_INDEX_READ_FRAME
{
	HostIoReadDoors io;
	const char *path;
	size_t		path_len;
	size_t		size;
	ChunksIndexBacking backing;
	bool		backing_live;
	CHUNKS_INDEX_RUN *result;
	ChunksErrorError *error;
	StdVec		footer;
	bool		footer_live;
	StdVec		tail;
	bool		tail_live;
	StdVec		keys;
	bool		keys_live;
	ChunksIndexFences fences;
	bool		fences_live;
	bool		ok;
}			CHUNKS_INDEX_READ_FRAME;

/* read's body. */
static void
CHUNKS_INDEX_RUN_NAME(read_body_0) (void *frame_)
{
	CHUNKS_INDEX_READ_FRAME *frame = frame_;
	StdStr		shown = {frame->path, frame->path_len};
	uint64		count;
	uint64		checksum;
	size_t		blocks;
	size_t		fences_at;
	size_t		summed;
	size_t		fences_len;
	StdChunks	chunks;
	StdSizeHint hint;
	const void *chunk;
	size_t		chunk_len;

	if (frame->size < 8 + CHUNKS_INDEX_FOOTER)
	{
		*frame->error = CHUNKS_INDEX_RUN_NAME(read_closure_0) (&shown);
		goto drop_backing;
	}
	if (!chunks_index_Backing_read(&frame->backing, frame->io, frame->path, frame->path_len,
								   (uint64) STD_SUB(usize, frame->size, CHUNKS_INDEX_FOOTER, 865, 45),
								   CHUNKS_INDEX_FOOTER, &frame->footer, frame->error))
		goto drop_backing;
	frame->footer_live = true;
	std_range_from_check(16, frame->footer.len, RUST_FILE, 866, 18);
	if (!(std_wrapping_sub_usize(frame->footer.len, 16) == sizeof(CHUNKS_INDEX_MAGIC) &&
		  memcmp(STD_PTR_AT(frame->footer.ptr, 16, 1), CHUNKS_INDEX_MAGIC, sizeof(CHUNKS_INDEX_MAGIC)) == 0))
	{
		*frame->error = CHUNKS_INDEX_RUN_NAME(read_closure_0) (&shown);
		goto drop_footer;
	}
	std_range_to_check(8, frame->footer.len, RUST_FILE, 869, 46);
	if (8 != sizeof(uint64))
		STD_RESULT_EXPECT_FAILED(869, 63, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	count = std_u64_from_le_bytes(frame->footer.ptr);
	blocks = std_div_ceil_usize((size_t) count, CHUNKS_INDEX_FN(per_block) (), RUST_FILE, 870, 39);
	fences_at = STD_ADD(usize, 8, STD_MUL(usize, blocks, CHUNKS_INDEX_BLOCK, 871, 29), 871, 25);
	if (STD_ADD(usize, STD_ADD(usize, fences_at, STD_MUL(usize, blocks, 32, 872, 24), 872, 12), CHUNKS_INDEX_FOOTER,
				872, 12) != frame->size)
	{
		*frame->error = CHUNKS_INDEX_RUN_NAME(read_closure_0) (&shown);
		goto drop_footer;
	}
	if (!chunks_index_Backing_read(&frame->backing, frame->io, frame->path, frame->path_len, (uint64) fences_at,
								   STD_SUB(usize, frame->size, fences_at, 875, 61), &frame->tail, frame->error))
		goto drop_footer;
	frame->tail_live = true;
	std_range_check(8, 16, frame->footer.len, RUST_FILE, 876, 49);
	if (std_wrapping_sub_usize(16, 8) != sizeof(uint64))
		STD_RESULT_EXPECT_FAILED(876, 68, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	checksum = std_u64_from_le_bytes(STD_PTR_AT(frame->footer.ptr, 8, 1));
	summed = STD_ADD(usize, STD_MUL(usize, blocks, 32, 877, 47), 8, 877, 47);
	std_range_to_check(summed, frame->tail.len, RUST_FILE, 877, 44);
	if (XXH3_64bits(frame->tail.ptr, summed) != checksum)
	{
		*frame->error = CHUNKS_INDEX_RUN_NAME(read_closure_0) (&shown);
		goto drop_tail;
	}
	fences_len = STD_MUL(usize, blocks, 32, 880, 29);
	std_range_to_check(fences_len, frame->tail.len, RUST_FILE, 880, 26);
	std_slice_chunks_check(32, RUST_FILE, 880, 42);
	chunks = std_slice_chunks(frame->tail.ptr, fences_len, 32);
	hint = std_slice_Chunks_size_hint(&chunks);
	std_vec_with_capacity_trusted(&frame->keys, hint, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->keys_live = true;
	std_vec_extend_trusted_reserve(&frame->keys, hint, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	while (std_slice_Chunks_next(&chunks, 1, 1, &chunk, &chunk_len))
	{
		if (chunk_len != sizeof(ChunksAddressAddress))
			STD_RESULT_EXPECT_FAILED(880, 82, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt);
		memcpy(STD_PTR_AT(frame->keys.ptr, frame->keys.len, sizeof(ChunksAddressAddress)), chunk,
			   sizeof(ChunksAddressAddress));
		frame->keys.len = std_wrapping_add_usize(frame->keys.len, 1);
	}
	frame->keys_live = false;
	if (!chunks_index_Fences_new(&frame->keys, &frame->fences))
	{
		*frame->error = CHUNKS_INDEX_RUN_NAME(read_closure_0) (&shown);
		goto drop_tail;
	}
	frame->fences_live = true;
	std_path_to_path_buf(frame->path, frame->path_len, &frame->result->path);
	frame->fences_live = false;
	frame->backing_live = false;
	frame->result->backing = frame->backing;
	frame->result->count = count;
	frame->result->fences = frame->fences;
	frame->result->at = 0;
	frame->ok = true;
drop_tail:
	frame->tail_live = false;
	std_vec_free_Checked(&frame->tail, 1, 1);
drop_footer:
	frame->footer_live = false;
	std_vec_free_Checked(&frame->footer, 1, 1);
	if (frame->ok)
		return;
drop_backing:
	frame->backing_live = false;
	chunks_index_Backing_drop(&frame->backing);
}

/* read's pad: the fences, the keys, the tail, the footer, and where the bytes are, while it owns them. */
static void
CHUNKS_INDEX_RUN_NAME(read_pad_0) (void *frame_)
{
	CHUNKS_INDEX_READ_FRAME *frame = frame_;

	if (frame->fences_live)
		chunks_index_Fences_drop(&frame->fences);
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->tail_live)
		std_vec_free_Checked(&frame->tail, 1, 1);
	if (frame->footer_live)
		std_vec_free_Checked(&frame->footer, 1, 1);
	if (frame->backing_live)
		chunks_index_Backing_drop(&frame->backing);
}

/* read opens a run of a size from where its bytes are, which it takes, reading its fences. */
static bool
CHUNKS_INDEX_RUN_NAME(read) (HostIoReadDoors io, const char *path, size_t path_len, size_t size,
							 ChunksIndexBacking *backing, CHUNKS_INDEX_RUN * result, ChunksErrorError *error)
{
	CHUNKS_INDEX_READ_FRAME frame = {.io = io, .path = path, .path_len = path_len, .size = size,
	.backing = *backing, .backing_live = true, .result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(read_body_0), CHUNKS_INDEX_RUN_NAME(read_pad_0));
	return frame.ok;
}

/* path returns the run's file. */
CHUNKS_INDEX_DEF const char *
CHUNKS_INDEX_RUN_NAME(path) (const CHUNKS_INDEX_RUN * self, size_t *result_len)
{
	*result_len = self->path.bytes.len;
	return self->path.bytes.ptr;
}

/* len returns the number of entries. */
CHUNKS_INDEX_DEF uint64
CHUNKS_INDEX_RUN_NAME(len) (const CHUNKS_INDEX_RUN * self)
{
	return self->count;
}

/* is_empty reports whether the run has no entries. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(is_empty) (const CHUNKS_INDEX_RUN * self)
{
	return self->count == 0;
}

/* per_block returns the number of entries a block holds. */
CHUNKS_INDEX_DEF size_t
CHUNKS_INDEX_RUN_NAME(per_block) (void)
{
	return CHUNKS_INDEX_FN(per_block) ();
}

/* blocks returns the number of blocks. */
CHUNKS_INDEX_DEF size_t
CHUNKS_INDEX_RUN_NAME(blocks) (const CHUNKS_INDEX_RUN * self)
{
	size_t		keys_len;

	(void) chunks_index_Fences_keys(&self->fences, &keys_len);
	return keys_len;
}

/* fence returns the first address of the block at the position. */
CHUNKS_INDEX_DEF ChunksAddressAddress
CHUNKS_INDEX_RUN_NAME(fence) (const CHUNKS_INDEX_RUN * self, size_t i)
{
	size_t		keys_len;
	const ChunksAddressAddress *keys = chunks_index_Fences_keys(&self->fences, &keys_len);

	std_index_check(i, keys_len, RUST_FILE, 912, 9);
	return keys[i];
}

/*
 * The state of block across its pad: its arguments, its result and error,
 * the block's bytes, the message of a block that fails its checksum, and
 * whether the entries it decodes are live.
 */
typedef struct CHUNKS_INDEX_BLOCK_FRAME
{
	const		CHUNKS_INDEX_RUN *self;
	HostIoReadDoors io;
	size_t		i;
	StdVec	   *result;
	ChunksErrorError *error;
	StdVec		bytes;
	bool		bytes_live;
	StdString	message;
	bool		message_live;
	bool		entries_live;
	bool		ok;
}			CHUNKS_INDEX_BLOCK_FRAME;

/* block's body. */
static void
CHUNKS_INDEX_RUN_NAME(block_body_0) (void *frame_)
{
	CHUNKS_INDEX_BLOCK_FRAME *frame = frame_;
	const		CHUNKS_INDEX_RUN *self = frame->self;
	size_t		entries = std_min_usize(STD_SUB(usize, (size_t) self->count,
												STD_MUL(usize, frame->i, CHUNKS_INDEX_FN(per_block) (), 918, 46), 918,
												23),
										CHUNKS_INDEX_FN(per_block) ());
	uint64		offset = (uint64) STD_ADD(usize, 8, STD_MUL(usize, frame->i, CHUNKS_INDEX_BLOCK, 919, 27), 919, 22);
	uint8		sum[8];
	size_t		len;
	size_t		chunk_size;
	StdChunks	chunks;
	StdSizeHint hint;
	const void *chunk;
	size_t		chunk_len;
	size_t		keys_len;
	const ChunksAddressAddress *keys;

	if (!chunks_index_Backing_read(&self->backing, frame->io, self->path.bytes.ptr, self->path.bytes.len, offset,
								   CHUNKS_INDEX_BLOCK, &frame->bytes, frame->error))
		return;
	frame->bytes_live = true;
	offset = STD_ADD(u64, self->at, offset, 921, 22);
	std_range_to_check(CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, frame->bytes.len, RUST_FILE, 922, 45);
	std_u64_to_le_bytes(XXH3_64bits(frame->bytes.ptr, CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM), sum);
	std_range_from_check(CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, frame->bytes.len, RUST_FILE, 922, 84);
	if (!(std_wrapping_sub_usize(frame->bytes.len, CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM) == sizeof(sum) &&
		  memcmp(sum, STD_PTR_AT(frame->bytes.ptr, CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, 1), sizeof(sum)) == 0))
	{
		StdStr		shown = {self->path.bytes.ptr, self->path.bytes.len};
		StdPathBuf	path;
		StdFmtPiece pieces[] = {STD_FMT_LIT("address index block at offset "), STD_FMT_ARG(0),
		STD_FMT_LIT(" of file \""), STD_FMT_ARG(1), STD_FMT_LIT("\" fails its checksum")};
		StdFmtArg	args[] = {{&offset, std_u64_Display_fmt}, {&shown, std_path_Display_fmt}};
		StdFmtArguments arguments = {pieces, 5, args};

		std_fmt_format_Checked(&frame->message, &arguments);
		frame->message_live = true;
		std_PathBuf_Clone_clone(&self->path, &path);
		frame->message_live = false;
		frame->error->tag = CHUNKS_ERROR_ERROR_DAMAGED;
		frame->error->u.damaged.path = path;
		frame->error->u.damaged.offset = offset;
		frame->error->u.damaged.message = frame->message;
		goto drop_bytes;
	}
	len = STD_MUL(usize, entries, CHUNKS_INDEX_FN(entry) (), 929, 30);
	std_range_to_check(len, frame->bytes.len, RUST_FILE, 929, 27);
	chunk_size = CHUNKS_INDEX_FN(entry) ();
	std_slice_chunks_check(chunk_size, RUST_FILE, 931, 19);
	chunks = std_slice_chunks(frame->bytes.ptr, len, chunk_size);
	hint = std_slice_Chunks_size_hint(&chunks);
	std_vec_with_capacity_trusted(frame->result, hint, CHUNKS_INDEX_ENTRIES);
	frame->entries_live = true;
	std_vec_extend_trusted_reserve(frame->result, hint, CHUNKS_INDEX_ENTRIES);
	while (std_slice_Chunks_next(&chunks, 1, 1, &chunk, &chunk_len))
	{
		CHUNKS_INDEX_ENTRY entry;

		std_range_to_check(32, chunk_len, RUST_FILE, 931, 58);
		if (32 != sizeof(entry.f0.f0))
			STD_RESULT_EXPECT_FAILED(931, 76, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
									 std_TryFromSliceError_Debug_fmt);
		memcpy(entry.f0.f0, chunk, sizeof(entry.f0.f0));
		std_range_from_check(32, chunk_len, RUST_FILE, 931, 97);
		entry.f1 = CHUNKS_INDEX_FIXED(read) (STD_PTR_AT(chunk, 32, 1), std_wrapping_sub_usize(chunk_len, 32));
		memcpy(STD_PTR_AT(frame->result->ptr, frame->result->len, sizeof(entry)), &entry, sizeof(entry));
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	keys = chunks_index_Fences_keys(&self->fences, &keys_len);
	if (!chunks_index_block_fits(sizeof(CHUNKS_INDEX_ENTRY), frame->result->ptr, frame->result->len, keys, keys_len,
								 frame->i))
	{
		*frame->error = CHUNKS_INDEX_RUN_NAME(damaged) (self, frame->i, "entries out of order or outside its fences",
														42);
		frame->entries_live = false;
		std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
		goto drop_bytes;
	}
	frame->entries_live = false;
	frame->ok = true;
drop_bytes:
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* block's pad: the message, the entries, and the bytes, while it holds them. */
static void
CHUNKS_INDEX_RUN_NAME(block_pad_0) (void *frame_)
{
	CHUNKS_INDEX_BLOCK_FRAME *frame = frame_;

	if (frame->message_live)
		std_vec_free_Checked(&frame->message, 1, 1);
	if (frame->entries_live)
		std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * block reads the entries of the block at the position once its checksum
 * matches, refusing a damaged block by the run's file and the block's
 * offset.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(block) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io, size_t i, StdVec *result,
							  ChunksErrorError *error)
{
	CHUNKS_INDEX_BLOCK_FRAME frame = {.self = self, .io = io, .i = i, .result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(block_body_0), CHUNKS_INDEX_RUN_NAME(block_pad_0));
	return frame.ok;
}

/* The state of damaged across its pad: its arguments, its result, and the message it formats. */
typedef struct CHUNKS_INDEX_DAMAGED_FRAME
{
	const		CHUNKS_INDEX_RUN *self;
	size_t		i;
	const char *holds;
	size_t		holds_len;
	ChunksErrorError result;
	StdString	message;
	bool		message_live;
}			CHUNKS_INDEX_DAMAGED_FRAME;

/* damaged's body. */
static void
CHUNKS_INDEX_RUN_NAME(damaged_body_0) (void *frame_)
{
	CHUNKS_INDEX_DAMAGED_FRAME *frame = frame_;
	const		CHUNKS_INDEX_RUN *self = frame->self;
	uint64		offset = STD_ADD(u64, self->at,
								 (uint64) STD_ADD(usize, 8, STD_MUL(usize, frame->i, CHUNKS_INDEX_BLOCK, 941, 37), 941,
												  32),
								 941, 22);
	StdStr		shown = {self->path.bytes.ptr, self->path.bytes.len};
	StdStr		holds = {frame->holds, frame->holds_len};
	StdPathBuf	path;
	StdFmtPiece pieces[] = {STD_FMT_LIT("address index block at offset "), STD_FMT_ARG(0), STD_FMT_LIT(" of file \""),
	STD_FMT_ARG(1), STD_FMT_LIT("\" holds "), STD_FMT_ARG(2)};
	StdFmtArg	args[] = {{&offset, std_u64_Display_fmt}, {&shown, std_path_Display_fmt},
	{&holds, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 6, args};

	std_fmt_format_Checked(&frame->message, &arguments);
	frame->message_live = true;
	std_PathBuf_Clone_clone(&self->path, &path);
	frame->message_live = false;
	frame->result.tag = CHUNKS_ERROR_ERROR_DAMAGED;
	frame->result.u.damaged.path = path;
	frame->result.u.damaged.offset = offset;
	frame->result.u.damaged.message = frame->message;
}

/* damaged's pad: the message, while the path's clone can unwind. */
static void
CHUNKS_INDEX_RUN_NAME(damaged_pad_0) (void *frame_)
{
	CHUNKS_INDEX_DAMAGED_FRAME *frame = frame_;

	if (frame->message_live)
		std_vec_free_Checked(&frame->message, 1, 1);
}

/*
 * damaged returns the error of the block at a position that holds what no
 * writer of a run writes, by the run's file and the block's offset.
 */
CHUNKS_INDEX_DEF ChunksErrorError
CHUNKS_INDEX_RUN_NAME(damaged) (const CHUNKS_INDEX_RUN * self, size_t i, const char *holds, size_t holds_len)
{
	CHUNKS_INDEX_DAMAGED_FRAME frame = {.self = self, .i = i, .holds = holds, .holds_len = holds_len};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(damaged_body_0), CHUNKS_INDEX_RUN_NAME(damaged_pad_0));
	return frame.result;
}

/*
 * The state of get across its pad: its arguments, its result and error,
 * whether it still owns keep, and the block it reads.
 */
typedef struct CHUNKS_INDEX_GET_FRAME
{
	const		CHUNKS_INDEX_RUN *self;
	HostIoReadDoors io;
	const ChunksAddressAddress *address;
	bool		(*keep) (void *captures, const CHUNKS_INDEX_V * value);
	void	   *keep_captures;
	void		(*keep_drop) (void *);
	bool		keep_live;
	bool	   *has_result;
	CHUNKS_INDEX_V *result;
	ChunksErrorError *error;
	StdVec		block;
	bool		block_live;
	bool		ok;
}			CHUNKS_INDEX_GET_FRAME;

/* get's body. */
static void
CHUNKS_INDEX_RUN_NAME(get_body_0) (void *frame_)
{
	CHUNKS_INDEX_GET_FRAME *frame = frame_;
	size_t		keys_len;
	const ChunksAddressAddress *keys = chunks_index_Fences_keys(&frame->self->fences, &keys_len);
	size_t		first;
	size_t		end;

	chunks_index_block_range(keys, keys_len, frame->address, &first, &end);
	for (size_t i = first, i_end = end; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		size_t		at;
		const		CHUNKS_INDEX_ENTRY *tail;
		bool		found = false;

		if (!CHUNKS_INDEX_RUN_NAME(block) (frame->self, frame->io, i, &frame->block, frame->error))
			goto drop_keep;
		frame->block_live = true;
		if (!chunks_index_first_with(sizeof(CHUNKS_INDEX_ENTRY), frame->block.ptr, frame->block.len, frame->address,
									 &at))
		{
			frame->block_live = false;
			std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
			continue;
		}
		std_range_from_check(at, frame->block.len, RUST_FILE, 953, 39);
		tail = STD_PTR_AT(frame->block.ptr, at, sizeof(CHUNKS_INDEX_ENTRY));
		for (size_t j = 0, j_len = std_wrapping_sub_usize(frame->block.len, at); j < j_len; j++)
		{
			CHUNKS_INDEX_V value;

			if (!chunks_address_Address_same(&tail[j].f0, frame->address))
				break;
			value = tail[j].f1;
			if (frame->keep(frame->keep_captures, &value))
			{
				*frame->result = value;
				found = true;
				break;
			}
		}
		frame->block_live = false;
		std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
		if (found)
		{
			*frame->has_result = true;
			frame->ok = true;
			goto drop_keep;
		}
	}
	*frame->has_result = false;
	frame->ok = true;
drop_keep:
	frame->keep_live = false;
	if (frame->keep_drop != NULL)
		frame->keep_drop(frame->keep_captures);
}

/* get's pad: the block, and keep while it owns it. */
static void
CHUNKS_INDEX_RUN_NAME(get_pad_0) (void *frame_)
{
	CHUNKS_INDEX_GET_FRAME *frame = frame_;

	if (frame->block_live)
		std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
	if (frame->keep_live && frame->keep_drop != NULL)
		frame->keep_drop(frame->keep_captures);
}

/* get returns the first value of the address that keep keeps, if the run has one, taking keep. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(get) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io, const ChunksAddressAddress *address,
							bool (*keep) (void *captures, const CHUNKS_INDEX_V * value), void *keep_captures,
							void (*keep_drop) (void *), bool *has_result, CHUNKS_INDEX_V * result,
							ChunksErrorError *error)
{
	CHUNKS_INDEX_GET_FRAME frame = {.self = self, .io = io, .address = address, .keep = keep,
		.keep_captures = keep_captures, .keep_drop = keep_drop, .keep_live = true, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(get_body_0), CHUNKS_INDEX_RUN_NAME(get_pad_0));
	return frame.ok;
}

/* range's closure_0, its capture the low address: whether a fence is below it. */
static bool
CHUNKS_INDEX_RUN_NAME(range_closure_0) (void *captures, const void *f)
{
	return chunks_address_Address_PartialOrd_lt(f, captures);
}

/* range's closure_1, its capture the high address: whether a fence is at or below it. */
static bool
CHUNKS_INDEX_RUN_NAME(range_closure_1) (void *captures, const void *f)
{
	return chunks_address_Address_PartialOrd_le(f, captures);
}

/* The state of range across its pad: its arguments, its result and error, and the block it reads. */
typedef struct CHUNKS_INDEX_RANGE_FRAME
{
	const		CHUNKS_INDEX_RUN *self;
	HostIoReadDoors io;
	const ChunksAddressAddress *low;
	const ChunksAddressAddress *high;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		out_live;
	StdVec		block;
	bool		block_live;
	bool		ok;
}			CHUNKS_INDEX_RANGE_FRAME;

/* range's body. */
static void
CHUNKS_INDEX_RUN_NAME(range_body_0) (void *frame_)
{
	CHUNKS_INDEX_RANGE_FRAME *frame = frame_;
	size_t		keys_len;
	const ChunksAddressAddress *keys = chunks_index_Fences_keys(&frame->self->fences, &keys_len);
	size_t		first = std_saturating_sub_usize(std_slice_partition_point(keys, keys_len,
																		   sizeof(ChunksAddressAddress),
																		   CHUNKS_INDEX_RUN_NAME(range_closure_0),
																		   (void *) frame->low, NULL),
												 1);
	size_t		end = std_slice_partition_point(keys, keys_len, sizeof(ChunksAddressAddress),
												CHUNKS_INDEX_RUN_NAME(range_closure_1), (void *) frame->high, NULL);

	*frame->result = (StdVec) STD_VEC_NEW;
	frame->out_live = true;
	for (size_t i = first, i_end = end; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		if (!CHUNKS_INDEX_RUN_NAME(block) (frame->self, frame->io, i, &frame->block, frame->error))
		{
			frame->out_live = false;
			std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
			return;
		}
		frame->block_live = true;
		for (size_t j = 0, j_len = frame->block.len; j < j_len; j++)
		{
			const		CHUNKS_INDEX_ENTRY *e = STD_PTR_AT(frame->block.ptr, j, sizeof(CHUNKS_INDEX_ENTRY));

			if (chunks_address_Address_PartialOrd_ge(&e->f0, frame->low) &&
				chunks_address_Address_PartialOrd_le(&e->f0, frame->high))
				std_vec_extend_one(frame->result, e, 0, CHUNKS_INDEX_ENTRIES);
		}
		frame->block_live = false;
		std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
	}
	frame->out_live = false;
	frame->ok = true;
}

/* range's pad: the block, and the entries it gathers. */
static void
CHUNKS_INDEX_RUN_NAME(range_pad_0) (void *frame_)
{
	CHUNKS_INDEX_RANGE_FRAME *frame = frame_;

	if (frame->block_live)
		std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
	if (frame->out_live)
		std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
}

/* range returns the entries whose addresses lie between the two, inclusive, in address order. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(range) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io, const ChunksAddressAddress *low,
							  const ChunksAddressAddress *high, StdVec *result, ChunksErrorError *error)
{
	CHUNKS_INDEX_RANGE_FRAME frame = {.self = self, .io = io, .low = low, .high = high, .result = result,
	.error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(range_body_0), CHUNKS_INDEX_RUN_NAME(range_pad_0));
	return frame.ok;
}

/* The state of entries across its pad: its arguments, its result and error, and the block it reads. */
typedef struct CHUNKS_INDEX_RUN_ENTRIES_FRAME
{
	const		CHUNKS_INDEX_RUN *self;
	HostIoReadDoors io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		out_live;
	StdVec		block;
	bool		block_live;
	bool		ok;
}			CHUNKS_INDEX_RUN_ENTRIES_FRAME;

/* entries' body. */
static void
CHUNKS_INDEX_RUN_NAME(entries_body_0) (void *frame_)
{
	CHUNKS_INDEX_RUN_ENTRIES_FRAME *frame = frame_;
	size_t		keys_len;

#ifdef DOLTGRES_TEST_CHUNKS
	{
		HostLocalLocal read = host_local_BackendLocal_get(&CHUNKS_INDEX_ENTRIES_READ.local, &STD_ATOMIC_U64_TYPE);

		(void) std_AtomicU64_fetch_add_relaxed((StdAtomicU64 *) host_local_Local_Deref_deref(&read,
																							 &STD_ATOMIC_U64_TYPE),
											   1);
		host_local_Local_drop(&read, &STD_ATOMIC_U64_TYPE);
	}
#endif
	std_vec_with_capacity_Checked(frame->result, (size_t) frame->self->count, CHUNKS_INDEX_ENTRIES);
	frame->out_live = true;
	(void) chunks_index_Fences_keys(&frame->self->fences, &keys_len);
	for (size_t i = 0, i_end = keys_len; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		if (!CHUNKS_INDEX_RUN_NAME(block) (frame->self, frame->io, i, &frame->block, frame->error))
		{
			frame->out_live = false;
			std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
			return;
		}
		frame->block_live = true;
		std_vec_extend_from_slice_Checked(frame->result, frame->block.ptr, frame->block.len, CHUNKS_INDEX_ENTRIES);
		frame->block_live = false;
		std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
	}
	frame->out_live = false;
	frame->ok = true;
}

/* entries' pad: the block, and the entries it gathers. */
static void
CHUNKS_INDEX_RUN_NAME(entries_pad_0) (void *frame_)
{
	CHUNKS_INDEX_RUN_ENTRIES_FRAME *frame = frame_;

	if (frame->block_live)
		std_vec_free_Checked(&frame->block, CHUNKS_INDEX_ENTRIES);
	if (frame->out_live)
		std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
}

/* entries returns every entry in address order. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_RUN_NAME(entries) (const CHUNKS_INDEX_RUN * self, HostIoReadDoors io, StdVec *result,
								ChunksErrorError *error)
{
	CHUNKS_INDEX_RUN_ENTRIES_FRAME frame = {.self = self, .io = io, .result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_RUN_NAME(entries_body_0), CHUNKS_INDEX_RUN_NAME(entries_pad_0));
	return frame.ok;
}

/* Run<V>'s drop glue: its path, where its bytes are, and its fences. */
CHUNKS_INDEX_DEF void
CHUNKS_INDEX_RUN_NAME(drop) (void *self_)
{
	CHUNKS_INDEX_RUN *self = self_;

	std_vec_free_Checked(&self->path.bytes, 1, 1);
	chunks_index_Backing_drop(&self->backing);
	chunks_index_Fences_drop(&self->fences);
}

/* AddressIndex<V>'s Default: no runs. */
CHUNKS_INDEX_DEF CHUNKS_INDEX_INDEX
CHUNKS_INDEX_INDEX_NAME(Default_default) (void)
{
	CHUNKS_INDEX_INDEX result = {.runs = STD_VEC_NEW};

	return result;
}

/* AddressIndex<V>'s Clone: an index sharing the same runs. */
CHUNKS_INDEX_DEF CHUNKS_INDEX_INDEX
CHUNKS_INDEX_INDEX_NAME(Clone_clone) (const CHUNKS_INDEX_INDEX * self)
{
	CHUNKS_INDEX_INDEX result;

	std_slice_to_vec_clone(&result.runs, self->runs.ptr, self->runs.len, CHUNKS_INDEX_ARCS,
						   CHUNKS_INDEX_ARC_RUN(Clone_clone), CHUNKS_INDEX_ARC_RUN(drop));
	return result;
}

/* new returns an index of the runs, newest last, taking them. */
CHUNKS_INDEX_DEF CHUNKS_INDEX_INDEX
CHUNKS_INDEX_INDEX_NAME(new) (StdVec *runs)
{
	CHUNKS_INDEX_INDEX result;

	std_vec_from_iter_in_place_map(runs, sizeof(CHUNKS_INDEX_RUN), _Alignof(CHUNKS_INDEX_RUN),
								   CHUNKS_INDEX_RUN_NAME(drop), sizeof(StdArcInner *), CHUNKS_INDEX_ARC_RUN(drop),
								   CHUNKS_INDEX_ARC_RUN(new), &result.runs);
	return result;
}

/* shared returns an index of runs that other indexes may hold open too, newest last, taking them. */
CHUNKS_INDEX_DEF CHUNKS_INDEX_INDEX
CHUNKS_INDEX_INDEX_NAME(shared) (StdVec *runs)
{
	CHUNKS_INDEX_INDEX result = {.runs = *runs};

	return result;
}

/* The state of add across its pad: its arguments, and the new reference to the run. */
typedef struct CHUNKS_INDEX_ADD_FRAME
{
	CHUNKS_INDEX_INDEX *self;
	CHUNKS_INDEX_RUN *run;
	StdArcInner *arc;
	bool		arc_live;
}			CHUNKS_INDEX_ADD_FRAME;

/* add's body. */
static void
CHUNKS_INDEX_INDEX_NAME(add_body_0) (void *frame_)
{
	CHUNKS_INDEX_ADD_FRAME *frame = frame_;

	frame->arc = std_arc_new_dropping(frame->run, sizeof(CHUNKS_INDEX_RUN), _Alignof(CHUNKS_INDEX_RUN),
									  CHUNKS_INDEX_RUN_NAME(drop));
	frame->arc_live = true;
	std_vec_push_Checked(&frame->self->runs, &frame->arc, CHUNKS_INDEX_ARCS);
	frame->arc_live = false;
}

/* add's pad: the new reference, should the push unwind. */
static void
CHUNKS_INDEX_INDEX_NAME(add_pad_0) (void *frame_)
{
	CHUNKS_INDEX_ADD_FRAME *frame = frame_;

	if (frame->arc_live)
		CHUNKS_INDEX_ARC_RUN(drop) (&frame->arc);
}

/* add adds a run as the newest, taking it. */
CHUNKS_INDEX_DEF void
CHUNKS_INDEX_INDEX_NAME(add) (CHUNKS_INDEX_INDEX * self, CHUNKS_INDEX_RUN * run)
{
	CHUNKS_INDEX_ADD_FRAME frame = {.self = self, .run = run};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(add_body_0), CHUNKS_INDEX_INDEX_NAME(add_pad_0));
}

/* runs returns the runs, newest last. */
CHUNKS_INDEX_DEF StdArcInner *const *
CHUNKS_INDEX_INDEX_NAME(runs) (const CHUNKS_INDEX_INDEX * self, size_t *result_len)
{
	*result_len = self->runs.len;
	return self->runs.ptr;
}

/* The state of fences across its pad: its arguments, and the fences it gathers. */
typedef struct CHUNKS_INDEX_FENCES_FRAME
{
	const		CHUNKS_INDEX_INDEX *self;
	StdVec		result;
	bool		result_live;
}			CHUNKS_INDEX_FENCES_FRAME;

/* fences' body. */
static void
CHUNKS_INDEX_INDEX_NAME(fences_body_0) (void *frame_)
{
	CHUNKS_INDEX_FENCES_FRAME *frame = frame_;
	StdSizeHint hint = {frame->self->runs.len, true, frame->self->runs.len};

	std_vec_with_capacity_trusted(&frame->result, hint, sizeof(const ChunksIndexFences *),
								  _Alignof(const ChunksIndexFences *));
	frame->result_live = true;
	std_vec_extend_trusted_reserve(&frame->result, hint, sizeof(const ChunksIndexFences *),
								   _Alignof(const ChunksIndexFences *));
	for (size_t i = 0, i_len = frame->self->runs.len; i < i_len; i++)
	{
		const		CHUNKS_INDEX_RUN *run = std_arc_Deref_deref(STD_VEC_AT(&frame->self->runs, StdArcInner *, i),
																_Alignof(CHUNKS_INDEX_RUN));

		STD_VEC_AT(&frame->result, const ChunksIndexFences *, frame->result.len) = &run->fences;
		frame->result.len = std_wrapping_add_usize(frame->result.len, 1);
	}
	frame->result_live = false;
}

/* fences' pad: the fences it gathers. */
static void
CHUNKS_INDEX_INDEX_NAME(fences_pad_0) (void *frame_)
{
	CHUNKS_INDEX_FENCES_FRAME *frame = frame_;

	if (frame->result_live)
		std_vec_free_Checked(&frame->result, sizeof(const ChunksIndexFences *), _Alignof(const ChunksIndexFences *));
}

/* fences returns each run's fences, oldest first, as the proved lookups take them. */
CHUNKS_INDEX_DEF StdVec
CHUNKS_INDEX_INDEX_NAME(fences) (const CHUNKS_INDEX_INDEX * self)
{
	CHUNKS_INDEX_FENCES_FRAME frame = {.self = self};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(fences_body_0), CHUNKS_INDEX_INDEX_NAME(fences_pad_0));
	return frame.result;
}

/*
 * unread returns the error of a lookup that ended without an answer, taking
 * it: the read's own, or that of a block that doesn't fit its run's fences.
 */
CHUNKS_INDEX_DEF ChunksErrorError
CHUNKS_INDEX_INDEX_NAME(unread) (const CHUNKS_INDEX_INDEX * self, ChunksIndexUnreadError *unread)
{
	const		CHUNKS_INDEX_RUN *run;

	switch (unread->tag)
	{
		case CHUNKS_INDEX_UNREAD_SOURCE:
			return unread->u.source;
		case CHUNKS_INDEX_UNREAD_UNFIT:
			std_index_check(unread->u.unfit.f0, self->runs.len, RUST_FILE, 1037, 51);
			run = std_arc_Deref_deref(STD_VEC_AT(&self->runs, StdArcInner *, unread->u.unfit.f0),
									  _Alignof(CHUNKS_INDEX_RUN));
			return CHUNKS_INDEX_RUN_NAME(damaged) (run, unread->u.unfit.f1,
												   "entries out of order or outside its fences", 42);
	}
	pg_unreachable();
}

/* The state of get across its pad: its arguments, its result and error, and the runs' fences. */
typedef struct CHUNKS_INDEX_INDEX_GET_FRAME
{
	const		CHUNKS_INDEX_INDEX *self;
	HostIoReadDoors io;
	const ChunksAddressAddress *address;
	bool	   *has_result;
	CHUNKS_INDEX_V *result;
	ChunksErrorError *error;
	StdVec		fences;
	bool		fences_live;
	bool		ok;
}			CHUNKS_INDEX_INDEX_GET_FRAME;

/* get's body. */
static void
CHUNKS_INDEX_INDEX_NAME(get_body_0) (void *frame_)
{
	CHUNKS_INDEX_INDEX_GET_FRAME *frame = frame_;
	ChunksIndexBlocks blocks = {{&CHUNKS_INDEX_RUNS_OPS, frame->self}};
	ChunksIndexSource source = {&CHUNKS_INDEX_BLOCKS_SOURCE_OPS, &blocks};
	std_TupleUsizeUsize_t found_at;
	CHUNKS_INDEX_V value;
	bool		has;
	ChunksIndexUnreadError unread;

	frame->fences = CHUNKS_INDEX_INDEX_NAME(fences) (frame->self);
	frame->fences_live = true;
	if (CHUNKS_INDEX_FIRST_IN_INDEX(source, &frame->io, frame->fences.ptr, frame->fences.len, frame->address, &has,
									&found_at, &value, &unread))
	{
		*frame->has_result = has;
		if (has)
			*frame->result = value;
		frame->ok = true;
	}
	else
		*frame->error = CHUNKS_INDEX_INDEX_NAME(unread) (frame->self, &unread);
	frame->fences_live = false;
	std_vec_free_Checked(&frame->fences, sizeof(const ChunksIndexFences *), _Alignof(const ChunksIndexFences *));
}

/* get's pad: the runs' fences. */
static void
CHUNKS_INDEX_INDEX_NAME(get_pad_0) (void *frame_)
{
	CHUNKS_INDEX_INDEX_GET_FRAME *frame = frame_;

	if (frame->fences_live)
		std_vec_free_Checked(&frame->fences, sizeof(const ChunksIndexFences *), _Alignof(const ChunksIndexFences *));
}

/* get returns the value of the address from the newest run that has one. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_INDEX_NAME(get) (const CHUNKS_INDEX_INDEX * self, HostIoReadDoors io, const ChunksAddressAddress *address,
							  bool *has_result, CHUNKS_INDEX_V * result, ChunksErrorError *error)
{
	CHUNKS_INDEX_INDEX_GET_FRAME frame = {.self = self, .io = io, .address = address, .has_result = has_result,
	.result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(get_body_0), CHUNKS_INDEX_INDEX_NAME(get_pad_0));
	return frame.ok;
}

/* The state of all across its pad: its arguments, its result and error, and a run's entries of the address. */
typedef struct CHUNKS_INDEX_ALL_FRAME
{
	const		CHUNKS_INDEX_INDEX *self;
	HostIoReadDoors io;
	const ChunksAddressAddress *address;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		out_live;
	StdVec		found;
	bool		found_live;
	bool		ok;
}			CHUNKS_INDEX_ALL_FRAME;

/* all's body. */
static void
CHUNKS_INDEX_INDEX_NAME(all_body_0) (void *frame_)
{
	CHUNKS_INDEX_ALL_FRAME *frame = frame_;

	*frame->result = (StdVec) STD_VEC_NEW;
	frame->out_live = true;
	for (size_t i_end = 0, i = frame->self->runs.len; i > i_end;)
	{
		const		CHUNKS_INDEX_RUN *run;

		i--;
		run = std_arc_Deref_deref(STD_VEC_AT(&frame->self->runs, StdArcInner *, i), _Alignof(CHUNKS_INDEX_RUN));
		if (!CHUNKS_INDEX_RUN_NAME(range) (run, frame->io, frame->address, frame->address, &frame->found,
										   frame->error))
		{
			frame->out_live = false;
			std_vec_free_Checked(frame->result, sizeof(CHUNKS_INDEX_V), _Alignof(CHUNKS_INDEX_V));
			return;
		}
		frame->found_live = true;
		for (size_t j = 0, j_len = frame->found.len; j < j_len; j++)
		{
			const		CHUNKS_INDEX_V *value = &STD_VEC_AT(&frame->found, CHUNKS_INDEX_ENTRY, j).f1;
			bool		contains = false;

			for (size_t k = 0, k_len = frame->result->len; k < k_len; k++)
			{
				if (CHUNKS_INDEX_V_EQ(&STD_VEC_AT(frame->result, CHUNKS_INDEX_V, k), value))
				{
					contains = true;
					break;
				}
			}
			if (!contains)
				std_vec_push_Checked(frame->result, value, sizeof(CHUNKS_INDEX_V), _Alignof(CHUNKS_INDEX_V));
		}
		frame->found_live = false;
		std_vec_free_Checked(&frame->found, CHUNKS_INDEX_ENTRIES);
	}
	frame->out_live = false;
	frame->ok = true;
}

/* all's pad: a run's entries, and the values it gathers. */
static void
CHUNKS_INDEX_INDEX_NAME(all_pad_0) (void *frame_)
{
	CHUNKS_INDEX_ALL_FRAME *frame = frame_;

	if (frame->found_live)
		std_vec_free_Checked(&frame->found, CHUNKS_INDEX_ENTRIES);
	if (frame->out_live)
		std_vec_free_Checked(frame->result, sizeof(CHUNKS_INDEX_V), _Alignof(CHUNKS_INDEX_V));
}

/* all returns every value of the address, each once, from the newest run to the oldest. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_INDEX_NAME(all) (const CHUNKS_INDEX_INDEX * self, HostIoReadDoors io, const ChunksAddressAddress *address,
							  StdVec *result, ChunksErrorError *error)
{
	CHUNKS_INDEX_ALL_FRAME frame = {.self = self, .io = io, .address = address, .result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(all_body_0), CHUNKS_INDEX_INDEX_NAME(all_pad_0));
	return frame.ok;
}

/* The state of with_prefix across its pad: its arguments, its result and error, and a run's entries of the range. */
typedef struct CHUNKS_INDEX_WITH_PREFIX_FRAME
{
	const		CHUNKS_INDEX_INDEX *self;
	HostIoIo   *io;
	const char *prefix;
	size_t		prefix_len;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		out_live;
	StdVec		found;
	bool		found_live;
	bool		ok;
}			CHUNKS_INDEX_WITH_PREFIX_FRAME;

/* with_prefix's body. */
static void
CHUNKS_INDEX_INDEX_NAME(with_prefix_body_0) (void *frame_)
{
	CHUNKS_INDEX_WITH_PREFIX_FRAME *frame = frame_;
	chunks_TupleAddressAddress_t range;

	if (!chunks_address_Address_prefix_range(frame->prefix, frame->prefix_len, &range))
	{
		*frame->result = (StdVec) STD_VEC_NEW;
		frame->ok = true;
		return;
	}
	*frame->result = (StdVec) STD_VEC_NEW;
	frame->out_live = true;
	for (size_t i = 0, i_len = frame->self->runs.len; i < i_len; i++)
	{
		const		CHUNKS_INDEX_RUN *run = std_arc_Deref_deref(STD_VEC_AT(&frame->self->runs, StdArcInner *, i),
																_Alignof(CHUNKS_INDEX_RUN));

		if (!CHUNKS_INDEX_RUN_NAME(range) (run, HOST_IO_IO_READ_DOORS(frame->io), &range.f0, &range.f1,
										   &frame->found, frame->error))
		{
			frame->out_live = false;
			std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
			return;
		}
		frame->found_live = true;
		for (size_t j = 0, j_len = frame->found.len; j < j_len; j++)
		{
			const		CHUNKS_INDEX_ENTRY *e = STD_PTR_AT(frame->found.ptr, j, sizeof(CHUNKS_INDEX_ENTRY));

			if (chunks_address_Address_has_prefix(&e->f0, frame->prefix, frame->prefix_len))
				std_vec_extend_one(frame->result, e, 0, CHUNKS_INDEX_ENTRIES);
		}
		frame->found_live = false;
		std_vec_free_Checked(&frame->found, CHUNKS_INDEX_ENTRIES);
	}
	frame->out_live = false;
	frame->ok = true;
}

/* with_prefix's pad: a run's entries, and the entries it gathers. */
static void
CHUNKS_INDEX_INDEX_NAME(with_prefix_pad_0) (void *frame_)
{
	CHUNKS_INDEX_WITH_PREFIX_FRAME *frame = frame_;

	if (frame->found_live)
		std_vec_free_Checked(&frame->found, CHUNKS_INDEX_ENTRIES);
	if (frame->out_live)
		std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
}

/* with_prefix returns the entries in the index whose address's text starts with the prefix. */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_INDEX_NAME(with_prefix) (const CHUNKS_INDEX_INDEX * self, HostIoIo *io, const char *prefix,
									  size_t prefix_len, StdVec *result, ChunksErrorError *error)
{
	CHUNKS_INDEX_WITH_PREFIX_FRAME frame = {.self = self, .io = io, .prefix = prefix, .prefix_len = prefix_len,
	.result = result, .error = error};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(with_prefix_body_0), CHUNKS_INDEX_INDEX_NAME(with_prefix_pad_0));
	return frame.ok;
}

/*
 * to_merge returns where the newest runs that the next merge combines begin:
 * each older run joins while it is at most twice the size of the newer ones
 * it would join, so the index keeps a number of runs that grows with the
 * logarithm of its size. It returns false when no merge is due.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_INDEX_NAME(to_merge) (const CHUNKS_INDEX_INDEX * self, size_t *result)
{
	size_t		from;
	uint64		newer;

	if (!std_checked_sub_usize(self->runs.len, 1, &from))
		return false;
	std_index_check(from, self->runs.len, RUST_FILE, 1077, 34);
	newer = CHUNKS_INDEX_RUN_NAME(len) (std_arc_Deref_deref(STD_VEC_AT(&self->runs, StdArcInner *, from),
															_Alignof(CHUNKS_INDEX_RUN)));
	for (;;)
	{
		size_t		older;
		uint64		older_len;

		if (!(from > 0))
			break;
		older = STD_SUB(usize, from, 1, 1078, 37);
		std_index_check(older, self->runs.len, RUST_FILE, 1078, 36);
		older_len = CHUNKS_INDEX_RUN_NAME(len) (std_arc_Deref_deref(STD_VEC_AT(&self->runs, StdArcInner *, older),
																	_Alignof(CHUNKS_INDEX_RUN)));
		if (!(older_len <= STD_MUL(u64, 2, newer, 1078, 56)))
			break;
		older = STD_SUB(usize, from, 1, 1079, 32);
		std_index_check(older, self->runs.len, RUST_FILE, 1079, 31);
		newer = STD_ADD(u64, newer,
						CHUNKS_INDEX_RUN_NAME(len) (std_arc_Deref_deref(STD_VEC_AT(&self->runs, StdArcInner *, older),
																		_Alignof(CHUNKS_INDEX_RUN))),
						1079, 13);
		from = STD_SUB(usize, from, 1, 1080, 13);
	}
	if (STD_ADD(usize, from, 1, 1082, 10) < self->runs.len)
	{
		*result = from;
		return true;
	}
	return false;
}

/* merged's closure_0: an entry's address, the key its sort orders by. */
static ChunksAddressAddress
CHUNKS_INDEX_INDEX_NAME(merged_closure_0) (void *captures, const void *a)
{
	const		CHUNKS_INDEX_ENTRY *entry = a;

	return entry->f0;
}

/* The state of merged across its pad: its arguments, its result and error, the entries it sorts, and a run's. */
typedef struct CHUNKS_INDEX_MERGED_FRAME
{
	const		CHUNKS_INDEX_INDEX *self;
	HostIoReadDoors io;
	size_t		from;
	size_t		to;
	StdVec	   *result;
	ChunksErrorError *error;
	StdVec		entries;
	bool		entries_live;
	StdVec		run_entries;
	bool		run_entries_live;
	bool		merged_live;
	bool		ok;
}			CHUNKS_INDEX_MERGED_FRAME;

/* merged's body. */
static void
CHUNKS_INDEX_INDEX_NAME(merged_body_0) (void *frame_)
{
	CHUNKS_INDEX_MERGED_FRAME *frame = frame_;
	StdArcInner *const *runs;

	frame->entries = (StdVec) STD_VEC_NEW;
	frame->entries_live = true;
	std_range_check(frame->from, frame->to, frame->self->runs.len, RUST_FILE, 1089, 29);
	runs = STD_PTR_AT(frame->self->runs.ptr, frame->from, sizeof(StdArcInner *));
	for (size_t i_end = 0, i = std_wrapping_sub_usize(frame->to, frame->from); i > i_end;)
	{
		i--;
		if (!CHUNKS_INDEX_RUN_NAME(entries) (std_arc_Deref_deref(runs[i], _Alignof(CHUNKS_INDEX_RUN)), frame->io,
											 &frame->run_entries, frame->error))
		{
			frame->entries_live = false;
			std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_ENTRIES);
			return;
		}
		frame->run_entries_live = true;
		std_vec_extend_from_slice_Checked(&frame->entries, frame->run_entries.ptr, frame->run_entries.len,
										  CHUNKS_INDEX_ENTRIES);
		frame->run_entries_live = false;
		std_vec_free_Checked(&frame->run_entries, CHUNKS_INDEX_ENTRIES);
	}
	CHUNKS_INDEX_SORT_BY_KEY(frame->entries.ptr, frame->entries.len, CHUNKS_INDEX_INDEX_NAME(merged_closure_0), NULL,
							 NULL);
	std_vec_with_capacity_Checked(frame->result, frame->entries.len, CHUNKS_INDEX_ENTRIES);
	frame->merged_live = true;
	for (size_t i = 0, i_len = frame->entries.len; i < i_len; i++)
	{
		CHUNKS_INDEX_ENTRY entry = STD_VEC_AT(&frame->entries, CHUNKS_INDEX_ENTRY, i);
		bool		kept = false;

		for (size_t j_end = 0, j = frame->result->len; j > j_end;)
		{
			const		CHUNKS_INDEX_ENTRY *other;

			j--;
			other = STD_PTR_AT(frame->result->ptr, j, sizeof(CHUNKS_INDEX_ENTRY));
			if (!chunks_address_Address_PartialEq_eq(&other->f0, &entry.f0))
				break;
			if (chunks_address_Address_PartialEq_eq(&other->f0, &entry.f0) && CHUNKS_INDEX_V_EQ(&other->f1, &entry.f1))
			{
				kept = true;
				break;
			}
		}
		if (!kept)
			std_vec_push_Checked(frame->result, &entry, CHUNKS_INDEX_ENTRIES);
	}
	frame->entries_live = false;
	std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_ENTRIES);
	frame->merged_live = false;
	frame->ok = true;
}

/* merged's pad: a run's entries, the entries it sorts, and those it keeps. */
static void
CHUNKS_INDEX_INDEX_NAME(merged_pad_0) (void *frame_)
{
	CHUNKS_INDEX_MERGED_FRAME *frame = frame_;

	if (frame->run_entries_live)
		std_vec_free_Checked(&frame->run_entries, CHUNKS_INDEX_ENTRIES);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_ENTRIES);
	if (frame->merged_live)
		std_vec_free_Checked(frame->result, CHUNKS_INDEX_ENTRIES);
}

/*
 * merged returns the entries of the runs at the positions from..to, each
 * once, in address order with a newer run's entries for an address first.
 */
CHUNKS_INDEX_DEF bool
CHUNKS_INDEX_INDEX_NAME(merged) (const CHUNKS_INDEX_INDEX * self, HostIoReadDoors io, size_t from, size_t to,
								 StdVec *result, ChunksErrorError *error)
{
	CHUNKS_INDEX_MERGED_FRAME frame = {.self = self, .io = io, .from = from, .to = to, .result = result,
	.error = error};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(merged_body_0), CHUNKS_INDEX_INDEX_NAME(merged_pad_0));
	return frame.ok;
}

/*
 * The state of remove across its pad: its arguments, whether it still owns
 * pick, the runs it takes out of the index as an IntoIter, the run it
 * places, and the two lists it places them in.
 */
typedef struct CHUNKS_INDEX_REMOVE_FRAME
{
	CHUNKS_INDEX_INDEX *self;
	bool		(*pick) (void *captures, const CHUNKS_INDEX_RUN * run);
	void	   *pick_captures;
	void		(*pick_drop) (void *);
	bool		pick_live;
	StdVecIntoIter iter;
	bool		iter_live;
	StdArcInner *run;
	bool		run_live;
	StdVec		removed;
	StdVec		kept;
	bool		lists_live;
}			CHUNKS_INDEX_REMOVE_FRAME;

/* remove's body. */
static void
CHUNKS_INDEX_INDEX_NAME(remove_body_0) (void *frame_)
{
	CHUNKS_INDEX_REMOVE_FRAME *frame = frame_;
	StdVec		taken = frame->self->runs;

	frame->self->runs = (StdVec) STD_VEC_NEW;
	frame->iter = std_Vec_IntoIterator_into_iter(&taken, CHUNKS_INDEX_ARCS, CHUNKS_INDEX_ARC_RUN(drop));
	frame->iter_live = true;
	frame->removed = (StdVec) STD_VEC_NEW;
	frame->kept = (StdVec) STD_VEC_NEW;
	frame->lists_live = true;
	while (std_VecIntoIter_Iterator_next(&frame->iter, &frame->run))
	{
		frame->run_live = true;
		if (frame->pick(frame->pick_captures, std_arc_Deref_deref(frame->run, _Alignof(CHUNKS_INDEX_RUN))))
			std_vec_push_Checked(&frame->removed, &frame->run, CHUNKS_INDEX_ARCS);
		else
			std_vec_push_Checked(&frame->kept, &frame->run, CHUNKS_INDEX_ARCS);
		frame->run_live = false;
	}
	frame->iter_live = false;
	std_VecIntoIter_drop(&frame->iter);
	frame->lists_live = false;
	frame->self->runs = frame->kept;
	frame->pick_live = false;
	if (frame->pick_drop != NULL)
		frame->pick_drop(frame->pick_captures);
}

/* remove's pad: the run it places, the IntoIter, the two lists, and pick while it owns it. */
static void
CHUNKS_INDEX_INDEX_NAME(remove_pad_0) (void *frame_)
{
	CHUNKS_INDEX_REMOVE_FRAME *frame = frame_;

	if (frame->run_live)
		CHUNKS_INDEX_ARC_RUN(drop) (&frame->run);
	if (frame->iter_live)
		std_VecIntoIter_drop(&frame->iter);
	if (frame->lists_live)
	{
		std_vec_drop_Checked(&frame->kept, CHUNKS_INDEX_ARCS, CHUNKS_INDEX_ARC_RUN(drop));
		std_vec_drop_Checked(&frame->removed, CHUNKS_INDEX_ARCS, CHUNKS_INDEX_ARC_RUN(drop));
	}
	if (frame->pick_live && frame->pick_drop != NULL)
		frame->pick_drop(frame->pick_captures);
}

/*
 * remove takes the runs out of the index that pick picks and returns them,
 * whose files can be removed once no reader uses them, taking pick.
 */
CHUNKS_INDEX_DEF StdVec
CHUNKS_INDEX_INDEX_NAME(remove) (CHUNKS_INDEX_INDEX * self, bool (*pick) (void *captures, const CHUNKS_INDEX_RUN * run),
								 void *pick_captures, void (*pick_drop) (void *))
{
	CHUNKS_INDEX_REMOVE_FRAME frame = {.self = self, .pick = pick, .pick_captures = pick_captures,
	.pick_drop = pick_drop, .pick_live = true};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(remove_body_0), CHUNKS_INDEX_INDEX_NAME(remove_pad_0));
	return frame.removed;
}

/*
 * The state of replace across its pad: its arguments, the run's new
 * reference, the runs it replaces, and the drain that takes them out.
 */
typedef struct CHUNKS_INDEX_REPLACE_FRAME
{
	CHUNKS_INDEX_INDEX *self;
	size_t		from;
	size_t		to;
	CHUNKS_INDEX_RUN *run;
	StdArcInner *arc;
	bool		arc_live;
	StdVec		replaced;
	bool		replaced_live;
	StdVecDrain drain;
	bool		drain_live;
}			CHUNKS_INDEX_REPLACE_FRAME;

/* replace's body. */
static void
CHUNKS_INDEX_INDEX_NAME(replace_body_0) (void *frame_)
{
	CHUNKS_INDEX_REPLACE_FRAME *frame = frame_;
	StdBound	start = {STD_BOUND_INCLUDED, frame->from};
	StdBound	end = {STD_BOUND_EXCLUDED, frame->to};
	StdSizeHint hint;
	StdArcInner *taken;

	frame->arc = std_arc_new_dropping(frame->run, sizeof(CHUNKS_INDEX_RUN), _Alignof(CHUNKS_INDEX_RUN),
									  CHUNKS_INDEX_RUN_NAME(drop));
	frame->arc_live = true;
	std_vec_with_capacity_Checked(&frame->replaced, STD_SUB(usize, frame->to, frame->from, 1114, 47),
								  CHUNKS_INDEX_ARCS);
	frame->replaced_live = true;
	frame->drain = std_vec_drain(&frame->self->runs, start, end, CHUNKS_INDEX_ARCS, CHUNKS_INDEX_ARC_RUN(drop));
	frame->drain_live = true;
	hint.lower = std_wrapping_sub_usize(frame->drain.end, frame->drain.next);
	hint.has_upper = true;
	hint.upper = hint.lower;
	std_vec_extend_trusted_reserve(&frame->replaced, hint, CHUNKS_INDEX_ARCS);
	while (std_VecDrain_Iterator_next(&frame->drain, &taken))
	{
		STD_VEC_AT(&frame->replaced, StdArcInner *, frame->replaced.len) = taken;
		frame->replaced.len = std_wrapping_add_usize(frame->replaced.len, 1);
	}
	frame->drain_live = false;
	std_VecDrain_drop(&frame->drain);
	std_vec_insert(&frame->self->runs, frame->from, &frame->arc, CHUNKS_INDEX_ARCS, RUST_FILE, 1116, 19);
	frame->arc_live = false;
	frame->replaced_live = false;
}

/* replace's pad: the drain, the runs it replaces, and the run's new reference. */
static void
CHUNKS_INDEX_INDEX_NAME(replace_pad_0) (void *frame_)
{
	CHUNKS_INDEX_REPLACE_FRAME *frame = frame_;

	if (frame->drain_live)
		std_VecDrain_drop(&frame->drain);
	if (frame->replaced_live)
		std_vec_drop_Checked(&frame->replaced, CHUNKS_INDEX_ARCS, CHUNKS_INDEX_ARC_RUN(drop));
	if (frame->arc_live)
		CHUNKS_INDEX_ARC_RUN(drop) (&frame->arc);
}

/*
 * replace puts one run in place of the runs at the positions from..to and
 * returns the runs it replaced, whose files can be removed once no reader
 * uses them, taking the run.
 */
CHUNKS_INDEX_DEF StdVec
CHUNKS_INDEX_INDEX_NAME(replace) (CHUNKS_INDEX_INDEX * self, size_t from, size_t to, CHUNKS_INDEX_RUN * run)
{
	CHUNKS_INDEX_REPLACE_FRAME frame = {.self = self, .from = from, .to = to, .run = run};

	std_landing(&frame, CHUNKS_INDEX_INDEX_NAME(replace_body_0), CHUNKS_INDEX_INDEX_NAME(replace_pad_0));
	return frame.replaced;
}

/* AddressIndex<V>'s drop glue: its runs' references. */
CHUNKS_INDEX_DEF void
CHUNKS_INDEX_INDEX_NAME(drop) (void *self_)
{
	CHUNKS_INDEX_INDEX *self = self_;

	std_vec_drop_Checked(&self->runs, CHUNKS_INDEX_ARCS, CHUNKS_INDEX_ARC_RUN(drop));
}

#undef CHUNKS_INDEX_ENTRIES
#undef CHUNKS_INDEX_ARCS
#undef CHUNKS_INDEX_FIRST_IN_INDEX
#undef CHUNKS_INDEX_WRITE_FRAME
#undef CHUNKS_INDEX_ENCODE_FRAME
#undef CHUNKS_INDEX_OPEN_FRAME
#undef CHUNKS_INDEX_READ_FRAME
#undef CHUNKS_INDEX_BLOCK_FRAME
#undef CHUNKS_INDEX_DAMAGED_FRAME
#undef CHUNKS_INDEX_GET_FRAME
#undef CHUNKS_INDEX_RANGE_FRAME
#undef CHUNKS_INDEX_RUN_ENTRIES_FRAME
#undef CHUNKS_INDEX_ADD_FRAME
#undef CHUNKS_INDEX_FENCES_FRAME
#undef CHUNKS_INDEX_INDEX_GET_FRAME
#undef CHUNKS_INDEX_ALL_FRAME
#undef CHUNKS_INDEX_WITH_PREFIX_FRAME
#undef CHUNKS_INDEX_MERGED_FRAME
#undef CHUNKS_INDEX_REMOVE_FRAME
#undef CHUNKS_INDEX_REPLACE_FRAME
#endif							/* CHUNKS_INDEX_DEFINE */

#undef CHUNKS_INDEX_FN
#undef CHUNKS_INDEX_FIXED
#undef CHUNKS_INDEX_ARC_RUN
#undef CHUNKS_INDEX_RUN
#undef CHUNKS_INDEX_RUN_NAME
#undef CHUNKS_INDEX_INDEX
#undef CHUNKS_INDEX_INDEX_NAME
#endif							/* CHUNKS_INDEX_VS */

#ifdef CHUNKS_INDEX_ES
#undef CHUNKS_INDEX_UNREAD
#endif

#undef CHUNKS_INDEX_CAT_
#undef CHUNKS_INDEX_CAT
#undef CHUNKS_INDEX_SCOPE
#undef CHUNKS_INDEX_DECLARE
#undef CHUNKS_INDEX_DEFINE
#undef CHUNKS_INDEX_DEF_static
#undef CHUNKS_INDEX_DEF_extern
#undef CHUNKS_INDEX_DEF
#undef CHUNKS_INDEX_ES
#undef CHUNKS_INDEX_E
#undef CHUNKS_INDEX_VS
#undef CHUNKS_INDEX_V
#undef CHUNKS_INDEX_ENTRY
#undef CHUNKS_INDEX_V_LEN
#undef CHUNKS_INDEX_V_EQ
#undef CHUNKS_INDEX_RUNS_OPS
#undef CHUNKS_INDEX_SORT_BY_KEY
