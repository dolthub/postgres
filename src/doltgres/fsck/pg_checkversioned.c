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
 * pg_checkversioned, the fork's check of a stopped cluster's versioned storage
 * (design item D12): it reads every repository's packs, list files, and
 * manifests as a read reads them, through the frontend library's one entry,
 * prints what doesn't verify, and reads the WAL ranges a fallback would replay.
 * It exits 0 when it finds nothing, 2 when it finds something, and 1 when it
 * can't run, as pg_amcheck does.
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "common/controldata_utils.h"
#include "common/logging.h"
#include "fe_utils/version.h"
#include "getopt_long.h"

#include "fe_calls.h"

/* KIND_FOUND, KIND_SETTLED, and KIND_NOTE are the kinds of line the check reports. */
#define KIND_FOUND 0
#define KIND_SETTLED 1
#define KIND_NOTE 2

/* KeptRange is a WAL range a repository's fallback would replay, as the check handed it. */
typedef struct KeptRange
{
	uint32		repository;
	XLogRecPtr	from;
	bool		oldest;
	bool		found;
} KeptRange;

/* KeptReader is the private state of the WAL reader of the kept ranges: the segment it found missing. */
typedef struct KeptReader
{
	char		missing[MAXFNAMELEN];
} KeptReader;

/*
 * The timelines the control file's timeline descends from, oldest first, each
 * with where it begins and ends as its history file gives them; the last is
 * the control file's own, which has no end.
 */
static TimeLineHistoryEntry *history = NULL;
static int	history_count = 0;

static const char *progname;
static bool verbose = false;
static int64 found = 0;
static char *wal_dir;

/* The ranges the check handed, stored with plain malloc under the entry (W3). */
static KeptRange *kept = NULL;
static size_t kept_count = 0;
static size_t kept_room = 0;
static bool kept_failed = false;

