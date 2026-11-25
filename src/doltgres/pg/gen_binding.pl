#!/usr/bin/perl
#
# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Generates the binding from pg's lists (design item 17), their one parser and
# the one writer of everything made of them: from calls.list, entries.list,
# settings.list, structs.list, and globals.list the C wrappers that catch what
# a Postgres call raises, the C entry thunks that raise what an entry returns,
# the variables of the fork's settings, the C structs and globals Postgres
# keeps, each call's and setting's function over them, and each entry's
# exported side (calls.h, calls.c, entries.c, pg_calls.h, pg_calls.c,
# export.c); with --testing, test-calls.list's and test-entries.list's join
# them; with --frontend, the frontend build's wrappers, their functions, and
# its entry's exported side from frontend-calls.list and frontend-entries.list
# (fe_calls.h, fe_calls.c, fe_pg_calls.h, fe_pg_calls.c, fe_export.c). meson
# runs it into the build folder.

use strict;
use warnings FATAL => 'all';

use Getopt::Long;

my $testing = 0;
my $frontend = 0;

GetOptions(
	'testing' => \$testing,
	'frontend' => \$frontend)
  or die "$0: wrong arguments\n";
die "usage: $0 [--testing] [--frontend] <pg directory> <output directory>\n"
  unless @ARGV == 2;
my ($dir, $out) = @ARGV;

# rows returns a list's lines that aren't comments or blank, split at tabs.
sub rows
{
	my ($name) = @_;
	my $path = "$dir/$name";
	open(my $fh, '<:raw', $path) or die "cannot read $path: $!\n";
	my @rows;
	while (my $line = <$fh>)
	{
		$line =~ s/\n\z//;
		$line =~ s/\r\z//;
		next if $line =~ /^#/ || $line !~ /\S/;
		push @rows, [ split(/\t/, $line, -1) ];
	}
	close($fh);
	return @rows;
}

# params splits C parameters into types and names.
sub params
{
	my ($text) = @_;
	my @params;
	foreach my $p (split(/,/, $text, -1))
	{
		$p =~ s/^\s+|\s+$//g;
		next if $p eq '' || $p eq 'void';
		my $at = -1;
		for (my $i = length($p) - 1; $i >= 0; $i--)
		{
			my $c = substr($p, $i, 1);
			if ($c eq ' ' || $c eq '*')
			{
				$at = $i;
				last;
			}
		}
		die "no name in parameter $p\n" if $at < 0;
		my $type = substr($p, 0, $at + 1);
		$type =~ s/^\s+|\s+$//g;
		push @params, [ $type, substr($p, $at + 1) ];
	}
	return @params;
}

# mark returns whether a mark is its first word, refusing any other.
sub mark
{
	my ($text, $yes, $no) = @_;
	return 1 if $text eq $yes;
	return 0 if $text eq $no;
	die "mark $text is neither $yes nor $no\n";
}

# has_out returns whether an entry gives a value through an out-argument: it
# returns bool, and its last parameter is a pointer to one of the pointers an
# entry may give.
sub has_out
{
	my ($returns, @params) = @_;
	return 0 unless $returns eq 'bool' && @params;
	my $last = $params[-1][0];
	return
	  scalar grep { $_ eq $last }
	  ('pg_locale_t *', 'const char **', 'char **',
		'const TableAmRoutine **');
}

# statements returns a body's statements, split at each "; " outside braces and
# string literals, one per line at the given indent, each ending with its
# semicolon unless it is a block.
sub statements
{
	my ($body, $indent) = @_;
	my $text = '';
	my ($depth, $start, $quoted) = (0, 0, 0);
	for (my $i = 0; $i < length($body); $i++)
	{
		my $c = substr($body, $i, 1);
		if ($c eq '"' && ($i == 0 || substr($body, $i - 1, 1) ne '\\'))
		{
			$quoted = !$quoted;
		}
		elsif ($quoted)
		{
		}
		elsif ($c eq '{')
		{
			$depth++;
		}
		elsif ($c eq '}')
		{
			$depth--;
		}
		elsif ($c eq ';' && $depth == 0 && substr($body, $i + 1, 1) eq ' ')
		{
			$text .= $indent . substr($body, $start, $i - $start) . ";\n";
			$start = $i + 2;
		}
	}
	my $last = substr($body, $start);
	return $text . $indent . $last . ($last =~ /\}\z/ ? '' : ';') . "\n";
}

# split_star returns a type without a trailing star, and the star, if it has one.
sub split_star
{
	my ($type) = @_;
	if ($type =~ /\*\z/)
	{
		my $base = substr($type, 0, -1);
		$base =~ s/\s+\z//;
		return ($base, '*');
	}
	return ($type, '');
}

# extern_decl returns a function's extern declaration as pgindent lays it out,
# the name at column 12.
sub extern_decl
{
	my ($returns, $name, $params) = @_;
	my ($base, $star) = split_star($returns);
	my $lead = "extern $base";
	my $gap = length($lead) < 11 && $star eq '' ? "\t" : ' ';
	return "$lead$gap$star$name($params);";
}

# c_decl returns a C declaration of a name of a type as pgindent lays it out,
# the name at column 12.
sub c_decl
{
	my ($type, $name) = @_;
	my ($base, $star) = split_star($type);
	my $target = 12 - length($star);
	my $decl = $base;
	my $col = length($base);
	if ($col >= $target)
	{
		$decl .= ' ';
	}
	else
	{
		while ((int($col / 4) + 1) * 4 <= $target)
		{
			$decl .= "\t";
			$col = (int($col / 4) + 1) * 4;
		}
		$decl .= ' ' x ($target - $col);
	}
	return "$decl$star$name";
}

# c_params returns C parameters as a declaration list, with more appended.
sub c_params
{
	my ($params, @more) = @_;
	my @all =
	  map { $_->[0] . ($_->[0] =~ /\*\z/ ? '' : ' ') . $_->[1] } @$params;
	push @all, @more;
	return @all ? join(', ', @all) : 'void';
}

# written writes a generated file into the output directory with Unix line
# ends on every platform.
sub written
{
	my ($name, $text) = @_;
	my $path = "$out/$name";
	open(my $fh, '>:raw', $path) or die "cannot write $path: $!\n";
	print $fh $text;
	close($fh) or die "cannot write $path: $!\n";
}

# capabilities returns the name of the capability set run grants an entry,
# refusing marks item 17 forbids: an entry whose panic becomes a PANIC is one
# Postgres calls while aborting, releasing, or exiting, so it gets neither
# MayRaise nor Catalog, and a catalog mark needs its reason.
sub capabilities
{
	my ($panic, $catalog, $reason) = @_;
	die "an entry whose panic becomes a PANIC gets no catalog access\n"
	  if $panic && $catalog;
	return 'Cleanup' if $panic;
	die "a catalog mark needs its reason\n"
	  if $catalog && $reason !~ /\S/;
	return 'CatalogCaps' if $catalog;
	return 'Caps';
}

