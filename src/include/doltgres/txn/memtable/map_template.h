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
 * The template of map.rs's functions generic over a value they hold by value
 * (txn's memtable/map.rs): prepare, one instance per its closure's result R,
 * as pin<R> is, and finish and took, one instance per the value T a take's
 * step keeps, its Step<T> and Taken<T> each a variant with T beside it.
 * Before including it define, for prepare:
 *
 * TXN_MEMTABLE_MAP_RS: R's spelling in the instance's names.
 * TXN_MEMTABLE_MAP_RETURNS: R's C return type, bool for a Result.
 * TXN_MEMTABLE_MAP_RESULT_PARAMS: R's out-parameters, each with its leading
 * comma (for a Result, section 5's result pointers and its error).
 *
 * and for finish and took:
 *
 * TXN_MEMTABLE_MAP_TS: T's spelling in the instance's names.
 * TXN_MEMTABLE_MAP_KEPT_OUT: T's out-parameter with its leading comma, none
 * for ().
 * TXN_MEMTABLE_MAP_KEPT_IN: T's borrowed parameter with its leading comma,
 * none for ().
 *
 * TXN_MEMTABLE_MAP_DECLARE and TXN_MEMTABLE_MAP_DEFINE: whether to declare
 * and to define them; the definitions run table.c's readied and map.c's
 * took_core, and take, for prepare, TXN_MEMTABLE_MAP_RESULT_FIELDS,
 * TXN_MEMTABLE_MAP_RESULT_INIT, TXN_MEMTABLE_MAP_RESULT_ARGS(c), and
 * TXN_MEMTABLE_MAP_RESULT_ERROR, as table_template.h's prepare does, and,
 * for finish and took:
 * TXN_MEMTABLE_MAP_KEPT_ARG: the kept parameter's name, kept, or NULL for ().
 * TXN_MEMTABLE_MAP_KEPT_CALL(k): the kept value k with its leading comma, as
 * the closures take it, or none for ().
 * TXN_MEMTABLE_MAP_KEPT_DROP(k): T's drop of the kept value k, which a take
 * that fails after its step answered a put or a removal runs, or nothing for
 * a T whose drop does nothing.
 *
 * Every instance's own Result is section 5's bool with an error, and its
 * closures are (fn, captures, drop), taken by value. It undefines every name
 * it takes at its end.
 */
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"

/* TXN_MEMTABLE_MAP_CAT pastes two tokens after expanding them. */
#define TXN_MEMTABLE_MAP_CAT_(a, b) a##b
#define TXN_MEMTABLE_MAP_CAT(a, b) TXN_MEMTABLE_MAP_CAT_(a, b)

#ifdef TXN_MEMTABLE_MAP_DECLARE
#ifdef TXN_MEMTABLE_MAP_RS
extern bool TXN_MEMTABLE_MAP_CAT(txn_memtable_map_prepare_, TXN_MEMTABLE_MAP_RS) (HostIoIo *io,
																				  const TxnMemtableMapTake *takes,
																				  size_t takes_len,
																				  TXN_MEMTABLE_MAP_RETURNS(*inside) (void *captures,
																													 HostIoIo *io,
																													 TxnMemtableTablePreparedState *prepared
																													 TXN_MEMTABLE_MAP_RESULT_PARAMS),
																				  void *inside_captures,
																				  void (*inside_drop) (void *captures)
																				  TXN_MEMTABLE_MAP_RESULT_PARAMS);
#endif
#ifdef TXN_MEMTABLE_MAP_TS
extern bool TXN_MEMTABLE_MAP_CAT(txn_memtable_map_finish_, TXN_MEMTABLE_MAP_TS) (HostIoTakesShort io,
																				 TxnMemtableTablePreparedState *prepared,
																				 size_t take,
																				 TxnMemtableVersionWriter writer,
																				 bool has_roomed,
																				 TxnStatusRoomed roomed,
																				 bool has_lsn, uint64 lsn,
																				 bool has_base, const uint8 *base,
																				 size_t base_len,
																				 const TxnMemtableMapRead *read,
																				 TxnMemtableMapStep (*step) (void *captures,
																											 bool has_current,
																											 const uint8 *current,
																											 size_t current_len,
																											 StdVec *out
																											 TXN_MEMTABLE_MAP_KEPT_OUT),
																				 void *step_captures,
																				 void (*step_drop) (void *captures),
																				 void (*then) (void *captures
																							   TXN_MEMTABLE_MAP_KEPT_IN),
																				 void *then_captures,
																				 void (*then_drop) (void *captures),
																				 TxnMemtableMapTaken *result
																				 TXN_MEMTABLE_MAP_KEPT_OUT,
																				 HostError *error);
extern bool TXN_MEMTABLE_MAP_CAT(txn_memtable_map_took_, TXN_MEMTABLE_MAP_TS) (HostIoTakesShort io,
																			   TxnMemtableTablePreparedState *prepared,
																			   size_t take,
																			   TxnMemtableVersionWriter writer,
																			   bool has_roomed, TxnStatusRoomed roomed,
																			   bool has_lsn, uint64 lsn, bool has_claim,
																			   HostRow claim_tid, uint64 claim_bytes,
																			   bool has_base, const uint8 *base,
																			   size_t base_len,
																			   const TxnMemtableMapRead *read,
																			   TxnMemtableMapStep (*step) (void *captures,
																										   bool has_current,
																										   const uint8 *current,
																										   size_t current_len,
																										   StdVec *out
																										   TXN_MEMTABLE_MAP_KEPT_OUT),
																			   void *step_captures,
																			   void (*step_drop) (void *captures),
																			   HostRow (*tuple) (void *captures
																								 TXN_MEMTABLE_MAP_KEPT_IN),
																			   void *tuple_captures,
																			   void (*tuple_drop) (void *captures),
																			   void (*then) (void *captures
																							 TXN_MEMTABLE_MAP_KEPT_IN),
																			   void *then_captures,
																			   void (*then_drop) (void *captures),
																			   TxnMemtableMapTaken *result
																			   TXN_MEMTABLE_MAP_KEPT_OUT,
																			   HostError *error);
#endif
#endif

#ifdef TXN_MEMTABLE_MAP_DEFINE
#ifdef TXN_MEMTABLE_MAP_RS
/* TXN_MEMTABLE_MAP_RNAME names prepare's instance's own names after R. */
#define TXN_MEMTABLE_MAP_RNAME(name) TXN_MEMTABLE_MAP_CAT(name##_, TXN_MEMTABLE_MAP_RS)

/* prepare's instance's captures for readied: the caller's closure and the result pointers. */
typedef struct TXN_MEMTABLE_MAP_RNAME (txn_memtable_map_inside_t)
{
	TXN_MEMTABLE_MAP_RETURNS(*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared
									   TXN_MEMTABLE_MAP_RESULT_PARAMS);
	void	   *inside_captures;
	void		(*inside_drop) (void *captures);

	TXN_MEMTABLE_MAP_RESULT_FIELDS
} TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_inside_t);

/* prepare's instance's inside for readied: the caller's closure. */
static bool
TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
												 HostError *error)
{
	TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_inside_t) * c = captures;

	return c->inside(c->inside_captures, io, prepared TXN_MEMTABLE_MAP_RESULT_ARGS(c), error);
}

