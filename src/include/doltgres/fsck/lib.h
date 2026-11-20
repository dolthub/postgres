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
 * pg_checkversioned's check: the frontend entry that checks a stopped
 * server's data directory through fehost's reads and store's checks.
 */
#ifndef DOLTGRES_FSCK_LIB_H
#define DOLTGRES_FSCK_LIB_H

#include "doltgres/pg/frontend.h"

/* FSCK_ENTRIES_FOUND, FSCK_ENTRIES_SETTLED, and FSCK_ENTRIES_NOTE are the kinds of line the main prints: found always, the others with -v. */
#define FSCK_ENTRIES_FOUND 0
#define FSCK_ENTRIES_SETTLED 1
#define FSCK_ENTRIES_NOTE 2

extern bool fsck_entries_doltgres_fsck(const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
									   StdString *error);

#endif							/* DOLTGRES_FSCK_LIB_H */