# FORM_PARAMS gives a C type of the lists the C form of its parameter: the
# declaration, with N for its name, and the expression that passes it to the
# wrapper.
my %FORM_PARAMS = (
	'bool' => [ 'bool N', 'N' ],
	'int' => [ 'int32 N', 'N' ],
	'int32' => [ 'int32 N', 'N' ],
	'uint8' => [ 'uint8 N', 'N' ],
	'uint16' => [ 'uint16 N', 'N' ],
	'uint32' => [ 'uint32 N', 'N' ],
	'Oid' => [ 'uint32 N', 'N' ],
	'CommandId' => [ 'uint32 N', 'N' ],
	'TransactionId' => [ 'uint32 N', 'N' ],
	'SubTransactionId' => [ 'uint32 N', 'N' ],
	'BlockNumber' => [ 'uint32 N', 'N' ],
	'uint64' => [ 'uint64 N', 'N' ],
	'int64' => [ 'int64 N', 'N' ],
	'size_t' => [ 'size_t N', 'N' ],
	'Size' => [ 'size_t N', 'N' ],
	'double' => [ 'float8 N', 'N' ],
	'char' => [ 'char N', 'N' ],
	'ScanDirection' => [ 'int32 N', 'N' ],
	'XactEvent' => [ 'int32 N', 'N' ],
	'SubXactEvent' => [ 'int32 N', 'N' ],
	'ForkNumber' => [ 'int32 N', 'N' ],
	'IndexUniqueCheck' => [ 'int32 N', 'N' ],
	'TM_Result' => [ 'int32 N', 'N' ],
	'LockTupleMode' => [ 'int32 N', 'N' ],
	'LockWaitPolicy' => [ 'int32 N', 'N' ],
	'MemoryContext' => [ 'const PgContext *N', '(MemoryContext) N' ],
	'DgIcuLocale' => [ 'PgIcuLocale *N', 'N->f0' ],
	'DgBuiltinLocale' => [ 'PgBuiltinLocale *N', 'N->f0' ],
	'DgLocale' => [ 'PgLocale *N', 'N->f0' ],
	'DgLibcLocale' => [ 'PgLibcLocale N', 'N.f0' ],
	'char *' => [ 'PgPgString *N', 'N->f0' ],
	'Datum' => [ 'PgDatum N', 'N.f0' ],
	'HeapTuple' => [ 'PgHeapTuple N', 'N.f0' ],
	'MinimalTuple' => [ 'PgMinimalTuple N', 'N.f0' ],
	'TupleTableSlot *' => [ 'PgSlot *N', '(TupleTableSlot *) N' ],
	'TupleTableSlot **' => [ 'TupleTableSlot **N', 'N' ],
	'struct BulkInsertStateData *' =>
	  [ 'const PgBulkInsert *N', '(struct BulkInsertStateData *) N' ],
	'const VacuumParams *' =>
	  [ 'const PgVacuumParams *N', '(const VacuumParams *) N' ],
	'TidStore *' => [ 'const PgDeadItems *N', 'N->f0' ],
	'DgRewrite *' => [ 'const PgRewrite *N', 'N->f0' ],
	'IndexBulkDeleteResult *' => [
		'PgIndexStatsOption *N',
		'(N->has ? pg_IndexStats_into_raw(&N->stats) : NULL)'
	],
	'BufferAccessStrategy' =>
	  [ 'const PgStrategy *N', '(BufferAccessStrategy) N' ],
	'const TupleTableSlot *' =>
	  [ 'const PgSlot *N', '(const TupleTableSlot *) N' ],
	'Relation' => [ 'const PgRel *N', '(Relation) N' ],
	'text *' => [ 'const PgText *N', '(text *) N' ],
	'const TableAmRoutine *' => [ 'PgTableRoutine *N', 'N->f0' ],
	'const TupleTableSlotOps *' => [ 'PgSlotOps *N', 'N->f0' ],
	'bool *' => [ 'PgIsNull *N', '(bool *) N' ],
	'const bool *' => [ 'const PgIsNull *N', '(const bool *) N' ],
	'LWLockMode' => [ 'LWLockMode N', 'N' ],
	'AlterTableType' => [ 'AlterTableType N', 'N' ],
	'AlterTableCmd *' => [ 'const PgAlterCmd *N', '(AlterTableCmd *) N' ],
	'const AlterTableCmd *' =>
	  [ 'const PgAlterCmd *N', '(const AlterTableCmd *) N' ],
	'const char *' => [ 'const char *N', 'N' ],
	'DgBytes' => [
		'const uint8 *N, size_t N_len',
		'(DgBytes) {(const char *) N, N_len}'
	],
	'DgBuffer' =>
	  [ 'uint8 *N, size_t N_len', '(DgBuffer) {(char *) N, N_len}' ],
	'DgStored' => [ 'DgStored N', 'N' ],
	'DgParallelBase' => [ 'DgParallelBase N', 'N' ],
	'ParallelTableScanDescData *' =>
	  [ 'const PgParallelScan *N', '(ParallelTableScanDescData *) N' ],
	'DgScanKeys' => [ 'DgScanKeys N', 'N' ],
	'DgOwned' => [ 'DgOwned N', 'N' ],
	'DgCompared' => [ 'DgCompared N', 'N' ],
	'DgStorage' => [ 'DgStorage N', 'N' ],
	'DgSnapshotView' => [ 'DgSnapshotView N', 'N' ],
	'DgBitmapPage' => [ 'DgBitmapPage N', 'N' ],
	'DgCount *' => [ 'PgCounter *N', '(DgCount *) N' ],
	'DgKeyColumn *' => [ 'DgKeyColumn *N', 'N' ],
	'DgIndexColumn *' => [ 'DgIndexColumn *N', 'N' ],
	'DgClass' => [ 'DgClass N', 'N' ],
	'TableScanDesc' => [ 'PgScan *N', '(TableScanDesc) N' ],
	'const TableScanDesc' => [ 'const PgScan *N', '(TableScanDesc) N' ],
	'const BackgroundWorker *' =>
	  [ 'const PgBackgroundWorker *N', '(const BackgroundWorker *) N' ],
	'IndexScanDesc' => [ 'PgIndexScan *N', '(IndexScanDesc) N' ],
	'const IndexScanDesc' => [ 'const PgIndexScan *N', '(IndexScanDesc) N' ],
	'TIDBitmap *' => [ 'PgBitmap *N', '(TIDBitmap *) N' ],
	'struct PlannerInfo *' => [ 'PgPlanner *N', '(struct PlannerInfo *) N' ],
	'struct IndexPath *' => [ 'PgIndexPath *N', '(struct IndexPath *) N' ],
	'Cost *' => [ 'float8 *N', 'N' ],
	'Selectivity *' => [ 'float8 *N', 'N' ],
	'Buffer *' => [ 'int32 *N', 'N' ],
	'DgIndexKey' => [ 'DgIndexKey N', 'N' ],
	'DgIndexArray' => [ 'DgIndexArray N', 'N' ],
	'DgPreprocessed' => [ 'DgPreprocessed N', 'N' ],
	'DgSkipStep' => [ 'DgSkipStep N', 'N' ],
	'struct IndexFetchTableData *' =>
	  [ 'PgIndexFetch *N', '(struct IndexFetchTableData *) N' ],
	'struct SampleScanState *' =>
	  [ 'PgSampleState *N', '(struct SampleScanState *) N' ],
	'ReadStream *' => [ 'PgStream *N', '(ReadStream *) N' ],
	'const struct IndexFetchTableData *' =>
	  [ 'const PgIndexFetch *N', '(const struct IndexFetchTableData *) N' ],
	'Snapshot' => [ 'const PgSnapshot *N', '(Snapshot) N' ],
	'ScanKey' => [ 'const PgScanKeys *N', '(ScanKey) N' ],
	'ParallelTableScanDesc' =>
	  [ 'const PgParallelScan *N', '(ParallelTableScanDesc) N' ],
	'BlockNumber *' => [ 'uint32 *N', 'N' ],
	'TransactionId *' => [ 'uint32 *N', 'N' ],
	'MultiXactId *' => [ 'uint32 *N', 'N' ],
	'const RelFileLocator *' =>
	  [ 'const PgFileLocator *N', '(const RelFileLocator *) N' ],
	'double *' => [ 'float8 *N', 'N' ],
	'int32 *' => [ 'PgWidths N', 'N.f0' ],
	'ItemPointer' => [ 'PgTid *N', '(ItemPointer) N' ],
	'const ItemPointerData *' =>
	  [ 'const PgTid *N', '(const ItemPointerData *) N' ],
	'TM_FailureData *' => [ 'PgFailure *N', '(TM_FailureData *) N' ],
	'TM_IndexDeleteOp *' => [ 'PgIndexDeleteOp *N', '(TM_IndexDeleteOp *) N' ],
	'const TM_IndexDeleteOp *' =>
	  [ 'const PgIndexDeleteOp *N', '(const TM_IndexDeleteOp *) N' ],
	'Datum *' => [ 'const PgDatums *N', '(Datum *) N' ],
	'DoltgresBuild *' => [ 'PgBuild *N', '(DoltgresBuild *) N' ],
	'DoltgresSort *' => [ 'PgSort *N', '(DoltgresSort *) N' ],
	'IndexBuildResult *' => [ 'PgBuildResult N', 'N.f0' ],
	'IndexVacuumInfo *' => [ 'PgVacuumInfo *N', '(IndexVacuumInfo *) N' ],
	'const IndexVacuumInfo *' =>
	  [ 'const PgVacuumInfo *N', '(const IndexVacuumInfo *) N' ],
	'IndexBulkDeleteCallback' => [ 'PgBulkCallback N', 'N.f0' ],
	'IndexBuildCallback' => [ 'PgBuildCallback N', 'N.f0' ],
	'DgBuildState' => [ 'PgBuildState N', 'N.f0' ],
	'DgParallelTarget' => [ 'PgParallelTarget *N', 'N->f0' ],
	'struct IndexInfo *' => [ 'PgIndexInfo *N', '(struct IndexInfo *) N' ],
	'struct ValidateIndexState *' =>
	  [ 'PgValidateState *N', '(struct ValidateIndexState *) N' ],
	'LockTupleMode *' => [ 'PgLockModeOut *N', '(LockTupleMode *) N' ],
	'TU_UpdateIndexes *' =>
	  [ 'PgUpdateIndexesOut *N', '(TU_UpdateIndexes *) N' ],
	'XLogReaderState *' => [ 'const PgWalRecord *N', '(XLogReaderState *) N' ],
	'StringInfo' => [ 'PgDesc *N', '(StringInfo) N' ],
	'FunctionCallInfo' => [ 'const PgCall *N', '(FunctionCallInfo) N' ],
	'void *' => [ 'void *N', 'N' ],
	'const DefElem *' => [ 'const void *N', '(const DefElem *) N' ],
	'void **' => [ 'void **N', 'N' ],
	'LWLock *' => [ 'LWLock *N', 'N' ],
	'const void *' => [ 'const void *N', 'N' ],
	'uint32 *' => [ 'uint32 *N', 'N' ],
	'uint64 *' => [ 'uint64 *N', 'N' ],
	'pg_atomic_uint32 *' => [ 'pg_atomic_uint32 *N', 'N' ],
	'pg_atomic_uint64 *' => [ 'pg_atomic_uint64 *N', 'N' ],
	'ConditionVariable *' => [ 'ConditionVariable *N', 'N' ],
	'HTAB *' => [ 'HTAB *N', 'N' ],
	'ResourceOwner' => [ 'ResourceOwner N', 'N' ],);

