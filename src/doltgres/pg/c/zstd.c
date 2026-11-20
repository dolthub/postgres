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
 * zstd's compression, decompression, and dictionary training over the
 * libzstd Postgres links (design item 11), which the server's host and the
 * simulation's both reach through these wrappers alone.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <zdict.h>

#include "doltgres/pg/zstd.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sync.h"
#ifdef DOLTGRES_TEST_PG
#include "doltgres/std/test.h"
#endif
#include "doltgres/std/unwind.h"

#define RUST_FILE "pg" STD_SEP "src" STD_SEP "zstd.rs"

/* pg_zstd_CompressDictionary_new prepares a dictionary's bytes for compressing at the level, or nothing when libzstd can't allocate them. */
bool
pg_zstd_CompressDictionary_new(const uint8 *bytes, size_t bytes_len, int32 level, PgZstdCompressDictionary *result)
{
	ZSTD_CDict *made = ZSTD_createCDict(bytes, bytes_len, level);

	if (made == NULL)
		return false;
	*result = (PgZstdCompressDictionary)
	{
		made
	};
	return true;
}

/* pg_zstd_DecompressDictionary_new prepares a dictionary's bytes for decompressing, or nothing when libzstd can't allocate them. */
bool
pg_zstd_DecompressDictionary_new(const uint8 *bytes, size_t bytes_len, PgZstdDecompressDictionary *result)
{
	ZSTD_DDict *made = ZSTD_createDDict(bytes, bytes_len);

	if (made == NULL)
		return false;
	*result = (PgZstdDecompressDictionary)
	{
		made
	};
	return true;
}

/* pg_zstd_CompressDictionary_drop is CompressDictionary's drop: the dictionary freed. */
void
pg_zstd_CompressDictionary_drop(void *self_)
{
	PgZstdCompressDictionary *self = self_;

	(void) ZSTD_freeCDict(self->f0);
}

/* pg_zstd_DecompressDictionary_drop is DecompressDictionary's drop: the dictionary freed. */
void
pg_zstd_DecompressDictionary_drop(void *self_)
{
	PgZstdDecompressDictionary *self = self_;

	(void) ZSTD_freeDDict(self->f0);
}

/* PG_ZSTD_COMPRESS_DICTIONARY_TYPE_ID is CompressDictionary's identity, which a host's dictionary holds as an Any. */
const StdTypeId PG_ZSTD_COMPRESS_DICTIONARY_TYPE_ID = {
	"pg::zstd::CompressDictionary", sizeof(PgZstdCompressDictionary), _Alignof(PgZstdCompressDictionary),
	pg_zstd_CompressDictionary_drop
};

/* PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID is DecompressDictionary's identity, which a host's dictionary holds as an Any. */
const StdTypeId PG_ZSTD_DECOMPRESS_DICTIONARY_TYPE_ID = {
	"pg::zstd::DecompressDictionary", sizeof(PgZstdDecompressDictionary), _Alignof(PgZstdDecompressDictionary),
	pg_zstd_DecompressDictionary_drop
};

/* PgZstdContextZstdCctx is Context<ZSTD_CCtx>: a compression context the process owns. */
typedef struct PgZstdContextZstdCctx
{
	ZSTD_CCtx  *f0;
} PgZstdContextZstdCctx;

/* PgZstdContextZstdDctx is Context<ZSTD_DCtx>: a decompression context the process owns. */
typedef struct PgZstdContextZstdDctx
{
	ZSTD_DCtx  *f0;
} PgZstdContextZstdDctx;

/* PgZstdOptionContextZstdCctx is Option<Context<ZSTD_CCtx>>, the value COMPRESSOR's RwLock guards. */
typedef struct PgZstdOptionContextZstdCctx
{
	bool		has_value;
	PgZstdContextZstdCctx value;
} PgZstdOptionContextZstdCctx;

/* PgZstdOptionContextZstdDctx is Option<Context<ZSTD_DCtx>>, the value DECOMPRESSOR's RwLock guards. */
typedef struct PgZstdOptionContextZstdDctx
{
	bool		has_value;
	PgZstdContextZstdDctx value;
} PgZstdOptionContextZstdDctx;

/* PG_ZSTD_COMPRESSOR_LOCK is COMPRESSOR's RwLock, which guards PG_ZSTD_COMPRESSOR. */
static StdRwLock PG_ZSTD_COMPRESSOR_LOCK = STD_RW_LOCK_NEW;

/* PG_ZSTD_COMPRESSOR is the process's compression context, made at its first use, None before it. */
static PgZstdOptionContextZstdCctx PG_ZSTD_COMPRESSOR = {false};

/* PG_ZSTD_DECOMPRESSOR_LOCK is DECOMPRESSOR's RwLock, which guards PG_ZSTD_DECOMPRESSOR. */
static StdRwLock PG_ZSTD_DECOMPRESSOR_LOCK = STD_RW_LOCK_NEW;

/* PG_ZSTD_DECOMPRESSOR is the process's decompression context, made at its first use, None before it. */
static PgZstdOptionContextZstdDctx PG_ZSTD_DECOMPRESSOR = {false};

/* pg_zstd_compress_frame_0_t is compress's state across its pad: its arguments, its result, the context's guard, and the frame. */
typedef struct pg_zstd_compress_frame_0_t
{
	const uint8 *bytes;
	size_t		bytes_len;
	int32		level;
	const PgZstdCompressDictionary *dictionary;
	StdVec	   *result;
	bool		done;
	StdRwLockWriteGuard context;
	bool		context_live;
	StdVec		out;
	bool		out_live;
} pg_zstd_compress_frame_0_t;

