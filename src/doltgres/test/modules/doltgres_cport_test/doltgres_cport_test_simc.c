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
 * simc's tests (the port text's section 13), the C side of the Rust test
 * crate's boundary, run in a backend: the paths of simc's entries that the
 * crates' tests over sim don't reach, each a case of the safety contract its
 * unsafe block names: a Rust panic raised again in the C, an operating
 * system's error crossing as the C's, an actor's C panic handed to sim's
 * runner, and a run's unrun bodies dropped.
 */
#include "postgres.h"

#include "fmgr.h"

#include "doltgres/host/lib.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/path.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres_cport_test.h"

PG_MODULE_MAGIC;

/* RUST_FILE is the file simc's tests' panics name, theirs alone, since no Rust file holds them. */
#define RUST_FILE "doltgres_cport_test_simc.c"

/* test_simc_tests_unported_call calls an entry simc doesn't serve yet, through the C port's host. */
static void
test_simc_tests_unported_call(void *captures)
{
	host_collector_started();
	*(bool *) captures = true;
}

/*
 * test_simc_tests_a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message:
 * an entry's Rust panic, an unserved entry's named error here, unwinds the C
 * to its catch with the panic's message as a String's payload.
 */
static void
test_simc_tests_a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message(void)
{
	static const char expected[] = "internal error: simc doesn't serve the simulation's host's collector_started yet";
	SimcTestIo	test_io = simc_io();
	bool		answered = false;
	StdCaught	caught;
	const char *text;
	size_t		len;

	STD_ASSERT(__LINE__, 5, !std_catch_unwind(test_simc_tests_unported_call, &answered, &caught),
			   "a call of an unserved entry panics");
	STD_ASSERT(__LINE__, 5, std_Caught_downcast_string(&caught, &text, &len), "the panic's payload is a String");
	STD_ASSERT(__LINE__, 5, len == sizeof(expected) - 1 && memcmp(text, expected, len) == 0,
			   "the panic's payload is the entry's named error");
	std_Caught_drop(&caught);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_failed_std_fs_call_returns_the_operating_systems_error:
 * a read of a missing file fails with the operating system's error, by its
 * code, whose kind is NotFound.
 */
static void
test_simc_tests_a_failed_std_fs_call_returns_the_operating_systems_error(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	error;
	StdPathBuf	missing;
	const char *dir_path;
	size_t		dir_path_len;
	StdVec		bytes;
	StdIoErrorData data;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "missing", 7, &missing);
	STD_ASSERT(__LINE__, 5, !std_fs_read(missing.bytes.ptr, missing.bytes.len, &bytes, &error), "a missing file reads");
	data = std_io_Error_data(&error);
	STD_ASSERT(__LINE__, 5, data.tag == STD_IO_ERROR_DATA_OS, "the error is the operating system's");
	STD_ASSERT(__LINE__, 5, std_sys_decode_error_kind(data.u.os) == STD_ERROR_KIND_NOT_FOUND, "the error is NotFound");
	std_vec_free_Checked(&missing.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_panicking_actor is an actor's body that panics with a literal message. */
static bool
test_simc_tests_panicking_actor(void *captures, HostIoIo *io, HostError *error)
{
	static const char text[] = "boom";

	std_panic(RUST_FILE, __LINE__, 5, text, sizeof(text) - 1);
}

/*
 * test_simc_tests_an_actors_c_panic_fails_its_run_with_its_message: an
 * actor's C panic is caught on its thread and handed to sim's runner as its
 * message alone, which fails the run with the actor's name, and its outcome's
 * Debug is the Rust's.
 */
static void
test_simc_tests_an_actors_c_panic_fails_its_run_with_its_message(void)
{
	static const char expected[] = "boomer panicked: boom";
	static const char debug[] = "Failed(\"boomer panicked: boom\")";
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	SimcActor	actor = {test_simc_tests_panicking_actor};
	SimcReport	report;
	StdString	formatted;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&report.outcome, simc_Outcome_Debug_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};

	simc_Run_actor(&run, "boomer", 6, actor);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_FAILED, "the run fails");
	STD_ASSERT(__LINE__, 5, report.outcome.u.failed.len == sizeof(expected) - 1 &&
			   memcmp(report.outcome.u.failed.ptr, expected, sizeof(expected) - 1) == 0,
			   "the run's failure names the actor and its message");
	std_fmt_format_Checked(&formatted, &arguments);
	STD_ASSERT(__LINE__, 5, formatted.len == sizeof(debug) - 1 && memcmp(formatted.ptr, debug, sizeof(debug) - 1) == 0,
			   "the outcome's Debug is the Rust's");
	std_vec_free_Checked(&formatted, 1, 1);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_unrun_actor is an actor's body the test never runs. */
static bool
test_simc_tests_unrun_actor(void *captures, HostIoIo *io, HostError *error)
{
	return true;
}

/* test_simc_tests_dropped_body is the unrun body's drop glue, which marks its captures dropped. */
static void
test_simc_tests_dropped_body(void *captures)
{
	**(bool **) captures = true;
}

/*
 * test_simc_tests_a_run_dropped_unfinished_drops_its_actors_bodies: a run
 * dropped before it finishes drops its actors' bodies unrun, their captures
 * through their drop glue and their boxes freed.
 */
static void
test_simc_tests_a_run_dropped_unfinished_drops_its_actors_bodies(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	bool		dropped = false;
	bool	  **captures = std_alloc(sizeof(bool *), _Alignof(bool *));
	SimcActor	actor = {test_simc_tests_unrun_actor, captures, test_simc_tests_dropped_body, sizeof(bool *),
	_Alignof(bool *)};

	*captures = &dropped;
	simc_Run_actor(&run, "unrun", 5, actor);
	simc_Run_drop(&run);
	STD_ASSERT(__LINE__, 5, dropped, "the unrun body's captures are dropped");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c:
 * the dictionaries simc prepares cross to the C as its own boxes, which a
 * compression and a decompression use and whose drops the C runs.
 */
static void
test_simc_tests_a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c(void)
{
	static const char content[] = "a dictionary of the words a chunk's leaves repeat, repeat, and repeat";
	static const char text[] = "the words a chunk's leaves repeat, again and again, repeat";
	SimcTestIo	test_io = simc_io();
	HostCompressDictionary compressing;
	HostDecompressDictionary decompressing;
	HostError	error;
	StdVec		frame;
	StdVec		bytes;
	uint64		size;
	bool		has;

	STD_ASSERT(__LINE__, 5, host_prepare_compressing((const uint8 *) content, sizeof(content) - 1, 3, &compressing,
													 &error), "a compression dictionary is prepared");
	STD_ASSERT(__LINE__, 5, host_compress((const uint8 *) text, sizeof(text) - 1, 3, &compressing, &frame, &error),
			   "the text compresses with it");
	STD_ASSERT(__LINE__, 5, host_frame_size(frame.ptr, frame.len, &size) && size == sizeof(text) - 1,
			   "the frame records the text's size");
	STD_ASSERT(__LINE__, 5, host_prepare_decompressing((const uint8 *) content, sizeof(content) - 1, &decompressing,
													   &error), "a decompression dictionary is prepared");
	STD_ASSERT(__LINE__, 5, host_decompress(frame.ptr, frame.len, sizeof(text) - 1, &decompressing, &has, &bytes,
											&error) && has, "the frame decompresses with it");
	STD_ASSERT(__LINE__, 5, bytes.len == sizeof(text) - 1 && memcmp(bytes.ptr, text, bytes.len) == 0,
			   "the frame decompresses to the text");
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&frame, 1, 1);
	host_DecompressDictionary_drop(&decompressing);
	host_CompressDictionary_drop(&compressing);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_hinted_actor is an actor's body that fails with a hinted error, as host_Error_hinted makes one. */
static bool
test_simc_tests_hinted_actor(void *captures, HostIoIo *io, HostError *error)
{
	*error = host_Error_hinted((const uint8 *) "55000", "the vector is busy", 18, "Retry the collection.", 21);
	return false;
}

/*
 * test_simc_tests_a_hinted_c_error_crosses_to_sim_as_its_literals: an
 * actor's hinted C error, its message and hint the C's literals, crosses to
 * sim as a hinted error of the same literals, which the run reports as its
 * SQLSTATE and message.
 */
static void
test_simc_tests_a_hinted_c_error_crosses_to_sim_as_its_literals(void)
{
	static const char expected[] = "the vector is busy";
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	SimcActor	actor = {test_simc_tests_hinted_actor};
	SimcReport	report;
	const simc_TupleStringEnding_t *ending;

	simc_Run_actor(&run, "hinter", 6, actor);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED && report.outcome.u.ended.len == 1,
			   "the run ends");
	ending = report.outcome.u.ended.ptr;
	STD_ASSERT(__LINE__, 5, ending->f1.tag == SIMC_ENDING_RETURNED && ending->f1.is_err &&
			   memcmp(ending->f1.sqlstate, "55000", 5) == 0, "the actor fails with its SQLSTATE");
	STD_ASSERT(__LINE__, 5, ending->f1.message.len == sizeof(expected) - 1 &&
			   memcmp(ending->f1.message.ptr, expected, sizeof(expected) - 1) == 0,
			   "the actor fails with its message");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_each_served_entry_answers_as_the_simulations_host: each
 * entry simc serves, reached through the C port's own host functions,
 * answers as the simulation's host does: the files, directories, and sizes
 * of a directory's seams, the clock, the process's numbers, the actor's
 * seams, the count of base backups, and a dictionary's training.
 */
static void
test_simc_tests_each_served_entry_answers_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(&test_io.io);
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(&test_io.io);
	SimcTempDir dir;
	StdIoError	io_error;
	HostError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	a;
	StdPathBuf	b;
	StdPathBuf	sub;
	StdPathBuf	missing;
	StdPathBuf	tablespace;
	HostFile	file;
	HostScratchFile scratch;
	StdVec		names;
	uint8		buffer[5];
	uint64		size;
	uint64		free_bytes;
	uint64		total_bytes;
	uint64		tree;
	uint32		database;
	uint32		in_tablespace;
	bool		has;
	bool		there;
	StdDuration nap = {0, 1000};
	uint8		sample_bytes[256][64];
	StdBytes	samples[256];
	StdVec		trained;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &io_error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a", 1, &a);
	std_path_join(dir_path, dir_path_len, "b", 1, &b);
	std_path_join(dir_path, dir_path_len, "sub", 3, &sub);
	std_path_join(dir_path, dir_path_len, "missing", 7, &missing);
	STD_ASSERT(__LINE__, 5, host_make_dir(io, sub.bytes.ptr, sub.bytes.len, &error), "a directory is made");
	STD_ASSERT(__LINE__, 5, host_create_if_absent(io, a.bytes.ptr, a.bytes.len, &has, &file, &error) && has,
			   "a file is created");
	STD_ASSERT(__LINE__, 5, host_write_at(io, &file, 0, (const uint8 *) "bytes", 5, &error), "the file is written");
	STD_ASSERT(__LINE__, 5, host_truncate(io, &file, 5, &error), "the file is cut to its bytes");
	STD_ASSERT(__LINE__, 5, host_sync(io, &file, &error), "the file is synced");
	host_prefetch(&file, 0, 5);
	host_File_drop(&file);
	STD_ASSERT(__LINE__, 5, host_open_to_read(read, a.bytes.ptr, a.bytes.len, &file, &error), "the file opens to read");
	STD_ASSERT(__LINE__, 5, host_size(read, &file, &size, &error) && size == 5, "the file holds its bytes");
	STD_ASSERT(__LINE__, 5, host_read_at(read, &file, 0, buffer, sizeof(buffer), &error) &&
			   memcmp(buffer, "bytes", 5) == 0, "the file reads back its bytes");
	host_File_drop(&file);
	STD_ASSERT(__LINE__, 5, host_open(io, a.bytes.ptr, a.bytes.len, HOST_OPEN_READ_WRITE, &file, &error),
			   "the file opens to write");
	host_File_drop(&file);
	STD_ASSERT(__LINE__, 5, host_open_if_there(read, missing.bytes.ptr, missing.bytes.len, &has, &file, &error) &&
			   !has, "a missing file opens as none");
	STD_ASSERT(__LINE__, 5, !host_open_to_read(read, missing.bytes.ptr, missing.bytes.len, &file, &error),
			   "a missing file fails to open");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, host_list_dir(read, dir_path, dir_path_len, &names, &error) && names.len == 2,
			   "the directory lists its two entries");
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	STD_ASSERT(__LINE__, 5, host_list_dir_if_there(read, missing.bytes.ptr, missing.bytes.len, &has, &names, &error) &&
			   !has, "a missing directory lists as none");
	STD_ASSERT(__LINE__, 5, host_list_dir_if_there(read, a.bytes.ptr, a.bytes.len, &has, &names, &error) && !has,
			   "a file lists as none");
	STD_ASSERT(__LINE__, 5, !host_list_dir_if_there(read, "a\0b", 3, &has, &names, &error),
			   "a path holding a nul lists as an error");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, host_sync_path(io, dir_path, dir_path_len, true, &error), "the directory is synced");
	STD_ASSERT(__LINE__, 5, host_rename(io, a.bytes.ptr, a.bytes.len, b.bytes.ptr, b.bytes.len, &error),
			   "the file is renamed");
	STD_ASSERT(__LINE__, 5, host_rename_durably(io, b.bytes.ptr, b.bytes.len, a.bytes.ptr, a.bytes.len, &error),
			   "the file is renamed back durably");
	STD_ASSERT(__LINE__, 5, host_rename_back(io, a.bytes.ptr, a.bytes.len, b.bytes.ptr, b.bytes.len),
			   "the file is renamed back");
	STD_ASSERT(__LINE__, 5, host_remove_durably_if_there(io, missing.bytes.ptr, missing.bytes.len, &there, &error) &&
			   !there, "a missing file is removed as none");
	STD_ASSERT(__LINE__, 5, host_remove_durably(io, b.bytes.ptr, b.bytes.len, &error), "the file is removed");
	host_remove_leftover(io, missing.bytes.ptr, missing.bytes.len);
	STD_ASSERT(__LINE__, 5, host_disk_space(io, dir_path, dir_path_len, &free_bytes, &total_bytes, &error) &&
			   free_bytes <= total_bytes, "the volume has its space");
	STD_ASSERT(__LINE__, 5, host_tree_bytes(io, dir_path, dir_path_len, &tree, &error), "the tree's bytes are read");
	STD_ASSERT(__LINE__, 5, host_remove_tree(io, sub.bytes.ptr, sub.bytes.len, &there, &error) && there,
			   "the directory is removed");
	STD_ASSERT(__LINE__, 5, host_open_scratch(&test_io.io, &scratch, &error), "a scratch file opens");
	host_ScratchFile_drop(&scratch);
	STD_ASSERT(__LINE__, 5, host_nap(io, nap, &error), "the backend naps");
	STD_ASSERT(__LINE__, 5, host_io_Io_check(&test_io.io, &error), "no interrupt is pending");
	tablespace = host_tablespace_dir(1, 1663);
	std_vec_free_Checked(&tablespace.bytes, 1, 1);
	(void) host_now();
	(void) host_instant();
	(void) host_process_id();
	STD_ASSERT(__LINE__, 5, !host_session_database(&database, &in_tablespace, &error),
			   "the test's process is connected to no database");
	host_Error_drop(&error);
	simc_connect(5, 1663);
	STD_ASSERT(__LINE__, 5, host_session_database(&database, &in_tablespace, &error) && database == 5 &&
			   in_tablespace == 1663, "the connected backend's database is its session's");
	STD_ASSERT(__LINE__, 5, !host_exiting(), "the backend isn't exiting");
	STD_ASSERT(__LINE__, 5, host_unique() != host_unique(), "each unique number differs");
	STD_ASSERT(__LINE__, 5, !host_acting(), "no run's actor runs");
	STD_ASSERT(__LINE__, 5, !host_backups_running(), "no base backup runs");
	host_yield_point();
	for (size_t i = 0; i < 256; i++)
	{
		for (size_t j = 0; j < 64; j++)
			sample_bytes[i][j] = (uint8) ('A' + (i * 7 + j * 13) % 61);
		samples[i].ptr = sample_bytes[i];
		samples[i].len = 64;
	}
	STD_ASSERT(__LINE__, 5, host_train(samples, 256, 4096, &trained) && trained.len <= 4096,
			   "a dictionary trains within its capacity");
	std_vec_free_Checked(&trained, 1, 1);
	std_vec_free_Checked(&missing.bytes, 1, 1);
	std_vec_free_Checked(&sub.bytes, 1, 1);
	std_vec_free_Checked(&b.bytes, 1, 1);
	std_vec_free_Checked(&a.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_out_of_memory_actor is an actor's body that fails with the C's out-of-memory error, a borrowed literal. */
static bool
test_simc_tests_out_of_memory_actor(void *captures, HostIoIo *io, HostError *error)
{
	*error = host_Error_out_of_memory();
	return false;
}

/* test_simc_tests_formatted_actor is an actor's body that fails with a fork error of a formatted, owned message. */
static bool
test_simc_tests_formatted_actor(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the body failed")};
	StdFmtArguments arguments = {pieces, 1, NULL};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/*
 * test_simc_tests_an_unhinted_c_error_crosses_to_sim_as_its_message: an
 * actor's unhinted C error, its message borrowed or owned, crosses to sim as
 * a fork error of the same SQLSTATE and message.
 */
static void
test_simc_tests_an_unhinted_c_error_crosses_to_sim_as_its_message(void)
{
	static const char borrowed[] = "out of memory";
	static const char owned[] = "the body failed";
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	SimcActor	short_of_memory = {test_simc_tests_out_of_memory_actor};
	SimcActor	formatted = {test_simc_tests_formatted_actor};
	SimcReport	report;
	const simc_TupleStringEnding_t *endings;

	simc_Run_actor(&run, "starved", 7, short_of_memory);
	simc_Run_actor(&run, "formatter", 9, formatted);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED && report.outcome.u.ended.len == 2,
			   "the run ends");
	endings = report.outcome.u.ended.ptr;
	STD_ASSERT(__LINE__, 5, endings[0].f1.is_err && memcmp(endings[0].f1.sqlstate, "53200", 5) == 0 &&
			   endings[0].f1.message.len == sizeof(borrowed) - 1 &&
			   memcmp(endings[0].f1.message.ptr, borrowed, sizeof(borrowed) - 1) == 0,
			   "the borrowed message crosses");
	STD_ASSERT(__LINE__, 5, endings[1].f1.is_err && memcmp(endings[1].f1.sqlstate, "XX000", 5) == 0 &&
			   endings[1].f1.message.len == sizeof(owned) - 1 &&
			   memcmp(endings[1].f1.message.ptr, owned, sizeof(owned) - 1) == 0,
			   "the owned message crosses");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_returning_actor is an actor's body that returns without an error. */
static bool
test_simc_tests_returning_actor(void *captures, HostIoIo *io, HostError *error)
{
	return true;
}

/* test_simc_tests_refusing_actor is an actor's body that fails with a fork error of an owned message. */
static bool
test_simc_tests_refusing_actor(void *captures, HostIoIo *io, HostError *error)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("refused")};
	StdFmtArguments arguments = {pieces, 1, NULL};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/*
 * test_simc_tests_an_open_scratch_file_is_counted_until_it_closes: the
 * simulation counts a scratch file while it is open, and not once it closes.
 */
static void
test_simc_tests_an_open_scratch_file_is_counted_until_it_closes(void)
{
	SimcTestIo	test_io = simc_io();
	size_t		before = simc_scratch_files();
	HostScratchFile scratch;
	HostError	error;

	STD_ASSERT(__LINE__, 5, host_open_scratch(&test_io.io, &scratch, &error), "a scratch file opens");
	STD_ASSERT(__LINE__, 5, simc_scratch_files() == before + 1, "the open scratch file is counted");
	host_ScratchFile_drop(&scratch);
	STD_ASSERT(__LINE__, 5, simc_scratch_files() == before, "the closed scratch file isn't");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_next_decompression_runs_out_of_memory_when_asked: once
 * asked, the next decompression fails with Postgres' out of memory error,
 * and the one after it decompresses.
 */
static void
test_simc_tests_the_next_decompression_runs_out_of_memory_when_asked(void)
{
	static const char text[] = "the words a chunk's leaves repeat, again and again, repeat";
	SimcTestIo	test_io = simc_io();
	HostError	error;
	StdVec		frame;
	StdVec		bytes;
	bool		has;

	STD_ASSERT(__LINE__, 5, host_compress((const uint8 *) text, sizeof(text) - 1, 3, NULL, &frame, &error),
			   "the text compresses");
	simc_out_of_memory_at_next_decompress();
	STD_ASSERT(__LINE__, 5, !host_decompress(frame.ptr, frame.len, sizeof(text) - 1, NULL, &has, &bytes, &error),
			   "the next decompression fails");
	STD_ASSERT(__LINE__, 5, memcmp(host_Error_sqlstate(&error), "53200", 5) == 0, "it fails out of memory");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, host_decompress(frame.ptr, frame.len, sizeof(text) - 1, NULL, &has, &bytes, &error) &&
			   has && bytes.len == sizeof(text) - 1, "the one after it decompresses");
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&frame, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_EXITS counts the runs of the exit case's work. */
static int	test_simc_tests_EXITS;

/* test_simc_tests_exit_work is the exit case's work, which counts its runs. */
static bool
test_simc_tests_exit_work(HostIoIo *io, HostError *error)
{
	test_simc_tests_EXITS++;
	return true;
}

/* TEST_SIMC_TESTS_EXIT_TYPE names the exit case's work. */
static const StdTypeId TEST_SIMC_TESTS_EXIT_TYPE = {"simc::tests::Exit"};

/* TEST_SIMC_TESTS_EXIT_OPS is the exit case's Exit table. */
static const HostExitOps TEST_SIMC_TESTS_EXIT_OPS = {test_simc_tests_exit_work};

/* test_simc_tests_exiting_actor is the exit case's body: its exit work listed twice, its number asked twice. */
static bool
test_simc_tests_exiting_actor(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	uint32		first = host.ops->proc_number(host.self);

	if (!host_at_exit(&TEST_SIMC_TESTS_EXIT_TYPE, &TEST_SIMC_TESTS_EXIT_OPS, error) ||
		!host_at_exit(&TEST_SIMC_TESTS_EXIT_TYPE, &TEST_SIMC_TESTS_EXIT_OPS, error))
		return false;
	*(bool *) captures = host.ops->proc_number(host.self) == first;
	return true;
}

/*
 * test_simc_tests_an_exit_work_runs_once_at_its_process_exit: an actor
 * lists one exit work twice, which runs once when the actor exits and not
 * before, and the actor's process number answers the same each time.
 */
static void
test_simc_tests_an_exit_work_runs_once_at_its_process_exit(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	bool		same = false;
	SimcActor	exiting = {test_simc_tests_exiting_actor, &same};
	SimcReport	report;

	test_simc_tests_EXITS = 0;
	simc_Run_actor(&run, "exiter", 6, exiting);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ends");
	STD_ASSERT(__LINE__, 5, test_simc_tests_EXITS == 1, "the exit work ran once");
	STD_ASSERT(__LINE__, 5, same, "the process number answered the same");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_ORDER holds which of the order case's works ran, in the order they ran. */
static int	test_simc_tests_ORDER[2];

/* test_simc_tests_ORDERED counts the order case's works that ran. */
static int	test_simc_tests_ORDERED;

/* test_simc_tests_older_work is the order case's work listed first, which notes 1. */
static bool
test_simc_tests_older_work(HostIoIo *io, HostError *error)
{
	test_simc_tests_ORDER[test_simc_tests_ORDERED++ % 2] = 1;
	return true;
}

/* test_simc_tests_newer_work is the order case's work listed second, which notes 2. */
static bool
test_simc_tests_newer_work(HostIoIo *io, HostError *error)
{
	test_simc_tests_ORDER[test_simc_tests_ORDERED++ % 2] = 2;
	return true;
}

/* TEST_SIMC_TESTS_OLDER_TYPE and TEST_SIMC_TESTS_NEWER_TYPE name the order case's works. */
static const StdTypeId TEST_SIMC_TESTS_OLDER_TYPE = {"simc::tests::Older"};
static const StdTypeId TEST_SIMC_TESTS_NEWER_TYPE = {"simc::tests::Newer"};

/* TEST_SIMC_TESTS_OLDER_OPS and TEST_SIMC_TESTS_NEWER_OPS are the order case's Exit tables. */
static const HostExitOps TEST_SIMC_TESTS_OLDER_OPS = {test_simc_tests_older_work};
static const HostExitOps TEST_SIMC_TESTS_NEWER_OPS = {test_simc_tests_newer_work};

/* test_simc_tests_ordering_actor is the order case's body: its two works listed, the older first. */
static bool
test_simc_tests_ordering_actor(void *captures, HostIoIo *io, HostError *error)
{
	return host_at_exit(&TEST_SIMC_TESTS_OLDER_TYPE, &TEST_SIMC_TESTS_OLDER_OPS, error) &&
		host_at_exit(&TEST_SIMC_TESTS_NEWER_TYPE, &TEST_SIMC_TESTS_NEWER_OPS, error);
}

/*
 * test_simc_tests_exit_works_run_newest_first: an actor lists two exit
 * works, which run at its exit the newest first, each once.
 */
static void
test_simc_tests_exit_works_run_newest_first(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(8);
	SimcActor	ordering = {test_simc_tests_ordering_actor, NULL};
	SimcReport	report;

	test_simc_tests_ORDERED = 0;
	simc_Run_actor(&run, "orderer", 7, ordering);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ends");
	STD_ASSERT(__LINE__, 5, report.exits.len == 0, "no exit work failed");
	STD_ASSERT(__LINE__, 5, test_simc_tests_ORDERED == 2, "each exit work ran once");
	STD_ASSERT(__LINE__, 5, test_simc_tests_ORDER[0] == 2 && test_simc_tests_ORDER[1] == 1,
			   "the newer exit work ran first");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* FIRST_FAILED and SECOND_FAILED are the failing case's works' messages. */
static const char FIRST_FAILED[] = "the first exit work failed";
static const char SECOND_FAILED[] = "the second exit work failed";

/* test_simc_tests_first_failing_work is the failing case's work listed first, which fails. */
static bool
test_simc_tests_first_failing_work(HostIoIo *io, HostError *error)
{
	StdString	message;

	std_string_from_Checked(&message, FIRST_FAILED, sizeof(FIRST_FAILED) - 1);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/* test_simc_tests_second_failing_work is the failing case's work listed second, which fails. */
static bool
test_simc_tests_second_failing_work(HostIoIo *io, HostError *error)
{
	StdString	message;

	std_string_from_Checked(&message, SECOND_FAILED, sizeof(SECOND_FAILED) - 1);
	*error = host_Error_fork((const uint8 *) "XX000", &message);
	return false;
}

/* TEST_SIMC_TESTS_FIRST_FAILING_TYPE and TEST_SIMC_TESTS_SECOND_FAILING_TYPE name the failing case's works. */
static const StdTypeId TEST_SIMC_TESTS_FIRST_FAILING_TYPE = {"simc::tests::FirstFailing"};
static const StdTypeId TEST_SIMC_TESTS_SECOND_FAILING_TYPE = {"simc::tests::SecondFailing"};

/* TEST_SIMC_TESTS_FIRST_FAILING_OPS and TEST_SIMC_TESTS_SECOND_FAILING_OPS are the failing case's Exit tables. */
static const HostExitOps TEST_SIMC_TESTS_FIRST_FAILING_OPS = {test_simc_tests_first_failing_work};
static const HostExitOps TEST_SIMC_TESTS_SECOND_FAILING_OPS = {test_simc_tests_second_failing_work};

/* test_simc_tests_failing_actor is the failing case's body: its two failing works listed. */
static bool
test_simc_tests_failing_actor(void *captures, HostIoIo *io, HostError *error)
{
	return host_at_exit(&TEST_SIMC_TESTS_FIRST_FAILING_TYPE, &TEST_SIMC_TESTS_FIRST_FAILING_OPS, error) &&
		host_at_exit(&TEST_SIMC_TESTS_SECOND_FAILING_TYPE, &TEST_SIMC_TESTS_SECOND_FAILING_OPS, error);
}

/* test_simc_tests_exit_is reports whether a report's failed exit work is the actor's, with a SQLSTATE and message. */
static bool
test_simc_tests_exit_is(const SimcReport *report, size_t at, const char *name, const char *message)
{
	const simc_TupleStringSqlstateString_t *exit = &STD_VEC_AT(&report->exits, simc_TupleStringSqlstateString_t, at);

	return exit->f0.len == strlen(name) && memcmp(exit->f0.ptr, name, exit->f0.len) == 0 &&
		memcmp(exit->f1, "XX000", 5) == 0 && exit->f2.len == strlen(message) &&
		memcmp(exit->f2.ptr, message, exit->f2.len) == 0;
}

/*
 * test_simc_tests_each_failing_exit_work_is_in_the_report: an actor lists
 * two exit works that fail, and the run's report has each one's error, the
 * newer's first, as sim reports its own.
 */
static void
test_simc_tests_each_failing_exit_work_is_in_the_report(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(9);
	SimcActor	failing = {test_simc_tests_failing_actor, NULL};
	SimcReport	report;

	simc_Run_actor(&run, "failer", 6, failing);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ends");
	STD_ASSERT(__LINE__, 5, report.exits.len == 2, "each failed exit work is in the report");
	STD_ASSERT(__LINE__, 5, test_simc_tests_exit_is(&report, 0, "failer", SECOND_FAILED),
			   "the newer work's error comes first");
	STD_ASSERT(__LINE__, 5, test_simc_tests_exit_is(&report, 1, "failer", FIRST_FAILED),
			   "the older work's error comes second");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_REFUSALS counts the late case's listings refused with sim's error. */
static int	test_simc_tests_REFUSALS;

/* test_simc_tests_LATE_RAN counts the runs of the work the late case lists after its exit began. */
static int	test_simc_tests_LATE_RAN;

/* test_simc_tests_late_work is the work the late case lists once its exit began, which counts its runs. */
static bool
test_simc_tests_late_work(HostIoIo *io, HostError *error)
{
	test_simc_tests_LATE_RAN++;
	return true;
}

/* TEST_SIMC_TESTS_LATE_TYPE names the late case's late work. */
static const StdTypeId TEST_SIMC_TESTS_LATE_TYPE = {"simc::tests::Late"};

/* TEST_SIMC_TESTS_LATE_OPS is the late case's late work's Exit table. */
static const HostExitOps TEST_SIMC_TESTS_LATE_OPS = {test_simc_tests_late_work};

/* LATE_REFUSED is sim's error of a listing made once the exit began. */
static const char LATE_REFUSED[] = "an exit work was registered after its process's exit began";

/* test_simc_tests_refused counts a listing's refusal when it has sim's error, which it frees. */
static void
test_simc_tests_refused(bool listed, HostError *refusal)
{
	StdStr		message;

	if (listed)
		return;
	message = host_Error_message(refusal);
	if (memcmp(host_Error_sqlstate(refusal), "XX000", 5) == 0 && message.len == sizeof(LATE_REFUSED) - 1 &&
		memcmp(message.ptr, LATE_REFUSED, message.len) == 0)
		test_simc_tests_REFUSALS++;
	host_Error_drop(refusal);
}

/* TEST_SIMC_TESTS_LISTING_TYPE names the late case's work that lists others. */
static const StdTypeId TEST_SIMC_TESTS_LISTING_TYPE = {"simc::tests::Listing"};

/* test_simc_tests_listing_work is the late case's work: it lists a new work and itself again. */
static bool
test_simc_tests_listing_work(HostIoIo *io, HostError *error)
{
	static const HostExitOps listing = {test_simc_tests_listing_work};
	HostError	refusal;

	test_simc_tests_refused(host_at_exit(&TEST_SIMC_TESTS_LATE_TYPE, &TEST_SIMC_TESTS_LATE_OPS, &refusal),
							&refusal);
	test_simc_tests_refused(host_at_exit(&TEST_SIMC_TESTS_LISTING_TYPE, &listing, &refusal), &refusal);
	return true;
}

/* TEST_SIMC_TESTS_LISTING_OPS is the late case's listing work's Exit table. */
static const HostExitOps TEST_SIMC_TESTS_LISTING_OPS = {test_simc_tests_listing_work};

/* test_simc_tests_late_actor is the late case's body: its listing work listed. */
static bool
test_simc_tests_late_actor(void *captures, HostIoIo *io, HostError *error)
{
	return host_at_exit(&TEST_SIMC_TESTS_LISTING_TYPE, &TEST_SIMC_TESTS_LISTING_OPS, error);
}

/*
 * test_simc_tests_an_exit_work_listed_once_the_exit_began_is_refused: an exit
 * work that lists a new work and itself again has both refused with sim's
 * error, and the new work never runs.
 */
static void
test_simc_tests_an_exit_work_listed_once_the_exit_began_is_refused(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(10);
	SimcActor	late = {test_simc_tests_late_actor, NULL};
	SimcReport	report;

	test_simc_tests_REFUSALS = 0;
	test_simc_tests_LATE_RAN = 0;
	simc_Run_actor(&run, "lister", 6, late);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ends");
	STD_ASSERT(__LINE__, 5, report.exits.len == 0, "no exit work failed");
	STD_ASSERT(__LINE__, 5, test_simc_tests_REFUSALS == 2, "both late listings were refused with sim's error");
	STD_ASSERT(__LINE__, 5, test_simc_tests_LATE_RAN == 0, "the late work never ran");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_two returns the flag case's pool's and the aged kind's count of records. */
static size_t
test_simc_tests_two(const HostSharedLimits *limits)
{
	return 2;
}

/* TEST_SIMC_TESTS_FLAGGED is the flag case's kind: a pool of records of one flag. */
static const HostSharedKind TEST_SIMC_TESTS_FLAGGED = {
	.id = UINT64CONST(0x53494d43464c4147),
	.flags = 1,
	.form = {.tag = HOST_SHARED_FORM_POOL, .u.pool = test_simc_tests_two},
};

/* test_simc_tests_PLACE is the flag case's record, which its actor made. */
static uint64 test_simc_tests_PLACE;

/* test_simc_tests_TOOK counts the flag case's works that took the record's flag. */
static int	test_simc_tests_TOOK;

/* test_simc_tests_taking_work is the flag case's work: it takes the record's flag and leaves it held. */
static bool
test_simc_tests_taking_work(HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));

	if (!host.ops->flag_take(host.self, &raise, test_simc_tests_PLACE, 0, error))
		return false;
	test_simc_tests_TOOK++;
	return true;
}

/* TEST_SIMC_TESTS_FIRST_TAKING_TYPE and TEST_SIMC_TESTS_SECOND_TAKING_TYPE name the flag case's works. */
static const StdTypeId TEST_SIMC_TESTS_FIRST_TAKING_TYPE = {"simc::tests::FirstTaking"};
static const StdTypeId TEST_SIMC_TESTS_SECOND_TAKING_TYPE = {"simc::tests::SecondTaking"};

/* TEST_SIMC_TESTS_TAKING_OPS is the flag case's works' Exit table. */
static const HostExitOps TEST_SIMC_TESTS_TAKING_OPS = {test_simc_tests_taking_work};

/*
 * test_simc_tests_flagging_actor is the flag case's body: a record made, its
 * flag taken and cleared, and two works listed that each take the flag.
 */
static bool
test_simc_tests_flagging_actor(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	bool		made;

	if (!host.ops->shared_make(host.self, &raise, &TEST_SIMC_TESTS_FLAGGED, &made, &test_simc_tests_PLACE, error))
		return false;
	STD_ASSERT(__LINE__, 5, made, "the pool has a free record");
	if (!host.ops->flag_take(host.self, &raise, test_simc_tests_PLACE, 0, error))
		return false;
	host.ops->flag_clear(host.self, test_simc_tests_PLACE, 0);
	return host_at_exit(&TEST_SIMC_TESTS_FIRST_TAKING_TYPE, &TEST_SIMC_TESTS_TAKING_OPS, error) &&
		host_at_exit(&TEST_SIMC_TESTS_SECOND_TAKING_TYPE, &TEST_SIMC_TESTS_TAKING_OPS, error);
}

/*
 * test_simc_tests_a_processs_flags_are_cleared_before_each_exit_work: an
 * actor's two exit works each take a flag the other leaves held, and each
 * takes it, since its process's flags are cleared before each work as sim
 * clears them before its own.
 */
static void
test_simc_tests_a_processs_flags_are_cleared_before_each_exit_work(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(11);
	SimcActor	flagging = {test_simc_tests_flagging_actor, NULL};
	SimcReport	report;

	test_simc_tests_TOOK = 0;
	simc_Run_actor(&run, "flagger", 7, flagging);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ends");
	STD_ASSERT(__LINE__, 5, report.exits.len == 0, "no exit work failed");
	STD_ASSERT(__LINE__, 5, test_simc_tests_TOOK == 2, "each exit work took the flag");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts:
 * a run of a returning actor and a refusing one ends with each actor's
 * ending in the order they were added, each equal to itself and the two
 * unequal, as Ending's derived PartialEq compares them.
 */
static void
test_simc_tests_a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	SimcActor	returning = {test_simc_tests_returning_actor};
	SimcActor	refusing = {test_simc_tests_refusing_actor};
	SimcReport	report;
	const simc_TupleStringEnding_t *endings;

	simc_Run_actor(&run, "returner", 8, returning);
	simc_Run_actor(&run, "refuser", 7, refusing);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED && report.outcome.u.ended.len == 2,
			   "the run ends");
	endings = report.outcome.u.ended.ptr;
	STD_ASSERT(__LINE__, 5, endings[0].f0.len == 8 && memcmp(endings[0].f0.ptr, "returner", 8) == 0 &&
			   endings[1].f0.len == 7 && memcmp(endings[1].f0.ptr, "refuser", 7) == 0,
			   "the endings keep the actors' order");
	STD_ASSERT(__LINE__, 5, endings[0].f1.tag == SIMC_ENDING_RETURNED && !endings[0].f1.is_err,
			   "the returning actor ends ok");
	STD_ASSERT(__LINE__, 5, simc_Ending_PartialEq_eq(&endings[0].f1, &endings[0].f1) &&
			   simc_Ending_PartialEq_eq(&endings[1].f1, &endings[1].f1), "each ending equals itself");
	STD_ASSERT(__LINE__, 5, !simc_Ending_PartialEq_eq(&endings[0].f1, &endings[1].f1),
			   "the two endings differ");
	{
		StdString	shown;
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&endings[0].f1, simc_Ending_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&shown, &arguments);
		STD_ASSERT(__LINE__, 5, shown.len == strlen("Returned(Ok(()))") &&
				   memcmp(shown.ptr, "Returned(Ok(()))", shown.len) == 0, "the ending's Debug is the Rust's");
		std_vec_free_Checked(&shown, 1, 1);
		args[0].value = &endings[1].f1;
		std_fmt_format_Checked(&shown, &arguments);
		STD_ASSERT(__LINE__, 5, shown.len > strlen("Returned(Err(") &&
				   memcmp(shown.ptr, "Returned(Err(", strlen("Returned(Err(")) == 0,
				   "a refusing actor's ending's Debug is the Rust's Err");
		std_vec_free_Checked(&shown, 1, 1);
	}
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_simulation_counts_traces_and_fills_its_calls: a
 * simulation over a directory counts each call that changes a file, traces
 * each, and fails the call a full disk is planned at alone.
 */
static void
test_simc_tests_a_simulation_counts_traces_and_fills_its_calls(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(&test_io.io);
	SimcTempDir dir;
	StdIoError	io_error;
	HostError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	a;
	HostFile	file;
	SimcSim		sim;
	StdVec		trace;
	uint64		before;
	bool		has;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &io_error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a", 1, &a);
	sim = simc_Sim_start(dir_path, dir_path_len, 9);
	STD_ASSERT(__LINE__, 5, host_create_if_absent(io, a.bytes.ptr, a.bytes.len, &has, &file, &error) && has,
			   "a file is created");
	before = simc_Sim_calls(&sim);
	simc_Sim_full_at(&sim, before + 1);
	STD_ASSERT(__LINE__, 5, !host_write_at(io, &file, 0, (const uint8 *) "full", 4, &error) &&
			   memcmp(host_Error_sqlstate(&error), "53100", 5) == 0, "the planned call finds the disk full");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, host_write_at(io, &file, 0, (const uint8 *) "room", 4, &error),
			   "the next call finds room");
	STD_ASSERT(__LINE__, 5, simc_Sim_calls(&sim) == before + 2, "each write is a call");
	trace = simc_Sim_trace(&sim);
	STD_ASSERT(__LINE__, 5, trace.len >= simc_Sim_calls(&sim), "each counted call is traced");
	std_vec_drop_Checked(&trace, sizeof(StdString), _Alignof(StdString), std_String_drop);
	host_File_drop(&file);
	simc_Sim_stop(&sim);
	simc_Sim_drop(&sim);
	std_vec_free_Checked(&a.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_test_only_std_fs_writes_reads_appends_and_removes_a_file:
 * the test-only std::fs writes a file, reads it back, appends to it, reads
 * its length, finds it, and removes it, as Rust's own does.
 */
static void
test_simc_tests_the_test_only_std_fs_writes_reads_appends_and_removes_a_file(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	a;
	StdVec		bytes;
	StdFsOpenOptions options;
	StdFsFile	file;
	StdFsMetadata metadata;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a", 1, &a);
	STD_ASSERT(__LINE__, 5, std_fs_write(a.bytes.ptr, a.bytes.len, (const uint8 *) "ab", 2, &error),
			   "the file is written");
	STD_ASSERT(__LINE__, 5, std_fs_read(a.bytes.ptr, a.bytes.len, &bytes, &error) && bytes.len == 2 &&
			   memcmp(bytes.ptr, "ab", 2) == 0, "the file reads back");
	std_vec_free_Checked(&bytes, 1, 1);
	options = std_fs_OpenOptions_new();
	STD_ASSERT(__LINE__, 5, std_fs_OpenOptions_open(std_fs_OpenOptions_append(&options, true), a.bytes.ptr,
													a.bytes.len, &file, &error), "the file opens to append");
	STD_ASSERT(__LINE__, 5, std_fs_File_Write_write_all(&file, (const uint8 *) "cd", 2, &error),
			   "the append is written");
	std_fs_File_drop(&file);
	std_fs_OpenOptions_drop(&options);
	STD_ASSERT(__LINE__, 5, std_fs_metadata(a.bytes.ptr, a.bytes.len, &metadata, &error) &&
			   std_fs_Metadata_len(&metadata) == 4, "the file holds both writes");
	std_fs_Metadata_drop(&metadata);
	STD_ASSERT(__LINE__, 5, std_path_Path_exists(a.bytes.ptr, a.bytes.len), "the file exists");
	STD_ASSERT(__LINE__, 5, std_fs_remove_file(a.bytes.ptr, a.bytes.len, &error), "the file is removed");
	STD_ASSERT(__LINE__, 5, !std_path_Path_exists(a.bytes.ptr, a.bytes.len), "the removed file is gone");
	std_vec_free_Checked(&a.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * SimcTestsListing is a directory's listing a catch's body makes: the
 * capability's read doors, the directory, and the call's places.
 */
typedef struct SimcTestsListing
{
	HostIoReadDoors read;
	const char *path;
	size_t		path_len;
	StdVec		names;
	HostError	error;
} SimcTestsListing;

/* test_simc_tests_listing_call lists a directory through the C port's host. */
static void
test_simc_tests_listing_call(void *captures)
{
	SimcTestsListing *listing = captures;

	(void) host_list_dir(listing->read, listing->path, listing->path_len, &listing->names, &listing->error);
}

/*
 * test_simc_tests_a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory:
 * a listing of two names whose second name's allocation fails frees the
 * first and raises the C's out of memory.
 */
static void
test_simc_tests_a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(&test_io.io);
	SimcTempDir dir;
	StdIoError	io_error;
	HostError	error;
	StdPathBuf	a;
	StdPathBuf	b;
	SimcTestsListing listing;
	StdCaught	caught;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &io_error), "the test makes its directory");
	listing.read = HOST_IO_IO_READ_DOORS(&test_io.io);
	listing.path = simc_TempDir_path(&dir, &listing.path_len);
	std_path_join(listing.path, listing.path_len, "a", 1, &a);
	std_path_join(listing.path, listing.path_len, "b", 1, &b);
	STD_ASSERT(__LINE__, 5, host_make_dir(io, a.bytes.ptr, a.bytes.len, &error) &&
			   host_make_dir(io, b.bytes.ptr, b.bytes.len, &error), "the directory holds two names");
	simc_fail_allocation(2);
	STD_ASSERT(__LINE__, 5, !std_catch_unwind(test_simc_tests_listing_call, &listing, &caught),
			   "the listing unwinds");
	STD_ASSERT(__LINE__, 5, caught.payload == STD_PAYLOAD_OUT_OF_MEMORY, "the listing runs out of memory");
	std_Caught_drop(&caught);
	std_vec_free_Checked(&b.bytes, 1, 1);
	std_vec_free_Checked(&a.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory:
 * an entry's Rust panic whose payload's box can't be allocated frees the
 * message it copied and raises the C's out of memory.
 */
static void
test_simc_tests_a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory(void)
{
	SimcTestIo	test_io = simc_io();
	bool		answered = false;
	StdCaught	caught;

	simc_fail_allocation(2);
	STD_ASSERT(__LINE__, 5, !std_catch_unwind(test_simc_tests_unported_call, &answered, &caught),
			   "a call of an unserved entry panics");
	STD_ASSERT(__LINE__, 5, caught.payload == STD_PAYLOAD_OUT_OF_MEMORY, "the panic runs out of memory");
	std_Caught_drop(&caught);
	simc_TestIo_drop(&test_io);
}

/* TEST_SIMC_TESTS_TEMP_NAMES are the variables the operating systems read their temporary directory from. */
static const char *const TEST_SIMC_TESTS_TEMP_NAMES[] = {"TMPDIR", "TMP", "TEMP"};

/*
 * test_simc_tests_a_test_directory_the_operating_system_cant_make_fails_with_its_error:
 * with the temporary directory named as a regular file, a test's directory
 * can't be made, and the call fails with the operating system's error.
 */
static void
test_simc_tests_a_test_directory_the_operating_system_cant_make_fails_with_its_error(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	SimcTempDir refused;
	StdIoError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	file;
	char	   *file_name;
	char	   *saved[lengthof(TEST_SIMC_TESTS_TEMP_NAMES)];
	bool		made;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "file", 4, &file);
	STD_ASSERT(__LINE__, 5, std_fs_write(file.bytes.ptr, file.bytes.len, (const uint8 *) "x", 1, &error),
			   "the file is written");
	file_name = pnstrdup(file.bytes.ptr, file.bytes.len);
	for (size_t i = 0; i < lengthof(TEST_SIMC_TESTS_TEMP_NAMES); i++)
	{
		const char *value = getenv(TEST_SIMC_TESTS_TEMP_NAMES[i]);

		saved[i] = value != NULL ? pstrdup(value) : NULL;
		setenv(TEST_SIMC_TESTS_TEMP_NAMES[i], file_name, 1);
	}
	made = simc_tempdir(&refused, &error);
	for (size_t i = 0; i < lengthof(TEST_SIMC_TESTS_TEMP_NAMES); i++)
	{
		if (saved[i] != NULL)
		{
			setenv(TEST_SIMC_TESTS_TEMP_NAMES[i], saved[i], 1);
			pfree(saved[i]);
		}
		else
			unsetenv(TEST_SIMC_TESTS_TEMP_NAMES[i]);
	}
	STD_ASSERT(__LINE__, 5, !made, "a directory under a file isn't made");
	STD_ASSERT(__LINE__, 5, std_io_Error_data(&error).tag == STD_IO_ERROR_DATA_OS,
			   "the error is the operating system's");
	pfree(file_name);
	std_vec_free_Checked(&file.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_fill writes a node's five bytes. */
static void
test_simc_tests_fill(void *captures, uint8 *bytes, size_t len)
{
	memcpy(bytes, "bytes", len);
}

/*
 * test_simc_tests_the_node_cache_installs_finds_and_unpins_a_nodes_frame:
 * the server's node cache holds no frame of a key until an install fills
 * one, which a later lookup finds and pins with the fill's bytes, and a
 * backend's own cache, once made, holds none of the server's.
 */
static void
test_simc_tests_the_node_cache_installs_finds_and_unpins_a_nodes_frame(void)
{
	SimcTestIo	test_io = simc_io();
	uint8		address[HOST_CACHE_ADDRESS] = {7};
	uint8		locator[HOST_CACHE_LOCATOR] = {9};
	HostCacheKey key = host_cache_key(3, address, locator);
	HostError	error;
	HostCacheRead read;
	HostCachePinned pinned;
	StdBytes	bytes;
	bool		has;

	STD_ASSERT(__LINE__, 5, host_cache_contains(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &error) &&
			   !has, "an uninstalled key isn't cached");
	STD_ASSERT(__LINE__, 5, host_cache_get(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &pinned, &error) &&
			   !has, "an uninstalled key finds nothing");
	STD_ASSERT(__LINE__, 5, host_cache_install(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, 5, test_simc_tests_fill,
											   NULL, NULL, &read, &error) && read.tag == HOST_CACHE_READ_CACHED,
			   "the install fills a frame");
	bytes = host_cache_Read_Deref_deref(&read);
	STD_ASSERT(__LINE__, 5, bytes.len == 5 && memcmp(bytes.ptr, "bytes", 5) == 0, "the frame holds the fill's bytes");
	host_cache_Read_drop(&read);
	STD_ASSERT(__LINE__, 5, host_cache_contains(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &error) &&
			   has, "the installed key is cached");
	STD_ASSERT(__LINE__, 5, host_cache_get(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &pinned, &error) &&
			   has, "the installed key is found");
	bytes = host_cache_Pinned_Deref_deref(&pinned);
	STD_ASSERT(__LINE__, 5, bytes.len == 5 && memcmp(bytes.ptr, "bytes", 5) == 0, "the found frame holds its bytes");
	host_cache_Pinned_drop(&pinned);
	STD_ASSERT(__LINE__, 5, host_cache_make_local(HOST_IO_IO_IO_DOORS(&test_io.io), 1 << 20, &error),
			   "the backend makes its own cache");
	STD_ASSERT(__LINE__, 5, host_cache_contains(&test_io.io, HOST_CACHE_PLACEMENT_BACKEND, &key, &has, &error) &&
			   !has, "the backend's own cache holds none of the server's");
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_panicking_fill is a node's fill that panics with a literal message. */
static void
test_simc_tests_panicking_fill(void *captures, uint8 *bytes, size_t len)
{
	static const char text[] = "the fill failed";

	std_panic(RUST_FILE, __LINE__, 5, text, sizeof(text) - 1);
}

/* test_simc_tests_panicking_install installs a key whose fill panics. */
static void
test_simc_tests_panicking_install(void *captures)
{
	HostIoIo   *io = captures;
	uint8		address[HOST_CACHE_ADDRESS] = {8};
	uint8		locator[HOST_CACHE_LOCATOR] = {9};
	HostCacheKey key = host_cache_key(3, address, locator);
	HostCacheRead read;
	HostError	error;

	(void) host_cache_install(io, HOST_CACHE_PLACEMENT_SERVER, &key, 5, test_simc_tests_panicking_fill, NULL, NULL,
							  &read, &error);
}

/*
 * test_simc_tests_a_c_fills_panic_unwinds_the_simulations_install_with_its_payload:
 * a C fill's panic unwinds the simulation's install as a Rust unwind and
 * reaches the C's catch with the fill's own payload.
 */
static void
test_simc_tests_a_c_fills_panic_unwinds_the_simulations_install_with_its_payload(void)
{
	static const char expected[] = "the fill failed";
	SimcTestIo	test_io = simc_io();
	StdCaught	caught;
	const char *text;
	size_t		len;

	STD_ASSERT(__LINE__, 5, !std_catch_unwind(test_simc_tests_panicking_install, &test_io.io, &caught),
			   "the install unwinds");
	STD_ASSERT(__LINE__, 5, std_Caught_downcast_str(&caught, &text, &len) && len == sizeof(expected) - 1 &&
			   memcmp(text, expected, len) == 0, "the unwind carries the fill's payload");
	std_Caught_drop(&caught);
	simc_TestIo_drop(&test_io);
}

/*
 * SimcTestsOwnFill is an actor's install whose fill checks the backend's
 * state it runs under: the byte of its node's address, the error context
 * its actor pushed, and whether the fill saw it.
 */
typedef struct SimcTestsOwnFill
{
	uint8		address_byte;
	ErrorContextCallback *pushed;
	bool		seen;
} SimcTestsOwnFill;

/* test_simc_tests_no_context is an error context that adds nothing. */
static void
test_simc_tests_no_context(void *arg)
{
}

/* test_simc_tests_own_fill writes a node's bytes and records whether it runs under its actor's error context. */
static void
test_simc_tests_own_fill(void *captures, uint8 *bytes, size_t len)
{
	SimcTestsOwnFill *own = captures;

	own->seen = error_context_stack == own->pushed;
	memset(bytes, own->address_byte, len);
}

/* test_simc_tests_own_fill_actor installs a node of its own under an error context it pushes. */
static bool
test_simc_tests_own_fill_actor(void *captures, HostIoIo *io, HostError *error)
{
	SimcTestsOwnFill *own = captures;
	uint8		address[HOST_CACHE_ADDRESS] = {own->address_byte};
	uint8		locator[HOST_CACHE_LOCATOR] = {9};
	HostCacheKey key = host_cache_key(5, address, locator);
	ErrorContextCallback callback = {.previous = error_context_stack, .callback = test_simc_tests_no_context};
	HostCacheRead read;
	bool		installed;

	own->pushed = &callback;
	error_context_stack = &callback;
	installed = host_cache_install(io, HOST_CACHE_PLACEMENT_SERVER, &key, 5, test_simc_tests_own_fill, own, NULL,
								   &read, error);
	error_context_stack = callback.previous;
	if (installed)
		host_cache_Read_drop(&read);
	return installed;
}

/*
 * test_simc_tests_a_c_fill_after_a_yield_runs_under_its_own_actors_backend_state:
 * two actors' installs yield to each other before their fills run, and each
 * fill runs under the error context its own actor pushed.
 */
static void
test_simc_tests_a_c_fill_after_a_yield_runs_under_its_own_actors_backend_state(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(7);
	SimcTestsOwnFill first = {.address_byte = 1};
	SimcTestsOwnFill second = {.address_byte = 2};
	SimcActor	first_actor = {test_simc_tests_own_fill_actor, &first};
	SimcActor	second_actor = {test_simc_tests_own_fill_actor, &second};
	SimcReport	report;
	const simc_TupleStringEnding_t *endings;

	simc_Run_actor(&run, "first", 5, first_actor);
	simc_Run_actor(&run, "second", 6, second_actor);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED && report.outcome.u.ended.len == 2,
			   "the run ends");
	endings = report.outcome.u.ended.ptr;
	STD_ASSERT(__LINE__, 5, !endings[0].f1.is_err && !endings[1].f1.is_err, "both installs succeed");
	STD_ASSERT(__LINE__, 5, first.seen && second.seen, "each fill runs under its own actor's error context");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_eight returns the records case's pool's count of records. */
static size_t
test_simc_tests_eight(const HostSharedLimits *limits)
{
	return 8;
}

/* TEST_SIMC_TESTS_BLOBS is the records case's blobs: one of 16 bytes. */
static const size_t TEST_SIMC_TESTS_BLOBS[] = {16};

/* TEST_SIMC_TESTS_ARRAYS is the records case's arrays: one of two entries of 8 bytes. */
static const std_TupleUsizeUsize_t TEST_SIMC_TESTS_ARRAYS[] = {{2, 8}};

/* TEST_SIMC_TESTS_POOLED is the records case's kind: a pool of records of a lock, two atomics, a blob, and an array. */
static const HostSharedKind TEST_SIMC_TESTS_POOLED = {
	.id = UINT64CONST(0x53494d43504f4f4c),
	.locks = 1,
	.atomics = 2,
	.blobs = TEST_SIMC_TESTS_BLOBS,
	.blobs_len = lengthof(TEST_SIMC_TESTS_BLOBS),
	.arrays = TEST_SIMC_TESTS_ARRAYS,
	.arrays_len = lengthof(TEST_SIMC_TESTS_ARRAYS),
	.form = {.tag = HOST_SHARED_FORM_POOL, .u.pool = test_simc_tests_eight},
};

/* test_simc_tests_no_words returns the single case's run of words: none. */
static size_t
test_simc_tests_no_words(const HostSharedLimits *limits)
{
	return 0;
}

/* TEST_SIMC_TESTS_SINGLE is the records case's single kind: one record of an atomic. */
static const HostSharedKind TEST_SIMC_TESTS_SINGLE = {
	.id = UINT64CONST(0x53494d4353494e47),
	.atomics = 1,
	.form = {.tag = HOST_SHARED_FORM_SINGLE, .u.single = {test_simc_tests_no_words}},
};

/* test_simc_tests_op returns an atomic's operation: a load, an add, or an exchange. */
static HostSharedOp
test_simc_tests_op(HostSharedOpTag tag, uint64 a, uint64 b)
{
	HostSharedOp op = {.tag = tag};

	if (tag == HOST_SHARED_OP_ADD)
		op.u.add = a;
	else if (tag == HOST_SHARED_OP_EXCHANGE)
	{
		op.u.exchange.f0 = a;
		op.u.exchange.f1 = b;
	}
	return op;
}

/*
 * test_simc_tests_a_pooled_records_doors_answer_as_the_simulations_host: a
 * pooled record made, changed, locked, and published answers each door as
 * the simulation's host does: its atomics, blob, and entries read back what
 * was written, a sweep lists it, a claim takes it and a give-up publishes it
 * again, a reclaim takes it and frees it, after which it is gone, and the
 * single record of a kind is found.
 */
static void
test_simc_tests_a_pooled_records_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	const HostSharedKind *kind = &TEST_SIMC_TESTS_POOLED;
	HostError	error;
	bool		has;
	uint64		place;
	uint64		opened;
	uint64		location;
	uint64		single;
	bool		gone;
	StdVec		bytes;
	StdVec		swept;
	HostSharedLooked looked;
	static const uint8 blob[16] = "a blob's bytes!";
	static const uint8 entry[8] = "an entry";

	STD_ASSERT(__LINE__, 5, host.ops->shared_ready(host.self), "the simulation made shared memory");
	STD_ASSERT(__LINE__, 5, host.ops->shared_make(host.self, &raise, kind, &has, &place, &error) && has,
			   "the pool makes a record");
	STD_ASSERT(__LINE__, 5, host.ops->shared_atomics(host.self, place) >= 2, "the record has its atomics");
	STD_ASSERT(__LINE__, 5, host.ops->atomic(host.self, place, 1, test_simc_tests_op(HOST_SHARED_OP_ADD, 5, 0)) == 0,
			   "an add returns the value before");
	STD_ASSERT(__LINE__, 5,
			   host.ops->atomic(host.self, place, 1, test_simc_tests_op(HOST_SHARED_OP_EXCHANGE, 5, 9)) == 5,
			   "an exchange returns the value before");
	STD_ASSERT(__LINE__, 5, host.ops->atomic(host.self, place, 1, test_simc_tests_op(HOST_SHARED_OP_LOAD, 0, 0)) == 9,
			   "a load reads the exchange's value");
	STD_ASSERT(__LINE__, 5, host.ops->shared_lock(host.self, &raise, place, 0, true, &error),
			   "the record's lock is taken");
	STD_ASSERT(__LINE__, 5, host.ops->blob_write(host.self, place, 0, blob, sizeof(blob), &error),
			   "the blob is written");
	bytes = host.ops->blob_read(host.self, place, 0);
	STD_ASSERT(__LINE__, 5, bytes.len == sizeof(blob) && memcmp(bytes.ptr, blob, sizeof(blob)) == 0,
			   "the blob reads back");
	std_vec_free_Checked(&bytes, 1, 1);
	STD_ASSERT(__LINE__, 5, host.ops->entry_write(host.self, place, 0, 1, entry, sizeof(entry), &error),
			   "the entry is written");
	STD_ASSERT(__LINE__, 5, host.ops->entry_read(host.self, place, 0, 1, &bytes, &error) &&
			   bytes.len == sizeof(entry) && memcmp(bytes.ptr, entry, sizeof(entry)) == 0, "the entry reads back");
	std_vec_free_Checked(&bytes, 1, 1);
	STD_ASSERT(__LINE__, 5, !host.ops->entry_read(host.self, place, 0, 2, &bytes, &error),
			   "an entry past the array's count is refused");
	host_Error_drop(&error);
	host.ops->shared_unlock(host.self, place, 0);
	location = host.ops->shared_publish(host.self, place);
	STD_ASSERT(__LINE__, 5, host.ops->shared_location(host.self, place) == location &&
			   host.ops->shared_live(host.self, location), "the published record has its location");
	STD_ASSERT(__LINE__, 5, host.ops->shared_sweep(host.self, &raise, kind, 1, &swept, &error), "the pool is swept");
	STD_ASSERT(__LINE__, 5, swept.len == 1 && STD_VEC_AT(&swept, std_TupleU64U64_t, 0).f0 == location &&
			   STD_VEC_AT(&swept, std_TupleU64U64_t, 0).f1 == 9, "the sweep lists the record with its atomic");
	std_vec_free_Checked(&swept, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	STD_ASSERT(__LINE__, 5, host.ops->shared_open(host.self, &raise, kind, location, &has, &opened, &error) && has,
			   "the published record opens");
	host.ops->shared_close(host.self, opened);
	STD_ASSERT(__LINE__, 5, host.ops->shared_claim(host.self, kind, location, &has, &place, &error) && has,
			   "the record is claimed");
	host.ops->shared_give_up(host.self, place);
	STD_ASSERT(__LINE__, 5, host.ops->shared_reclaim(host.self, kind, location, &looked, &error) &&
			   looked.tag == HOST_SHARED_LOOKED_MINE, "a reclaim takes the record no process holds");
	host.ops->shared_unreclaim(host.self, looked.u.mine);
	STD_ASSERT(__LINE__, 5, host.ops->shared_reclaim(host.self, kind, location, &looked, &error) &&
			   looked.tag == HOST_SHARED_LOOKED_MINE, "a reclaim given up takes it again");
	STD_ASSERT(__LINE__, 5, host.ops->shared_reclaimed(host.self, looked.u.mine), "the reclaim frees the record");
	STD_ASSERT(__LINE__, 5, host.ops->shared_gone(host.self, kind, location, &gone, &error) && gone,
			   "the freed record is gone");
	STD_ASSERT(__LINE__, 5, host.ops->shared_make(host.self, &raise, kind, &has, &place, &error) && has,
			   "the pool makes another record");
	host.ops->shared_free(host.self, place);
	STD_ASSERT(__LINE__, 5, host.ops->shared_single(host.self, &TEST_SIMC_TESTS_SINGLE, &single, &error) &&
			   host.ops->atomic(host.self, single, 0, test_simc_tests_op(HOST_SHARED_OP_LOAD, 0, 0)) == 0,
			   "the single record is found");
	simc_TestIo_drop(&test_io);
}

/* TEST_SIMC_TESTS_WORDS_OPS is the words case's ProcSlots: two words for each process. */
static const HostSharedProcSlotsOps TEST_SIMC_TESTS_WORDS_OPS = {.id = UINT64CONST(0x53494d43574f5244), .words = 2};

/*
 * test_simc_tests_a_processs_words_answer_as_the_simulations_host: the
 * running process's slot of a kind of words takes the words it stores, which
 * another process's load reads by its number, as its own store at the kind's
 * place in the server's table does.
 */
static void
test_simc_tests_a_processs_words_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostSharedKind kind = host_shared_Kind_slots(&TEST_SIMC_TESTS_WORDS_OPS);
	uint32		proc = host.ops->proc_number(host.self);
	HostError	error;
	uint64		mine;
	uint64		placed;
	uint64		loaded;
	StdVec		processes;
	bool		listed = false;

	STD_ASSERT(__LINE__, 5, host.ops->words_mine(host.self, &kind, &mine, &error), "the process has its slot");
	host.ops->word_store(host.self, mine, 0, 41);
	STD_ASSERT(__LINE__, 5, host.ops->word_load(host.self, &kind, proc, 0, &loaded, &error) && loaded == 41,
			   "a load reads the word stored");
	host.ops->word_store_own(host.self, 0, &kind, 1, 43);
	STD_ASSERT(__LINE__, 5, host.ops->word_load(host.self, &kind, proc, 1, &loaded, &error) && loaded == 43,
			   "a load reads the word the process stored as its own");
	STD_ASSERT(__LINE__, 5, host.ops->words_place(host.self, &kind, proc, &placed, &error) && placed == mine,
			   "the process's place is its slot");
	STD_ASSERT(__LINE__, 5, host.ops->words_processes(host.self, &kind, &processes, &error),
			   "the processes are listed");
	for (size_t k = 0; k < processes.len; k++)
		listed = listed || STD_VEC_AT(&processes, uint32, k) == proc;
	STD_ASSERT(__LINE__, 5, listed, "the running process is among them");
	std_vec_free_Checked(&processes, sizeof(uint32), _Alignof(uint32));
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_epoch_doors_answer_as_the_simulations_host: an epoch
 * advanced under the epoch lock moves the current one past it, a reader then
 * starts at the current one, and its end lets it go.
 */
static void
test_simc_tests_the_epoch_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	uint64		advanced;
	uint64		read;

	STD_ASSERT(__LINE__, 5, host.ops->epochs_lock(host.self, &raise, &error), "the epoch lock is taken");
	advanced = host.ops->epochs_advance(host.self);
	STD_ASSERT(__LINE__, 5, host.ops->epochs_oldest(host.self) > advanced,
			   "with no reader, the oldest is the current epoch, past the one advanced");
	host.ops->epochs_unlock(host.self);
	STD_ASSERT(__LINE__, 5, host.ops->epochs_read(host.self, &raise, &read, &error) && read > advanced,
			   "a reader starts at the current epoch");
	host.ops->epochs_end(host.self, read);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host:
 * a collection runs while no backup is counted in and its end lets a backup
 * begin, a backup counted in refuses a collection until it ends, and a
 * database's lock is taken with no session waiting for it and given back.
 */
static void
test_simc_tests_the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	bool		answer;

	STD_ASSERT(__LINE__, 5, host.ops->collection_begin(host.self), "a collection may run with no backup counted in");
	host.ops->collection_end(host.self);
	STD_ASSERT(__LINE__, 5, host.ops->backup_begin(host.self, &raise, &error), "a backup begins once the collection ended");
	STD_ASSERT(__LINE__, 5, host.ops->backups_running(host.self), "the backup is counted in");
	STD_ASSERT(__LINE__, 5, !host.ops->collection_begin(host.self), "a backup counted in refuses a collection");
	host.ops->backup_end(host.self);
	STD_ASSERT(__LINE__, 5, !host.ops->backups_running(host.self), "the backup is counted out");
	STD_ASSERT(__LINE__, 5, host.ops->database_locked(host.self, &raise, 9300, &answer, &error) && answer,
			   "the database's lock is taken");
	STD_ASSERT(__LINE__, 5, host.ops->database_lock_waited(host.self, &raise, 9300, &answer, &error) && !answer,
			   "no session waits for the lock");
	host.ops->database_unlocked(host.self, 9300);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host:
 * the delta's advance answers an epoch the oldest is past with no reader, its seed
 * writes the run's seed and zeros after its first eight bytes, and a pass
 * returns.
 */
static void
test_simc_tests_the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	uint64		passed;
	uint64		oldest;
	uint8		seed[32];
	uint8		again[32];
	static const uint8 zeros[24];

	memset(seed, 0xff, sizeof(seed));
	memset(again, 0x00, sizeof(again));
	host.ops->delta_advanced(host.self, &passed, &oldest);
	STD_ASSERT(__LINE__, 5, oldest > passed, "with no reader, the oldest epoch is past the one advanced");
	host.ops->delta_seed(host.self, seed);
	host.ops->delta_seed(host.self, again);
	STD_ASSERT(__LINE__, 5, memcmp(seed, again, sizeof(seed)) == 0, "the seed is the same each time");
	STD_ASSERT(__LINE__, 5, memcmp(seed + 8, zeros, sizeof(zeros)) == 0, "the seed is zeros after its first eight bytes");
	host.ops->delta_pass(host.self);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_delta_hold_and_budget_doors_answer_as_the_simulations_host:
 * a hold is the oldest reader past an advance, its release lets the oldest
 * pass it, and the delta's bytes in use are within its budget.
 */
static void
test_simc_tests_the_delta_hold_and_budget_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	uint64		held;

	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken");
	STD_ASSERT(__LINE__, 5, host.ops->epochs_lock(host.self, &raise, &error), "the epoch lock is taken");
	(void) host.ops->epochs_advance(host.self);
	host.ops->epochs_unlock(host.self);
	STD_ASSERT(__LINE__, 5, host.ops->epochs_oldest(host.self) == held, "the hold is the oldest reader");
	host.ops->delta_release(host.self, held);
	STD_ASSERT(__LINE__, 5, host.ops->epochs_oldest(host.self) > held, "the oldest passes a released hold");
	STD_ASSERT(__LINE__, 5, host.ops->delta_budget(host.self) > 0, "the delta has a budget");
	STD_ASSERT(__LINE__, 5, host.ops->delta_used(host.self) <= host.ops->delta_budget(host.self),
			   "the delta's bytes in use are within its budget");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_delta_page_is_taken_written_and_given_back_as_the_simulations_host:
 * an allocation of a page takes a block and lists its name in its caller's
 * room, records append to it at a moving mark, a word stores into it and a
 * store past its words fails, and its give back returns the block.
 */
static void
test_simc_tests_a_delta_page_is_taken_written_and_given_back_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostDeltaPagePage name;
	StdVec		names;
	HostError	error;
	bool		refused;
	uint64		frees;
	uint32		offset;
	uint64		before;
	static const uint8 record[] = {1, 2, 3};

	before = host.ops->delta_used(host.self);
	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error),
			   "the allocation answers");
	STD_ASSERT(__LINE__, 5, !refused && names.len == 1, "the page is taken and its name listed");
	STD_ASSERT(__LINE__, 5, host.ops->delta_used(host.self) > before, "the page takes a block");
	name = STD_VEC_AT(&names, HostDeltaPagePage, 0);
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_append(host.self, name, record, sizeof(record), &offset, &error) &&
			   offset == 0, "the first record appends at the page's start");
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_append(host.self, name, record, sizeof(record), &offset, &error) &&
			   offset == sizeof(record), "the next record appends past the first");
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_store(host.self, name, 0, 7, &error), "a word stores into the page");
	STD_ASSERT(__LINE__, 5, !host.ops->delta_owned_store(host.self, name, 4, 7, &error),
			   "a store past the page's words fails");
	host_Error_drop(&error);
	host.ops->delta_give_back(host.self, name);
	STD_ASSERT(__LINE__, 5, host.ops->delta_used(host.self) == before, "the give back returns the block");
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/* TEST_SIMC_TESTS_AGED is the delta's cases' kind: two aged records of a lock that owns their pages. */
static const HostSharedKind TEST_SIMC_TESTS_AGED = {
	.id = UINT64CONST(0x53494d4341474544),
	.locks = 1,
	.atomics = 1,
	.form = {.tag = HOST_SHARED_FORM_AGED, .u.aged = {test_simc_tests_two}},
};

/* test_simc_tests_word is a word operation on a page: a load, a store, an exchange, or an add. */
static uint64
test_simc_tests_word(HostHost host, uint64 body, HostDeltaWordOpTag tag, uint64 a, uint64 b)
{
	HostDeltaWordOp op = {.tag = tag};

	if (tag == HOST_DELTA_WORD_OP_STORE)
		op.u.store = a;
	else if (tag == HOST_DELTA_WORD_OP_ADD)
		op.u.add = a;
	else if (tag == HOST_DELTA_WORD_OP_EXCHANGE)
	{
		op.u.exchange.f0 = a;
		op.u.exchange.f1 = b;
	}
	return host.ops->delta_word(host.self, body, 0, op);
}

/*
 * test_simc_tests_a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host:
 * a page linked to an aged record opens under a hold with the bytes and the
 * word written while owned, its words load, store, exchange, and add, an
 * append under its owner moves the mark and its seal refuses the next, the
 * record is found under the hold, a wait under a hold fails, and once both
 * are retired and a pass frees them neither opens and the block is back.
 */
static void
test_simc_tests_a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostDeltaDirectoryOwner owner;
	HostDeltaDirectoryOpened opened;
	HostDeltaLeaving leaving;
	HostCacheBytes bytes;
	StdBytes	read;
	HostDeltaPagePage name;
	StdVec		names;
	HostError	error;
	bool		made;
	bool		refused;
	uint64		before;
	uint64		place;
	uint64		location;
	uint64		frees;
	uint64		held;
	uint64		found;
	uint32		offset;
	static const uint8 record[] = {1, 2, 3};

	before = host.ops->delta_used(host.self);
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												 &location, &frees, &error) && made, "an aged record is made");
	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "a page is taken");
	name = STD_VEC_AT(&names, HostDeltaPagePage, 0);
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_append(host.self, name, record, sizeof(record), &offset, &error),
			   "a record appends to the owned page");
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_store(host.self, name, 0, 7, &error), "a word stores into the page");
	owner.shape = TEST_SIMC_TESTS_AGED.id;
	owner.location = location;
	owner.lock = 0;
	host.ops->delta_link(host.self, name, owner);
	host.ops->delta_publish(host.self, place);
	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken");
	STD_ASSERT(__LINE__, 5, host.ops->delta_open(host.self, name, held, &opened, &error), "the linked page opens");
	STD_ASSERT(__LINE__, 5, host.ops->delta_filled(host.self, &opened) == sizeof(record) &&
			   host.ops->delta_room(host.self, &opened) == size.bytes - sizeof(record),
			   "the page holds the record below its mark and the rest above it");
	STD_ASSERT(__LINE__, 5, host.ops->delta_bytes(host.self, &opened, 0, sizeof(record), &bytes, &error),
			   "the page's bytes are lent");
	read = host_cache_Bytes_Deref_deref(&bytes);
	STD_ASSERT(__LINE__, 5, read.len == sizeof(record) && memcmp(read.ptr, record, sizeof(record)) == 0,
			   "the bytes are the record");
	host_cache_Bytes_drop(&bytes);
	STD_ASSERT(__LINE__, 5, test_simc_tests_word(host, opened.body, HOST_DELTA_WORD_OP_LOAD, 0, 0) == 7,
			   "the word stored while owned loads");
	STD_ASSERT(__LINE__, 5, test_simc_tests_word(host, opened.body, HOST_DELTA_WORD_OP_EXCHANGE, 7, 9) == 7 &&
			   test_simc_tests_word(host, opened.body, HOST_DELTA_WORD_OP_EXCHANGE, 7, 11) == 9 &&
			   test_simc_tests_word(host, opened.body, HOST_DELTA_WORD_OP_ADD, 1, 0) == 9 &&
			   test_simc_tests_word(host, opened.body, HOST_DELTA_WORD_OP_STORE, 5, 0) == 0 &&
			   test_simc_tests_word(host, opened.body, HOST_DELTA_WORD_OP_LOAD, 0, 0) == 5,
			   "an exchange takes only its expected value, an add adds, and a store stores");
	STD_ASSERT(__LINE__, 5, host.ops->delta_append(host.self, &opened, owner, record, sizeof(record), &offset, &error) &&
			   offset == sizeof(record), "an append under the owner appends past the first record");
	STD_ASSERT(__LINE__, 5, host.ops->delta_seal(host.self, &opened, owner, &error), "the owner seals the page");
	STD_ASSERT(__LINE__, 5, host.ops->delta_room(host.self, &opened) == 0, "a sealed page has no room");
	STD_ASSERT(__LINE__, 5, !host.ops->delta_append(host.self, &opened, owner, record, sizeof(record), &offset, &error),
			   "a sealed page refuses an append");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, host.ops->delta_record(host.self, &TEST_SIMC_TESTS_AGED, location, held, &found, &error) &&
			   found == place, "the published record is found under the hold");
	STD_ASSERT(__LINE__, 5, !host.ops->delta_wait(host.self, &raise, 0, &error), "a wait under a hold fails");
	host_Error_drop(&error);
	host.ops->delta_release(host.self, held);
	leaving.id = TEST_SIMC_TESTS_AGED.id;
	leaving.location = location;
	host.ops->delta_retire(host.self, &leaving, 1, &name, 1);
	host.ops->delta_pass(host.self);
	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken again");
	STD_ASSERT(__LINE__, 5, !host.ops->delta_open(host.self, name, held, &opened, &error), "the freed page doesn't open");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, !host.ops->delta_record(host.self, &TEST_SIMC_TESTS_AGED, location, held, &found, &error),
			   "the freed record isn't found");
	host_Error_drop(&error);
	host.ops->delta_release(host.self, held);
	STD_ASSERT(__LINE__, 5, host.ops->delta_used(host.self) == before, "the pass gave the page's block back");
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_an_aged_record_unmade_is_given_back_and_a_replacing_page_is_taken:
 * the aged kind's two records are made and a third make answers full, an
 * unmade record is given back so the next make takes it, and a page taken to
 * replace an owned page is listed and given back.
 */