# FORM_RETURNS gives a C return type of the lists the C type its form returns
# and the wrapping of the wrapper's value V into it; a type it doesn't name
# returns as its parameter form, unwrapped.
my %FORM_RETURNS = (
	'TupleTableSlot *' => [ 'PgOwnedSlot', '(PgOwnedSlot) {V}' ],
	'DoltgresBuild *' => [ 'PgOwnedBuild', '(PgOwnedBuild) {V}' ],
	'DoltgresSort *' => [ 'PgOwnedSort', '(PgOwnedSort) {V}' ],
	'Datum *' => [ 'const PgDatums *', '(const PgDatums *) V' ],
	'const bool *' => [ 'const PgIsNull *', '(const PgIsNull *) V' ],
	'TidStore *' => [ 'PgDeadItems', '(PgDeadItems) {V}' ],
	'DgRewrite *' => [ 'PgRewrite', '(PgRewrite) {V}' ],
	'TableScanDesc' => [ 'PgScanDesc', '(PgScanDesc) {V}' ],
	'IndexScanDesc' => [ 'PgIndexScanDesc', '(PgIndexScanDesc) {V}' ],
	'struct IndexFetchTableData *' =>
	  [ 'PgIndexFetchDesc', '(PgIndexFetchDesc) {V}' ],
	'Datum' => [ 'PgDatum', '(PgDatum) {V}' ],
	'DgIcuLocale' => [ 'PgIcuLocale', '(PgIcuLocale) {V}' ],
	'DgBuiltinLocale' => [ 'PgBuiltinLocale', '(PgBuiltinLocale) {V}' ],
	'DgLocale' => [ 'PgLocale', '(PgLocale) {V}' ],
	'char *' => [ 'PgPgString', '(PgPgString) {V}' ],
	'HeapTuple' => [ 'PgHeapTuple', '(PgHeapTuple) {V}' ],
	'MinimalTuple' => [ 'PgMinimalTuple', '(PgMinimalTuple) {V}' ],
	'IndexBuildResult *' => [ 'PgBuildResult', '(PgBuildResult) {V}' ],
	'DgBuildState' => [ 'PgBuildState', '(PgBuildState) {V}' ],
	'DgParallelTarget' => [ 'PgParallelTarget', '(PgParallelTarget) {V}' ],
	'IndexBulkDeleteResult *' =>
	  [ 'PgIndexStatsOption', 'pg_IndexStats_from_raw(V)' ],
	'const TableAmRoutine *' => [ 'PgTableRoutine', '(PgTableRoutine) {V}' ],
	'const TupleTableSlotOps *' => [ 'PgSlotOps', '(PgSlotOps) {V}' ],);

# form_param returns a parameter's C form declaration and the expression
# passing it, failing closed on a type the binding has no form for.
sub form_param
{
	my ($type, $name) = @_;
	my $form = $FORM_PARAMS{$type}
	  or die "the binding has no C form for the C type $type\n";
	my ($decl, $pass) = @$form;
	$decl =~ s/\bN\b/$name/g;
	$decl =~ s/\bN_len\b/${name}_len/g;
	$pass =~ s/\bN\b/$name/g;
	$pass =~ s/\bN_len\b/${name}_len/g;
	return ($decl, $pass);
}

# form_return returns a C return type's form type and its wrapping of V.
sub form_return
{
	my ($type) = @_;
	return @{ $FORM_RETURNS{$type} } if $FORM_RETURNS{$type};
	my ($decl) = form_param($type, 'X');
	$decl =~ s/\s*\bX\z//;
	return ($decl, 'V');
}

# frontend writes the frontend build's wrappers (design item D12) from
# frontend-calls.list and frontend-entries.list: each wrapper's C over the port
# layer, which returns a negative number with errno in its error on a failure
# and never exits, and each entry's declaration.
sub frontend
{
	my $header =
	  "/* Generated by gen_binding.pl from frontend-calls.list and frontend-entries.list; never edited by hand. */\n";
	my $h = $header;
	$h .= "#ifndef DOLTGRES_FE_CALLS_H\n#define DOLTGRES_FE_CALLS_H\n\n";
	$h .=
	  "typedef struct DgBuffer\n{\n\tchar\t   *data;\n\tsize_t\t\tlen;\n} DgBuffer;\n\n";
	$h .=
	  "/* A line the check reports to its program's main, of a kind, which prints it and never exits. */\n";
	$h .= "typedef void (*DgLine) (int kind, const char *text);\n\n";
	$h .=
	  "/* A WAL range the check hands its program's main, which stores it and never exits. */\n";
	$h .=
	  "typedef void (*DgKept) (uint32 repository, uint64 from, bool oldest, bool found);\n\n";
	my $c = $header;
	$c .=
	  "#include \"postgres_fe.h\"\n\n#include <dirent.h>\n#include <fcntl.h>\n#include <sys/stat.h>\n#include <unistd.h>\n\n#include \"common/relpath.h\"\n\n#include \"fe_calls.h\"\n";

	foreach my $row (rows('frontend-calls.list'))
	{
		die "frontend-calls.list needs six fields: @$row\n" unless @$row == 6;
		my ($name, $returns, $fails, $body) =
		  ($row->[0], $row->[1], $row->[3], $row->[4]);
		die
		  "$name: a frontend wrapper returns int64, negative on a failure\n"
		  unless $returns eq 'int64';
		die "$name: a frontend wrapper fails with errno\n"
		  unless $fails eq 'errno';
		my @params = params($row->[2]);
		my $decl = c_params(\@params, 'int *error');
		$h .= extern_decl('int64', "doltgres_fe_$name", $decl) . "\n";
		$c .=
		  "\nint64\ndoltgres_fe_$name($decl)\n{\n\tint64\t\tresult;\n\n\terrno = 0;\n"
		  . statements($body, "\t")
		  . "\t*error = result < 0 ? errno : 0;\n\treturn result;\n}\n";
	}
	foreach my $row (rows('frontend-entries.list'))
	{
		die "frontend-entries.list needs three fields: @$row\n"
		  unless @$row == 3;
		die "$row->[0]: the one frontend entry's form\n"
		  unless $row->[1] eq 'const char *datadir, DgLine line, DgKept kept';
		$h .= extern_decl('bool', $row->[0],
			'const char *datadir, DgLine line, DgKept kept, DgBuffer failure')
		  . "\n";
	}
	$h .= "\n#endif\t\t\t\t\t\t\t/* DOLTGRES_FE_CALLS_H */\n";
	written('fe_calls.h', $h);
	written('fe_calls.c', $c);
}

# frontend_forms writes each frontend-calls.list wrapper's function in
# fe_pg_calls.c and fe_pg_calls.h, and the exported side of each
# frontend-entries.list entry, which runs fsck's body of the same name, in
# fe_export.c.
sub frontend_forms
{
	my $header =
	  "/* Generated by gen_binding.pl from frontend-calls.list and frontend-entries.list; never edited by hand. */\n";
	my $fh = $header;
	$fh .=
	  "#ifndef DOLTGRES_PG_FE_PG_CALLS_H\n#define DOLTGRES_PG_FE_PG_CALLS_H\n\n#include \"doltgres/pg/frontend.h\"\n\n";
	my $fc = $header;
	$fc .= "#include \"postgres_fe.h\"\n\n#include \"fe_pg_calls.h\"\n";
	foreach my $row (rows('frontend-calls.list'))
	{
		my ($name, $body) = ($row->[0], $row->[4]);
		my @decls;
		my @passes;
		foreach my $param (params($row->[2]))
		{
			my ($decl, $pass) = form_param(@$param);
			push @decls, $decl;
			push @passes, $pass;
		}
		push @decls, 'int64 *result', 'PgFrontendFeError *error';
		my $decl = join(', ', @decls);
		$fh .= extern_decl('bool', "pg_frontend_calls_$name", $decl) . "\n";
		(my $line_of = "`$body` (`frontend-calls.list`)") =~ s/\*\//* \//g;
		$fc .=
		  "\n/* pg_frontend_calls_$name is the wrapper of $line_of, which never exits. */\nbool\npg_frontend_calls_$name($decl)\n{\n\tint\t\t\terror_number = 0;\n\tint64\t\tvalue = doltgres_fe_$name("
		  . join('', map { "$_, " } @passes)
		  . "&error_number);\n\n\tif (value < 0)\n\t{\n\t\terror->f0 = error_number;\n\t\treturn false;\n\t}\n\t*result = value;\n\treturn true;\n}\n";
	}
	$fh .= "\n#endif\t\t\t\t\t\t\t/* DOLTGRES_PG_FE_PG_CALLS_H */\n";
	my $fe = $header;
	$fe .=
	  "#include \"postgres_fe.h\"\n\n#include \"doltgres/fsck/lib.h\"\n#include \"doltgres/pg/frontend.h\"\n";
	foreach my $row (rows('frontend-entries.list'))
	{
		my $name = $row->[0];
		$fe .=
		  "\n/* $name is the exported side of $name (`frontend-entries.list`), which runs fsck's body of the name. */\nbool\n$name(const char *datadir, DgLine line, DgKept kept, DgBuffer failure)\n{\n\treturn pg_frontend_run_frontend(datadir, line, kept, failure, fsck_entries_$name);\n}\n";
	}
	written('fe_pg_calls.h', $fh);
	written('fe_pg_calls.c', $fc);
	written('fe_export.c', $fe);
}

if ($frontend)
{
	frontend();
	frontend_forms();
	exit 0;
}

# exported returns the PGDLLEXPORT an entry thunk is declared with: postgres
# exports the binding's doltgres_ names, its entry thunks, wrappers, settings,
# and globals, to the modules that call them, contrib's and the fork's tests',
# while the fork's other names stay hidden; a built-in SQL function, an
# entries.list entry taking only a FunctionCallInfo, has none, since
# fmgrprotos.h declares it and fmgr's table calls it.
sub exported
{
	my ($entry) = @_;
	my @params = @{ $entry->{params} };
	my $builtin =
		!$entry->{tested}
	  && $entry->{returns} eq 'Datum'
	  && @params == 1
	  && $params[0][0] eq 'FunctionCallInfo';
	return $builtin ? '' : 'PGDLLEXPORT ';
}