/* usage prints the program's help. */
static void
usage(void)
{
	printf(_("%s checks the versioned storage of a stopped PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D, --pgdata=]DATADIR    data directory\n"));
	printf(_("  -v, --verbose            also show what the next start settles, and notes\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -?, --help               show this help, then exit\n"));
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/* report prints a line the check reports: one found always, and the others with --verbose; it never exits (W3). */
static void
report(int kind, const char *text)
{
	if (kind == KIND_FOUND)
		found++;
	if (kind == KIND_FOUND || verbose)
		printf("%s\n", text);
}

/* keep stores a WAL range the check hands, with plain malloc, setting a flag on a failure; it never exits (W3). */
static void
keep(uint32 repository, uint64 from, bool oldest, bool gap_found)
{
	if (kept_count == kept_room)
	{
		size_t		room = kept_room == 0 ? 8 : kept_room * 2;
		KeptRange  *larger = realloc(kept, room * sizeof(KeptRange));

		if (larger == NULL)
		{
			kept_failed = true;
			return;
		}
		kept = larger;
		kept_room = room;
	}
	kept[kept_count].repository = repository;
	kept[kept_count].from = from;
	kept[kept_count].oldest = oldest;
	kept[kept_count].found = gap_found;
	kept_count++;
}

/* segment_path returns the path of a WAL segment in the data directory. */
static void
segment_path(char *path, TimeLineID timeline, XLogSegNo segno, int segsize)
{
	char		name[MAXFNAMELEN];

	XLogFileName(name, timeline, segno, segsize);
	snprintf(path, MAXPGPATH, "%s/%s", wal_dir, name);
}

/*
 * read_history reads the history file of a timeline, as the server's
 * readTimeLineHistory does, into the timelines it descends from.
 */
static void
read_history(TimeLineID current)
{
	XLogRecPtr	begin = InvalidXLogRecPtr;
	int			room = 8;

	history = pg_malloc(room * sizeof(TimeLineHistoryEntry));
	if (current > 1)
	{
		char		path[MAXPGPATH];
		char		line[MAXPGPATH];
		FILE	   *fd;

		snprintf(path, sizeof(path), "%s/%08X.history", wal_dir, current);
		fd = fopen(path, "r");
		if (fd == NULL)
			pg_fatal("could not open file \"%s\": %m", path);
		while (fgets(line, sizeof(line), fd) != NULL)
		{
			TimeLineID	tli;
			uint32		hi;
			uint32		lo;

			if (sscanf(line, "%u\t%X/%X", &tli, &hi, &lo) != 3)
				continue;
			if (history_count + 1 == room)
				history = pg_realloc(history, (room *= 2) * sizeof(TimeLineHistoryEntry));
			history[history_count].tli = tli;
			history[history_count].begin = begin;
			history[history_count].end = ((uint64) hi) << 32 | lo;
			begin = history[history_count++].end;
		}
		fclose(fd);
	}
	history[history_count].tli = current;
	history[history_count].begin = begin;
	history[history_count++].end = InvalidXLogRecPtr;
}

/*
 * segment_timeline returns the timeline a WAL segment is read from: the one
 * active at the segment's end, whose copy of a switch point's segment holds
 * the WAL before the switch too, as the server's XLogReadDetermineTimeline
 * reads it.
 */
static TimeLineID
segment_timeline(XLogSegNo segno, int segsize)
{
	XLogRecPtr	end;
	int			i = 0;

	XLogSegNoOffsetToRecPtr(segno + 1, 0, segsize, end);
	while (i + 1 < history_count && end - 1 >= history[i].end)
		i++;
	return history[i].tli;
}

/* kept_segment_open opens a WAL segment the page reader found there. */
static void
kept_segment_open(XLogReaderState *state, XLogSegNo nextSegNo, TimeLineID *tli_p)
{
	char		path[MAXPGPATH];

	segment_path(path, *tli_p, nextSegNo, state->segcxt.ws_segsize);
	state->seg.ws_file = open(path, O_RDONLY | PG_BINARY, 0);
	if (state->seg.ws_file < 0)
		pg_fatal("could not open file \"%s\": %m", path);
}

/* kept_segment_close closes a WAL segment. */
static void
kept_segment_close(XLogReaderState *state)
{
	close(state->seg.ws_file);
	state->seg.ws_file = -1;
}

/*
 * kept_page_read reads a page of the WAL, or ends the read where its segment
 * isn't there, naming it.
 */
static int
kept_page_read(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
			   XLogRecPtr targetPtr, char *readBuff)
{
	KeptReader *private = state->private_data;
	XLogSegNo	segno;
	TimeLineID	tli;
	char		path[MAXPGPATH];
	struct stat st;
	WALReadError errinfo;

	XLByteToSeg(targetPagePtr, segno, state->segcxt.ws_segsize);
	tli = segment_timeline(segno, state->segcxt.ws_segsize);
	segment_path(path, tli, segno, state->segcxt.ws_segsize);
	if (stat(path, &st) == 0 &&
		WALRead(state, readBuff, targetPagePtr, XLOG_BLCKSZ, tli, &errinfo))
		return XLOG_BLCKSZ;
	XLogFileName(private->missing, tli, segno, state->segcxt.ws_segsize);
	return -1;
}

/* oldest_segment returns the first segment pg_wal holds of the history's timelines, or 0 when it holds none. */
static XLogSegNo
oldest_segment(int segsize)
{
	DIR		   *dir = opendir(wal_dir);
	struct dirent *de;
	XLogSegNo	oldest = 0;

	if (dir == NULL)
		pg_fatal("could not open directory \"%s\": %m", wal_dir);
	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		TimeLineID	tli;
		XLogSegNo	segno;

		if (!IsXLogFileName(de->d_name))
			continue;
		XLogFromFileName(de->d_name, &tli, &segno, segsize);
		for (int i = 0; i < history_count; i++)
			if (history[i].tli == tli && (oldest == 0 || segno < oldest))
				oldest = segno;
	}
	if (errno != 0)
		pg_fatal("could not read directory \"%s\": %m", wal_dir);
	closedir(dir);
	return oldest;
}

/*
 * read_kept reads a range's WAL to the control file's redo point, printing a
 * line of its kind where it can't, naming the segment.
 */