/* pg_zstd_compress_body_0 is compress's body. */
static void
pg_zstd_compress_body_0(void *frame_)
{
	pg_zstd_compress_frame_0_t *frame = frame_;
	PgZstdOptionContextZstdCctx *context;
	ZSTD_CCtx  *cctx;
	size_t		bound;
	size_t		written;

	(void) std_rw_lock_write(&PG_ZSTD_COMPRESSOR_LOCK, &PG_ZSTD_COMPRESSOR, &frame->context);
	frame->context_live = true;
	context = frame->context.data;
	if (!context->has_value)
	{
		ZSTD_CCtx  *made = ZSTD_createCCtx();

		if (made == NULL)
		{
			frame->done = false;
			frame->context_live = false;
			std_RwLockWriteGuard_drop(&frame->context);
			return;
		}
		context->value = (PgZstdContextZstdCctx)
		{
			made
		};
		context->has_value = true;
	}
	if (!context->has_value)
	{
		frame->done = false;
		frame->context_live = false;
		std_RwLockWriteGuard_drop(&frame->context);
		return;
	}
	cctx = context->value.f0;
	bound = ZSTD_compressBound(frame->bytes_len);
	std_vec_with_capacity_Checked(&frame->out, bound, 1, 1);
	frame->out_live = true;
	if (frame->dictionary != NULL)
		written = ZSTD_compress_usingCDict(cctx, frame->out.ptr, bound, frame->bytes, frame->bytes_len,
										   frame->dictionary->f0);
	else
		written = ZSTD_compressCCtx(cctx, frame->out.ptr, bound, frame->bytes, frame->bytes_len, frame->level);
	if (ZSTD_isError(written) != 0)
	{
		frame->done = false;
		frame->out_live = false;
		std_vec_free_Checked(&frame->out, 1, 1);
		frame->context_live = false;
		std_RwLockWriteGuard_drop(&frame->context);
		return;
	}
	std_vec_set_len_Checked(&frame->out, written);
	frame->out_live = false;
	*frame->result = frame->out;
	frame->done = true;
	frame->context_live = false;
	std_RwLockWriteGuard_drop(&frame->context);
}

/* pg_zstd_compress_pad_0 is compress's pad: the frame, then the context's guard, while each is live. */
static void
pg_zstd_compress_pad_0(void *frame_)
{
	pg_zstd_compress_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, 1, 1);
	if (frame->context_live)
		std_RwLockWriteGuard_drop(&frame->context);
}

/*
 * pg_zstd_compress returns the bytes compressed as one frame that records
 * their length, at the level or with the dictionary, which carries its own,
 * or nothing when libzstd can't allocate its context.
 */
bool
pg_zstd_compress(const uint8 *bytes, size_t bytes_len, int32 level, const PgZstdCompressDictionary *dictionary,
				 StdVec *result)
{
	pg_zstd_compress_frame_0_t frame = {.bytes = bytes, .bytes_len = bytes_len, .level = level,
	.dictionary = dictionary, .result = result};

	std_landing(&frame, pg_zstd_compress_body_0, pg_zstd_compress_pad_0);
	return frame.done;
}

/* pg_zstd_frame_size returns the length a frame records of its bytes, or nothing when it records none or is no frame. */
bool
pg_zstd_frame_size(const uint8 *frame, size_t frame_len, uint64 *result)
{
	unsigned long long size = ZSTD_getFrameContentSize(frame, frame_len);

	if (size < (uint64) ZSTD_CONTENTSIZE_ERROR)
	{
		*result = (uint64) size;
		return true;
	}
	return false;
}

/* pg_zstd_Failed_Debug_fmt is Failed's derived Debug: its variant's name. */
bool
pg_zstd_Failed_Debug_fmt(const void *value, StdFormatter *f)
{
	switch (*(const PgZstdFailed *) value)
	{
		case PG_ZSTD_FAILED_DAMAGED:
			return std_fmt_write_str(f, "Damaged", 7);
		case PG_ZSTD_FAILED_OUT_OF_MEMORY:
			return std_fmt_write_str(f, "OutOfMemory", 11);
	}
	pg_unreachable();
}

/* pg_zstd_decompress_frame_0_t is decompress's state across its pad: its arguments, its result, the context's guard, and the bytes. */
typedef struct pg_zstd_decompress_frame_0_t
{
	const uint8 *frame;
	size_t		frame_len;
	size_t		size;
	const PgZstdDecompressDictionary *dictionary;
	StdVec	   *result;
	PgZstdFailed *error;
	bool		done;
	StdRwLockWriteGuard context;
	bool		context_live;
	StdVec		out;
	bool		out_live;
} pg_zstd_decompress_frame_0_t;

/* pg_zstd_decompress_body_0 is decompress's body. */
static void
pg_zstd_decompress_body_0(void *frame_)
{
	pg_zstd_decompress_frame_0_t *frame = frame_;
	PgZstdOptionContextZstdDctx *context;
	ZSTD_DCtx  *dctx;
	size_t		written;

	(void) std_rw_lock_write(&PG_ZSTD_DECOMPRESSOR_LOCK, &PG_ZSTD_DECOMPRESSOR, &frame->context);
	frame->context_live = true;
	context = frame->context.data;
	if (!context->has_value)
	{
		ZSTD_DCtx  *made = ZSTD_createDCtx();

		if (made == NULL)
		{
			*frame->error = PG_ZSTD_FAILED_OUT_OF_MEMORY;
			frame->done = false;
			frame->context_live = false;
			std_RwLockWriteGuard_drop(&frame->context);
			return;
		}
		context->value = (PgZstdContextZstdDctx)
		{
			made
		};
		context->has_value = true;
	}
	if (!context->has_value)
	{
		*frame->error = PG_ZSTD_FAILED_OUT_OF_MEMORY;
		frame->done = false;
		frame->context_live = false;
		std_RwLockWriteGuard_drop(&frame->context);
		return;
	}
	dctx = context->value.f0;
	std_vec_with_capacity_Checked(&frame->out, frame->size, 1, 1);
	frame->out_live = true;
	if (frame->dictionary != NULL)
		written = ZSTD_decompress_usingDDict(dctx, frame->out.ptr, frame->size, frame->frame, frame->frame_len,
											 frame->dictionary->f0);
	else
		written = ZSTD_decompressDCtx(dctx, frame->out.ptr, frame->size, frame->frame, frame->frame_len);
	if (ZSTD_isError(written) != 0 || written != frame->size)
	{
		*frame->error = PG_ZSTD_FAILED_DAMAGED;
		frame->done = false;
		frame->out_live = false;
		std_vec_free_Checked(&frame->out, 1, 1);
		frame->context_live = false;
		std_RwLockWriteGuard_drop(&frame->context);
		return;
	}
	std_vec_set_len_Checked(&frame->out, written);
	frame->out_live = false;
	*frame->result = frame->out;
	frame->done = true;
	frame->context_live = false;
	std_RwLockWriteGuard_drop(&frame->context);
}

