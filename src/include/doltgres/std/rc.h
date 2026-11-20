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
 * The C form of alloc's Rc<T> (the port text's section 9), for a sized T and
 * for a dyn Trait's value through its table's size, alignment, and drop:
 * RcInner's block, repr(C), of the strong count, the weak count, and the
 * value at the data offset its alignment gives.
 */
#ifndef DOLTGRES_STD_RC_H
#define DOLTGRES_STD_RC_H

#include "doltgres/std/fmt.h"

/* StdRcDyn is an Rc<dyn Trait>: its RcInner's block, and the value's trait table, whose first fields are its drop, size, and alignment. */
typedef struct StdRcDyn
{
	void	   *ptr;
	const void *vtable;
} StdRcDyn;

extern void *std_Rc_new(const void *value, size_t size, size_t align);
extern void *std_Rc_deref(void *ptr, size_t align);
extern void std_Rc_clone(void *ptr);
extern void std_Rc_drop(void *ptr, size_t size, size_t align, void (*drop) (void *value));
extern bool std_Rc_Debug_fmt(void *ptr, size_t align, StdFmtFn value_fmt, StdFormatter *f);

#endif							/* DOLTGRES_STD_RC_H */
