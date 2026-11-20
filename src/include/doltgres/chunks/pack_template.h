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
 * The template of chunks' pack.rs PackWriter<F>, which holds its file F by
 * value (the port text's section 2). An instance defines its parameters and
 * includes this file, and every parameter is undefined at the end.
 *
 * CHUNKS_PACK_SCOPE is static for an instance one file alone uses, and
 * extern for one defined in one file and declared in others.
 * CHUNKS_PACK_DECLARE generates the instance's type and declarations, and
 * CHUNKS_PACK_DEFINE its definitions, which pack.c includes after the
 * helpers they call.
 * CHUNKS_PACK_FS spells F, and CHUNKS_PACK_F is F's C form; for the
 * definitions, CHUNKS_PACK_F_OPS is F's PackFile and
 * CHUNKS_PACK_F_DROP(place) drops an F there.
 */

/* CHUNKS_PACK_CAT pastes two tokens after expanding them. */
#define CHUNKS_PACK_CAT_(a, b) a##b
#define CHUNKS_PACK_CAT(a, b) CHUNKS_PACK_CAT_(a, b)

/* CHUNKS_PACK_TYPE is PackWriter<F>, and CHUNKS_PACK_NAME names its methods. */
#define CHUNKS_PACK_TYPE CHUNKS_PACK_CAT(ChunksPackPackWriter, CHUNKS_PACK_FS)
#define CHUNKS_PACK_NAME(name) CHUNKS_PACK_CAT(CHUNKS_PACK_CAT(chunks_pack_PackWriter, CHUNKS_PACK_FS), _##name)

#ifdef CHUNKS_PACK_DECLARE

/* PackWriter<F> is a writer of a pack, which writes its records to F. */
typedef struct CHUNKS_PACK_TYPE
{
	ChunksPackPackId id;
	uint64		number;
	CHUNKS_PACK_F file;
	StdVec		header;

	/*
	 * flushed is the pack offset of the buffer's first byte, up to which the
	 * file is written.
	 */
	uint64		flushed;
	StdVec		buffer;
	StdVec		entries;

	/*
	 * samples are the leaves collected to train the pack's own dictionary on,
	 * until it is trained, none for a pack that trains none.
	 */
	bool		has_samples;
	StdVec		samples;

	/*
	 * dictionaries are the pack's dictionaries by slot, each prepared for
	 * reading back what the pack holds: those carried with records copied
	 * from other packs and its own once trained.
	 */
	StdVec		dictionaries;

	/*
	 * own is the slot of the pack's own dictionary and its form prepared for
	 * compressing, once trained.
	 */
	bool		has_own;
	host_TupleUsizeCompressDictionary_t own;

	/*
	 * shared is the dictionary the pack takes as its own from the packs it
	 * shares one with, rather than training its own.
	 */
	bool		has_shared;
	ChunksPackSharedDictionary shared;

	/*
	 * repo is the id of the packs of the repository the pack is written for,
	 * which tags its trace events.
	 */
	uint64		repo;

	/*
	 * failed is the error of a write to the file that failed, after which no
	 * write or finish goes on, since what the file holds is unknown.
	 */
	bool		has_failed;
	StdString	failed;
}			CHUNKS_PACK_TYPE;

CHUNKS_PACK_SCOPE void CHUNKS_PACK_NAME(train) (CHUNKS_PACK_TYPE * self);
CHUNKS_PACK_SCOPE void CHUNKS_PACK_NAME(share) (CHUNKS_PACK_TYPE * self, ChunksPackSharedDictionary *shared);
CHUNKS_PACK_SCOPE ChunksPackPackId CHUNKS_PACK_NAME(id) (const CHUNKS_PACK_TYPE * self);
CHUNKS_PACK_SCOPE bool CHUNKS_PACK_NAME(is_empty) (const CHUNKS_PACK_TYPE * self);
CHUNKS_PACK_SCOPE uint64 CHUNKS_PACK_NAME(number) (const CHUNKS_PACK_TYPE * self);
CHUNKS_PACK_SCOPE uint64 CHUNKS_PACK_NAME(size) (const CHUNKS_PACK_TYPE * self);
CHUNKS_PACK_SCOPE bool CHUNKS_PACK_NAME(write) (CHUNKS_PACK_TYPE * self, HostIoIo *io, ChunksAddressAddress address,
												const uint8 *stored, size_t stored_len, bool leaf,
												ChunksTrailerLocator *result, ChunksErrorError *error);
CHUNKS_PACK_SCOPE bool CHUNKS_PACK_NAME(copy) (CHUNKS_PACK_TYPE * self, HostIoIo *io, ChunksAddressAddress address,
											   const ChunksPackRaw *raw, const uint8 *dictionary,
											   size_t dictionary_len, bool *has_result,
											   ChunksTrailerLocator *result, ChunksErrorError *error);
CHUNKS_PACK_SCOPE bool CHUNKS_PACK_NAME(read) (const CHUNKS_PACK_TYPE * self, HostIoIo *io,
											   ChunksTrailerLocator locator, const ChunksAddressAddress *address,
											   ChunksChunkChunk *result, ChunksErrorError *error);
CHUNKS_PACK_SCOPE void CHUNKS_PACK_NAME(drop) (CHUNKS_PACK_TYPE * self);

#endif							/* CHUNKS_PACK_DECLARE */

#ifdef CHUNKS_PACK_DEFINE

/* CHUNKS_PACK_DEF is CHUNKS_PACK_SCOPE's storage class for a definition: static, or none for extern. */
#define CHUNKS_PACK_DEF_static static
#define CHUNKS_PACK_DEF_extern
#define CHUNKS_PACK_DEF CHUNKS_PACK_CAT(CHUNKS_PACK_DEF_, CHUNKS_PACK_SCOPE)

/* CHUNKS_PACK_FILE_OF is the File F derefs to, through CHUNKS_PACK_F_OPS, F's PackFile. */
#define CHUNKS_PACK_FILE_OF(self) ((const HostFile *) CHUNKS_PACK_F_OPS->deref.deref(&(self)->file))

/* CHUNKS_PACK_OVER_FRAME is over's state across its pad: the header, while it owns it. */
#define CHUNKS_PACK_OVER_FRAME CHUNKS_PACK_CAT(chunks_pack_over_frame_, CHUNKS_PACK_FS)
typedef struct CHUNKS_PACK_OVER_FRAME
{
	CHUNKS_PACK_F *file;
	uint64		number;
	ChunksPackPackId id;
	CHUNKS_PACK_TYPE *result;
	StdVec		header;
	bool		header_live;
}			CHUNKS_PACK_OVER_FRAME;

/* over_pad_0 is over's pad: the header, while it owns it. */
static void
CHUNKS_PACK_NAME(over_pad_0) (void *frame_)
{
	CHUNKS_PACK_OVER_FRAME *frame = frame_;

	if (frame->header_live)
	{
		frame->header_live = false;
		std_vec_free_Checked(&frame->header, 1, 1);
	}
}

/* over_body_0 is over's body: the header, its copy as the buffer's start, and the writer, which takes the file last. */
static void
CHUNKS_PACK_NAME(over_body_0) (void *frame_)
{
	CHUNKS_PACK_OVER_FRAME *frame = frame_;
	CHUNKS_PACK_TYPE *result = frame->result;

	std_slice_to_vec_Checked(&frame->header, CHUNKS_PACK_MAGIC, sizeof(CHUNKS_PACK_MAGIC), 1, 1);
	frame->header_live = true;
	std_vec_extend_from_slice_Checked(&frame->header, frame->id.f0, sizeof(frame->id.f0), 1, 1);
	std_vec_u8_push_Checked(&frame->header, CHUNKS_PACK_ZSTD);
	std_slice_to_vec_Checked(&result->buffer, frame->header.ptr, frame->header.len, 1, 1);
	result->header = frame->header;
	frame->header_live = false;
	result->id = frame->id;
	result->number = frame->number;
	result->flushed = 0;
	std_vec_new(&result->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	result->has_samples = false;
	std_vec_new(&result->dictionaries, sizeof(chunks_TupleVecU8DecompressDictionary_t),
				_Alignof(chunks_TupleVecU8DecompressDictionary_t));
	result->has_own = false;
	result->has_shared = false;
	result->repo = 0;
	result->has_failed = false;
	result->file = *frame->file;
}

/*
 * over starts a pack in a file, which it takes only once nothing else it
 * does can fail, so the caller still owns it should over unwind.
 */
static void
CHUNKS_PACK_NAME(over) (CHUNKS_PACK_F * file, uint64 number, ChunksPackPackId id, CHUNKS_PACK_TYPE * result)
{
	CHUNKS_PACK_OVER_FRAME frame = {.file = file, .number = number, .id = id, .result = result};

	std_landing(&frame, CHUNKS_PACK_NAME(over_body_0), CHUNKS_PACK_NAME(over_pad_0));
}

/* train makes the pack train a dictionary of its own on its first leaves once it holds TRAIN_AFTER bytes. */
CHUNKS_PACK_DEF void
CHUNKS_PACK_NAME(train) (CHUNKS_PACK_TYPE * self)
{
	if (!self->has_own && !self->has_samples)
	{
		std_vec_new(&self->samples, sizeof(StdVec), _Alignof(StdVec));
		self->has_samples = true;
	}
}

/* share makes the pack take its own dictionary from a shared one, which it takes. */
CHUNKS_PACK_DEF void
CHUNKS_PACK_NAME(share) (CHUNKS_PACK_TYPE * self, ChunksPackSharedDictionary *shared)
{
	CHUNKS_PACK_NAME(train) (self);
	if (self->has_shared)
		chunks_pack_SharedDictionary_drop(self->shared);
	self->shared = *shared;
	self->has_shared = true;
}

/* id returns the pack's id. */
CHUNKS_PACK_DEF ChunksPackPackId
CHUNKS_PACK_NAME(id) (const CHUNKS_PACK_TYPE * self)
{
	return self->id;
}

/* is_empty reports whether the pack holds no chunks. */
CHUNKS_PACK_DEF bool
CHUNKS_PACK_NAME(is_empty) (const CHUNKS_PACK_TYPE * self)
{
	return self->entries.len == 0;
}

/* number returns the number the pack is written under. */
CHUNKS_PACK_DEF uint64
CHUNKS_PACK_NAME(number) (const CHUNKS_PACK_TYPE * self)
{
	return self->number;
}

/* size returns the bytes written so far. */
CHUNKS_PACK_DEF uint64
CHUNKS_PACK_NAME(size) (const CHUNKS_PACK_TYPE * self)
{
	return STD_ADD(u64, self->flushed, (uint64) self->buffer.len, 448, 9);
}

/* chunks_pack_usable_frame_0_t is usable's state across its pad: the file's name, while live. */
#define chunks_pack_usable_frame_0_t CHUNKS_PACK_CAT(chunks_pack_usable_frame_0_, CHUNKS_PACK_FS)
typedef struct chunks_pack_usable_frame_0_t
{
	const		CHUNKS_PACK_TYPE *self;
	ChunksErrorError *error;
	StdString	name;
	bool		name_live;
} chunks_pack_usable_frame_0_t;

/* usable_pad_0 is usable's pad: the file's name, while live. */
static void
CHUNKS_PACK_NAME(usable_pad_0) (void *frame_)
{
	chunks_pack_usable_frame_0_t *frame = frame_;

	if (frame->name_live)
	{
		frame->name_live = false;
		std_vec_free_Checked(&frame->name, 1, 1);
	}
}

/* usable_body_0 is usable's body: the error naming the file and the write that failed. */
static void
CHUNKS_PACK_NAME(usable_body_0) (void *frame_)
{
	chunks_pack_usable_frame_0_t *frame = frame_;
	const		CHUNKS_PACK_TYPE *self = frame->self;
	StdStr		named;
	StdStr		failed = {(const char *) self->failed.ptr, self->failed.len};
	StdString	message;

	frame->name = CHUNKS_PACK_F_OPS->name(&self->file);
	frame->name_live = true;
	named.ptr = (const char *) frame->name.ptr;
	named.len = frame->name.len;
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("could not write "), STD_FMT_ARG(0),
		STD_FMT_LIT(": an earlier write failed: "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{&named, std_str_Display_fmt}, {&failed, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, 4, args};

		std_fmt_format_Checked(&message, &arguments);
	}
	*frame->error = chunks_error_Error_corrupt(&message);
	CHUNKS_PACK_NAME(usable_pad_0) (frame);
}

/* usable refuses to go on with a pack a write to whose file failed. */
static bool
CHUNKS_PACK_NAME(usable) (const CHUNKS_PACK_TYPE * self, ChunksErrorError *error)
{
	chunks_pack_usable_frame_0_t frame = {.self = self, .error = error};

	if (!self->has_failed)
		return true;
	std_landing(&frame, CHUNKS_PACK_NAME(usable_body_0), CHUNKS_PACK_NAME(usable_pad_0));
	return false;
}

/* flush writes the buffer to the file, keeping the error of a write that fails so that nothing goes on after it. */
static bool
CHUNKS_PACK_NAME(flush) (CHUNKS_PACK_TYPE * self, HostIoTakesShort io, ChunksErrorError *error)
{
	if (!CHUNKS_PACK_NAME(usable) (self, error))
		return false;
	if (!CHUNKS_PACK_F_OPS->write(&self->file, io, self->flushed, self->buffer.ptr, self->buffer.len, error))
	{
		chunks_pack_failed(&self->failed, error);
		self->has_failed = true;
		return false;
	}
	self->flushed = STD_ADD(u64, self->flushed, (uint64) self->buffer.len, 600, 9);
	self->buffer.len = 0;
	return true;
}

/* CHUNKS_PACK_APPEND_EVENT is append's event's captures: the chunk's address and locator, and the names it is traced with. */
#define CHUNKS_PACK_APPEND_EVENT CHUNKS_PACK_CAT(chunks_pack_append_event_, CHUNKS_PACK_FS)
typedef struct CHUNKS_PACK_APPEND_EVENT
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	bool		(*names) (void *captures, bool *has_result, StdVec *result, ChunksErrorError *error);
	void	   *names_captures;
}			CHUNKS_PACK_APPEND_EVENT;

/* append_closure_0 is append's event, ChunkWritten, with the locators its names give. */
static bool
CHUNKS_PACK_NAME(append_closure_0) (void *captures, ChunksTraceEvent *result, ChunksErrorError *error)
{
	CHUNKS_PACK_APPEND_EVENT *event = captures;

	result->tag = CHUNKS_TRACE_EVENT_CHUNK_WRITTEN;
	result->u.chunk_written.address = event->address;
	result->u.chunk_written.locator = event->locator;
	return event->names(event->names_captures, &result->u.chunk_written.has_names, &result->u.chunk_written.names,
						error);
}

/*
 * append adds a chunk's record of a flag and its data and returns its
 * locator, tracing it with the locators names gives, whose captures own
 * nothing.
 */
static bool
CHUNKS_PACK_NAME(append) (CHUNKS_PACK_TYPE * self, HostIoIo *io, ChunksAddressAddress address, uint8 flag,
						  const uint8 *data, size_t data_len,
						  bool (*names) (void *captures, bool *has_result, StdVec *result, ChunksErrorError *error),
						  void *names_captures, ChunksTrailerLocator *result, ChunksErrorError *error)
{
	uint64		offset;
	ChunksTrailerLocator locator;
	chunks_TupleAddressLocator_t entry;
	CHUNKS_PACK_APPEND_EVENT event;

	if (!CHUNKS_PACK_NAME(usable) (self, error))
		return false;
	if (STD_ADD(usize, data_len, 1, 532, 12) > CHUNKS_PACK_MAX_STORED)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" is too large to store ("),
		STD_FMT_ARG(1), STD_FMT_LIT(" bytes)")};
		StdFmtArg	args[] = {{&address, chunks_address_Address_Display_fmt}, {&data_len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, 5, args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = chunks_error_Error_corrupt(&message);
		return false;
	}
	offset = CHUNKS_PACK_NAME(size) (self);
	if (offset > CHUNKS_PACK_MAX_OFFSET)
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("pack "), STD_FMT_ARG(0), STD_FMT_LIT(" is too large to hold another chunk")};
		StdFmtArg	args[] = {{&self->number, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};

		error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
		std_fmt_format_Checked(&error->u.invariant, &arguments);
		return false;
	}
	std_vec_u8_push_Checked(&self->buffer, flag);
	std_vec_extend_from_slice_Checked(&self->buffer, data, data_len, 1, 1);
	locator = chunks_trailer_Locator_at(self->number, offset, STD_ADD(u32, (uint32) data_len, 1, 540, 51));
	entry.f0 = address;
	entry.f1 = locator;
	std_vec_push_Checked(&self->entries, &entry, sizeof(entry), _Alignof(chunks_TupleAddressLocator_t));
	if (self->buffer.len >= CHUNKS_PACK_FLUSH && !CHUNKS_PACK_NAME(flush) (self, HOST_IO_IO_TAKES_SHORT(io), error))
		return false;
	event.address = address;
	event.locator = locator;
	event.names = names;
	event.names_captures = names_captures;
	if (!chunks_trace_try_emit(self->repo, CHUNKS_PACK_NAME(append_closure_0), &event, NULL, error))
		return false;
	*result = locator;
	return true;
}

