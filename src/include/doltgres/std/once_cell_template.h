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
 * The template of core's OnceCell<T> (the port text's section 9), ported
 * from core/src/cell/once.rs at Rust 1.99.1: new, get_or_init through
 * get_or_try_init's try_init, and the drop glue. A cell is its
 * Option<T>, a flag and the value, set at most once. STD_ONCE_CELL_SCOPE
 * is static or extern, STD_ONCE_CELL_DECLARE and STD_ONCE_CELL_DEFINE
 * choose the declarations and definitions, STD_ONCE_CELL_TS spells T,
 * STD_ONCE_CELL_T is T, and STD_ONCE_CELL_DROP is T's drop glue, left
 * undefined for a T with none; every parameter is undefined at the end.
 */

#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/* STD_ONCE_CELL_CAT pastes two tokens after expanding them. */
#define STD_ONCE_CELL_CAT_(a, b) a##b
#define STD_ONCE_CELL_CAT(a, b) STD_ONCE_CELL_CAT_(a, b)

/* STD_ONCE_CELL_DEF is STD_ONCE_CELL_SCOPE's storage class for a definition: static, or none for extern. */
#define STD_ONCE_CELL_DEF_static static
#define STD_ONCE_CELL_DEF_extern
#define STD_ONCE_CELL_DEF STD_ONCE_CELL_CAT(STD_ONCE_CELL_DEF_, STD_ONCE_CELL_SCOPE)

/* STD_ONCE_CELL_NAME names the instance's functions. */
#define STD_ONCE_CELL_NAME(name) STD_ONCE_CELL_CAT(std_OnceCell_##name##_, STD_ONCE_CELL_TS)

/* STD_ONCE_CELL_TYPE names the instance's cell. */
#define STD_ONCE_CELL_TYPE STD_ONCE_CELL_CAT(StdOnceCell, STD_ONCE_CELL_TS)

/* STD_ONCE_CELL_FRAME names get_or_init's frame. */
#define STD_ONCE_CELL_FRAME STD_ONCE_CELL_NAME(get_or_init_frame_0_t)

#ifdef STD_ONCE_CELL_DECLARE

/* The instance's cell, OnceCell<T>: whether it holds a value, and the value. */
typedef struct STD_ONCE_CELL_TYPE
{
	bool		has_inner;
	STD_ONCE_CELL_T inner;
}			STD_ONCE_CELL_TYPE;

STD_ONCE_CELL_SCOPE void STD_ONCE_CELL_NAME(new) (STD_ONCE_CELL_TYPE * result);
STD_ONCE_CELL_SCOPE const STD_ONCE_CELL_T *STD_ONCE_CELL_NAME(get_or_init) (STD_ONCE_CELL_TYPE * self,
																			void (*f) (void *captures,
																					   STD_ONCE_CELL_T * result),
																			void *f_captures,
																			void (*f_drop) (void *));
STD_ONCE_CELL_SCOPE void STD_ONCE_CELL_NAME(drop) (STD_ONCE_CELL_TYPE * self);

#endif							/* STD_ONCE_CELL_DECLARE */

#ifdef STD_ONCE_CELL_DEFINE

/* The instance's new is OnceCell::new: an empty cell. */
STD_ONCE_CELL_DEF void
STD_ONCE_CELL_NAME(new) (STD_ONCE_CELL_TYPE * result)
{
	result->has_inner = false;
}

/* The instance's get_or_init frame is its state across its pad: the cell, the function and whether it still owns its captures. */
typedef struct STD_ONCE_CELL_FRAME
{
	STD_ONCE_CELL_TYPE *self;
	void		(*f) (void *captures, STD_ONCE_CELL_T * result);
	void	   *f_captures;
	void		(*f_drop) (void *);
	bool		f_live;
}			STD_ONCE_CELL_FRAME;

/*
 * The instance's get_or_init body is try_init's: the function's value, its
 * captures dropped once it returns, then the value put in the cell, or
 * dropped with "reentrant init" when the function filled the cell itself.
 */
static void
STD_ONCE_CELL_NAME(get_or_init_body_0) (void *frame_)
{
	STD_ONCE_CELL_FRAME *frame = frame_;
	STD_ONCE_CELL_T val;

	frame->f(frame->f_captures, &val);
	frame->f_live = false;
	if (frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
	if (frame->self->has_inner)
	{
		static const char text[] = "reentrant init";

#ifdef STD_ONCE_CELL_DROP
		STD_ONCE_CELL_DROP(&val);
#endif
		std_panic(STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "cell" STD_SEP "once.rs", 325, 66, text,
				  sizeof(text) - 1);
	}
	frame->self->inner = val;
	frame->self->has_inner = true;
}

/* The instance's get_or_init pad drops the function's captures while it still owns them. */
static void
STD_ONCE_CELL_NAME(get_or_init_pad_0) (void *frame_)
{
	STD_ONCE_CELL_FRAME *frame = frame_;

	if (frame->f_live && frame->f_drop != NULL)
		frame->f_drop(frame->f_captures);
}

/*
 * The instance's get_or_init is OnceCell::get_or_init: the cell's value,
 * made by the function, which it takes, when the cell is empty, the
 * function dropped unused otherwise.
 */
STD_ONCE_CELL_DEF const STD_ONCE_CELL_T *
STD_ONCE_CELL_NAME(get_or_init) (STD_ONCE_CELL_TYPE * self, void (*f) (void *captures, STD_ONCE_CELL_T * result),
								 void *f_captures, void (*f_drop) (void *))
{
	STD_ONCE_CELL_FRAME frame = {.self = self, .f = f, .f_captures = f_captures, .f_drop = f_drop, .f_live = true};

	if (self->has_inner)
	{
		if (f_drop != NULL)
			f_drop(f_captures);
		return &self->inner;
	}
	std_landing(&frame, STD_ONCE_CELL_NAME(get_or_init_body_0), STD_ONCE_CELL_NAME(get_or_init_pad_0));
	return &self->inner;
}

/* The instance's drop is OnceCell's drop glue: its value, when it holds one. */
STD_ONCE_CELL_DEF void
STD_ONCE_CELL_NAME(drop) (STD_ONCE_CELL_TYPE * self)
{
#ifdef STD_ONCE_CELL_DROP
	if (self->has_inner)
		STD_ONCE_CELL_DROP(&self->inner);
#endif
}

#endif							/* STD_ONCE_CELL_DEFINE */

#undef STD_ONCE_CELL_CAT_
#undef STD_ONCE_CELL_CAT
#undef STD_ONCE_CELL_DEF_static
#undef STD_ONCE_CELL_DEF_extern
#undef STD_ONCE_CELL_DEF
#undef STD_ONCE_CELL_NAME
#undef STD_ONCE_CELL_TYPE
#undef STD_ONCE_CELL_FRAME
#undef STD_ONCE_CELL_SCOPE
#undef STD_ONCE_CELL_DECLARE
#undef STD_ONCE_CELL_DEFINE
#undef STD_ONCE_CELL_TS
#undef STD_ONCE_CELL_T
#undef STD_ONCE_CELL_DROP
