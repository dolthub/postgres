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
 * The C form of Rust's dyn Any (the port text's section 8): the identity of
 * (), and the drop of a Box<dyn Any> through its type's vtable.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/any.h"
#include "doltgres/std/error.h"
#include "doltgres/std/ptr.h"

const StdTypeId STD_UNIT_TYPE_ID = {"()", 0, 1, NULL};

/*
 * std_any_box_drop drops a Box<dyn Any>'s value and frees its allocation, as
 * the crates' code does, with Layout::for_value_raw's check.
 */
void
std_any_box_drop(StdAny *self)
{
	if (self->type->drop != NULL)
		self->type->drop(self->value);
	std_Layout_for_value_raw_check(self->type->size, self->type->align);
	std_free(self->value, self->type->size, self->type->align);
}

/* std_dyn_error_drop drops a Box<dyn Error>'s value and frees its allocation. */
void
std_dyn_error_drop(StdDynError *self)
{
	if (self->ops->drop != NULL)
		self->ops->drop(self->self);
	std_free(self->self, self->ops->size, self->ops->align);
}

/* std_dyn_error_Display_fmt is a Box<dyn Error>'s Display, its value's through the vtable. */
bool
std_dyn_error_Display_fmt(const void *value, StdFormatter *f)
{
	const StdDynError *self = (const StdDynError *) value;

	return self->ops->display(self->self, f);
}

/* std_dyn_error_Debug_fmt is a Box<dyn Error>'s Debug, its value's through the vtable. */
bool
std_dyn_error_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdDynError *self = (const StdDynError *) value;

	return self->ops->debug(self->self, f);
}

/* std_default_call is T::default passed as an FnOnce() -> T, its captures T's StdDefaultType. */
void
std_default_call(void *captures, void *result)
{
	const StdDefaultType *t = captures;

	t->default_(result, NULL);
}
