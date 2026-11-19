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
 * Packs, the immutable files every chunk is stored in: a header, the
 * chunks' records, a table of the zstd dictionaries its leaves are
 * compressed with, its address table, and a footer (chunks' pack.rs).
 */
#ifndef DOLTGRES_CHUNKS_PACK_H
#define DOLTGRES_CHUNKS_PACK_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/dead.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/index.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/any.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/hash.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"

/*
 * CHUNKS_PACK_MAX_DICTIONARIES is the most dictionaries a pack keeps: its
 * own and those carried with records copied from other packs.
 */
#define CHUNKS_PACK_MAX_DICTIONARIES ((size_t) 8)

/* CHUNKS_PACK_PACK_TARGET is the size at which writers close a pack and start another. */
#define CHUNKS_PACK_PACK_TARGET ((uint64) 256 << 20)

/* ChunksPackPackId is PackId: 128 bits naming a pack, unique among every pack ever written. */
typedef struct ChunksPackPackId
{
	uint8		f0[16];
} ChunksPackPackId;

/* chunks_TupleU64PackId_t is the tuple (u64, PackId): a pack's number and its id. */
typedef struct chunks_TupleU64PackId_t
{
	uint64		f0;
	ChunksPackPackId f1;
} chunks_TupleU64PackId_t;

/* CHUNKS_PACK_TEMP ends the name of an unfinished pack's file. */
#define CHUNKS_PACK_TEMP ".pack.tmp"

/*
 * ChunksPackSharedDictionary is SharedDictionary, an
 * Arc<OnceLock<Option<Vec<u8>>>>: the dictionary packs written together share,
 * once one of them has trained it.
 */
typedef StdArcInner *ChunksPackSharedDictionary;

/* chunks_OptionVecU8_t is an Option<Vec<u8>>: a SharedDictionary's dictionary, none when too few samples trained it. */
typedef struct chunks_OptionVecU8_t
{
	bool		has;
	StdVec		value;
} chunks_OptionVecU8_t;

/*
 * ChunksPackPackFileOps is PackFile's table, its supertrait Deref's first,
 * whose target is the File: a write of bytes at an offset, and its name.
 */
typedef struct ChunksPackPackFileOps
{
	StdDerefOps deref;
	bool		(*write) (const void *self, HostIoTakesShort io, uint64 offset, const uint8 *bytes, size_t bytes_len,
						  ChunksErrorError *error);
	StdString	(*name) (const void *self);
} ChunksPackPackFileOps;

/* ChunksPackUnfinished is Unfinished: a pack's temporary path, and the entry's flag its drop sets. */
typedef struct ChunksPackUnfinished
{
	StdPathBuf	path;
	/* dropped is an Arc<AtomicBool>, the entry's flag, which the drop sets. */
	StdArcInner *dropped;
} ChunksPackUnfinished;

/* ChunksPackInDir is InDir: a pack's file under a temporary name in the directory finish names it in. */
typedef struct ChunksPackInDir
{
	HostFile	file;
	StdPathBuf	dir;
	ChunksPackUnfinished temp;

	/*
	 * owner is the hold of the pack's number among its process's open
	 * writers, which the finished pack takes.
	 */
	bool		has_owner;
	ChunksPacksWritersWriting owner;
} ChunksPackInDir;

/*
 * ChunksPackRaw is Raw: a chunk record as a pack stores it, for a copy that
 * keeps its compressed bytes. Its storage is opaque, laid out by pack.c
 * alone, as the Rust's fields are private to its module, so no other C names
 * its parts: PackReader's raw fills one in its caller's storage, a copy
 * takes it, and t/117 refuses a file outside pack.c that initializes one or
 * copies bytes into one.
 */
typedef struct ChunksPackRaw
{
	uint64		opaque[6];
} ChunksPackRaw;

/* PackWriter<InDir>: a writer of a pack in its repository's directory, which pack.c defines. */
#define CHUNKS_PACK_SCOPE extern
#define CHUNKS_PACK_DECLARE
#define CHUNKS_PACK_FS InDir
#define CHUNKS_PACK_F ChunksPackInDir
#include "doltgres/chunks/pack_template.h"

/* PackWriter<ScratchFile>: a writer of a scratch pack, which pack.c defines. */
#define CHUNKS_PACK_SCOPE extern
#define CHUNKS_PACK_DECLARE
#define CHUNKS_PACK_FS ScratchFile
#define CHUNKS_PACK_F HostScratchFile
#include "doltgres/chunks/pack_template.h"

/* ChunksPackPack is Pack: a written pack and its run of the address index. */
typedef struct ChunksPackPack
{
	ChunksPackPackId id;
	uint64		number;
	StdPathBuf	path;
	ChunksIndexRunLocator run;

	/*
	 * owner is the hold of the pack's number among its process's open
	 * writers, until the pack is added, for a pack its repository's packs
	 * started.
	 */
	bool		has_owner;
	ChunksPacksWritersWriting owner;
} ChunksPackPack;

/* ChunksPackPackReader is PackReader: a reader of a pack's chunks. */
typedef struct ChunksPackPackReader
{
	ChunksPackPackId id;
	HostFile	file;
	StdPathBuf	path;

	/*
	 * dictionaries is a Vec<Slot> of the pack's dictionaries by slot, each
	 * prepared once a read first needs it.
	 */
	StdVec		dictionaries;
	uint64		count;
	/* records is the bytes of its chunks' records. */
	uint64		records;

	/*
	 * addresses is where its address table lies, with its length and block
	 * count.
	 */
	std_TupleU64U32U32_t addresses;
} ChunksPackPackReader;

/*
 * ChunksPackVerified is Verified: a chunk a pack's address table names whose
 * record, read where the table says, hashes to the table's address, which
 * only PackReader's verified makes.
 */
typedef struct ChunksPackVerified
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	bool		has_slot;
	size_t		slot;
	StdVec		stored;
} ChunksPackVerified;

