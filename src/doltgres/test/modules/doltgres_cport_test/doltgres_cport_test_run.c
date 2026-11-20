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
 * The C port's test modules' runner (the port text's section 13): each test
 * in its own internal subtransaction, rolled back when it fails, so a failure
 * leaves nothing of Postgres' for the next.
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"

#include "doltgres/simc/backend.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/io.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#include "doltgres_cport_test.h"

/*
 * std_io__print is std's _print under libtest's output capture: the
 * formatted text written into the running test's capture, as
 * print_to_buffer_if_capture_used writes it, whose write's result is
 * ignored.
 */
void
std_io__print(const StdFmtArguments *args)
{
	if (!std_io_print_to_buffer_if_capture_used(args))
		elog(ERROR, "a C port test printed outside its runner");
}

/*
 * std_io__eprint is std's _eprint under libtest's output capture, which
 * takes stderr's output as it takes stdout's: the formatted text written
 * into the running test's capture, whose write's result is ignored.
 */
void
std_io__eprint(const StdFmtArguments *args)
{
	if (!std_io_print_to_buffer_if_capture_used(args))
		elog(ERROR, "a C port test printed outside its runner");
}

/*
 * doltgres_cport_test_run runs a test in its own internal subtransaction and returns
 * its outcome: "ok", libtest's message for a #[should_panic] test that
 * failed, or the panic's message or Postgres' error's followed by
 * the test's captured output.
 */
static char *
doltgres_cport_test_run(const CportTest *test)
{
	MemoryContext caller = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	char	   *outcome;
	StdOutputCapture capture = {STD_RW_LOCK_NEW};

	std_vec_new(&capture.buf, 1, 1);
	(void) std_io_set_output_capture(&capture);
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(caller);
	PG_TRY();
	{
		StdUnwind	unwind;
		StdCaught	caught;
		bool		passed = false;

		STD_UNWIND_PUSH_CATCHING(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			test->run();
			std_unwind_pop(&unwind);
			passed = true;
		}
		else
			caught = std_unwind_end(&unwind);
		outcome = doltgres_cport_test_judged(test, passed, &caught);
		if (passed)
			ReleaseCurrentSubTransaction();
		else
			RollbackAndReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		ErrorData  *error;

		MemoryContextSwitchTo(caller);
		error = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		outcome = psprintf("error: %s", error->message);
	}
	PG_END_TRY();
	MemoryContextSwitchTo(caller);
	CurrentResourceOwner = owner;
	(void) std_io_set_output_capture(NULL);
	outcome = doltgres_cport_test_with_output(outcome, &capture);
	std_vec_free_Checked(&capture.buf, 1, 1);
	return outcome;
}

/*
 * doltgres_cport_test_run_all runs a module's tests in order and returns each
 * one's name and outcome as the function's rows.
 */
Datum
doltgres_cport_test_run_all(FunctionCallInfo fcinfo, const CportTest *tests, size_t len)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	std_heap_make();
	for (size_t i = 0; i < len; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = CStringGetTextDatum(tests[i].name);
		values[1] = CStringGetTextDatum(doltgres_cport_test_run(&tests[i]));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * simc_backend_save returns the backend's state an actor keeps as its own,
 * read without changing it: set_stack_base gives the stack base it replaces,
 * which is put back at once.
 */
SimcBackend
simc_backend_save(void)
{
	SimcBackend saved = {
		.unwind_top = std_unwind_top,
		.panic_count = std_panic_count,
		.payload = std_payload,
		.payload_box = std_payload_box,
#ifndef WIN32
		.exception = std_exception,
#endif
		.in_panic_hook = std_local_in_panic_hook,
		.memory_context = CurrentMemoryContext,
		.error_context = error_context_stack,
		.exception_stack = PG_exception_stack,
	};

	saved.stack_base = set_stack_base();
	restore_stack_base(saved.stack_base);
	return saved;
}

/*
 * simc_backend_fresh returns the state an actor starts with on its own
 * thread: no landing pad, no panic, no error context or exception stack of
 * another thread's, its thread's stack as its stack base, which the second
 * set_stack_base returns, and the memory context the run's backend holds.
 */
SimcBackend
simc_backend_fresh(void)
{
	SimcBackend fresh = {
		.payload = STD_PAYLOAD_STR,
		.memory_context = CurrentMemoryContext,
	};
	pg_stack_base_t base = set_stack_base();

	fresh.stack_base = set_stack_base();
	restore_stack_base(base);
	return fresh;
}

/* simc_backend_restore puts back the backend's state an actor saved. */
void
simc_backend_restore(const SimcBackend *saved)
{
	std_unwind_top = saved->unwind_top;
	std_panic_count = saved->panic_count;
	std_payload = saved->payload;
	std_payload_box = saved->payload_box;
#ifndef WIN32
	std_exception = saved->exception;
#endif
	std_local_in_panic_hook = saved->in_panic_hook;
	CurrentMemoryContext = saved->memory_context;
	error_context_stack = saved->error_context;
	PG_exception_stack = saved->exception_stack;
	restore_stack_base(saved->stack_base);
}
