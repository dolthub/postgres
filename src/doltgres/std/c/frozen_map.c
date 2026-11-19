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
 * elsa 1.11.2's FrozenMap (the port text's section 15), from its map.rs: an
 * append-only hash map whose insert and get take it shared, a std HashMap of
 * keys to separately allocated values behind a flag that refuses a
 * reentrant use, whose values' targets stay put until the map drops.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/frozen_map.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/unwind.h"

/* ELSA_MAP_FILE is map.rs as the remapped build names it, where a reentrant use panics. */
#define ELSA_MAP_FILE "cargo-registry/index.crates.io-1949cf8c6b5b557f" STD_SEP "elsa-1.11.2" STD_SEP "src" STD_SEP "map.rs"

/* std_FrozenMap_Default_default is FrozenMap's Default: an empty map of S's default hasher, not in use. */
void
std_FrozenMap_Default_default(StdFrozenMap *result, const StdHashMapType *t)
{
	std_HashMap_Default_default(&result->map, t);
	result->in_use = false;
}

/*
 * std_FrozenMap_insert_frame_0_t is insert's state across its pad: its
 * arguments, its result, and whether it still holds the key and the value.
 */
typedef struct std_FrozenMap_insert_frame_0_t
{
	StdFrozenMap *self;
	const StdHashMapType *t;
	const StdDerefOps *deref;
	void	   *k;
	bool		k_live;
	void	   *v;
	bool		v_live;
	const void *result;
} std_FrozenMap_insert_frame_0_t;

/* std_FrozenMap_insert_body_0 is insert's body. */
static void
std_FrozenMap_insert_body_0(void *frame_)
{
	std_FrozenMap_insert_frame_0_t *frame = frame_;
	StdHashMapEntry entry;
	void	   *elem;
	void	   *value;

	if (frame->self->in_use)
	{
		static const char text[] = "assertion failed: !self.in_use.get()";

		std_panic(ELSA_MAP_FILE, 74, 9, text, sizeof(text) - 1);
	}
	frame->self->in_use = true;
	entry = std_HashMap_entry(&frame->self->map, frame->t, frame->k);
	if (entry.occupied)
	{
		elem = entry.elem;
		frame->k_live = false;
		if (frame->t->key_drop != NULL)
			frame->t->key_drop(frame->k);
		frame->v_live = false;
		if (frame->t->value_drop != NULL)
			frame->t->value_drop(frame->v);
	}
	else
	{
		frame->k_live = false;
		frame->v_live = false;
		elem = std_HashMap_VacantEntry_insert(&frame->self->map, frame->t, &entry, frame->k, frame->v);
	}
	value = STD_PTR_AT(elem, frame->t->value_offset, 1);
	frame->self->in_use = false;
	frame->result = frame->deref->deref(value);
}

/* std_FrozenMap_insert_pad_0 is insert's pad: the key, which entry owns, then the value, while it holds them. */
static void
std_FrozenMap_insert_pad_0(void *frame_)
{
	std_FrozenMap_insert_frame_0_t *frame = frame_;

	if (frame->k_live && frame->t->key_drop != NULL)
		frame->t->key_drop(frame->k);
	if (frame->v_live && frame->t->value_drop != NULL)
		frame->t->value_drop(frame->v);
}

/*
 * std_FrozenMap_insert is FrozenMap::insert: the target of the key's value,
 * the value given when the map lacked the key, which it takes along with
 * the key, or the one it held, the given key and value dropped then. It
 * panics on a use while another of the map's methods runs.
 */
const void *
std_FrozenMap_insert(StdFrozenMap *self, const StdHashMapType *t, const StdDerefOps *deref, void *k, void *v)
{
	std_FrozenMap_insert_frame_0_t frame = {.self = self, .t = t, .deref = deref, .k = k, .k_live = true, .v = v,
	.v_live = true};

	std_landing(&frame, std_FrozenMap_insert_body_0, std_FrozenMap_insert_pad_0);
	return frame.result;
}

/*
 * std_FrozenMap_get is FrozenMap::get: the target of the key's value, or
 * NULL when the map lacks the key. It panics on a use while another of the
 * map's methods runs.
 */
const void *
std_FrozenMap_get(StdFrozenMap *self, const StdHashMapType *t, const StdDerefOps *deref, const void *k)
{
	const void *elem;
	const void *result;

	if (self->in_use)
	{
		static const char text[] = "assertion failed: !self.in_use.get()";

		std_panic(ELSA_MAP_FILE, 105, 9, text, sizeof(text) - 1);
	}
	self->in_use = true;
	elem = std_HashMap_get(&self->map, t, k);
	result = elem == NULL ? NULL : deref->deref(STD_PTR_AT(elem, t->value_offset, 1));
	self->in_use = false;
	return result;
}

/* std_FrozenMap_drop is FrozenMap's drop glue: its map. */
void
std_FrozenMap_drop(StdFrozenMap *self, const StdHashMapType *t)
{
	std_HashMap_drop(&self->map, t);
}
