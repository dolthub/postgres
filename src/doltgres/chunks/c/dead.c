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
 * A pack's deletion vector (chunks' dead.rs): the offsets of the chunks a
 * full collection found dead in a pack it kept, in a file beside the pack
 * named by the pack's id and the vector's generation, whose missing or
 * damaged file is an error of chunks' own, never an empty vector.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/str.h"
#include "doltgres/std/unwind.h"
#ifdef DOLTGRES_TEST_CHUNKS
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#endif

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#define RUST_FILE "chunks" STD_SEP "src" STD_SEP "dead.rs"

#ifdef DOLTGRES_TEST_CHUNKS

/*
 * std_u64_PartialOrd_lt_FnMut_call_mut is the fn item u64's PartialOrd::lt's
 * FnMut shim, as the sort passes it for is_less.
 */
static bool
std_u64_PartialOrd_lt_FnMut_call_mut(void *captures, const void *a, const void *b)
{
	return std_u64_PartialOrd_lt(a, b);
}

/* The stable sort of offsets that the tests' BTreeSet<u64>::from runs: core's shared sort parts and sort at u64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_SHARED_ONLY
#define STD_SORT_WITHOUT_UNSTABLE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#include "doltgres/std/sort_template.h"
#define STD_STABLE_SORT_SCOPE static
#define STD_STABLE_SORT_DECLARE
#define STD_STABLE_SORT_DEFINE
#define STD_STABLE_SORT_TS U64
#define STD_STABLE_SORT_T uint64
#define STD_STABLE_SORT_FREEZE 1
#define STD_STABLE_SORT_LT std_u64_PartialOrd_lt_FnMut_call_mut
#include "doltgres/std/stable_sort_template.h"
#endif

/* BTreeSet<u64>, a deletion vector's offsets, a BTreeMap<u64, SetValZST> its crate iterates. */
#define STD_BTREE_SCOPE extern
#define STD_BTREE_DECLARE
#define STD_BTREE_DEFINE
#define STD_BTREE_KS U64
#define STD_BTREE_K uint64
#define STD_BTREE_VS Set
#define STD_BTREE_V StdBTreeSetValZST
#define STD_BTREE_V_ZERO_SIZED
#define STD_BTREE_ITERATE
#define STD_BTREE_SET
#ifdef DOLTGRES_TEST_CHUNKS
#define STD_BTREE_DROP
#define STD_BTREE_BULK
#define STD_BTREE_KEY_NE(a, b) (*(a) != *(b))
#define STD_BTREE_SLICE_SORT(v, len) std_slice_sort_U64((v), (len))
#endif
#define STD_BTREE_LAYOUT U64_SET
#define STD_BTREE_CMP(a, b) std_u64_Ord_cmp((a), (b))
#define STD_BTREE_KEY_DROP(k) ((void) (k))
#define STD_BTREE_VAL_DROP(v) ((void) (v))
#define STD_BTREE_VAL_DEFAULT(v) ((void) (v))
#define STD_BTREE_TABLES
#define STD_BTREE_TABLE_ARGS
#include "doltgres/std/btree_template.h"

/* CHUNKS_DEAD_MAGIC starts every deletion vector's file. */
static const uint8 CHUNKS_DEAD_MAGIC[8] = {0xd1, 0x5a, 0x0e, 0x77, 0xb3, 0x29, 0xc4, 0x6f};

