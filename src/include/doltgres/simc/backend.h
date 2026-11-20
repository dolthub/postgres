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
 * The backend's state the test modules' runner saves and restores for simc
 * (the port text's section 13), which the runner's C defines, since
 * Postgres' globals are data a Windows module imports, and simc calls.
 */
#ifndef DOLTGRES_SIMC_BACKEND_H
#define DOLTGRES_SIMC_BACKEND_H

#include "miscadmin.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#include "doltgres/std/unwind.h"

/*
 * SimcBackend is the backend's state each of a run's actors keeps as its own
 * across the handoffs inside simc's host calls: the C port's unwinding and
 * panic state, and Postgres' memory context, error context, exception stack,
 * and stack base.
 */
typedef struct SimcBackend
{
	StdUnwind  *unwind_top;
	size_t		panic_count;
	StdPayload	payload;
	void	   *payload_box;
#ifndef WIN32
	void	   *exception;
#endif
	bool		in_panic_hook;
	MemoryContext memory_context;
	ErrorContextCallback *error_context;
	sigjmp_buf *exception_stack;
	pg_stack_base_t stack_base;
} SimcBackend;

extern SimcBackend simc_backend_save(void);
extern SimcBackend simc_backend_fresh(void);
extern void simc_backend_restore(const SimcBackend *saved);

#endif							/* DOLTGRES_SIMC_BACKEND_H */
