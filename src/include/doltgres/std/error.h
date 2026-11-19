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
 * The C form of Rust's Box<dyn std::error::Error + Send + Sync> (the port
 * text's section 8): an owned value with the table of the Error trait and
 * its supertraits Debug and Display, and its vtable's drop and size.
 */
#ifndef DOLTGRES_STD_ERROR_H
#define DOLTGRES_STD_ERROR_H

#include "doltgres/std/fmt.h"

/* StdDynErrorOps is a boxed error's vtable: its Debug, its Display, its drop, and its size and alignment. */
typedef struct StdDynErrorOps
{
	StdFmtFn	debug;
	StdFmtFn	display;
	void		(*drop) (void *self);
	size_t		size;
	size_t		align;
} StdDynErrorOps;

/* StdDynError is a Box<dyn Error + Send + Sync>, which owns its value. */
typedef struct StdDynError
{
	const StdDynErrorOps *ops;
	void	   *self;
} StdDynError;

/* std_dyn_error_drop drops a boxed error's value and frees its allocation. */
extern void std_dyn_error_drop(StdDynError *self);
extern bool std_dyn_error_Display_fmt(const void *value, StdFormatter *f);
extern bool std_dyn_error_Debug_fmt(const void *value, StdFormatter *f);

#endif							/* DOLTGRES_STD_ERROR_H */