static void
test_simc_tests_an_aged_record_unmade_is_given_back_and_a_replacing_page_is_taken(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	StdVec		names;
	HostError	error;
	bool		made;
	bool		refused;
	uint64		places[2];
	uint64		place;
	uint64		location;
	uint64		frees;
	uint64		before;

	for (int i = 0; i < 2; i++)
		STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made,
													 &places[i], &location, &frees, &error) && made,
				   "a record of the aged kind is made");
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												 &location, &frees, &error) && !made, "a full aged kind answers full");
	host.ops->delta_unmake(host.self, places[1]);
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made,
												 &places[1], &location, &frees, &error) && made,
			   "an unmade record is given back for the next make");
	host.ops->delta_unmake(host.self, places[0]);
	host.ops->delta_unmake(host.self, places[1]);
	before = host.ops->delta_used(host.self);
	std_vec_with_capacity_Checked(&names, 2, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "a page is taken");
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc_replacing(host.self, &raise, size,
															STD_VEC_AT(&names, HostDeltaPagePage, 0), &names,
															&refused, &frees, &error) && !refused && names.len == 2,
			   "a page to replace it is taken and listed");
	host.ops->delta_give_back(host.self, STD_VEC_AT(&names, HostDeltaPagePage, 1));
	host.ops->delta_give_back(host.self, STD_VEC_AT(&names, HostDeltaPagePage, 0));
	STD_ASSERT(__LINE__, 5, host.ops->delta_used(host.self) == before, "both pages are given back");
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_RELEASED is the owned case's count of releases, and the slot of the last. */
static uint32 test_simc_tests_RELEASED[2];