# listed returns a list's rows, and its test list's after them in a build
# with injection points.
sub listed
{
	my ($name, $test) = @_;
	my @all = map { [ @$_, 0 ] } rows($name);
	my @tests = map { [ @$_, 1 ] } rows($test);
	push @all, @tests if $testing;
	return @all;
}

my @calls;
foreach my $r (listed('calls.list', 'test-calls.list'))
{
	pop(@$r);
	die "calls.list needs eight fields: @$r\n" unless @$r == 8;
	my $raises = $r->[3] eq 'raises';
	my $escalates = $r->[3] eq 'escalates';
	die "$r->[0]: mark $r->[3] is not raises, noraise, or escalates\n"
	  unless $raises || $escalates || $r->[3] eq 'noraise';
	die
	  "$r->[0]: a wrapper returns bytes only of a record, a slot's row, a scan's shared scan, or an index fetch's key it borrows, without raising, or of a call's argument, a slot's value, an index entry's value, or a scan key's argument, whose type it checks\n"
	  unless $r->[1] ne 'DgBytes'
	  || (
		!$raises
		&& (   index($r->[2], 'XLogReaderState *') >= 0
			|| $r->[2] eq 'const IndexScanDesc scan'
			|| index($r->[2], 'const TupleTableSlot *') >= 0
			|| $r->[2] eq 'const TableScanDesc scan'
			|| $r->[2] eq 'const struct IndexFetchTableData *fetch'))
	  || (
		$raises
		&& (   index($r->[2], 'FunctionCallInfo') >= 0
			|| index($r->[2], 'const TupleTableSlot *') >= 0
			|| index($r->[2], 'Datum *values') >= 0
			|| index($r->[2], 'const IndexScanDesc scan') == 0));
	die
	  "$r->[0]: a wrapper returns a relation or a snapshot only of a scan or an index fetch it borrows, without raising\n"
	  unless ($r->[1] ne 'Relation' && $r->[1] ne 'Snapshot')
	  || (
		$r->[3] eq 'noraise'
		&& ($r->[2] eq 'const TableScanDesc scan'
			|| (   $r->[1] eq 'Relation'
				&& $r->[2] eq 'const struct IndexFetchTableData *fetch')
			|| (   $r->[1] eq 'Relation'
				&& $r->[2] eq 'Relation rel, int32 at, int32 index')
			|| (   $r->[1] eq 'Relation'
				&& $r->[2] eq 'const IndexVacuumInfo *info')));
	die
	  "$r->[0]: only a void wrapper, a raising one setting *out, or one that can't raise setting result has statements\n"
	  unless $r->[1] eq 'void'
	  || index($r->[6], '; ') < 0
	  || ($raises && index($r->[6], '*out') >= 0)
	  || ($r->[3] eq 'noraise' && index($r->[6], 'result = ') >= 0);
	my $catalog = mark($r->[4], 'catalog', 'nocatalog');
	die "$r->[0] needs the reason for its marks\n"
	  unless ($raises && $catalog) || $r->[5] =~ /\S/;
	push @calls,
	  {
		name => $r->[0],
		returns => $r->[1],
		params => [ params($r->[2]) ],
		raises => $raises,
		escalates => $escalates,
		body => $r->[6],
		catalog => $catalog,
	  };
}

my @entries;
foreach my $r (listed('entries.list', 'test-entries.list'))
{
	my $tested = pop(@$r);
	die "entries.list needs seven fields: @$r\n" unless @$r == 7;
	my $panic = mark($r->[3], 'panic', 'error');
	my $catalog = mark($r->[4], 'catalog', 'nocatalog');
	my $caps = eval { capabilities($panic, $catalog, $r->[5]) };
	die "$r->[0]: $@" unless defined($caps);
	my @params = params($r->[2]);
	my $given = has_out($r->[1], @params) ? pop(@params) : undef;
	push @entries,
	  {
		name => $r->[0],
		returns => $r->[1],
		params => \@params,
		out => $given,
		panic => $panic,
		caps => $caps,
		tested => $tested,
	  };
}

my @settings = rows('settings.list');
my @structs = rows('structs.list');
my @globals = rows('globals.list');

my $header =
  "/* Generated by gen_binding.pl from calls.list, entries.list, settings.list, structs.list, and globals.list; never edited by hand. */\n";
my $h = $header;
$h .=
  "#ifndef DOLTGRES_CALLS_H\n#define DOLTGRES_CALLS_H\n\n#include \"access/htup_details.h\"\n#include \"access/skey.h\"\n#include \"access/genam.h\"\n#include \"access/nbtree.h\"\n#include \"access/tableam.h\"\n#include \"access/tidstore.h\"\n#include \"access/xlog_internal.h\"\n#include \"access/xlogreader.h\"\n#include \"executor/tuptable.h\"\n#include \"lib/stringinfo.h\"\n#include \"nodes/execnodes.h\"\n#include \"nodes/parsenodes.h\"\n#include \"port/atomics.h\"\n#include \"postmaster/bgworker.h\"\n#include \"storage/condition_variable.h\"\n#include \"storage/ipc.h\"\n#include \"storage/lwlock.h\"\n#include \"storage/proc.h\"\n#include \"storage/shmem.h\"\n#include \"storage/spin.h\"\n#include \"utils/hsearch.h\"\n#include \"utils/pg_locale.h\"\n#include \"utils/rel.h\"\n#include \"utils/resowner.h\"\n\n";
$h .=
  "typedef struct DgError\n{\n\tErrorData  *data;\n\tint\t\t\tsqlerrcode;\n} DgError;\n\n";
$h .=
  "typedef struct DgBytes\n{\n\tconst char *data;\n\tsize_t\t\tlen;\n} DgBytes;\n\n";
$h .=
  "typedef struct DgStored\n{\n\tuint64\t\twriter;\n\tuint64\t\tdeleter;\n\tuint32\t\tcommand;\n\tuint32\t\tdeleter_command;\n\tOid\t\t\ttable;\n\tOid\t\t\ttoast;\n\tuint32\t\tblock;\n\tuint16\t\toffset;\n\tsize_t\t\troom;\n} DgStored;\n\n";
$h .=
  "typedef struct DgScanKeys\n{\n\tint\t\t\tcount;\n\tScanKey\t\tkeys;\n} DgScanKeys;\n\n";
$h .=
  "typedef struct DgOwned\n{\n\tuint32\t\tslot;\n\tuint64\t\tgeneration;\n} DgOwned;\n\n";
$h .=
  "typedef struct DgStorage\n{\n\tOid\t\t\ttablespace;\n\tOid\t\t\tdatabase;\n\tRelFileNumber relation;\n} DgStorage;\n\n";
$h .= "typedef uint64 DgCount;\n\n";
$h .= "typedef void *DgBuildState;\n\n";
$h .= "typedef struct DgRewrite DgRewrite;\n\n";
$h .= "typedef void *DgParallelTarget;\n\n";
$h .=
  "typedef struct DgBitmapPage\n{\n\tbool\t\tfound;\n\tbool\t\tlossy;\n\tbool\t\trecheck;\n\tBlockNumber block;\n\tint\t\t\tcount;\n\tOffsetNumber offsets[MaxHeapTuplesPerPage];\n} DgBitmapPage;\n\n";
$h .=
  "typedef struct DgSnapshotView\n{\n\tint\t\t\tkind;\n\tuint64\t\txmin;\n\tuint64\t\txmax;\n\tuint32\t\tcommand;\n\tbool\t\trecovery;\n\tbool\t\toverflowed;\n\tconst TransactionId *xip;\n\tuint32\t\txcnt;\n\tconst TransactionId *subxip;\n\tint32\t\tsubxcnt;\n} DgSnapshotView;\n\n";
$h .=
  "typedef struct DgRoot\n{\n\tbool\t\tpresent;\n\tuint8\t\taddress[32];\n\tuint8\t\tlocator[14];\n} DgRoot;\n\n";
$h .=
  "typedef struct DgParallelBase\n{\n\tDgRoot\t\ttable;\n\tDgRoot\t\ttuples;\n\tDgRoot\t\tchain;\n\tuint64\t\tentries;\n\tuint64\t\tfresh;\n\tuint64\t\tfolded;\n\tuint32\t\tepoch;\n} DgParallelBase;\n\n";
$h .=
  "typedef struct DgCompared\n{\n\tOid\t\t\tfunction;\n\tOid\t\t\tcollation;\n\tint16\t\ttyplen;\n\tbool\t\tbyval;\n} DgCompared;\n\n";
$h .=
  "typedef struct DgKeyColumn\n{\n\tint16\t\tattnum;\n\tOid\t\t\tcollation;\n\tOid\t\t\ttype;\n\tOid\t\t\tclass_;\n} DgKeyColumn;\n\n";
$h .=
  "typedef struct DgIndexColumn\n{\n\tint16\t\tattnum;\n\tint16\t\toption;\n\tOid\t\t\tcollation;\n\tOid\t\t\ttype;\n} DgIndexColumn;\n\n";
$h .=
  "typedef struct DgClass\n{\n\tchar\t\tkind;\n\tOid\t\t\trewrite;\n} DgClass;\n\n";
$h .=
  "typedef struct DgPreprocessed\n{\n\tbool\t\tqual_ok;\n\tbool\t\tskip_scan;\n\tbool\t\torders;\n\tint\t\t\tkeys;\n\tint\t\t\tarrays;\n} DgPreprocessed;\n\n";
