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
 * The address index (chunks' index.rs): a set of runs, each a file of
 * entries sorted by address in 4 KB blocks, each ending in a checksum of its
 * entries, of which only the first address of each block stays in memory.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/atomic.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/chunks/record.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "index.rs"

/* chunks_index_Address_Keyed_key is Address's Keyed::key: the address itself. */
const ChunksAddressAddress *
chunks_index_Address_Keyed_key(const void *self)
{
	return self;
}

/* chunks_index_TupleAddress_Keyed_key is (Address, V)'s Keyed::key: the tuple's address, its first field. */
const ChunksAddressAddress *
chunks_index_TupleAddress_Keyed_key(const void *self)
{
	return self;
}

/* CHUNKS_INDEX_ADDRESS_KEYED_OPS is Address's Keyed table. */
const ChunksIndexKeyedOps CHUNKS_INDEX_ADDRESS_KEYED_OPS = {chunks_index_Address_Keyed_key};

/* CHUNKS_INDEX_TUPLE_ADDRESS_KEYED_OPS is (Address, V)'s Keyed table, the same for every V. */
const ChunksIndexKeyedOps CHUNKS_INDEX_TUPLE_ADDRESS_KEYED_OPS = {chunks_index_TupleAddress_Keyed_key};

/*
 * chunks_index_first_from returns the place in sorted values of the first
 * whose key isn't below a key: every value before it is below the key and
 * none from it on is.
 */
size_t
chunks_index_first_from(size_t t_size, const ChunksIndexKeyedOps *t, const void *values, size_t values_len,
						const ChunksAddressAddress *key)
{
	size_t		low = 0;
	size_t		high = values_len;

	while (low < high)
	{
		size_t		mid = STD_ADD(usize, low, STD_DIV_FOLDED(usize, STD_SUB(usize, high, low, 102, 25), 2), 102, 19);

		std_index_check(mid, values_len, RUST_FILE, 103, 12);
		if (chunks_address_Address_before(t->key(STD_PTR_AT(values, mid, t_size)), key))
			low = STD_ADD(usize, mid, 1, 122, 19);
		else
			high = mid;
	}
	return low;
}

/*
 * chunks_index_first_past returns the place in sorted values of the first
 * whose key is above a key: no value before it is above the key and every
 * value from it on is.
 */
size_t
chunks_index_first_past(size_t t_size, const ChunksIndexKeyedOps *t, const void *values, size_t values_len,
						const ChunksAddressAddress *key)
{
	size_t		low = 0;
	size_t		high = values_len;

	while (low < high)
	{
		size_t		mid = STD_ADD(usize, low, STD_DIV_FOLDED(usize, STD_SUB(usize, high, low, 176, 25), 2), 176, 19);

		std_index_check(mid, values_len, RUST_FILE, 177, 23);
		if (chunks_address_Address_before(key, t->key(STD_PTR_AT(values, mid, t_size))))
			high = mid;
		else
			low = STD_ADD(usize, mid, 1, 218, 19);
	}
	return low;
}

/* chunks_index_in_order reports whether values are sorted, which a decode checks before anything searches them. */
bool
chunks_index_in_order(size_t t_size, const ChunksIndexKeyedOps *t, const void *values, size_t values_len)
{
	size_t		k = 1;

	while (k < values_len)
	{
		const ChunksAddressAddress *later;
		size_t		before;

		std_index_check(k, values_len, RUST_FILE, 240, 12);
		later = t->key(STD_PTR_AT(values, k, t_size));
		before = STD_SUB(usize, k, 1, 240, 42);
		std_index_check(before, values_len, RUST_FILE, 240, 35);
		if (chunks_address_Address_before(later, t->key(STD_PTR_AT(values, before, t_size))))
			return false;
		k = STD_ADD(usize, k, 1, 266, 9);
	}
	return true;
}

/*
 * chunks_index_block_range returns the blocks of a run that can hold a key,
 * by the run's fences, each a block's first key: the block before the first
 * fence that isn't below the key through the last block whose fence isn't
 * above it.
 */
void
chunks_index_block_range(const ChunksAddressAddress *fences, size_t fences_len, const ChunksAddressAddress *key,
						 size_t *out_0, size_t *out_1)
{
	size_t		from = chunks_index_first_from(sizeof(ChunksAddressAddress), &CHUNKS_INDEX_ADDRESS_KEYED_OPS, fences,
											   fences_len, key);

	*out_0 = from == 0 ? 0 : STD_SUB(usize, from, 1, 299, 9);
	*out_1 = chunks_index_first_past(sizeof(ChunksAddressAddress), &CHUNKS_INDEX_ADDRESS_KEYED_OPS, fences,
									 fences_len, key);
}

/* chunks_index_first_with returns the place of the first of sorted entries with a key, or false when none has it. */
bool
chunks_index_first_with(size_t entries_size, const void *entries, size_t entries_len, const ChunksAddressAddress *key,
						size_t *result)
{
	size_t		at = chunks_index_first_from(entries_size, &CHUNKS_INDEX_TUPLE_ADDRESS_KEYED_OPS, entries,
											 entries_len, key);

	if (at < entries_len)
	{
		std_index_check(at, entries_len, RUST_FILE, 337, 30);
		if (chunks_address_Address_same(STD_PTR_AT(entries, at, entries_size), key))
		{
			*result = at;
			return true;
		}
	}
	return false;
}

/*
 * chunks_index_block_fits reports whether decoded entries are the run's block
 * at a position, which a block's decode checks, so that what the fences say
 * of a block is true of the entries a search reads.
 */
bool
chunks_index_block_fits(size_t entries_size, const void *entries, size_t entries_len,
						const ChunksAddressAddress *fences, size_t fences_len, size_t i)
{
	size_t		next;
	size_t		last;

	if (i >= fences_len || entries_len == 0 ||
		!chunks_index_in_order(entries_size, &CHUNKS_INDEX_TUPLE_ADDRESS_KEYED_OPS, entries, entries_len))
		return false;
	std_index_check(0, entries_len, RUST_FILE, 366, 74);
	std_index_check(i, fences_len, RUST_FILE, 367, 10);
	if (!chunks_address_Address_same(entries, &fences[i]))
		return false;
	if (STD_ADD(usize, i, 1, 371, 5) >= fences_len)
		return true;
	next = STD_ADD(usize, i, 1, 371, 38);
	std_index_check(next, fences_len, RUST_FILE, 371, 31);
	last = STD_SUB(usize, entries_len, 1, 371, 61);
	std_index_check(last, entries_len, RUST_FILE, 371, 53);
	return !chunks_address_Address_before(&fences[next], STD_PTR_AT(entries, last, entries_size));
}

/*
 * chunks_index_Fences_new_frame_0_t is Fences::new's state across its pad:
 * its keys, which it owns until a Fences holds them, and its result.
 */
typedef struct chunks_index_Fences_new_frame_0_t
{
	StdVec		keys;
	bool		keys_live;
	ChunksIndexFences *result;
	bool		ok;
} chunks_index_Fences_new_frame_0_t;

/* chunks_index_Fences_new_body_0 is Fences::new's body. */
static void
chunks_index_Fences_new_body_0(void *frame_)
{
	chunks_index_Fences_new_frame_0_t *frame = frame_;

	if (chunks_index_in_order(sizeof(ChunksAddressAddress), &CHUNKS_INDEX_ADDRESS_KEYED_OPS, frame->keys.ptr,
							  frame->keys.len))
	{
		frame->keys_live = false;
		frame->result->keys = frame->keys;
		frame->ok = true;
		return;
	}
	frame->keys_live = false;
	std_vec_free_Checked(&frame->keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
}

/* chunks_index_Fences_new_pad_0 is Fences::new's pad: its keys, while it owns them. */
static void
chunks_index_Fences_new_pad_0(void *frame_)
{
	chunks_index_Fences_new_frame_0_t *frame = frame_;

	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
}

/* chunks_index_Fences_new returns the fences of keys that are in order, taking the keys, or false for keys that aren't. */
bool
chunks_index_Fences_new(StdVec *keys, ChunksIndexFences *result)
{
	chunks_index_Fences_new_frame_0_t frame = {.keys = *keys, .keys_live = true, .result = result};

	std_landing(&frame, chunks_index_Fences_new_body_0, chunks_index_Fences_new_pad_0);
	return frame.ok;
}

/* chunks_index_Fences_keys returns the fences' keys, which are sorted. */
const ChunksAddressAddress *
chunks_index_Fences_keys(const ChunksIndexFences *self, size_t *result_len)
{
	*result_len = self->keys.len;
	return self->keys.ptr;
}

/* chunks_index_Fences_drop is Fences' drop glue: its keys. */
void
chunks_index_Fences_drop(void *self_)
{
	ChunksIndexFences *self = self_;

	std_vec_free_Checked(&self->keys, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
}

/*
 * chunks_index_Blocks_SourceTupleUsizeUsizeVecTupleAddressError_ask is the
 * blocks' Source::ask: the decoded block at a run's and a block's positions,
 * read through the runs' own block.
 */
bool
chunks_index_Blocks_SourceTupleUsizeUsizeVecTupleAddressError_ask(void *self, void *with, const void *question,
																  void *result, void *error)
{
	const ChunksIndexBlocks *blocks = self;
	const std_TupleUsizeUsize_t *at = question;

	return blocks->runs.ops->block(blocks->runs.self, with, at->f0, at->f1, result, error);
}

/* CHUNKS_INDEX_BLOCKS_SOURCE_OPS is the blocks' Source table. */
const ChunksIndexSourceOps CHUNKS_INDEX_BLOCKS_SOURCE_OPS = {
	chunks_index_Blocks_SourceTupleUsizeUsizeVecTupleAddressError_ask
};

/* CHUNKS_INDEX_MAGIC starts and ends every run file. */
const uint8 CHUNKS_INDEX_MAGIC[8] = {0x5d, 0x0b, 0xc6, 0x92, 0x3f, 0x7e, 0x18, 0xe8};

/* chunks_index_Locator_Fixed_write is Locator's Fixed::write: its stored bytes appended. */
void
chunks_index_Locator_Fixed_write(const ChunksTrailerLocator *self, StdVec *out)
{
	chunks_trailer_Locator_write(self, out);
}

/* chunks_index_Locator_Fixed_read is Locator's Fixed::read: the locator its stored bytes hold. */
ChunksTrailerLocator
chunks_index_Locator_Fixed_read(const uint8 *bytes, size_t bytes_len)
{
	return chunks_trailer_Locator_read(bytes, bytes_len);
}

/* chunks_index_u64_Fixed_write is u64's Fixed::write: its bytes appended, the low byte first. */
void
chunks_index_u64_Fixed_write(const uint64 *self, StdVec *out)
{
	uint8		le[8];

	std_u64_to_le_bytes(*self, le);
	std_vec_extend_from_slice_Checked(out, le, sizeof(le), 1, 1);
}

/* chunks_index_u64_Fixed_read is u64's Fixed::read: the number its first 8 bytes hold, the low byte first. */
uint64
chunks_index_u64_Fixed_read(const uint8 *bytes, size_t bytes_len)
{
	std_range_to_check(8, bytes_len, RUST_FILE, 749, 33);
	if (8 != sizeof(uint64))
		STD_RESULT_EXPECT_FAILED(749, 50, "called `Result::unwrap()` on an `Err` value", STD_DANGLING,
								 std_TryFromSliceError_Debug_fmt);
	return std_u64_from_le_bytes(bytes);
}

/* chunks_index_Backing_drop is Backing's drop glue: its file, or its bytes. */
void
chunks_index_Backing_drop(void *self_)
{
	ChunksIndexBacking *self = self_;

	switch (self->tag)
	{
		case CHUNKS_INDEX_BACKING_FILE:
			host_File_drop(&self->u.file);
			break;
		case CHUNKS_INDEX_BACKING_MEMORY:
			std_vec_free_Checked(&self->u.memory, 1, 1);
			break;
	}
}

/* chunks_index_Backing_read reads bytes at an offset of a run, from its file or from the memory a run is kept in. */
static bool
chunks_index_Backing_read(const ChunksIndexBacking *self, HostIoReadDoors io, const char *path, size_t path_len,
						  uint64 offset, size_t len, StdVec *result, ChunksErrorError *error)
{
	switch (self->tag)
	{
		case CHUNKS_INDEX_BACKING_FILE:
			return chunks_file_read_at(io, &self->u.file, offset, len, result, error);
		case CHUNKS_INDEX_BACKING_MEMORY:
			{
				size_t		start = (size_t) offset;
				size_t		end = STD_ADD(usize, (size_t) offset, len, 781, 55);
				StdStr		shown = {path, path_len};
				StdString	message;
				StdFmtPiece pieces[] = {STD_FMT_LIT("address index run "), STD_FMT_ARG(0), STD_FMT_LIT(" is corrupt")};
				StdFmtArg	args[] = {{&shown, std_path_Display_fmt}};
				StdFmtArguments arguments = {pieces, 3, args};

				if (start <= end && end <= self->u.memory.len)
				{
					std_slice_to_vec_Checked(result, STD_PTR_AT(self->u.memory.ptr, start, 1),
											 std_wrapping_sub_usize(end, start), 1, 1);
					return true;
				}
				std_fmt_format_Checked(&message, &arguments);
				*error = chunks_error_Error_corrupt(&message);
				return false;
			}
	}
	pg_unreachable();
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_OptionPathBuf_t is Option<PathBuf>: a path, or none. */
typedef struct chunks_OptionPathBuf_t
{
	bool		has;
	StdPathBuf	value;
} chunks_OptionPathBuf_t;

/* chunks_index_OptionPathBuf_drop is Option<PathBuf>'s drop glue: its path's, when it has one. */
static void
chunks_index_OptionPathBuf_drop(void *value)
{
	chunks_OptionPathBuf_t *self = value;

	if (self->has)
		std_vec_free_Checked(&self->value.bytes, 1, 1);
}

/* taken at Option<PathBuf> and IoMutex<Option<PathBuf>>, which index.c alone uses. */
#define HOST_LOCK_SCOPE static
#define HOST_LOCK_DECLARE
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS OptionPathBuf
#define HOST_LOCK_TAKEN_T chunks_OptionPathBuf_t
#define HOST_LOCK_TAKEN_DEFAULT(place) ((place)->has = false)
#define HOST_LOCK_TAKEN_DROP(place) chunks_index_OptionPathBuf_drop(place)
#define HOST_LOCK_TAKEN_TYPE CHUNKS_INDEX_SHORT_WRITE_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS OptionPathBuf
#define HOST_LOCK_T chunks_OptionPathBuf_t
#define HOST_LOCK_DEFAULT(place) ((place)->has = false)
#include "doltgres/host/lock_template.h"

/* chunks_index_IoMutexOptionPathBuf_drop is IoMutex<Option<PathBuf>>' drop glue: its value's. */
static void
chunks_index_IoMutexOptionPathBuf_drop(void *value)
{
	HostLockIoMutexOptionPathBuf *self = value;

	chunks_index_OptionPathBuf_drop(&self->value);
}

/* CHUNKS_INDEX_SHORT_WRITE_TYPE is IoMutex<Option<PathBuf>>' size, Default, and drop. */
static const StdDefaultType CHUNKS_INDEX_SHORT_WRITE_TYPE = {sizeof(HostLockIoMutexOptionPathBuf),
	_Alignof(HostLockIoMutexOptionPathBuf), host_lock_IoMutexOptionPathBuf_Default_default,
chunks_index_IoMutexOptionPathBuf_drop};

/*
 * CHUNKS_INDEX_SHORT_WRITE is SHORT_WRITE: the directory whose next run
 * write by the backend leaves its last entry out, for tests of what reads a
 * run back; its BackendLocal header and the process's value, which follows
 * it.
 */
static struct
{
	HostLocalBackendLocal local;
	HostLockIoMutexOptionPathBuf process;
}			CHUNKS_INDEX_SHORT_WRITE = {HOST_LOCAL_BACKEND_LOCAL_NEW};

/*
 * CHUNKS_INDEX_ENTRIES_READ is ENTRIES_READ: the runs whose entries the
 * backend read whole, for tests of what reads them; its BackendLocal header
 * and the process's value, which follows it.
 */
static struct
{
	HostLocalBackendLocal local;
	StdAtomicU64 process;
}			CHUNKS_INDEX_ENTRIES_READ = {HOST_LOCAL_BACKEND_LOCAL_NEW};

#endif							/* DOLTGRES_TEST_CHUNKS */

/* The sort of (Address, Locator) entries that merged runs: core's shared sort parts at the tuple, which its stable sort calls. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS TupleAddressLocator
#define STD_SORT_T chunks_TupleAddressLocator_t
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"

/* The stable sort_by_key of (Address, Locator) entries by their address, which merged runs. */
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS TupleAddressLocator
#define STD_STABLE_SORT_T chunks_TupleAddressLocator_t
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_WITHOUT_SORT
#define STD_STABLE_SORT_WITH_BY_KEY
#define STD_STABLE_SORT_KEY_KS Address
#define STD_STABLE_SORT_KEY_K ChunksAddressAddress
#define STD_STABLE_SORT_KEY_LT(a, b) chunks_address_Address_PartialOrd_lt((a), (b))
#include "doltgres/std/stable_sort_template.h"

/* Run<Locator>, AddressIndex<Locator>, and the lookups at Locator and Error, which index.h declares. */
#define CHUNKS_INDEX_SCOPE extern
#define CHUNKS_INDEX_DEFINE
#define CHUNKS_INDEX_ES Error
#define CHUNKS_INDEX_E ChunksErrorError
#define CHUNKS_INDEX_VS Locator
#define CHUNKS_INDEX_V ChunksTrailerLocator
#define CHUNKS_INDEX_ENTRY chunks_TupleAddressLocator_t
#define CHUNKS_INDEX_V_LEN CHUNKS_TRAILER_LOCATOR_LEN
#define CHUNKS_INDEX_V_EQ(a, b) chunks_trailer_Locator_PartialEq_eq((a), (b))
#define CHUNKS_INDEX_RUNS_OPS CHUNKS_INDEX_ADDRESS_INDEX_LOCATOR_RUNS_OPS
#define CHUNKS_INDEX_SORT_BY_KEY std_slice_sort_by_key_TupleAddressLocatorAddress
#include "doltgres/chunks/index_template.h"

/*
 * chunks_index_written_run writes an address-index run of entries and checks
 * that the run read back holds as many, removing the file and failing when a
 * write lost some.
 */
bool
chunks_index_written_run(HostIoIoDoors io, const char *path, size_t path_len,
						 const chunks_TupleAddressLocator_t *entries, size_t entries_len,
						 ChunksIndexRunLocator *result, ChunksErrorError *error)
{
	uint64		held;
	StdStr		shown = {path, path_len};
	StdString	message;
	StdFmtPiece pieces[] = {STD_FMT_LIT("address-index run "), STD_FMT_ARG(0), STD_FMT_LIT(" holds "), STD_FMT_ARG(1),
	STD_FMT_LIT(" entries where "), STD_FMT_ARG(2), STD_FMT_LIT(" were written")};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&held, std_u64_Display_fmt},
	{&entries_len, std_usize_Display_fmt}};
	StdFmtArguments arguments = {pieces, 7, args};

	if (!chunks_index_RunLocator_write(io, path, path_len, entries, entries_len, result, error))
		return false;
	if (chunks_index_RunLocator_len(result) == (uint64) entries_len)
		return true;
	held = chunks_index_RunLocator_len(result);
	chunks_index_RunLocator_drop(result);
	if (!chunks_file_remove_file(io, path, path_len, error))
		return false;
	std_fmt_format_Checked(&message, &arguments);
	*error = chunks_error_Error_corrupt(&message);
	return false;
}

#ifdef DOLTGRES_TEST_CHUNKS

/* chunks_index_tests_entries_closure_1 is entries' |a| a.0: an entry's address, the key its sort orders by. */
static ChunksAddressAddress
chunks_index_tests_entries_closure_1(void *captures, const void *a)
{
	const chunks_TupleAddressLocator_t *entry = a;

	return entry->f0;
}

/* The state of entries across its pad: its arguments, and the entries it makes until they are its result. */
typedef struct chunks_index_tests_entries_frame_0_t
{
	uint32		start;
	uint32		end;
	uint64		pack;
	StdVec		out;
	bool		out_live;
} chunks_index_tests_entries_frame_0_t;

/* chunks_index_tests_entries_body_0 is entries' body. */
static void
chunks_index_tests_entries_body_0(void *frame_)
{
	chunks_index_tests_entries_frame_0_t *frame = frame_;
	size_t		n = frame->start < frame->end ? (size_t) std_wrapping_sub_u32(frame->end, frame->start) : 0;
	StdSizeHint hint = {n, true, n};

	std_vec_with_capacity_trusted(&frame->out, hint, sizeof(chunks_TupleAddressLocator_t),
								  _Alignof(chunks_TupleAddressLocator_t));
	frame->out_live = true;
	std_vec_extend_trusted_reserve(&frame->out, hint, sizeof(chunks_TupleAddressLocator_t),
								   _Alignof(chunks_TupleAddressLocator_t));
	for (uint32 i = frame->start, i_end = frame->end; i < i_end; i = std_forward_unchecked_u32_Checked(i, 1))
	{
		uint8		le[4];
		chunks_TupleAddressLocator_t entry;

		std_u32_to_le_bytes(i, le);
		entry.f0 = chunks_address_Address_of(le, sizeof(le));
		entry.f1 = chunks_trailer_Locator_at(frame->pack, STD_MUL(u64, (uint64) i, 10, 1145, 77),
											 STD_ADD(u32, i, 1, 1145, 92));
		memcpy(STD_PTR_AT(frame->out.ptr, frame->out.len, sizeof(entry)), &entry, sizeof(entry));
		frame->out.len = std_wrapping_add_usize(frame->out.len, 1);
	}
	std_slice_sort_by_key_TupleAddressLocatorAddress(frame->out.ptr, frame->out.len,
													 chunks_index_tests_entries_closure_1, NULL, NULL);
	frame->out_live = false;
}

/* chunks_index_tests_entries_pad_0 is entries' pad: the entries, while it makes them. */
static void
chunks_index_tests_entries_pad_0(void *frame_)
{
	chunks_index_tests_entries_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, sizeof(chunks_TupleAddressLocator_t),
							 _Alignof(chunks_TupleAddressLocator_t));
}

/*
 * chunks_index_tests_entries returns the entries of the numbers of a range,
 * each the address of its bytes and a locator in the pack, sorted by
 * address.
 */
static StdVec
chunks_index_tests_entries(uint32 start, uint32 end, uint64 pack)
{
	chunks_index_tests_entries_frame_0_t frame = {.start = start, .end = end, .pack = pack};

	std_landing(&frame, chunks_index_tests_entries_body_0, chunks_index_tests_entries_pad_0);
	return frame.out;
}

/* CHUNKS_INDEX_TESTS_ENTRIES is the size and alignment of an (Address, Locator) entry, a vector's element. */
#define CHUNKS_INDEX_TESTS_ENTRIES sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t)

/* CHUNKS_INDEX_TESTS_UNWRAP is Result::unwrap's panic message. */
#define CHUNKS_INDEX_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* CHUNKS_INDEX_TESTS_UNWRAP_ERR is Result::unwrap_err's panic message. */
#define CHUNKS_INDEX_TESTS_UNWRAP_ERR "called `Result::unwrap_err()` on an `Ok` value"

/* test_chunks_index_tests_..._closure_0 is |_| true: keep every value. */
static bool
test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_closure_0(void *captures,
																								  const ChunksTrailerLocator *value)
{
	return true;
}

/*
 * The state of the test across its pad: its capability, directory, path,
 * entries, the run, the file's bytes, the swapped entries, the error it
 * refuses with, and an unwrapped error.
 */
typedef struct test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		written;
	bool		written_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		swapped;
	bool		swapped_live;
	ChunksIndexRunLocator run;
	bool		run_live;
	ChunksErrorError refused;
	bool		refused_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_body_0(void *frame_)
{
	test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksIndexRunLocator written_run;
	size_t		first_start;
	size_t		first_end;
	size_t		second_start;
	size_t		second_end;
	StdBytes	parts[2];
	uint8		sum[8];
	bool		has;
	ChunksTrailerLocator found;
	bool		wrote;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1248, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "r.idx", 5, &frame->path);
	frame->path_live = true;
	frame->written = chunks_index_tests_entries(0, 3, 1);
	frame->written_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->written.ptr,
									   frame->written.len, &written_run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1251, 46, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	chunks_index_RunLocator_drop(&written_run);
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1252, 46, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	first_start = 8;
	first_end = STD_ADD(usize, 8, chunks_index_entry_Locator(), 1253, 35);
	second_start = STD_ADD(usize, 8, chunks_index_entry_Locator(), 1253, 59);
	second_end = STD_ADD(usize, 8, STD_MUL(usize, 2, chunks_index_entry_Locator(), 1253, 87), 1253, 83);
	std_range_check(second_start, second_end, frame->bytes.len, RUST_FILE, 1254, 30);
	parts[0].ptr = STD_PTR_AT(frame->bytes.ptr, second_start, 1);
	parts[0].len = std_wrapping_sub_usize(second_end, second_start);
	std_range_check(first_start, first_end, frame->bytes.len, RUST_FILE, 1254, 54);
	parts[1].ptr = STD_PTR_AT(frame->bytes.ptr, first_start, 1);
	parts[1].len = std_wrapping_sub_usize(first_end, first_start);
	std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &frame->swapped);
	frame->swapped_live = true;
	std_range_check(first_start, second_end, frame->bytes.len, RUST_FILE, 1255, 14);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, first_start, 1),
									  std_wrapping_sub_usize(second_end, first_start), frame->swapped.ptr,
									  frame->swapped.len, 1, 1, RUST_FILE, 1255, 40);
	std_range_check(8, 8 + CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, frame->bytes.len, RUST_FILE, 1256, 52);
	std_u64_to_le_bytes(XXH3_64bits(STD_PTR_AT(frame->bytes.ptr, 8, 1), CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM), sum);
	std_range_check(8 + CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, 8 + CHUNKS_INDEX_BLOCK, frame->bytes.len, RUST_FILE,
					1257, 14);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, 8 + CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, 1),
									  CHUNKS_INDEX_SUM, sum, sizeof(sum), 1, 1, RUST_FILE, 1257, 43);
	frame->bytes_live = false;
	wrote = std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, frame->bytes.ptr, frame->bytes.len, &io_error);
	std_vec_free_Checked(&frame->bytes, 1, 1);
	if (!wrote)
		STD_RESULT_EXPECT_FAILED(1258, 38, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	if (!chunks_index_RunLocator_open(HOST_IO_IO_READ_DOORS(&frame->test_io.io), frame->path.bytes.ptr,
									  frame->path.bytes.len, &frame->run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1259, 51, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->run_live = true;
	std_index_check(0, frame->written.len, RUST_FILE, 1260, 43);
	if (chunks_index_RunLocator_get(&frame->run, HOST_IO_IO_READ_DOORS(&frame->test_io.io),
									&STD_VEC_AT(&frame->written, chunks_TupleAddressLocator_t, 0).f0,
									test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_closure_0,
									NULL, NULL, &has, &found, &frame->refused))
	{
		StdOptionRef ok = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};

		STD_RESULT_EXPECT_FAILED(1260, 60, CHUNKS_INDEX_TESTS_UNWRAP_ERR, &ok, std_option_Debug_fmt);
	}
	frame->refused_live = true;
	if (!(frame->refused.tag == CHUNKS_ERROR_ERROR_DAMAGED && frame->refused.u.damaged.offset == 8 &&
		  std_str_contains_str_Checked(frame->refused.u.damaged.message.ptr, frame->refused.u.damaged.message.len,
									   "out of order", 12)))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->refused, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1261, 9, &arguments);
	}
	frame->refused_live = false;
	chunks_error_Error_drop(&frame->refused);
	frame->run_live = false;
	chunks_index_RunLocator_drop(&frame->run);
	frame->swapped_live = false;
	std_vec_free_Checked(&frame->swapped, 1, 1);
	frame->written_live = false;
	std_vec_free_Checked(&frame->written, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_pad_0(void *frame_)
{
	test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->refused_live)
		chunks_error_Error_drop(&frame->refused);
	if (frame->run_live)
		chunks_index_RunLocator_drop(&frame->run);
	if (frame->swapped_live)
		std_vec_free_Checked(&frame->swapped, 1, 1);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->written_live)
		std_vec_free_Checked(&frame->written, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode:
 * a block whose entries are out of order is refused as damage where it is
 * decoded, though its checksum matches, so no search runs over entries that
 * aren't sorted.
 */
void
test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode(void)
{
	test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_body_0,
				test_chunks_index_tests_a_block_whose_entries_are_out_of_order_is_refused_at_its_decode_pad_0);
}

