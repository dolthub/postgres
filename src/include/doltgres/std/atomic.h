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
 * The C forms of Rust's atomics (the port text's section 9) for code built in
 * both configurations: Postgres' pg_atomic forms in the server, and in the
 * frontend, where Postgres has none, C11's atomics with Rust's orderings
 * exactly; and core::hint::spin_loop.
 */
#ifndef DOLTGRES_STD_ATOMIC_H
#define DOLTGRES_STD_ATOMIC_H

#ifndef FRONTEND
#include "port/atomics.h"
#else
#include <stdatomic.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif
#include "doltgres/std/any.h"
#include "doltgres/std/unwind.h"

/* StdAtomicU8 is AtomicU8: in the server the 32-bit atomic holding the byte, as section 9's AtomicBool is. */
#ifndef FRONTEND
typedef pg_atomic_uint32 StdAtomicU8;
#else
typedef _Atomic uint8 StdAtomicU8;
#endif

/* STD_ATOMIC_U8_INIT is AtomicU8::new of a constant, a static's initializer. */
#ifndef FRONTEND
#define STD_ATOMIC_U8_INIT(v) {(v)}
#else
#define STD_ATOMIC_U8_INIT(v) (v)
#endif

/* std_AtomicU8_load_acquire is AtomicU8::load(Acquire): the read, then section 9's barrier. */
static inline uint8
std_AtomicU8_load_acquire(StdAtomicU8 *self)
{
#ifndef FRONTEND
	uint8		v = (uint8) pg_atomic_read_u32(self);

	pg_memory_barrier();
	return v;
#else
	return atomic_load_explicit(self, memory_order_acquire);
#endif
}

/* std_AtomicU8_store_release is AtomicU8::store(Release): section 9's barrier, then the write. */
static inline void
std_AtomicU8_store_release(StdAtomicU8 *self, uint8 v)
{
#ifndef FRONTEND
	pg_memory_barrier();
	pg_atomic_write_u32(self, v);
#else
	atomic_store_explicit(self, v, memory_order_release);
#endif
}

/*
 * std_AtomicU8_compare_exchange_weak_acquire is AtomicU8's
 * compare_exchange_weak(.., Acquire, Acquire): true when it stored, else
 * false with the value seen in *current.
 */
static inline bool
std_AtomicU8_compare_exchange_weak_acquire(StdAtomicU8 *self, uint8 *current, uint8 new_value)
{
#ifndef FRONTEND
	uint32		expected = *current;
	bool		stored = pg_atomic_compare_exchange_u32(self, &expected, new_value);

	*current = (uint8) expected;
	return stored;
#else
	return atomic_compare_exchange_weak_explicit(self, current, new_value, memory_order_acquire,
												 memory_order_acquire);
#endif
}

/* StdAtomicBool is AtomicBool: in the server the 32-bit atomic holding 0 or 1. */
#ifndef FRONTEND
typedef pg_atomic_uint32 StdAtomicBool;
#else
typedef _Atomic bool StdAtomicBool;
#endif

/* STD_ATOMIC_BOOL_INIT is AtomicBool::new of a constant, a static's initializer. */
#ifndef FRONTEND
#define STD_ATOMIC_BOOL_INIT(v) {(v)}
#else
#define STD_ATOMIC_BOOL_INIT(v) (v)
#endif

/* std_AtomicBool_new is AtomicBool::new, made in place. */
static inline void
std_AtomicBool_new(StdAtomicBool *self, bool v)
{
#ifndef FRONTEND
	pg_atomic_init_u32(self, v);
#else
	atomic_init(self, v);
#endif
}

/*
 * std_AtomicBool_Default_default is AtomicBool's Default, new(false), made in
 * place, for generic code that takes a type's Default.
 */
extern void std_AtomicBool_Default_default(void *result, const StdLocation *caller);

/* STD_ATOMIC_BOOL_TYPE is AtomicBool's size, its Default, and its drop, for generic code that makes one. */
extern const StdDefaultType STD_ATOMIC_BOOL_TYPE;

/*
 * std_AtomicU64_Default_default is AtomicU64's Default, new(0), made in
 * place, for generic code that takes a type's Default.
 */
extern void std_AtomicU64_Default_default(void *result, const StdLocation *caller);

/* STD_ATOMIC_U64_TYPE is AtomicU64's size, its Default, and its drop, for generic code that makes one. */
extern const StdDefaultType STD_ATOMIC_U64_TYPE;

/* std_AtomicBool_load_relaxed is AtomicBool::load(Relaxed). */
static inline bool
std_AtomicBool_load_relaxed(StdAtomicBool *self)
{
#ifndef FRONTEND
	return pg_atomic_read_u32(self) != 0;
#else
	return atomic_load_explicit(self, memory_order_relaxed);
#endif
}

/* std_AtomicBool_store_relaxed is AtomicBool::store(.., Relaxed). */
static inline void
std_AtomicBool_store_relaxed(StdAtomicBool *self, bool v)
{
#ifndef FRONTEND
	pg_atomic_write_u32(self, v);
#else
	atomic_store_explicit(self, v, memory_order_relaxed);
#endif
}