$h .=
  "typedef struct DgIndexKey\n{\n\tint\t\t\tflags;\n\tint16\t\tattno;\n\tint16\t\tstrategy;\n\tOid\t\t\tsubtype;\n\tOid\t\t\tcollation;\n\tuint64\t\targument;\n\tint\t\t\tmembers;\n} DgIndexKey;\n\n";
$h .=
  "typedef struct DgIndexArray\n{\n\tint\t\t\tscan_key;\n\tint\t\t\tnum_elems;\n\tbool\t\tnull_elem;\n\tbool\t\tskip_support;\n\tbool\t\tlow;\n\tbool\t\thigh;\n\tuint64\t\tlow_elem;\n\tuint64\t\thigh_elem;\n} DgIndexArray;\n\n";
$h .=
  "typedef struct DgSkipStep\n{\n\tbool\t\toverflow;\n\tuint64\t\tdatum;\n} DgSkipStep;\n\n";
$h .=
  "typedef struct DgBuffer\n{\n\tchar\t   *data;\n\tsize_t\t\tlen;\n} DgBuffer;\n\n";
$h .=
  "/* A locale Postgres' ICU constructor made, which alone the wrappers of a locale's ICU fields take. */\n";
$h .= "typedef pg_locale_t DgIcuLocale;\n\n";
$h .= "/* A locale Postgres' builtin constructor made. */\n";
$h .= "typedef pg_locale_t DgBuiltinLocale;\n\n";
$h .= "/* A locale of any provider, which an entry gives Postgres. */\n";
$h .= "typedef pg_locale_t DgLocale;\n\n";
$h .=
  "/* The C library's locale Postgres made of a libc collation's names, which an entry keeps. */\n";
$h .= "typedef locale_t DgLibcLocale;\n\n";
$h .=
  "/* The number of auxiliary processes, as a constant bindgen can read. */\n";
$h .=
  "static const unsigned int DOLTGRES_AUXILIARY_PROCS = NUM_AUXILIARY_PROCS;\n\n";
$h .=
  "typedef enum DgOutcomeKind\n{\n\tDG_OK,\n\tDG_POSTGRES_ERROR,\n\tDG_PANIC,\n\tDG_OUT_OF_MEMORY,\n\tDG_FORK_ERROR,\n} DgOutcomeKind;\n\n";
$h .=
  "typedef struct DgOutcome\n{\n\tDgOutcomeKind kind;\n\tsize_t\t\tsize;\n\tDgError\t\terror;\n\tchar\t\tmessage[1024];\n\tchar\t\thint[1024];\n} DgOutcome;\n\n";

my $c = $header;
$c .=
  "#include \"postgres.h\"\n\n#include \"access/relation.h\"\n#include <sys/stat.h>\n#include <unicode/ucasemap.h>\n#include <unicode/ucol.h>\n\n#include \"doltgres/disk_space.h\"\n#include \"doltgres/hooks.h\"\n#include \"doltgres/in_progress.h\"\n#include \"doltgres/kept_wal.h\"\n#include \"doltgres/locale_ctype.h\"\n#include \"fmgr.h\"\n#include \"access/detoast.h\"\n#include \"access/genam.h\"\n#include \"access/heapam_xlog.h\"\n#include \"access/heaptoast.h\"\n#include \"access/toast_internals.h\"\n#include \"access/htup_details.h\"\n#include \"access/multixact.h\"\n#include \"access/nbtree.h\"\n#include \"access/table.h\"\n#include \"access/tableam.h\"\n#include \"access/transam.h\"\n#include \"access/tsmapi.h\"\n#include \"access/twophase.h\"\n#include \"access/visibilitymap.h\"\n#include \"access/xact.h\"\n#include \"access/xlog.h\"\n#include \"access/xloginsert.h\"\n#include \"access/xlogrecovery.h\"\n#include \"access/xlogutils.h\"\n#include \"catalog/catalog.h\"\n#include \"catalog/index.h\"\n#include \"catalog/pg_am.h\"\n#include \"catalog/pg_am_d.h\"\n#include \"catalog/pg_collation.h\"\n#include \"catalog/pg_database.h\"\n#include \"catalog/pg_enum.h\"\n#include \"catalog/pg_index.h\"\n#include \"catalog/pg_inherits.h\"\n#include \"catalog/pg_range.h\"\n#include \"commands/defrem.h\"\n#include \"catalog/pg_operator_d.h\"\n#include \"catalog/pg_proc.h\"\n#include \"commands/vacuum.h\"\n#include \"executor/executor.h\"\n#include \"mb/pg_wchar.h\"\n#include \"miscadmin.h\"\n#include \"optimizer/optimizer.h\"\n#include \"pgstat.h\"\n#include \"port/pg_bitutils.h\"\n#include \"postmaster/autovacuum.h\"\n#include \"postmaster/bgworker.h\"\n#include \"postmaster/interrupt.h\"\n#include \"postmaster/bgwriter.h\"\n#include \"storage/fd.h\"\n#include \"storage/latch.h\"\n#include \"storage/lmgr.h\"\n#include \"storage/predicate.h\"\n#include \"storage/proc.h\"\n#include \"storage/procarray.h\"\n#include \"storage/standby.h\"\n#include \"utils/builtins.h\"\n#include \"utils/catcache.h\"\n#include \"utils/combocid.h\"\n#include \"utils/datum.h\"\n#include \"utils/fmgroids.h\"\n#include \"utils/guc.h\"\n#include \"utils/index_selfuncs.h\"\n#include \"utils/guc_tables.h\"\n#include \"utils/injection_point.h\"\n#include \"utils/inval.h\"\n#include \"utils/lsyscache.h\"\n#include \"utils/memutils.h\"\n#include \"utils/rel.h\"\n#include \"utils/snapmgr.h\"\n#include \"utils/syscache.h\"\n#include \"utils/timestamp.h\"\n#include \"utils/tuplesort.h\"\n#include \"utils/typcache.h\"\n#include \"utils/wait_event.h\"\n#include \"varatt.h\"\n\n#include \"calls.h\"\n#include \"catalog/storage.h\"\n#include \"catalog/storage_xlog.h\"\n\n";
$c .=
  "/* DgTidKey is heap's TidHashKey in rewriteheap.c: the xid and the tuple id a held version waits for, its padding\n * zeroed. */\ntypedef struct DgTidKey\n{\n\tTransactionId xmin;\n\tItemPointerData tid;\n} DgTidKey;\n\n/* DgHeld is a held version's entry: its key, and the version's place among the rewrite's row versions. */\ntypedef struct DgHeld\n{\n\tDgTidKey\tkey;\n\tuint64\t\tversion;\n} DgHeld;\n\n/* DgRewrite is a table rewrite's state: its held versions, heap's unresolved tuples, and its order, a sort of heap\n * tuples or of index tuples by the index's keys and heap tid, or a scan of the index. */\nstruct DgRewrite\n{\n\tHTAB\t   *held;\n\tTuplesortstate *sort;\n\tbool\t\theap;\n\tRelation\tindex;\n\tIndexInfo  *info;\n\tEState\t   *estate;\n\tIndexScanDesc scan;\n};\n\n";
$c .=
  "/* Postgres' ICU locale constructor and version function, which pg_locale.c declares for itself alone. */\nextern pg_locale_t create_pg_locale_icu(Oid collid, MemoryContext context);\nextern char *get_collation_actual_version_icu(const char *collcollate);\n/* Postgres' builtin locale constructor, which pg_locale.c declares for itself alone. */\nextern pg_locale_t create_pg_locale_builtin(Oid collid, MemoryContext context);\n\n";

foreach my $record (@structs)
{
	die "structs.list needs three fields: @$record\n" unless @$record == 3;
	die "$record->[0] needs its reason\n" unless $record->[2] =~ /\S/;
	$h .=
	  "/* $record->[0] is $record->[2] (structs.list). */\ntypedef struct $record->[0]\n{\n";
	foreach my $member (split(/; /, $record->[1], -1))
	{
		my $at = -1;
		for (my $i = length($member) - 1; $i >= 0; $i--)
		{
			my $ch = substr($member, $i, 1);
			if ($ch eq ' ' || $ch eq '*')
			{
				$at = $i;
				last;
			}
		}
		die "no name in member $member\n" if $at < 0;
		my $type = substr($member, 0, $at + 1);
		$type =~ s/^\s+|\s+$//g;
		$h .= "\t" . c_decl($type, substr($member, $at + 1)) . ";\n";
	}
	$h .= "} $record->[0];\n\n";
}

foreach my $global (@globals)
{
	die "globals.list needs four fields: @$global\n" unless @$global == 4;
	die "$global->[0] needs its reason\n" unless $global->[3] =~ /\S/;
	if (index($global->[1], 'ResourceOwnerDesc') >= 0)
	{
		my $priority;
		if ($global->[2] =~ /\.release_priority = ([^,}]*)/)
		{
			$priority = $1;
		}
		my $named =
			 defined($priority)
		  && $priority =~ /^RELEASE_PRIO_/
		  && $priority =~ /^[A-Z_]*\z/;
		my $after_pins =
		  defined($priority) && $priority eq 'RELEASE_PRIO_FIRST + 1';
		my $first = defined($priority) && $priority eq 'RELEASE_PRIO_FIRST';
		die
		  "$global->[0]: a resource kind names its priority by a RELEASE_PRIO_ constant; only the node cache's pins take RELEASE_PRIO_FIRST, and only the fork's owned records RELEASE_PRIO_FIRST + 1, right after them\n"
		  unless ($named || $after_pins)
		  && (($global->[0] eq 'doltgres_pin_desc') == $first)
		  && (($global->[0] eq 'doltgres_owned_desc') == $after_pins);
	}
	my ($name, $type, $init) = @$global[ 0 .. 2 ];
	my ($base, $star) = split_star($type);
	my $lead =
	  'extern ' . ($name =~ /^doltgres_/ ? 'PGDLLEXPORT ' : '') . $base;
	$h .=
		$lead
	  . (length($lead) < 11 && $star eq '' ? "\t" : ' ')
	  . "$star$name;\n";
	if ($init =~ /^\{(.*)\}\z/s)
	{
		$init = "{\n" . join('', map { "\t$_,\n" } split(/, /, $1, -1)) . "}";
	}
	$c .=
		"/* $name is $global->[3] (globals.list). */\n"
	  . c_decl($type, $name)
	  . " = $init;\n\n";
}