/* prepare's instance's drop of its captures: the caller's closure's, when readied never runs it. */
static void
TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_dropped) (void *captures)
{
	TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_inside_t) * c = captures;

	if (c->inside_drop != NULL)
		c->inside_drop(c->inside_captures);
}

/* The instance of prepare: map takes readied, each in the storage its take names, the window's work run with them. */
bool
TXN_MEMTABLE_MAP_CAT(txn_memtable_map_prepare_, TXN_MEMTABLE_MAP_RS) (HostIoIo *io, const TxnMemtableMapTake *takes,
																	  size_t takes_len,
																	  TXN_MEMTABLE_MAP_RETURNS(*inside) (void *captures,
																										 HostIoIo *io,
																										 TxnMemtableTablePreparedState *prepared
																										 TXN_MEMTABLE_MAP_RESULT_PARAMS),
																	  void *inside_captures,
																	  void (*inside_drop) (void *captures)
																	  TXN_MEMTABLE_MAP_RESULT_PARAMS)
{
	TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_inside_t) captures =
	{
		inside, inside_captures, inside_drop
			TXN_MEMTABLE_MAP_RESULT_INIT
	};
	HostSharedRecord none = {0};

	return txn_memtable_table_readied(io, false, none, NULL, 0, takes, takes_len, false,
									  TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_inside), &captures,
									  TXN_MEMTABLE_MAP_RNAME(txn_memtable_map_dropped), NULL,
									  TXN_MEMTABLE_MAP_RESULT_ERROR);
}

