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
 * Records, the form of every object that is not a tree node: fields in
 * increasing number order, each written as its number, its length, and its
 * bytes (chunks' record.rs).
 */
#ifndef DOLTGRES_CHUNKS_RECORD_H
#define DOLTGRES_CHUNKS_RECORD_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/std/vec.h"

/* ChunksRecordField is Field: a record's field. */
typedef struct ChunksRecordField
{
	uint32		number;
	const uint8 *bytes;
	size_t		bytes_len;
	/* at is the offset of the field's bytes in the record. */
	size_t		at;
} ChunksRecordField;

/* ChunksRecordRecordWriter is RecordWriter: a writer of a record's fields, which must come in increasing number order. */
typedef struct ChunksRecordRecordWriter
{
	StdVec		out;
	bool		has_last;
	uint32		last;

	/*
	 * addresses is a Vec<(u32, Locator)> of the offsets of the addresses
	 * written, with their locators.
	 */
	StdVec		addresses;
} ChunksRecordRecordWriter;

/*
 * ChunksRecordRecord is Record: a record chunk read back, its kind, its
 * fields, and where each address it holds was stored.
 */
typedef struct ChunksRecordRecord
{
	ChunksHeaderKind kind;
	/* fields is a Vec<Field>. */
	StdVec		fields;

	/*
	 * offsets is a Vec<u32> of the body offsets of the record's addresses, in
	 * increasing order.
	 */
	StdVec		offsets;
	/* locators is a Vec<Locator>. */
	StdVec		locators;
} ChunksRecordRecord;

extern ChunksRecordRecordWriter chunks_record_RecordWriter_Default_default(void);
extern ChunksRecordRecordWriter chunks_record_RecordWriter_new(void);
extern ChunksRecordRecordWriter chunks_record_RecordWriter_with_capacity(size_t bytes);
extern ChunksRecordRecordWriter *chunks_record_RecordWriter_field(ChunksRecordRecordWriter *self, uint32 number,
																  const uint8 *bytes, size_t bytes_len);
extern ChunksRecordRecordWriter *chunks_record_RecordWriter_uint(ChunksRecordRecordWriter *self, uint32 number,
																 uint64 value);
extern ChunksRecordRecordWriter *chunks_record_RecordWriter_int(ChunksRecordRecordWriter *self, uint32 number,
																int64 value);
extern ChunksRecordRecordWriter *chunks_record_RecordWriter_addresses(ChunksRecordRecordWriter *self, uint32 number,
																	  const chunks_TupleAddressLocator_t *addresses,
																	  size_t addresses_len);
extern ChunksRecordRecordWriter *chunks_record_RecordWriter_record(ChunksRecordRecordWriter *self, uint32 number,
																   ChunksRecordRecordWriter *inner);
extern ChunksRecordRecordWriter *chunks_record_RecordWriter_list(ChunksRecordRecordWriter *self, uint32 number,
																 StdVec *items);
extern StdVec chunks_record_RecordWriter_finish(ChunksRecordRecordWriter *self);
extern void chunks_record_RecordWriter_finish_value(ChunksRecordRecordWriter *self, StdVec *out_0, StdVec *out_1);
extern void chunks_record_RecordWriter_finish_chunk(ChunksRecordRecordWriter *self, ChunksHeaderKind kind,
													StdVec *out_0, StdVec *out_1);
extern bool chunks_record_fields(const uint8 *bytes, size_t bytes_len, StdVec *result, ChunksErrorError *error);
extern bool chunks_record_int(const ChunksRecordField *field, int64 *result, ChunksErrorError *error);
extern uint64 chunks_record_zigzag(int64 value);
extern int64 chunks_record_unzigzag(uint64 value);
extern bool chunks_record_Record_parse(const ChunksChunkChunk *chunk, ChunksRecordRecord *result,
									   ChunksErrorError *error);
extern bool chunks_record_Record_value(const uint8 *bytes, size_t bytes_len, const chunks_TupleU32Locator_t *addresses,
									   size_t addresses_len, ChunksRecordRecord *result, ChunksErrorError *error);
extern bool chunks_record_Record_nested(const ChunksRecordRecord *self, const ChunksRecordField *field,
										ChunksRecordRecord *result, ChunksErrorError *error);
extern bool chunks_record_Record_list(const ChunksRecordRecord *self, const ChunksRecordField *field, StdVec *result,
									  ChunksErrorError *error);
extern const uint8 *chunks_record_Record_bytes(const ChunksRecordRecord *self, uint32 number, size_t *result_len);
extern bool chunks_record_Record_uint(const ChunksRecordRecord *self, uint32 number, uint64 *result,
									  ChunksErrorError *error);
extern bool chunks_record_Record_int(const ChunksRecordRecord *self, uint32 number, int64 *result,
									 ChunksErrorError *error);
extern bool chunks_record_Record_address(const ChunksRecordRecord *self, uint32 number, bool *has_result,
										 ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1,
										 ChunksErrorError *error);
extern const ChunksRecordField *chunks_record_Record_field(const ChunksRecordRecord *self, uint32 number);
extern bool chunks_record_Record_addresses(const ChunksRecordRecord *self, const ChunksRecordField *field,
										   StdVec *result, ChunksErrorError *error);
extern bool chunks_record_uint(const ChunksRecordField *field, uint64 *result, ChunksErrorError *error);
extern void chunks_record_RecordWriter_drop(void *self_);
extern void chunks_record_Record_drop(void *self_);

#ifdef DOLTGRES_TEST_CHUNKS
extern bool chunks_record_tests_VecAddress_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_record_tests_VecAddress_PartialEq_eq(const StdVec *self, const StdVec *other);
extern bool chunks_record_tests_TupleAddressLocator_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_record_tests_VecTupleAddressLocator_Debug_fmt(const void *value, StdFormatter *f);
extern bool chunks_record_tests_VecTupleAddressLocator_PartialEq_eq(const StdVec *self, const StdVec *other);
extern void test_chunks_record_tests_round_trips_fields_in_order(void);
extern void test_chunks_record_tests_records_hold_addresses_and_signed_integers(void);
extern void test_chunks_record_tests_rejects_bad_records(void);
extern void test_chunks_record_tests_rejects_addresses_out_of_order(void);
#endif

#endif							/* DOLTGRES_CHUNKS_RECORD_H */
