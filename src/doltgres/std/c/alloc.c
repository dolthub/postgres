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
 * The port's allocator (the port text's section 9), the allocator door's one
 * caller in the port: std's System allocator, which in the server is the port
 * heap, one memory context of the backend's life as the Rust's heap is, which
 * the process's first entry makes, and elsewhere each platform's; the global
 * allocator's shims, which call System's unless the configuration's
 * #[global_allocator] crate defines them (DOLTGRES_GLOBAL_ALLOCATOR); and
 * handle_alloc_error with std's alloc error hook, the hook set with
 * set_alloc_error_hook or std's default.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/std/alloc.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"

#ifndef FRONTEND
#include "utils/memutils.h"

/* std_port_heap is the heap the port's allocations go to, the port heap std_heap_make makes at the process's first entry. */
static MemoryContext std_port_heap = NULL;

/* std_reads_heap is the spill reads' heap, a child of the port heap std_heap_make makes with it (bugs row 487). */
static MemoryContext std_reads_heap = NULL;

/*
 * std_heap_make makes the port heap, a child of TopMemoryContext that a
 * critical section may allocate in, as the Rust's system allocator can, if
 * the process has none yet. The entry calls it before any port frame, since
 * making a context can raise.
 */
void
std_heap_make(void)
{
	if (std_port_heap != NULL)
		return;
	std_port_heap = AllocSetContextCreate(TopMemoryContext, "doltgres port heap", ALLOCSET_DEFAULT_SIZES);
	MemoryContextAllowInCriticalSection(std_port_heap, true);
	std_reads_heap = AllocSetContextCreate(std_port_heap, "doltgres spill reads", ALLOCSET_DEFAULT_SIZES);
	MemoryContextAllowInCriticalSection(std_reads_heap, true);
}

/* std_heap_current returns the heap the port's allocations go to now, which std_heap_left puts back. */
void *
std_heap_current(void)
{
	return std_port_heap;
}

/*
 * std_heap_reads_entered makes the spill reads' heap the one the port's
 * allocations go to, so a spill read's own allocations show as that heap's
 * in pg_backend_memory_contexts (bugs row 487), answering the heap it
 * replaces, which std_heap_left puts back; a free finds each allocation's
 * own heap, so frees need no switch.
 */
void *
std_heap_reads_entered(void)
{
	MemoryContext previous = std_port_heap;

	std_port_heap = std_reads_heap;
	return previous;
}

/* std_heap_left puts back the heap std_heap_reads_entered replaced, or one std_heap_current answered. */
void
std_heap_left(void *previous)
{
	std_port_heap = previous;
}

/* std_heap_reads_bytes returns the bytes the spill reads' heap holds in use, as Postgres accounts them, for the cases. */
uint64
std_heap_reads_bytes(void)
{
	MemoryContextCounters consumed;

	MemoryContextMemConsumed(std_reads_heap, &consumed);
	return (uint64) (consumed.totalspace - consumed.freespace);
}

/*
 * std_System_heap_alloc is the port heap's allocation with no error: NULL for
 * a size the heap can't take, as the system refuses one.
 */
static void *
std_System_heap_alloc(size_t size, size_t align, int flags)
{
	flags |= MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM;
	if (!AllocHugeSizeIsValid(size))
		return NULL;
	if (align <= MAXIMUM_ALIGNOF)
		return MemoryContextAllocExtended(std_port_heap, size, flags);
	return MemoryContextAllocAligned(std_port_heap, size, align, flags);
}

/* std_System_GlobalAlloc_alloc is System's alloc: the port heap's, NULL when it fails. */
void *
std_System_GlobalAlloc_alloc(size_t size, size_t align)
{
	return std_System_heap_alloc(size, align, 0);
}

/* std_System_GlobalAlloc_alloc_zeroed is System's alloc_zeroed: the port heap's, zeroed, NULL when it fails. */
void *
std_System_GlobalAlloc_alloc_zeroed(size_t size, size_t align)
{
	return std_System_heap_alloc(size, align, MCXT_ALLOC_ZERO);
}

/*
 * std_System_GlobalAlloc_realloc is System's realloc: the port heap's, which
 * keeps the block's alignment, NULL for a size it can't take, the block kept.
 */
void *
std_System_GlobalAlloc_realloc(void *ptr, size_t old_size, size_t align, size_t new_size)
{
	if (!AllocHugeSizeIsValid(new_size))
		return NULL;
	return repalloc_extended(ptr, new_size, MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);
}