/* test_simc_tests_release is the owned case's release: it counts the call and keeps the slot. */
static void
test_simc_tests_release(uint32 slot)
{
	test_simc_tests_RELEASED[0]++;
	test_simc_tests_RELEASED[1] = slot;
}

/*
 * test_simc_tests_an_owned_record_is_released_by_its_owners_abort_unless_forgotten:
 * a record the top-level owner remembers outlives a subtransaction's abort
 * and is released by the transaction's at its slot, and a record it forgot
 * isn't released.
 */
static void
test_simc_tests_an_owned_record_is_released_by_its_owners_abort_unless_forgotten(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	uint64		owner;

	STD_ASSERT(__LINE__, 5, host.ops->owner_top_enlarge(host.self, &raise, &owner, &error), "the top owner has room");
	host.ops->owned_remember(host.self, owner, UINT64CONST(1) << 32 | 7, test_simc_tests_release);
	simc_begin_owner();
	simc_abort_owner();
	STD_ASSERT(__LINE__, 5, test_simc_tests_RELEASED[0] == 0, "a subtransaction's abort keeps the top owner's record");
	simc_abort_owner();
	STD_ASSERT(__LINE__, 5, test_simc_tests_RELEASED[0] == 1 && test_simc_tests_RELEASED[1] == 7,
			   "the transaction's abort releases the record at its slot");
	STD_ASSERT(__LINE__, 5, host.ops->owner_top_enlarge(host.self, &raise, &owner, &error), "the top owner has room");
	host.ops->owned_remember(host.self, owner, UINT64CONST(1) << 32 | 8, test_simc_tests_release);
	host.ops->owned_forget(host.self, owner, UINT64CONST(1) << 32 | 8);
	simc_abort_owner();
	STD_ASSERT(__LINE__, 5, test_simc_tests_RELEASED[0] == 1, "a forgotten record isn't released");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_nap_is_counted_and_the_disk_space_set_is_answered:
 * the naps asked for since the count was last taken are counted, the take
 * starts the count again, and the disk space set is what the door answers.
 */
static void
test_simc_tests_a_nap_is_counted_and_the_disk_space_set_is_answered(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	StdDuration napped;
	StdDuration nap = {.secs = 2, .nanos = 500};
	uint64		free;
	uint64		total;

	(void) simc_napped();
	STD_ASSERT(__LINE__, 5, host.ops->nap(host.self, &raise, nap, &error) &&
			   host.ops->nap(host.self, &raise, nap, &error), "two naps are taken");
	napped = simc_napped();
	STD_ASSERT(__LINE__, 5, napped.secs == 4 && napped.nanos == 1000, "the naps are counted");
	napped = simc_napped();
	STD_ASSERT(__LINE__, 5, napped.secs == 0 && napped.nanos == 0, "the take starts the count again");
	simc_set_disk_space(5, 10);
	STD_ASSERT(__LINE__, 5, host.ops->disk_space(host.self, &raise, ".", 1, &free, &total, &error) && free == 5 &&
			   total == 10, "the disk space set is answered");
	simc_set_disk_space(UINT64CONST(1) << 40, UINT64CONST(2) << 40);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_panicking_release is an owned kind's release that panics with a literal message. */
static void
test_simc_tests_panicking_release(uint32 slot)
{
	static const char text[] = "the release failed";

	std_panic(RUST_FILE, __LINE__, 5, text, sizeof(text) - 1);
}

/* test_simc_tests_panicking_abort aborts the top-level owner, whose record's release panics. */
static void
test_simc_tests_panicking_abort(void *captures)
{
	simc_abort_owner();
}

/*
 * test_simc_tests_a_c_releases_panic_unwinds_the_owners_abort_with_its_payload:
 * a C release's panic unwinds the simulation's abort of its owner as a Rust
 * unwind and reaches the C's catch with the release's own payload.
 */
static void
test_simc_tests_a_c_releases_panic_unwinds_the_owners_abort_with_its_payload(void)
{
	static const char expected[] = "the release failed";
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	StdCaught	caught;
	const char *text;
	size_t		len;
	uint64		owner;

	STD_ASSERT(__LINE__, 5, host.ops->owner_top_enlarge(host.self, &raise, &owner, &error), "the top owner has room");
	host.ops->owned_remember(host.self, owner, UINT64CONST(1) << 32 | 9, test_simc_tests_panicking_release);
	STD_ASSERT(__LINE__, 5, !std_catch_unwind(test_simc_tests_panicking_abort, NULL, &caught), "the abort unwinds");
	STD_ASSERT(__LINE__, 5, std_Caught_downcast_str(&caught, &text, &len) && len == sizeof(expected) - 1 &&
			   memcmp(text, expected, len) == 0, "the unwind carries the release's payload");
	std_Caught_drop(&caught);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_historic_snapshot_lends_its_bounds_command_and_sorted_xids:
 * a historic snapshot lends its kind, its bounds, and its command, the
 * committed and the own xids each sorted, and the taken snapshot as its
 * own, of its type.
 */
static void
test_simc_tests_a_historic_snapshot_lends_its_bounds_command_and_sorted_xids(void)
{
	HostXid		committed[] = {{720}, {705}};
	HostXid		own[] = {{703}};
	HostXid		xmin = {701};
	HostXid		xmax = {730};
	HostCommand command = {4};
	SimcTaken	taken = simc_historic(xmin, xmax, committed, lengthof(committed), own, lengthof(own), command);
	HostSnapshot snapshot = simc_Taken_lent(&taken);

	STD_ASSERT(__LINE__, 5, snapshot.kind == HOST_SNAPSHOT_KIND_HISTORIC_MVCC && snapshot.xmin.f0 == 701 &&
			   snapshot.xmax.f0 == 730 && snapshot.command.f0 == 4 && !snapshot.recovery && !snapshot.overflowed,
			   "the snapshot lends its kind, bounds, and command");
	STD_ASSERT(__LINE__, 5, snapshot.xip_len == 2 && snapshot.xip[0] == 705 && snapshot.xip[1] == 720 &&
			   snapshot.subxip_len == 1 && snapshot.subxip[0] == 703, "the snapshot lends its xids sorted");
	STD_ASSERT(__LINE__, 5, snapshot.of.value == taken.f0 && snapshot.of.type != NULL &&
			   strcmp(snapshot.of.type->name, "sim::Taken") == 0, "the snapshot's own is the taken one, of its type");
	simc_Taken_drop(&taken);
}

/* test_simc_tests_returning is the report case's actor: it returns at once. */
static bool
test_simc_tests_returning(void *captures, HostIoIo *io, HostError *error)
{
	return true;
}

/*
 * test_simc_tests_a_reports_debug_is_the_rusts:
 * a finished run's report formats as the Rust's derived Debug of it: its
 * outcome, its trace, and its exits.
 */
static void
test_simc_tests_a_reports_debug_is_the_rusts(void)
{
	static const char expected[] = "Report { outcome: Ended([(\"returner\", Returned(Ok(())))]), trace: [";
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(5);
	SimcActor	actor = {test_simc_tests_returning};
	SimcReport	report;
	StdString text;
	StdFmtArg	args[1];
	StdFmtArguments formatted = {pieces, lengthof(pieces), args};

	simc_Run_actor(&run, "returner", 8, actor);
	report = simc_Run_finish(run);
	args[0].value = &report;
	args[0].fmt = simc_Report_Debug_fmt;
	std_fmt_format_Checked(&text, &formatted);
	STD_ASSERT(__LINE__, 5, text.len > sizeof(expected) - 1 &&
			   memcmp(text.ptr, expected, sizeof(expected) - 1) == 0, "the report formats as the Rust's");
	std_vec_free_Checked(&text, 1, 1);
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_transactions_status_answers_as_the_simulations_host:
 * a transaction begun is in progress and not the running process's own, its
 * commit makes it committed and no longer in progress, another's abort makes
 * it aborted, and the running transactions are answered for.
 */
static void
test_simc_tests_a_transactions_status_answers_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostStatus	status;
	HostXid		committed = simc_begin();
	HostXid		aborted;
	bool		running;

	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, committed, &status, &error) &&
			   status == HOST_STATUS_IN_PROGRESS, "a begun transaction is in progress");
	STD_ASSERT(__LINE__, 5, host.ops->xid_in_progress(host.self, &raise, committed, &running, &error) && running,
			   "a begun transaction is running");
	STD_ASSERT(__LINE__, 5, !host.ops->xid_is_current(host.self, committed),
			   "a transaction begun for a test isn't the process's own");
	simc_commit(committed);
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, committed, &status, &error) &&
			   status == HOST_STATUS_COMMITTED, "a committed transaction is committed");
	STD_ASSERT(__LINE__, 5, host.ops->xid_in_progress(host.self, &raise, committed, &running, &error) && !running,
			   "a committed transaction isn't running");
	aborted = simc_begin();
	STD_ASSERT(__LINE__, 5, aborted.f0 > committed.f0, "the next transaction takes a later xid");
	simc_abort(aborted);
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, aborted, &status, &error) &&
			   status == HOST_STATUS_ABORTED, "an aborted transaction is aborted");
	STD_ASSERT(__LINE__, 5, host.ops->xids_answered(host.self), "the running transactions are answered for");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_subtransaction_runs_until_its_abort_under_its_running_parent:
 * a subtransaction begun under a running transaction takes a later xid and
 * is in progress, its abort makes it aborted while its parent stays in
 * progress, and the parent's commit makes the parent committed.
 */