/*
 * The state of the test across its pad: its capability, directory, path,
 * entries, the file's bytes, the swapped fences, the error it refuses with,
 * and an unwrapped error.
 */
typedef struct test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		written;
	bool		written_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		swapped;
	bool		swapped_live;
	ChunksErrorError refused;
	bool		refused_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_body_0(void *frame_)
{
	test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksIndexRunLocator run;
	size_t		fences_at;
	size_t		start;
	size_t		end;
	StdBytes	parts[2];
	uint8		sum[8];
	bool		wrote;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1273, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "r.idx", 5, &frame->path);
	frame->path_live = true;
	frame->written = chunks_index_tests_entries(0, STD_ADD(u32, (uint32) chunks_index_per_block_Locator(), 1, 1275, 48),
												1);
	frame->written_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->written.ptr,
									   frame->written.len, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1275, 87, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	chunks_index_RunLocator_drop(&run);
	frame->written_live = false;
	std_vec_free_Checked(&frame->written, CHUNKS_INDEX_TESTS_ENTRIES);
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1276, 46, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	fences_at = STD_ADD(usize, 8, STD_MUL(usize, 2, CHUNKS_INDEX_BLOCK, 1277, 29), 1277, 25);
	start = STD_ADD(usize, fences_at, 32, 1278, 31);
	end = STD_ADD(usize, fences_at, 64, 1278, 47);
	std_range_check(start, end, frame->bytes.len, RUST_FILE, 1278, 30);
	parts[0].ptr = STD_PTR_AT(frame->bytes.ptr, start, 1);
	parts[0].len = std_wrapping_sub_usize(end, start);
	end = STD_ADD(usize, fences_at, 32, 1278, 82);
	std_range_check(fences_at, end, frame->bytes.len, RUST_FILE, 1278, 70);
	parts[1].ptr = STD_PTR_AT(frame->bytes.ptr, fences_at, 1);
	parts[1].len = std_wrapping_sub_usize(end, fences_at);
	std_slice_concat(parts, 2, sizeof(StdBytes), 1, 1, &frame->swapped);
	frame->swapped_live = true;
	end = STD_ADD(usize, fences_at, 64, 1279, 26);
	std_range_check(fences_at, end, frame->bytes.len, RUST_FILE, 1279, 14);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, fences_at, 1), std_wrapping_sub_usize(end, fences_at),
									  frame->swapped.ptr, frame->swapped.len, 1, 1, RUST_FILE, 1279, 42);
	end = STD_ADD(usize, fences_at, 72, 1280, 64);
	std_range_check(fences_at, end, frame->bytes.len, RUST_FILE, 1280, 52);
	std_u64_to_le_bytes(XXH3_64bits(STD_PTR_AT(frame->bytes.ptr, fences_at, 1), std_wrapping_sub_usize(end, fences_at)),
						sum);
	start = STD_ADD(usize, fences_at, 72, 1281, 15);
	end = STD_ADD(usize, fences_at, 80, 1281, 31);
	std_range_check(start, end, frame->bytes.len, RUST_FILE, 1281, 14);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, start, 1), std_wrapping_sub_usize(end, start), sum,
									  sizeof(sum), 1, 1, RUST_FILE, 1281, 47);
	frame->bytes_live = false;
	wrote = std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, frame->bytes.ptr, frame->bytes.len, &io_error);
	std_vec_free_Checked(&frame->bytes, 1, 1);
	if (!wrote)
		STD_RESULT_EXPECT_FAILED(1282, 38, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	if (chunks_index_RunLocator_open(HOST_IO_IO_READ_DOORS(&frame->test_io.io), frame->path.bytes.ptr,
									 frame->path.bytes.len, &run, &frame->refused))
	{
		chunks_index_RunLocator_drop(&run);
		STD_OPTION_UNWRAP_FAILED(1283, 61);
	}
	frame->refused_live = true;
	if (!(frame->refused.tag == CHUNKS_ERROR_ERROR_CORRUPT &&
		  std_str_contains_str_Checked(frame->refused.u.corrupt.ptr, frame->refused.u.corrupt.len, "is corrupt", 10)))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->refused, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1284, 9, &arguments);
	}
	frame->refused_live = false;
	chunks_error_Error_drop(&frame->refused);
	frame->swapped_live = false;
	std_vec_free_Checked(&frame->swapped, 1, 1);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_pad_0(void *frame_)
{
	test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->refused_live)
		chunks_error_Error_drop(&frame->refused);
	if (frame->swapped_live)
		std_vec_free_Checked(&frame->swapped, 1, 1);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->written_live)
		std_vec_free_Checked(&frame->written, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read:
 * a run whose fences are out of order is refused as corrupt where it is
 * opened, though its footer's checksum matches, so no lookup searches fences
 * that aren't sorted.
 */
void
test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read(void)
{
	test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_body_0,
				test_chunks_index_tests_a_run_whose_fences_are_out_of_order_is_refused_at_its_read_pad_0);
}