/*
 * ChunksPackLost is Lost: what a walk of a pack's chunks could not verify,
 * each address table block that fails its checksum and each chunk whose
 * record doesn't read whole or doesn't hash to its address.
 */
typedef struct ChunksPackLost
{
	/* blocks is a Vec<(Address, Option<Address>, u64)>. */
	StdVec		blocks;
	/* records is a Vec<(Address, Locator)>. */
	StdVec		records;
} ChunksPackLost;

extern const ChunksPackPackFileOps CHUNKS_PACK_IN_DIR_PACK_FILE_OPS;
extern const ChunksPackPackFileOps CHUNKS_PACK_SCRATCH_FILE_PACK_FILE_OPS;

extern bool chunks_pack_PackId_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_pack_PackId_PartialEq_eq(const ChunksPackPackId *self, const ChunksPackPackId *other);
extern StdOrdering chunks_pack_PackId_Ord_cmp(const ChunksPackPackId *self, const ChunksPackPackId *other);
extern ChunksPackPackId chunks_pack_PackId_new(void);
extern StdString chunks_pack_PackId_file_name(const ChunksPackPackId *self, const char *extension,
											  size_t extension_len);
extern bool chunks_pack_temp_of(const char *name, size_t name_len, uint32 *result_0, uint64 *result_1);
extern bool chunks_pack_PackId_of_name(const char *name, size_t name_len, ChunksPackPackId *result);
extern void chunks_pack_PackId_Hash_hash(const void *self_, StdHasher state);
extern bool chunks_pack_PackId_PartialOrd_partial_cmp(const ChunksPackPackId *self, const ChunksPackPackId *other,
													  StdOrdering *result);
extern ChunksPackPackId chunks_pack_PackId_Default_default(void);
extern const void *chunks_pack_InDir_Deref_deref(const void *self);
extern bool chunks_pack_InDir_PackFile_write(const void *self, HostIoTakesShort io, uint64 offset,
											 const uint8 *bytes, size_t bytes_len, ChunksErrorError *error);
extern StdString chunks_pack_InDir_PackFile_name(const void *self);
extern bool chunks_pack_ScratchFile_PackFile_write(const void *self, HostIoTakesShort io, uint64 offset,
												   const uint8 *bytes, size_t bytes_len, ChunksErrorError *error);
extern StdString chunks_pack_ScratchFile_PackFile_name(const void *self);
extern bool chunks_pack_PackWriterInDir_create(HostIoIo *io, const char *dir, size_t dir_len, uint64 number,
											   ChunksPackPackWriterInDir *result, ChunksErrorError *error);
