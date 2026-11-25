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
 * The pooled records' doors over the fork's region (pghost's shared.rs,
 * design item 14's S2).
 */
#ifndef DOLTGRES_PGHOST_SHARED_H
#define DOLTGRES_PGHOST_SHARED_H

#ifndef FRONTEND

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/pg/lib.h"
#include "doltgres/shmem/records.h"
#include "doltgres/std/sync.h"

extern StdRwLockWriteGuard pghost_shared_own(StdRwLock *list, void *data);
extern bool pghost_shared_limits(const PgMayRaise *raise, HostSharedLimits *result, PgPgError *error);
extern bool pghost_shared_held(uint64 id, uint64 *result, HostError *error);
extern bool pghost_shared_kinds(const PgMayRaise *raise, const HostSharedKind *table, size_t table_len,
								StdVec *result, PgPgError *error);
extern void pghost_shared_placed(uint64 place, size_t *result_0, uint32 *result_1);
extern bool pghost_shared_make(const HostMayRaise *raise, uint64 id, bool *has_result, uint64 *result,
							   HostError *error);
extern uint64 pghost_shared_publish(uint64 place);
extern bool pghost_shared_claim(uint64 id, uint64 location, bool *has_result, uint64 *result, HostError *error);
extern void pghost_shared_free(uint64 place);
extern void pghost_shared_give_up(uint64 place);
extern bool pghost_shared_open(const HostMayRaise *raise, uint64 id, uint64 location, bool *has_result,
							   uint64 *result, HostError *error);
extern void pghost_shared_close(uint64 place);
extern bool pghost_shared_reclaim(uint64 id, uint64 location, HostSharedLooked *result, HostError *error);
extern bool pghost_shared_reclaimed(uint64 place);
extern void pghost_shared_unreclaim(uint64 place);
extern bool pghost_shared_sweep(const HostMayRaise *raise, uint64 id, size_t atomic, StdVec *result,
								HostError *error);
extern bool pghost_shared_single(uint64 id, uint64 *result, HostError *error);
extern uint64 pghost_shared_location(uint64 place);
extern uint64 pghost_shared_placed_aged(uint32 aged, uint32 index);
extern bool pghost_shared_live(uint64 location);
extern bool pghost_shared_gone(uint64 id, uint64 location, bool *result, HostError *error);
extern bool pghost_shared_mine(uint64 id, uint64 *result, HostError *error);
extern bool pghost_shared_load(uint64 id, uint32 proc, size_t word, uint64 *result, HostError *error);
extern bool pghost_shared_place_of(uint64 id, uint32 proc, uint64 *result, HostError *error);
extern bool pghost_shared_processes(uint64 id, StdVec *result, HostError *error);
extern bool pghost_shared_at_exit(const StdTypeId *id, bool (*run) (HostIoIo *io, HostError *error),
								  HostError *error);
extern bool pghost_shared_exit_step(const PgMayRaise *raise, HostError *error);
extern void pghost_shared_exit(void);

#ifdef DOLTGRES_TEST_PGHOST
extern void test_pghost_shared_tests_the_holders_field_holds_every_process(void);
extern void test_pghost_shared_tests_a_place_is_a_kind_and_a_number(void);
#endif

#endif							/* FRONTEND */

#endif							/* DOLTGRES_PGHOST_SHARED_H */