/* StdAtomicUsize is AtomicUsize: in the server the 64-bit atomic. */
#ifndef FRONTEND
typedef pg_atomic_uint64 StdAtomicUsize;
#else
typedef _Atomic size_t StdAtomicUsize;
#endif

/* std_AtomicUsize_new is AtomicUsize::new, made in place. */
static inline void
std_AtomicUsize_new(StdAtomicUsize *self, size_t v)
{
#ifndef FRONTEND
	pg_atomic_init_u64(self, v);
#else
	atomic_init(self, v);
#endif
}

/* std_AtomicUsize_fetch_add_relaxed is AtomicUsize::fetch_add(.., Relaxed), returning the value before. */
static inline size_t
std_AtomicUsize_fetch_add_relaxed(StdAtomicUsize *self, size_t v)
{
#ifndef FRONTEND
	return (size_t) pg_atomic_fetch_add_u64(self, (int64) v);
#else
	return atomic_fetch_add_explicit(self, v, memory_order_relaxed);
#endif
}

/* std_AtomicUsize_fetch_sub_release is AtomicUsize::fetch_sub(.., Release), returning the value before. */
static inline size_t
std_AtomicUsize_fetch_sub_release(StdAtomicUsize *self, size_t v)
{
#ifndef FRONTEND
	return (size_t) pg_atomic_fetch_sub_u64(self, (int64) v);
#else
	return atomic_fetch_sub_explicit(self, v, memory_order_release);
#endif
}

/* StdAtomicU64 is AtomicU64. */
#ifndef FRONTEND
typedef pg_atomic_uint64 StdAtomicU64;
#else
typedef _Atomic uint64 StdAtomicU64;
#endif

/* STD_ATOMIC_U64_INIT is AtomicU64::new's value, a static's initializer. */
#ifndef FRONTEND
#define STD_ATOMIC_U64_INIT(v) {(v)}
#else
#define STD_ATOMIC_U64_INIT(v) (v)
#endif

/* std_AtomicU64_new is AtomicU64::new, made in place. */
static inline void
std_AtomicU64_new(StdAtomicU64 *self, uint64 v)
{
#ifndef FRONTEND
	pg_atomic_init_u64(self, v);
#else
	atomic_init(self, v);
#endif
}

/* std_AtomicU64_load_relaxed is AtomicU64::load(Relaxed). */
static inline uint64
std_AtomicU64_load_relaxed(StdAtomicU64 *self)
{
#ifndef FRONTEND
	return pg_atomic_read_u64(self);
#else
	return atomic_load_explicit(self, memory_order_relaxed);
#endif
}

/* std_AtomicU64_store_relaxed is AtomicU64::store(.., Relaxed). */
static inline void
std_AtomicU64_store_relaxed(StdAtomicU64 *self, uint64 v)
{
#ifndef FRONTEND
	pg_atomic_write_u64(self, v);
#else
	atomic_store_explicit(self, v, memory_order_relaxed);
#endif
}

/* StdAtomicU32 is AtomicU32. */
#ifndef FRONTEND
typedef pg_atomic_uint32 StdAtomicU32;
#else
typedef _Atomic uint32 StdAtomicU32;
#endif

/* std_AtomicU32_new is AtomicU32::new, made in place. */
static inline void
std_AtomicU32_new(StdAtomicU32 *self, uint32 v)
{
#ifndef FRONTEND
	pg_atomic_init_u32(self, v);
#else
	atomic_init(self, v);
#endif
}

/* std_AtomicU32_load_relaxed is AtomicU32::load(Relaxed). */
static inline uint32
std_AtomicU32_load_relaxed(StdAtomicU32 *self)
{
#ifndef FRONTEND
	return pg_atomic_read_u32(self);
#else
	return atomic_load_explicit(self, memory_order_relaxed);
#endif
}

/* std_AtomicU32_store_relaxed is AtomicU32::store(.., Relaxed). */
static inline void
std_AtomicU32_store_relaxed(StdAtomicU32 *self, uint32 v)
{
#ifndef FRONTEND
	pg_atomic_write_u32(self, v);
#else
	atomic_store_explicit(self, v, memory_order_relaxed);
#endif
}

/* std_AtomicU64_fetch_add_relaxed is AtomicU64::fetch_add(.., Relaxed), returning the value before, wrapping. */
static inline uint64
std_AtomicU64_fetch_add_relaxed(StdAtomicU64 *self, uint64 v)
{
#ifndef FRONTEND
	return (uint64) pg_atomic_fetch_add_u64(self, (int64) v);
#else
	return atomic_fetch_add_explicit(self, v, memory_order_relaxed);
#endif
}