#undef TXN_MEMTABLE_MAP_RNAME
#endif
#ifdef TXN_MEMTABLE_MAP_TS
/* TXN_MEMTABLE_MAP_TNAME names finish's and took's instances' own names after T. */
#define TXN_MEMTABLE_MAP_TNAME(name) TXN_MEMTABLE_MAP_CAT(name##_, TXN_MEMTABLE_MAP_TS)

/*
 * finish's and took's instances' captures for took_core: the caller's step,
 * tuple, and then, each with whether took_core ran it, and the kept value,
 * the caller's out-parameter, through which they share T.
 */
typedef struct TXN_MEMTABLE_MAP_TNAME (txn_memtable_map_took_t)
{
	TxnMemtableMapStep (*step) (void *captures, bool has_current, const uint8 *current, size_t current_len,
								StdVec *out TXN_MEMTABLE_MAP_KEPT_OUT);
	void	   *step_captures;
	void		(*step_drop) (void *captures);
	bool		step_ran;
	HostRow		(*tuple) (void *captures TXN_MEMTABLE_MAP_KEPT_IN);
	void	   *tuple_captures;
	void		(*tuple_drop) (void *captures);
	bool		tuple_ran;
	void		(*then) (void *captures TXN_MEMTABLE_MAP_KEPT_IN);
	void	   *then_captures;
	void		(*then_drop) (void *captures);
	bool		then_ran;
	TxnMemtableMapStep answer;
	void	   *kept;
} TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_t);

/* The instance's step for took_core: the caller's step, at the kept value. */
static TxnMemtableMapStep
TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_step) (void *captures, bool has_current, const uint8 *current,
													size_t current_len, StdVec *out)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_t) * c = captures;

	c->step_ran = true;
	c->answer = c->step(c->step_captures, has_current, current, current_len, out TXN_MEMTABLE_MAP_KEPT_CALL(c->kept));
	return c->answer;
}

/* The instance's tuple for took_core: the caller's tuple at the kept value, or no tuple id for finish, which has none. */
static HostRow
TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_tuple) (void *captures)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_t) * c = captures;
	HostRow		none = {0};

	if (c->tuple == NULL)
		return none;
	c->tuple_ran = true;
	return c->tuple(c->tuple_captures TXN_MEMTABLE_MAP_KEPT_CALL(c->kept));
}

/* The instance's then for took_core: the caller's then at the kept value. */
static void
TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_then) (void *captures)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_t) * c = captures;

	c->then_ran = true;
	c->then(c->then_captures TXN_MEMTABLE_MAP_KEPT_CALL(c->kept));
}

/*
 * The instance's end of took_core: the drops of the closures it never ran,
 * as their values drop unused, and, for a take that failed after its step
 * answered a put or a removal, the kept value's.
 */