/* pg_zstd_decompress_pad_0 is decompress's pad: the bytes, then the context's guard, while each is live. */
static void
pg_zstd_decompress_pad_0(void *frame_)
{
	pg_zstd_decompress_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, 1, 1);
	if (frame->context_live)
		std_RwLockWriteGuard_drop(&frame->context);
}

/* pg_zstd_decompress returns a frame's size bytes, decompressed with the dictionary if it names one. */
bool
pg_zstd_decompress(const uint8 *frame_, size_t frame_len, size_t size, const PgZstdDecompressDictionary *dictionary,
				   StdVec *result, PgZstdFailed *error)
{
	pg_zstd_decompress_frame_0_t frame = {.frame = frame_, .frame_len = frame_len, .size = size,
	.dictionary = dictionary, .result = result, .error = error};

	std_landing(&frame, pg_zstd_decompress_body_0, pg_zstd_decompress_pad_0);
	return frame.done;
}

/* pg_zstd_train_frame_0_t is train's state across its pad: its arguments, its result, and the vectors it makes. */
typedef struct pg_zstd_train_frame_0_t
{
	const StdBytes *samples;
	size_t		samples_len;
	size_t		capacity;
	StdVec	   *result;
	bool		done;
	StdVec		joined;
	bool		joined_live;
	StdVec		sizes;
	bool		sizes_live;
	StdVec		out;
	bool		out_live;
} pg_zstd_train_frame_0_t;

/* pg_zstd_train_body_0 is train's body. */
static void
pg_zstd_train_body_0(void *frame_)
{
	pg_zstd_train_frame_0_t *frame = frame_;
	StdSizeHint hint;
	uint32		count;
	StdTryFromIntError tried;
	size_t		written;

	std_slice_concat(frame->samples, frame->samples_len, sizeof(StdBytes), 1, 1, &frame->joined);
	frame->joined_live = true;
	hint = (StdSizeHint)
	{
		frame->samples_len, true, frame->samples_len
	};
	std_vec_with_capacity_trusted(&frame->sizes, hint, sizeof(size_t), _Alignof(size_t));
	frame->sizes_live = true;
	std_vec_extend_trusted_reserve(&frame->sizes, hint, sizeof(size_t), _Alignof(size_t));
	for (size_t i = 0, i_len = frame->samples_len; i < i_len; i++)
	{
		STD_VEC_AT(&frame->sizes, size_t, frame->sizes.len) =
			((const StdBytes *) STD_PTR_AT(frame->samples, i, sizeof(StdBytes)))->len;
		frame->sizes.len = std_wrapping_add_usize(frame->sizes.len, 1);
	}
	if (!std_u32_try_from_usize(frame->samples_len, &count, &tried))
		frame->done = false;
	else
	{
		std_vec_with_capacity_Checked(&frame->out, frame->capacity, 1, 1);
		frame->out_live = true;
		written = ZDICT_trainFromBuffer(frame->out.ptr, frame->capacity, frame->joined.ptr, frame->sizes.ptr, count);
		if (ZDICT_isError(written) != 0)
		{
			frame->done = false;
			frame->out_live = false;
			std_vec_free_Checked(&frame->out, 1, 1);
		}
		else
		{
			std_vec_set_len_Checked(&frame->out, written);
			frame->out_live = false;
			*frame->result = frame->out;
			frame->done = true;
		}
	}
	frame->sizes_live = false;
	std_vec_free_Checked(&frame->sizes, sizeof(size_t), _Alignof(size_t));
	frame->joined_live = false;
	std_vec_free_Checked(&frame->joined, 1, 1);
}

/* pg_zstd_train_pad_0 is train's pad: the dictionary, the sizes, and the joined samples, while each is live. */
static void
pg_zstd_train_pad_0(void *frame_)
{
	pg_zstd_train_frame_0_t *frame = frame_;

	if (frame->out_live)
		std_vec_free_Checked(&frame->out, 1, 1);
	if (frame->sizes_live)
		std_vec_free_Checked(&frame->sizes, sizeof(size_t), _Alignof(size_t));
	if (frame->joined_live)
		std_vec_free_Checked(&frame->joined, 1, 1);
}

/*
 * pg_zstd_train returns a dictionary of at most capacity bytes trained on the
 * samples, or nothing when libzstd can't train one on them.
 */
bool
pg_zstd_train(const StdBytes *samples, size_t samples_len, size_t capacity, StdVec *result)
{
	pg_zstd_train_frame_0_t frame = {.samples = samples, .samples_len = samples_len, .capacity = capacity,
	.result = result};

	std_landing(&frame, pg_zstd_train_body_0, pg_zstd_train_pad_0);
	return frame.done;
}

#ifdef DOLTGRES_TEST_PG

/* PgZstdTestsResultRefSliceU8Failed is a Result<&[u8], Failed> as the tests compare it: Ok's bytes or Err's reason. */
typedef struct PgZstdTestsResultRefSliceU8Failed
{
	bool		ok;
	StdBytes	ok_value;
	PgZstdFailed err;
} PgZstdTestsResultRefSliceU8Failed;

/* pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt is Result<&[u8], Failed>'s Debug: Ok's bytes or Err's reason in a tuple. */
static bool
pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt(const void *value, StdFormatter *f)
{
	const PgZstdTestsResultRefSliceU8Failed *self = value;
	StdDebugTuple b;

	if (self->ok)
	{
		b = std_fmt_debug_tuple(f, "Ok", 2);
		std_DebugTuple_field(&b, &self->ok_value, std_bytes_Debug_fmt);
	}
	else
	{
		b = std_fmt_debug_tuple(f, "Err", 3);
		std_DebugTuple_field(&b, &self->err, pg_zstd_Failed_Debug_fmt);
	}
	return std_DebugTuple_finish(&b);
}

/* pg_zstd_tests_ResultRefSliceU8Failed_PartialEq_eq is Result<&[u8], Failed>'s PartialEq: the same variant with equal values. */
static bool
pg_zstd_tests_ResultRefSliceU8Failed_PartialEq_eq(const PgZstdTestsResultRefSliceU8Failed *self, const PgZstdTestsResultRefSliceU8Failed *other)
{
	if (self->ok != other->ok)
		return false;
	if (self->ok)
		return self->ok_value.len == other->ok_value.len &&
			memcmp(self->ok_value.ptr, other->ok_value.ptr, self->ok_value.len) == 0;
	return self->err == other->err;
}

/* PgZstdTestsResultVecU8Failed is a Result<Vec<u8>, Failed> as the tests compare it: Ok's bytes or Err's reason. */
typedef struct PgZstdTestsResultVecU8Failed
{
	bool		ok;
	StdVec		ok_value;
	PgZstdFailed err;
} PgZstdTestsResultVecU8Failed;

