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
 * The template of pg's run<G, R> (pg's lib.rs), one instance per
 * capabilities G and value R an entry runs with, since both are held by
 * value (the port text's section 2). Before including it define:
 *
 * PG_RUN_SCOPE: the instance's functions' linkage, static or empty.
 * PG_RUN_DECLARE and PG_RUN_DEFINE: whether to declare and to define them.
 * PG_RUN_GS and PG_RUN_RS: G's and R's spellings in the instance's names.
 * PG_RUN_G: G's C type, and PG_RUN_G_OPS: its Grant table.
 * PG_RUN_R: R's C type, absent when R is ().
 *
 * It undefines every name it takes at its end.
 */
#include "doltgres/pg/lib.h"
#include "doltgres/std/unwind.h"

/* PG_RUN_CAT2 pastes two tokens after expanding them, and PG_RUN_CAT3 three. */
#define PG_RUN_CAT2_(a, b) a##b
#define PG_RUN_CAT2(a, b) PG_RUN_CAT2_(a, b)
#define PG_RUN_CAT3(a, b, c) PG_RUN_CAT2(PG_RUN_CAT2(a, b), c)
/* PG_RUN_NAME names an instance's function or type after G and R. */
#define PG_RUN_NAME(name) PG_RUN_CAT3(name, _, PG_RUN_CAT2(PG_RUN_GS, PG_RUN_RS))

/* PG_RUN_CLOSURE is the instance's catch closure, pg_run_<G><R>_closure_0, and PG_RUN_CAPTURES its captures' type. */
#define PG_RUN_CLOSURE PG_RUN_CAT2(PG_RUN_NAME(pg_run), _closure_0)
#define PG_RUN_CAPTURES PG_RUN_CAT3(PgRun, PG_RUN_CAT2(PG_RUN_GS, PG_RUN_RS), Closure0)

#ifdef PG_RUN_R
#define PG_RUN_DEFAULT_PARAM , PG_RUN_R default_
#define PG_RUN_DEFAULT default_
#define PG_RUN_RETURNS PG_RUN_R
#define PG_RUN_RESULT_PARAM , PG_RUN_R *result
#else
#define PG_RUN_DEFAULT_PARAM
#define PG_RUN_RETURNS void
#define PG_RUN_RESULT_PARAM
#endif

/* PG_RUN_BODY is a body run takes: a closure of the capabilities giving a result or a PgError. */
#define PG_RUN_BODY bool (*body) (void *captures, const PG_RUN_G *caps PG_RUN_RESULT_PARAM, PgPgError *error)

#ifdef PG_RUN_DECLARE
PG_RUN_SCOPE PG_RUN_RETURNS PG_RUN_NAME(pg_run) (DgOutcome *outcome PG_RUN_DEFAULT_PARAM, PG_RUN_BODY,
												 void *body_captures, void (*body_drop) (void *));
#endif

#ifdef PG_RUN_DEFINE

/* PG_RUN_CAPTURES is run's catch closure's captures: the body, and where its result goes. */
typedef struct PG_RUN_CAPTURES
{
	PG_RUN_BODY;
	void	   *body_captures;
	bool		ok;
#ifdef PG_RUN_R
	PG_RUN_R	value;
#endif
	PgPgError	error;
}			PG_RUN_CAPTURES;

/* PG_RUN_CLOSURE runs the body with the capabilities G grants. */
static void
PG_RUN_CLOSURE(void *captures)
{
	PG_RUN_CAPTURES *c = captures;
	PgSealedToken token = {0};
	PG_RUN_G	caps;

	PG_RUN_G_OPS.grant(token, &caps);
#ifdef PG_RUN_R
	c->ok = c->body(c->body_captures, &caps, &c->value, &c->error);
#else
	c->ok = c->body(c->body_captures, &caps, &c->error);
#endif
}

/*
 * PG_RUN_NAME(pg_run) runs an entry's body with the capabilities its entry
 * list line grants, retaking the reserve first for an ordinary entry, and
 * writes how it ended into the outcome its C thunk raises from; it returns
 * the body's value, or the default when the thunk will raise. outcome points
 * to a DgOutcome live for the call and used by nothing else, as the C entry
 * thunk's own is.
 */
PG_RUN_SCOPE PG_RUN_RETURNS
PG_RUN_NAME(pg_run) (DgOutcome *outcome PG_RUN_DEFAULT_PARAM, PG_RUN_BODY, void *body_captures,
					 void (*body_drop) (void *))
{
	PG_RUN_CAPTURES captures;
	StdCaught	ended;

	pg_install();
	if (PG_RUN_G_OPS.ORDINARY && !pg_retake_reserve())
	{
		outcome->kind = DG_OUT_OF_MEMORY;
		outcome->size = (size_t) pg_atomic_read_membarrier_u64(&PG_FAILED_SIZE);
		if (body_drop != NULL)
			body_drop(body_captures);
#ifdef PG_RUN_R
		return PG_RUN_DEFAULT;
#else
		return;
#endif
	}
	captures.body = body;
	captures.body_captures = body_captures;
	if (std_catch_unwind(PG_RUN_CLOSURE, &captures, &ended))
	{
		if (captures.ok)
		{
#ifdef PG_RUN_R
			return captures.value;
#else
			return;
#endif
		}
		else
		{
			DgError		raw;
			bool		has_fork;
			StdString	fork;
			StdStr		hint;

			pg_PgError_into_raw(&captures.error, &raw, &has_fork, &fork, &hint);
			outcome->error = raw;
			if (has_fork)
			{
				pg_written(outcome->message, fork.ptr, fork.len);
				if (hint.ptr == NULL)
					hint = (StdStr)
				{
					"", 0
				};
				pg_written(outcome->hint, (const uint8 *) hint.ptr, hint.len);
				outcome->kind = DG_FORK_ERROR;
				std_vec_free_Checked(&fork, 1, 1);
			}
			else
				outcome->kind = DG_POSTGRES_ERROR;
#ifdef PG_RUN_R
			return PG_RUN_DEFAULT;
#else
			return;
#endif
		}
	}
	pg_report(outcome, &ended);
#ifdef PG_RUN_R
	return PG_RUN_DEFAULT;
#endif
}

#endif							/* PG_RUN_DEFINE */

#undef PG_RUN_CAT2_
#undef PG_RUN_CAT2
#undef PG_RUN_CAT3
#undef PG_RUN_NAME
#undef PG_RUN_CLOSURE
#undef PG_RUN_CAPTURES
#undef PG_RUN_DEFAULT_PARAM
#undef PG_RUN_DEFAULT
#undef PG_RUN_RETURNS
#undef PG_RUN_RESULT_PARAM
#undef PG_RUN_BODY
#undef PG_RUN_SCOPE
#undef PG_RUN_DECLARE
#undef PG_RUN_DEFINE
#undef PG_RUN_GS
#undef PG_RUN_RS
#undef PG_RUN_G
#undef PG_RUN_G_OPS
#undef PG_RUN_R
