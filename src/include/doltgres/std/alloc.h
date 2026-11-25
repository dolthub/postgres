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
 * The port's allocator (the port text's section 9): the one way the port
 * allocates, at the points the Rust's global allocator is called, with a
 * failure unwinding as the Rust's allocation error hook makes it.
 */
#ifndef DOLTGRES_STD_ALLOC_H
#define DOLTGRES_STD_ALLOC_H

#include "doltgres/std/any.h"

/*
 * STD_DANGLING is the address of a zero-size allocation and an empty slice,
 * never NULL, as Rust's NonNull::dangling is never null.
 */
#define STD_DANGLING ((void *) (uintptr_t) MAXIMUM_ALIGNOF)

/*
 * STD_DANGLING_FOR is NonNull::dangling of a type of an alignment: the
 * alignment as an address, an empty Vec's and a zero-size allocation's
 * pointer, aligned for the type and never NULL.
 */
#define STD_DANGLING_FOR(align) ((void *) (uintptr_t) (align))

/* StdAllocErrorHook is a hook set_alloc_error_hook sets: the function a failed allocation's layout is given to. */
typedef struct StdAllocErrorHook
{
	void		(*hook) (size_t size, size_t align);
} StdAllocErrorHook;

#ifndef FRONTEND
extern void std_heap_make(void);
extern uint64 std_heap_reads_bytes(void);
#endif
extern void *std_heap_current(void);
extern void *std_heap_reads_entered(void);
extern void std_heap_left(void *previous);
extern void *std_System_GlobalAlloc_alloc(size_t size, size_t align);
extern void *std_System_GlobalAlloc_alloc_zeroed(size_t size, size_t align);
extern void *std_System_GlobalAlloc_realloc(void *ptr, size_t old_size, size_t align, size_t new_size);
extern void std_System_GlobalAlloc_dealloc(void *ptr, size_t size, size_t align);
extern void std_alloc_set_alloc_error_hook(const StdAllocErrorHook *hook);
extern void *std_global_alloc(size_t size, size_t align);
extern void *std_global_alloc_zeroed(size_t size, size_t align);
extern void *std_global_realloc(void *ptr, size_t old_size, size_t align, size_t new_size);
extern void std_global_dealloc(void *ptr, size_t size, size_t align);
pg_noreturn extern void std_handle_alloc_error(size_t size, size_t align);
extern void *std_alloc(size_t size, size_t align);
#if (defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)) && !defined(DOLTGRES_GLOBAL_ALLOCATOR)
extern void test_std_alloc_tests_fail_allocation(uint64 nth);
extern void test_std_alloc_tests_fail_reallocation(uint64 nth);
extern uint64 test_std_alloc_tests_in_use(void);
#endif
extern void std_free(void *ptr, size_t size, size_t align);
extern const void *std_box_Deref_deref(const void *self);

extern const StdDerefOps STD_BOX_DEREF_OPS;

#endif							/* DOLTGRES_STD_ALLOC_H */
