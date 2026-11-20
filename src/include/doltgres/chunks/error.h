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
 * Errors of reading and writing chunks (chunks' error.rs).
 */
#ifndef DOLTGRES_CHUNKS_ERROR_H
#define DOLTGRES_CHUNKS_ERROR_H

#include "doltgres/chunks/address.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/error.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"

/*
 * ChunksErrorReading is what a missing chunk was read for: a tree, by its
 * database's, branch's, and own ids, and the work that read it.
 */
typedef struct ChunksErrorReading
{
	uint32		db;
	uint64		branch;
	uint64		tree;
	StdStr		work;
} ChunksErrorReading;

/* ChunksErrorErrorTag is Error's variants. */
typedef enum ChunksErrorErrorTag
{
	/* A seam of the host failed, with Postgres' error. */
	CHUNKS_ERROR_ERROR_HOST,
	/* Stored data does not have the expected format. */
	CHUNKS_ERROR_ERROR_CORRUPT,

	/*
	 * A durable file's bytes are damaged at an offset, as a crash never
	 * leaves them.
	 */
	CHUNKS_ERROR_ERROR_DAMAGED,
	/* The store has no chunk with the address, with what it was read for. */
	CHUNKS_ERROR_ERROR_MISSING,
	/* The request is refused in the store's state. */
	CHUNKS_ERROR_ERROR_REFUSED,
	/* Code outside storage that storage called failed. */
	CHUNKS_ERROR_ERROR_EXTERNAL,

	/*
	 * A check of a plan invariant failed, with the invariant's name and what
	 * broke it.
	 */
	CHUNKS_ERROR_ERROR_INVARIANT,
} ChunksErrorErrorTag;

/* ChunksErrorError is a failure to read or write chunks. */
typedef struct ChunksErrorError
{
	ChunksErrorErrorTag tag;
	union
	{
		HostError	host;
		StdString	corrupt;
		struct
		{
			StdPathBuf	path;
			uint64		offset;
			StdString	message;
		}			damaged;
		struct
		{
			ChunksAddressAddress f0;
			bool		has_f1;
			ChunksErrorReading f1;
		}			missing;
		StdString	refused;
		StdDynError external;
		StdString	invariant;
	}			u;
} ChunksErrorError;

/* ChunksErrorActionTag is Action's variants: what a file operation did. */
typedef enum ChunksErrorActionTag
{
	CHUNKS_ERROR_ACTION_OPEN,
	CHUNKS_ERROR_ACTION_CREATE,
	CHUNKS_ERROR_ACTION_READ,
	CHUNKS_ERROR_ACTION_WRITE,
	/* A write past the end of a file, which disk space that ran out refuses. */
	CHUNKS_ERROR_ACTION_EXTEND,
	CHUNKS_ERROR_ACTION_SYNC,
	CHUNKS_ERROR_ACTION_REMOVE,
	CHUNKS_ERROR_ACTION_TRUNCATE,
	CHUNKS_ERROR_ACTION_STAT,
	CHUNKS_ERROR_ACTION_CREATE_DIR,
	CHUNKS_ERROR_ACTION_READ_DIR,
	CHUNKS_ERROR_ACTION_REMOVE_DIR,
	/* A rename to the path. */
	CHUNKS_ERROR_ACTION_RENAME,
} ChunksErrorActionTag;

/* ChunksErrorAction is what a file operation did, which its failure's message names. */
typedef struct ChunksErrorAction
{
	ChunksErrorActionTag tag;
	union
	{
		StdPathBuf	rename;
	}			u;
} ChunksErrorAction;

/* ChunksErrorOwnTag is Own's variants. */
typedef enum ChunksErrorOwnTag
{
	CHUNKS_ERROR_OWN_CORRUPT,
	CHUNKS_ERROR_OWN_DAMAGED,
	CHUNKS_ERROR_OWN_MISSING,
	CHUNKS_ERROR_OWN_REFUSED,
	CHUNKS_ERROR_OWN_INVARIANT,
} ChunksErrorOwnTag;

/*
 * ChunksErrorOwn is one of chunks' own failures, which holds no error of the
 * host's or of code outside storage.
 */
typedef struct ChunksErrorOwn
{
	ChunksErrorOwnTag tag;
	union
	{
		StdString	corrupt;
		struct
		{
			StdPathBuf	path;
			uint64		offset;
			StdString	message;
		}			damaged;
		struct
		{
			ChunksAddressAddress f0;
			bool		has_f1;
			ChunksErrorReading f1;
		}			missing;
		StdString	refused;
		StdString	invariant;
	}			u;
} ChunksErrorOwn;

extern ChunksErrorError chunks_error_Error_FromError_from(HostError *error);
extern void chunks_error_Action_describe(const ChunksErrorAction *self,
										 const char *path, size_t path_len,
										 StdString *result);
extern ChunksErrorError chunks_error_Error_corrupt(StdString *message);
extern ChunksErrorError chunks_error_Error_corrupt_RefStr(const char *message, size_t message_len);
extern ChunksErrorError chunks_error_Error_refused(StdString *message);
extern void chunks_error_Error_into_finding(ChunksErrorError *self,
											StdString *result);
extern bool chunks_error_Error_own(ChunksErrorError *self,
								   ChunksErrorOwn *result,
								   ChunksErrorError *error);
extern ChunksErrorError chunks_error_Error_reading(ChunksErrorError *self,
												   ChunksErrorReading reading);
extern bool chunks_error_Error_Display_fmt(const void *self, StdFormatter *f);
extern bool chunks_error_Error_Debug_fmt(const void *self, StdFormatter *f);
extern void chunks_error_Error_drop(ChunksErrorError *self);
extern void chunks_error_Own_drop(ChunksErrorOwn *self);
extern bool chunks_error_Reading_Debug_fmt(const void *value, StdFormatter *f);
extern void chunks_error_Action_drop(ChunksErrorAction *self);

/*
 * CHUNKS_CONTRACT is contract!: in builds with assertions, the condition
 * asserted with its message, panicking at the macro's line and column of
 * the including file's RUST_FILE.
 */
#ifdef USE_ASSERT_CHECKING
#define CHUNKS_CONTRACT(line, col, cond, literal) \
	do { \
		if (!(cond)) \
			std_panic(RUST_FILE, (line), (col), (literal), sizeof(literal) - 1); \
	} while (0)
#else
#define CHUNKS_CONTRACT(line, col, cond, literal) ((void) 0)
#endif

extern void test_chunks_error_tests_a_failed_allocation_in_a_lossy_description_leaks_nothing(void);
extern void test_chunks_error_tests_a_failed_allocation_in_a_lossy_sqlstate_leaks_nothing(void);

extern void test_chunks_error_tests_file_actions_read_as_postgres_messages_do(void);
extern void test_chunks_error_tests_a_seams_failure_reads_as_its_message(void);

#endif							/* DOLTGRES_CHUNKS_ERROR_H */
