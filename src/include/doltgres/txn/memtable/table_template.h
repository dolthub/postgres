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
 * The template of table.rs's functions that hold their closure's result R by
 * value (txn's memtable/table.rs), one instance per R, as pin<R> is: prepare
 * and prepare_striped at State. Before including it define:
 *
 * TXN_MEMTABLE_TABLE_RS: R's spelling in the instance's names.
 * TXN_MEMTABLE_TABLE_RETURNS: R's C return type, bool for a Result.
 * TXN_MEMTABLE_TABLE_RESULT_PARAMS: R's out-parameters, each with its leading
 * comma (for a Result, section 5's result pointers and its error).
 * TXN_MEMTABLE_TABLE_DECLARE and TXN_MEMTABLE_TABLE_DEFINE: whether to declare
 * and to define it; the definition runs table.c's readied, carrying the
 * closure's result pointers in its captures, and takes:
 * TXN_MEMTABLE_TABLE_RESULT_FIELDS: the result pointers' fields, each with
 * its semicolon, and none for a closure whose only out-parameter is its
 * error.
 * TXN_MEMTABLE_TABLE_RESULT_INIT: their initializers, each with its leading
 * comma.
 * TXN_MEMTABLE_TABLE_RESULT_ARGS(c): the pointers from captures c, each with
 * its leading comma, which the closure takes before its error.
 * TXN_MEMTABLE_TABLE_RESULT_ERROR: the instance's error parameter's name.
 *
 * Every instance's own Result is section 5's bool with an error, and its
 * closure is (fn, captures, drop), taken by value. It undefines every name it
 * takes at its end.
 */
#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/txn/memtable/table.h"

/* TXN_MEMTABLE_TABLE_CAT pastes two tokens after expanding them. */
#define TXN_MEMTABLE_TABLE_CAT_(a, b) a##b
#define TXN_MEMTABLE_TABLE_CAT(a, b) TXN_MEMTABLE_TABLE_CAT_(a, b)

/* TXN_MEMTABLE_TABLE_NAME names the instance's function after R. */
#define TXN_MEMTABLE_TABLE_NAME(name) TXN_MEMTABLE_TABLE_CAT(name##_, TXN_MEMTABLE_TABLE_RS)

#ifdef TXN_MEMTABLE_TABLE_DECLARE
extern bool TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_prepare) (HostIoIo *io, HostSharedRecord state,
																 const txn_TupleRefSliceU8Usize_t *rows, size_t rows_len,
																 const TxnMemtableMapTake *takes, size_t takes_len,
																 bool tuple,
																 TXN_MEMTABLE_TABLE_RETURNS(*inside) (void *captures,
																									  HostIoIo *io,
																									  TxnMemtableTablePreparedState *prepared
																									  TXN_MEMTABLE_TABLE_RESULT_PARAMS),
																 void *inside_captures, void (*inside_drop) (void *captures)
																 TXN_MEMTABLE_TABLE_RESULT_PARAMS);
extern bool TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_prepare_striped) (HostIoIo *io, HostIoWaits *waits,
																		 HostSharedRecord state,
																		 const txn_TupleRefSliceU8Usize_t *rows,
																		 size_t rows_len,
																		 const TxnMemtableMapTake *takes,
																		 size_t takes_len, bool tuple,
																		 TXN_MEMTABLE_TABLE_RETURNS(*inside) (void *captures,
																											  HostIoIo *io,
																											  TxnMemtableTablePreparedState *prepared,
																											  HostIoStriping striping
																											  TXN_MEMTABLE_TABLE_RESULT_PARAMS),
																		 void *inside_captures,
																		 void (*inside_drop) (void *captures)
																		 TXN_MEMTABLE_TABLE_RESULT_PARAMS);
#endif

#ifdef TXN_MEMTABLE_TABLE_DEFINE
/* The instance's captures for readied: the caller's closure, the striping it hands on, and the result pointers. */
typedef struct TXN_MEMTABLE_TABLE_NAME (txn_memtable_table_inside_t)
{
	TXN_MEMTABLE_TABLE_RETURNS(*inside) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared
										 TXN_MEMTABLE_TABLE_RESULT_PARAMS);
	TXN_MEMTABLE_TABLE_RETURNS(*striped) (void *captures, HostIoIo *io, TxnMemtableTablePreparedState *prepared,
										  HostIoStriping striping TXN_MEMTABLE_TABLE_RESULT_PARAMS);
	void	   *inside_captures;
	void		(*inside_drop) (void *captures);
	HostIoStriping striping;

	TXN_MEMTABLE_TABLE_RESULT_FIELDS
} TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside_t);