static void
TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_unran) (TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_t) * c, bool ok)
{
	if (!ok && c->step_ran &&
		(c->answer == TXN_MEMTABLE_MAP_STEP_PUT || c->answer == TXN_MEMTABLE_MAP_STEP_REMOVE))
		TXN_MEMTABLE_MAP_KEPT_DROP(c->kept);
	if (!c->step_ran && c->step_drop != NULL)
		c->step_drop(c->step_captures);
	if (!c->tuple_ran && c->tuple_drop != NULL)
		c->tuple_drop(c->tuple_captures);
	if (!c->then_ran && c->then_drop != NULL)
		c->then_drop(c->then_captures);
}

/*
 * finish's and took's state across their pad: took_core's arguments, its
 * captures, and whether took_core returned, so that an unwind out of it
 * drops the closures it never ran and the kept value as a failed take does.
 */
typedef struct TXN_MEMTABLE_MAP_TNAME (txn_memtable_map_took_frame_t)
{
	HostIoTakesShort io;
	TxnMemtableTablePreparedState *prepared;
	size_t		take;
	TxnMemtableVersionWriter writer;
	bool		has_roomed;
	TxnStatusRoomed roomed;
	bool		has_lsn;
	uint64		lsn;
	bool		has_claim;
	HostRow		claim_tid;
	uint64		claim_bytes;
	bool		has_base;
	const uint8 *base;
	size_t		base_len;
	const TxnMemtableMapRead *read;

	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_t) captures;
	TxnMemtableMapTaken *result;
	HostError  *error;
	bool		ok;
	bool		returned;
} TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_frame_t);

/* The instance's pad of finish and took: the end of a take that took_core never returned from, a failed one. */
static void
TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_pad) (void *frame_)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_frame_t) * frame = frame_;

	if (!frame->returned)
	{
		frame->returned = true;
		TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_unran) (&frame->captures, false);
	}
}

/* The instance's body of finish and took: took_core, then the end of what it left. */
static void
TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_body) (void *frame_)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_frame_t) * frame = frame_;

	frame->ok = txn_memtable_map_took_core(frame->io, frame->prepared, frame->take, frame->writer, frame->has_roomed,
										   frame->roomed, frame->has_lsn, frame->lsn, frame->has_claim,
										   frame->claim_tid, frame->claim_bytes,
										   frame->has_base, frame->base, frame->base_len, frame->read,
										   TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_step),
										   TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_tuple),
										   TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_then), &frame->captures,
										   frame->result, frame->error);
	frame->returned = true;
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_unran) (&frame->captures, frame->ok);
}

/* The instance of finish: took with no claim and no tuple id, its step's put or removal under the take's key. */
bool
TXN_MEMTABLE_MAP_CAT(txn_memtable_map_finish_, TXN_MEMTABLE_MAP_TS) (HostIoTakesShort io,
																	 TxnMemtableTablePreparedState *prepared,
																	 size_t take, TxnMemtableVersionWriter writer,
																	 bool has_roomed, TxnStatusRoomed roomed,
																	 bool has_lsn, uint64 lsn, bool has_base,
																	 const uint8 *base, size_t base_len,
																	 const TxnMemtableMapRead *read,
																	 TxnMemtableMapStep (*step) (void *captures,
																								 bool has_current,
																								 const uint8 *current,
																								 size_t current_len,
																								 StdVec *out
																								 TXN_MEMTABLE_MAP_KEPT_OUT),
																	 void *step_captures,
																	 void (*step_drop) (void *captures),
																	 void (*then) (void *captures
																				   TXN_MEMTABLE_MAP_KEPT_IN),
																	 void *then_captures,
																	 void (*then_drop) (void *captures),
																	 TxnMemtableMapTaken *result
																	 TXN_MEMTABLE_MAP_KEPT_OUT, HostError *error)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_frame_t) frame =
	{
		.io = io, .prepared = prepared, .take = take, .writer = writer, .has_roomed = has_roomed, .roomed = roomed,
			.has_lsn = has_lsn, .lsn = lsn,
			.has_base = has_base, .base = base, .base_len = base_len, .read = read,
			.captures =
		{
			.step = step, .step_captures = step_captures, .step_drop = step_drop, .then = then,
				.then_captures = then_captures, .then_drop = then_drop, .answer = TXN_MEMTABLE_MAP_STEP_RETRY,
				.kept = TXN_MEMTABLE_MAP_KEPT_ARG
		},
			.result = result, .error = error
	};

	std_landing(&frame, TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_body),
				TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_pad));
	return frame.ok;
}

