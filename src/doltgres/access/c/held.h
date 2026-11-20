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
 * What access' landing frames hold across their pads: up to four vectors,
 * freed on an unwind, which the port's internals share between key.c and
 * heaptuple.c.
 */
#ifndef DOLTGRES_ACCESS_HELD_H
#define DOLTGRES_ACCESS_HELD_H

#include "doltgres/std/vec.h"

/*
 * access_key_held_t is what a key frame holds across its pad, as the frame's
 * first member: up to four vectors, each with its element size and
 * alignment, the live ones freed on an unwind.
 */
typedef struct access_key_held_t
{
	StdVec		vec[4];
	size_t		size[4];
	size_t		align[4];
	bool		live[4];
} access_key_held_t;

/* access_key_held_pad frees the vectors a frame holds live. */
static inline void
access_key_held_pad(void *frame_)
{
	access_key_held_t *held = frame_;

	for (size_t i = 0; i < lengthof(held->vec); i++)
	{
		if (held->live[i])
			std_vec_free_Checked(&held->vec[i], held->size[i], held->align[i]);
		held->live[i] = false;
	}
}

/* access_key_held_new starts an empty vector in a slot of what a frame holds. */
static inline StdVec *
access_key_held_new(access_key_held_t *held, size_t slot, size_t size, size_t align)
{
	std_vec_new(&held->vec[slot], size, align);
	held->size[slot] = size;
	held->align[slot] = align;
	held->live[slot] = true;
	return &held->vec[slot];
}

/* access_key_held_take takes the vector of a slot out of what a frame holds, for a caller that owns it from then on. */
static inline StdVec
access_key_held_take(access_key_held_t *held, size_t slot)
{
	held->live[slot] = false;
	return held->vec[slot];
}


#endif							/* DOLTGRES_ACCESS_HELD_H */
