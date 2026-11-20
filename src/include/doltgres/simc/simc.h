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
 * sim's test API for the C port's tests (the port text's section 13): the
 * functions simc, the Rust test crate, exports over the simulation's host,
 * which it installs as the C port's host.
 */
#ifndef DOLTGRES_SIMC_SIMC_H
#define DOLTGRES_SIMC_SIMC_H

#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/simc/backend.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/io.h"
#include "doltgres/std/path.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

/*
 * SimcTestIo is sim's TestIo: a test's capability, the C port's Io, and its
 * hold of the test process, a Box<Process> the drop releases.
 */
typedef struct SimcTestIo
{
	HostIoIo	io;
	void	   *process;
} SimcTestIo;

/* SimcTaken is sim's Taken: a snapshot a test took, which simc_Taken_lent lends, a Box<Taken>. */
typedef struct SimcTaken
{
	void	   *f0;
} SimcTaken;

/* SimcTempDir is sim's TempDir: a directory a test made, which its drop removes, a Box<TempDir>. */
typedef struct SimcTempDir
{
	void	   *f0;
} SimcTempDir;

/* SimcSim is an Arc<sim::Sim>: the storage simulation over a directory's files. */
typedef struct SimcSim
{
	void	   *f0;
} SimcSim;

/* SimcRun is sim's Run: actors the seed or a script interleaves, a Box<Run>. */
typedef struct SimcRun
{
	void	   *f0;
} SimcRun;

/*
 * SimcActor is an actor's body, a Box<dyn FnOnce(&mut Io) -> Result<(),
 * host::Error> + Send>: its call, which consumes its captures and gives the
 * actor's capability, its captures, and their drop glue, size, and alignment.
 */
typedef struct SimcActor
{
	bool		(*call_once) (void *captures, HostIoIo *io, HostError *error);
	void	   *captures;
	void		(*drop) (void *captures);
	size_t		size;
	size_t		align;
} SimcActor;

/* SimcStep is a step of a run's script: an actor, by its name's UTF-8 bytes, and how many yields it runs for. */
typedef struct SimcStep
{
	const char *name;
	size_t		name_len;
	uint64		yields;
} SimcStep;

/* SimcEndingTag is sim's Ending's variants. */
typedef enum SimcEndingTag
{
	SIMC_ENDING_RETURNED,
	SIMC_ENDING_TERMINATED,
} SimcEndingTag;

/*
 * SimcEnding is sim's Ending: how an actor ended, its body's result, an error
 * as its SQLSTATE and message, or its termination.
 */
typedef struct SimcEnding
{
	SimcEndingTag tag;
	bool		is_err;
	uint8		sqlstate[5];
	StdString	message;
} SimcEnding;

/* simc_TupleStringEnding_t is the tuple (String, Ending): an actor's name and how it ended. */
typedef struct simc_TupleStringEnding_t
{
	StdString	f0;
	SimcEnding	f1;
} simc_TupleStringEnding_t;

/* SimcOutcomeTag is sim's Outcome's variants. */
typedef enum SimcOutcomeTag
{
	SIMC_OUTCOME_ENDED,
	SIMC_OUTCOME_CRASHED,
	SIMC_OUTCOME_FAILED,
} SimcOutcomeTag;

/* SimcOutcome is sim's Outcome: every actor's ending, a crash, or a failure's message. */
typedef struct SimcOutcome
{
	SimcOutcomeTag tag;
	union
	{
		StdVec		ended;
		StdString	failed;
	}			u;
} SimcOutcome;

/*
 * simc_TupleStringSqlstateString_t is the tuple (String, [u8; 5], String): an
 * actor's name and a failed exit work's SQLSTATE and message.
 */
typedef struct simc_TupleStringSqlstateString_t
{
	StdString	f0;
	uint8		f1[5];
	StdString	f2;
} simc_TupleStringSqlstateString_t;

/* simc_TupleSqlstateString_t is the tuple ([u8; 5], String): a report's SQLSTATE and its message. */
typedef struct simc_TupleSqlstateString_t
{
	uint8		f0[5];
	StdString	f1;
} simc_TupleSqlstateString_t;

/*
 * SimcReport is sim's Report: the run's outcome, each exit work that failed,
 * and the Rust's report, a Box<Report>, which holds the rest and the test
 * process's shared memory.
 */
typedef struct SimcReport
{
	SimcOutcome outcome;
	/* exits is a Vec<(String, [u8; 5], String)>. */
	StdVec		exits;
	void	   *report;
} SimcReport;

extern SimcTestIo simc_io(void);
extern void simc_TestIo_drop(void *self);
extern bool simc_tempdir(SimcTempDir *result, StdIoError *error);
extern const char *simc_TempDir_path(const SimcTempDir *self, size_t *len);
extern void simc_TempDir_drop(void *self);
extern void simc_interrupt_at(uint64 checks);
extern void simc_begin_owner(void);
extern void simc_abort_owner(void);
extern StdDuration simc_napped(void);
extern void simc_set_disk_space(uint64 free, uint64 total);
extern void simc_refuse_owned_append(uint64 after);
extern void simc_refuse_open(HostDeltaPagePage name, uint64 after);
extern void simc_refuse_record(uint64 shape, uint64 location, uint64 after);
extern void simc_refuse_any_take(uint64 after);
extern void simc_refuse_nothing(void);
extern HostXid simc_begin(void);
extern HostXid simc_begin_under(HostXid parent);
extern void simc_commit(HostXid xid);
extern void simc_abort(HostXid xid);
extern SimcTaken simc_historic(HostXid xmin, HostXid xmax, const HostXid *committed, size_t committed_len,
							   const HostXid *own, size_t own_len, HostCommand command);
extern SimcTaken simc_snapshot(HostSnapshotKind kind, bool recovery);
extern HostSnapshot simc_Taken_lent(const SimcTaken *self);
extern void simc_Taken_drop(void *self);
extern size_t simc_scratch_files(void);
extern void simc_out_of_memory_at_next_decompress(void);
extern void simc_fail_allocation(uint64 nth);
extern void simc_connect(uint32 database, uint32 tablespace);
extern void simc_as_actor(uint64 actor, void (*body) (void *captures), void *captures);
extern void simc_checkpointed(HostLsn redo);
extern StdPathBuf simc_data_dir(void);
extern StdVec simc_take_warnings(void);
extern StdVec simc_take_logs(void);
extern bool simc_kept_wal_read(HostLsn *from, HostLsn *to);
extern SimcSim simc_Sim_start(const char *root, size_t root_len, uint64 seed);
extern void simc_Sim_stop(const SimcSim *self);
extern StdVec simc_Sim_trace(const SimcSim *self);
extern uint64 simc_Sim_calls(const SimcSim *self);
extern void simc_Sim_full_at(const SimcSim *self, uint64 call);
extern void simc_Sim_crash_at(const SimcSim *self, uint64 call);
extern bool simc_Sim_recover(const SimcSim *self, bool torn, StdIoError *error);
extern void simc_Sim_drop(void *self);
extern SimcRun simc_Run_new(uint64 seed);
extern void simc_Run_actor(SimcRun *self, const char *name, size_t name_len, SimcActor body);
extern void simc_Run_until(SimcRun *self, const char *name, size_t name_len, const char *point);
extern void simc_Run_script(SimcRun *self, const SimcStep *steps, size_t steps_len);
extern void simc_Run_terminate_at(SimcRun *self, const char *name, size_t name_len, uint64 yields);
extern SimcReport simc_Run_finish(SimcRun self);
extern void simc_Run_drop(void *self);
extern void simc_Report_drop(void *self);
extern bool simc_Report_Debug_fmt(const void *value, StdFormatter *f);
extern bool simc_Outcome_Debug_fmt(const void *value, StdFormatter *f);
extern bool simc_Ending_Debug_fmt(const void *value, StdFormatter *f);
extern bool simc_Ending_PartialEq_eq(const SimcEnding *self, const SimcEnding *other);
extern void simc_size_delta(uint64 budget);
extern uint64 simc_delta_holds(void);
extern uint64 simc_delta_waiting(void);
extern uint64 simc_delta_wakes(void);
extern void simc_unfreed(uint64 shape, size_t *records, size_t *pages);
extern void simc_size_shared(const size_t counts[HOST_CACHE_CLASSES_LEN]);
extern void simc_end(bool committed);
extern void simc_unanswered(bool recovering);
extern uint64 simc_delta_passes(void);
extern void simc_Run_check(SimcRun *self, bool (*check) (StdString *message));

#endif							/* DOLTGRES_SIMC_SIMC_H */