static void
test_simc_tests_a_subtransaction_runs_until_its_abort_under_its_running_parent(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostStatus	status;
	HostXid		parent = simc_begin();
	HostXid		sub = simc_begin_under(parent);

	STD_ASSERT(__LINE__, 5, sub.f0 > parent.f0, "the subtransaction takes a later xid");
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, sub, &status, &error) &&
			   status == HOST_STATUS_IN_PROGRESS, "a begun subtransaction is in progress");
	simc_abort(sub);
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, sub, &status, &error) &&
			   status == HOST_STATUS_ABORTED, "an aborted subtransaction is aborted");
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, parent, &status, &error) &&
			   status == HOST_STATUS_IN_PROGRESS, "the parent stays in progress");
	simc_commit(parent);
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, parent, &status, &error) &&
			   status == HOST_STATUS_COMMITTED, "the committed parent is committed");
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_door_refused reports whether an error is the refusal of a door a test refused, and drops it. */
static bool
test_simc_tests_door_refused(HostError *error)
{
	static const char message[] = "a test refused the delta's door";
	StdStr text = host_Error_message(error);
	bool		refused = memcmp(host_Error_sqlstate(error), "XX000", 5) == 0 && text.len == sizeof(message) - 1 &&
		memcmp(text.ptr, message, text.len) == 0;

	host_Error_drop(error);
	return refused;
}

