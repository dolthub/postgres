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
 * A heap row's columns (access' heaptuple.rs), the forms of it other crates'
 * ports call before access's port defines them.
 */
#ifndef DOLTGRES_ACCESS_HEAPTUPLE_H
#define DOLTGRES_ACCESS_HEAPTUPLE_H

#ifndef FRONTEND
#include "doltgres/pg/lib.h"
#include "doltgres/std/str.h"

/* AccessHeaptupleStorage is Storage: how a column's values may be kept, Postgres' attstorage. */
typedef enum AccessHeaptupleStorage
{
	ACCESS_HEAPTUPLE_STORAGE_PLAIN,
	ACCESS_HEAPTUPLE_STORAGE_EXTERNAL,
	ACCESS_HEAPTUPLE_STORAGE_MAIN,
	ACCESS_HEAPTUPLE_STORAGE_EXTENDED,
} AccessHeaptupleStorage;

/*
 * AccessHeaptupleColumn is Column: a row's column, its type, length,
 * by-value flag, alignment, storage, whether it is dropped, and the field it
 * stands for, if any.
 */
typedef struct AccessHeaptupleColumn
{
	uint32		typid;
	int16		len;
	bool		by_value;
	size_t		align;
	AccessHeaptupleStorage storage;
	bool		dropped;
	bool		has_field;
	size_t		field;
} AccessHeaptupleColumn;

/* AccessHeaptupleSetTag is Set's variants. */
typedef enum AccessHeaptupleSetTag
{
	ACCESS_HEAPTUPLE_SET_NULL,
	ACCESS_HEAPTUPLE_SET_WORD,
	ACCESS_HEAPTUPLE_SET_FIXED,
	ACCESS_HEAPTUPLE_SET_VARLENA,
} AccessHeaptupleSetTag;

/* AccessHeaptupleSet is Set: a column's value as a row's forming takes it, borrowed. */
typedef struct AccessHeaptupleSet
{
	AccessHeaptupleSetTag tag;
	union
	{
		uint64		word;
		StdBytes	fixed;
		StdBytes varlena;
	}			u;
} AccessHeaptupleSet;

extern bool access_heaptuple_Storage_of(uint8 byte, AccessHeaptupleStorage *result, PgPgError *error);
#endif

#endif							/* DOLTGRES_ACCESS_HEAPTUPLE_H */