/* std_System_GlobalAlloc_dealloc is System's dealloc: the port heap's free. */
void
std_System_GlobalAlloc_dealloc(void *ptr, size_t size, size_t align)
{
	pfree(ptr);
}
#else

/* std_heap_current is the frontend's: its one allocator has no heaps, so it answers none. */
void *
std_heap_current(void)
{
	return NULL;
}

/* std_heap_reads_entered is the frontend's: its one allocator has no heaps to switch, so it answers none. */
void *
std_heap_reads_entered(void)
{
	return NULL;
}

/* std_heap_left is the frontend's, which has no heap to put back. */
void
std_heap_left(void *previous)
{
}

/*
 * STD_MIN_ALIGN is sys::alloc's MIN_ALIGN on the 64-bit targets: the
 * alignment the system's plain allocation gives.
 */
#define STD_MIN_ALIGN 16

#ifdef WIN32
/* STD_HEAP_ZERO_MEMORY is HEAP_ZERO_MEMORY, HeapAlloc's flag for zeroed memory. */
#define STD_HEAP_ZERO_MEMORY 0x00000008

/* std_System_process_heap_alloc is process_heap_alloc: HeapAlloc from the process's heap, NULL if it has none. */
static void *
std_System_process_heap_alloc(DWORD flags, size_t bytes)
{
	HANDLE		heap = GetProcessHeap();

	if (unlikely(heap == NULL))
		return NULL;
	return HeapAlloc(heap, flags, bytes);
}

/*
 * std_System_allocate is Windows' allocate: HeapAlloc, and for an alignment
 * above MIN_ALIGN a block of the alignment more, aligned within it, with the
 * block's own address in the Header word before the aligned one.
 */
static void *
std_System_allocate(size_t size, size_t align, bool zeroed)
{
	DWORD		flags = zeroed ? STD_HEAP_ZERO_MEMORY : 0;
	uint8	   *ptr;
	uint8	   *aligned;

	if (align <= STD_MIN_ALIGN)
		return std_System_process_heap_alloc(flags, size);
	ptr = std_System_process_heap_alloc(flags, align + size);
	if (ptr == NULL)
		return NULL;
	aligned = ptr + (align - ((uintptr_t) ptr & (align - 1)));
	((uint8 **) aligned)[-1] = ptr;
	return aligned;
}

/* std_System_GlobalAlloc_alloc is System's alloc. */
void *
std_System_GlobalAlloc_alloc(size_t size, size_t align)
{
	return std_System_allocate(size, align, false);
}

/* std_System_GlobalAlloc_alloc_zeroed is System's alloc_zeroed. */
void *
std_System_GlobalAlloc_alloc_zeroed(size_t size, size_t align)
{
	return std_System_allocate(size, align, true);
}

/* std_System_GlobalAlloc_dealloc is System's dealloc: HeapFree of the block, through its Header for an alignment above MIN_ALIGN. */
void
std_System_GlobalAlloc_dealloc(void *ptr, size_t size, size_t align)
{
	void	   *block = align <= STD_MIN_ALIGN ? ptr : ((void **) ptr)[-1];

	HeapFree(GetProcessHeap(), 0, block);
}
#else

/* std_System_aligned_malloc is aligned_malloc: posix_memalign at the alignment, at least a pointer's. */
static void *
std_System_aligned_malloc(size_t size, size_t align)
{
	void	   *out = NULL;

	if (posix_memalign(&out, Max(align, sizeof(size_t)), size) != 0)
		return NULL;
	return out;
}

/* std_System_GlobalAlloc_alloc is System's alloc: malloc, or aligned_malloc above MIN_ALIGN or for a size below the alignment. */
void *
std_System_GlobalAlloc_alloc(size_t size, size_t align)
{
	if (align <= STD_MIN_ALIGN && align <= size)
		return malloc(size);
#ifdef __APPLE__
	if (align > ((size_t) 1 << 31))
		return NULL;
#endif
	return std_System_aligned_malloc(size, align);
}

/* std_System_GlobalAlloc_alloc_zeroed is System's alloc_zeroed: calloc, or alloc then zeroing. */
void *
std_System_GlobalAlloc_alloc_zeroed(size_t size, size_t align)
{
	void	   *ptr;

	if (align <= STD_MIN_ALIGN && align <= size)
		return calloc(size, 1);
	ptr = std_System_GlobalAlloc_alloc(size, align);
	if (ptr != NULL)
		memset(ptr, 0, size);
	return ptr;
}

