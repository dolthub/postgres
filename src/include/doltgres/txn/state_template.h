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
 * The template of state.rs's held, which holds its call's result T by value
 * (txn's state.rs), one instance per T, as pin<R> is. Before including it
 * define:
 *
 * TXN_STATE_TS: T's spelling in the instance's names.
 * TXN_STATE_RETURNS: T's C return type in the call, bool for a Result.
 * TXN_STATE_RESULT_PARAMS: T's out-parameters, each with its leading comma
 * (for a Result, section 5's result pointers and its error).
 * TXN_STATE_DECLARE and TXN_STATE_DEFINE: whether to declare and to define
 * it; the definition lends the call through state.c's held_call, carrying
 * the call's result pointers in its captures, and takes:
 * TXN_STATE_RESULT_FIELDS: the result pointers' fields, each with its
 * semicolon, and none for a call whose only out-parameter is its error.
 * TXN_STATE_RESULT_INIT: their initializers, each with its leading comma.
 * TXN_STATE_RESULT_ARGS(c): the pointers from captures c, each with its
 * leading comma, which the call takes before its error.
 * TXN_STATE_RESULT_ERROR: the instance's error parameter's name.
 *
 * Every instance's own Result is section 5's bool with an error, and its call
 * is (fn, captures, drop), taken by value. It undefines every name it takes
 * at its end.
 */
#include "doltgres/host/io.h"
#include "doltgres/host/shared.h"
#include "doltgres/txn/state.h"

/* TXN_STATE_CAT pastes two tokens after expanding them. */
#define TXN_STATE_CAT_(a, b) a##b
#define TXN_STATE_CAT(a, b) TXN_STATE_CAT_(a, b)

#ifdef TXN_STATE_DECLARE
extern bool TXN_STATE_CAT(txn_state_held_, TXN_STATE_TS) (HostIoIo *io, const RepoRepo *repo,
														  const HostSharedReader *reading, TxnStateFound found,
														  TXN_STATE_RETURNS(*call) (void *captures, HostIoIo *io,
																					const RepoRepo *repo,
																					const HostSharedHeld *held
																					TXN_STATE_RESULT_PARAMS),
														  void *call_captures, void (*call_drop) (void *captures)
														  TXN_STATE_RESULT_PARAMS);
#endif

#ifdef TXN_STATE_DEFINE
/* TXN_STATE_NAME names the instance's own names after T. */
#define TXN_STATE_NAME(name) TXN_STATE_CAT(name##_, TXN_STATE_TS)

/* The instance's captures for held_call: the caller's call and the result pointers it writes. */
typedef struct TXN_STATE_NAME (txn_state_held_t)
{
	TXN_STATE_RETURNS(*call) (void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held
							  TXN_STATE_RESULT_PARAMS);
	void	   *call_captures;
	void		(*call_drop) (void *captures);

	TXN_STATE_RESULT_FIELDS
} TXN_STATE_NAME(txn_state_held_t);

/* The instance's lent call: the caller's call at the held state, writing its result through the captures' pointers. */
static bool
TXN_STATE_NAME(txn_state_held_lent) (void *captures, HostIoIo *io, const RepoRepo *repo, const HostSharedHeld *held,
									 HostError *error)
{
	TXN_STATE_NAME(txn_state_held_t) * c = captures;

	return c->call(c->call_captures, io, repo, held TXN_STATE_RESULT_ARGS(c), error);
}

/* The instance's drop of its captures: the caller's call's, when held_call never lends it. */
static void
TXN_STATE_NAME(txn_state_held_dropped) (void *captures)
{
	TXN_STATE_NAME(txn_state_held_t) * c = captures;

	if (c->call_drop != NULL)
		c->call_drop(c->call_captures);
}

/*
 * The instance: held lends the call the state of a storage the backend's
 * top-level transaction holds, taking the find and the call.
 */
bool
TXN_STATE_CAT(txn_state_held_, TXN_STATE_TS) (HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
											  TxnStateFound found,
											  TXN_STATE_RETURNS(*call) (void *captures, HostIoIo *io,
																		const RepoRepo *repo,
																		const HostSharedHeld *held
																		TXN_STATE_RESULT_PARAMS),
											  void *call_captures, void (*call_drop) (void *captures)
											  TXN_STATE_RESULT_PARAMS)
{
	TXN_STATE_NAME(txn_state_held_t) captures =
	{
		call, call_captures, call_drop TXN_STATE_RESULT_INIT
	};

	return txn_state_held_call(io, repo, reading, &found, TXN_STATE_NAME(txn_state_held_lent), &captures,
							   TXN_STATE_NAME(txn_state_held_dropped), TXN_STATE_RESULT_ERROR);
}

#undef TXN_STATE_NAME
#endif

#undef TXN_STATE_CAT_
#undef TXN_STATE_CAT
#undef TXN_STATE_TS
#undef TXN_STATE_RETURNS
#undef TXN_STATE_RESULT_PARAMS
#undef TXN_STATE_DECLARE
#undef TXN_STATE_DEFINE
#undef TXN_STATE_RESULT_FIELDS
#undef TXN_STATE_RESULT_INIT
#undef TXN_STATE_RESULT_ARGS
#undef TXN_STATE_RESULT_ERROR