/*
 * test_simc_tests_a_refused_open_and_a_refused_record_fail_once_with_the_refusal:
 * a page's open refused at once fails with the refusal and the next open of
 * the page is the delta's own answer, and an aged record's open refused at
 * once fails with the refusal and the next finds the record.
 */
static void
test_simc_tests_a_refused_open_and_a_refused_record_fail_once_with_the_refusal(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostDeltaDirectoryOpened opened;
	HostDeltaLeaving leaving;
	HostDeltaPagePage name;
	StdVec		names;
	HostError	error;
	bool		refused;
	bool		made;
	uint64		frees;
	uint64		held;
	uint64		place;
	uint64		location;
	uint64		found;

	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "a page is taken");
	name = STD_VEC_AT(&names, HostDeltaPagePage, 0);
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												 &location, &frees, &error) && made, "an aged record is made");
	host.ops->delta_publish(host.self, place);
	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken");
	simc_refuse_open(name, 0);
	STD_ASSERT(__LINE__, 5, !host.ops->delta_open(host.self, name, held, &opened, &error) &&
			   test_simc_tests_door_refused(&error), "the refused open fails with the refusal");
	STD_ASSERT(__LINE__, 5, !host.ops->delta_open(host.self, name, held, &opened, &error) &&
			   !test_simc_tests_door_refused(&error), "the next open is the delta's own answer");
	simc_refuse_record(TEST_SIMC_TESTS_AGED.id, location, 0);
	STD_ASSERT(__LINE__, 5, !host.ops->delta_record(host.self, &TEST_SIMC_TESTS_AGED, location, held, &found, &error) &&
			   test_simc_tests_door_refused(&error), "the refused record fails with the refusal");
	STD_ASSERT(__LINE__, 5, host.ops->delta_record(host.self, &TEST_SIMC_TESTS_AGED, location, held, &found, &error) &&
			   found == place, "the next open finds the record");
	host.ops->delta_release(host.self, held);
	leaving.id = TEST_SIMC_TESTS_AGED.id;
	leaving.location = location;
	host.ops->delta_retire(host.self, &leaving, 1, NULL, 0);
	host.ops->delta_pass(host.self);
	host.ops->delta_give_back(host.self, name);
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_refused_take_fails_once_and_refuse_nothing_takes_back_the_rest:
 * after a refusal of any take past one, a page's allocation is the delta's
 * own, the record make after it fails with the refusal, and the next make is
 * the delta's own; refuse_nothing takes back a take's and a record's
 * refusals that haven't come.
 */
static void
test_simc_tests_a_refused_take_fails_once_and_refuse_nothing_takes_back_the_rest(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostDeltaLeaving leaving;
	HostDeltaPagePage first;
	StdVec		names;
	HostError	error;
	bool		refused;
	bool		made;
	uint64		frees;
	uint64		held;
	uint64		place;
	uint64		location;
	uint64		found;

	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_refuse_any_take(1);
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "the first take is the delta's own");
	first = STD_VEC_AT(&names, HostDeltaPagePage, 0);
	names.len = 0;
	STD_ASSERT(__LINE__, 5, !host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												  &location, &frees, &error) && test_simc_tests_door_refused(&error),
			   "the refused make fails with the refusal");
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												 &location, &frees, &error) && made, "the next make is the delta's own");
	host.ops->delta_publish(host.self, place);
	simc_refuse_any_take(0);
	simc_refuse_record(TEST_SIMC_TESTS_AGED.id, location, 0);
	simc_refuse_nothing();
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "a take refused and taken back is the delta's own");
	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken");
	STD_ASSERT(__LINE__, 5, host.ops->delta_record(host.self, &TEST_SIMC_TESTS_AGED, location, held, &found, &error) &&
			   found == place, "a record refused and taken back is found");
	host.ops->delta_release(host.self, held);
	leaving.id = TEST_SIMC_TESTS_AGED.id;
	leaving.location = location;
	host.ops->delta_retire(host.self, &leaving, 1, NULL, 0);
	host.ops->delta_pass(host.self);
	host.ops->delta_give_back(host.self, first);
	host.ops->delta_give_back(host.self, STD_VEC_AT(&names, HostDeltaPagePage, 0));
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_an_owned_append_refused_after_a_count_fails_once:
 * after a refusal of an owned page's append past one, the first append is
 * taken, the second fails with the internal error, and the third is taken.
 */
static void
test_simc_tests_an_owned_append_refused_after_a_count_fails_once(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostDeltaPagePage name;
	StdVec		names;
	HostError	error;
	bool		refused;
	uint64		frees;
	uint32		offset;
	static const uint8 record[] = {1, 2, 3};

	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "a page is taken");
	name = STD_VEC_AT(&names, HostDeltaPagePage, 0);
	simc_refuse_owned_append(1);
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_append(host.self, name, record, sizeof(record), &offset, &error),
			   "the first append is taken");
	STD_ASSERT(__LINE__, 5, !host.ops->delta_owned_append(host.self, name, record, sizeof(record), &offset, &error) &&
			   memcmp(host_Error_sqlstate(&error), "XX000", 5) == 0, "the second append is refused");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, host.ops->delta_owned_append(host.self, name, record, sizeof(record), &offset, &error),
			   "the third append is taken");
	host.ops->delta_give_back(host.self, name);
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host:
 * a pause outside a run, a warning of a C error, a log, and the last redo
 * point each answer as the simulation's host does.
 */
static void
test_simc_tests_a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostError	warned;
	StdString	message;
	HostLsn		redo;

	STD_ASSERT(__LINE__, 5, host.ops->pause(host.self, &raise, "doltgres-simc-case-pause", &error),
			   "a pause outside a run returns");
	std_string_from_Checked(&message, "a warned error", strlen("a warned error"));
	warned = host_Error_fork((const uint8 *) "01000", &message);
	STD_ASSERT(__LINE__, 5, host.ops->warn(host.self, &raise, &warned, "a context", &error), "a warning is reported");
	STD_ASSERT(__LINE__, 5, host.ops->log(host.self, &raise, "a logged message", &error), "a log is reported");
	STD_ASSERT(__LINE__, 5, host.ops->last_redo(host.self, &raise, &redo, &error), "the last redo point is read");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_checkpoint_sets_the_redo_point_the_host_answers: a
 * checkpoint sets the redo point the simulation's host answers for the last
 * redo.
 */
