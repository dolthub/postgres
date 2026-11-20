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
 * The frontend build's binding: its wrappers' error, the forms of the
 * callbacks its entry hands the check, and the runner of an entry, which
 * turns a failure or a panic into a message in the main's buffer.
 */
#ifndef DOLTGRES_PG_FRONTEND_H
#define DOLTGRES_PG_FRONTEND_H

#include "fe_calls.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/str.h"

/* PgFrontendFeError is FeError: a frontend wrapper's failure, the errno its call set. */
typedef struct PgFrontendFeError
{
	int32		f0;
} PgFrontendFeError;

/* PgFrontendLine is Line: the callback through which the check reports a line to the program's main. */
typedef struct PgFrontendLine
{
	DgLine		f0;
} PgFrontendLine;

/* PgFrontendKept is Kept: the callback through which the check hands the program's main a WAL range a fallback would replay. */
typedef struct PgFrontendKept
{
	DgKept		f0;
} PgFrontendKept;

/* PgFrontendBody is a frontend entry's body: it checks a data directory, reporting through the callbacks, or fails with a message. */
typedef bool (*PgFrontendBody) (const char *datadir, const PgFrontendLine *line, const PgFrontendKept *kept,
								StdString *error);

extern void pg_frontend_FeError_text(PgFrontendFeError self, StdString *result);
extern void pg_frontend_Line_report(const PgFrontendLine *self, int32 kind, const char *line);
extern void pg_frontend_Kept_hand(const PgFrontendKept *self, uint32 repository, uint64 from, bool oldest,
								  bool found);
extern bool pg_frontend_run_frontend(const char *datadir, DgLine line, DgKept kept, DgBuffer failure,
									 PgFrontendBody body);

extern bool pg_frontend_FeError_Debug_fmt(const void *value, StdFormatter *f);

#ifdef DOLTGRES_TEST_PG
extern void test_pg_tests_a_file_opens_reads_and_closes(void);
extern void test_pg_tests_a_directory_lists_its_names_and_asks_for_room(void);
extern void test_pg_tests_a_missing_file_fails_with_its_errno_and_text(void);
extern void test_pg_tests_the_version_directory_is_the_major_versions(void);
extern void test_pg_tests_a_failure_is_written_cut_to_the_buffer_and_terminated(void);
#endif

#endif							/* DOLTGRES_PG_FRONTEND_H */