/* std_System_GlobalAlloc_dealloc is System's dealloc: free. */
void
std_System_GlobalAlloc_dealloc(void *ptr, size_t size, size_t align)
{
	free(ptr);
}
#endif

/*
 * std_System_realloc_fallback is realloc_fallback: a new block of the old
 * alignment, the bytes both sizes hold copied, and the old block freed.
 */
static void *
std_System_realloc_fallback(void *ptr, size_t old_size, size_t align, size_t new_size)
{
	void	   *new_ptr = std_System_GlobalAlloc_alloc(new_size, align);

	if (new_ptr != NULL)
	{
		memcpy(new_ptr, ptr, std_min_usize(old_size, new_size));
		std_System_GlobalAlloc_dealloc(ptr, old_size, align);
	}
	return new_ptr;
}

/* std_System_GlobalAlloc_realloc is System's realloc: the system's in place, or realloc_fallback. */
void *
std_System_GlobalAlloc_realloc(void *ptr, size_t old_size, size_t align, size_t new_size)
{
#ifdef WIN32
	if (align <= STD_MIN_ALIGN)
		return HeapReAlloc(GetProcessHeap(), 0, ptr, new_size);
#else
	if (align <= STD_MIN_ALIGN && align <= new_size)
		return realloc(ptr, new_size);
#endif
	return std_System_realloc_fallback(ptr, old_size, align, new_size);
}
#endif

/* std_alloc_HOOK is alloc's HOOK: the hook set_alloc_error_hook set, or NULL for the default. */
static StdAtomicPtr std_alloc_HOOK = STD_ATOMIC_PTR_INIT(NULL);

/* std_alloc_set_alloc_error_hook is set_alloc_error_hook: the hook stored with Release. */
void
std_alloc_set_alloc_error_hook(const StdAllocErrorHook *hook)
{
	std_AtomicPtr_store_release(&std_alloc_HOOK, (void *) hook);
}

/* std_alloc_PREV_ALLOC_FAILURE is default_alloc_error_hook's PREV_ALLOC_FAILURE: whether an allocation has failed before. */
static StdAtomicBool std_alloc_PREV_ALLOC_FAILURE = STD_ATOMIC_BOOL_INIT(false);

/*
 * std_alloc_default_alloc_error_hook is default_alloc_error_hook: the size's
 * report to stderr, and after no earlier failure the backtrace the style
 * asks for, or the note on RUST_BACKTRACE.
 */
static void
std_alloc_default_alloc_error_hook(size_t size, size_t align)
{
	StdStderr	err = std_Stderr_new();
	StdIoWrite	out;
	char		digits[24];
	int			style;

	snprintf(digits, sizeof(digits), "%zu", size);
	if (std_AtomicBool_swap_relaxed(&std_alloc_PREV_ALLOC_FAILURE, true))
	{
		static const char skipping[] = " bytes failed\nskipping backtrace printing to avoid potential recursion\n";

		if (std_Stderr_write_all(&err, "memory allocation of ", 21) && std_Stderr_write_all(&err, digits, strlen(digits)))
			(void) std_Stderr_write_all(&err, skipping, sizeof(skipping) - 1);
		return;
	}
	if (std_Stderr_write_all(&err, "memory allocation of ", 21) && std_Stderr_write_all(&err, digits, strlen(digits)))
		(void) std_Stderr_write_all(&err, " bytes failed\n", 14);
	err = std_Stderr_new();
	out = std_Stderr_as_write(&err);
	style = std_panic_get_backtrace_style();
	if (style == STD_BACKTRACE_SHORT || style == STD_BACKTRACE_FULL)
		std_backtrace_print(&out, style);
	else if (style == STD_BACKTRACE_OFF)
	{
		static const char off[] = "note: run with `RUST_BACKTRACE=1` environment variable to display a backtrace\n";

		(void) std_Stderr_write_all(&err, off, sizeof(off) - 1);
	}
}

/*
 * std_handle_alloc_error is handle_alloc_error with std's rust_oom: the hook
 * loaded with Acquire, set_alloc_error_hook's or the default, called with
 * the layout, then process::abort.
 */
void
std_handle_alloc_error(size_t size, size_t align)
{
	const StdAllocErrorHook *hook = std_AtomicPtr_load_acquire(&std_alloc_HOOK);

	if (hook == NULL)
		std_alloc_default_alloc_error_hook(size, align);
	else
		hook->hook(size, align);
	std_process_abort();
}