extern void chunks_pack_PackWriterInDir_owned(ChunksPackPackWriterInDir *self, ChunksPacksWritersWriting *owner);
extern bool chunks_pack_PackWriterInDir_abandon(ChunksPackPackWriterInDir *self, HostIoTakesShort io,
												ChunksErrorError *error);
extern bool chunks_pack_PackWriterInDir_finish(ChunksPackPackWriterInDir *self, HostIoTakesShort io,
											   ChunksPackPack *result, ChunksErrorError *error);
extern bool chunks_pack_PackWriterScratchFile_scratch(HostIoIo *io, ChunksPackPackWriterScratchFile *result,
													  ChunksErrorError *error);
extern bool chunks_pack_address_of(const uint8 *stored, size_t stored_len, ChunksAddressAddress *result,
								   ChunksErrorError *error);
extern const char *chunks_pack_PackReader_path(const ChunksPackPackReader *self, size_t *result_len);
extern bool chunks_pack_PackReader_dead(const ChunksPackPackReader *self, HostIoReadDoors io, uint64 generation,
										ChunksDeadDead *result, ChunksErrorError *error);
extern bool chunks_pack_PackReader_open(HostIoReadDoors io, const char *path, size_t path_len,
										ChunksPackPackReader *result, ChunksErrorError *error);
extern uint64 chunks_pack_PackReader_len(const ChunksPackPackReader *self);
extern bool chunks_pack_PackReader_is_empty(const ChunksPackPackReader *self);
extern uint64 chunks_pack_PackReader_records_len(const ChunksPackPackReader *self);
extern bool chunks_pack_PackReader_has_dictionaries(const ChunksPackPackReader *self);
extern bool chunks_pack_PackReader_read(const ChunksPackPackReader *self, HostIoReadDoors io,
										ChunksTrailerLocator locator, const ChunksAddressAddress *address,
										ChunksChunkChunk *result, ChunksErrorError *error);
extern bool chunks_pack_PackReader_raw(const ChunksPackPackReader *self, HostIoIo *io, ChunksTrailerLocator locator,
									   const ChunksAddressAddress *address, ChunksPackRaw *result,
									   ChunksErrorError *error);
extern bool chunks_pack_PackReader_dictionary(const ChunksPackPackReader *self, HostIoIo *io, size_t slot,
											  StdVec *result, ChunksErrorError *error);
extern bool chunks_pack_PackReader_dictionaries(const ChunksPackPackReader *self, HostIoIo *io,
												ChunksErrorError *error);
extern bool chunks_pack_PackReader_table(const ChunksPackPackReader *self, HostIoReadDoors io,
										 ChunksIndexRunLocator *result, ChunksErrorError *error);
extern void chunks_pack_PackReader_advise(const ChunksPackPackReader *self, HostIoIo *io,
										  ChunksTrailerLocator locator);
extern bool chunks_pack_PackReader_verified(const ChunksPackPackReader *self, ChunksFileInterruptible io,
											uint64 number,
											bool (*f) (void *captures, ChunksFileInterruptible io,
													   ChunksPackVerified *verified, ChunksErrorError *error),
											void *f_captures, void (*f_drop) (void *), ChunksPackLost *result,
											ChunksErrorError *error);
extern ChunksAddressAddress chunks_pack_Verified_address(const ChunksPackVerified *self);
extern ChunksTrailerLocator chunks_pack_Verified_locator(const ChunksPackVerified *self);
extern bool chunks_pack_Verified_slot(const ChunksPackVerified *self, size_t *result);
extern const uint8 *chunks_pack_Verified_stored(const ChunksPackVerified *self, size_t *result_len);
extern bool chunks_pack_Verified_split(const ChunksPackVerified *self,
									   bool (*addresses) (void *captures, const uint8 *hashed, size_t hashed_len,
														  size_t *result, ChunksErrorError *error),
									   void *addresses_captures, void (*addresses_drop) (void *),
									   ChunksTrailerStored *result, ChunksErrorError *error);