/* pg_zstd_tests_VecU8_Debug_fmt is Vec<u8>'s Debug, its slice's. */
static bool
pg_zstd_tests_VecU8_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *self = value;
	StdBytes	bytes = {self->ptr, self->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* pg_zstd_tests_ResultVecU8Failed_Debug_fmt is Result<Vec<u8>, Failed>'s Debug: Ok's bytes or Err's reason in a tuple. */
static bool
pg_zstd_tests_ResultVecU8Failed_Debug_fmt(const void *value, StdFormatter *f)
{
	const PgZstdTestsResultVecU8Failed *self = value;
	StdDebugTuple b;

	if (self->ok)
	{
		b = std_fmt_debug_tuple(f, "Ok", 2);
		std_DebugTuple_field(&b, &self->ok_value, pg_zstd_tests_VecU8_Debug_fmt);
	}
	else
	{
		b = std_fmt_debug_tuple(f, "Err", 3);
		std_DebugTuple_field(&b, &self->err, pg_zstd_Failed_Debug_fmt);
	}
	return std_DebugTuple_finish(&b);
}

/* pg_zstd_tests_ResultVecU8Failed_PartialEq_eq is Result<Vec<u8>, Failed>'s PartialEq: the same variant with equal values. */
static bool
pg_zstd_tests_ResultVecU8Failed_PartialEq_eq(const PgZstdTestsResultVecU8Failed *self,
											 const PgZstdTestsResultVecU8Failed *other)
{
	if (self->ok != other->ok)
		return false;
	if (self->ok)
		return self->ok_value.len == other->ok_value.len &&
			memcmp(self->ok_value.ptr, other->ok_value.ptr, self->ok_value.len) == 0;
	return self->err == other->err;
}

/* pg_zstd_tests_VecU8_drop is a Vec<u8>'s drop glue, an element of the samples. */
static void
pg_zstd_tests_VecU8_drop(void *elem)
{
	std_vec_free_Checked(elem, 1, 1);
}

/* pg_zstd_tests_samples_closure_0_frame_0_t is the samples' closure's state across its pad: its argument, its result, and the row it formats. */
typedef struct pg_zstd_tests_samples_closure_0_frame_0_t
{
	int32		n;
	StdVec	   *result;
	StdString	shown;
	bool		shown_live;
} pg_zstd_tests_samples_closure_0_frame_0_t;

/* pg_zstd_tests_samples_closure_0_body_0 is the samples' closure's body. */
static void
pg_zstd_tests_samples_closure_0_body_0(void *frame_)
{
	pg_zstd_tests_samples_closure_0_frame_0_t *frame = frame_;
	StdFmtPiece pieces[] = {STD_FMT_LIT("{\"id\": "), STD_FMT_ARG(0), STD_FMT_LIT(", \"name\": \"row "), STD_FMT_ARG(1),
	STD_FMT_LIT("\", \"kind\": \"versioned\"}")};
	StdFmtArg	args[] = {{&frame->n, std_i32_Display_fmt}, {&frame->n, std_i32_Display_fmt}};
	StdFmtArguments arguments = {pieces, 5, args};

	std_fmt_format_Checked(&frame->shown, &arguments);
	frame->shown_live = true;
	std_str_repeat((const char *) frame->shown.ptr, frame->shown.len, 8, frame->result);
	frame->shown_live = false;
	std_vec_free_Checked(&frame->shown, 1, 1);
}

/* pg_zstd_tests_samples_closure_0_pad_0 is the samples' closure's pad: the formatted row while it is live. */
static void
pg_zstd_tests_samples_closure_0_pad_0(void *frame_)
{
	pg_zstd_tests_samples_closure_0_frame_0_t *frame = frame_;

	if (frame->shown_live)
		std_vec_free_Checked(&frame->shown, 1, 1);
}

/* pg_zstd_tests_samples_closure_0 is a row's sample: its JSON repeated eight times, as bytes. */
static void
pg_zstd_tests_samples_closure_0(int32 n, StdVec *result)
{
	pg_zstd_tests_samples_closure_0_frame_0_t frame = {.n = n, .result = result};

	std_landing(&frame, pg_zstd_tests_samples_closure_0_body_0, pg_zstd_tests_samples_closure_0_pad_0);
}

/* pg_zstd_tests_samples_frame_0_t is samples' state across its pad: its result while it is collected. */
typedef struct pg_zstd_tests_samples_frame_0_t
{
	StdVec	   *result;
	bool		result_live;
} pg_zstd_tests_samples_frame_0_t;

/* pg_zstd_tests_samples_body_0 is samples' body. */
static void
pg_zstd_tests_samples_body_0(void *frame_)
{
	pg_zstd_tests_samples_frame_0_t *frame = frame_;
	StdSizeHint hint = {256, true, 256};

	std_vec_with_capacity_trusted(frame->result, hint, sizeof(StdVec), _Alignof(StdVec));
	frame->result_live = true;
	std_vec_extend_trusted_reserve(frame->result, hint, sizeof(StdVec), _Alignof(StdVec));
	for (int32 n = 0; n < 256; n = std_forward_unchecked_i32_Checked(n, 1))
	{
		pg_zstd_tests_samples_closure_0(n, &STD_VEC_AT(frame->result, StdVec, frame->result->len));
		frame->result->len = std_wrapping_add_usize(frame->result->len, 1);
	}
	frame->result_live = false;
}

/* pg_zstd_tests_samples_pad_0 is samples' pad: the samples collected so far, while they are live. */
static void
pg_zstd_tests_samples_pad_0(void *frame_)
{
	pg_zstd_tests_samples_frame_0_t *frame = frame_;

	if (frame->result_live)
		std_vec_drop_Checked(frame->result, sizeof(StdVec), _Alignof(StdVec), pg_zstd_tests_VecU8_drop);
}

/* pg_zstd_tests_samples returns 256 rows' JSON, each repeated eight times, the kind of leaf zstd trains on. */
static void
pg_zstd_tests_samples(StdVec *result)
{
	pg_zstd_tests_samples_frame_0_t frame = {.result = result};

	std_landing(&frame, pg_zstd_tests_samples_body_0, pg_zstd_tests_samples_pad_0);
}

/* test_pg_zstd_tests_compression_round_trips_frame_0_t is the test's state across its pad: the vectors it makes. */
typedef struct test_pg_zstd_tests_compression_round_trips_frame_0_t
{
	StdVec		bytes;
	bool		bytes_live;
	StdVec		frame;
	bool		frame_live;
	StdVec		got;
	bool		got_live;
	StdVec		empty;
	bool		empty_live;
} test_pg_zstd_tests_compression_round_trips_frame_0_t;

/* test_pg_zstd_tests_compression_round_trips_body_0 is the test's body. */
static void
test_pg_zstd_tests_compression_round_trips_body_0(void *frame_)
{
	test_pg_zstd_tests_compression_round_trips_frame_0_t *frame = frame_;

	std_slice_repeat("versioned ", 10, 1, 1, 100, &frame->bytes);
	frame->bytes_live = true;
	if (!pg_zstd_compress(frame->bytes.ptr, frame->bytes.len, 3, NULL, &frame->frame))
		STD_OPTION_EXPECT_FAILED(230, 47, "compresses");
	frame->frame_live = true;
	STD_ASSERT(231, 9, frame->frame.len < frame->bytes.len, "frame.len() < bytes.len()");
	{
		uint64		size;
		bool		has = pg_zstd_frame_size(frame->frame.ptr, frame->frame.len, &size);
		uint64		expected = 1000;
		StdOptionRef left = {has, {&size, std_u64_Debug_fmt}};
		StdOptionRef right = {true, {&expected, std_u64_Debug_fmt}};

		STD_ASSERT_EQ(232, 9, has && size == expected, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	{
		PgZstdTestsResultRefSliceU8Failed left = {0};
		PgZstdTestsResultRefSliceU8Failed right = {true, {frame->bytes.ptr, frame->bytes.len}};

		left.ok = pg_zstd_decompress(frame->frame.ptr, frame->frame.len, 1000, NULL, &frame->got, &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = (StdBytes)
			{
				frame->got.ptr, frame->got.len
			};
		}
		STD_ASSERT_EQ(233, 9, pg_zstd_tests_ResultRefSliceU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	if (!pg_zstd_compress((const uint8 *) "", 0, 3, NULL, &frame->empty))
		STD_OPTION_EXPECT_FAILED(234, 44, "compresses");
	frame->empty_live = true;
	{
		PgZstdTestsResultRefSliceU8Failed left = {0};
		PgZstdTestsResultRefSliceU8Failed right = {true, {(const uint8 *) "", 0}};

		left.ok = pg_zstd_decompress(frame->empty.ptr, frame->empty.len, 0, NULL, &frame->got, &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = (StdBytes)
			{
				frame->got.ptr, frame->got.len
			};
		}
		STD_ASSERT_EQ(235, 9, pg_zstd_tests_ResultRefSliceU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	frame->empty_live = false;
	std_vec_free_Checked(&frame->empty, 1, 1);
	frame->frame_live = false;
	std_vec_free_Checked(&frame->frame, 1, 1);
	frame->bytes_live = false;
	std_vec_free_Checked(&frame->bytes, 1, 1);
}

/* test_pg_zstd_tests_compression_round_trips_pad_0 is the test's pad: each vector while it is live. */
static void
test_pg_zstd_tests_compression_round_trips_pad_0(void *frame_)
{
	test_pg_zstd_tests_compression_round_trips_frame_0_t *frame = frame_;

	if (frame->got_live)
		std_vec_free_Checked(&frame->got, 1, 1);
	if (frame->empty_live)
		std_vec_free_Checked(&frame->empty, 1, 1);
	if (frame->frame_live)
		std_vec_free_Checked(&frame->frame, 1, 1);
	if (frame->bytes_live)
		std_vec_free_Checked(&frame->bytes, 1, 1);
}

/*
 * test_pg_zstd_tests_compression_round_trips: a frame decompresses to the
 * bytes compressed into it and records their length, the empty bytes too.
 */
void
test_pg_zstd_tests_compression_round_trips(void)
{
	test_pg_zstd_tests_compression_round_trips_frame_0_t frame = {0};

	std_landing(&frame, test_pg_zstd_tests_compression_round_trips_body_0,
				test_pg_zstd_tests_compression_round_trips_pad_0);
}

/* test_pg_zstd_tests_damaged_frames_record_no_size_frame_0_t is the test's state across its pad: the vectors it makes. */
typedef struct test_pg_zstd_tests_damaged_frames_record_no_size_frame_0_t
{
	StdVec		repeated;
	bool		repeated_live;
	StdVec		frame;
	bool		frame_live;
} test_pg_zstd_tests_damaged_frames_record_no_size_frame_0_t;

/* test_pg_zstd_tests_damaged_frames_record_no_size_body_0 is the test's body. */
static void
test_pg_zstd_tests_damaged_frames_record_no_size_body_0(void *frame_)
{
	test_pg_zstd_tests_damaged_frames_record_no_size_frame_0_t *frame = frame_;

	std_slice_repeat("versioned ", 10, 1, 1, 100, &frame->repeated);
	frame->repeated_live = true;
	if (!pg_zstd_compress(frame->repeated.ptr, frame->repeated.len, 3, NULL, &frame->frame))
		STD_OPTION_EXPECT_FAILED(242, 67, "compresses");
	frame->frame_live = true;
	frame->repeated_live = false;
	std_vec_free_Checked(&frame->repeated, 1, 1);
	{
		uint64		size;
		bool		has = pg_zstd_frame_size((const uint8 *) "not a frame", 11, &size);
		StdOptionRef left = {has, {&size, std_u64_Debug_fmt}};
		StdOptionRef right = {false};

		STD_ASSERT_EQ(243, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	std_range_to_check(3, frame->frame.len, RUST_FILE, 244, 37);
	{
		uint64		size;
		bool		has = pg_zstd_frame_size(frame->frame.ptr, 3, &size);
		StdOptionRef left = {has, {&size, std_u64_Debug_fmt}};
		StdOptionRef right = {false};

		STD_ASSERT_EQ(244, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	{
		uint64		size;
		bool		has = pg_zstd_frame_size((const uint8 *) STD_DANGLING, 0, &size);
		StdOptionRef left = {has, {&size, std_u64_Debug_fmt}};
		StdOptionRef right = {false};

		STD_ASSERT_EQ(245, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	frame->frame_live = false;
	std_vec_free_Checked(&frame->frame, 1, 1);
}

/* test_pg_zstd_tests_damaged_frames_record_no_size_pad_0 is the test's pad: each vector while it is live. */
static void
test_pg_zstd_tests_damaged_frames_record_no_size_pad_0(void *frame_)
{
	test_pg_zstd_tests_damaged_frames_record_no_size_frame_0_t *frame = frame_;

	if (frame->frame_live)
		std_vec_free_Checked(&frame->frame, 1, 1);
	if (frame->repeated_live)
		std_vec_free_Checked(&frame->repeated, 1, 1);
}

/* test_pg_zstd_tests_damaged_frames_record_no_size: bytes that are no frame, or a frame's first bytes alone, record no size. */
void
test_pg_zstd_tests_damaged_frames_record_no_size(void)
{
	test_pg_zstd_tests_damaged_frames_record_no_size_frame_0_t frame = {0};

	std_landing(&frame, test_pg_zstd_tests_damaged_frames_record_no_size_body_0,
				test_pg_zstd_tests_damaged_frames_record_no_size_pad_0);
}

/* test_pg_zstd_tests_damaged_frames_fail_as_damaged_frame_0_t is the test's state across its pad: the vectors it makes. */
typedef struct test_pg_zstd_tests_damaged_frames_fail_as_damaged_frame_0_t
{
	StdVec		repeated;
	bool		repeated_live;
	StdVec		frame;
	bool		frame_live;
	StdVec		got;
	bool		got_live;
	StdVec		flipped;
	bool		flipped_live;
} test_pg_zstd_tests_damaged_frames_fail_as_damaged_frame_0_t;

/* test_pg_zstd_tests_damaged_frames_fail_as_damaged_body_0 is the test's body. */
static void
test_pg_zstd_tests_damaged_frames_fail_as_damaged_body_0(void *frame_)
{
	test_pg_zstd_tests_damaged_frames_fail_as_damaged_frame_0_t *frame = frame_;
	size_t		end;
	size_t		last;

	std_slice_repeat("versioned ", 10, 1, 1, 100, &frame->repeated);
	frame->repeated_live = true;
	if (!pg_zstd_compress(frame->repeated.ptr, frame->repeated.len, 3, NULL, &frame->frame))
		STD_OPTION_EXPECT_FAILED(252, 67, "compresses");
	frame->frame_live = true;
	frame->repeated_live = false;
	std_vec_free_Checked(&frame->repeated, 1, 1);
	end = STD_SUB(usize, frame->frame.len, 1, 253, 40);
	std_range_to_check(end, frame->frame.len, RUST_FILE, 253, 37);
	{
		PgZstdTestsResultVecU8Failed left = {0};
		PgZstdTestsResultVecU8Failed right = {false, {0}, PG_ZSTD_FAILED_DAMAGED};

		left.ok = pg_zstd_decompress(frame->frame.ptr, end, 1000, NULL, &frame->got, &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = frame->got;
		}
		STD_ASSERT_EQ(253, 9, pg_zstd_tests_ResultVecU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultVecU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultVecU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	{
		PgZstdTestsResultVecU8Failed left = {0};
		PgZstdTestsResultVecU8Failed right = {false, {0}, PG_ZSTD_FAILED_DAMAGED};

		left.ok = pg_zstd_decompress(frame->frame.ptr, frame->frame.len, 999, NULL, &frame->got, &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = frame->got;
		}
		STD_ASSERT_EQ(254, 9, pg_zstd_tests_ResultVecU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultVecU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultVecU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	std_vec_clone(&frame->flipped, &frame->frame, 1, 1);
	frame->flipped_live = true;
	last = STD_SUB(usize, frame->flipped.len, 1, 256, 20);
	std_index_check(last, frame->flipped.len, RUST_FILE, 257, 9);
	STD_VEC_AT(&frame->flipped, uint8, last) ^= 0xff;
	{
		PgZstdTestsResultVecU8Failed left = {0};
		PgZstdTestsResultVecU8Failed right = {false, {0}, PG_ZSTD_FAILED_DAMAGED};

		left.ok = pg_zstd_decompress(frame->flipped.ptr, frame->flipped.len, 1000, NULL, &frame->got, &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = frame->got;
		}
		STD_ASSERT_EQ(258, 9, pg_zstd_tests_ResultVecU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultVecU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultVecU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	frame->flipped_live = false;
	std_vec_free_Checked(&frame->flipped, 1, 1);
	frame->frame_live = false;
	std_vec_free_Checked(&frame->frame, 1, 1);
}

/* test_pg_zstd_tests_damaged_frames_fail_as_damaged_pad_0 is the test's pad: each vector while it is live. */
static void
test_pg_zstd_tests_damaged_frames_fail_as_damaged_pad_0(void *frame_)
{
	test_pg_zstd_tests_damaged_frames_fail_as_damaged_frame_0_t *frame = frame_;

	if (frame->got_live)
		std_vec_free_Checked(&frame->got, 1, 1);
	if (frame->flipped_live)
		std_vec_free_Checked(&frame->flipped, 1, 1);
	if (frame->frame_live)
		std_vec_free_Checked(&frame->frame, 1, 1);
	if (frame->repeated_live)
		std_vec_free_Checked(&frame->repeated, 1, 1);
}

/*
 * test_pg_zstd_tests_damaged_frames_fail_as_damaged: a cut frame, a frame
 * asked for the wrong size, and a frame with a flipped byte fail as damaged.
 */
void
test_pg_zstd_tests_damaged_frames_fail_as_damaged(void)
{
	test_pg_zstd_tests_damaged_frames_fail_as_damaged_frame_0_t frame = {0};

	std_landing(&frame, test_pg_zstd_tests_damaged_frames_fail_as_damaged_body_0,
				test_pg_zstd_tests_damaged_frames_fail_as_damaged_pad_0);
}

/* test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_frame_0_t is the test's state across its pad: what it makes. */
typedef struct test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_frame_0_t
{
	StdVec		samples;
	bool		samples_live;
	StdVec		slices;
	bool		slices_live;
	StdVec		trained;
	bool		trained_live;
	PgZstdCompressDictionary compressing;
	bool		compressing_live;
	PgZstdDecompressDictionary decompressing;
	bool		decompressing_live;
	StdVec		frame;
	bool		frame_live;
	StdVec		plain;
	bool		plain_live;
	StdVec		got;
	bool		got_live;
} test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_frame_0_t;

/* test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_body_0 is the test's body. */
static void
test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_body_0(void *frame_)
{
	test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_frame_0_t *frame = frame_;
	StdSizeHint hint;
	const StdVec *leaf;
	bool		shorter;

	pg_zstd_tests_samples(&frame->samples);
	frame->samples_live = true;
	hint = (StdSizeHint)
	{
		frame->samples.len, true, frame->samples.len
	};
	std_vec_with_capacity_trusted(&frame->slices, hint, sizeof(StdBytes), _Alignof(StdBytes));
	frame->slices_live = true;
	std_vec_extend_trusted_reserve(&frame->slices, hint, sizeof(StdBytes), _Alignof(StdBytes));
	for (size_t i = 0, i_len = frame->samples.len; i < i_len; i++)
	{
		const StdVec *sample = &STD_VEC_AT(&frame->samples, StdVec, i);

		STD_VEC_AT(&frame->slices, StdBytes, frame->slices.len) = (StdBytes)
		{
			sample->ptr, sample->len
		};
		frame->slices.len = std_wrapping_add_usize(frame->slices.len, 1);
	}
	if (!pg_zstd_train(frame->slices.ptr, frame->slices.len, ((size_t) (16 << 10)), &frame->trained))
		STD_OPTION_EXPECT_FAILED(266, 95, "trains");
	frame->trained_live = true;
	frame->slices_live = false;
	std_vec_free_Checked(&frame->slices, sizeof(StdBytes), _Alignof(StdBytes));
	if (!pg_zstd_CompressDictionary_new(frame->trained.ptr, frame->trained.len, 3, &frame->compressing))
		STD_OPTION_EXPECT_FAILED(268, 50, "prepares");
	frame->compressing_live = true;
	if (!pg_zstd_DecompressDictionary_new(frame->trained.ptr, frame->trained.len, &frame->decompressing))
		STD_OPTION_EXPECT_FAILED(269, 49, "prepares");
	frame->decompressing_live = true;
	std_index_check(7, frame->samples.len, RUST_FILE, 271, 21);
	leaf = &STD_VEC_AT(&frame->samples, StdVec, 7);
	if (!pg_zstd_compress(leaf->ptr, leaf->len, 3, &frame->compressing, &frame->frame))
		STD_OPTION_EXPECT_FAILED(272, 59, "compresses");
	frame->frame_live = true;
	if (!pg_zstd_compress(leaf->ptr, leaf->len, 3, NULL, &frame->plain))
		STD_OPTION_EXPECT_FAILED(273, 55, "compresses");
	frame->plain_live = true;
	shorter = frame->frame.len < frame->plain.len;
	frame->plain_live = false;
	std_vec_free_Checked(&frame->plain, 1, 1);
	STD_ASSERT(273, 9, shorter, "frame.len() < compress(leaf, 3, None).expect(\"compresses\").len()");
	{
		PgZstdTestsResultRefSliceU8Failed left = {0};
		PgZstdTestsResultRefSliceU8Failed right = {true, {leaf->ptr, leaf->len}};

		left.ok = pg_zstd_decompress(frame->frame.ptr, frame->frame.len, leaf->len, &frame->decompressing, &frame->got,
									 &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = (StdBytes)
			{
				frame->got.ptr, frame->got.len
			};
		}
		STD_ASSERT_EQ(274, 9, pg_zstd_tests_ResultRefSliceU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultRefSliceU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	{
		PgZstdTestsResultVecU8Failed left = {0};
		PgZstdTestsResultVecU8Failed right = {false, {0}, PG_ZSTD_FAILED_DAMAGED};

		left.ok = pg_zstd_decompress(frame->frame.ptr, frame->frame.len, leaf->len, NULL, &frame->got, &left.err);
		if (left.ok)
		{
			frame->got_live = true;
			left.ok_value = frame->got;
		}
		STD_ASSERT_EQ(275, 9, pg_zstd_tests_ResultVecU8Failed_PartialEq_eq(&left, &right), &left,
					  pg_zstd_tests_ResultVecU8Failed_Debug_fmt, &right, pg_zstd_tests_ResultVecU8Failed_Debug_fmt);
		if (frame->got_live)
		{
			frame->got_live = false;
			std_vec_free_Checked(&frame->got, 1, 1);
		}
	}
	frame->frame_live = false;
	std_vec_free_Checked(&frame->frame, 1, 1);
	frame->decompressing_live = false;
	pg_zstd_DecompressDictionary_drop(&frame->decompressing);
	frame->compressing_live = false;
	pg_zstd_CompressDictionary_drop(&frame->compressing);
	frame->trained_live = false;
	std_vec_free_Checked(&frame->trained, 1, 1);
	frame->samples_live = false;
	std_vec_drop_Checked(&frame->samples, sizeof(StdVec), _Alignof(StdVec), pg_zstd_tests_VecU8_drop);
}

/* test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_pad_0 is the test's pad: what it made, while each is live. */
static void
test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_pad_0(void *frame_)
{
	test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_frame_0_t *frame = frame_;

	if (frame->got_live)
		std_vec_free_Checked(&frame->got, 1, 1);
	if (frame->plain_live)
		std_vec_free_Checked(&frame->plain, 1, 1);
	if (frame->frame_live)
		std_vec_free_Checked(&frame->frame, 1, 1);
	if (frame->decompressing_live)
		pg_zstd_DecompressDictionary_drop(&frame->decompressing);
	if (frame->compressing_live)
		pg_zstd_CompressDictionary_drop(&frame->compressing);
	if (frame->trained_live)
		std_vec_free_Checked(&frame->trained, 1, 1);
	if (frame->slices_live)
		std_vec_free_Checked(&frame->slices, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->samples_live)
		std_vec_drop_Checked(&frame->samples, sizeof(StdVec), _Alignof(StdVec), pg_zstd_tests_VecU8_drop);
}

/*
 * test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses: a trained
 * dictionary compresses a leaf smaller than no dictionary does, and its frame
 * decompresses only with the dictionary.
 */
void
test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses(void)
{
	test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_frame_0_t frame = {0};

	std_landing(&frame, test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_body_0,
				test_pg_zstd_tests_a_dictionary_round_trips_what_it_compresses_pad_0);
}

/* test_pg_zstd_tests_too_few_samples_train_nothing_frame_0_t is the test's state across its pad: the dictionary if one is trained. */
typedef struct test_pg_zstd_tests_too_few_samples_train_nothing_frame_0_t
{
	StdVec		trained;
	bool		trained_live;
} test_pg_zstd_tests_too_few_samples_train_nothing_frame_0_t;

/* test_pg_zstd_tests_too_few_samples_train_nothing_body_0 is the test's body. */
static void
test_pg_zstd_tests_too_few_samples_train_nothing_body_0(void *frame_)
{
	test_pg_zstd_tests_too_few_samples_train_nothing_frame_0_t *frame = frame_;
	StdBytes	samples[] = {{(const uint8 *) "one sample", 10}};
	bool		has = pg_zstd_train(samples, lengthof(samples), ((size_t) (16 << 10)), &frame->trained);
	StdBytes	shown;
	StdOptionRef left = {has, {&shown, std_bytes_Debug_fmt}};
	StdOptionRef right = {false};

	if (has)
	{
		frame->trained_live = true;
		shown = (StdBytes)
		{
			frame->trained.ptr, frame->trained.len
		};
	}
	STD_ASSERT_EQ(282, 9, !has, &left, std_option_Debug_fmt, &right, std_option_Debug_fmt);
}

/* test_pg_zstd_tests_too_few_samples_train_nothing_pad_0 is the test's pad: the dictionary while it is live. */
static void
test_pg_zstd_tests_too_few_samples_train_nothing_pad_0(void *frame_)
{
	test_pg_zstd_tests_too_few_samples_train_nothing_frame_0_t *frame = frame_;

	if (frame->trained_live)
		std_vec_free_Checked(&frame->trained, 1, 1);
}

/* test_pg_zstd_tests_too_few_samples_train_nothing: too few samples train no dictionary. */
void
test_pg_zstd_tests_too_few_samples_train_nothing(void)
{
	test_pg_zstd_tests_too_few_samples_train_nothing_frame_0_t frame = {0};

	std_landing(&frame, test_pg_zstd_tests_too_few_samples_train_nothing_body_0,
				test_pg_zstd_tests_too_few_samples_train_nothing_pad_0);
}

#endif							/* DOLTGRES_TEST_PG */
