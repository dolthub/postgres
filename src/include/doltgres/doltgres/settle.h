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
 * The backend's list of the tables whose primary key a statement changed
 * (doltgres' settle.rs): its utility statements' levels, each the tables
 * its statement's sites named, its subtransactions' marks, the transient
 * tables' keys and rebuilt indexes, the relations whose new relfilenode keeps
 * their frozen xid, and each named table's old key columns.
 */
#ifndef DOLTGRES_DOLTGRES_SETTLE_H
#define DOLTGRES_DOLTGRES_SETTLE_H

#include "doltgres/access/key.h"
#include "doltgres/encoding/key.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/txn/base.h"

/* doltgres_TupleUsizeOrderKeyType_t is one of Columns: a key column's position, its order, and its key type. */
typedef struct doltgres_TupleUsizeOrderKeyType_t
{
	size_t		f0;
	EncodingKeyOrder f1;
	AccessKeyKeyType f2;
} doltgres_TupleUsizeOrderKeyType_t;

extern void doltgres_settle_Columns_drop(StdVec *columns);
extern bool doltgres_settle_opened(HostIoIo *io, HostError *error);
extern bool doltgres_settle_named(HostIoIo *io, uint32 table, uint32 subtransaction, HostError *error);
extern void doltgres_settle_closed(HostIoIo *io, StdVec *result);
extern bool doltgres_settle_pending(HostIoIo *io, StdVec *result, HostError *error);
extern void doltgres_settle_ended(HostIoIo *io, bool aborted);
extern bool doltgres_settle_sub_started(HostIoIo *io, uint32 subtransaction, HostError *error);
extern void doltgres_settle_sub_ended(HostIoIo *io, uint32 subtransaction, bool aborted);
extern bool doltgres_settle_transient(HostIoIo *io, uint32 table, TxnBaseKey *key, uint32 index, HostError *error);
extern bool doltgres_settle_rebuilt(HostIoIo *io, uint32 table, uint32 *result);
extern bool doltgres_settle_transient_key(HostIoIo *io, uint32 table, TxnBaseKey *result);
extern bool doltgres_settle_keep_frozen(HostIoIo *io, uint32 relation, HostError *error);
extern bool doltgres_settle_kept_frozen(HostIoIo *io, uint32 relation);
extern bool doltgres_settle_is_pinned(HostIoIo *io, uint32 table);
extern bool doltgres_settle_pin(HostIoIo *io, uint32 table, StdVec *columns, HostError *error);
extern bool doltgres_settle_held(HostIoIo *io, uint32 table, StdVec *result);
extern bool doltgres_settle_pinned(HostIoIo *io, uint32 table, StdVec *result);

#endif							/* DOLTGRES_DOLTGRES_SETTLE_H */