/* chunks_dead_Dead_Debug_fmt is Dead's derived Debug. */
bool
chunks_dead_Dead_Debug_fmt(const void *value, StdFormatter *f)
{
	const ChunksDeadDead *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Dead", 4);

	std_DebugStruct_field(&s, "offsets", 7, &self->offsets, chunks_trailer_VecU64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* chunks_dead_Dead_PartialEq_eq is Dead's derived PartialEq: the same offsets in the same order. */
bool
chunks_dead_Dead_PartialEq_eq(const void *self_, const void *other_)
{
	const ChunksDeadDead *self = self_;
	const ChunksDeadDead *other = other_;

	return self->offsets.len == other->offsets.len &&
		memcmp(self->offsets.ptr, other->offsets.ptr, std_wrapping_mul_usize(self->offsets.len, sizeof(uint64))) == 0;
}

/*
 * chunks_dead_Dead_path_frame_0_t is path's state across its pad: its
 * arguments, its result, and the extension it formats.
 */
typedef struct chunks_dead_Dead_path_frame_0_t
{
	const char *pack;
	size_t		pack_len;
	uint64		generation;
	StdPathBuf	result;
	StdString	extension;
	bool		extension_live;
} chunks_dead_Dead_path_frame_0_t;

/* chunks_dead_Dead_path_body_0 is path's body. */
static void
chunks_dead_Dead_path_body_0(void *frame_)
{
	chunks_dead_Dead_path_frame_0_t *frame = frame_;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(".dead")};
	StdFmtArg	args[] = {{&frame->generation, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};

	std_fmt_format_Checked(&frame->extension, &arguments);
	frame->extension_live = true;
	std_path_with_extension(frame->pack, frame->pack_len, frame->extension.ptr, frame->extension.len,
							&frame->result);
	frame->extension_live = false;
	std_vec_free_Checked(&frame->extension, 1, 1);
}

/* chunks_dead_Dead_path_pad_0 is path's pad: the extension, while with_extension holds it. */
static void
chunks_dead_Dead_path_pad_0(void *frame_)
{
	chunks_dead_Dead_path_frame_0_t *frame = frame_;

	if (frame->extension_live)
		std_vec_free_Checked(&frame->extension, 1, 1);
}

/* chunks_dead_Dead_path returns the file of a generation of the vector of the pack at a path, beside the pack. */
StdPathBuf
chunks_dead_Dead_path(const char *pack, size_t pack_len, uint64 generation)
{
	chunks_dead_Dead_path_frame_0_t frame = {.pack = pack, .pack_len = pack_len, .generation = generation};

	std_landing(&frame, chunks_dead_Dead_path_body_0, chunks_dead_Dead_path_pad_0);
	return frame.result;
}

/*
 * chunks_dead_Dead_write_frame_0_t is write's state across its pad: its
 * arguments, its error, the bytes it writes, and the vector's path.
 */
typedef struct chunks_dead_Dead_write_frame_0_t
{
	HostIoIoDoors io;
	const char *pack;
	size_t		pack_len;
	uint64		generation;
	const StdBTreeMap *offsets;
	ChunksErrorError *error;
	StdVec		bytes;
	bool		bytes_live;
	StdPathBuf	path;
	bool		path_live;
	bool		ok;
} chunks_dead_Dead_write_frame_0_t;

/* chunks_dead_Dead_write_body_0 is write's body. */
static void
chunks_dead_Dead_write_body_0(void *frame_)
{
	chunks_dead_Dead_write_frame_0_t *frame = frame_;
	size_t		capacity = STD_ADD(usize, 16, STD_MUL(usize, 8, frame->offsets->length, 47, 49), 47, 44);
	StdBTreeSetIter offsets;
	const uint64 *offset;
	uint8		le[8];

	std_vec_with_capacity_Checked(&frame->bytes, capacity, 1, 1);
	frame->bytes_live = true;
	std_vec_extend_from_slice_Checked(&frame->bytes, CHUNKS_DEAD_MAGIC, sizeof(CHUNKS_DEAD_MAGIC), 1, 1);
	offsets = std_btree_RefBTreeSetU64_IntoIterator_into_iter(frame->offsets);
	while (std_btree_SetIterU64_Iterator_next(&offsets, &offset))
	{
		std_u64_to_le_bytes(*offset, le);
		std_vec_extend_from_slice_Checked(&frame->bytes, le, sizeof(le), 1, 1);
	}
	std_u64_to_le_bytes(XXH3_64bits(frame->bytes.ptr, frame->bytes.len), le);
	std_vec_extend_from_slice_Checked(&frame->bytes, le, sizeof(le), 1, 1);
	frame->path = chunks_dead_Dead_path(frame->pack, frame->pack_len, frame->generation);
	frame->path_live = true;
	frame->ok = chunks_file_write_file(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, frame->bytes.ptr,
									   frame->bytes.len, frame->error);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* chunks_dead_Dead_write_pad_0 is write's pad: the vector's path and its bytes, while it holds them. */
static void
chunks_dead_Dead_write_pad_0(void *frame_)
{
	chunks_dead_Dead_write_frame_0_t *frame = frame_;

	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * chunks_dead_Dead_write writes a generation of a pack's vector durably: the
 * magic, each offset in order, and a checksum of both.
 */
bool
chunks_dead_Dead_write(HostIoIoDoors io, const char *pack, size_t pack_len, uint64 generation,
					   const StdBTreeMap *offsets, ChunksErrorError *error)
{
	chunks_dead_Dead_write_frame_0_t frame = {.io = io, .pack = pack, .pack_len = pack_len,
	.generation = generation, .offsets = offsets, .error = error};

	std_landing(&frame, chunks_dead_Dead_write_body_0, chunks_dead_Dead_write_pad_0);
	return frame.ok;
}

/*
 * chunks_dead_Dead_read_closure_0_frame_0_t is damaged's state across its
 * pad: its capture, its arguments, its result, and the message it formats.
 */
typedef struct chunks_dead_Dead_read_closure_0_frame_0_t
{
	const StdPathBuf *path;
	size_t		offset;
	const char *what;
	size_t		what_len;
	ChunksErrorError *result;
	StdString	message;
	bool		message_live;
} chunks_dead_Dead_read_closure_0_frame_0_t;

/* chunks_dead_Dead_read_closure_0_body_0 is damaged's body. */
static void
chunks_dead_Dead_read_closure_0_body_0(void *frame_)
{
	chunks_dead_Dead_read_closure_0_frame_0_t *frame = frame_;
	StdStr		shown = {frame->path->bytes.ptr, frame->path->bytes.len};
	StdStr		wrong = {frame->what, frame->what_len};
	StdPathBuf	path;
	StdFmtPiece pieces[] = {STD_FMT_LIT("deletion vector file \""), STD_FMT_ARG(0), STD_FMT_LIT("\" "),
	STD_FMT_ARG(1)};
	StdFmtArg	args[] = {{&shown, std_path_Display_fmt}, {&wrong, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 4, args};

	std_fmt_format_Checked(&frame->message, &arguments);
	frame->message_live = true;
	std_PathBuf_Clone_clone(frame->path, &path);
	frame->message_live = false;
	frame->result->tag = CHUNKS_ERROR_ERROR_DAMAGED;
	frame->result->u.damaged.path = path;
	frame->result->u.damaged.offset = (uint64) frame->offset;
	frame->result->u.damaged.message = frame->message;
}

/* chunks_dead_Dead_read_closure_0_pad_0 is damaged's pad: the message, while the path's clone can unwind. */
static void
chunks_dead_Dead_read_closure_0_pad_0(void *frame_)
{
	chunks_dead_Dead_read_closure_0_frame_0_t *frame = frame_;

	if (frame->message_live)
		std_vec_free_Checked(&frame->message, 1, 1);
}

/*
 * chunks_dead_Dead_read_closure_0 is read's damaged, its capture the
 * vector's path: the damage of the vector's file at an offset, with what is
 * wrong with it.
 */
static void
chunks_dead_Dead_read_closure_0(void *captures, size_t offset, const char *what, size_t what_len,
								ChunksErrorError *result)
{
	chunks_dead_Dead_read_closure_0_frame_0_t frame = {.path = captures, .offset = offset, .what = what,
	.what_len = what_len, .result = result};

	std_landing(&frame, chunks_dead_Dead_read_closure_0_body_0, chunks_dead_Dead_read_closure_0_pad_0);
}

/*
 * chunks_dead_Dead_read_frame_0_t is read's state across its pad: its
 * arguments, its result and error, the vector's path, its file, its bytes,
 * and the offsets it reads.
 */
typedef struct chunks_dead_Dead_read_frame_0_t
{
	HostIoReadDoors io;
	const char *pack;
	size_t		pack_len;
	uint64		generation;
	ChunksDeadDead *result;
	ChunksErrorError *error;
	StdPathBuf	path;
	bool		path_live;
	HostFile	file;
	bool		file_live;
	StdVec		bytes;
	bool		bytes_live;
	StdVec		offsets;
	bool		offsets_live;
	bool		ok;
} chunks_dead_Dead_read_frame_0_t;

/* chunks_dead_Dead_read_body_0 is read's body. */
static void
chunks_dead_Dead_read_body_0(void *frame_)
{
	chunks_dead_Dead_read_frame_0_t *frame = frame_;
	HostError	host_error;
	bool		has;
	uint64		size;
	const void *init;
	size_t		init_len;
	const void *last;
	bool		whole;
	const uint8 *body;
	uint8		sum[8];
	const void *chunks;
	size_t		chunks_len;
	const void *remainder;
	size_t		remainder_len;
	StdSizeHint hint;
	StdWindows	windows;
	const void *window;
	size_t		window_len;
	size_t		at = 0;
	bool		found = false;

	frame->path = chunks_dead_Dead_path(frame->pack, frame->pack_len, frame->generation);
	frame->path_live = true;
	if (!host_open_if_there(frame->io, frame->path.bytes.ptr, frame->path.bytes.len, &has, &frame->file,
							&host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		goto drop_path;
	}
	if (!has)
	{
		chunks_dead_Dead_read_closure_0(&frame->path, 0, "is missing", 10, frame->error);
		goto drop_path;
	}
	frame->file_live = true;
	if (!host_size(frame->io, &frame->file, &size, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		goto drop_file;
	}
	if (!chunks_file_read_at(frame->io, &frame->file, 0, (size_t) size, &frame->bytes, frame->error))
		goto drop_file;
	frame->bytes_live = true;
	whole = std_slice_split_last_chunk(frame->bytes.ptr, frame->bytes.len, 8, 1, 1, &init, &init_len, &last);
	if (whole)
		whole = std_bytes_starts_with(init, init_len, CHUNKS_DEAD_MAGIC, sizeof(CHUNKS_DEAD_MAGIC)) &&
			STD_REM_FOLDED(usize, init_len, 8) == 0;
	if (!whole)
	{
		chunks_dead_Dead_read_closure_0(&frame->path, 0, "is not a whole deletion vector", 30, frame->error);
		goto drop_bytes;
	}
	body = init;
	std_u64_to_le_bytes(XXH3_64bits(body, init_len), sum);
	if (memcmp(sum, last, sizeof(sum)) != 0)
	{
		chunks_dead_Dead_read_closure_0(&frame->path, 0, "fails its checksum", 18, frame->error);
		goto drop_bytes;
	}
	std_range_from_check(8, init_len, RUST_FILE, 73, 37);
	std_slice_as_chunks(STD_PTR_AT(body, 8, 1), std_wrapping_sub_usize(init_len, 8), 1, 1, 8, &chunks, &chunks_len, &remainder, &remainder_len,
						RUST_FILE, 73, 43);
	hint = (StdSizeHint)
	{
		chunks_len, true, chunks_len
	};
	std_vec_with_capacity_trusted(&frame->offsets, hint, sizeof(uint64), _Alignof(uint64));
	frame->offsets_live = true;
	std_vec_extend_trusted_reserve(&frame->offsets, hint, sizeof(uint64), _Alignof(uint64));
	for (size_t i = 0, i_len = chunks_len; i < i_len; i++)
	{
		STD_VEC_AT(&frame->offsets, uint64, frame->offsets.len) =
			std_u64_from_le_bytes(STD_PTR_AT(chunks, i, 8));
		frame->offsets.len = std_wrapping_add_usize(frame->offsets.len, 1);
	}
	windows = std_slice_windows(frame->offsets.ptr, frame->offsets.len, 2, RUST_FILE, 74, 35);
	while (std_slice_Windows_next(&windows, sizeof(uint64), &window, &window_len))
	{
		const uint64 *pair = window;

		std_index_check(0, window_len, RUST_FILE, 74, 62);
		std_index_check(1, window_len, RUST_FILE, 74, 73);
		if (pair[0] >= pair[1])
		{
			found = true;
			break;
		}
		at = std_Iterator_position_step(at);
	}
	if (found)
	{
		size_t		offset = STD_ADD(usize, 16, STD_MUL(usize, 8, at, 75, 37), 75, 32);

		chunks_dead_Dead_read_closure_0(&frame->path, offset, "holds offsets out of order", 26, frame->error);
		frame->offsets_live = false;
		std_vec_free_Checked(&frame->offsets, sizeof(uint64), _Alignof(uint64));
		goto drop_bytes;
	}
	frame->offsets_live = false;
	frame->result->offsets = frame->offsets;
	frame->ok = true;
drop_bytes:
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
drop_file:
	frame->file_live = false;
	host_File_drop(&frame->file);
drop_path:
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
}

/* chunks_dead_Dead_read_pad_0 is read's pad: the offsets, the bytes, the file, and the path, while it holds them. */
static void
chunks_dead_Dead_read_pad_0(void *frame_)
{
	chunks_dead_Dead_read_frame_0_t *frame = frame_;

	if (frame->offsets_live)
		std_vec_free_Checked(&frame->offsets, sizeof(uint64), _Alignof(uint64));
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
	if (frame->file_live)
		host_File_drop(&frame->file);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
}

/*
 * chunks_dead_Dead_read reads a generation of a pack's vector, refusing as
 * damage a file that is missing, is no whole vector, fails its checksum, or
 * holds offsets out of order.
 */
bool
chunks_dead_Dead_read(HostIoReadDoors io, const char *pack, size_t pack_len, uint64 generation,
					  ChunksDeadDead *result, ChunksErrorError *error)
{
	chunks_dead_Dead_read_frame_0_t frame = {.io = io, .pack = pack, .pack_len = pack_len,
	.generation = generation, .result = result, .error = error};

	std_landing(&frame, chunks_dead_Dead_read_body_0, chunks_dead_Dead_read_pad_0);
	return frame.ok;
}

/* chunks_dead_Dead_holds reports whether the chunk at an offset of the pack is dead. */
bool
chunks_dead_Dead_holds(const ChunksDeadDead *self, uint64 offset)
{
	size_t		index;

	return std_slice_binary_search(self->offsets.ptr, self->offsets.len, sizeof(uint64), &offset, std_u64_Ord_cmp,
								   &index);
}

/* chunks_dead_Dead_drop is Dead's drop glue: its offsets. */
void
chunks_dead_Dead_drop(void *self_)
{
	ChunksDeadDead *self = self_;

	std_vec_free_Checked(&self->offsets, sizeof(uint64), _Alignof(uint64));
}

#ifdef DOLTGRES_TEST_CHUNKS

/* CHUNKS_DEAD_TESTS_UNWRAP is Result::unwrap's panic message. */
#define CHUNKS_DEAD_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* CHUNKS_DEAD_TESTS_UNWRAP_ERR is Result::unwrap_err's panic message. */
#define CHUNKS_DEAD_TESTS_UNWRAP_ERR "called `Result::unwrap_err()` on an `Ok` value"

/* chunks_dead_tests_generation returns a vector's generation, a number other than 0. */
static uint64
chunks_dead_tests_generation(uint64 number)
{
	if (number == 0)
		STD_OPTION_UNWRAP_FAILED(93, 33);
	return number;
}

/*
 * The state of the test across its pad: its capability, directory, the
 * pack's path, the offsets it writes, the paths it compares, the two
 * vectors it reads, and an unwrapped error.
 */
typedef struct test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	pack;
	bool		pack_live;
	StdBTreeMap offsets;
	bool		offsets_live;
	StdPathBuf	left;
	bool		left_live;
	StdPathBuf	right;
	bool		right_live;
	ChunksDeadDead first;
	bool		first_live;
	ChunksDeadDead second;
	bool		second_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_frame_0_t;

/* The test's body. */
static void
test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_body_0(void *frame_)
{
	test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	HostIoReadDoors read;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	uint64		generation;
	uint64		arr[3];

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	read = HOST_IO_IO_READ_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(102, 34, CHUNKS_DEAD_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "0a.pack", 7, &frame->pack);
	frame->pack_live = true;
	generation = chunks_dead_tests_generation(1);
	arr[0] = 40;
	arr[1] = 8;
	arr[2] = (uint64) 1 << 39;
	frame->offsets = std_btree_BTreeSetU64_FromArray_from(arr, 3);
	frame->offsets_live = true;
	if (!chunks_dead_Dead_write(io, frame->pack.bytes.ptr, frame->pack.bytes.len, generation, &frame->offsets,
								&frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(104, 73, CHUNKS_DEAD_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->offsets_live = false;
	std_btree_BTreeMapU64Set_drop(&frame->offsets);
	generation = chunks_dead_tests_generation(2);
	frame->offsets = std_btree_BTreeSetU64_new();
	frame->offsets_live = true;
	if (!chunks_dead_Dead_write(io, frame->pack.bytes.ptr, frame->pack.bytes.len, generation, &frame->offsets,
								&frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(105, 65, CHUNKS_DEAD_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->offsets_live = false;
	std_btree_BTreeMapU64Set_drop(&frame->offsets);
	frame->left = chunks_dead_Dead_path(frame->pack.bytes.ptr, frame->pack.bytes.len, chunks_dead_tests_generation(1));
	frame->left_live = true;
	std_path_join(dir_path, dir_path_len, "0a.1.dead", 9, &frame->right);
	frame->right_live = true;
	STD_ASSERT_EQ(106, 9,
				  std_path_PartialEq_eq(frame->left.bytes.ptr, frame->left.bytes.len, frame->right.bytes.ptr,
										frame->right.bytes.len),
				  &frame->left, std_PathBuf_Debug_fmt, &frame->right, std_PathBuf_Debug_fmt);
	frame->right_live = false;
	std_vec_free_Checked(&frame->right.bytes, 1, 1);
	frame->left_live = false;
	std_vec_free_Checked(&frame->left.bytes, 1, 1);
	if (!chunks_dead_Dead_read(read, frame->pack.bytes.ptr, frame->pack.bytes.len, chunks_dead_tests_generation(1),
							   &frame->first, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(107, 58, CHUNKS_DEAD_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->first_live = true;
	STD_ASSERT(108, 9,
			   chunks_dead_Dead_holds(&frame->first, 8) && chunks_dead_Dead_holds(&frame->first, 40) &&
			   chunks_dead_Dead_holds(&frame->first, (uint64) 1 << 39),
			   "first.holds(8) && first.holds(40) && first.holds(1 << 39)");
	STD_ASSERT(109, 9,
			   !chunks_dead_Dead_holds(&frame->first, 0) && !chunks_dead_Dead_holds(&frame->first, 9) &&
			   !chunks_dead_Dead_holds(&frame->first, PG_UINT64_MAX),
			   "!first.holds(0) && !first.holds(9) && !first.holds(u64::MAX)");
	if (!chunks_dead_Dead_read(read, frame->pack.bytes.ptr, frame->pack.bytes.len, chunks_dead_tests_generation(2),
							   &frame->second, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(110, 59, CHUNKS_DEAD_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->second_live = true;
	STD_ASSERT(111, 9, !chunks_dead_Dead_holds(&frame->second, 8), "!second.holds(8)");
	frame->second_live = false;
	chunks_dead_Dead_drop(&frame->second);
	frame->first_live = false;
	chunks_dead_Dead_drop(&frame->first);
	frame->pack_live = false;
	std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_pad_0(void *frame_)
{
	test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->second_live)
		chunks_dead_Dead_drop(&frame->second);
	if (frame->first_live)
		chunks_dead_Dead_drop(&frame->first);
	if (frame->right_live)
		std_vec_free_Checked(&frame->right.bytes, 1, 1);
	if (frame->left_live)
		std_vec_free_Checked(&frame->left.bytes, 1, 1);
	if (frame->offsets_live)
		std_btree_BTreeMapU64Set_drop(&frame->offsets);
	if (frame->pack_live)
		std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written: a
 * vector read back holds the offsets written and no other, each generation
 * in a file of its own beside the pack, and an empty vector holds none.
 */
void
test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written(void)
{
	test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_body_0,
				test_chunks_dead_tests_a_vector_read_back_holds_the_offsets_written_pad_0);
}

/* The state of the test across its pad: its capability, directory, the pack's path, the vector an unwrap_err finds, and the error. */
typedef struct test_chunks_dead_tests_a_missing_vector_fails_its_read_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	pack;
	bool		pack_live;
	ChunksDeadDead found;
	bool		found_live;
	ChunksErrorError missing;
	bool		missing_live;
	ChunksErrorOwn own;
	bool		own_live;
	ChunksErrorError own_error;
	bool		own_error_live;
} test_chunks_dead_tests_a_missing_vector_fails_its_read_frame_0_t;

/* The test's body. */
static void
test_chunks_dead_tests_a_missing_vector_fails_its_read_body_0(void *frame_)
{
	test_chunks_dead_tests_a_missing_vector_fails_its_read_frame_0_t *frame = frame_;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	bool		is_ok;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(120, 34, CHUNKS_DEAD_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "0a.pack", 7, &frame->pack);
	frame->pack_live = true;
	if (chunks_dead_Dead_read(HOST_IO_IO_READ_DOORS(&frame->test_io.io), frame->pack.bytes.ptr, frame->pack.bytes.len,
							  chunks_dead_tests_generation(1), &frame->found, &frame->missing))
	{
		frame->found_live = true;
		STD_RESULT_EXPECT_FAILED(121, 82, CHUNKS_DEAD_TESTS_UNWRAP_ERR, &frame->found, chunks_dead_Dead_Debug_fmt);
	}
	frame->missing_live = true;
	frame->pack_live = false;
	std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	if (!(frame->missing.tag == CHUNKS_ERROR_ERROR_DAMAGED &&
		  std_str_ends_with_str_Checked(frame->missing.u.damaged.message.ptr, frame->missing.u.damaged.message.len,
										"is missing", 10)))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->missing, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 122, 9, &arguments);
	}
	frame->missing_live = false;
	is_ok = chunks_error_Error_own(&frame->missing, &frame->own, &frame->own_error);
	frame->own_live = is_ok;
	frame->own_error_live = !is_ok;
	STD_ASSERT(123, 9, is_ok, "missing.own().is_ok()");
	frame->own_live = false;
	chunks_error_Own_drop(&frame->own);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_dead_tests_a_missing_vector_fails_its_read_pad_0(void *frame_)
{
	test_chunks_dead_tests_a_missing_vector_fails_its_read_frame_0_t *frame = frame_;

	if (frame->own_error_live)
		chunks_error_Error_drop(&frame->own_error);
	if (frame->own_live)
		chunks_error_Own_drop(&frame->own);
	if (frame->missing_live)
		chunks_error_Error_drop(&frame->missing);
	if (frame->found_live)
		chunks_dead_Dead_drop(&frame->found);
	if (frame->pack_live)
		std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_dead_tests_a_missing_vector_fails_its_read: a vector that
 * isn't there fails its read as damage, an error of chunks' own that a
 * collector can go on from, never as a vector that holds nothing.
 */
void
test_chunks_dead_tests_a_missing_vector_fails_its_read(void)
{
	test_chunks_dead_tests_a_missing_vector_fails_its_read_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_dead_tests_a_missing_vector_fails_its_read_body_0,
				test_chunks_dead_tests_a_missing_vector_fails_its_read_pad_0);
}

/* The test's closure_0's captures: the vector's path and the pack's path, by reference. */
typedef struct test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_t
{
	const StdPathBuf *path;
	const StdPathBuf *pack;
} test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_t;

/* The state of the test's closure_0 across its pad: its captures, its arguments, and what the read gives back. */
typedef struct test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_frame_0_t
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_t *captures;
	HostIoIo   *io;
	const uint8 *bytes;
	size_t		bytes_len;
	uint64		offset;
	const char *what;
	size_t		what_len;
	ChunksDeadDead found;
	bool		found_live;
	ChunksErrorError refused;
	bool		refused_live;
} test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_frame_0_t;

/* The closure_0's body. */
static void
test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_body_0(void *frame_)
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_frame_0_t *frame = frame_;
	const StdPathBuf *path = frame->captures->path;
	const StdPathBuf *pack = frame->captures->pack;
	StdIoError	io_error;

	if (!std_fs_write(path->bytes.ptr, path->bytes.len, frame->bytes, frame->bytes_len, &io_error))
		STD_RESULT_EXPECT_FAILED(139, 42, CHUNKS_DEAD_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	if (chunks_dead_Dead_read(HOST_IO_IO_READ_DOORS(frame->io), pack->bytes.ptr, pack->bytes.len,
							  chunks_dead_tests_generation(3), &frame->found, &frame->refused))
	{
		frame->found_live = true;
		STD_RESULT_EXPECT_FAILED(140, 64, CHUNKS_DEAD_TESTS_UNWRAP_ERR, &frame->found, chunks_dead_Dead_Debug_fmt);
	}
	frame->refused_live = true;
	if (!(frame->refused.tag == CHUNKS_ERROR_ERROR_DAMAGED &&
		  std_path_PartialEq_eq(frame->refused.u.damaged.path.bytes.ptr, frame->refused.u.damaged.path.bytes.len,
								path->bytes.ptr, path->bytes.len) &&
		  frame->refused.u.damaged.offset == frame->offset &&
		  std_str_contains_str_Checked(frame->refused.u.damaged.message.ptr, frame->refused.u.damaged.message.len,
									   frame->what, frame->what_len)))
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->refused, chunks_error_Error_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 141, 13, &arguments);
	}
	frame->refused_live = false;
	chunks_error_Error_drop(&frame->refused);
}

/* The closure_0's pad: the vector an unwrap_err finds, and the error it refuses with. */
static void
test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_pad_0(void *frame_)
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_frame_0_t *frame = frame_;

	if (frame->refused_live)
		chunks_error_Error_drop(&frame->refused);
	if (frame->found_live)
		chunks_dead_Dead_drop(&frame->found);
}

/*
 * test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0 is the test's
 * refused, its captures the vector's and the pack's paths: the bytes written
 * as the vector's file, and its read refused as damage at the offset, of
 * what is wrong.
 */
static void
test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0(void *captures, HostIoIo *io, const uint8 *bytes,
															 size_t bytes_len, uint64 offset, const char *what,
															 size_t what_len)
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_frame_0_t frame = {.captures = captures, .io = io,
	.bytes = bytes, .bytes_len = bytes_len, .offset = offset, .what = what, .what_len = what_len};

	std_landing(&frame, test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_body_0,
				test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_pad_0);
}

/*
 * The state of the test across its pad: its capability, directory, the
 * pack's and the vector's paths, the offsets it writes, the file's bytes,
 * and its changed and swapped copies.
 */
typedef struct test_chunks_dead_tests_a_damaged_vector_is_refused_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	SimcTempDir dir;
	bool		dir_live;
	StdPathBuf	pack;
	bool		pack_live;
	StdPathBuf	path;
	bool		path_live;
	StdBTreeMap offsets;
	bool		offsets_live;
	StdVec		written;
	bool		written_live;
	StdVec		changed;
	bool		changed_live;
	StdVec		swapped;
	bool		swapped_live;
	StdVec		parts;
	bool		parts_live;
	ChunksErrorError error;
	bool		error_live;
} test_chunks_dead_tests_a_damaged_vector_is_refused_frame_0_t;

/* The test's body. */
static void
test_chunks_dead_tests_a_damaged_vector_is_refused_body_0(void *frame_)
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_frame_0_t *frame = frame_;
	HostIoIoDoors io;
	StdIoError	io_error;
	const char *dir_path;
	size_t		dir_path_len;
	uint64		generation;
	uint64		arr[3] = {8, 40, 90};
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0_t refused;
	size_t		len;
	StdBytes	halves[2];
	uint8		sum[8];
	static const char whole[] = "is not a whole deletion vector";
	static const char checksum[] = "fails its checksum";
	static const char order[] = "holds offsets out of order";

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = HOST_IO_IO_IO_DOORS(&frame->test_io.io);
	if (!simc_tempdir(&frame->dir, &io_error))
		STD_RESULT_EXPECT_FAILED(132, 34, CHUNKS_DEAD_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->dir_live = true;
	dir_path = simc_TempDir_path(&frame->dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "0a.pack", 7, &frame->pack);
	frame->pack_live = true;
	frame->path = chunks_dead_Dead_path(frame->pack.bytes.ptr, frame->pack.bytes.len, chunks_dead_tests_generation(3));
	frame->path_live = true;
	generation = chunks_dead_tests_generation(3);
	frame->offsets = std_btree_BTreeSetU64_FromArray_from(arr, 3);
	frame->offsets_live = true;
	if (!chunks_dead_Dead_write(io, frame->pack.bytes.ptr, frame->pack.bytes.len, generation, &frame->offsets,
								&frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(135, 68, CHUNKS_DEAD_TESTS_UNWRAP, &frame->error, chunks_error_Error_Debug_fmt);
	}
	frame->offsets_live = false;
	std_btree_BTreeMapU64Set_drop(&frame->offsets);
	if (!std_fs_read(frame->path.bytes.ptr, frame->path.bytes.len, &frame->written, &io_error))
		STD_RESULT_EXPECT_FAILED(136, 44, CHUNKS_DEAD_TESTS_UNWRAP, &io_error, std_io_Error_Debug_fmt);
	frame->written_live = true;
	refused.path = &frame->path;
	refused.pack = &frame->pack;
	len = STD_SUB(usize, frame->written.len, 1, 147, 32);
	std_range_to_check(len, frame->written.len, RUST_FILE, 147, 29);
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0(&refused, &frame->test_io.io, frame->written.ptr, len,
																 0, whole, sizeof(whole) - 1);
	std_range_to_check(7, frame->written.len, RUST_FILE, 148, 29);
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0(&refused, &frame->test_io.io, frame->written.ptr, 7, 0,
																 whole, sizeof(whole) - 1);
	std_vec_clone(&frame->changed, &frame->written, 1, 1);
	frame->changed_live = true;
	std_index_check(8, frame->changed.len, RUST_FILE, 150, 16);
	STD_VEC_AT(&frame->changed, uint8, 8) ^= 1;
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0(&refused, &frame->test_io.io, frame->changed.ptr,
																 frame->changed.len, 0, checksum, sizeof(checksum) - 1);
	std_vec_clone(&frame->swapped, &frame->written, 1, 1);
	frame->swapped_live = true;
	std_range_check(16, 32, frame->swapped.len, RUST_FILE, 153, 16);
	std_range_check(24, 32, frame->written.len, RUST_FILE, 153, 51);
	halves[0].ptr = STD_PTR_AT(frame->written.ptr, 24, 1);
	halves[0].len = 8;
	std_range_check(16, 24, frame->written.len, RUST_FILE, 153, 69);
	halves[1].ptr = STD_PTR_AT(frame->written.ptr, 16, 1);
	halves[1].len = 8;
	std_slice_concat(halves, 2, sizeof(StdBytes), 1, 1, &frame->parts);
	frame->parts_live = true;
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->swapped.ptr, 16, 1), 16, frame->parts.ptr, frame->parts.len, 1,
									  1, RUST_FILE, 153, 25);
	frame->parts_live = false;
	std_vec_free_Checked(&frame->parts, 1, 1);
	std_range_to_check(32, frame->swapped.len, RUST_FILE, 154, 54);
	std_u64_to_le_bytes(XXH3_64bits(frame->swapped.ptr, 32), sum);
	std_range_from_check(32, frame->swapped.len, RUST_FILE, 155, 16);
	std_slice_copy_from_slice_Checked(STD_PTR_AT(frame->swapped.ptr, 32, 1),
									  std_wrapping_sub_usize(frame->swapped.len, 32), sum, sizeof(sum), 1, 1, RUST_FILE,
									  155, 23);
	test_chunks_dead_tests_a_damaged_vector_is_refused_closure_0(&refused, &frame->test_io.io, frame->swapped.ptr,
																 frame->swapped.len, 24, order, sizeof(order) - 1);
	frame->swapped_live = false;
	std_vec_free_Checked(&frame->swapped, 1, 1);
	frame->changed_live = false;
	std_vec_free_Checked(&frame->changed, 1, 1);
	frame->written_live = false;
	std_vec_free_Checked(&frame->written, 1, 1);
	frame->path_live = false;
	std_vec_free_Checked(&frame->path.bytes, 1, 1);
	frame->pack_live = false;
	std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	frame->dir_live = false;
	simc_TempDir_drop(&frame->dir);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* The test's pad. */
static void
test_chunks_dead_tests_a_damaged_vector_is_refused_pad_0(void *frame_)
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->parts_live)
		std_vec_free_Checked(&frame->parts, 1, 1);
	if (frame->swapped_live)
		std_vec_free_Checked(&frame->swapped, 1, 1);
	if (frame->changed_live)
		std_vec_free_Checked(&frame->changed, 1, 1);
	if (frame->written_live)
		std_vec_free_Checked(&frame->written, 1, 1);
	if (frame->offsets_live)
		std_btree_BTreeMapU64Set_drop(&frame->offsets);
	if (frame->path_live)
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	if (frame->pack_live)
		std_vec_free_Checked(&frame->pack.bytes, 1, 1);
	if (frame->dir_live)
		simc_TempDir_drop(&frame->dir);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_chunks_dead_tests_a_damaged_vector_is_refused: a vector cut short,
 * one whose bytes changed, and one whose offsets are out of order under a
 * checksum that matches are each refused as damage of the vector's file.
 */
void
test_chunks_dead_tests_a_damaged_vector_is_refused(void)
{
	test_chunks_dead_tests_a_damaged_vector_is_refused_frame_0_t frame = {0};

	std_landing(&frame, test_chunks_dead_tests_a_damaged_vector_is_refused_body_0,
				test_chunks_dead_tests_a_damaged_vector_is_refused_pad_0);
}

#endif							/* DOLTGRES_TEST_CHUNKS */