$c .=
  "/* dg_take moves the error Postgres raised into error, leaving Postgres' error state empty. */\nstatic void\ndg_take(DgError *error)\n{\n\terror->data = CopyErrorData();\n\tFlushErrorState();\n\terror->sqlerrcode = error->data->sqlerrcode;\n}\n";

foreach my $setting (@settings)
{
	die "settings.list needs five fields: @$setting\n" unless @$setting == 5;
	die "$setting->[0] needs its reason\n" unless $setting->[3] =~ /\S/;
	my $name = $setting->[0];
	my $enumerated;
	if ($setting->[1] eq 'enum')
	{
		$enumerated = 1;
	}
	elsif ($setting->[1] eq 'int')
	{
		$enumerated = 0;
	}
	else
	{
		die "$name: the binding has no settings of type $setting->[1]\n";
	}
	my @values = $enumerated ? split(/,/, $setting->[2], -1) : ();
	my $default = $enumerated ? '0' : $setting->[2];
	$c .=
	  "\n/* doltgres_$name is the variable of the setting $name (settings.list), which the GUC table sets. */\nint\t\t\tdoltgres_$name = $default;\n";
	if ($enumerated)
	{
		$c .=
		  "\n/* doltgres_${name}_options are its values, in settings.list's order, the first its default. */\nconst struct config_enum_entry doltgres_${name}_options[] = {\n";
		for (my $i = 0; $i < @values; $i++)
		{
			$c .= "\t{\"$values[$i]\", $i, false},\n";
		}
		$c .= "\t{NULL, 0, false}\n};\n";
	}
	$h .=
	  extern_decl('PGDLLEXPORT int', "doltgres_setting_$name", 'void') . "\n";
	$c .=
	  "\nint\ndoltgres_setting_$name(void)\n{\n\treturn doltgres_$name;\n}\n";
}

foreach my $call (@calls)
{
	my $name = "doltgres_call_$call->{name}";
	my $returns = $call->{returns};
	my $out_param;
	if ($returns ne 'void')
	{
		$out_param = $returns =~ /\*\z/ ? "${returns}*out" : "$returns *out";
	}
	if ($call->{raises})
	{
		my @more;
		push @more, $out_param if defined($out_param);
		push @more, 'DgError *error';
		my $decl = c_params($call->{params}, @more);
		$h .= extern_decl('PGDLLEXPORT int', $name, $decl) . "\n";
		my $assign =
		  defined($out_param)
		  && index($call->{body}, '*out') < 0
		  ? "\t\t*out = $call->{body};\n"
		  : statements($call->{body}, "\t\t");
		$c .=
		  "\nint\n$name($decl)\n{\n\tMemoryContext context = CurrentMemoryContext;\n\tuint32\t\tholdoff = InterruptHoldoffCount;\n\tuint32\t\tcancel = QueryCancelHoldoffCount;\n\tvolatile int failed = 0;\n\n\tPG_TRY();\n\t{\n$assign\t}\n\tPG_CATCH();\n\t{\n\t\tMemoryContextSwitchTo(context);\n\t\tInterruptHoldoffCount = holdoff;\n\t\tQueryCancelHoldoffCount = cancel;\n\t\tdg_take(error);\n\t\tfailed = 1;\n\t}\n\tPG_END_TRY();\n\treturn failed;\n}\n";
	}
	elsif ($call->{escalates})
	{
		my $decl = c_params($call->{params});
		$h .= extern_decl("PGDLLEXPORT $returns", $name, $decl) . "\n";
		my ($result, $body, $give);
		if ($returns eq 'void')
		{
			($result, $body, $give) =
			  ('', statements($call->{body}, "\t\t"), '');
		}
		else
		{
			($result, $body, $give) = (
				"\t$returns volatile result;\n",
				"\t\tresult = $call->{body};\n",
				"\treturn result;\n");
		}
		$c .=
		  "\n$returns\n$name($decl)\n{\n\tMemoryContext context = CurrentMemoryContext;\n$result\n\tHOLD_INTERRUPTS();\n\tPG_TRY();\n\t{\n$body\t}\n\tPG_CATCH();\n\t{\n\t\tErrorData  *edata;\n\n\t\tMemoryContextSwitchTo(context);\n\t\tedata = CopyErrorData();\n\t\tereport(PANIC, errcode(edata->sqlerrcode), errmsg_internal(\"%s\", edata->message));\n\t}\n\tPG_END_TRY();\n\tRESUME_INTERRUPTS();\n$give}\n";
	}
	else
	{
		my $decl = c_params($call->{params});
		$h .= extern_decl("PGDLLEXPORT $returns", $name, $decl) . "\n";
		my $typed = $returns =~ /\*\z/ ? $returns : "$returns ";
		my $body;
		if ($returns eq 'void')
		{
			$body =
				"\tHOLD_INTERRUPTS();\n\t{\n"
			  . statements($call->{body}, "\t\t")
			  . "\t}\n\tRESUME_INTERRUPTS();\n";
		}
		elsif (index($call->{body}, 'result = ') >= 0)
		{
			$body =
				"\t${typed}result;\n\n\tHOLD_INTERRUPTS();\n\t{\n"
			  . statements($call->{body}, "\t\t")
			  . "\t}\n\tRESUME_INTERRUPTS();\n\treturn result;\n";
		}
		else
		{
			$body =
			  "\t${typed}result;\n\n\tHOLD_INTERRUPTS();\n\tresult = $call->{body};\n\tRESUME_INTERRUPTS();\n\treturn result;\n";
		}
		$c .= "\n$returns\n$name($decl)\n{\n$body}\n";
	}
}

my $e = $header;
$e .=
  "#include \"postgres.h\"\n\n#include \"fmgr.h\"\n#include \"doltgres/hooks.h\"\n"
  . "#include \"doltgres/std/alloc.h\"\n"
  . "#include \"utils/fmgrprotos.h\"\n\n#include \"calls.h\"\n\n";

# A C port thunk first makes the port heap the port allocates in, if the process
# has none yet, while nothing of the port's is live, so that its error can be
# raised.
my $heap = "\tstd_heap_make();\n";
$e .=
  "/* dg_raise raises what an entry's exported side returned, at level for all but an error Postgres raised. */\npg_noreturn static void\ndg_raise(DgOutcome *outcome, int level)\n{\n\tswitch (outcome->kind)\n\t{\n\t\tcase DG_POSTGRES_ERROR:\n\t\t\tReThrowError(outcome->error.data);\n\t\t\tbreak;\n\t\tcase DG_OUT_OF_MEMORY:\n\t\t\tereport(level, errcode(ERRCODE_OUT_OF_MEMORY), errmsg(\"out of memory\"),\n\t\t\t\t\terrdetail(\"Failed on request of size %zu.\", outcome->size));\n\t\t\tbreak;\n\t\tcase DG_PANIC:\n\t\t\tereport(level, errcode(ERRCODE_INTERNAL_ERROR), errmsg_internal(\"%s\", outcome->message));\n\t\t\tbreak;\n\t\tcase DG_FORK_ERROR:\n\t\t\tereport(level, errcode(outcome->error.sqlerrcode), errmsg_internal(\"%s\", outcome->message),\n\t\t\t\t\toutcome->hint[0] != '\\0' ? errhint(\"%s\", outcome->hint) : 0);\n\t\t\tbreak;\n\t\tcase DG_OK:\n\t\t\tbreak;\n\t}\n\tpg_unreachable();\n}\n";

foreach my $entry (@entries)
{
	my ($full, $gives);
	if (defined($entry->{out}))
	{
		my ($type, $name) = @{ $entry->{out} };
		$full = c_params($entry->{params}, "$type$name");
		$gives = substr($type, 0, -1);
		$gives =~ s/\s+\z//;
	}
	else
	{
		$full = c_params($entry->{params});
		$gives = $entry->{returns};
	}
	$h .=
	  extern_decl(exported($entry) . $entry->{returns}, $entry->{name}, $full)
	  . "\n";
	my $decl = c_params($entry->{params}, 'DgOutcome *outcome');
	$h .= extern_decl($gives, "doltgres_rs_$entry->{name}", $decl) . "\n";
	my @call_args = ((map { $_->[1] } @{ $entry->{params} }), '&outcome');
	my $level = $entry->{panic} ? 'PANIC' : 'ERROR';
	my ($result_decl, $assign, $ret);
	if (defined($entry->{out}))
	{
		my $name = $entry->{out}[1];
		$result_decl = "\t" . c_decl($gives, 'value') . ";\n";
		$assign = 'value = ';
		$ret =
		  "\tif (value == NULL)\n\t\treturn false;\n\t*$name = value;\n\treturn true;\n";
	}
	elsif ($entry->{returns} eq 'void')
	{
		($result_decl, $assign, $ret) = ('', '', '');
	}
	else
	{
		$result_decl = "\t$entry->{returns}\t\tresult;\n";
		$assign = 'result = ';
		$ret = "\treturn result;\n";
	}
	$e .=
	  "\n$entry->{returns}\n$entry->{name}($full)\n{\n\tDgOutcome\toutcome;\n$result_decl\n$heap\toutcome.kind = DG_OK;\n\t${assign}doltgres_rs_$entry->{name}("
	  . join(', ', @call_args)
	  . ");\n\tif (outcome.kind != DG_OK)\n\t\tdg_raise(&outcome, $level);\n$ret}\n";
}
$h .= "\n#endif\t\t\t\t\t\t\t/* DOLTGRES_CALLS_H */\n";

