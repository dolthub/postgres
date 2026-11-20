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
 * The template of trees' pin<R> (trees' pin.rs), one instance per value R
 * the scope's function returns, since R is held by value (the port text's
 * section 2). Before including it define:
 *
 * TREES_PIN_SCOPE: the instance's linkage, static or extern.
 * TREES_PIN_DECLARE and TREES_PIN_DEFINE: whether to declare and to define it.
 * TREES_PIN_RS: R's spelling in the instance's names.
 * TREES_PIN_RETURNS: R's C return type, bool for a Result.
 * TREES_PIN_RESULT_PARAMS: R's out-parameters after the scope's, each with its
 * leading comma (for a Result, section 5's result pointers and its error).
 * TREES_PIN_RESULT_FIELDS: the frame's fields that hold those pointers.
 * TREES_PIN_RESULT_INIT: the frame's designated initializers of them.
 * TREES_PIN_RESULT_ARGS: the frame's fields passed on, each with its leading
 * comma.
 *
 * It undefines every name it takes at its end.
 */
#include "doltgres/std/frozen_map.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/pin.h"

/* TREES_PIN_CAT pastes two tokens after expanding them. */
#define TREES_PIN_CAT_(a, b) a##b
#define TREES_PIN_CAT(a, b) TREES_PIN_CAT_(a, b)

/* TREES_PIN_NAME names the instance's function or type after R. */
#define TREES_PIN_NAME(name) TREES_PIN_CAT(name##_, TREES_PIN_RS)

#ifdef TREES_PIN_DECLARE
TREES_PIN_SCOPE TREES_PIN_RETURNS TREES_PIN_NAME(trees_pin_pin) (HostIoIo *io, TreesStoreNodeStore store,
																 TREES_PIN_RETURNS(*f) (void *captures,
																						HostIoIo *io,
																						TreesPinPin *pin
																						TREES_PIN_RESULT_PARAMS),
																 void *f_captures, void (*f_drop) (void *)
																 TREES_PIN_RESULT_PARAMS);
#endif

#ifdef TREES_PIN_DEFINE

/* The instance's frame is its state across its pad: its arguments, its result's pointers, and the scope it lends, while live. */
typedef struct TREES_PIN_NAME (trees_pin_pin_frame_0_t)
{
	HostIoIo   *io;
	TreesPinPin pin;
	bool		pin_live;

	TREES_PIN_RETURNS(*f) (void *captures, HostIoIo *io, TreesPinPin *pin TREES_PIN_RESULT_PARAMS);
	void	   *f_captures;
	void		(*f_drop) (void *);
	bool		f_live;

	TREES_PIN_RESULT_FIELDS
		TREES_PIN_RETURNS returned;
} TREES_PIN_NAME(trees_pin_pin_frame_0_t);

/* The instance's body: the scope made, the function run in it, its captures dropped, then the scope. */
static void
TREES_PIN_NAME(trees_pin_pin_body_0) (void *frame_)
{
	TREES_PIN_NAME(trees_pin_pin_frame_0_t) * frame = frame_;

	std_FrozenMap_Default_default(&frame->pin.nodes, &TREES_PIN_NODES_TYPE);
	frame->pin_live = true;
	frame->returned = frame->f(frame->f_captures, frame->io, &frame->pin TREES_PIN_RESULT_ARGS);
	frame->f_live = false;
	if (frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
	frame->pin_live = false;
	std_FrozenMap_drop(&frame->pin.nodes, &TREES_PIN_NODES_TYPE);
}

/* The instance's pad: the function's captures, then the scope, while live. */
static void
TREES_PIN_NAME(trees_pin_pin_pad_0) (void *frame_)
{
	TREES_PIN_NAME(trees_pin_pin_frame_0_t) * frame = frame_;

	if (frame->f_live && frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
	if (frame->pin_live)
		std_FrozenMap_drop(&frame->pin.nodes, &TREES_PIN_NODES_TYPE);
}

/*
 * The instance is pin: it runs a function, which it takes, in a scope that
 * reads nodes from the store, lending it the entry's capability, and returns
 * the function's R.
 */
TREES_PIN_SCOPE TREES_PIN_RETURNS
TREES_PIN_NAME(trees_pin_pin) (HostIoIo *io, TreesStoreNodeStore store,
							   TREES_PIN_RETURNS(*f) (void *captures, HostIoIo *io, TreesPinPin *pin
													  TREES_PIN_RESULT_PARAMS),
							   void *f_captures, void (*f_drop) (void *) TREES_PIN_RESULT_PARAMS)
{
	TREES_PIN_NAME(trees_pin_pin_frame_0_t) frame =
	{
		.io = io, .pin =
		{
			.store = store
		}, .f = f,
			.f_captures = f_captures, .f_drop = f_drop, .f_live = true,
			TREES_PIN_RESULT_INIT
	};

	std_landing(&frame, TREES_PIN_NAME(trees_pin_pin_body_0), TREES_PIN_NAME(trees_pin_pin_pad_0));
	return frame.returned;
}

#endif							/* TREES_PIN_DEFINE */

#undef TREES_PIN_CAT_
#undef TREES_PIN_CAT
#undef TREES_PIN_NAME
#undef TREES_PIN_SCOPE
#undef TREES_PIN_DECLARE
#undef TREES_PIN_DEFINE
#undef TREES_PIN_RS
#undef TREES_PIN_RETURNS
#undef TREES_PIN_RESULT_PARAMS
#undef TREES_PIN_RESULT_FIELDS
#undef TREES_PIN_RESULT_INIT
#undef TREES_PIN_RESULT_ARGS
