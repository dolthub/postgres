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
 * The rewrite of a table under its new primary key (doltgres' rekey.rs): the
 * new storage its row versions are laid out into.
 */
#ifndef DOLTGRES_DOLTGRES_REKEY_H
#define DOLTGRES_DOLTGRES_REKEY_H

#include "doltgres/doltgres/rewrite.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/version.h"

/*
 * doltgres_TupleVecU8OpWriterShownLockMode_t is a (Vec<u8>, Op, Writer,
 * Shown, LockMode): a carried version's key, its op, its writer, the command
 * it showed, and its row lock.
 */
typedef struct doltgres_TupleVecU8OpWriterShownLockMode_t
{
	StdVec		f0;
	TxnMemtableOp f1;
	TxnMemtableVersionWriter f2;
	TxnMemtableVersionShown f3;
	TxnMemtableLockMode f4;
} doltgres_TupleVecU8OpWriterShownLockMode_t;

/*
 * DoltgresRekeyRekeyed is Rekeyed: the new storage the rewrite makes, its
 * table tree's entries, its tuple id tree's pairs, their sizes, each carried
 * version, and the out-of-line values its versions name, each with its map
 * entry and how many name it (a Vec sorted by id, as its BTreeMap iterates).
 */
typedef struct DoltgresRekeyRekeyed
{
	/* rows is a Vec<Entry>. */
	StdVec		rows;
	/* tuple_ids is a Vec<Entry>. */
	StdVec		tuple_ids;
	TxnBaseSizes sizes;
	/* carried is a Vec<(Vec<u8>, Op, Writer, Shown, LockMode)>. */
	StdVec		carried;
	/* values is a Vec<(u32, (Entry, u64))>. */
	StdVec		values;
} DoltgresRekeyRekeyed;

extern void doltgres_rekey_Rekeyed_drop(DoltgresRekeyRekeyed *self);
extern bool doltgres_rekey_rekeyed(const PgCatalogCaps *caps, HostIoIo *io, DoltgresRewriteReformer *reformer,
								   const DoltgresRewriteRead *read, bool keyless, DoltgresRekeyRekeyed *result,
								   PgPgError *error);

#endif							/* DOLTGRES_DOLTGRES_REKEY_H */