/*
 * The state of the test across its pad: its capability, directory, path,
 * entries, the file's bytes, the index, the error it refuses with, and an
 * unwrapped error.
 */
typedef struct test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		written;
	bool		written_live;
	StdVec		bytes;
	bool		bytes_live;
	ChunksIndexAddressIndexLocator index;
	bool		index_live;
	ChunksErrorError refused;
	bool		refused_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_body_0(void *frame_)
{
	test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_frame_0_t *frame =
		frame_;
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksIndexRunLocator run;
	uint8		sum[8];
	bool		wrote;
	bool		has;
	ChunksTrailerLocator found;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1293, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "r.idx", 5, &frame->path);
	frame->path_live = true;
	frame->written = chunks_index_tests_entries(0, 3, 1);
	frame->written_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->written.ptr,
									   frame->written.len, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1296, 46, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	chunks_index_RunLocator_drop(&run);
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->bytes, &io_error))
		STD_RESULT_EXPECT_FAILED(1297, 46, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->bytes_live = true;
	std_range_check(8, 40, frame->bytes.len, RUST_FILE, 1298, 14);
	{
		uint8	   *filled = STD_PTR_AT(frame->bytes.ptr, 8, 1);

		for (size_t i = 0, i_len = 32; i < i_len; i++)
			filled[i] = 0;
	}
	std_range_check(8, 8 + CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, frame->bytes.len, RUST_FILE, 1299, 52);
	std_u64_to_le_bytes(XXH3_64bits(STD_PTR_AT(frame->bytes.ptr, 8, 1), CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM), sum);
	std_range_check(8 + CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, 8 + CHUNKS_INDEX_BLOCK, frame->bytes.len, RUST_FILE,
					1300, 14);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->bytes.ptr, 8 + CHUNKS_INDEX_BLOCK - CHUNKS_INDEX_SUM, 1),
									  CHUNKS_INDEX_SUM, sum, sizeof(sum), 1, 1, RUST_FILE, 1300, 43);
	frame->bytes_live = false;
	wrote = std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, frame->bytes.ptr, frame->bytes.len, &io_error);
	std_vec_free_Checked(&frame->bytes, 1, 1);
	if (!wrote)
		STD_RESULT_EXPECT_FAILED(1301, 38, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	if (!chunks_index_RunLocator_open(HOST_IO_IO_READ_DOORS(&frame->test_io.io), frame->path.bytes.ptr,
									  frame->path.bytes.len, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1302, 76, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		ChunksIndexRunLocator *boxed = std_box_new_uninit_layout_Checked(sizeof(ChunksIndexRunLocator),
																		 _Alignof(ChunksIndexRunLocator));
		StdVec		runs;

		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksIndexRunLocator), 1302, 39);
		STD_CHECK_NULL_DEREF(boxed, 1302, 39);
		boxed[0] = run;
		std_box_slice_into_vec_Checked(&runs, boxed, 1, sizeof(ChunksIndexRunLocator));
		frame->index = chunks_index_AddressIndexLocator_new(&runs);
		frame->index_live = true;
	}
	std_index_check(1, frame->written.len, RUST_FILE, 1303, 45);
	if (chunks_index_AddressIndexLocator_get(&frame->index, HOST_IO_IO_READ_DOORS(&frame->test_io.io),
											 &STD_VEC_AT(&frame->written, chunks_TupleAddressLocator_t, 1).f0, &has,
											 &found, &frame->refused))
	{
		StdOptionRef ok = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};

		STD_RESULT_EXPECT_FAILED(1303, 52, CHUNKS_INDEX_TESTS_UNWRAP_ERR, &ok, std_option_Debug_fmt);
	}
	frame->refused_live = true;
	if (!(frame->refused.tag == CHUNKS_ERROR_ERROR_DAMAGED && frame->refused.u.damaged.offset == 8 &&
		  std_str_contains_str_Checked(frame->refused.u.damaged.message.ptr, frame->refused.u.damaged.message.len,
									   "outside its fences", 18)))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->refused, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 1304, 9, &arguments);
	}
	frame->refused_live = false;
	chunks_error_Error_drop(&frame->refused);
	frame->index_live = false;
	chunks_index_AddressIndexLocator_drop(&frame->index);
	frame->written_live = false;
	std_vec_free_Checked(&frame->written, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_pad_0(void *frame_)
{
	test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_frame_0_t *frame =
		frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->refused_live)
		chunks_error_Error_drop(&frame->refused);
	if (frame->index_live)
		chunks_index_AddressIndexLocator_drop(&frame->index);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->written_live)
		std_vec_free_Checked(&frame->written, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode:
 * a block whose first entry isn't the fence its run keeps for it is refused
 * as damage where it is decoded, though its entries are in order and its
 * checksum matches, so what the fences say of a block is true of it.
 */
void
test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode(void)
{
	test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_body_0,
				test_chunks_index_tests_a_block_whose_first_entry_is_not_its_fence_is_refused_at_its_decode_pad_0);
}

