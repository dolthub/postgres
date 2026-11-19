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
 * The read of the WAL kept for a manifest's fallback (design item 6), which a
 * checkpoint and the startup's open of repositories make, and a standby's read
 * of the fork's records past a storage's reached fold point (PLAN item 8's
 * "Standbys' roots").
 */
#ifndef DOLTGRES_KEPT_WAL_H
#define DOLTGRES_KEPT_WAL_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"

/*
 * doltgres_warn_kept_wal reads the WAL between two positions, on a timeline and
 * up to a position, warning of what it can't read.
 */
extern void doltgres_warn_kept_wal(XLogRecPtr from, XLogRecPtr to, TimeLineID timeline, XLogRecPtr upto);

/*
 * doltgres_next_fork_record reads the first record of a resource manager whose
 * kind is in a mask past a record's end, writing it into a buffer, and returns
 * the bytes it needs, or 0 for none.
 */
extern uint64 doltgres_next_fork_record(XLogRecPtr from, RmgrId rmid, uint32 kinds, char *data, size_t len);

#endif							/* DOLTGRES_KEPT_WAL_H */