extern bool chunks_pack_Lost_Debug_fmt(const void *value, StdFormatter *f);
extern ChunksPackLost chunks_pack_Lost_Default_default(void);
extern bool chunks_pack_Lost_PartialEq_eq(const ChunksPackLost *self, const ChunksPackLost *other);
extern bool chunks_pack_Lost_message(const ChunksPackLost *self, const char *pack, size_t pack_len,
									 StdString *result);
extern uint64 chunks_pack_Lost_offset(const ChunksPackLost *self);
extern void chunks_pack_InDir_drop(ChunksPackInDir *self);
extern void chunks_pack_Raw_drop(ChunksPackRaw *self);
extern void chunks_pack_Pack_drop(ChunksPackPack *self);
extern void chunks_pack_PackReader_drop(ChunksPackPackReader *self);
extern StdArcInner *chunks_pack_ArcPackReader_new(ChunksPackPackReader *reader);
extern void chunks_pack_ArcPackReader_drop(StdArcInner *self);
extern void chunks_pack_VecVecU8_drop(StdVec *self);
extern void chunks_pack_PathBuf_drop(void *value);
extern bool chunks_pack_header_addresses(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
										 ChunksErrorError *error);
extern void chunks_pack_Lost_drop(ChunksPackLost *self);
extern bool chunks_pack_TupleAddressU64_lt(const chunks_TupleAddressU64_t *a, const chunks_TupleAddressU64_t *b);
extern ChunksPackSharedDictionary chunks_pack_SharedDictionary_Default_default(void);
extern const chunks_OptionVecU8_t *chunks_pack_SharedDictionary_get(ChunksPackSharedDictionary shared);
extern void chunks_pack_SharedDictionary_drop(ChunksPackSharedDictionary shared);

#ifdef DOLTGRES_TEST_CHUNKS
extern void test_chunks_pack_tests_a_write_past_what_a_locator_holds_is_refused(void);
extern void test_chunks_pack_tests_writes_and_reads_chunks(void);
extern void test_chunks_pack_tests_a_packs_reader_reads_the_packs_deletion_vector(void);
extern void test_chunks_pack_tests_refuses_a_footer_with_a_short_header(void);
extern void test_chunks_pack_tests_a_record_or_a_table_naming_a_dictionary_the_pack_lacks_is_corrupt(void);
extern void test_chunks_pack_tests_a_writer_dropped_unfinished_leaves_its_file_for_the_backends_next_writer_to_remove(void);
extern void test_chunks_pack_tests_an_abandon_that_fails_leaves_its_file_for_the_backends_next_writer_to_remove(void);
extern void test_chunks_pack_tests_an_unfinished_packs_name_carries_a_wide_number(void);
extern void test_chunks_pack_tests_a_copy_takes_no_record_whose_stored_bytes_lack_its_address(void);
extern void test_chunks_pack_tests_a_walk_of_a_packs_chunks_stops_within_a_chunk_once_a_cancel_is_pending(void);
extern void test_chunks_pack_tests_a_pack_keeps_each_write_of_an_address_whose_trailer_differs_and_walks_them_all(void);
extern void test_chunks_pack_tests_packs_that_share_a_dictionary_take_the_one_the_first_to_collect_its_samples_trained(void);
extern void test_chunks_pack_tests_a_pack_that_trains_none_compresses_its_leaves_without_a_dictionary(void);
extern void test_chunks_pack_tests_a_pack_that_trains_takes_no_dictionary_until_it_holds_its_threshold_of_bytes(void);
extern void test_chunks_pack_tests_a_pack_sharing_a_trained_dictionary_takes_it_once_it_collects_its_samples(void);
extern void test_chunks_pack_tests_a_scratch_pack_reads_back_its_chunks_and_goes_when_its_writer_drops(void);
extern void test_chunks_pack_tests_a_decompression_out_of_memory_fails_a_read_and_a_walk_with_postgres_error_and_loses_no_record(void);
extern void test_chunks_pack_tests_copies_keep_their_compressed_bytes_with_the_dictionaries_they_name_one_slot_each(void);
extern void test_chunks_pack_tests_a_pack_carries_at_most_its_bound_of_dictionaries_keeping_a_slot_for_its_own(void);
#endif

#endif							/* DOLTGRES_CHUNKS_PACK_H */
