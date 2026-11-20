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
 * The session's repository: the repository of the database a backend is
 * connected to, held until it exits.
 */
#ifndef DOLTGRES_STORE_SESSION_H
#define DOLTGRES_STORE_SESSION_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/sync.h"

/* StoreSessionHeld is Option<Arc<Repo>>: the backend's session repository once it opened it. */
typedef struct StoreSessionHeld
{
	bool		has;
	StdArcInner *repo;
} StoreSessionHeld;

extern bool store_session_session_repo(HostIoIo *io, StdArcInner **result, HostError *error);

#ifdef DOLTGRES_TEST_STORE
extern void test_store_session_tests_calls_in_one_backend_share_its_repository(void);
extern void test_store_session_tests_a_failed_open_stores_nothing(void);
extern void test_store_session_tests_a_refused_repository_raises_its_refusal(void);
extern void test_store_session_tests_a_process_with_no_database_has_no_session_repository(void);
extern void test_store_session_tests_a_backends_exit_appends_its_session_repositorys_deferred_adds(void);
#endif

#endif							/* DOLTGRES_STORE_SESSION_H */
