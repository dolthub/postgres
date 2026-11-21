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
 * A version's writer's outcome (txn's outcome.rs), from the version's hint
 * when it has one and from Postgres' commit log otherwise.
 */
#ifndef DOLTGRES_TXN_OUTCOME_H
#define DOLTGRES_TXN_OUTCOME_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/txn/memtable/version.h"

extern bool txn_outcome_outcome(HostIoIo *io, TxnMemtableVersionVersion *version, HostStatus *result,
								HostError *error);
extern bool txn_outcome_stamped(HostIoIo *io, uint64 xid, HostStatus *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_outcome_tests_a_writers_outcome_answers_frozen_with_no_door_and_caches_an_ended_writers(void);
#endif

#endif							/* DOLTGRES_TXN_OUTCOME_H */