written('calls.h', $h);
written('calls.c', $c);
written('entries.c', $e);

# Each calls.list line's function and each settings.list line's getter and
# constants, in pg_calls.c and pg_calls.h beside the wrappers, since meson
# writes a custom target's files into one folder.

my $fh = "/* Generated by gen_binding.pl from calls.list and settings.list; never edited by hand. */\n";
$fh .= "#ifndef DOLTGRES_PG_PG_CALLS_H\n#define DOLTGRES_PG_PG_CALLS_H\n\n";
$fh .= "#ifndef FRONTEND\n\n#include \"doltgres/pg/lib.h\"\n\n";
my $fc = "/* Generated by gen_binding.pl from calls.list and settings.list; never edited by hand. */\n";
$fc .= "#ifndef FRONTEND\n#include \"postgres.h\"\n\n#include \"pg_calls.h\"\n";

foreach my $setting (@settings)
{
	my $name = $setting->[0];
	my $enumerated = $setting->[1] eq 'enum';
	my @values = $enumerated ? split(/,/, $setting->[2], -1) : ();
	$fh .= extern_decl('int32', "pg_calls_settings_$name", 'void') . "\n";
	$fc .=
		"\n/* pg_calls_settings_$name returns the value of the setting $name"
	  . ($enumerated ? ', one of its constants' : '')
	  . ". */\nint32\npg_calls_settings_$name(void)\n{\n\treturn doltgres_setting_$name();\n}\n";
	for (my $i = 0; $i < @values; $i++)
	{
		my $constant = uc("${name}_$values[$i]");
		$fh .=
		  "\n/* PG_CALLS_SETTINGS_$constant is the value $values[$i] of $name. */\n#define PG_CALLS_SETTINGS_$constant $i\n";
	}
}
$fh .= "\n";

foreach my $call (@calls)
{
	my $name = $call->{name};
	my $returns = $call->{returns};
	my $lends = $call->{raises} && $returns eq 'DgBytes';
	my @decls;
	my @passes;
	push @decls, 'const PgMayRaise *_raise' if $call->{raises};
	push @decls, 'const PgCatalog *_catalog' if $call->{catalog};
	foreach my $param (@{ $call->{params} })
	{
		my ($decl, $pass) = form_param(@$param);
		push @decls, $decl;
		push @passes, $pass;
	}
	my $line_of = "`$call->{body}` (`calls.list`)";
	$line_of =~ s/\*\//* \//g;
	if ($call->{raises})
	{
		my ($ret, $wrap, $out) = ('', '', '');
		if ($lends)
		{
			push @decls, 'const uint8 **result, size_t *result_len';
		}
		elsif ($returns ne 'void')
		{
			($ret, $wrap) = form_return($returns);
			push @decls, ($ret =~ /\*\z/ ? "${ret}*result" : "$ret *result");
		}
		push @decls, 'PgPgError *error';
		my $decl = join(', ', @decls);
		$fh .= extern_decl('bool', "pg_calls_$name", $decl) . "\n";
		my $body = '';
		$body .= "\t" . c_decl($returns, 'out') . ";\n" if $returns ne 'void';
		$body .= "\tDgError\t\terror_ = {NULL, 0};\n\n";
		$body .=
			"\tif (doltgres_call_$name("
		  . join('', map { "$_, " } @passes)
		  . ($returns ne 'void' ? '&out, ' : '')
		  . "&error_) != 0)\n\t{\n\t\t*error = pg_PgError_new(error_);\n\t\treturn false;\n\t}\n";
		if ($lends)
		{
			$body .= "\t*result = pg_borrowed(out, result_len);\n";
		}
		elsif ($returns ne 'void')
		{
			(my $value = $wrap) =~ s/\bV\b/out/;
			$body .= "\t*result = $value;\n";
		}
		$body .= "\treturn true;\n";
		$fc .=
		  "\n/* pg_calls_$name is the wrapper of $line_of. */\nbool\npg_calls_$name($decl)\n{\n$body}\n";
	}
	else
	{
		my $call_expr = "doltgres_call_$name(" . join(', ', @passes) . ")";
		my ($ret, $value);
		if ($returns eq 'void')
		{
			($ret, $value) = ('void', undef);
		}
		elsif ($returns eq 'DgBytes')
		{
			push @decls, 'size_t *result_len';
			($ret, $value) = ('const uint8 *', "pg_borrowed($call_expr, result_len)");
		}
		elsif ($returns eq 'Relation')
		{
			($ret, $value) = ('const PgRel *', "(const PgRel *) $call_expr");
		}
		elsif ($returns eq 'Snapshot')
		{
			($ret, $value) =
			  ('const PgSnapshot *', "(const PgSnapshot *) $call_expr");
		}
		else
		{
			my $wrap;
			($ret, $wrap) = form_return($returns);
			($value = $wrap) =~ s/\bV\b/$call_expr/;
		}
		my $decl = @decls ? join(', ', @decls) : 'void';
		$fh .= extern_decl($ret, "pg_calls_$name", $decl) . "\n";
		my $body =
		  defined($value) ? "\treturn $value;\n" : "\t$call_expr;\n";
		my $why =
		  $call->{escalates}
		  ? "which can't raise into C, since it turns anything raised into a PANIC"
		  : "which can't raise";
		$fc .=
		  "\n/* pg_calls_$name is the wrapper of $line_of, $why. */\n$ret\npg_calls_$name($decl)\n{\n$body}\n";
	}
}
$fh .= "\n#endif\t\t\t\t\t\t\t/* FRONTEND */\n\n#endif\t\t\t\t\t\t\t/* DOLTGRES_PG_PG_CALLS_H */\n";
$fc .= "\n#endif\t\t\t\t\t\t\t/* FRONTEND */\n";

written('pg_calls.h', $fh);
written('pg_calls.c', $fc);

# Each entry's exported side, doltgres_rs_<entry>, in export.c, which runs the
# doltgres crate's body of the entry's name through the instance of run for
# its capabilities and value, after the body's prepare for an ordinary entry,
# with its arguments in their C forms.

# EXPORT_GIVES gives an entry's value type the C form its body gives, the C
# type run gives, the run instance's spelling of it, the default run gives
# when the thunk raises instead, the turning of the body's given G into run's
# value, and the thunk's turning of run's value V into its own.
my %EXPORT_GIVES = (
	'Datum' =>
	  [ 'PgDatum', 'PgDatum', 'Datum', '(PgDatum) {0}', 'G', 'V.f0' ],
	'int64' => [ 'int64', 'int64', 'I64', '0', 'G', 'V' ],
	'TM_Result' => [ 'int32', 'int32', 'I32', '0', 'G', 'V' ],
	'int' => [ 'int32', 'int32', 'I32', '0', 'G', 'V' ],
	'TransactionId' => [ 'uint32', 'uint32', 'U32', '0', 'G', 'V' ],
	'Oid' => [ 'uint32', 'uint32', 'U32', '0', 'G', 'V' ],
	'bool' => [ 'bool', 'bool', 'Bool', 'false', 'G', 'V' ],
	'uint64' => [ 'uint64', 'uint64', 'U64', '0', 'G', 'V' ],
	'Size' => [ 'size_t', 'size_t', 'Usize', '0', 'G', 'V' ],
	'double' => [ 'float8', 'float8', 'F64', '0.0', 'G', 'V' ],
	'HeapTuple' => [
		'PgHeapTuple', 'PgHeapTuple',
		'HeapTuple', '(PgHeapTuple) {0}',
		'G', 'V.f0'
	],
	'MinimalTuple' => [
		'PgMinimalTuple', 'PgMinimalTuple',
		'MinimalTuple', '(PgMinimalTuple) {0}',
		'G', 'V.f0'
	],
	'IndexBuildResult *' => [
		'PgBuildResult', 'PgBuildResult',
		'BuildResult', '(PgBuildResult) {0}',
		'G', 'V.f0'
	],
	'const TupleTableSlotOps *' =>
	  [ 'PgSlotOps', 'PgSlotOps', 'SlotOps', '(PgSlotOps) {0}', 'G', 'V.f0' ],
	'TableScanDesc' => [
		'PgScanDesc', 'PgScanDesc', 'ScanDesc', '(PgScanDesc) {0}',
		'G', 'V.f0'
	],
	'IndexScanDesc' => [
		'PgIndexScanDesc', 'PgIndexScanDesc',
		'IndexScanDesc', '(PgIndexScanDesc) {0}',
		'G', 'V.f0'
	],
	'struct IndexFetchTableData *' => [
		'PgIndexFetchDesc', 'PgIndexFetchDesc',
		'IndexFetchDesc', '(PgIndexFetchDesc) {0}',
		'G', 'V.f0'
	],
	'IndexBulkDeleteResult *' => [
		'PgIndexStatsOption',
		'IndexBulkDeleteResult *',
		'MutIndexBulkDeleteResult',
		'NULL',
		'G.has ? pg_IndexStats_into_raw(&G.stats) : NULL',
		'V'
	],
	'pg_locale_t' => [
		'PgLocaleOption', 'pg_locale_t', 'DgLocale', 'NULL',
		'G.has ? pg_Locale_into_raw(&G.locale) : NULL', 'V'
	],
	'const TableAmRoutine *' => [
		'PgTableRoutineOption',
		'const TableAmRoutine *',
		'ConstTableAmRoutine',
		'NULL',
		'G.has ? pg_TableRoutine_into_raw(&G.routine) : NULL',
		'V'
	],
	'char *' => [
		'PgPgStringOption', 'char *', 'MutCChar', 'NULL',
		'G.has ? pg_PgString_into_raw(&G.string) : NULL', 'V'
	],);