/* CHUNKS_PACK_WRITE_FRAME is write's state across its pad: what it takes and makes while live. */
#define CHUNKS_PACK_WRITE_FRAME CHUNKS_PACK_CAT(chunks_pack_write_frame_, CHUNKS_PACK_FS)
typedef struct CHUNKS_PACK_WRITE_FRAME
{
	CHUNKS_PACK_TYPE *self;
	HostIoIo   *io;
	ChunksAddressAddress address;
	const uint8 *stored;
	size_t		stored_len;
	bool		leaf;
	ChunksTrailerLocator *result;
	ChunksErrorError *error;
	bool		ok;
	/* sample is a leaf's copy on its way into the samples. */
	StdVec		sample;
	bool		sample_live;
	/* samples is a Vec<Vec<u8>>, taken out of the pack to train on. */
	StdVec		samples;
	bool		samples_live;
	/* slices is a Vec<&[u8]> of the samples. */
	StdVec		slices;
	bool		slices_live;
	/* trained is the dictionary trained, an Option<Vec<u8>>. */
	bool		has_trained;
	StdVec		trained;
	HostCompressDictionary compressing;
	bool		compressing_live;
	HostDecompressDictionary prepared;
	bool		prepared_live;
	/* data is the compressed record. */
	StdVec		data;
	bool		data_live;
}			CHUNKS_PACK_WRITE_FRAME;

