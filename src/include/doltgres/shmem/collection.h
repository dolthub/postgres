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
 * The collection's and the base backups' meeting point (shmem's
 * collection.rs), and a backend's ask of the collector worker.
 */
#ifndef DOLTGRES_SHMEM_COLLECTION_H
#define DOLTGRES_SHMEM_COLLECTION_H

#ifndef FRONTEND

#include "doltgres/pg/lib.h"
#include "doltgres/std/tuple.h"

/* SHMEM_COLLECTION_FAILED is each count of an answer whose collection failed, which the worker reported in its log. */
#define SHMEM_COLLECTION_FAILED PG_UINT64_MAX

extern bool shmem_collection_request(const PgMayRaise *raise, PgPgError *error);
extern bool shmem_collection_initialize(const PgMayRaise *raise, PgPgError *error);
extern bool shmem_collection_backup_begin(const PgMayRaise *raise, PgPgError *error);
extern void shmem_collection_backup_end(void);
extern bool shmem_collection_backups_running(void);
extern bool shmem_collection_collection_begin(void);
extern void shmem_collection_collection_end(void);
extern void shmem_collection_worker_started(void);
extern bool shmem_collection_worker_running(void);
extern bool shmem_collection_ask(const PgMayRaise *raise, uint32 database, uint32 emptied, uint64 result[3],
								 PgPgError *error);
extern bool shmem_collection_asked_for(std_TupleU64U32_t *result, uint32 *emptied);
extern void shmem_collection_answer(uint64 word, const uint64 counts[3]);

#endif							/* FRONTEND */

#endif							/* DOLTGRES_SHMEM_COLLECTION_H */