# EXPORT_CAPS gives an entry's capabilities their C type and Grant table.
my %EXPORT_CAPS = (
	'Caps' => [ 'PgCaps', 'PG_CAPS_GRANT_OPS' ],
	'CatalogCaps' => [ 'PgCatalogCaps', 'PG_CATALOG_CAPS_GRANT_OPS' ],
	'Cleanup' => [ 'PgCleanup', 'PG_CLEANUP_GRANT_OPS' ],);

# placed returns a pattern with each standing letter in it replaced by an
# expression.
sub placed
{
	my ($pattern, $letter, $expr) = @_;
	$pattern =~ s/\b$letter\b/$expr/g;
	return $pattern;
}

my $x =
  "/* Generated by gen_binding.pl from entries.list; never edited by hand. */\n#include \"postgres.h\"\n\n#include \"calls.h\"\n#include \"doltgres/doltgres/entries.h\"\n";
$x .= "#include \"doltgres/doltgres/entries/testing.h\"\n" if $testing;
$x .= "#include \"doltgres/pg/lib.h\"\n";
my %instances;
my $bodies = '';
foreach my $entry (@entries)
{
	my $name = $entry->{name};
	my $gives = $entry->{returns};
	if (defined($entry->{out}))
	{
		$gives = substr($entry->{out}[0], 0, -1);
		$gives =~ s/\s+\z//;
	}
	my $form;
	if ($gives eq 'const char *')
	{
		$form =
		  defined($entry->{out})
		  ? [
			'PgPgStringOption', 'const char *', 'ConstCChar', 'NULL',
			'G.has ? pg_PgString_into_raw(&G.string) : NULL', 'V'
		  ]
		  : [ 'const char *', 'const char *', 'ConstCChar', 'NULL', 'G',
			'V' ];
	}
	elsif ($gives ne 'void')
	{
		$form = $EXPORT_GIVES{$gives}
		  or die
		  "$name: the binding has no C form for an entry returning $gives\n";
	}
	my ($g, $ops) = @{ $EXPORT_CAPS{ $entry->{caps} } };
	my $rs = defined($form) ? $form->[2] : '';
	$instances{"$entry->{caps}$rs"} =
	  [ $entry->{caps}, $rs, $g, $ops, defined($form) ? $form->[1] : undef ];
	my $closure = "doltgres_rs_${name}_closure_0";
	my $captures =
	  join('', map { ucfirst } split(/_/, "doltgres_rs_$name")) . 'Closure0';
	my (@fields, @inits, @passes, @locals, @setup);
	my @params = @{ $entry->{params} };

	for (my $at = 0; $at < @params; $at++)
	{
		my ($type, $pname) = @{ $params[$at] };
		if ($type eq 'TupleTableSlot **')
		{
			my $count = $params[ $at + 1 ][1];
			push @fields, c_decl('TupleTableSlot', "**$pname"),
			  c_decl('int32', $count);
			push @inits, ".$pname = $pname", ".$count = $count";
			push @locals, c_decl('PgSlotsResult', $pname);
			push @setup,
			  "$pname.ok = pg_slots(c->$pname, c->$count, &$pname.u.slots.ptr, &$pname.u.slots.len, &$pname.u.error);";
			push @passes, "&$pname";
			$at++;
			next;
		}
		if ($at + 1 < @params && $params[ $at + 1 ][0] eq 'varlena *')
		{
			my $slice = $params[ $at + 1 ][1];
			push @fields, c_decl('int32', $pname),
			  c_decl('varlena *', $slice);
			push @inits, ".$pname = $pname", ".$slice = $slice";
			push @locals, c_decl('PgToastSliceResult', $slice);
			push @setup,
			  "$slice.ok = pg_toast_slice(c->$slice, c->$pname, &$slice.u.slice.ptr, &$slice.u.slice.len, &$slice.u.error);";
			push @passes, "&$slice";
			$at++;
			next;
		}
		if ($type eq 'IndexBulkDeleteResult *')
		{
			push @fields, c_decl('PgIndexStatsOption', $pname);
			push @inits, ".$pname = pg_IndexStats_from_raw($pname)";
			push @passes, "&c->$pname";
			next;
		}
		my ($decl, $pass) = form_param($type, $pname);
		(my $form_type = $decl) =~ s/\s*\b\Q$pname\E\z//;
		if ($pass eq $pname)
		{
			push @inits, ".$pname = $pname";
			push @passes, "c->$pname";
		}
		elsif ($pass =~ /^\(.+\) \Q$pname\E\z/)
		{
			push @inits, ".$pname = ($form_type) $pname";
			push @passes, "c->$pname";
		}
		elsif ($pass eq "$pname.f0")
		{
			push @inits, ".$pname = {$pname}";
			push @passes, "c->$pname";
		}
		elsif ($pass eq "$pname->f0")
		{
			$form_type =~ s/\s*\*\z//;
			push @inits, ".$pname = {$pname}";
			push @passes, "&c->$pname";
		}
		else
		{
			die
			  "$name: the binding has no C form of $type to pass its entry\n";
		}
		push @fields, c_decl($form_type, $pname);
	}
	my $body_name =
	  ($entry->{tested} ? 'doltgres_entries_testing_' : 'doltgres_entries_')
	  . $name;
	my $result_param = '';
	my $tail = '';
	my $call_result = '';
	if (defined($form))
	{
		my $r = $form->[1];
		$result_param = $r =~ /\*\z/ ? ", ${r}*result" : ", $r *result";
		if ($form->[4] eq 'G')
		{
			$call_result = 'result, ';
		}
		else
		{
			push @locals, c_decl($form->[0], 'given');
			$call_result = '&given, ';
			$tail =
				"\t*result = "
			  . placed($form->[4], 'G', 'given')
			  . ";\n\treturn true;\n";
		}
	}
	my $call =
		"$body_name(caps, "
	  . join('', map { "$_, " } @passes)
	  . "${call_result}error)";
	my $c_body = '';
	$c_body .= "\t$captures *c = captures;\n" if @fields;
	$c_body .= join('', map { "\t$_;\n" } @locals);
	$c_body .= "\n" if @fields || @locals;
	$c_body .= "\tdoltgres_entries_prepare();\n" unless $entry->{panic};
	$c_body .= join('', map { "\t$_\n" } @setup);

	if ($tail ne '')
	{
		$c_body .= "\tif (!$call)\n\t\treturn false;\n$tail";
	}
	else
	{
		$c_body .= "\treturn $call;\n";
	}
	if (@fields)
	{
		$bodies .=
		  "\n/* $captures is ${closure}'s captures: the arguments Postgres gave the thunk, in their C forms. */\ntypedef struct $captures\n{\n"
		  . join('', map { "\t$_;\n" } @fields)
		  . "} $captures;\n";
	}
	my $closure_decl =
	  "void *captures, const $g *caps$result_param, PgPgError *error";
	my $readying =
	  $entry->{panic}
	  ? ''
	  : ', readying the process for an ordinary entry first';
	$bodies .=
	  "\n/* $closure runs ${name}'s body with the capabilities run grants$readying. */\nstatic bool\n$closure($closure_decl)\n{\n$c_body}\n";
	my $run =
		"pg_run_$entry->{caps}$rs(outcome, "
	  . (defined($form) ? "$form->[3], " : '')
	  . "$closure, "
	  . (@fields ? '&captures' : 'NULL')
	  . ', NULL)';
	my $thunk_body = '';
	$thunk_body .= "\t$captures captures = {" . join(', ', @inits) . "};\n\n"
	  if @fields;
	$thunk_body .=
	  defined($form)
	  ? "\treturn " . placed($form->[5], 'V', $run) . ";\n"
	  : "\t$run;\n";
	my $decl = c_params($entry->{params}, 'DgOutcome *outcome');
	my ($base, $star) = split_star($gives);
	$bodies .=
	  "\n/* doltgres_rs_$name is the exported side of $name (`entries.list`), which runs the doltgres crate's body of the name. */\n$base"
	  . ($star eq '' ? '' : " $star")
	  . "\ndoltgres_rs_$name($decl)\n{\n$thunk_body}\n";
}
foreach my $key (sort keys %instances)
{
	my ($gs, $rs, $g, $ops, $r) = @{ $instances{$key} };
	$x .=
		"\n/* run::<$gs"
	  . ($rs eq '' ? ', ()' : ", $rs")
	  . ">, the instance of run the entries with those capabilities and value run with. */\n#define PG_RUN_SCOPE static\n#define PG_RUN_DECLARE\n#define PG_RUN_DEFINE\n#define PG_RUN_GS $gs\n#define PG_RUN_RS"
	  . ($rs eq '' ? '' : " $rs")
	  . "\n#define PG_RUN_G $g\n#define PG_RUN_G_OPS $ops\n"
	  . (defined($r) ? "#define PG_RUN_R $r\n" : '')
	  . "#include \"doltgres/pg/run_template.h\"\n";
}
$x .= $bodies;
written('export.c', $x);