static void
read_kept(const KeptRange *range, ControlFileData *control)
{
	int			segsize = control->xlog_seg_size;
	XLogRecPtr	to = control->checkPointCopy.redo;
	KeptReader	private = {0};
	XLogReaderState *reader;
	XLogRecPtr	start = range->from;
	XLogSegNo	segno;
	char		path[MAXPGPATH];
	char		line[MAXPGPATH * 2];
	char	   *errormsg = NULL;
	struct stat st;

	if (start >= to)
		return;
	XLByteToSeg(start, segno, segsize);
	segment_path(path, segment_timeline(segno, segsize), segno, segsize);
	if (range->oldest && stat(path, &st) != 0)
	{
		XLogSegNo	oldest = oldest_segment(segsize);

		snprintf(line, sizeof(line),
				 "database %u: the WAL from %X/%08X isn't kept, so its fallback starts at the oldest WAL the directory holds",
				 range->repository, LSN_FORMAT_ARGS(start));
		report(KIND_NOTE, line);
		if (oldest == 0)
			return;
		XLogSegNoOffsetToRecPtr(oldest, 0, segsize, start);
	}
	reader = XLogReaderAllocate(segsize, wal_dir,
								XL_ROUTINE(.page_read = kept_page_read,
										   .segment_open = kept_segment_open,
										   .segment_close = kept_segment_close),
								&private);
	if (reader == NULL)
		pg_fatal("out of memory while allocating a WAL reading processor");
	start = XLogFindNextRecord(reader, start, &errormsg);
	if (XLogRecPtrIsInvalid(start))
		errormsg = errormsg != NULL ? errormsg : _("no whole record");
	else
	{
		XLogBeginRead(reader, start);
		while (reader->NextRecPtr < to && XLogReadRecord(reader, &errormsg) != NULL)
			;
	}
	if (errormsg != NULL || reader->NextRecPtr < to)
	{
		if (private.missing[0] == '\0')
		{
			XLByteToSeg(reader->NextRecPtr, segno, segsize);
			XLogFileName(private.missing, segment_timeline(segno, segsize), segno, segsize);
		}
		snprintf(line, sizeof(line),
				 "database %u: could not read the WAL kept for a manifest's fallback in segment \"%s\": %s",
				 range->repository, private.missing,
				 errormsg != NULL ? errormsg : _("the WAL ends"));
		report(range->found ? KIND_FOUND : KIND_SETTLED, line);
	}
	XLogReaderFree(reader);
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	char	   *DataDir = NULL;
	int			c;
	int			option_index;
	bool		crc_ok;
	uint32		major_version;
	char	   *version_str;
	ControlFileData *ControlFile;
	char		failure[1024];
	DgBuffer	message = {.data = failure, .len = sizeof(failure)};

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_checkversioned"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_checkversioned (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:v", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				DataDir = optarg;
				break;
			case 'v':
				verbose = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (DataDir == NULL)
	{
		if (optind < argc)
			DataDir = argv[optind++];
		else
			DataDir = getenv("PGDATA");

		/* If no DataDir was specified, and none could be found, error out */
		if (DataDir == NULL)
		{
			pg_log_error("no data directory specified");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
	}

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	major_version = GET_PG_MAJORVERSION_NUM(get_pg_version(DataDir, &version_str));
	if (major_version != PG_MAJORVERSION_NUM)
	{
		pg_log_error("data directory is of wrong version");
		pg_log_error_detail("File \"%s\" contains \"%s\", which is not compatible with this program's version \"%s\".",
							"PG_VERSION", version_str, PG_MAJORVERSION);
		exit(1);
	}

	ControlFile = get_controlfile(DataDir, &crc_ok);
	if (!crc_ok)
		pg_fatal("pg_control CRC value is incorrect");
	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		pg_fatal("cluster is not compatible with this version of pg_checkversioned");

	/* A clean shutdown is required, as pg_checksums requires one. */
	if (ControlFile->state != DB_SHUTDOWNED &&
		ControlFile->state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("cluster must be shut down");

	wal_dir = psprintf("%s/%s", DataDir, XLOGDIR);
	failure[0] = '\0';
	if (!doltgres_fsck(DataDir, report, keep, message))
		pg_fatal("%s", failure);
	if (kept_failed)
		pg_fatal("out of memory");
	read_history(ControlFile->checkPointCopy.ThisTimeLineID);
	for (size_t i = 0; i < kept_count; i++)
		read_kept(&kept[i], ControlFile);
	free(kept);

	exit(found > 0 ? 2 : 0);
}