/* The instance's inside for readied: the caller's closure, given the striping when it takes one. */
static bool
TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside) (void *captures, HostIoIo *io,
													TxnMemtableTablePreparedState *prepared, HostError *error)
{
	TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside_t) * c = captures;

	if (c->striped != NULL)
		return c->striped(c->inside_captures, io, prepared, c->striping TXN_MEMTABLE_TABLE_RESULT_ARGS(c), error);
	return c->inside(c->inside_captures, io, prepared TXN_MEMTABLE_TABLE_RESULT_ARGS(c), error);
}

/* The instance's drop of its captures: the caller's closure's, when readied never runs it. */
static void
TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_dropped) (void *captures)
{
	TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside_t) * c = captures;

	if (c->inside_drop != NULL)
		c->inside_drop(c->inside_captures);
}

/*
 * The instance of prepare: a storage's row pushes and map takes readied
 * before a checkpoint window, the window's work run with them.
 */
bool
TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_prepare) (HostIoIo *io, HostSharedRecord state,
													 const txn_TupleRefSliceU8Usize_t *rows, size_t rows_len,
													 const TxnMemtableMapTake *takes, size_t takes_len, bool tuple,
													 TXN_MEMTABLE_TABLE_RETURNS(*inside) (void *captures,
																						  HostIoIo *io,
																						  TxnMemtableTablePreparedState *prepared
																						  TXN_MEMTABLE_TABLE_RESULT_PARAMS),
													 void *inside_captures, void (*inside_drop) (void *captures)
													 TXN_MEMTABLE_TABLE_RESULT_PARAMS)
{
	TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside_t) captures =
	{
		inside, NULL, inside_captures, inside_drop,
		{
			0
		} TXN_MEMTABLE_TABLE_RESULT_INIT
	};

	return txn_memtable_table_readied(io, true, state, rows, rows_len, takes, takes_len, tuple,
									  TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside), &captures,
									  TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_dropped), NULL,
									  TXN_MEMTABLE_TABLE_RESULT_ERROR);
}

/*
 * The instance of prepare_striped: prepare's readying for a change that
 * takes its keys' stripes of the key lock, handing inside the striping token
 * made from the caller's Waits.
 */
bool
TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_prepare_striped) (HostIoIo *io, HostIoWaits *waits,
															 HostSharedRecord state,
															 const txn_TupleRefSliceU8Usize_t *rows, size_t rows_len,
															 const TxnMemtableMapTake *takes, size_t takes_len,
															 bool tuple,
															 TXN_MEMTABLE_TABLE_RETURNS(*inside) (void *captures,
																								  HostIoIo *io,
																								  TxnMemtableTablePreparedState *prepared,
																								  HostIoStriping striping
																								  TXN_MEMTABLE_TABLE_RESULT_PARAMS),
															 void *inside_captures,
															 void (*inside_drop) (void *captures)
															 TXN_MEMTABLE_TABLE_RESULT_PARAMS)
{
	TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside_t) captures =
	{
		NULL, inside, inside_captures, inside_drop,
			host_io_Waits_striping(waits) TXN_MEMTABLE_TABLE_RESULT_INIT
	};

	return txn_memtable_table_readied(io, true, state, rows, rows_len, takes, takes_len, tuple,
									  TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_inside), &captures,
									  TXN_MEMTABLE_TABLE_NAME(txn_memtable_table_dropped), NULL,
									  TXN_MEMTABLE_TABLE_RESULT_ERROR);
}
#endif

#undef TXN_MEMTABLE_TABLE_CAT_
#undef TXN_MEMTABLE_TABLE_CAT
#undef TXN_MEMTABLE_TABLE_NAME
#undef TXN_MEMTABLE_TABLE_RS
#undef TXN_MEMTABLE_TABLE_RETURNS
#undef TXN_MEMTABLE_TABLE_RESULT_PARAMS
#undef TXN_MEMTABLE_TABLE_DECLARE
#undef TXN_MEMTABLE_TABLE_DEFINE
#undef TXN_MEMTABLE_TABLE_RESULT_FIELDS
#undef TXN_MEMTABLE_TABLE_RESULT_INIT
#undef TXN_MEMTABLE_TABLE_RESULT_ARGS
#undef TXN_MEMTABLE_TABLE_RESULT_ERROR
