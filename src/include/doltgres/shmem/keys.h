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
 * The key lock's stripes (shmem's keys.rs, (4b)'s key lock): a region of
 * Postgres' main shared memory of SHMEM_KEYS_STRIPES lightweight locks in a
 * tranche of their own.
 */
#ifndef DOLTGRES_SHMEM_KEYS_H
#define DOLTGRES_SHMEM_KEYS_H

#ifndef FRONTEND

#include "doltgres/pg/lib.h"

/* SHMEM_KEYS_STRIPES is the count of the stripes, which pghost holds equal to host's by a constant assertion. */
#define SHMEM_KEYS_STRIPES ((size_t) 4096)

extern bool shmem_keys_request(const PgMayRaise *raise, PgPgError *error);
extern bool shmem_keys_initialize(const PgMayRaise *raise, PgPgError *error);
extern bool shmem_keys_lock(const PgMayRaise *raise, const uint32 *stripes, size_t stripes_len, PgPgError *error);
extern void shmem_keys_unlock(const uint32 *stripes, size_t stripes_len, bool pushed);
extern bool shmem_keys_hold(uint32 at);
extern void shmem_keys_release(uint32 at);
extern bool shmem_keys_held_wait(const PgMayRaise *raise, uint32 at, PgPgError *error);
extern void shmem_keys_exit(void);
extern uint64 shmem_keys_count(uint32 at);

#endif							/* FRONTEND */

#endif							/* DOLTGRES_SHMEM_KEYS_H */
