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
 * The template of branch.rs's identity_prepare, which holds its closure's
 * result R and the value W it hands its closure by value (txn's
 * memtable/branch.rs), one instance per (R, W), as pin<R> is. Before
 * including it define:
 *
 * TXN_MEMTABLE_BRANCH_RS: R's spelling in the instance's names.
 * TXN_MEMTABLE_BRANCH_WS: W's spelling in the instance's names.
 * TXN_MEMTABLE_BRANCH_RETURNS: R's C return type, bool for a Result.
 * TXN_MEMTABLE_BRANCH_RESULT_PARAMS: R's out-parameters, each with its
 * leading comma (for a Result, section 5's result pointers and its error).
 * TXN_MEMTABLE_BRANCH_WITH_PARAMS: W's parameters, each with its leading
 * comma, none for ().
 * TXN_MEMTABLE_BRANCH_DECLARE and TXN_MEMTABLE_BRANCH_DEFINE: whether to
 * declare and to define it; the definition runs table.c's prepared over the
 * keys' aims, carrying W and the closure's result pointers in its captures,
 * and takes:
 * TXN_MEMTABLE_BRANCH_WITH_FIELDS: W's fields, each with its semicolon, none
 * for ().
 * TXN_MEMTABLE_BRANCH_WITH_INIT: their initializers from the parameters,
 * each with its leading comma.
 * TXN_MEMTABLE_BRANCH_WITH_ARGS(c): W's values from captures c, each with its
 * leading comma.
 * TXN_MEMTABLE_BRANCH_RESULT_FIELDS: the result pointers' fields, each with
 * its semicolon, none for a closure whose only out-parameter is its error.
 * TXN_MEMTABLE_BRANCH_RESULT_INIT: their initializers, each with its leading
 * comma.
 * TXN_MEMTABLE_BRANCH_RESULT_ARGS(c): the pointers from captures c, each with
 * its leading comma, which the closure takes before its error.
 * TXN_MEMTABLE_BRANCH_RESULT_ERROR: the instance's error parameter's name.
 *
 * Every instance's own Result is section 5's bool with an error, and its
 * closure is (fn, captures, drop), taken by value. It undefines every name it
 * takes at its end.
 */
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/table.h"

/* TXN_MEMTABLE_BRANCH_CAT pastes three tokens after expanding them. */
#define TXN_MEMTABLE_BRANCH_CAT_(a, b, c) a##b##_##c
#define TXN_MEMTABLE_BRANCH_CAT(a, b, c) TXN_MEMTABLE_BRANCH_CAT_(a, b, c)

/* TXN_MEMTABLE_BRANCH_NAME names the instance's function after R and W. */
#define TXN_MEMTABLE_BRANCH_NAME(name) TXN_MEMTABLE_BRANCH_CAT(name##_, TXN_MEMTABLE_BRANCH_RS, TXN_MEMTABLE_BRANCH_WS)

#ifdef TXN_MEMTABLE_BRANCH_DECLARE
extern bool TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare) (HostIoIo *io, HostSharedRecord deltas,
																			const txn_TupleRefSliceU8Usize_t *keys,
																			size_t keys_len
																			TXN_MEMTABLE_BRANCH_WITH_PARAMS,
																			TXN_MEMTABLE_BRANCH_RETURNS(*inside) (void *captures,
																												  HostIoIo *io,
																												  TxnMemtableTablePreparedBranchDeltas *prepared
																												  TXN_MEMTABLE_BRANCH_WITH_PARAMS
																												  TXN_MEMTABLE_BRANCH_RESULT_PARAMS),
																			void *inside_captures,
																			void (*inside_drop) (void *captures)
																			TXN_MEMTABLE_BRANCH_RESULT_PARAMS);
#endif

#ifdef TXN_MEMTABLE_BRANCH_DEFINE
/* The instance's captures for table.c's prepared: the caller's closure, the value it is handed, and its result pointers. */
typedef struct TXN_MEMTABLE_BRANCH_NAME (txn_memtable_branch_inside_t)
{
	TXN_MEMTABLE_BRANCH_RETURNS(*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedBranchDeltas *prepared
										  TXN_MEMTABLE_BRANCH_WITH_PARAMS TXN_MEMTABLE_BRANCH_RESULT_PARAMS);
	void	   *inside_captures;
	void		(*inside_drop) (void *captures);

	TXN_MEMTABLE_BRANCH_WITH_FIELDS
		TXN_MEMTABLE_BRANCH_RESULT_FIELDS
} TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside_t);

/* The instance's inside for prepared: the caller's closure, handed its value. */
static bool
TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside) (void *captures, HostIoIo *io,
													  TxnMemtableTablePreparedState *prepared, HostError *error)
{
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside_t) * c = captures;

	return c->inside(c->inside_captures, io, prepared TXN_MEMTABLE_BRANCH_WITH_ARGS(c)
					 TXN_MEMTABLE_BRANCH_RESULT_ARGS(c), error);
}

/* The instance's drop of its captures: the caller's closure's, when prepared never runs it. */
static void
TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_dropped) (void *captures)
{
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside_t) * c = captures;

	if (c->inside_drop != NULL)
		c->inside_drop(c->inside_captures);
}