#ifndef DOLTGRES_GLOBAL_ALLOCATOR
#if defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)
/* std_alloc_tests_FAILING is how many allocations come before the one a test fails, plus one, or 0 for none. */
static uint64 std_alloc_tests_FAILING = 0;

/*
 * test_std_alloc_tests_fail_allocation fails the nth of the global
 * allocator's next allocations, counting from 1, as an allocator out of
 * memory does, so that a test reaches what a failed allocation does.
 */
void
test_std_alloc_tests_fail_allocation(uint64 nth)
{
	std_alloc_tests_FAILING = nth;
}

/* std_alloc_tests_REFAILING is how many reallocations come before the one a test fails, plus one, or 0 for none. */
static uint64 std_alloc_tests_REFAILING = 0;

/*
 * test_std_alloc_tests_fail_reallocation fails the nth of the global
 * allocator's next reallocations, counting from 1, as an allocator out of
 * memory does when a block grows.
 */
void
test_std_alloc_tests_fail_reallocation(uint64 nth)
{
	std_alloc_tests_REFAILING = nth;
}

/* std_alloc_tests_IN_USE is how many bytes the global allocator has handed out and not taken back. */
static uint64 std_alloc_tests_IN_USE = 0;

/* test_std_alloc_tests_in_use returns how many bytes the global allocator has handed out and not taken back. */
uint64
test_std_alloc_tests_in_use(void)
{
	return std_alloc_tests_IN_USE;
}
#endif

/* std_global_alloc is alloc::alloc::alloc: the global allocator's, System's, NULL when it fails. */
void *
std_global_alloc(size_t size, size_t align)
{
#if defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)
	if (std_alloc_tests_FAILING != 0 && --std_alloc_tests_FAILING == 0)
		return NULL;
	{
		void	   *ptr = std_System_GlobalAlloc_alloc(size, align);

		if (ptr != NULL)
			std_alloc_tests_IN_USE += size;
		return ptr;
	}
#else
	return std_System_GlobalAlloc_alloc(size, align);
#endif
}

/* std_global_alloc_zeroed is alloc::alloc::alloc_zeroed: System's, NULL when it fails. */
void *
std_global_alloc_zeroed(size_t size, size_t align)
{
	void	   *ptr = std_System_GlobalAlloc_alloc_zeroed(size, align);

#if defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)
	if (ptr != NULL)
		std_alloc_tests_IN_USE += size;
#endif
	return ptr;
}

/* std_global_realloc is alloc::alloc::realloc: System's, NULL when it fails, the block kept. */
void *
std_global_realloc(void *ptr, size_t old_size, size_t align, size_t new_size)
{
	void	   *moved;

#if defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)
	if (std_alloc_tests_REFAILING != 0 && --std_alloc_tests_REFAILING == 0)
		return NULL;
#endif
	moved = std_System_GlobalAlloc_realloc(ptr, old_size, align, new_size);

#if defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)
	if (moved != NULL)
		std_alloc_tests_IN_USE += new_size - old_size;
#endif
	return moved;
}

/* std_global_dealloc is alloc::alloc::dealloc: System's. */
void
std_global_dealloc(void *ptr, size_t size, size_t align)
{
	std_System_GlobalAlloc_dealloc(ptr, size, align);
#if defined(DOLTGRES_TEST_HOST) || defined(DOLTGRES_TEST_CHUNKS)
	std_alloc_tests_IN_USE -= size;
#endif
}
#endif

/*
 * std_alloc allocates size bytes at an alignment, as std's allocation calls
 * the global allocator and handle_alloc_error when it fails, or nothing for
 * a zero size, for which std calls no allocator.
 */
void *
std_alloc(size_t size, size_t align)
{
	void	   *ptr;

	if (size == 0)
		return STD_DANGLING_FOR(align);
	ptr = std_global_alloc(size, align);
	if (ptr == NULL)
		std_handle_alloc_error(size, align);
	return ptr;
}

/* std_free frees an allocation, and nothing for a zero size. */
void
std_free(void *ptr, size_t size, size_t align)
{
	if (size != 0)
		std_global_dealloc(ptr, size, align);
}

/* std_box_Deref_deref is Box<T>'s Deref: the T the box points to, self being the box. */
const void *
std_box_Deref_deref(const void *self)
{
	return *(void *const *) self;
}

/* STD_BOX_DEREF_OPS is Box<T>'s Deref table. */
const StdDerefOps STD_BOX_DEREF_OPS = {
	.deref = std_box_Deref_deref,
};