static void
test_simc_tests_a_checkpoint_sets_the_redo_point_the_host_answers(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostLsn		redo;

	simc_checkpointed(0x1234);
	STD_ASSERT(__LINE__, 5, host.ops->last_redo(host.self, &raise, &redo, &error) && redo == 0x1234,
			   "the host answers the checkpoint's redo point");
	simc_checkpointed(0);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs:
 * a crash planned at a call fails it and every call after, and a recovery
 * takes a file back to its last sync and ends the crash.
 */
static void
test_simc_tests_a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIoDoors io = HOST_IO_IO_IO_DOORS(&test_io.io);
	SimcTempDir dir;
	StdIoError	io_error;
	HostError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	a;
	HostFile	file;
	SimcSim		sim;
	uint8		bytes[4];
	bool		has;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &io_error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a", 1, &a);
	sim = simc_Sim_start(dir_path, dir_path_len, 9);
	STD_ASSERT(__LINE__, 5, host_create_if_absent(io, a.bytes.ptr, a.bytes.len, &has, &file, &error) && has &&
			   host_write_at(io, &file, 0, (const uint8 *) "kept", 4, &error) && host_sync(io, &file, &error) &&
			   host_sync_path(io, dir_path, dir_path_len, true, &error), "a file is written and synced");
	simc_Sim_crash_at(&sim, simc_Sim_calls(&sim) + 1);
	STD_ASSERT(__LINE__, 5, !host_write_at(io, &file, 0, (const uint8 *) "lost", 4, &error),
			   "the planned call crashes");
	host_Error_drop(&error);
	STD_ASSERT(__LINE__, 5, !host_write_at(io, &file, 0, (const uint8 *) "lost", 4, &error),
			   "the call after the crash fails too");
	host_Error_drop(&error);
	host_File_drop(&file);
	STD_ASSERT(__LINE__, 5, simc_Sim_recover(&sim, false, &io_error), "the recovery takes the directory back");
	STD_ASSERT(__LINE__, 5, host_open(io, a.bytes.ptr, a.bytes.len, HOST_OPEN_READ_WRITE, &file, &error) &&
			   host_read_at(HOST_IO_IO_READ_DOORS(&test_io.io), &file, 0, bytes, 4, &error) &&
			   memcmp(bytes, "kept", 4) == 0, "the file reads as its last sync left it");
	STD_ASSERT(__LINE__, 5, host_write_at(io, &file, 0, (const uint8 *) "next", 4, &error),
			   "a call after the recovery succeeds");
	host_File_drop(&file);
	simc_Sim_stop(&sim);
	simc_Sim_drop(&sim);
	std_vec_free_Checked(&a.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_test_only_std_fs_makes_a_directory_with_its_parents:
 * the test-only std::fs makes a directory and each parent it lacks, and
 * makes one that stands again without an error.
 */
static void
test_simc_tests_the_test_only_std_fs_makes_a_directory_with_its_parents(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	nested;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a/b", 3, &nested);
	STD_ASSERT(__LINE__, 5, std_fs_create_dir_all(nested.bytes.ptr, nested.bytes.len, &error),
			   "the directory is made with its parent");
	STD_ASSERT(__LINE__, 5, std_path_Path_exists(nested.bytes.ptr, nested.bytes.len), "the directory stands");
	STD_ASSERT(__LINE__, 5, std_fs_create_dir_all(nested.bytes.ptr, nested.bytes.len, &error),
			   "a directory that stands is made again");
	std_vec_free_Checked(&nested.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_test_only_std_fs_lists_a_directorys_names: the
 * test-only std's read of a directory lists its entries' names, a renamed
 * file under its new name, and a missing directory fails with the
 * directory's error.
 */
static void
test_simc_tests_the_test_only_std_fs_lists_a_directorys_names(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	file;
	StdPathBuf	renamed;
	StdPathBuf	missing;
	StdVec		names;
	bool		entry;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a", 1, &file);
	std_path_join(dir_path, dir_path_len, "b", 1, &renamed);
	std_path_join(dir_path, dir_path_len, "missing", 7, &missing);
	STD_ASSERT(__LINE__, 5, std_fs_write(file.bytes.ptr, file.bytes.len, (const uint8 *) "a", 1, &error),
			   "the file is written");
	STD_ASSERT(__LINE__, 5, std_fs_rename(file.bytes.ptr, file.bytes.len, renamed.bytes.ptr, renamed.bytes.len,
										  &error), "the file is renamed");
	STD_ASSERT(__LINE__, 5, std_fs_read_dir_names(dir_path, dir_path_len, &names, &entry, &error) &&
			   names.len == 1 && STD_VEC_AT(&names, StdString, 0).len == 1 &&
			   memcmp(STD_VEC_AT(&names, StdString, 0).ptr, "b", 1) == 0, "the directory lists its file's name");
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	STD_ASSERT(__LINE__, 5, !std_fs_read_dir_names(missing.bytes.ptr, missing.bytes.len, &names, &entry, &error) &&
			   !entry, "a missing directory fails its read");
	std_vec_free_Checked(&missing.bytes, 1, 1);
	std_vec_free_Checked(&renamed.bytes, 1, 1);
	std_vec_free_Checked(&file.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_records_slots_answer_as_the_simulations_host: a pooled
 * record's slots array answers each door as the simulation's host does: its
 * name renames only from the name loaded, a claim takes an entry until every
 * one is taken, a read returns the entries claimed, in order, and an emptied
 * array reads none.
 */
static void
test_simc_tests_a_records_slots_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	bool		has;
	bool		claimed;
	uint64		place;
	StdVec		read;
	static const uint8 first[8] = "first!!";
	static const uint8 second[8] = "second!";

	STD_ASSERT(__LINE__, 5, host.ops->shared_make(host.self, &raise, &TEST_SIMC_TESTS_POOLED, &has, &place, &error) &&
			   has, "the pool makes a record");
	STD_ASSERT(__LINE__, 5, host.ops->slots_name(host.self, place, 0) == 0, "a new array names nothing");
	STD_ASSERT(__LINE__, 5, host.ops->slots_rename(host.self, place, 0, 0, 7), "the array is named from nothing");
	STD_ASSERT(__LINE__, 5, !host.ops->slots_rename(host.self, place, 0, 0, 9), "a stale name renames nothing");
	STD_ASSERT(__LINE__, 5, host.ops->slots_name(host.self, place, 0) == 7, "the array keeps its name");
	STD_ASSERT(__LINE__, 5, host.ops->slots_claim(host.self, &raise, place, 0, first, sizeof(first), &claimed,
												  &error) && claimed, "the first entry is claimed");
	STD_ASSERT(__LINE__, 5, host.ops->slots_claim(host.self, &raise, place, 0, second, sizeof(second), &claimed,
												  &error) && claimed, "the second entry is claimed");
	STD_ASSERT(__LINE__, 5, host.ops->slots_claim(host.self, &raise, place, 0, first, sizeof(first), &claimed,
												  &error) && !claimed, "a full array claims nothing");
	read = host.ops->slots_read(host.self, place, 0);
	STD_ASSERT(__LINE__, 5, read.len == 2 && STD_VEC_AT(&read, StdVec, 0).len == 8 &&
			   memcmp(STD_VEC_AT(&read, StdVec, 0).ptr, first, 8) == 0 &&
			   memcmp(STD_VEC_AT(&read, StdVec, 1).ptr, second, 8) == 0, "the array reads its entries in order");
	for (size_t k = 0; k < read.len; k++)
		std_vec_free_Checked(&STD_VEC_AT(&read, StdVec, k), 1, 1);
	std_vec_free_Checked(&read, sizeof(StdVec), _Alignof(StdVec));
	host.ops->slots_empty(host.self, place, 0);
	read = host.ops->slots_read(host.self, place, 0);
	STD_ASSERT(__LINE__, 5, read.len == 0, "an emptied array reads no entry");
	std_vec_free_Checked(&read, sizeof(StdVec), _Alignof(StdVec));
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_ORDER is the scripted case's actors' finishes, in the order they came. */
static int	test_simc_tests_ORDER[2];

/* test_simc_tests_FINISHED counts the scripted case's finishes. */
static int	test_simc_tests_FINISHED;

/* test_simc_tests_pausing_actor is the scripted case's body: a pause at its point, then its finish recorded. */
static bool
test_simc_tests_pausing_actor(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	int			which = (int) (intptr_t) captures;

	if (!host.ops->pause(host.self, &raise, which == 1 ? "doltgres-simc-case-first" : "doltgres-simc-case-second",
						 error))
		return false;
	test_simc_tests_ORDER[test_simc_tests_FINISHED++] = which;
	return true;
}

/* test_simc_tests_looping_actor is the scripted case's endless body, which yields at every call until terminated. */
static bool
test_simc_tests_looping_actor(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();

	for (;;)
		(void) host.ops->epochs_oldest(host.self);
	return true;
}

/*
 * test_simc_tests_a_scripted_run_runs_each_actor_until_its_point_and_terminates_one:
 * a run whose script runs the first actor until its pause and then the
 * second to its end finishes the second first, and an actor terminated at a
 * count of yields ends terminated.
 */
static void
test_simc_tests_a_scripted_run_runs_each_actor_until_its_point_and_terminates_one(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(13);
	SimcActor	first = {test_simc_tests_pausing_actor, (void *) (intptr_t) 1};
	SimcActor	second = {test_simc_tests_pausing_actor, (void *) (intptr_t) 2};
	SimcActor	looping = {test_simc_tests_looping_actor};
	SimcStep	steps[] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcReport	report;
	const simc_TupleStringEnding_t *endings;

	test_simc_tests_FINISHED = 0;
	simc_Run_actor(&run, "first", 5, first);
	simc_Run_actor(&run, "second", 6, second);
	simc_Run_actor(&run, "looper", 6, looping);
	simc_Run_until(&run, "first", 5, "doltgres-simc-case-first");
	simc_Run_script(&run, steps, lengthof(steps));
	simc_Run_terminate_at(&run, "looper", 6, 50);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED && report.outcome.u.ended.len == 3,
			   "the run ends");
	endings = report.outcome.u.ended.ptr;
	STD_ASSERT(__LINE__, 5, endings[0].f1.tag == SIMC_ENDING_RETURNED && !endings[0].f1.is_err &&
			   endings[1].f1.tag == SIMC_ENDING_RETURNED && !endings[1].f1.is_err, "the pausing actors return");
	STD_ASSERT(__LINE__, 5, endings[2].f1.tag == SIMC_ENDING_TERMINATED, "the looping actor is terminated");
	STD_ASSERT(__LINE__, 5, test_simc_tests_FINISHED == 2 && test_simc_tests_ORDER[0] == 2 &&
			   test_simc_tests_ORDER[1] == 1, "the second actor finishes first");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_trace_Debug_fmt is the trace case's Vec<usize> Debug. */
static bool
test_simc_tests_trace_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *trace = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < trace->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(trace, size_t, k), std_usize_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * test_simc_tests_a_reports_trace_is_the_rusts:
 * a finished run's trace is its report's, each yield point's actor in order
 * as the Rust's Debug of the report shows it, the first actor's pause first.
 */
static void
test_simc_tests_a_reports_trace_is_the_rusts(void)
{
	static const StdFmtPiece report_pieces[] = {STD_FMT_ARG(0)};
	static const StdFmtPiece trace_pieces[] = {STD_FMT_LIT("trace: "), STD_FMT_ARG(0), STD_FMT_LIT(", exits")};
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(13);
	SimcActor	first = {test_simc_tests_pausing_actor, (void *) (intptr_t) 1};
	SimcActor	second = {test_simc_tests_pausing_actor, (void *) (intptr_t) 2};
	SimcStep	steps[] = {{"second", 6, PG_UINT64_MAX}, {"first", 5, PG_UINT64_MAX}};
	SimcReport	report;
	StdVec		trace;
	StdString text;
	StdString	shown;
	StdFmtArg	report_args[1];
	StdFmtArg	trace_args[1];
	StdFmtArguments report_formatted = {report_pieces, lengthof(report_pieces), report_args};
	StdFmtArguments trace_formatted = {trace_pieces, lengthof(trace_pieces), trace_args};
	bool		found = false;

	test_simc_tests_FINISHED = 0;
	simc_Run_actor(&run, "first", 5, first);
	simc_Run_actor(&run, "second", 6, second);
	simc_Run_until(&run, "first", 5, "doltgres-simc-case-first");
	simc_Run_script(&run, steps, lengthof(steps));
	report = simc_Run_finish(run);
	trace = simc_Report_trace(&report);
	report_args[0].value = &report;
	report_args[0].fmt = simc_Report_Debug_fmt;
	std_fmt_format_Checked(&text, &report_formatted);
	trace_args[0].value = &trace;
	trace_args[0].fmt = test_simc_tests_trace_Debug_fmt;
	std_fmt_format_Checked(&shown, &trace_formatted);
	for (size_t at = 0; !found && at + shown.len <= text.len; at++)
		found = memcmp((const uint8 *) text.ptr + at, shown.ptr, shown.len) == 0;
	STD_ASSERT(__LINE__, 5, trace.len >= 2 && STD_VEC_AT(&trace, size_t, 0) == 0,
			   "the first actor's pause is the trace's first yield");
	STD_ASSERT(__LINE__, 5, found, "the trace is the report's");
	std_vec_free_Checked(&shown, 1, 1);
	std_vec_free_Checked(&text, 1, 1);
	std_vec_free_Checked(&trace, sizeof(size_t), _Alignof(size_t));
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_acting is the acting case's body: it records that it ran. */
static void
test_simc_tests_acting(void *captures)
{
	*(bool *) captures = true;
}

/* test_simc_tests_panicking is the acting case's panicking body. */
static void
test_simc_tests_panicking(void *captures)
{
	std_panic(RUST_FILE, __LINE__, 5, "an acting body panics", strlen("an acting body panics"));
}

/* test_simc_tests_act_panicking runs the panicking body as an actor, for the C's catch. */
static void
test_simc_tests_act_panicking(void *captures)
{
	simc_as_actor(3, test_simc_tests_panicking, NULL);
}

/*
 * test_simc_tests_a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory:
 * a body run as an actor runs on the calling thread, a panic of its unwinds
 * the C with its message, and the simulated server's data directory is
 * there.
 */
static void
test_simc_tests_a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory(void)
{
	static const char expected[] = "an acting body panics";
	SimcTestIo	test_io = simc_io();
	bool		acted = false;
	StdCaught	caught;
	const char *text;
	size_t		len;
	StdPathBuf	data;
	bool		has;
	StdVec		names;
	HostError	error;

	simc_as_actor(3, test_simc_tests_acting, &acted);
	STD_ASSERT(__LINE__, 5, acted, "the body ran");
	STD_ASSERT(__LINE__, 5, !std_catch_unwind(test_simc_tests_act_panicking, NULL, &caught),
			   "a panicking body unwinds");
	STD_ASSERT(__LINE__, 5, std_Caught_downcast_str(&caught, &text, &len), "the panic's payload is a str");
	STD_ASSERT(__LINE__, 5, len == strlen(expected) && memcmp(text, expected, len) == 0,
			   "the panic keeps its message");
	std_Caught_drop(&caught);
	data = simc_data_dir();
	STD_ASSERT(__LINE__, 5, host_list_dir_if_there(HOST_IO_IO_READ_DOORS(&test_io.io), data.bytes.ptr, data.bytes.len,
												   &has, &names, &error) && has, "the data directory is there");
	std_vec_drop_Checked(&names, sizeof(StdString), _Alignof(StdString), std_String_drop);
	std_vec_free_Checked(&data.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* CPORT_TESTS are simc's tests, sorted by name. */
/* test_simc_tests_warnings_drop drops the reports at WARNING simc took, their messages first. */
static void
test_simc_tests_warnings_drop(StdVec *warnings)
{
	for (size_t k = 0, k_len = warnings->len; k < k_len; k++)
		std_String_drop(&STD_VEC_AT(warnings, simc_TupleSqlstateString_t, k).f1);
	std_vec_free_Checked(warnings, sizeof(simc_TupleSqlstateString_t), _Alignof(simc_TupleSqlstateString_t));
}

/*
 * test_simc_tests_a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them:
 * the reports at WARNING and at LOG the work made, and the range of a read
 * of the kept WAL, are taken once each as the simulation took them, and the
 * WAL stands where the simulation's host says.
 */
static void
test_simc_tests_a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostError	warned;
	StdString	message;
	StdVec		warnings;
	StdVec		logs;
	HostLsn		from;
	HostLsn		to;
	HostWalStand stood;
	static const char expected[] = "a context: a warned error";
	static const char logged[] = "a logged message";

	warnings = simc_take_warnings();
	test_simc_tests_warnings_drop(&warnings);
	logs = simc_take_logs();
	std_vec_drop_Checked(&logs, sizeof(StdString), _Alignof(StdString), std_String_drop);
	(void) simc_kept_wal_read(&from, &to);
	std_string_from_Checked(&message, "a warned error", strlen("a warned error"));
	warned = host_Error_fork((const uint8 *) "01000", &message);
	STD_ASSERT(__LINE__, 5, host.ops->warn(host.self, &raise, &warned, "a context", &error), "a warning is reported");
	STD_ASSERT(__LINE__, 5, host.ops->log(host.self, &raise, logged, &error), "a log is reported");
	STD_ASSERT(__LINE__, 5, host.ops->warn_kept_wal(host.self, &raise, 50, 70, 1, 90, &error),
			   "a read of the kept WAL is reported");
	warnings = simc_take_warnings();
	STD_ASSERT(__LINE__, 5, warnings.len == 1 &&
			   memcmp(STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f0, "01000", 5) == 0 &&
			   STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f1.len == strlen(expected) &&
			   memcmp(STD_VEC_AT(&warnings, simc_TupleSqlstateString_t, 0).f1.ptr, expected, strlen(expected)) == 0,
			   "the warning is taken with its SQLSTATE and its context's message");
	test_simc_tests_warnings_drop(&warnings);
	warnings = simc_take_warnings();
	STD_ASSERT(__LINE__, 5, warnings.len == 0, "a warning is taken once");
	test_simc_tests_warnings_drop(&warnings);
	logs = simc_take_logs();
	STD_ASSERT(__LINE__, 5, logs.len == 1 && STD_VEC_AT(&logs, StdString, 0).len == strlen(logged) &&
			   memcmp(STD_VEC_AT(&logs, StdString, 0).ptr, logged, strlen(logged)) == 0, "the log is taken");
	std_vec_drop_Checked(&logs, sizeof(StdString), _Alignof(StdString), std_String_drop);
	STD_ASSERT(__LINE__, 5, simc_kept_wal_read(&from, &to) && from == 50 && to == 70,
			   "the kept WAL's read is taken with its range");
	STD_ASSERT(__LINE__, 5, !simc_kept_wal_read(&from, &to), "a read of the kept WAL is taken once");
	stood = host.ops->wal_stand(host.self);
	STD_ASSERT(__LINE__, 5, !stood.recovering && stood.insertion_timeline == 1 && stood.flushed == PG_UINT64_MAX &&
			   stood.replayed == 0 && stood.replayed_timeline == 0 && stood.target_timeline == 1,
			   "the WAL stands where the simulation's host says");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host:
 * a WAL record's insert answers the end of the record, past the one before
 * it by the record's header and body, a checkpoint sets the last redo point
 * to the WAL's end, and a database's quiet and its end each answer.
 */
static void
test_simc_tests_the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostLsn		first;
	HostLsn		second;
	HostLsn		redo;
	static const uint8 body[] = {1, 2, 3};

	STD_ASSERT(__LINE__, 5, host.ops->wal_insert(host.self, &raise, HOST_WAL_KIND_REPOSITORY_DROPPED, body, 1, &first,
												 &error), "a record is inserted");
	STD_ASSERT(__LINE__, 5, host.ops->wal_insert(host.self, &raise, HOST_WAL_KIND_REPOSITORY_CREATED, body,
												 lengthof(body), &second, &error) && second == first + 29 + 3,
			   "a record's insert answers its end, past the last by its header and its body");
	STD_ASSERT(__LINE__, 5, host.ops->checkpoint_now(host.self, &raise, &error), "a checkpoint is made");
	STD_ASSERT(__LINE__, 5, host.ops->last_redo(host.self, &raise, &redo, &error) && redo == second,
			   "a checkpoint's redo point is the WAL's end");
	STD_ASSERT(__LINE__, 5, host.ops->quiet_database(host.self, &raise, 7, &error), "a database is quieted");
	STD_ASSERT(__LINE__, 5, host.ops->unquiet_database(host.self, &raise, 7, &error), "a database's quiet ends");
	simc_checkpointed(0);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_sized_delta_answers_its_budget_and_counts_each_hold_begun:
 * a delta started afresh with a budget answers that budget, and each hold
 * begun counts one more.
 */
static void
test_simc_tests_a_sized_delta_answers_its_budget_and_counts_each_hold_begun(void)
{
	SimcTestIo	test_io;
	HostHost	host;
	HostMayRaise raise;
	HostError	error;
	uint64		holds;
	uint64		held;

	simc_size_delta(UINT64CONST(8) << 20);
	test_io = simc_io();
	host = host_current();
	raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	STD_ASSERT(__LINE__, 5, host.ops->delta_budget(host.self) == UINT64CONST(8) << 20,
			   "the delta answers its budget");
	holds = simc_delta_holds();
	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken");
	STD_ASSERT(__LINE__, 5, simc_delta_holds() == holds + 1, "the hold is counted");
	host.ops->delta_release(host.self, held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_test_only_std_fs_removes_a_directory_with_its_contents:
 * the test-only std::fs removes a directory with the files and directories
 * in it, and a removal of a directory that is gone fails with its error.
 */
static void
test_simc_tests_the_test_only_std_fs_removes_a_directory_with_its_contents(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	top;
	StdPathBuf	nested;
	StdPathBuf	file;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "a", 1, &top);
	std_path_join(top.bytes.ptr, top.bytes.len, "b", 1, &nested);
	std_path_join(nested.bytes.ptr, nested.bytes.len, "c", 1, &file);
	STD_ASSERT(__LINE__, 5, std_fs_create_dir_all(nested.bytes.ptr, nested.bytes.len, &error),
			   "the directory is made with its parent");
	STD_ASSERT(__LINE__, 5, std_fs_write(file.bytes.ptr, file.bytes.len, (const uint8 *) "x", 1, &error),
			   "a file is written in it");
	STD_ASSERT(__LINE__, 5, std_fs_remove_dir_all(top.bytes.ptr, top.bytes.len, &error),
			   "the directory is removed with its contents");
	STD_ASSERT(__LINE__, 5, !std_path_Path_exists(top.bytes.ptr, top.bytes.len), "the directory is gone");
	STD_ASSERT(__LINE__, 5, !std_fs_remove_dir_all(top.bytes.ptr, top.bytes.len, &error) &&
			   std_io_Error_kind(&error) == STD_ERROR_KIND_NOT_FOUND, "a directory that is gone fails to be removed");
	std_vec_free_Checked(&file.bytes, 1, 1);
	std_vec_free_Checked(&nested.bytes, 1, 1);
	std_vec_free_Checked(&top.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_btree_compare_door_answers_as_the_simulations_host: a
 * column's call compares two values as the simulation's function does, its
 * function 1 compares any two equal, and its function 0 raises its error.
 */
static void
test_simc_tests_the_btree_compare_door_answers_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostCompared reversed = {7, 0, -1, false};
	HostCompared equal = {1, 0, -1, false};
	HostCompared raising = {0, 0, -1, false};
	HostError	error;
	int32		answer;

	STD_ASSERT(__LINE__, 5, host.ops->btree_compare(host.self, &raise, &reversed, (const uint8 *) "ab", 2,
													(const uint8 *) "b", 1, &answer, &error) && answer > 0,
			   "the simulation's function 7 orders by the values' bytes reversed");
	STD_ASSERT(__LINE__, 5, host.ops->btree_compare(host.self, &raise, &equal, (const uint8 *) "x", 1,
													(const uint8 *) "y", 1, &answer, &error) && answer == 0,
			   "the simulation's function 1 compares any two values equal");
	STD_ASSERT(__LINE__, 5, !host.ops->btree_compare(host.self, &raise, &raising, (const uint8 *) "x", 1,
													 (const uint8 *) "y", 1, &answer, &error) &&
			   memcmp(host_Error_sqlstate(&error), "42883", 5) == 0,
			   "the simulation's function 0 raises its error");
	host_Error_drop(&error);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_running is the snapshot case's other actor: it begins a transaction it leaves running. */
static void
test_simc_tests_running(void *captures)
{
	*(HostXid *) captures = simc_begin();
}

/* test_simc_tests_snapshotting is the snapshot case's actor: it begins a second transaction, then takes two snapshots. */
static void
test_simc_tests_snapshotting(void *captures)
{
	HostXid		other = *(const HostXid *) captures;
	HostXid		second = simc_begin();
	SimcTaken	taken = simc_snapshot(HOST_SNAPSHOT_KIND_MVCC, false);
	HostSnapshot snapshot = simc_Taken_lent(&taken);

	STD_ASSERT(__LINE__, 5, snapshot.kind == HOST_SNAPSHOT_KIND_MVCC && !snapshot.recovery &&
			   snapshot.xmin.f0 == other.f0 && snapshot.xmax.f0 > second.f0, "the snapshot's kind and bounds");
	STD_ASSERT(__LINE__, 5, snapshot.xip_len == 2 && snapshot.xip[0] == (uint32) other.f0 &&
			   snapshot.xip[1] == (uint32) second.f0 && snapshot.subxip_len == 0,
			   "the snapshot holds the running top-level xids in order");
	simc_Taken_drop(&taken);
	taken = simc_snapshot(HOST_SNAPSHOT_KIND_DIRTY, true);
	snapshot = simc_Taken_lent(&taken);
	STD_ASSERT(__LINE__, 5, snapshot.kind == HOST_SNAPSHOT_KIND_DIRTY && snapshot.recovery && snapshot.xip_len == 0 &&
			   snapshot.subxip_len == 2 && snapshot.subxip[0] == (uint32) other.f0 &&
			   snapshot.subxip[1] == (uint32) second.f0, "in recovery's shape the running xids are subtransactions'");
	simc_Taken_drop(&taken);
	simc_commit(second);
	simc_commit(other);
}

/*
 * test_simc_tests_a_snapshot_takes_the_simulations_running_xids: a snapshot
 * of a kind holds the simulation's running top-level xids in order, its xmin
 * the oldest and its xmax past them, and in recovery's shape holds them as
 * subtransactions' xids.
 */
static void
test_simc_tests_a_snapshot_takes_the_simulations_running_xids(void)
{
	SimcTestIo	test_io = simc_io();
	HostXid		other;

	simc_as_actor(51, test_simc_tests_running, &other);
	simc_as_actor(52, test_simc_tests_snapshotting, &other);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_keying is the key doors' case's actor: it locks, counts, holds, waits on, and releases stripes. */
static void
test_simc_tests_keying(void *captures)
{
	static const uint32 stripes[] = {3, 5};
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS((HostIoIo *) captures));
	HostError	error;

	STD_ASSERT(__LINE__, 5, host.ops->key_lock(host.self, &raise, stripes, 2, &error), "a key lock takes free stripes");
	STD_ASSERT(__LINE__, 5, !host.ops->key_lock(host.self, &raise, stripes, 1, &error) &&
			   memcmp(host_Error_sqlstate(&error), "XX000", 5) == 0, "a key lock refuses a stripe its backend holds");
	host_Error_drop(&error);
	host.ops->key_unlock(host.self, stripes, 2, true);
	STD_ASSERT(__LINE__, 5, host.ops->key_count(host.self, 3) == 1 && host.ops->key_count(host.self, 5) == 1 &&
			   host.ops->key_count(host.self, 7) == 0, "an unlock that pushed counts a push on each of its stripes");
	STD_ASSERT(__LINE__, 5, host.ops->key_lock(host.self, &raise, stripes, 1, &error), "an unlocked stripe is free");
	host.ops->key_unlock(host.self, stripes, 1, false);
	STD_ASSERT(__LINE__, 5, host.ops->key_count(host.self, 3) == 1, "an unlock that pushed nothing counts nothing");
	STD_ASSERT(__LINE__, 5, host.ops->key_hold(host.self, 7) && host.ops->key_hold(host.self, 7),
			   "a hold-back flag is the running process's once it sets it");
	STD_ASSERT(__LINE__, 5, host.ops->key_held_wait(host.self, &raise, 7, &error),
			   "a wait on the running process's own flag returns at once");
	host.ops->key_release(host.self, 7);
}

/*
 * test_simc_tests_the_key_doors_answer_as_the_simulations_host: a key lock
 * takes its stripes and refuses one its backend holds, an unlock counts a
 * push on each stripe when it pushed, a hold-back flag is the running
 * process's once it sets it, and a wait on its own flag returns at once.
 */
static void
test_simc_tests_the_key_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(53, test_simc_tests_keying, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_delaying is the checkpoint delay doors' case's actor: it opens and closes two windows. */
static void
test_simc_tests_delaying(void *captures)
{
	static const char *const pauses[] = {"a-pause", "another-pause"};
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS((HostIoIo *) captures));
	HostError	error;

	STD_ASSERT(__LINE__, 5, host.ops->delay_checkpoints(host.self, &raise, pauses, lengthof(pauses), &error),
			   "a window opens with its pauses");
	host.ops->resume_checkpoints(host.self, true);
	STD_ASSERT(__LINE__, 5, host.ops->delay_checkpoints(host.self, &raise, NULL, 0, &error),
			   "a window opens again with no pauses once the last closed");
	host.ops->resume_checkpoints(host.self, false);
}

/*
 * test_simc_tests_a_taken_page_and_a_made_record_count_as_unfreed_until_freed:
 * a page taken counts as a page no pass has freed, a record made counts as
 * one of its kind's, and once the record is retired and a pass has run and
 * the page is given back, neither counts.
 */
static void
test_simc_tests_a_taken_page_and_a_made_record_count_as_unfreed_until_freed(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostDeltaLeaving leaving;
	StdVec		names;
	HostError	error;
	bool		refused;
	bool		made;
	uint64		frees;
	uint64		place;
	uint64		location;
	size_t		records;
	size_t		pages;

	simc_unfreed(TEST_SIMC_TESTS_AGED.id, &records, &pages);
	STD_ASSERT(__LINE__, 5, records == 0 && pages == 0, "nothing is unfreed at the start");
	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused && names.len == 1, "a page is taken");
	simc_unfreed(TEST_SIMC_TESTS_AGED.id, &records, &pages);
	STD_ASSERT(__LINE__, 5, records == 0 && pages == 1, "the taken page is unfreed");
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												 &location, &frees, &error) && made, "an aged record is made");
	host.ops->delta_publish(host.self, place);
	simc_unfreed(TEST_SIMC_TESTS_AGED.id, &records, &pages);
	STD_ASSERT(__LINE__, 5, records == 1 && pages == 1, "the made record is unfreed");
	leaving.id = TEST_SIMC_TESTS_AGED.id;
	leaving.location = location;
	host.ops->delta_retire(host.self, &leaving, 1, NULL, 0);
	host.ops->delta_pass(host.self);
	host.ops->delta_give_back(host.self, STD_VEC_AT(&names, HostDeltaPagePage, 0));
	simc_unfreed(TEST_SIMC_TESTS_AGED.id, &records, &pages);
	STD_ASSERT(__LINE__, 5, records == 0 && pages == 0, "the freed record and the given back page are not");
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_budget_t is the waiting case's holder's captures: the record
 * it retires, the count of waiters it saw, and the wakes counted before and
 * after its hold's end.
 */
typedef struct test_simc_tests_budget_t
{
	HostDeltaLeaving leaving;
	uint64		seen;
	uint64		before;
	uint64		after;
} test_simc_tests_budget_t;

/*
 * test_simc_tests_holder is the waiting case's holder: under a hold it retires
 * a record and runs a pass, which keeps it, yields until a process waits at
 * the budget, for at most a thousand yields, and ends its hold.
 */
static bool
test_simc_tests_holder(void *captures, HostIoIo *io, HostError *error)
{
	test_simc_tests_budget_t *budget = captures;
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	uint64		held;

	if (!host.ops->delta_hold(host.self, &raise, &held, error))
		return false;
	host.ops->delta_retire(host.self, &budget->leaving, 1, NULL, 0);
	host.ops->delta_pass(host.self);
	for (int k = 0; k < 1000 && simc_delta_waiting() == 0; k++)
		host.ops->yield_point(host.self);
	budget->seen = simc_delta_waiting();
	budget->before = simc_delta_wakes();
	host.ops->delta_release(host.self, held);
	budget->after = simc_delta_wakes();
	return true;
}

/*
 * test_simc_tests_waiter is the waiting case's waiter: its page is refused by
 * the full delta, and it waits at the budget.
 */
static bool
test_simc_tests_waiter(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	HostDeltaPageSize size = {.bytes = 4 << 10, .words = 4};
	StdVec		names;
	bool		refused;
	uint64		frees;

	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	if (!host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, error))
		return false;
	STD_ASSERT(__LINE__, 5, refused, "the full delta refuses the waiter's page");
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	return host.ops->delta_wait(host.self, &raise, frees, error);
}

/*
 * test_simc_tests_a_waiter_at_the_budget_is_counted_and_woken_by_the_end_of_a_hold_that_kept_a_retirement:
 * a process whose page the full delta refuses is counted as waiting at the
 * budget while it sleeps; the end of a hold that kept a retired record from
 * its free counts a wake and wakes it, after which none waits; and the end of
 * a hold that kept nothing counts none.
 */
static void
test_simc_tests_a_waiter_at_the_budget_is_counted_and_woken_by_the_end_of_a_hold_that_kept_a_retirement(void)
{
	test_simc_tests_budget_t budget = {0};
	SimcTestIo	test_io;
	HostHost	host;
	HostMayRaise raise;
	HostDeltaPageSize size = {.bytes = 4 << 10, .words = 4};
	HostError	error;
	SimcRun		run;
	SimcActor	holder = {test_simc_tests_holder, &budget};
	SimcActor	waiter = {test_simc_tests_waiter};
	SimcReport	report;
	const simc_TupleStringEnding_t *endings;
	StdVec		names;
	bool		refused = false;
	bool		made;
	uint64		frees;
	uint64		place;
	uint64		held;

	simc_size_delta(UINT64CONST(1) << 20);
	test_io = simc_io();
	host = host_current();
	raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	STD_ASSERT(__LINE__, 5, host.ops->delta_make(host.self, &raise, &TEST_SIMC_TESTS_AGED, "aged", 4, &made, &place,
												 &budget.leaving.location, &frees, &error) && made,
			   "an aged record is made");
	host.ops->delta_publish(host.self, place);
	budget.leaving.id = TEST_SIMC_TESTS_AGED.id;
	std_vec_new(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	while (!refused)
	{
		std_vec_reserve_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error),
				   "a page is taken or refused");
	}
	run = simc_Run_new(1);
	simc_Run_actor(&run, "holder", 6, holder);
	simc_Run_actor(&run, "waiter", 6, waiter);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED && report.outcome.u.ended.len == 2,
			   "the run ends");
	endings = report.outcome.u.ended.ptr;
	STD_ASSERT(__LINE__, 5, endings[0].f1.tag == SIMC_ENDING_RETURNED && !endings[0].f1.is_err &&
			   endings[1].f1.tag == SIMC_ENDING_RETURNED && !endings[1].f1.is_err, "both actors return");
	STD_ASSERT(__LINE__, 5, budget.seen == 1, "the holder saw one waiter");
	STD_ASSERT(__LINE__, 5, budget.after == budget.before + 1, "the end of the hold that kept the record counts a wake");
	STD_ASSERT(__LINE__, 5, simc_delta_waiting() == 0, "the woken waiter is no longer counted");
	STD_ASSERT(__LINE__, 5, host.ops->delta_hold(host.self, &raise, &held, &error), "a hold is taken");
	host.ops->delta_release(host.self, held);
	STD_ASSERT(__LINE__, 5, simc_delta_wakes() == budget.after, "the end of a hold that kept nothing counts none");
	simc_Report_drop(&report);
	for (size_t k = 0; k < names.len; k++)
		host.ops->delta_give_back(host.self, STD_VEC_AT(&names, HostDeltaPagePage, k));
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_ones fills a node's bytes with ones. */
static void
test_simc_tests_ones(void *captures, uint8 *bytes, size_t len)
{
	memset(bytes, 1, len);
}

/*
 * test_simc_tests_a_shared_cache_of_one_frame_evicts_a_key_for_the_next:
 * a shared node cache sized to one frame of the smallest class holds a key
 * installed in it until another key's install takes its frame.
 */
static void
test_simc_tests_a_shared_cache_of_one_frame_evicts_a_key_for_the_next(void)
{
	static const size_t counts[HOST_CACHE_CLASSES_LEN] = {1};
	SimcTestIo	test_io = simc_io();
	uint8		address[HOST_CACHE_ADDRESS] = {7};
	uint8		other_address[HOST_CACHE_ADDRESS] = {8};
	uint8		locator[HOST_CACHE_LOCATOR] = {0};
	HostCacheKey key = host_cache_key(1, address, locator);
	HostCacheKey other = host_cache_key(1, other_address, locator);
	HostError	error;
	HostCacheRead read;
	bool		has;

	simc_size_shared(counts);
	STD_ASSERT(__LINE__, 5, host_cache_install(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, 8, test_simc_tests_ones,
											   NULL, NULL, &read, &error) && read.tag == HOST_CACHE_READ_CACHED,
			   "the install fills the one frame");
	host_cache_Read_drop(&read);
	STD_ASSERT(__LINE__, 5, host_cache_contains(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &error) && has,
			   "the installed key is cached");
	STD_ASSERT(__LINE__, 5, host_cache_install(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &other, 8,
											   test_simc_tests_ones, NULL, NULL, &read, &error) &&
			   read.tag == HOST_CACHE_READ_CACHED, "the other key's install takes the frame");
	host_cache_Read_drop(&read);
	STD_ASSERT(__LINE__, 5, host_cache_contains(&test_io.io, HOST_CACHE_PLACEMENT_SERVER, &key, &has, &error) && !has,
			   "the first key is evicted");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one:
 * the running actor's end commits or aborts the transaction its writes were
 * assigned, and its next write is assigned a fresh, later one.
 */
static void
test_simc_tests_an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostStatus	status;
	HostXid		xid;
	HostXid		top;
	HostXid		next;

	STD_ASSERT(__LINE__, 5, host.ops->current_writer(host.self, &raise, &xid, &top, &error) && xid.f0 == top.f0,
			   "a write is assigned a top-level transaction");
	simc_end(true);
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, top, &status, &error) &&
			   status == HOST_STATUS_COMMITTED, "the committed end commits it");
	STD_ASSERT(__LINE__, 5, host.ops->current_writer(host.self, &raise, &next, &top, &error) && next.f0 > xid.f0,
			   "the next write is assigned a later transaction");
	simc_end(false);
	STD_ASSERT(__LINE__, 5, host.ops->xid_status(host.self, &raise, next, &status, &error) &&
			   status == HOST_STATUS_ABORTED, "the aborted end aborts it");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_an_unanswered_thread_is_answered_again_once_out_of_recovery:
 * a thread made to stand for a process in crash recovery isn't answered for
 * every running transaction, and once out of it, is.
 */
static void
test_simc_tests_an_unanswered_thread_is_answered_again_once_out_of_recovery(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();

	STD_ASSERT(__LINE__, 5, host.ops->xids_answered(host.self), "a thread is answered at the start");
	simc_unanswered(true);
	STD_ASSERT(__LINE__, 5, !host.ops->xids_answered(host.self), "a recovering thread isn't answered");
	simc_unanswered(false);
	STD_ASSERT(__LINE__, 5, host.ops->xids_answered(host.self), "a thread out of recovery is answered");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_free_pass_is_counted: each free pass the delta runs
 * counts one more.
 */
static void
test_simc_tests_a_free_pass_is_counted(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	uint64		passes = simc_delta_passes();

	host.ops->delta_pass(host.self);
	STD_ASSERT(__LINE__, 5, simc_delta_passes() == passes + 1, "the pass is counted");
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_FLAG_SET is whether the check case's actor has set its flag. */
static bool test_simc_tests_FLAG_SET;

/* test_simc_tests_flag_check is the check case's check: it fails once the actor has set its flag. */
static bool
test_simc_tests_flag_check(StdString *message)
{
	static const char text[] = "the flag is set";

	if (!test_simc_tests_FLAG_SET)
		return false;
	std_slice_to_vec_Checked(message, (const uint8 *) text, sizeof(text) - 1, 1, 1);
	return true;
}

/* test_simc_tests_flag_setter is the check case's actor: it yields, sets its flag, and yields again. */
static bool
test_simc_tests_flag_setter(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();

	host.ops->yield_point(host.self);
	test_simc_tests_FLAG_SET = true;
	host.ops->yield_point(host.self);
	return true;
}

/*
 * test_simc_tests_a_run_check_fails_the_run_at_a_yield_with_its_message:
 * a run's check passes at each yield point until it fails, and its message
 * fails the run there.
 */
static void
test_simc_tests_a_run_check_fails_the_run_at_a_yield_with_its_message(void)
{
	static const char text[] = "the flag is set";
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(1);
	SimcActor	flagging = {test_simc_tests_flag_setter};
	SimcReport	report;

	test_simc_tests_FLAG_SET = false;
	simc_Run_actor(&run, "flagging", 8, flagging);
	simc_Run_check(&run, test_simc_tests_flag_check);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_FAILED &&
			   report.outcome.u.failed.len == sizeof(text) - 1 &&
			   memcmp(report.outcome.u.failed.ptr, text, sizeof(text) - 1) == 0, "the check's message fails the run");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_panicking_check is the panicking check case's check: it panics with a literal message. */
static bool
test_simc_tests_panicking_check(StdString *message)
{
	static const char text[] = "the check panicked";

	std_panic(RUST_FILE, __LINE__, 5, text, sizeof(text) - 1);
}

/*
 * test_simc_tests_a_run_checks_c_panic_fails_the_run_with_its_message: a C
 * panic in a run's check is caught at the yield point that ran it and raised
 * there as its message alone, which fails the run with the yielding actor's
 * name.
 */
static void
test_simc_tests_a_run_checks_c_panic_fails_the_run_with_its_message(void)
{
	static const char expected[] = "setter panicked: the check panicked";
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(1);
	SimcActor	setter = {test_simc_tests_flag_setter};
	SimcReport	report;

	simc_Run_actor(&run, "setter", 6, setter);
	simc_Run_check(&run, test_simc_tests_panicking_check);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_FAILED &&
			   report.outcome.u.failed.len == sizeof(expected) - 1 &&
			   memcmp(report.outcome.u.failed.ptr, expected, sizeof(expected) - 1) == 0,
			   "the check's panic fails the run with its message");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_test_only_std_fs_makes_a_lone_directory_and_refuses_one_that_stands:
 * the test-only std's create_dir makes a directory whose parent stands, and
 * refuses one that stands with the operating system's AlreadyExists.
 */
static void
test_simc_tests_the_test_only_std_fs_makes_a_lone_directory_and_refuses_one_that_stands(void)
{
	SimcTestIo	test_io = simc_io();
	SimcTempDir dir;
	StdIoError	error;
	const char *dir_path;
	size_t		dir_path_len;
	StdPathBuf	lone;
	StdIoErrorData data;

	STD_ASSERT(__LINE__, 5, simc_tempdir(&dir, &error), "the test makes its directory");
	dir_path = simc_TempDir_path(&dir, &dir_path_len);
	std_path_join(dir_path, dir_path_len, "lone", 4, &lone);
	STD_ASSERT(__LINE__, 5, std_fs_create_dir(lone.bytes.ptr, lone.bytes.len, &error), "the directory is made");
	STD_ASSERT(__LINE__, 5, std_path_Path_exists(lone.bytes.ptr, lone.bytes.len), "the directory stands");
	STD_ASSERT(__LINE__, 5, !std_fs_create_dir(lone.bytes.ptr, lone.bytes.len, &error),
			   "a directory that stands isn't made again");
	data = std_io_Error_data(&error);
	STD_ASSERT(__LINE__, 5, data.tag == STD_IO_ERROR_DATA_OS &&
			   std_sys_decode_error_kind(data.u.os) == STD_ERROR_KIND_ALREADY_EXISTS, "the error is AlreadyExists");
	std_vec_free_Checked(&lone.bytes, 1, 1);
	simc_TempDir_drop(&dir);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_checkpoint_delay_doors_answer_as_the_simulations_host:
 * a window holding checkpoints off opens with its pauses' names and closes,
 * finished or not, after which another opens.
 */
static void
test_simc_tests_the_checkpoint_delay_doors_answer_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(54, test_simc_tests_delaying, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_horizon is the horizon case's actor: a NonVacuumable snapshot's removable xids. */
static void
test_simc_tests_horizon(void *captures)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS((HostIoIo *) captures));
	HostXid		horizon = {100};
	HostXid		below = {99};
	SimcTaken	taken = simc_non_vacuumable(horizon);
	HostSnapshot snapshot = simc_Taken_lent(&taken);
	HostError	error;
	bool		removable;

	STD_ASSERT(__LINE__, 5, snapshot.kind == HOST_SNAPSHOT_KIND_NON_VACUUMABLE, "the snapshot is NonVacuumable");
	STD_ASSERT(__LINE__, 5, host.ops->removable_in(host.self, &raise, snapshot.of, below, &removable, &error) &&
			   removable, "an xid below the horizon is removable");
	STD_ASSERT(__LINE__, 5, host.ops->removable_in(host.self, &raise, snapshot.of, horizon, &removable, &error) &&
			   !removable, "the horizon's own xid isn't");
	simc_Taken_drop(&taken);
}

/*
 * test_simc_tests_a_non_vacuumable_snapshot_finds_xids_below_its_horizon_removable:
 * a NonVacuumable snapshot is of its kind, and through the removable_in door
 * it finds an xid below its horizon removable and the horizon's own not.
 */
static void
test_simc_tests_a_non_vacuumable_snapshot_finds_xids_below_its_horizon_removable(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(55, test_simc_tests_horizon, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_logging is the logged case's actor: it inserts a record and reads it back as the WAL took it. */
static void
test_simc_tests_logging(void *captures)
{
	static const uint8 body[] = {1, 2, 3};
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS((HostIoIo *) captures));
	HostError	error;
	HostLsn		end;
	StdVec		logged;
	const simc_TupleU8VecU8Lsn_t *last;

	STD_ASSERT(__LINE__, 5, host.ops->wal_insert(host.self, &raise, HOST_WAL_KIND_CHUNK, body, sizeof(body), &end,
												 &error), "the record is inserted");
	logged = simc_logged();
	STD_ASSERT(__LINE__, 5, logged.len > 0, "the WAL took a record");
	last = &STD_VEC_AT(&logged, simc_TupleU8VecU8Lsn_t, logged.len - 1);
	STD_ASSERT(__LINE__, 5, last->f0 == host_WalKind_value(HOST_WAL_KIND_CHUNK) && last->f1.len == sizeof(body) &&
			   memcmp(last->f1.ptr, body, sizeof(body)) == 0 && last->f2 == end,
			   "the last record logged is the record, its kind, and its end");
	for (size_t i = 0; i < logged.len; i++)
		std_vec_free_Checked(&STD_VEC_AT(&logged, simc_TupleU8VecU8Lsn_t, i).f1, 1, 1);
	std_vec_free_Checked(&logged, sizeof(simc_TupleU8VecU8Lsn_t), _Alignof(simc_TupleU8VecU8Lsn_t));
}

/*
 * test_simc_tests_the_logged_records_are_the_records_the_wal_took: the
 * records simc lists are those the WAL door took, each with its kind, its
 * body, and its end.
 */
static void
test_simc_tests_the_logged_records_are_the_records_the_wal_took(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(56, test_simc_tests_logging, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_delta_page_takes_one_of_the_directorys_free_slots: a page
 * the delta takes holds one of its directory's free slots until it's given
 * back.
 */
static void
test_simc_tests_a_delta_page_takes_one_of_the_directorys_free_slots(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostDeltaPageSize size = {.bytes = 512, .words = 4};
	HostError	error;
	StdVec		names;
	bool		refused;
	uint64		frees;
	size_t		before = simc_free_slots();

	std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	STD_ASSERT(__LINE__, 5, host.ops->delta_alloc(host.self, &raise, &size, 1, &names, &refused, &frees, &error) &&
			   !refused, "the page is taken");
	STD_ASSERT(__LINE__, 5, simc_free_slots() == before - 1, "the page holds one free slot");
	host.ops->delta_give_back(host.self, STD_VEC_AT(&names, HostDeltaPagePage, 0));
	std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	host.ops->delta_pass(host.self);
	simc_TestIo_drop(&test_io);
}

/* test_simc_tests_new_ciding is the new cid case's actor: it logs a row's command ids and reads the record back. */
static void
test_simc_tests_new_ciding(void *captures)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS((HostIoIo *) captures));
	HostStorage storage = {HOST_PG_DEFAULT, 9301, 16384};
	HostRow		row = {UINT64CONST(1) << 16 | 2};
	HostNewCid	cid = {.has_cmin = true, .cmin = {3}, .has_combo = true, .combo = {5}};
	HostError	error;
	HostLsn		end;
	StdVec		cids;
	const simc_TupleStorageRowNewCidLsn_t *last;

	STD_ASSERT(__LINE__, 5, host.ops->wal_new_cid(host.self, &raise, &storage, row, cid, &end, &error),
			   "the new cid record is logged");
	cids = simc_new_cids();
	STD_ASSERT(__LINE__, 5, cids.len > 0, "the WAL took a new cid record");
	last = &STD_VEC_AT(&cids, simc_TupleStorageRowNewCidLsn_t, cids.len - 1);
	STD_ASSERT(__LINE__, 5, last->f0.database == 9301 && last->f0.relation == 16384 && last->f1.f0 == row.f0 &&
			   last->f2.has_cmin && last->f2.cmin.f0 == 3 && !last->f2.has_cmax && last->f2.has_combo &&
			   last->f2.combo.f0 == 5 && last->f3 == end, "the last record is the row's, its cids, and its end");
	std_vec_free_Checked(&cids, sizeof(simc_TupleStorageRowNewCidLsn_t), _Alignof(simc_TupleStorageRowNewCidLsn_t));
}

/*
 * test_simc_tests_the_new_cid_door_logs_its_record_as_the_simulations_host:
 * the wal_new_cid door logs a row's command ids with its storage and row,
 * which simc lists with the record's end.
 */
static void
test_simc_tests_the_new_cid_door_logs_its_record_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();

	simc_as_actor(57, test_simc_tests_new_ciding, &test_io.io);
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_wal_flush_door_answers_as_the_simulations_host: a
 * flush of the WAL up to a record's end answers, as the simulation's host
 * flushes it.
 */
static void
test_simc_tests_the_wal_flush_door_answers_as_the_simulations_host(void)
{
	static const uint8 body[] = {1, 2, 3};
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	HostLsn		end;

	STD_ASSERT(__LINE__, 5, host.ops->wal_insert(host.self, &raise, HOST_WAL_KIND_CHUNK, body, sizeof(body), &end,
												 &error), "the record is inserted");
	STD_ASSERT(__LINE__, 5, host.ops->wal_flush(host.self, &raise, end, &error), "the WAL is flushed to its end");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_the_injected_door_answers_as_the_simulations_host: the
 * simulation attaches no injection point, so the injected door answers false
 * for a point's name.
 */
static void
test_simc_tests_the_injected_door_answers_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostError	error;
	bool		injected = true;

	STD_ASSERT(__LINE__, 5, host.ops->injected(host.self, &raise, "doltgres-vacuum-found", &injected, &error) &&
			   !injected, "the simulation attaches no injection point");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_a_hold_of_the_test_process_keeps_its_data_directory_past_its_capability:
 * a hold of the test process taken before a test's capability keeps the
 * test's data directory once the capability drops, and its release ends the
 * test.
 */
static void
test_simc_tests_a_hold_of_the_test_process_keeps_its_data_directory_past_its_capability(void)
{
	SimcProcess process = simc_process();
	SimcTestIo	test_io = simc_io();
	StdPathBuf	before = simc_data_dir();
	StdPathBuf	after;

	simc_TestIo_drop(&test_io);
	after = simc_data_dir();
	STD_ASSERT(__LINE__, 5, before.bytes.len == after.bytes.len &&
			   memcmp(before.bytes.ptr, after.bytes.ptr, before.bytes.len) == 0,
			   "the data directory outlives the capability");
	std_vec_free_Checked(&after.bytes, 1, 1);
	std_vec_free_Checked(&before.bytes, 1, 1);
	simc_Process_drop(&process);
}

/*
 * test_simc_tests_a_pools_index_answers_as_the_simulations_host: a pool's
 * index answers nothing for a key never put, the location a put gives it,
 * the newer location a second put gives it, keeps it when a remove names
 * another location, and answers nothing once a remove names its own (bugs
 * row 405).
 */
static void
test_simc_tests_a_pools_index_answers_as_the_simulations_host(void)
{
	SimcTestIo	test_io = simc_io();
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(&test_io.io));
	HostSharedKind kind = TEST_SIMC_TESTS_POOLED;
	static const uint8 key[4] = "key!";
	HostError	error;
	bool		has;
	uint64		location;

	kind.index = sizeof(key);
	STD_ASSERT(__LINE__, 5, host.ops->shared_index_find(host.self, &raise, &kind, key, &has, &location, &error) && !has,
			   "an index holds nothing for a key never put");
	STD_ASSERT(__LINE__, 5, host.ops->shared_index_put(host.self, &raise, &kind, key, 7, &error) &&
			   host.ops->shared_index_find(host.self, &raise, &kind, key, &has, &location, &error) && has &&
			   location == 7, "an index holds the location a put gives a key");
	STD_ASSERT(__LINE__, 5, host.ops->shared_index_put(host.self, &raise, &kind, key, 9, &error) &&
			   host.ops->shared_index_find(host.self, &raise, &kind, key, &has, &location, &error) && has &&
			   location == 9, "a second put replaces the location");
	STD_ASSERT(__LINE__, 5, host.ops->shared_index_remove(host.self, &raise, &kind, key, 7, &error) &&
			   host.ops->shared_index_find(host.self, &raise, &kind, key, &has, &location, &error) && has &&
			   location == 9, "a remove that names another location keeps the key");
	STD_ASSERT(__LINE__, 5, host.ops->shared_index_remove(host.self, &raise, &kind, key, 9, &error) &&
			   host.ops->shared_index_find(host.self, &raise, &kind, key, &has, &location, &error) && !has,
			   "a remove that names the key's location takes it out");
	simc_TestIo_drop(&test_io);
}

/*
 * test_simc_tests_trying_actor is the flag try case's body: a record made,
 * its flag tried, tried again by the process that holds it, cleared, and
 * tried once more.
 */
static bool
test_simc_tests_trying_actor(void *captures, HostIoIo *io, HostError *error)
{
	HostHost	host = host_current();
	HostMayRaise raise = host_doors(HOST_IO_IO_READ_DOORS(io));
	uint64		place;
	bool		made;
	bool		taken;
	HostError	refused;

	(void) captures;
	if (!host.ops->shared_make(host.self, &raise, &TEST_SIMC_TESTS_FLAGGED, &made, &place, error))
		return false;
	STD_ASSERT(__LINE__, 5, made, "the pool has a free record");
	if (!host.ops->flag_try(host.self, &raise, place, 0, &taken, error))
		return false;
	STD_ASSERT(__LINE__, 5, taken, "a try takes a free flag");
	STD_ASSERT(__LINE__, 5, !host.ops->flag_try(host.self, &raise, place, 0, &taken, &refused),
			   "a try of a flag the process holds is refused");
	host_Error_drop(&refused);
	host.ops->flag_clear(host.self, place, 0);
	if (!host.ops->flag_try(host.self, &raise, place, 0, &taken, error))
		return false;
	STD_ASSERT(__LINE__, 5, taken, "a try takes the flag again once it is cleared");
	host.ops->flag_clear(host.self, place, 0);
	return true;
}

/*
 * test_simc_tests_a_flag_try_takes_a_free_flag_and_refuses_its_own: a try
 * takes a record's free flag, refuses the flag its own process holds, as a
 * take does, and takes it again once it is cleared (bugs row 451).
 */
static void
test_simc_tests_a_flag_try_takes_a_free_flag_and_refuses_its_own(void)
{
	SimcTestIo	test_io = simc_io();
	SimcRun		run = simc_Run_new(13);
	SimcActor	trying = {test_simc_tests_trying_actor, NULL};
	SimcReport	report;

	simc_Run_actor(&run, "trier", 5, trying);
	report = simc_Run_finish(run);
	STD_ASSERT(__LINE__, 5, report.outcome.tag == SIMC_OUTCOME_ENDED, "the run ends");
	simc_Report_drop(&report);
	simc_TestIo_drop(&test_io);
}

static const CportTest CPORT_TESTS[] = {
	{"simc::tests::a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory",
	test_simc_tests_a_body_as_an_actor_runs_with_that_actors_id_in_the_data_directory},
	{"simc::tests::a_c_fill_after_a_yield_runs_under_its_own_actors_backend_state",
	test_simc_tests_a_c_fill_after_a_yield_runs_under_its_own_actors_backend_state},
	{"simc::tests::a_c_fills_panic_unwinds_the_simulations_install_with_its_payload",
	test_simc_tests_a_c_fills_panic_unwinds_the_simulations_install_with_its_payload},
	{"simc::tests::a_c_releases_panic_unwinds_the_owners_abort_with_its_payload",
	test_simc_tests_a_c_releases_panic_unwinds_the_owners_abort_with_its_payload},
	{"simc::tests::a_checkpoint_sets_the_redo_point_the_host_answers",
	test_simc_tests_a_checkpoint_sets_the_redo_point_the_host_answers},
	{"simc::tests::a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs",
	test_simc_tests_a_crash_fails_each_call_until_a_recovery_takes_the_files_back_to_their_syncs},
	{"simc::tests::a_delta_page_is_taken_written_and_given_back_as_the_simulations_host",
	test_simc_tests_a_delta_page_is_taken_written_and_given_back_as_the_simulations_host},
	{"simc::tests::a_delta_page_takes_one_of_the_directorys_free_slots",
	test_simc_tests_a_delta_page_takes_one_of_the_directorys_free_slots},
	{"simc::tests::a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c",
	test_simc_tests_a_dictionary_simc_prepares_compresses_and_decompresses_and_drops_in_the_c},
	{"simc::tests::a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory",
	test_simc_tests_a_failed_allocation_of_a_listings_names_frees_the_names_made_and_raises_out_of_memory},
	{"simc::tests::a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory",
	test_simc_tests_a_failed_allocation_of_a_panics_payload_frees_its_message_and_raises_out_of_memory},
	{"simc::tests::a_failed_std_fs_call_returns_the_operating_systems_error",
	test_simc_tests_a_failed_std_fs_call_returns_the_operating_systems_error},
	{"simc::tests::a_free_pass_is_counted",
	test_simc_tests_a_free_pass_is_counted},
	{"simc::tests::a_hinted_c_error_crosses_to_sim_as_its_literals",
	test_simc_tests_a_hinted_c_error_crosses_to_sim_as_its_literals},
	{"simc::tests::a_historic_snapshot_lends_its_bounds_command_and_sorted_xids",
	test_simc_tests_a_historic_snapshot_lends_its_bounds_command_and_sorted_xids},
	{"simc::tests::a_hold_of_the_test_process_keeps_its_data_directory_past_its_capability",
	test_simc_tests_a_hold_of_the_test_process_keeps_its_data_directory_past_its_capability},
	{"simc::tests::a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host",
	test_simc_tests_a_linked_delta_page_opens_reads_appends_seals_and_retires_as_the_simulations_host},
	{"simc::tests::a_nap_is_counted_and_the_disk_space_set_is_answered",
	test_simc_tests_a_nap_is_counted_and_the_disk_space_set_is_answered},
	{"simc::tests::a_non_vacuumable_snapshot_finds_xids_below_its_horizon_removable",
	test_simc_tests_a_non_vacuumable_snapshot_finds_xids_below_its_horizon_removable},
	{"simc::tests::a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host",
	test_simc_tests_a_pause_a_warning_a_log_and_the_last_redo_answer_as_the_simulations_host},
	{"simc::tests::a_pooled_records_doors_answer_as_the_simulations_host",
	test_simc_tests_a_pooled_records_doors_answer_as_the_simulations_host},
	{"simc::tests::a_processs_flags_are_cleared_before_each_exit_work",
	test_simc_tests_a_processs_flags_are_cleared_before_each_exit_work},
	{"simc::tests::a_processs_words_answer_as_the_simulations_host",
	test_simc_tests_a_processs_words_answer_as_the_simulations_host},
	{"simc::tests::a_records_slots_answer_as_the_simulations_host",
	test_simc_tests_a_records_slots_answer_as_the_simulations_host},
	{"simc::tests::a_refused_open_and_a_refused_record_fail_once_with_the_refusal",
	test_simc_tests_a_refused_open_and_a_refused_record_fail_once_with_the_refusal},
	{"simc::tests::a_refused_take_fails_once_and_refuse_nothing_takes_back_the_rest",
	test_simc_tests_a_refused_take_fails_once_and_refuse_nothing_takes_back_the_rest},
	{"simc::tests::a_reports_debug_is_the_rusts",
	test_simc_tests_a_reports_debug_is_the_rusts},
	{"simc::tests::a_reports_trace_is_the_rusts",
	test_simc_tests_a_reports_trace_is_the_rusts},
	{"simc::tests::a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts",
	test_simc_tests_a_returning_actor_ends_ok_and_its_endings_compare_as_the_rusts},
	{"simc::tests::a_run_check_fails_the_run_at_a_yield_with_its_message",
	test_simc_tests_a_run_check_fails_the_run_at_a_yield_with_its_message},
	{"simc::tests::a_run_checks_c_panic_fails_the_run_with_its_message",
	test_simc_tests_a_run_checks_c_panic_fails_the_run_with_its_message},
	{"simc::tests::a_run_dropped_unfinished_drops_its_actors_bodies",
	test_simc_tests_a_run_dropped_unfinished_drops_its_actors_bodies},
	{"simc::tests::a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message",
	test_simc_tests_a_rust_panic_in_a_host_call_unwinds_the_c_with_its_message},
	{"simc::tests::a_scripted_run_runs_each_actor_until_its_point_and_terminates_one",
	test_simc_tests_a_scripted_run_runs_each_actor_until_its_point_and_terminates_one},
	{"simc::tests::a_shared_cache_of_one_frame_evicts_a_key_for_the_next",
	test_simc_tests_a_shared_cache_of_one_frame_evicts_a_key_for_the_next},
	{"simc::tests::a_simulation_counts_traces_and_fills_its_calls",
	test_simc_tests_a_simulation_counts_traces_and_fills_its_calls},
	{"simc::tests::a_sized_delta_answers_its_budget_and_counts_each_hold_begun",
	test_simc_tests_a_sized_delta_answers_its_budget_and_counts_each_hold_begun},
	{"simc::tests::a_snapshot_takes_the_simulations_running_xids",
	test_simc_tests_a_snapshot_takes_the_simulations_running_xids},
	{"simc::tests::a_subtransaction_runs_until_its_abort_under_its_running_parent",
	test_simc_tests_a_subtransaction_runs_until_its_abort_under_its_running_parent},
	{"simc::tests::a_taken_page_and_a_made_record_count_as_unfreed_until_freed",
	test_simc_tests_a_taken_page_and_a_made_record_count_as_unfreed_until_freed},
	{"simc::tests::a_test_directory_the_operating_system_cant_make_fails_with_its_error",
	test_simc_tests_a_test_directory_the_operating_system_cant_make_fails_with_its_error},
	{"simc::tests::a_transactions_status_answers_as_the_simulations_host",
	test_simc_tests_a_transactions_status_answers_as_the_simulations_host},
	{"simc::tests::a_waiter_at_the_budget_is_counted_and_woken_by_the_end_of_a_hold_that_kept_a_retirement",
	test_simc_tests_a_waiter_at_the_budget_is_counted_and_woken_by_the_end_of_a_hold_that_kept_a_retirement},
	{"simc::tests::a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them",
	test_simc_tests_a_warning_a_log_and_a_kept_wal_read_are_taken_as_the_simulation_took_them},
	{"simc::tests::an_actors_c_panic_fails_its_run_with_its_message",
	test_simc_tests_an_actors_c_panic_fails_its_run_with_its_message},
	{"simc::tests::an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one",
	test_simc_tests_an_actors_end_commits_or_aborts_its_transaction_and_starts_a_fresh_one},
	{"simc::tests::an_aged_record_unmade_is_given_back_and_a_replacing_page_is_taken",
	test_simc_tests_an_aged_record_unmade_is_given_back_and_a_replacing_page_is_taken},
	{"simc::tests::an_exit_work_listed_once_the_exit_began_is_refused",
	test_simc_tests_an_exit_work_listed_once_the_exit_began_is_refused},
	{"simc::tests::an_exit_work_runs_once_at_its_process_exit",
	test_simc_tests_an_exit_work_runs_once_at_its_process_exit},
	{"simc::tests::an_open_scratch_file_is_counted_until_it_closes",
	test_simc_tests_an_open_scratch_file_is_counted_until_it_closes},
	{"simc::tests::an_owned_append_refused_after_a_count_fails_once",
	test_simc_tests_an_owned_append_refused_after_a_count_fails_once},
	{"simc::tests::an_owned_record_is_released_by_its_owners_abort_unless_forgotten",
	test_simc_tests_an_owned_record_is_released_by_its_owners_abort_unless_forgotten},
	{"simc::tests::an_unanswered_thread_is_answered_again_once_out_of_recovery",
	test_simc_tests_an_unanswered_thread_is_answered_again_once_out_of_recovery},
	{"simc::tests::an_unhinted_c_error_crosses_to_sim_as_its_message",
	test_simc_tests_an_unhinted_c_error_crosses_to_sim_as_its_message},
	{"simc::tests::each_failing_exit_work_is_in_the_report",
	test_simc_tests_each_failing_exit_work_is_in_the_report},
	{"simc::tests::each_served_entry_answers_as_the_simulations_host",
	test_simc_tests_each_served_entry_answers_as_the_simulations_host},
	{"simc::tests::exit_works_run_newest_first",
	test_simc_tests_exit_works_run_newest_first},
	{"simc::tests::the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host",
	test_simc_tests_the_backup_collection_and_database_lock_doors_answer_as_the_simulations_host},
	{"simc::tests::the_btree_compare_door_answers_as_the_simulations_host",
	test_simc_tests_the_btree_compare_door_answers_as_the_simulations_host},
	{"simc::tests::the_checkpoint_delay_doors_answer_as_the_simulations_host",
	test_simc_tests_the_checkpoint_delay_doors_answer_as_the_simulations_host},
	{"simc::tests::the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host",
	test_simc_tests_the_delta_advance_seed_and_pass_doors_answer_as_the_simulations_host},
	{"simc::tests::the_delta_hold_and_budget_doors_answer_as_the_simulations_host",
	test_simc_tests_the_delta_hold_and_budget_doors_answer_as_the_simulations_host},
	{"simc::tests::the_epoch_doors_answer_as_the_simulations_host",
	test_simc_tests_the_epoch_doors_answer_as_the_simulations_host},
	{"simc::tests::the_injected_door_answers_as_the_simulations_host",
	test_simc_tests_the_injected_door_answers_as_the_simulations_host},
	{"simc::tests::the_key_doors_answer_as_the_simulations_host",
	test_simc_tests_the_key_doors_answer_as_the_simulations_host},
	{"simc::tests::the_logged_records_are_the_records_the_wal_took",
	test_simc_tests_the_logged_records_are_the_records_the_wal_took},
	{"simc::tests::the_new_cid_door_logs_its_record_as_the_simulations_host",
	test_simc_tests_the_new_cid_door_logs_its_record_as_the_simulations_host},
	{"simc::tests::the_next_decompression_runs_out_of_memory_when_asked",
	test_simc_tests_the_next_decompression_runs_out_of_memory_when_asked},
	{"simc::tests::the_node_cache_installs_finds_and_unpins_a_nodes_frame",
	test_simc_tests_the_node_cache_installs_finds_and_unpins_a_nodes_frame},
	{"simc::tests::the_test_only_std_fs_lists_a_directorys_names",
	test_simc_tests_the_test_only_std_fs_lists_a_directorys_names},
	{"simc::tests::the_test_only_std_fs_makes_a_directory_with_its_parents",
	test_simc_tests_the_test_only_std_fs_makes_a_directory_with_its_parents},
	{"simc::tests::the_test_only_std_fs_makes_a_lone_directory_and_refuses_one_that_stands",
	test_simc_tests_the_test_only_std_fs_makes_a_lone_directory_and_refuses_one_that_stands},
	{"simc::tests::the_test_only_std_fs_removes_a_directory_with_its_contents",
	test_simc_tests_the_test_only_std_fs_removes_a_directory_with_its_contents},
	{"simc::tests::the_test_only_std_fs_writes_reads_appends_and_removes_a_file",
	test_simc_tests_the_test_only_std_fs_writes_reads_appends_and_removes_a_file},
	{"simc::tests::the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host",
	test_simc_tests_the_wal_a_checkpoint_and_a_databases_quiet_answer_as_the_simulations_host},
	{"simc::tests::the_wal_flush_door_answers_as_the_simulations_host",
	test_simc_tests_the_wal_flush_door_answers_as_the_simulations_host},
	{"simc::tests::a_pools_index_answers_as_the_simulations_host",
	test_simc_tests_a_pools_index_answers_as_the_simulations_host},
	{"simc::tests::a_flag_try_takes_a_free_flag_and_refuses_its_own",
	test_simc_tests_a_flag_try_takes_a_free_flag_and_refuses_its_own},
};

PG_FUNCTION_INFO_V1(doltgres_cport_test_simc);

/* doltgres_cport_test_simc runs simc's tests and returns each one's name and outcome. */
Datum
doltgres_cport_test_simc(PG_FUNCTION_ARGS)
{
	return doltgres_cport_test_run_all(fcinfo, CPORT_TESTS, lengthof(CPORT_TESTS));
}