/* The instance's state across its pad: the aims it built, and its captures while prepared hasn't taken them. */
typedef struct TXN_MEMTABLE_BRANCH_NAME (txn_memtable_branch_identity_prepare_frame_t)
{
	HostIoIo   *io;
	HostSharedRecord deltas;
	const txn_TupleRefSliceU8Usize_t *keys;
	size_t		keys_len;

	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside_t) * captures;
	HostError  *error;
	bool		ok;
	bool		captures_live;
	StdVec		aims;
	bool		aims_live;
} TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_frame_t);

/* The instance's pad: the aims, and the caller's closure when prepared didn't take it, while live. */
static void
TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_pad) (void *frame_)
{
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_frame_t) * frame = frame_;

	if (frame->aims_live)
	{
		frame->aims_live = false;
		std_vec_free_Checked(&frame->aims, sizeof(TxnMemtableTableAim), _Alignof(TxnMemtableTableAim));
	}
	if (frame->captures_live)
	{
		frame->captures_live = false;
		TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_dropped) (frame->captures);
	}
}

/* The instance's body: the keys' aims at the delta's identity tree, then prepared over them. */
static void
TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_body) (void *frame_)
{
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_frame_t) * frame = frame_;
	TxnMemtableTableReservation none = {0};
	bool		has_unstored;
	TxnMemtableTableReservation unstored;

	std_vec_new(&frame->aims, sizeof(TxnMemtableTableAim), _Alignof(TxnMemtableTableAim));
	frame->aims_live = true;
	if (!std_vec_try_reserve_exact(&frame->aims, frame->keys_len, sizeof(TxnMemtableTableAim),
								   _Alignof(TxnMemtableTableAim)))
	{
		*frame->error = host_Error_out_of_memory();
		TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_pad) (frame);
		return;
	}
	for (size_t k = 0; k < frame->keys_len; k++)
	{
		TxnMemtableTableAim aim = {.owner = frame->deltas, .tree = &TXN_MEMTABLE_BRANCH_IDENTITY_TREE_OPS,
		.key = frame->keys[k].f0, .need = frame->keys[k].f1};

		std_vec_push_Checked(&frame->aims, &aim, sizeof(aim), _Alignof(TxnMemtableTableAim));
	}
	frame->captures_live = false;
	frame->ok = txn_memtable_table_prepared(frame->io, frame->aims.ptr, frame->aims.len, false, none,
											TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside), frame->captures,
											TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_dropped), &has_unstored,
											&unstored, frame->error);
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_pad) (frame);
}

/*
 * The instance of identity_prepare: the pushes of identity tree keys to a
 * working state's delta readied before the checkpoint window of their
 * records and the window's work run with them and with what the caller
 * hands it, each key with the longest record its version takes; each push
 * is finished by identity_finish.
 */
bool
TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare) (HostIoIo *io, HostSharedRecord deltas,
																const txn_TupleRefSliceU8Usize_t *keys,
																size_t keys_len TXN_MEMTABLE_BRANCH_WITH_PARAMS,
																TXN_MEMTABLE_BRANCH_RETURNS(*inside) (void *captures,
																									  HostIoIo *io,
																									  TxnMemtableTablePreparedBranchDeltas *prepared
																									  TXN_MEMTABLE_BRANCH_WITH_PARAMS
																									  TXN_MEMTABLE_BRANCH_RESULT_PARAMS),
																void *inside_captures,
																void (*inside_drop) (void *captures)
																TXN_MEMTABLE_BRANCH_RESULT_PARAMS)
{
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_inside_t) captures =
	{
		inside, inside_captures, inside_drop TXN_MEMTABLE_BRANCH_WITH_INIT TXN_MEMTABLE_BRANCH_RESULT_INIT
	};
	TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_frame_t) frame =
	{
		.io = io, .deltas = deltas, .keys = keys, .keys_len = keys_len, .captures = &captures,
			.error = TXN_MEMTABLE_BRANCH_RESULT_ERROR, .captures_live = true
	};

	std_landing(&frame, TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_body),
				TXN_MEMTABLE_BRANCH_NAME(txn_memtable_branch_identity_prepare_pad));
	return frame.ok;
}
#endif

#undef TXN_MEMTABLE_BRANCH_CAT_
#undef TXN_MEMTABLE_BRANCH_CAT
#undef TXN_MEMTABLE_BRANCH_NAME
#undef TXN_MEMTABLE_BRANCH_RS
#undef TXN_MEMTABLE_BRANCH_WS
#undef TXN_MEMTABLE_BRANCH_RETURNS
#undef TXN_MEMTABLE_BRANCH_RESULT_PARAMS
#undef TXN_MEMTABLE_BRANCH_WITH_PARAMS
#undef TXN_MEMTABLE_BRANCH_DECLARE
#undef TXN_MEMTABLE_BRANCH_DEFINE
#undef TXN_MEMTABLE_BRANCH_WITH_FIELDS
#undef TXN_MEMTABLE_BRANCH_WITH_INIT
#undef TXN_MEMTABLE_BRANCH_WITH_ARGS
#undef TXN_MEMTABLE_BRANCH_RESULT_FIELDS
#undef TXN_MEMTABLE_BRANCH_RESULT_INIT
#undef TXN_MEMTABLE_BRANCH_RESULT_ARGS
#undef TXN_MEMTABLE_BRANCH_RESULT_ERROR