/* write_pad_0 is write's pad: what it holds, while live. */
static void
CHUNKS_PACK_NAME(write_pad_0) (void *frame_)
{
	CHUNKS_PACK_WRITE_FRAME *frame = frame_;

	if (frame->sample_live)
	{
		frame->sample_live = false;
		std_vec_free_Checked(&frame->sample, 1, 1);
	}
	if (frame->prepared_live)
	{
		frame->prepared_live = false;
		host_DecompressDictionary_drop(&frame->prepared);
	}
	if (frame->compressing_live)
	{
		frame->compressing_live = false;
		host_CompressDictionary_drop(&frame->compressing);
	}
	if (frame->has_trained)
	{
		frame->has_trained = false;
		std_vec_free_Checked(&frame->trained, 1, 1);
	}
	if (frame->slices_live)
	{
		frame->slices_live = false;
		std_vec_free_Checked(&frame->slices, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (frame->samples_live)
	{
		frame->samples_live = false;
		chunks_pack_VecVecU8_drop(&frame->samples);
	}
	if (frame->data_live)
	{
		frame->data_live = false;
		std_vec_free_Checked(&frame->data, 1, 1);
	}
}

/* write_closure_0 is write's training of the samples, for the shared dictionary's first init. */
static void
CHUNKS_PACK_NAME(write_closure_0) (void *captures, void *result)
{
	const StdVec *slices = captures;
	chunks_OptionVecU8_t *trained = result;

	trained->has = chunks_pack_train_dictionary(slices->ptr, slices->len, &trained->value);
}

/* write_names_0 is write's names: the locators the stored bytes' trailer names. */
static bool
CHUNKS_PACK_NAME(write_names_0) (void *captures, bool *has_result, StdVec *result, ChunksErrorError *error)
{
	const StdBytes *stored = captures;

	return chunks_trace_named(stored->ptr, stored->len, has_result, result, error);
}

/* write_body_0 is write's body: the samples kept and trained, the record compressed, and its append. */
static void
CHUNKS_PACK_NAME(write_body_0) (void *frame_)
{
	CHUNKS_PACK_WRITE_FRAME *frame = frame_;
	CHUNKS_PACK_TYPE *self = frame->self;
	uint64		size = CHUNKS_PACK_NAME(size) (self);
	bool		shared_trained = self->has_shared && chunks_pack_SharedDictionary_trained(self->shared);
	uint8		flag = CHUNKS_PACK_RAW;
	bool		compressed = false;
	StdBytes	stored = {frame->stored, frame->stored_len};
	HostError	host;

	frame->ok = false;
	if (frame->leaf && self->has_samples)
	{
		if (self->samples.len < CHUNKS_PACK_TRAIN_LEAVES)
		{
			std_slice_to_vec_Checked(&frame->sample, frame->stored, frame->stored_len, 1, 1);
			frame->sample_live = true;
			std_vec_push_Checked(&self->samples, &frame->sample, sizeof(StdVec), _Alignof(StdVec));
			frame->sample_live = false;
		}
		if (self->samples.len == CHUNKS_PACK_TRAIN_LEAVES && (size >= CHUNKS_PACK_TRAIN_AFTER || shared_trained))
		{
			frame->samples = self->samples;
			self->has_samples = false;
			frame->samples_live = true;
			std_vec_with_capacity_Checked(&frame->slices, frame->samples.len, sizeof(StdBytes), _Alignof(StdBytes));
			frame->slices_live = true;
			for (size_t k = 0, k_len = frame->samples.len; k < k_len; k++)
			{
				const StdVec *sample = &STD_VEC_AT(&frame->samples, StdVec, k);
				StdBytes	slice = {sample->ptr, sample->len};

				std_vec_push_Checked(&frame->slices, &slice, sizeof(slice), _Alignof(StdBytes));
			}
			if (self->has_shared)
			{
				const chunks_OptionVecU8_t *held = chunks_pack_SharedDictionary_get_or_init(self->shared,
																							CHUNKS_PACK_NAME(write_closure_0),
																							&frame->slices);

				if (held->has)
				{
					std_slice_to_vec_Checked(&frame->trained, held->value.ptr, held->value.len, 1, 1);
					frame->has_trained = true;
				}
			}
			else
				frame->has_trained = chunks_pack_train_dictionary(frame->slices.ptr, frame->slices.len,
																  &frame->trained);
			if (frame->has_trained && self->dictionaries.len < CHUNKS_PACK_MAX_DICTIONARIES)
			{
				chunks_TupleVecU8DecompressDictionary_t entry;

				if (!host_prepare_compressing(frame->trained.ptr, frame->trained.len, CHUNKS_PACK_LEVEL,
											  &frame->compressing, &host))
				{
					*frame->error = chunks_error_Error_FromError_from(&host);
					CHUNKS_PACK_NAME(write_pad_0) (frame);
					return;
				}
				frame->compressing_live = true;
				self->own.f0 = self->dictionaries.len;
				self->own.f1 = frame->compressing;
				self->has_own = true;
				frame->compressing_live = false;
				if (!host_prepare_decompressing(frame->trained.ptr, frame->trained.len, &frame->prepared, &host))
				{
					*frame->error = chunks_error_Error_FromError_from(&host);
					CHUNKS_PACK_NAME(write_pad_0) (frame);
					return;
				}
				frame->prepared_live = true;
				entry.f0 = frame->trained;
				entry.f1 = frame->prepared;
				std_vec_push_Checked(&self->dictionaries, &entry, sizeof(entry),
									 _Alignof(chunks_TupleVecU8DecompressDictionary_t));
				frame->has_trained = false;
				frame->prepared_live = false;
			}
			if (frame->has_trained)
			{
				frame->has_trained = false;
				std_vec_free_Checked(&frame->trained, 1, 1);
			}
			frame->slices_live = false;
			std_vec_free_Checked(&frame->slices, sizeof(StdBytes), _Alignof(StdBytes));
			frame->samples_live = false;
			chunks_pack_VecVecU8_drop(&frame->samples);
		}
	}
	if (self->has_own && frame->leaf)
	{
		StdVec		packed;

		std_vec_new(&frame->data, 1, 1);
		frame->data_live = true;
		chunks_varint_put_varint(&frame->data, (uint64) self->own.f0);
		if (!host_compress(frame->stored, frame->stored_len, CHUNKS_PACK_LEVEL, &self->own.f1, &packed, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			CHUNKS_PACK_NAME(write_pad_0) (frame);
			return;
		}
		std_vec_extend_from_slice_Checked(&frame->data, packed.ptr, packed.len, 1, 1);
		std_vec_free_Checked(&packed, 1, 1);
		flag = CHUNKS_PACK_WITH_DICTIONARY;
		compressed = true;
	}
	else if (frame->stored_len >= 64)
	{
		if (!host_compress(frame->stored, frame->stored_len, CHUNKS_PACK_LEVEL, NULL, &frame->data, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			CHUNKS_PACK_NAME(write_pad_0) (frame);
			return;
		}
		frame->data_live = true;
		flag = CHUNKS_PACK_COMPRESSED;
		compressed = true;
	}
	if (compressed && frame->data.len < frame->stored_len)
		frame->ok = CHUNKS_PACK_NAME(append) (self, frame->io, frame->address, flag, frame->data.ptr, frame->data.len,
											  CHUNKS_PACK_NAME(write_names_0), &stored, frame->result,
											  frame->error);
	else
		frame->ok = CHUNKS_PACK_NAME(append) (self, frame->io, frame->address, CHUNKS_PACK_RAW, frame->stored,
											  frame->stored_len, CHUNKS_PACK_NAME(write_names_0), &stored,
											  frame->result, frame->error);
	CHUNKS_PACK_NAME(write_pad_0) (frame);
}

/*
 * write adds a chunk's stored bytes, compressed with the pack's own
 * dictionary when it is a leaf and the pack has one, and returns its locator.
 */
CHUNKS_PACK_DEF bool
CHUNKS_PACK_NAME(write) (CHUNKS_PACK_TYPE * self, HostIoIo *io, ChunksAddressAddress address, const uint8 *stored,
						 size_t stored_len, bool leaf, ChunksTrailerLocator *result, ChunksErrorError *error)
{
	CHUNKS_PACK_WRITE_FRAME frame = {.self = self, .io = io, .address = address, .stored = stored,
	.stored_len = stored_len, .leaf = leaf, .result = result, .error = error};

	std_landing(&frame, CHUNKS_PACK_NAME(write_body_0), CHUNKS_PACK_NAME(write_pad_0));
	return frame.ok;
}

/* copy_names_0 is copy's names: none. */
static bool
CHUNKS_PACK_NAME(copy_names_0) (void *captures, bool *has_result, StdVec *result, ChunksErrorError *error)
{
	*has_result = false;
	return true;
}

/* CHUNKS_PACK_COPY_FRAME is copy's state across its pad: the dictionary it carries and the record it builds, while live. */
#define CHUNKS_PACK_COPY_FRAME CHUNKS_PACK_CAT(chunks_pack_copy_frame_, CHUNKS_PACK_FS)
typedef struct CHUNKS_PACK_COPY_FRAME
{
	CHUNKS_PACK_TYPE *self;
	HostIoIo   *io;
	ChunksAddressAddress address;
	const ChunksPackRaw *raw;
	const uint8 *dictionary;
	size_t		dictionary_len;
	bool	   *has_result;
	ChunksTrailerLocator *result;
	ChunksErrorError *error;
	bool		ok;
	HostDecompressDictionary prepared;
	bool		prepared_live;
	StdVec		carried;
	bool		carried_live;
	StdVec		data;
	bool		data_live;
}			CHUNKS_PACK_COPY_FRAME;

/* copy_pad_0 is copy's pad: what it holds, while live. */
static void
CHUNKS_PACK_NAME(copy_pad_0) (void *frame_)
{
	CHUNKS_PACK_COPY_FRAME *frame = frame_;

	if (frame->prepared_live)
	{
		frame->prepared_live = false;
		host_DecompressDictionary_drop(&frame->prepared);
	}
	if (frame->carried_live)
	{
		frame->carried_live = false;
		std_vec_free_Checked(&frame->carried, 1, 1);
	}
	if (frame->data_live)
	{
		frame->data_live = false;
		std_vec_free_Checked(&frame->data, 1, 1);
	}
}

/* copy_body_0 is copy's body: the dictionary's slot found or carried, and the record appended. */
static void
CHUNKS_PACK_NAME(copy_body_0) (void *frame_)
{
	CHUNKS_PACK_COPY_FRAME *frame = frame_;
	CHUNKS_PACK_TYPE *self = frame->self;
	size_t		carried;
	size_t		slot = 0;
	bool		found = false;
	HostError	host;

	frame->ok = false;
	if (frame->dictionary == NULL || !CHUNKS_PACK_RAW_LAYOUT(frame->raw)->has_slot)
	{
		*frame->has_result = true;
		frame->ok = CHUNKS_PACK_NAME(append) (self, frame->io, frame->address, CHUNKS_PACK_RAW_LAYOUT(frame->raw)->flag,
											  CHUNKS_PACK_RAW_LAYOUT(frame->raw)->data.ptr, CHUNKS_PACK_RAW_LAYOUT(frame->raw)->data.len,
											  CHUNKS_PACK_NAME(copy_names_0), NULL, frame->result, frame->error);
		return;
	}
	carried = STD_SUB(usize, self->dictionaries.len, (size_t) self->has_own, 508, 23);
	for (size_t k = 0, k_len = self->dictionaries.len; k < k_len; k++)
	{
		const chunks_TupleVecU8DecompressDictionary_t *entry = &STD_VEC_AT(&self->dictionaries,
																		   chunks_TupleVecU8DecompressDictionary_t,
																		   k);

		if (entry->f0.len == frame->dictionary_len &&
			memcmp(entry->f0.ptr, frame->dictionary, frame->dictionary_len) == 0)
		{
			slot = k;
			found = true;
			break;
		}
	}
	if (!found)
	{
		chunks_TupleVecU8DecompressDictionary_t entry;

		if (STD_ADD(usize, carried, 1, 511, 21) >= CHUNKS_PACK_MAX_DICTIONARIES)
		{
			*frame->has_result = false;
			frame->ok = true;
			return;
		}
		if (!host_prepare_decompressing(frame->dictionary, frame->dictionary_len, &frame->prepared, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			return;
		}
		frame->prepared_live = true;
		std_slice_to_vec_Checked(&frame->carried, frame->dictionary, frame->dictionary_len, 1, 1);
		frame->carried_live = true;
		entry.f0 = frame->carried;
		entry.f1 = frame->prepared;
		std_vec_push_Checked(&self->dictionaries, &entry, sizeof(entry),
							 _Alignof(chunks_TupleVecU8DecompressDictionary_t));
		frame->carried_live = false;
		frame->prepared_live = false;
		slot = std_wrapping_sub_usize(self->dictionaries.len, 1);
	}
	std_vec_with_capacity_Checked(&frame->data, STD_ADD(usize, CHUNKS_PACK_RAW_LAYOUT(frame->raw)->data.len, 1, 518, 43), 1, 1);
	frame->data_live = true;
	chunks_varint_put_varint(&frame->data, (uint64) slot);
	std_vec_extend_from_slice_Checked(&frame->data, CHUNKS_PACK_RAW_LAYOUT(frame->raw)->data.ptr, CHUNKS_PACK_RAW_LAYOUT(frame->raw)->data.len, 1, 1);
	*frame->has_result = true;
	frame->ok = CHUNKS_PACK_NAME(append) (self, frame->io, frame->address, CHUNKS_PACK_WITH_DICTIONARY,
										  frame->data.ptr, frame->data.len, CHUNKS_PACK_NAME(copy_names_0), NULL,
										  frame->result, frame->error);
	CHUNKS_PACK_NAME(copy_pad_0) (frame);
}

/*
 * copy adds a chunk's record from another pack with its compressed bytes as
 * they are, carrying the dictionary it was compressed with, and returns its
 * locator, or none when the pack carries as many dictionaries as it can, one
 * slot kept for its own, and lacks that one.
 */
CHUNKS_PACK_DEF bool
CHUNKS_PACK_NAME(copy) (CHUNKS_PACK_TYPE * self, HostIoIo *io, ChunksAddressAddress address, const ChunksPackRaw *raw,
						const uint8 *dictionary, size_t dictionary_len, bool *has_result,
						ChunksTrailerLocator *result, ChunksErrorError *error)
{
	CHUNKS_PACK_COPY_FRAME frame = {.self = self, .io = io, .address = address, .raw = raw,
		.dictionary = dictionary, .dictionary_len = dictionary_len, .has_result = has_result, .result = result,
	.error = error};

	std_landing(&frame, CHUNKS_PACK_NAME(copy_body_0), CHUNKS_PACK_NAME(copy_pad_0));
	return frame.ok;
}

/* read_closure_0 is read's dictionary of a slot: the pack's prepared one, or none. */
static bool
CHUNKS_PACK_NAME(read_closure_0) (void *captures, HostIoReadDoors io, size_t slot,
								  const HostDecompressDictionary **result, ChunksErrorError *error)
{
	const		CHUNKS_PACK_TYPE *self = captures;

	*result = slot < self->dictionaries.len ?
		&STD_VEC_AT(&self->dictionaries, chunks_TupleVecU8DecompressDictionary_t, slot).f1 : NULL;
	return true;
}

/* CHUNKS_PACK_READ_FRAME is read's state across its pad: the record read from the file, while live. */
#define CHUNKS_PACK_READ_FRAME CHUNKS_PACK_CAT(chunks_pack_read_frame_, CHUNKS_PACK_FS)
typedef struct CHUNKS_PACK_READ_FRAME
{
	const		CHUNKS_PACK_TYPE *self;
	HostIoIo   *io;
	ChunksTrailerLocator locator;
	const ChunksAddressAddress *address;
	ChunksChunkChunk *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		record;
	bool		record_live;
}			CHUNKS_PACK_READ_FRAME;

/* read_pad_0 is read's pad: the record, while live. */
static void
CHUNKS_PACK_NAME(read_pad_0) (void *frame_)
{
	CHUNKS_PACK_READ_FRAME *frame = frame_;

	if (frame->record_live)
	{
		frame->record_live = false;
		std_vec_free_Checked(&frame->record, 1, 1);
	}
}

/* read_body_0 is read's body: the record from the file or the buffer, and its stored bytes. */
static void
CHUNKS_PACK_NAME(read_body_0) (void *frame_)
{
	CHUNKS_PACK_READ_FRAME *frame = frame_;
	const		CHUNKS_PACK_TYPE *self = frame->self;
	uint64		offset = chunks_trailer_Locator_offset(&frame->locator);
	size_t		length = (size_t) chunks_trailer_Locator_length(&frame->locator);
	uint64		at;

	frame->ok = false;
	if (offset < self->flushed)
	{
		if (!chunks_file_read_at(HOST_IO_IO_READ_DOORS(frame->io), CHUNKS_PACK_FILE_OF(self), offset, length,
								 &frame->record, frame->error))
			return;
		frame->record_live = true;
		frame->ok = chunks_pack_stored_of(HOST_IO_IO_READ_DOORS(frame->io), frame->record.ptr, frame->record.len,
										  CHUNKS_PACK_NAME(read_closure_0), (void *) self, NULL, frame->address,
										  frame->result, frame->error);
		CHUNKS_PACK_NAME(read_pad_0) (frame);
		return;
	}
	at = std_wrapping_sub_u64(offset, self->flushed);
	if (at > self->buffer.len || length > std_wrapping_sub_usize(self->buffer.len, (size_t) at))
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0), STD_FMT_LIT(" is past the end of its pack")};
		StdFmtArg	args[] = {{frame->address, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, 3, args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*frame->error = chunks_error_Error_corrupt(&message);
		return;
	}
	frame->ok = chunks_pack_stored_of(HOST_IO_IO_READ_DOORS(frame->io), STD_PTR_AT(self->buffer.ptr, at, 1), length,
									  CHUNKS_PACK_NAME(read_closure_0), (void *) self, NULL, frame->address,
									  frame->result, frame->error);
}

/*
 * read returns the stored bytes of a chunk the pack holds at the locator,
 * from its file or from what it has not written to it yet, after checking
 * that its hashed bytes have the address.
 */
CHUNKS_PACK_DEF bool
CHUNKS_PACK_NAME(read) (const CHUNKS_PACK_TYPE * self, HostIoIo *io, ChunksTrailerLocator locator,
						const ChunksAddressAddress *address, ChunksChunkChunk *result, ChunksErrorError *error)
{
	CHUNKS_PACK_READ_FRAME frame = {.self = self, .io = io, .locator = locator, .address = address,
	.result = result, .error = error};

	std_landing(&frame, CHUNKS_PACK_NAME(read_body_0), CHUNKS_PACK_NAME(read_pad_0));
	return frame.ok;
}

/* drop is PackWriter's drop glue: its file, its buffers, its entries, its samples, its dictionaries, and its shared one. */
CHUNKS_PACK_DEF void
CHUNKS_PACK_NAME(drop) (CHUNKS_PACK_TYPE * self)
{
	CHUNKS_PACK_F_DROP(&self->file);
	std_vec_free_Checked(&self->header, 1, 1);
	std_vec_free_Checked(&self->buffer, 1, 1);
	std_vec_free_Checked(&self->entries, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	if (self->has_samples)
	{
		self->has_samples = false;
		chunks_pack_VecVecU8_drop(&self->samples);
	}
	chunks_pack_Dictionaries_drop(&self->dictionaries);
	if (self->has_own)
	{
		self->has_own = false;
		host_CompressDictionary_drop(&self->own.f1);
	}
	if (self->has_shared)
	{
		self->has_shared = false;
		chunks_pack_SharedDictionary_drop(self->shared);
	}
	if (self->has_failed)
	{
		self->has_failed = false;
		std_vec_free_Checked(&self->failed, 1, 1);
	}
}

#undef CHUNKS_PACK_FILE_OF
#undef chunks_pack_usable_frame_0_t
#undef CHUNKS_PACK_DEF_static
#undef CHUNKS_PACK_DEF_extern
#undef CHUNKS_PACK_DEF
#undef CHUNKS_PACK_APPEND_EVENT
#undef CHUNKS_PACK_OVER_FRAME
#undef CHUNKS_PACK_WRITE_FRAME
#undef CHUNKS_PACK_COPY_FRAME
#undef CHUNKS_PACK_READ_FRAME

#endif							/* CHUNKS_PACK_DEFINE */

#undef CHUNKS_PACK_TYPE
#undef CHUNKS_PACK_NAME
#undef CHUNKS_PACK_CAT_
#undef CHUNKS_PACK_CAT
#undef CHUNKS_PACK_SCOPE
#undef CHUNKS_PACK_DECLARE
#undef CHUNKS_PACK_FS
#undef CHUNKS_PACK_F
#undef CHUNKS_PACK_F_OPS
#undef CHUNKS_PACK_F_DROP
#undef CHUNKS_PACK_DEFINE
