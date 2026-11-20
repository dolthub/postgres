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
 * The C forms of Rust's dyn Any, Deref, and DerefMut (the port text's
 * section 8): a value with the identity of its type, and the tables of the
 * two traits every view of a capability implements.
 */
#ifndef DOLTGRES_STD_ANY_H
#define DOLTGRES_STD_ANY_H

#include "doltgres/std/unwind.h"

/*
 * StdTypeId is a Rust type's identity and the vtable a Box<dyn Any> drops
 * through, one constant per type made into an Any, compared by address: its
 * name, its size and alignment, and the drop of its value, NULL for a type
 * whose drop does nothing.
 */
typedef struct StdTypeId
{
	const char *name;
	size_t		size;
	size_t		align;
	void		(*drop) (void *value);
} StdTypeId;

/* STD_UNIT_TYPE_ID is the identity of (), the value a test's capability holds. */
extern const StdTypeId STD_UNIT_TYPE_ID;

/*
 * StdAny is a reference to a dyn Any, or a Box<dyn Any> that owns its value:
 * the value and its type's identity.
 */
typedef struct StdAny
{
	const StdTypeId *type;
	void	   *value;
} StdAny;

/* std_any_downcast is downcast_ref: the value if it has the type, or NULL. */
static inline void *
std_any_downcast(StdAny any, const StdTypeId *type)
{
	return any.type == type ? any.value : NULL;
}

/*
 * std_any_box_drop is the drop of a Box<dyn Any>: its value's drop, then the
 * free of its allocation, as Rust's drop through the vtable does.
 */
extern void std_any_box_drop(StdAny *self);

/* StdDerefOps is Deref's table: deref returns the target. */
typedef struct StdDerefOps
{
	const void *(*deref) (const void *self);
} StdDerefOps;

/* StdDerefMutOps is DerefMut's table, its supertrait Deref's first. */
typedef struct StdDerefMutOps
{
	StdDerefOps deref;
	void	   *(*deref_mut) (void *self);
} StdDerefMutOps;

/*
 * StdDefaultType is what generic code needs of a type parameter T bounded by
 * Default: its size, its Default, and its drop glue, NULL for a type whose
 * drop does nothing.
 */
typedef struct StdDefaultType
{
	size_t		size;
	size_t		align;
	void		(*default_) (void *result, const StdLocation *caller);
	void		(*drop) (void *value);

	/*
	 * arc_caller is the location Arc's Default passes T's Default when that
	 * is #[track_caller], else NULL.
	 */
	const StdLocation *arc_caller;
} StdDefaultType;

extern void std_default_call(void *captures, void *result);

#endif							/* DOLTGRES_STD_ANY_H */