/* The instance of took: a take's step on its key's current value inside the window, refused when its claim was overtaken. */
bool
TXN_MEMTABLE_MAP_CAT(txn_memtable_map_took_, TXN_MEMTABLE_MAP_TS) (HostIoTakesShort io,
																   TxnMemtableTablePreparedState *prepared, size_t take,
																   TxnMemtableVersionWriter writer, bool has_roomed,
																   TxnStatusRoomed roomed, bool has_lsn,
																   uint64 lsn, bool has_claim, HostRow claim_tid,
																   uint64 claim_bytes, bool has_base,
																   const uint8 *base, size_t base_len,
																   const TxnMemtableMapRead *read,
																   TxnMemtableMapStep (*step) (void *captures,
																							   bool has_current,
																							   const uint8 *current,
																							   size_t current_len,
																							   StdVec *out
																							   TXN_MEMTABLE_MAP_KEPT_OUT),
																   void *step_captures,
																   void (*step_drop) (void *captures),
																   HostRow (*tuple) (void *captures
																					 TXN_MEMTABLE_MAP_KEPT_IN),
																   void *tuple_captures,
																   void (*tuple_drop) (void *captures),
																   void (*then) (void *captures
																				 TXN_MEMTABLE_MAP_KEPT_IN),
																   void *then_captures,
																   void (*then_drop) (void *captures),
																   TxnMemtableMapTaken *result
																   TXN_MEMTABLE_MAP_KEPT_OUT, HostError *error)
{
	TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_frame_t) frame =
	{
		.io = io, .prepared = prepared, .take = take, .writer = writer, .has_roomed = has_roomed, .roomed = roomed,
			.has_lsn = has_lsn, .lsn = lsn,
			.has_claim = has_claim, .claim_tid = claim_tid, .claim_bytes = claim_bytes, .has_base = has_base,
			.base = base, .base_len = base_len, .read = read,
			.captures =
		{
			.step = step, .step_captures = step_captures, .step_drop = step_drop, .tuple = tuple,
				.tuple_captures = tuple_captures, .tuple_drop = tuple_drop, .then = then,
				.then_captures = then_captures, .then_drop = then_drop, .answer = TXN_MEMTABLE_MAP_STEP_RETRY,
				.kept = TXN_MEMTABLE_MAP_KEPT_ARG
		},
			.result = result, .error = error
	};

	std_landing(&frame, TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_body),
				TXN_MEMTABLE_MAP_TNAME(txn_memtable_map_took_pad));
	return frame.ok;
}

#undef TXN_MEMTABLE_MAP_TNAME
#endif
#endif

#undef TXN_MEMTABLE_MAP_CAT_
#undef TXN_MEMTABLE_MAP_CAT
#undef TXN_MEMTABLE_MAP_RS
#undef TXN_MEMTABLE_MAP_RETURNS
#undef TXN_MEMTABLE_MAP_RESULT_PARAMS
#undef TXN_MEMTABLE_MAP_TS
#undef TXN_MEMTABLE_MAP_KEPT_OUT
#undef TXN_MEMTABLE_MAP_KEPT_IN
#undef TXN_MEMTABLE_MAP_DECLARE
#undef TXN_MEMTABLE_MAP_DEFINE
#undef TXN_MEMTABLE_MAP_RESULT_FIELDS
#undef TXN_MEMTABLE_MAP_RESULT_INIT
#undef TXN_MEMTABLE_MAP_RESULT_ARGS
#undef TXN_MEMTABLE_MAP_RESULT_ERROR
#undef TXN_MEMTABLE_MAP_KEPT_ARG
#undef TXN_MEMTABLE_MAP_KEPT_CALL
#undef TXN_MEMTABLE_MAP_KEPT_DROP