/* std_AtomicU64_fetch_sub_relaxed is AtomicU64::fetch_sub(.., Relaxed), returning the value before, wrapping. */
static inline uint64
std_AtomicU64_fetch_sub_relaxed(StdAtomicU64 *self, uint64 v)
{
#ifndef FRONTEND
	return (uint64) pg_atomic_fetch_sub_u64(self, (int64) v);
#else
	return atomic_fetch_sub_explicit(self, v, memory_order_relaxed);
#endif
}

/* std_atomic_fence_acquire is fence(Acquire): section 9's full barrier. */
static inline void
std_atomic_fence_acquire(void)
{
#ifndef FRONTEND
	pg_memory_barrier();
#else
	atomic_thread_fence(memory_order_acquire);
#endif
}

/* std_AtomicBool_swap_relaxed is AtomicBool::swap(.., Relaxed): the old value, the new one stored. */
static inline bool
std_AtomicBool_swap_relaxed(StdAtomicBool *self, bool v)
{
#ifndef FRONTEND
	return pg_atomic_exchange_u32(self, v ? 1 : 0) != 0;
#else
	return atomic_exchange_explicit(self, v, memory_order_relaxed);
#endif
}

/* StdAtomicI64 is AtomicI64, the 64-bit atomic through casts. */
#ifndef FRONTEND
typedef pg_atomic_uint64 StdAtomicI64;
#else
typedef _Atomic int64 StdAtomicI64;
#endif

/* STD_ATOMIC_I64_INIT is AtomicI64::new's value, a static's initializer. */
#ifndef FRONTEND
#define STD_ATOMIC_I64_INIT(v) {(uint64) (v)}
#else
#define STD_ATOMIC_I64_INIT(v) (v)
#endif

/* std_AtomicI64_load_relaxed is AtomicI64::load(Relaxed). */
static inline int64
std_AtomicI64_load_relaxed(StdAtomicI64 *self)
{
#ifndef FRONTEND
	return (int64) pg_atomic_read_u64(self);
#else
	return atomic_load_explicit(self, memory_order_relaxed);
#endif
}

/* std_AtomicI64_store_relaxed is AtomicI64::store(.., Relaxed). */
static inline void
std_AtomicI64_store_relaxed(StdAtomicI64 *self, int64 v)
{
#ifndef FRONTEND
	pg_atomic_write_u64(self, (uint64) v);
#else
	atomic_store_explicit(self, v, memory_order_relaxed);
#endif
}

/* StdAtomicPtr is AtomicPtr: in the server the 64-bit atomic holding the address. */
#ifndef FRONTEND
typedef pg_atomic_uint64 StdAtomicPtr;
#else
typedef void *_Atomic StdAtomicPtr;
#endif

/* STD_ATOMIC_PTR_INIT is AtomicPtr::new of a constant address, a static's initializer. */
#ifndef FRONTEND
#define STD_ATOMIC_PTR_INIT(p) {(uint64) (uintptr_t) (p)}
#else
#define STD_ATOMIC_PTR_INIT(p) ((void *) (p))
#endif

/* std_AtomicPtr_load_relaxed is AtomicPtr::load(Relaxed). */
static inline void *
std_AtomicPtr_load_relaxed(StdAtomicPtr *self)
{
#ifndef FRONTEND
	return (void *) (uintptr_t) pg_atomic_read_u64(self);
#else
	return atomic_load_explicit(self, memory_order_relaxed);
#endif
}

/* std_AtomicPtr_store_relaxed is AtomicPtr::store(.., Relaxed). */
static inline void
std_AtomicPtr_store_relaxed(StdAtomicPtr *self, void *v)
{
#ifndef FRONTEND
	pg_atomic_write_u64(self, (uint64) (uintptr_t) v);
#else
	atomic_store_explicit(self, v, memory_order_relaxed);
#endif
}

/* std_AtomicPtr_load_acquire is AtomicPtr::load(Acquire): the read, then a barrier. */
static inline void *
std_AtomicPtr_load_acquire(StdAtomicPtr *self)
{
#ifndef FRONTEND
	void	   *v = (void *) (uintptr_t) pg_atomic_read_u64(self);

	pg_memory_barrier();
	return v;
#else
	return atomic_load_explicit(self, memory_order_acquire);
#endif
}

/* std_AtomicPtr_store_release is AtomicPtr::store(.., Release): a barrier, then the write. */
static inline void
std_AtomicPtr_store_release(StdAtomicPtr *self, void *v)
{
#ifndef FRONTEND
	pg_memory_barrier();
	pg_atomic_write_u64(self, (uint64) (uintptr_t) v);
#else
	atomic_store_explicit(self, v, memory_order_release);
#endif
}

/* std_hint_spin_loop is core::hint::spin_loop: x86-64's pause, aarch64's isb sy. */
static inline void
std_hint_spin_loop(void)
{
#if defined(_MSC_VER) && defined(_M_ARM64)
	__isb(_ARM64_BARRIER_SY);
#elif defined(__aarch64__)
	__asm__ __volatile__("isb sy");
#else
	_mm_pause();
#endif
}

#endif							/* DOLTGRES_STD_ATOMIC_H */