/* test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_closure_0 is |_| true: keep every value. */
static bool
test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_closure_0(void *captures,
																				 const ChunksTrailerLocator *value)
{
	return true;
}

/*
 * The state of the test across its pad: its capability, directory,
 * entries, path, the run kept in memory and the one written, the two runs'
 * entries it compares, and an unwrapped error.
 */
typedef struct test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdVec		entries;
	bool		entries_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksIndexRunLocator kept;
	bool		kept_live;
	ChunksIndexRunLocator written;
	bool		written_live;
	StdVec		left;
	bool		left_live;
	StdVec		right;
	bool		right_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_body_0(void *frame_)
{
	test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	HostIoReadDoors read;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	StdSliceStepBy step;
	const void *item;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1191, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	frame->entries = chunks_index_tests_entries(0, 500, 3);
	frame->entries_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "kept.idx", 8, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_in_memory(&frame->test_io.io, frame->path.bytes.ptr, frame->path.bytes.len,
										   frame->entries.ptr, frame->entries.len, &frame->kept, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1194, 56, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->kept_live = true;
	STD_ASSERT(1195, 9, !std_path_Path_exists(frame->path.bytes.ptr, frame->path.bytes.len), "!path.exists()");
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->entries.ptr,
									   frame->entries.len, &frame->written, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1196, 55, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->written_live = true;
	if (!chunks_index_RunLocator_entries(&frame->kept, read, &frame->left, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1197, 37, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->left_live = true;
	if (!chunks_index_RunLocator_entries(&frame->written, read, &frame->right, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1197, 67, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->right_live = true;
	STD_ASSERT_EQ(1197, 9, chunks_record_tests_VecTupleAddressLocator_PartialEq_eq(&frame->left, &frame->right),
				  &frame->left, chunks_record_tests_VecTupleAddressLocator_Debug_fmt, &frame->right,
				  chunks_record_tests_VecTupleAddressLocator_Debug_fmt);
	frame->right_live = false;
	std_vec_free_Checked(&frame->right, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->left_live = false;
	std_vec_free_Checked(&frame->left, CHUNKS_INDEX_TESTS_ENTRIES);
	step = std_slice_Iter_step_by(frame->entries.ptr, frame->entries.len, 37);
	while (std_SliceStepBy_Iterator_next(&step, sizeof(chunks_TupleAddressLocator_t), &item))
	{
		const chunks_TupleAddressLocator_t *entry = item;
		bool		has;
		ChunksTrailerLocator found;
		StdOptionRef left;
		StdOptionRef right = {true, STD_ITEM(&entry->f1, chunks_trailer_Locator_Debug_fmt)};

		if (!chunks_index_RunLocator_get(&frame->kept, read, &entry->f0,
										 test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_closure_0,
										 NULL, NULL, &has, &found, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1199, 56, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		left = (StdOptionRef)
		{
			has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)
		};
		STD_ASSERT_EQ(1199, 13, has && chunks_trailer_Locator_PartialEq_eq(&found, &entry->f1), &left,
					  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	frame->written_live = false;
	chunks_index_RunLocator_drop(&frame->written);
	frame->kept_live = false;
	chunks_index_RunLocator_drop(&frame->kept);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->entries_live = false;
	std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_pad_0(void *frame_)
{
	test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->right_live)
		std_vec_free_Checked(&frame->right, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->left_live)
		std_vec_free_Checked(&frame->left, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->written_live)
		chunks_index_RunLocator_drop(&frame->written);
	if (frame->kept_live)
		chunks_index_RunLocator_drop(&frame->kept);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would: a
 * run kept in memory writes no file and answers every read as the run
 * written from the same entries does.
 */
void
test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would(void)
{
	test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_body_0,
				test_chunks_index_tests_a_run_kept_in_memory_answers_as_its_file_would_pad_0);
}

/* The copies of an address, a pack each, that the copies tests place: Map<Range<u64>, closure> as its own iterator. */
typedef struct test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter
{
	uint64		start;
	uint64		end;
	const ChunksAddressAddress *repeated;
} test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter;

/* test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter_next is the copies' next: the range's next pack, as the copy (repeated, at(pack, 0, 1)). */
static bool
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter_next(void *self_, void *result)
{
	test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter *self = self_;
	chunks_TupleAddressLocator_t *copy = result;
	uint64		pack;

	if (!(self->start < self->end))
		return false;
	pack = self->start;
	self->start = std_forward_unchecked_u64_Checked(pack, 1);
	copy->f0 = *self->repeated;
	copy->f1 = chunks_trailer_Locator_at(pack, 0, 1);
	return true;
}

/* test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter_size_hint is the copies' size_hint, the range's: the exact number left. */
static StdSizeHint
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter_size_hint(const void *self_)
{
	const test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter *self = self_;
	size_t		n = self->start < self->end ? (size_t) std_wrapping_sub_u64(self->end, self->start) : 0;

	return (StdSizeHint)
	{
		n, true, n
	};
}

/* TEST_CHUNKS_INDEX_TESTS_A_RUN_FINDS_EACH_COPY_COPIES_ITER_OPS is the copies' Iterator table. */
static const StdIteratorOps TEST_CHUNKS_INDEX_TESTS_A_RUN_FINDS_EACH_COPY_COPIES_ITER_OPS = {test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter_next,
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter_size_hint, NULL};

/* The test's closure_0, its capture the repeated address: whether an entry's address is below it. */
static bool
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_0(void *captures,
																								const void *e)
{
	const chunks_TupleAddressLocator_t *entry = e;

	return chunks_address_Address_PartialOrd_lt(&entry->f0, captures);
}

/* The test's closure_2, its capture the pack: whether a locator is in it. */
static bool
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_2(void *captures,
																								const ChunksTrailerLocator *l)
{
	return l->pack == *(const uint64 *) captures;
}

/* The test's closure_4, |_| true: keep every value. */
static bool
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_4(void *captures,
																								const ChunksTrailerLocator *l)
{
	return true;
}

/* The test's closure_6: whether a locator is in pack 1. */
static bool
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_6(void *captures,
																								const ChunksTrailerLocator *l)
{
	return l->pack == 1;
}

/* The test's closure_8, |_| true: keep every value. */
static bool
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_8(void *captures,
																								const ChunksTrailerLocator *l)
{
	return true;
}

/*
 * The state of the test across its pad: its capability, directory, entries,
 * the path it writes, the run, and an unwrapped error.
 */
typedef struct test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdVec		entries;
	bool		entries_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksIndexRunLocator run;
	bool		run_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_body_0(void *frame_)
{
	test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_frame_0_t *frame = frame_;
	static const uint8 repeated_bytes[] = {'r', 'e', 'p', 'e', 'a', 't', 'e', 'd'};
	HostIoIoDoors io;
	HostIoReadDoors read;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksAddressAddress repeated;
	size_t		at;
	test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_copies_iter copies;
	StdBound	start = {STD_BOUND_INCLUDED};
	StdBound	end = {STD_BOUND_EXCLUDED};
	StdIterator replace_with;
	StdVecSplice splice;
	uint64		remaining;
	uint64		next;
	bool		has;
	ChunksTrailerLocator found;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1207, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	repeated = chunks_address_Address_of(repeated_bytes, sizeof(repeated_bytes));
	frame->entries = chunks_index_tests_entries(0, 300, 1);
	frame->entries_live = true;
	at = std_slice_partition_point(frame->entries.ptr, frame->entries.len, sizeof(chunks_TupleAddressLocator_t),
								   test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_0,
								   &repeated, NULL);
	copies.start = 2;
	copies.end = STD_ADD(u64, 2, STD_MUL(u64, 3, (uint64) chunks_index_per_block_Locator(), 1211, 30), 1211, 26);
	copies.repeated = &repeated;
	start.index = at;
	end.index = at;
	replace_with.ops = &TEST_CHUNKS_INDEX_TESTS_A_RUN_FINDS_EACH_COPY_COPIES_ITER_OPS;
	replace_with.self = &copies;
	splice = std_vec_splice(&frame->entries, start, end, CHUNKS_INDEX_TESTS_ENTRIES, NULL, replace_with);
	std_VecSplice_drop(&splice);
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "r.idx", 5, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->entries.ptr,
									   frame->entries.len, &frame->run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1213, 71, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->run_live = true;
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	remaining = std_iter_step_by_range_u64(2, STD_ADD(u64, 2, STD_MUL(u64, 3, (uint64) chunks_index_per_block_Locator(),
																	  1214, 29),
													  1214, 25),
										   7);
	next = 2;
	while (remaining > 0)
	{
		uint64		pack = next;
		StdOptionRef left;
		StdOptionRef right = {true, STD_ITEM(&pack, std_u64_Debug_fmt)};

		next = std_wrapping_add_u64(pack, 7);
		remaining = std_wrapping_sub_u64(remaining, 1);
		if (!chunks_index_RunLocator_get(&frame->run, read, &repeated,
										 test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_2,
										 &pack, NULL, &has, &found, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1215, 67, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		left = (StdOptionRef)
		{
			has, STD_ITEM(&found.pack, std_u64_Debug_fmt)
		};
		STD_ASSERT_EQ(1215, 13, has && found.pack == pack, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	if (!chunks_index_RunLocator_get(&frame->run, read, &repeated,
									 test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_4,
									 NULL, NULL, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1217, 53, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		uint64		two = 2;
		StdOptionRef left = {has, STD_ITEM(&found.pack, std_u64_Debug_fmt)};
		StdOptionRef right = {true, STD_ITEM(&two, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(1217, 9, has && found.pack == two, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	if (!chunks_index_RunLocator_get(&frame->run, read, &repeated,
									 test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_6,
									 NULL, NULL, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1218, 60, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		StdOptionRef left = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef right = {false};

		STD_ASSERT_EQ(1218, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	for (size_t i = 0, i_len = frame->entries.len; i < i_len; i++)
	{
		const chunks_TupleAddressLocator_t *entry = STD_PTR_AT(frame->entries.ptr, i,
															   sizeof(chunks_TupleAddressLocator_t));
		StdOptionRef left;
		StdOptionRef right = {true, STD_ITEM(&entry->f1, chunks_trailer_Locator_Debug_fmt)};

		if (!chunks_address_Address_PartialEq_ne(&entry->f0, &repeated))
			continue;
		if (!chunks_index_RunLocator_get(&frame->run, read, &entry->f0,
										 test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_closure_8,
										 NULL, NULL, &has, &found, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1220, 55, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		left = (StdOptionRef)
		{
			has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)
		};
		STD_ASSERT_EQ(1220, 13, has && chunks_trailer_Locator_PartialEq_eq(&found, &entry->f1), &left,
					  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	frame->run_live = false;
	chunks_index_RunLocator_drop(&frame->run);
	frame->entries_live = false;
	std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_pad_0(void *frame_)
{
	test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->run_live)
		chunks_index_RunLocator_drop(&frame->run);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks:
 * a run whose copies of one address fill three blocks finds each copy a
 * keep picks, the first copy for a keep of any, none for a keep of none,
 * and every other entry.
 */
void
test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks(void)
{
	test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_body_0,
				test_chunks_index_tests_a_run_finds_each_copy_of_an_address_whose_entries_span_blocks_pad_0);
}

/* chunks_SliceAddress_t is a &[Address]: its addresses and their count. */
typedef struct chunks_SliceAddress_t
{
	const ChunksAddressAddress *ptr;
	size_t		len;
} chunks_SliceAddress_t;

/* chunks_index_tests_SliceAddress_Debug_fmt is [Address]'s Debug, which a &[Address]'s and an array's call: a list of the addresses. */
static bool
chunks_index_tests_SliceAddress_Debug_fmt(const ChunksAddressAddress *items, size_t len, StdFormatter *f)
{
	StdDebugList builder = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = len; i < i_len; i++)
		std_DebugList_entry(&builder, &items[i], chunks_address_Address_Debug_fmt);
	return std_DebugList_finish(&builder);
}

/* chunks_index_tests_RefSliceAddress_Debug_fmt is &[Address]'s Debug, its slice's. */
static bool
chunks_index_tests_RefSliceAddress_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_SliceAddress_t *self = value;

	return chunks_index_tests_SliceAddress_Debug_fmt(self->ptr, self->len, f);
}

/* chunks_index_tests_Array2Address_Debug_fmt is [Address; 2]'s Debug, its slice's. */
static bool
chunks_index_tests_Array2Address_Debug_fmt(const void *value, StdFormatter *f)
{
	return chunks_index_tests_SliceAddress_Debug_fmt(value, 2, f);
}

/*
 * chunks_index_tests_SliceAddress_PartialEqArray2Address_eq is [Address]'s
 * PartialEq with an [Address; 2]: a slice of two, each address equal.
 */
static bool
chunks_index_tests_SliceAddress_PartialEqArray2Address_eq(const ChunksAddressAddress *self, size_t self_len,
														  const ChunksAddressAddress *other)
{
	return self_len == 2 && chunks_address_Address_PartialEq_eq(&self[0], &other[0]) &&
		chunks_address_Address_PartialEq_eq(&self[1], &other[1]);
}

/*
 * The entries below an address that the test's run starts with:
 * Take<Filter<vec::IntoIter>> as its own iterator, the IntoIter it owns, the
 * count still to take, and the filter's capture.
 */
typedef struct test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter
{
	StdVecIntoIter iter;
	size_t		n;
	const ChunksAddressAddress *repeated;
} test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter;

/* below's next: Take's, a count down, then Filter's next entry below the address. */
static bool
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter_next(test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter *self,
																													chunks_TupleAddressLocator_t *result)
{
	if (self->n == 0)
		return false;
	self->n = std_wrapping_sub_usize(self->n, 1);
	while (std_VecIntoIter_Iterator_next(&self->iter, result))
	{
		if (chunks_address_Address_PartialOrd_lt(&result->f0, self->repeated))
			return true;
	}
	return false;
}

/* below's size_hint: Take's of Filter's, no lower bound, and the fewer of the IntoIter's and the count left. */
static StdSizeHint
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter_size_hint(const test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter *self)
{
	size_t		upper;

	if (self->n == 0)
		return (StdSizeHint)
	{
		0, true, 0
	};
	upper = std_VecIntoIter_Iterator_size_hint(&self->iter).upper;
	return (StdSizeHint)
	{
		std_min_usize(0, self->n), true, upper < self->n ? upper : self->n
	};
}

/* The copies the test's run ends with: Map<Range<u64>, closure_1> as its own iterator, the range and the capture. */
typedef struct test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter
{
	uint64		start;
	uint64		end;
	const ChunksAddressAddress *repeated;
} test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter;

/* copies' next: the range's next pack, as the copy (repeated, at(pack, 0, 1)). */
static bool
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter_next(test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter *self,
																													 chunks_TupleAddressLocator_t *result)
{
	uint64		pack;

	if (!(self->start < self->end))
		return false;
	pack = self->start;
	self->start = std_forward_unchecked_u64_Checked(pack, 1);
	result->f0 = *self->repeated;
	result->f1 = chunks_trailer_Locator_at(pack, 0, 1);
	return true;
}

/* copies' size_hint, the range's: the exact number left. */
static StdSizeHint
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter_size_hint(const test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter *self)
{
	size_t		n = self->start < self->end ? (size_t) std_wrapping_sub_u64(self->end, self->start) : 0;

	return (StdSizeHint)
	{
		n, true, n
	};
}

/* The test's closure_2: whether a locator is in pack 2. */
static bool
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_closure_2(void *captures,
																											  const ChunksTrailerLocator *l)
{
	return l->pack == 2;
}

/* The test's closure_4: whether a locator is in a pack other than 2. */
static bool
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_closure_4(void *captures,
																											  const ChunksTrailerLocator *l)
{
	return l->pack != 2;
}

/* The test's closure_6, its capture the last pack: whether a locator is in it. */
static bool
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_closure_6(void *captures,
																											  const ChunksTrailerLocator *l)
{
	return l->pack == *(const uint64 *) captures;
}

/*
 * The state of the test across its pad: its capability, directory, the
 * entries below and whether its chain still holds them, the entries, the
 * path it writes, the run, the entries a range reads, and an unwrapped
 * error.
 */
typedef struct test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter below;
	bool		below_live;
	StdVec		entries;
	bool		entries_live;
	StdPathBuf	path;
	bool		path_live;
	ChunksIndexRunLocator run;
	bool		run_live;
	StdVec		range;
	bool		range_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_body_0(void *frame_)
{
	test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_frame_0_t *frame =
		frame_;
	static const uint8 repeated_bytes[] = {'r', 'e', 'p', 'e', 'a', 't', 'e', 'd'};
	HostIoIoDoors io;
	HostIoReadDoors read;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksAddressAddress repeated;
	StdVec		all;
	test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter copies;
	bool		copies_live = true;
	chunks_TupleAddressLocator_t item;
	bool		has_item;
	size_t		keys_len;
	const ChunksAddressAddress *keys;
	ChunksAddressAddress want[2];
	chunks_SliceAddress_t left_keys;
	bool		has;
	ChunksTrailerLocator found;
	uint64		last;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1228, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	repeated = chunks_address_Address_of(repeated_bytes, sizeof(repeated_bytes));
	all = chunks_index_tests_entries(0, 10000, 1);
	frame->below.iter = std_Vec_IntoIterator_into_iter(&all, CHUNKS_INDEX_TESTS_ENTRIES, NULL);
	frame->below_live = true;
	frame->below.repeated = &repeated;
	frame->below.n = STD_SUB(usize, chunks_index_per_block_Locator(), 1, 1230, 87);
	copies.start = 2;
	copies.end = STD_ADD(u64, 3, (uint64) chunks_index_per_block_Locator(), 1231, 26);
	copies.repeated = &repeated;
	has_item = test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter_next(&frame->below,
																																   &item);
	if (!has_item)
	{
		frame->below_live = false;
		std_VecIntoIter_drop(&frame->below.iter);
		has_item = test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter_next(&copies,
																																		&item);
	}
	if (!has_item)
		frame->entries = (StdVec) STD_VEC_NEW;
	else
	{
		size_t		a_lower = frame->below_live ?
			test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter_size_hint(&frame->below).lower : 0;
		size_t		b_lower = copies_live ?
			test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter_size_hint(&copies).lower : 0;

		std_vec_collect_first(&frame->entries, &item, std_saturating_add_usize(a_lower, b_lower),
							  CHUNKS_INDEX_TESTS_ENTRIES);
	}
	frame->entries_live = true;
	for (;;)
	{
		size_t		a_lower;
		size_t		b_lower;

		has_item = false;
		if (frame->below_live)
		{
			has_item = test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter_next(&frame->below,
																																		   &item);
			if (!has_item)
			{
				frame->below_live = false;
				std_VecIntoIter_drop(&frame->below.iter);
			}
		}
		if (!has_item && copies_live)
			has_item = test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter_next(&copies,
																																			&item);
		if (!has_item)
			break;
		a_lower = frame->below_live ?
			test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_below_iter_size_hint(&frame->below).lower : 0;
		b_lower = copies_live ?
			test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_copies_iter_size_hint(&copies).lower : 0;
		std_vec_extend_one(&frame->entries, &item, std_saturating_add_usize(a_lower, b_lower),
						   CHUNKS_INDEX_TESTS_ENTRIES);
	}
	if (frame->below_live)
	{
		frame->below_live = false;
		std_VecIntoIter_drop(&frame->below.iter);
	}
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "r.idx", 5, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->entries.ptr,
									   frame->entries.len, &frame->run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1233, 71, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->run_live = true;
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	keys = chunks_index_Fences_keys(&frame->run.fences, &keys_len);
	std_index_check(0, frame->entries.len, RUST_FILE, 1234, 47);
	want[0] = STD_VEC_AT(&frame->entries, chunks_TupleAddressLocator_t, 0).f0;
	want[1] = repeated;
	left_keys.ptr = keys;
	left_keys.len = keys_len;
	STD_ASSERT_EQ(1234, 9, chunks_index_tests_SliceAddress_PartialEqArray2Address_eq(keys, keys_len, want), &left_keys,
				  chunks_index_tests_RefSliceAddress_Debug_fmt, want, chunks_index_tests_Array2Address_Debug_fmt);
	if (!chunks_index_RunLocator_get(&frame->run, read, &repeated,
									 test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_closure_2,
									 NULL, NULL, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1235, 60, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		uint64		two = 2;
		StdOptionRef left = {has, STD_ITEM(&found.pack, std_u64_Debug_fmt)};
		StdOptionRef right = {true, STD_ITEM(&two, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(1235, 9, has && found.pack == two, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	if (!chunks_index_RunLocator_get(&frame->run, read, &repeated,
									 test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_closure_4,
									 NULL, NULL, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1236, 60, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		uint64		three = 3;
		StdOptionRef left = {has, STD_ITEM(&found.pack, std_u64_Debug_fmt)};
		StdOptionRef right = {true, STD_ITEM(&three, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(1236, 9, has && found.pack == three, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	last = STD_ADD(u64, 2, (uint64) chunks_index_per_block_Locator(), 1237, 20);
	if (!chunks_index_RunLocator_get(&frame->run, read, &repeated,
									 test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_closure_6,
									 &last, NULL, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1238, 63, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		StdOptionRef left = {has, STD_ITEM(&found.pack, std_u64_Debug_fmt)};
		StdOptionRef right = {true, STD_ITEM(&last, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(1238, 9, has && found.pack == last, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	if (!chunks_index_RunLocator_range(&frame->run, read, &repeated, &repeated, &frame->range, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1239, 56, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->range_live = true;
	{
		size_t		len = frame->range.len;
		size_t		want_len = STD_ADD(usize, chunks_index_per_block_Locator(), 1, 1239, 72);

		STD_ASSERT_EQ(1239, 9, len == want_len, &len, std_usize_Debug_fmt, &want_len, std_usize_Debug_fmt);
	}
	frame->range_live = false;
	std_vec_free_Checked(&frame->range, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->run_live = false;
	chunks_index_RunLocator_drop(&frame->run);
	frame->entries_live = false;
	std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_pad_0(void *frame_)
{
	test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_frame_0_t *frame =
		frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->range_live)
		std_vec_free_Checked(&frame->range, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->run_live)
		chunks_index_RunLocator_drop(&frame->run);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->entries_live)
		std_vec_free_Checked(&frame->entries, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->below_live)
		std_VecIntoIter_drop(&frame->below.iter);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address:
 * a run whose second block starts with an address that its first block's
 * last entry also has finds every copy, the first one in the block before.
 */
void
test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address(void)
{
	test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_frame_0_t frame =
	{0};

	std_landing(&frame, test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_body_0,
				test_chunks_index_tests_a_run_finds_a_copy_in_the_block_before_a_block_that_starts_with_its_address_pad_0);
}

/*
 * chunks_address_Address_PartialOrd_lt_FnMut_call_mut is the fn item
 * Address's PartialOrd::lt's FnMut shim, as the addresses' sort passes it for
 * is_less.
 */
static bool
chunks_address_Address_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return chunks_address_Address_PartialOrd_lt(a, b);
}

/* The stable sort of addresses that the merging test's addresses runs: core's shared sort parts and sort at Address. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS Address
#define STD_SORT_T ChunksAddressAddress
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS Address
#define STD_STABLE_SORT_T ChunksAddressAddress
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT chunks_address_Address_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"

/*
 * The state of the test's addresses across its pad: its capture, its
 * arguments, its result, and the entries with the prefix it reads.
 */
typedef struct test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_frame_0_t
{
	const ChunksIndexAddressIndexLocator *index;
	HostIoIo   *io;
	const char *prefix;
	size_t		prefix_len;
	StdVec	   *result;
	StdVecIntoIter entries;
	bool		entries_live;
	bool		found_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_frame_0_t;

/* The addresses' body. */
static void
test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_body_0(void *frame_)
{
	test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_frame_0_t *frame = frame_;
	StdVec		with_prefix;
	StdSizeHint hint;
	chunks_TupleAddressLocator_t entry;

	if (!chunks_index_AddressIndexLocator_with_prefix(frame->index, frame->io, frame->prefix, frame->prefix_len,
													  &with_prefix, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1167, 73, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->entries = std_Vec_IntoIterator_into_iter(&with_prefix, CHUNKS_INDEX_TESTS_ENTRIES, NULL);
	frame->entries_live = true;
	hint = std_VecIntoIter_Iterator_size_hint(&frame->entries);
	std_vec_with_capacity_trusted(frame->result, hint, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->found_live = true;
	std_vec_extend_trusted_reserve(frame->result, hint, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	while (std_VecIntoIter_Iterator_next(&frame->entries, &entry))
	{
		STD_VEC_AT(frame->result, ChunksAddressAddress, frame->result->len) = entry.f0;
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	frame->entries_live = false;
	std_VecIntoIter_drop(&frame->entries);
	std_slice_sort_Address(frame->result->ptr, frame->result->len);
	std_vec_dedup(frame->result, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress),
				  chunks_address_Address_PartialEq_eq, NULL);
	frame->found_live = false;
}

/* The addresses' pad: the entries it reads, and the addresses it gathers. */
static void
test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_pad_0(void *frame_)
{
	test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->found_live)
		std_vec_free_Checked(frame->result, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->entries_live)
		std_VecIntoIter_drop(&frame->entries);
}

/*
 * test_chunks_index_tests_finds_entries_and_merges_runs_closure_0 is the
 * test's addresses, its capture the index: the addresses of the index's
 * entries with a prefix, each once, in order.
 */
static void
test_chunks_index_tests_finds_entries_and_merges_runs_closure_0(const ChunksIndexAddressIndexLocator *index, HostIoIo *io,
																const char *prefix, size_t prefix_len, StdVec *result)
{
	test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_frame_0_t frame = {.index = index, .io = io,
	.prefix = prefix, .prefix_len = prefix_len, .result = result};

	std_landing(&frame, test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_body_0,
				test_chunks_index_tests_finds_entries_and_merges_runs_closure_0_pad_0);
}

/*
 * The state of the test across its pad: its capability, directory, the
 * entries it writes, the paths it joins, the index, the address's text, the
 * addresses it compares, the merged entries, the runs replaced, the run
 * reopened, the entries it compares, and an unwrapped error.
 */
typedef struct test_chunks_index_tests_finds_entries_and_merges_runs_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdVec		first;
	bool		first_live;
	StdPathBuf	path;
	bool		path_live;
	StdVec		second;
	bool		second_live;
	ChunksIndexAddressIndexLocator index;
	bool		index_live;
	StdString	some;
	bool		some_live;
	StdVec		found;
	bool		found_live;
	StdVec		want;
	bool		want_live;
	StdVec		left;
	bool		left_live;
	StdVec		right;
	bool		right_live;
	StdVec		merged;
	bool		merged_live;
	StdVec		replaced;
	bool		replaced_live;
	ChunksIndexRunLocator reopened;
	bool		reopened_live;
	ChunksIndexRunLocator garbage_run;
	bool		garbage_run_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_index_tests_finds_entries_and_merges_runs_frame_0_t;

/* The test's body. */
static void
test_chunks_index_tests_finds_entries_and_merges_runs_body_0(void *frame_)
{
	test_chunks_index_tests_finds_entries_and_merges_runs_frame_0_t *frame = frame_;
	static const uint32 numbers[] = {0, 499, 500, 999, 1000, 1499};
	static const uint8 absent[] = {'a', 'b', 's', 'e', 'n', 't'};
	static const uint8 garbage[] = "garbage that is long enough to have a footer";
	HostIoIoDoors io;
	HostIoReadDoors read;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	ChunksIndexRunLocator run;
	uint8		le[4];
	ChunksAddressAddress address;
	bool		has;
	ChunksTrailerLocator found;
	size_t		runs_len;
	StdArcInner *const *runs;
	bool		wrote;
	bool		opened;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(1154, 34, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	frame->first = chunks_index_tests_entries(0, 1000, 1);
	frame->first_live = true;
	std_path_join(dir_path, dir_path_len, "a.idx", 5, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->first.ptr,
									   frame->first.len, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1156, 98, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		ChunksIndexRunLocator *boxed = std_box_new_uninit_layout_Checked(sizeof(ChunksIndexRunLocator),
																		 _Alignof(ChunksIndexRunLocator));
		StdVec		runs_vec;

		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksIndexRunLocator), 1156, 43);
		STD_CHECK_NULL_DEREF(boxed, 1156, 43);
		boxed[0] = run;
		std_box_slice_into_vec_Checked(&runs_vec, boxed, 1, sizeof(ChunksIndexRunLocator));
		frame->index = chunks_index_AddressIndexLocator_new(&runs_vec);
		frame->index_live = true;
	}
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_path_join(dir_path, dir_path_len, "b.idx", 5, &frame->path);
	frame->path_live = true;
	frame->second = chunks_index_tests_entries(500, 1500, 2);
	frame->second_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->second.ptr,
									   frame->second.len, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1157, 85, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	chunks_index_AddressIndexLocator_add(&frame->index, &run);
	frame->second_live = false;
	std_vec_free_Checked(&frame->second, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	std_path_join(dir_path, dir_path_len, "c.idx", 5, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len,
									   (const chunks_TupleAddressLocator_t *) STD_DANGLING_FOR(_Alignof(chunks_TupleAddressLocator_t)),
									   0, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1158, 66, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	chunks_index_AddressIndexLocator_add(&frame->index, &run);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	for (size_t k = 0, k_len = lengthof(numbers); k < k_len; k++)
	{
		uint32		i = numbers[k];
		uint64		pack;
		uint64		want_pack;
		uint64		offset;
		uint64		want_offset;

		std_u32_to_le_bytes(i, le);
		address = chunks_address_Address_of(le, sizeof(le));
		if (!chunks_index_AddressIndexLocator_get(&frame->index, read, &address, &has, &found, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(1160, 71, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
		if (!has)
			STD_OPTION_UNWRAP_FAILED(1160, 80);
		pack = found.pack;
		want_pack = i < 500 ? 1 : 2;
		STD_ASSERT_EQ(1161, 13, pack == want_pack, &pack, std_u64_Debug_fmt, &want_pack, std_u64_Debug_fmt);
		offset = chunks_trailer_Locator_offset(&found);
		want_offset = STD_MUL(u64, (uint64) i, 10, 1162, 40);
		STD_ASSERT_EQ(1162, 13, offset == want_offset, &offset, std_u64_Debug_fmt, &want_offset, std_u64_Debug_fmt);
	}
	address = chunks_address_Address_of(absent, sizeof(absent));
	if (!chunks_index_AddressIndexLocator_get(&frame->index, read, &address, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1164, 59, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	{
		StdOptionRef left = {has, STD_ITEM(&found, chunks_trailer_Locator_Debug_fmt)};
		StdOptionRef right = {false};

		STD_ASSERT_EQ(1164, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	std_u32_to_le_bytes(600, le);
	address = chunks_address_Address_of(le, sizeof(le));
	std_to_string(&address, chunks_address_Address_Display_fmt, &frame->some);
	frame->some_live = true;
	std_str_index_range(frame->some.ptr, frame->some.len, 0, 8, RUST_FILE, 1172, 39);
	test_chunks_index_tests_finds_entries_and_merges_runs_closure_0(&frame->index, &frame->test_io.io, frame->some.ptr, 8,
																	&frame->found);
	frame->found_live = true;
	{
		ChunksAddressAddress want;
		ChunksAddressAddress *boxed;

		std_u32_to_le_bytes(600, le);
		want = chunks_address_Address_of(le, sizeof(le));
		boxed = std_box_new_uninit_layout_Checked(sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
		STD_CHECK_ALIGNED(boxed, _Alignof(ChunksAddressAddress), 1172, 47);
		STD_CHECK_NULL_DEREF(boxed, 1172, 47);
		boxed[0] = want;
		std_box_slice_into_vec_Checked(&frame->want, boxed, 1, sizeof(ChunksAddressAddress));
		frame->want_live = true;
	}
	STD_ASSERT_EQ(1172, 9, chunks_record_tests_VecAddress_PartialEq_eq(&frame->found, &frame->want), &frame->found,
				  chunks_record_tests_VecAddress_Debug_fmt, &frame->want, chunks_record_tests_VecAddress_Debug_fmt);
	frame->want_live = false;
	std_vec_free_Checked(&frame->want, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->found_live = false;
	std_vec_free_Checked(&frame->found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	test_chunks_index_tests_finds_entries_and_merges_runs_closure_0(&frame->index, &frame->test_io.io, "", 0,
																	&frame->found);
	frame->found_live = true;
	STD_ASSERT(1173, 9, frame->found.len == 1500, "addresses(io, \"\").len() == 1500");
	frame->found_live = false;
	std_vec_free_Checked(&frame->found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (!chunks_index_AddressIndexLocator_merged(&frame->index, read, 0, 3, &frame->merged, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1174, 45, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->merged_live = true;
	std_path_join(dir_path, dir_path_len, "m.idx", 5, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_write(io, frame->path.bytes.ptr, frame->path.bytes.len, frame->merged.ptr,
									   frame->merged.len, &run, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1175, 95, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->replaced = chunks_index_AddressIndexLocator_replace(&frame->index, 0, 3, &run);
	frame->replaced_live = true;
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	{
		size_t		len = frame->replaced.len;
		size_t		three = 3;

		STD_ASSERT_EQ(1176, 9, len == three, &len, std_usize_Debug_fmt, &three, std_usize_Debug_fmt);
	}
	runs = chunks_index_AddressIndexLocator_runs(&frame->index, &runs_len);
	{
		size_t		one = 1;

		STD_ASSERT_EQ(1177, 9, runs_len == one, &runs_len, std_usize_Debug_fmt, &one, std_usize_Debug_fmt);
	}
	runs = chunks_index_AddressIndexLocator_runs(&frame->index, &runs_len);
	std_index_check(0, runs_len, RUST_FILE, 1178, 20);
	{
		uint64		len = chunks_index_RunLocator_len(std_arc_Deref_deref(runs[0], _Alignof(ChunksIndexRunLocator)));
		uint64		want = 2000;

		STD_ASSERT_EQ(1178, 9, len == want, &len, std_u64_Debug_fmt, &want, std_u64_Debug_fmt);
	}
	std_u32_to_le_bytes(700, le);
	address = chunks_address_Address_of(le, sizeof(le));
	if (!chunks_index_AddressIndexLocator_get(&frame->index, read, &address, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1179, 71, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	if (!has)
		STD_OPTION_UNWRAP_FAILED(1179, 80);
	{
		uint64		two = 2;

		STD_ASSERT_EQ(1179, 9, found.pack == two, &found.pack, std_u64_Debug_fmt, &two, std_u64_Debug_fmt);
	}
	std_u32_to_le_bytes(7, le);
	address = chunks_address_Address_of(le, sizeof(le));
	if (!chunks_index_AddressIndexLocator_get(&frame->index, read, &address, &has, &found, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1180, 69, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	if (!has)
		STD_OPTION_UNWRAP_FAILED(1180, 78);
	{
		uint64		one = 1;

		STD_ASSERT_EQ(1180, 9, found.pack == one, &found.pack, std_u64_Debug_fmt, &one, std_u64_Debug_fmt);
	}
	std_path_join(dir_path, dir_path_len, "m.idx", 5, &frame->path);
	frame->path_live = true;
	if (!chunks_index_RunLocator_open(read, frame->path.bytes.ptr, frame->path.bytes.len, &frame->reopened,
									  &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1181, 65, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->reopened_live = true;
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (!chunks_index_RunLocator_entries(&frame->reopened, read, &frame->left, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1182, 41, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->left_live = true;
	runs = chunks_index_AddressIndexLocator_runs(&frame->index, &runs_len);
	std_index_check(0, runs_len, RUST_FILE, 1182, 51);
	if (!chunks_index_RunLocator_entries(std_arc_Deref_deref(runs[0], _Alignof(ChunksIndexRunLocator)), read,
										 &frame->right, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(1182, 79, CHUNKS_INDEX_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->right_live = true;
	STD_ASSERT_EQ(1182, 9, chunks_record_tests_VecTupleAddressLocator_PartialEq_eq(&frame->left, &frame->right),
				  &frame->left, chunks_record_tests_VecTupleAddressLocator_Debug_fmt, &frame->right,
				  chunks_record_tests_VecTupleAddressLocator_Debug_fmt);
	frame->right_live = false;
	std_vec_free_Checked(&frame->right, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->left_live = false;
	std_vec_free_Checked(&frame->left, CHUNKS_INDEX_TESTS_ENTRIES);
	std_path_join(dir_path, dir_path_len, "m.idx", 5, &frame->path);
	frame->path_live = true;
	wrote = std_fs_write(frame->path.bytes.ptr, frame->path.bytes.len, garbage, sizeof(garbage) - 1, &io_error);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (!wrote)
		STD_RESULT_EXPECT_FAILED(1183, 99, CHUNKS_INDEX_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	std_path_join(dir_path, dir_path_len, "m.idx", 5, &frame->path);
	frame->path_live = true;
	opened = chunks_index_RunLocator_open(read, frame->path.bytes.ptr, frame->path.bytes.len, &frame->garbage_run,
										  &frame->error);
	frame->garbage_run_live = opened;
	frame->error_live = !opened;
	STD_ASSERT(1184, 9, !opened, "Run::<Locator>::open(io, &dir.path().join(\"m.idx\")).is_err()");
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->reopened_live = false;
	chunks_index_RunLocator_drop(&frame->reopened);
	frame->replaced_live = false;
	std_vec_drop_Checked(&frame->replaced, sizeof(StdArcInner *), _Alignof(StdArcInner *),
						 chunks_index_ArcRunLocator_drop);
	frame->merged_live = false;
	std_vec_free_Checked(&frame->merged, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->some_live = false;
	std_vec_free_Checked(&frame->some, 1, 1);
	frame->index_live = false;
	chunks_index_AddressIndexLocator_drop(&frame->index);
	frame->first_live = false;
	std_vec_free_Checked(&frame->first, CHUNKS_INDEX_TESTS_ENTRIES);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_index_tests_finds_entries_and_merges_runs_pad_0(void *frame_)
{
	test_chunks_index_tests_finds_entries_and_merges_runs_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->garbage_run_live)
		chunks_index_RunLocator_drop(&frame->garbage_run);
	if (frame->right_live)
		std_vec_free_Checked(&frame->right, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->left_live)
		std_vec_free_Checked(&frame->left, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->want_live)
		std_vec_free_Checked(&frame->want, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->found_live)
		std_vec_free_Checked(&frame->found, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->reopened_live)
		chunks_index_RunLocator_drop(&frame->reopened);
	if (frame->replaced_live)
		std_vec_drop_Checked(&frame->replaced, sizeof(StdArcInner *), _Alignof(StdArcInner *),
							 chunks_index_ArcRunLocator_drop);
	if (frame->merged_live)
		std_vec_free_Checked(&frame->merged, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->some_live)
		std_vec_free_Checked(&frame->some, 1, 1);
	if (frame->index_live)
		chunks_index_AddressIndexLocator_drop(&frame->index);
	if (frame->second_live)
		std_vec_free_Checked(&frame->second, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->first_live)
		std_vec_free_Checked(&frame->first, CHUNKS_INDEX_TESTS_ENTRIES);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_index_tests_finds_entries_and_merges_runs: an index of three
 * runs finds each address in its newest run, lists the addresses with a
 * prefix, merges its runs into one that answers the same, and refuses a run
 * file that is garbage.
 */
void
test_chunks_index_tests_finds_entries_and_merges_runs(void)
{
	test_chunks_index_tests_finds_entries_and_merges_runs_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_index_tests_finds_entries_and_merges_runs_body_0,
				test_chunks_index_tests_finds_entries_and_merges_runs_pad_0);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
