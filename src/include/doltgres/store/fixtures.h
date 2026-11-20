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
 * What the store's tests share: a repository made as a restart leaves it,
 * and the forms of the reports the simulation took.
 */
#ifndef DOLTGRES_STORE_FIXTURES_H
#define DOLTGRES_STORE_FIXTURES_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/path.h"

extern uint32 store_fixtures_id(uint32 number);
extern void store_fixtures_made(HostIoIo *io, uint32 number);
extern StdPathBuf store_fixtures_manifest(uint32 number);
extern bool store_tests_Sqlstate_Debug_fmt(const void *value, StdFormatter *f);
extern bool store_tests_Logs_Debug_fmt(const void *value, StdFormatter *f);
extern bool store_tests_Warnings_Debug_fmt(const void *value, StdFormatter *f);
extern void store_tests_Logs_drop(StdVec *logs);
extern void store_tests_Warnings_drop(StdVec *warnings);
extern void store_tests_ended(const char *file, const SimcReport *report, uint32 line, uint32 col);
extern void store_tests_kept_eq(const char *file, bool has, HostLsn from, HostLsn to, uint32 line, const char *message);
extern void store_tests_warned_once(const char *file, StdVec *warnings, const char *needle, uint32 line);
extern void store_tests_raised_eq(const char *file, HostError *error, const char *code, const char *message,
								  size_t message_len, uint32 line, uint32 col);

#endif							/* DOLTGRES_STORE_FIXTURES_H */
