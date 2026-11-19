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
# keeps, the stand-ins a binary built without Postgres links, and their Rust
# sides (calls.h, calls.c, entries.c, outside.c, calls.rs, export.rs); with
# --testing, test-calls.list's and test-entries.list's join them; with
# --frontend, the frontend build's wrappers and their Rust sides from
# frontend-calls.list and frontend-entries.list (fe_calls.h, fe_calls.c,
# fe_calls.rs, fe_export.rs). pg's build script runs it into OUT_DIR.

use strict;
use warnings FATAL => 'all';

use Getopt::Long;

my $testing = 0;
my $frontend = 0;
my $forms = 0;

GetOptions(
	'testing' => \$testing,
	'frontend' => \$frontend,
	'forms' => \$forms)
  or die "$0: wrong arguments\n";
die "usage: $0 [--testing] [--frontend] [--forms] <pg directory> <output directory>\n"
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

# stand_in returns the C of a generated function's stand-in in a binary built
# without Postgres, which aborts naming it.
sub stand_in
{
	my ($returns, $name, $decl) = @_;
	return "\n$returns\n$name($decl)\n{\n\tdg_outside(\"$name\");\n}\n";
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

# RUST_TYPES gives a C type of the lists its Rust type in pg, and whether it
# is a pointer whose validity a caller vouches for (1), failing closed on any
# other. DgBytes and DgBuffer are a slice's pointer and length, borrowed for
# the call, XLogReaderState *, StringInfo, MemoryContext, FunctionCallInfo,
# Relation, and TupleTableSlot * are the records, descriptions, memory
# contexts, function calls, relations, and slots Postgres hands an entry,
# borrowed for its call; a slot is borrowed exclusively by what changes the
# row it holds, and shared, as const TupleTableSlot *, by what reads the row
# or fills its columns. const TableAmRoutine * and const TupleTableSlotOps *
# are the fork's table routine and its kind of slot, each a value only its
# wrapper makes, and DgIcuLocale and a wrapper's char * are the locales and
# strings wrappers make, each owned by the one value that holds it. A
# DgStored is a stored row's version, its writer's and deleter's full xids
# and their commands, with its table, its values' TOAST relation, its ctid,
# and the room its deform takes, passed by value. An entry's TupleTableSlot **
# and the int after it are a multi-row insert's slots and their count, which
# its exported side makes one slice by slots, and a BulkInsertState is a bulk
# insert's state, null for a single insert, which the fork only receives,
# named struct BulkInsertStateData * as tableam.h names it, and a
# const VacuumParams * and a BufferAccessStrategy are a VACUUM's options and
# its buffer strategy, null for none, which the fork only passes back to
# Postgres, a TidStore * is VACUUM's dead items, owned by a DeadItems, a
# DgRewrite * is a table rewrite's state, held by a Rewrite, and an
# IndexBulkDeleteResult * is an index's bulk delete statistics, owned by an
# IndexStats a wrapper takes or gives. A DgSnapshotView is a snapshot's
# fields as its view reads them, its arrays lent for the snapshot's life. A
# DgCount * is a bitmap scan's count of pages Postgres hands its entry,
# uint64 * in tableam.h, which only a wrapper adds to, and a DgBitmapPage is a
# TID bitmap's next page as a bitmap scan reads it, passed by value. A
# TM_FailureData *, LockTupleMode *, or TU_UpdateIndexes * is what a row's
# change fills for its caller, written only through its setter, and a
# TM_Result, LockTupleMode, or LockWaitPolicy is its C enum's int.
my %RUST_TYPES = (
	'void' => [ '()', 0 ],
	'bool' => [ 'bool', 0 ],
	'int' => [ 'i32', 0 ],
	'int32' => [ 'i32', 0 ],
	'uint8' => [ 'u8', 0 ],
	'uint32' => [ 'u32', 0 ],
	'Oid' => [ 'u32', 0 ],
	'CommandId' => [ 'u32', 0 ],
	'TransactionId' => [ 'u32', 0 ],
	'SubTransactionId' => [ 'u32', 0 ],
	'MemoryContext' => [ '&crate::Context', 0 ],
	'DgIcuLocale' => [ 'crate::IcuLocale', 0 ],
	'DgBuiltinLocale' => [ 'crate::BuiltinLocale', 0 ],
	'DgLocale' => [ 'crate::Locale', 0 ],
	'DgLibcLocale' => [ 'crate::LibcLocale', 0 ],
	'char *' => [ 'crate::PgString', 0 ],
	'uint64' => [ 'u64', 0 ],
	'int64' => [ 'i64', 0 ],
	'size_t' => [ 'usize', 0 ],
	'Size' => [ 'usize', 0 ],
	'Datum' => [ 'crate::Datum', 0 ],
	'HeapTuple' => [ 'crate::HeapTuple', 0 ],
	'MinimalTuple' => [ 'crate::MinimalTuple', 0 ],
	'TupleTableSlot *' => [ '&mut crate::Slot', 0 ],
	'TupleTableSlot **' => [ '*mut *mut crate::ffi::TupleTableSlot', 1 ],
	'struct BulkInsertStateData *' => [ 'Option<&crate::BulkInsert>', 0 ],
	'const VacuumParams *' => [ '&crate::VacuumParams', 0 ],
	'TidStore *' => [ '&crate::DeadItems', 0 ],
	'DgRewrite *' => [ '&crate::Rewrite', 0 ],
	'IndexBulkDeleteResult *' => [ 'Option<crate::IndexStats>', 0 ],
	'BufferAccessStrategy' => [ 'Option<&crate::Strategy>', 0 ],
	'const TupleTableSlot *' => [ '&crate::Slot', 0 ],
	'Relation' => [ '&crate::Rel', 0 ],
	'text *' => [ '&crate::Text', 0 ],
	'const TableAmRoutine *' => [ 'crate::TableRoutine', 0 ],
	'const TupleTableSlotOps *' => [ 'crate::SlotOps', 0 ],
	'bool *' => [ '&mut crate::IsNull', 0 ],
	'const bool *' => [ '&crate::IsNull', 0 ],
	'LWLockMode' => [ 'crate::ffi::LWLockMode', 0 ],
	'AlterTableType' => [ 'crate::ffi::AlterTableType', 0 ],
	'AlterTableCmd *' => [ 'Option<&crate::AlterCmd>', 0 ],
	'const AlterTableCmd *' => [ '&crate::AlterCmd', 0 ],
	'const char *' => [ '&core::ffi::CStr', 0 ],
	'DgBytes' => [ '&[u8]', 0 ],
	'DgBuffer' => [ '&mut [u8]', 0 ],
	'DgStored' => [ 'crate::ffi::DgStored', 0 ],
	'DgParallelBase' => [ 'crate::ffi::DgParallelBase', 0 ],
	'ParallelTableScanDescData *' => [ '&crate::ParallelScan', 0 ],
	'DgScanKeys' => [ 'crate::ffi::DgScanKeys', 0 ],
	'DgOwned' => [ 'crate::ffi::DgOwned', 0 ],
	'DgCompared' => [ 'crate::ffi::DgCompared', 0 ],
	'DgStorage' => [ 'crate::ffi::DgStorage', 0 ],
	'DgSnapshotView' => [ 'crate::ffi::DgSnapshotView', 0 ],
	'DgBitmapPage' => [ 'crate::ffi::DgBitmapPage', 0 ],
	'DgCount *' => [ '&mut crate::Counter', 0 ],
	'DgKeyColumn *' =>
	  [ '&mut [crate::ffi::DgKeyColumn; crate::INDEX_MAX_KEYS]', 0 ],
	'DgIndexColumn *' =>
	  [ '&mut [crate::ffi::DgIndexColumn; crate::INDEX_MAX_KEYS]', 0 ],
	'DgClass' => [ 'crate::ffi::DgClass', 0 ],
	'TableScanDesc' => [ '&mut crate::Scan', 0 ],
	'const TableScanDesc' => [ '&crate::Scan', 0 ],
	'const BackgroundWorker *' => [ '&crate::BackgroundWorker', 0 ],
	'IndexScanDesc' => [ '&mut crate::IndexScan', 0 ],
	'const IndexScanDesc' => [ '&crate::IndexScan', 0 ],
	'TIDBitmap *' => [ '&mut crate::Bitmap', 0 ],
	'struct PlannerInfo *' => [ '&mut crate::Planner', 0 ],
	'struct IndexPath *' => [ '&mut crate::IndexPath', 0 ],
	'Cost *' => [ '&mut f64', 0 ],
	'Selectivity *' => [ '&mut f64', 0 ],
	'Buffer *' => [ '&mut i32', 0 ],
	'DgIndexKey' => [ 'crate::ffi::DgIndexKey', 0 ],
	'DgIndexArray' => [ 'crate::ffi::DgIndexArray', 0 ],
	'DgPreprocessed' => [ 'crate::ffi::DgPreprocessed', 0 ],
	'DgSkipStep' => [ 'crate::ffi::DgSkipStep', 0 ],
	'struct IndexFetchTableData *' => [ '&mut crate::IndexFetch', 0 ],
	'struct SampleScanState *' => [ '&mut crate::SampleState', 0 ],
	'ReadStream *' => [ '&mut crate::Stream', 0 ],
	'const struct IndexFetchTableData *' => [ '&crate::IndexFetch', 0 ],
	'Snapshot' => [ 'Option<&crate::Snapshot>', 0 ],
	'ScanKey' => [ 'Option<&crate::ScanKeys>', 0 ],
	'ParallelTableScanDesc' => [ 'Option<&crate::ParallelScan>', 0 ],
	'ScanDirection' => [ 'i32', 0 ],
	'XactEvent' => [ 'i32', 0 ],
	'SubXactEvent' => [ 'i32', 0 ],
	'ForkNumber' => [ 'i32', 0 ],
	'BlockNumber *' => [ '&mut u32', 0 ],
	'TransactionId *' => [ '&mut u32', 0 ],
	'MultiXactId *' => [ '&mut u32', 0 ],
	'const RelFileLocator *' => [ '&crate::FileLocator', 0 ],
	'char' => [ 'core::ffi::c_char', 0 ],
	'double *' => [ '&mut f64', 0 ],
	'int32 *' => [ 'crate::Widths', 0 ],
	'uint16' => [ 'u16', 0 ],
	'ItemPointer' => [ '&mut crate::Tid', 0 ],
	'TM_FailureData *' => [ '&mut crate::Failure', 0 ],
	'TM_IndexDeleteOp *' => [ '&mut crate::IndexDeleteOp', 0 ],
	'Datum *' => [ '&crate::Datums', 0 ],
	'DoltgresBuild *' => [ '&mut crate::Build', 0 ],
	'DoltgresSort *' => [ '&mut crate::Sort', 0 ],
	'IndexBuildResult *' => [ 'crate::BuildResult', 0 ],
	'IndexVacuumInfo *' => [ '&mut crate::VacuumInfo', 0 ],
	'const IndexVacuumInfo *' => [ '&crate::VacuumInfo', 0 ],
	'IndexBulkDeleteCallback' => [ 'crate::BulkCallback', 0 ],
	'IndexBuildCallback' => [ 'crate::BuildCallback', 0 ],
	'DgBuildState' => [ 'crate::BuildState', 0 ],
	'DgParallelTarget' => [ 'crate::ParallelTarget', 0 ],
	'double' => [ 'f64', 0 ],
	'BlockNumber' => [ 'u32', 0 ],
	'struct IndexInfo *' => [ '&mut crate::IndexInfo', 0 ],
	'struct ValidateIndexState *' => [ '&mut crate::ValidateState', 0 ],
	'IndexUniqueCheck' => [ 'i32', 0 ],
	'const TM_IndexDeleteOp *' => [ '&crate::IndexDeleteOp', 0 ],
	'LockTupleMode *' => [ '&mut crate::LockModeOut', 0 ],
	'TU_UpdateIndexes *' => [ '&mut crate::UpdateIndexesOut', 0 ],
	'TM_Result' => [ 'i32', 0 ],
	'LockTupleMode' => [ 'i32', 0 ],
	'LockWaitPolicy' => [ 'i32', 0 ],
	'const ItemPointerData *' => [ '&crate::Tid', 0 ],
	'XLogReaderState *' => [ '&crate::WalRecord', 0 ],
	'StringInfo' => [ '&mut crate::Desc', 0 ],
	'FunctionCallInfo' => [ '&crate::Call', 0 ],
	'void *' => [ '*mut core::ffi::c_void', 1 ],
	'const DefElem *' => [ '*const core::ffi::c_void', 1 ],
	'void **' => [ '*mut *mut core::ffi::c_void', 1 ],
	'LWLock *' => [ '*mut crate::ffi::LWLock', 1 ],
	'const void *' => [ '*const core::ffi::c_void', 1 ],
	'uint32 *' => [ '*mut u32', 1 ],
	'uint64 *' => [ '*mut u64', 1 ],
	'pg_atomic_uint32 *' => [ '*mut crate::ffi::pg_atomic_uint32', 1 ],
	'pg_atomic_uint64 *' => [ '*mut crate::ffi::pg_atomic_uint64', 1 ],
	'ConditionVariable *' => [ '*mut crate::ffi::ConditionVariable', 1 ],
	'HTAB *' => [ '*mut crate::ffi::HTAB', 1 ],
	'ResourceOwner' => [ 'crate::ffi::ResourceOwner', 1 ],);

# rust_type returns a C type's Rust type in pg, and whether it is a pointer
# whose validity a caller vouches for.
sub rust_type
{
	my ($type) = @_;
	my $rust = $RUST_TYPES{$type}
	  or die "the binding has no Rust form for the C type $type\n";
	return @$rust;
}

# NULLABLE names the entries' pointer parameters Postgres may pass as NULL,
# which an entry takes as an Option: index_fetch_tuple's all_dead, which
# tableam.h lets a caller leave out, index_build_range_scan's scan, which only
# a parallel build passes, and relation_copy_for_cluster's old_index, which
# VACUUM FULL passes as NULL.
my @NULLABLE = ('all_dead', 'build_scan', 'old_index', 'forkName');

# rust_param returns a C parameter type's Rust type and the expression
# passing it.
sub rust_param
{
	my ($type, $name) = @_;
	my $expr;
	if ($type eq 'const char *')
	{
		$expr = "$name.as_ptr()";
	}
	elsif ($type eq 'DgBytes')
	{
		$expr =
		  "ffi::DgBytes { data: $name.as_ptr().cast(), len: $name.len() }";
	}
	elsif ($type eq 'DgBuffer')
	{
		$expr =
		  "ffi::DgBuffer { data: $name.as_mut_ptr().cast(), len: $name.len() }";
	}
	elsif ($type eq 'MemoryContext')
	{
		$expr = "core::ptr::from_ref($name).cast_mut().cast()";
	}
	elsif (
		grep { $_ eq $type } (
			'Datum', 'DgIcuLocale',
			'DgBuiltinLocale', 'DgLocale',
			'DgLibcLocale'))
	{
		$expr = "$name.0";
	}
	elsif (
		grep { $_ eq $type } (
			'XLogReaderState *',
			'FunctionCallInfo',
			'const TupleTableSlot *',
			'Relation',
			'text *',
			'const TableScanDesc',
			'const IndexScanDesc',
			'const struct IndexFetchTableData *',
			'const ItemPointerData *',
			'const RelFileLocator *',
			'const TM_IndexDeleteOp *',
			'Datum *',
			'const bool *',
			'const IndexVacuumInfo *',
			'const VacuumParams *',
			'const BackgroundWorker *',
			'const AlterTableCmd *',
			'ParallelTableScanDescData *'))
	{
		$expr = "core::ptr::from_ref($name).cast_mut().cast()";
	}
	elsif (
		grep { $_ eq $type } (
			'StringInfo',
			'TupleTableSlot *',
			'bool *',
			'TableScanDesc',
			'IndexScanDesc',
			'TIDBitmap *',
			'struct PlannerInfo *',
			'struct IndexPath *',
			'struct IndexFetchTableData *',
			'DgCount *',
			'struct SampleScanState *',
			'ReadStream *',
			'ItemPointer',
			'TM_FailureData *',
			'TM_IndexDeleteOp *',
			'struct IndexInfo *',
			'struct ValidateIndexState *',
			'DoltgresBuild *',
			'DoltgresSort *',
			'IndexVacuumInfo *',
			'LockTupleMode *',
			'TU_UpdateIndexes *',
			'BlockNumber *',
			'TransactionId *',
			'MultiXactId *',
			'Cost *',
			'Selectivity *',
			'Buffer *',
			'double *'))
	{
		$expr = "core::ptr::from_mut($name).cast()";
	}
	elsif ($type eq 'DgKeyColumn *' || $type eq 'DgIndexColumn *')
	{
		$expr = "$name.as_mut_ptr()";
	}
	elsif (
		grep { $_ eq $type } (
			'int32 *', 'TidStore *',
			'IndexBuildCallback', 'IndexBulkDeleteCallback',
			'DgBuildState', 'DgRewrite *',
			'DgParallelTarget'))
	{
		$expr = "$name.0";
	}
	elsif ($type eq 'IndexBulkDeleteResult *')
	{
		$expr =
		  "$name.map_or(core::ptr::null_mut(), crate::IndexStats::into_raw)";
	}
	elsif (
		grep { $_ eq $type } (
			'Snapshot', 'ScanKey',
			'ParallelTableScanDesc', 'BufferAccessStrategy'))
	{
		$expr =
		  "$name.map_or(core::ptr::null_mut(), |given| core::ptr::from_ref(given).cast_mut().cast())";
	}
	else
	{
		$expr = $name;
	}
	my ($rust) = rust_type($type);
	return ($rust, $expr);
}

# RUST_RETURNS gives a C return type its Rust type where it isn't its
# parameter form's.
my %RUST_RETURNS = (
	'TupleTableSlot *' => 'crate::OwnedSlot',
	'DoltgresBuild *' => 'crate::OwnedBuild',
	'DoltgresSort *' => 'crate::OwnedSort',
	'Datum *' => '&crate::Datums',
	'const bool *' => '&crate::IsNull',
	'TidStore *' => 'crate::DeadItems',
	'DgRewrite *' => 'crate::Rewrite',
	'TableScanDesc' => 'crate::ScanDesc',
	'IndexScanDesc' => 'crate::IndexScanDesc',
	'struct IndexFetchTableData *' => 'crate::IndexFetchDesc',);

# rust_return returns a C return type's Rust type.
sub rust_return
{
	my ($type) = @_;
	return $RUST_RETURNS{$type} if exists $RUST_RETURNS{$type};
	my ($rust) = rust_type($type);
	return $rust;
}

# RUST_WRAPS gives a C return type what turns its value into its Rust type,
# before and after it.
my %RUST_WRAPS = (
	'Datum' => 'crate::Datum',
	'DgIcuLocale' => 'crate::IcuLocale',
	'DgBuiltinLocale' => 'crate::BuiltinLocale',
	'DgLocale' => 'crate::Locale',
	'char *' => 'crate::PgString',
	'HeapTuple' => 'crate::HeapTuple',
	'MinimalTuple' => 'crate::MinimalTuple',
	'TupleTableSlot *' => 'crate::OwnedSlot',
	'DoltgresBuild *' => 'crate::OwnedBuild',
	'DoltgresSort *' => 'crate::OwnedSort',
	'IndexBuildResult *' => 'crate::BuildResult',
	'DgBuildState' => 'crate::BuildState',
	'DgParallelTarget' => 'crate::ParallelTarget',
	'TidStore *' => 'crate::DeadItems',
	'DgRewrite *' => 'crate::Rewrite',
	'IndexBulkDeleteResult *' => 'crate::IndexStats::from_raw',
	'const TableAmRoutine *' => 'crate::TableRoutine',
	'const TupleTableSlotOps *' => 'crate::SlotOps',
	'TableScanDesc' => 'crate::ScanDesc',
	'IndexScanDesc' => 'crate::IndexScanDesc',
	'struct IndexFetchTableData *' => 'crate::IndexFetchDesc',);

# rust_wrap returns what turns a C return value into its Rust type, before
# and after it.
sub rust_wrap
{
	my ($type) = @_;
	return ("$RUST_WRAPS{$type}(", ')') if exists $RUST_WRAPS{$type};
	return ('', '');
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

# frontend writes the frontend build's wrappers (design item D12) from
# frontend-calls.list and frontend-entries.list: each wrapper's C over the port
# layer, which returns a negative number with errno in its error on a failure
# and never exits, its Rust function, each entry's declaration, and the
# exported Rust side of each entry.
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
	my $rs = '';
	foreach my $row (rows('frontend-calls.list'))
	{
		my ($name, $body, $test) = ($row->[0], $row->[4], $row->[5]);
		my @args;
		my @pass;
		foreach my $param (params($row->[2]))
		{
			my ($type, $pname) = @$param;
			my ($rust, $expr) = rust_param($type, $pname);
			die "$name: a frontend wrapper takes no raw pointer\n"
			  if (rust_type($type))[1];
			push @args, "$pname: $rust";
			push @pass, $expr;
		}
		push @pass, '&mut error';
		$rs .=
		  "/// The wrapper of `$body` (`frontend-calls.list`), which never exits.\npub fn $name("
		  . join(', ', @args)
		  . ") -> Result<i64, crate::FeError> {\n    let mut error = 0;\n    // SAFETY: the generated wrapper returns normally, setting `error` on a failure, and never exits. Tested by $test.\n    let result = unsafe { ffi::doltgres_fe_$name("
		  . join(', ', @pass)
		  . ") };\n    if result < 0 {\n        return Err(crate::FeError(error));\n    }\n    Ok(result)\n}\n\n";
	}
	my $m =
	  "/// Expands into the frontend library the exported Rust side of every entry in `frontend-entries.list`, each\n/// calling the safe function of the same name in that crate's `entries` module, which `crate` names on purpose.\n#[macro_export]\n#[allow(clippy::crate_in_macro_def)]\nmacro_rules! export_frontend_entries {\n    () => {\n";
	foreach my $row (rows('frontend-entries.list'))
	{
		my $name = $row->[0];
		$m .=
		  "        /// The exported Rust side of `$name` (`frontend-entries.list`).\n        ///\n        /// # Safety\n        ///\n        /// Only the program's main calls it, with a data directory's path that ends with a NUL, callbacks that never\n        /// exit, and a buffer for its failure's message, each live for the call.\n        #[unsafe(no_mangle)]\n        pub unsafe extern \"C\" fn $name(datadir: *const core::ffi::c_char, line: \$crate::ffi::DgLine, kept: \$crate::ffi::DgKept, failure: \$crate::ffi::DgBuffer) -> bool {\n            // SAFETY: the caller passes what the function's safety section names. Tested by $row->[2].\n            unsafe { \$crate::run_frontend(datadir, line, kept, failure, crate::entries::$name) }\n        }\n";
	}
	$m .= "    };\n}\n";
	written('fe_calls.rs', $rs);
	written('fe_export.rs', $m);
}

if ($frontend)
{
	frontend();
	exit 0;
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
	my $tested = pop(@$r);
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
		test => $r->[7],
		tested => $tested,
	  };
}

my @entries;
foreach my $r (listed('entries.list', 'test-entries.list'))
{
	pop(@$r);
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
		test => $r->[6],
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
	my $lead = "extern $base";
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

my $o = $header;
$o .=
  "#include \"postgres.h\"\n\n#include \"fmgr.h\"\n\n#include \"calls.h\"\n\n/* dg_outside reports a generated function called in a binary built without Postgres, and aborts. */\npg_noreturn static void\ndg_outside(const char *name)\n{\n\tfputs(name, stderr);\n\tfputs(\" called outside Postgres\\n\", stderr);\n\tabort();\n}\n";
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
	$h .= extern_decl('int', "doltgres_setting_$name", 'void') . "\n";
	$c .=
	  "\nint\ndoltgres_setting_$name(void)\n{\n\treturn doltgres_$name;\n}\n";
	$o .= stand_in('int', "doltgres_setting_$name", 'void');
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
		$h .= extern_decl('int', $name, $decl) . "\n";
		my $assign =
		  defined($out_param)
		  && index($call->{body}, '*out') < 0
		  ? "\t\t*out = $call->{body};\n"
		  : statements($call->{body}, "\t\t");
		$c .=
		  "\nint\n$name($decl)\n{\n\tMemoryContext context = CurrentMemoryContext;\n\tuint32\t\tholdoff = InterruptHoldoffCount;\n\tuint32\t\tcancel = QueryCancelHoldoffCount;\n\tvolatile int failed = 0;\n\n\tPG_TRY();\n\t{\n$assign\t}\n\tPG_CATCH();\n\t{\n\t\tMemoryContextSwitchTo(context);\n\t\tInterruptHoldoffCount = holdoff;\n\t\tQueryCancelHoldoffCount = cancel;\n\t\tdg_take(error);\n\t\tfailed = 1;\n\t}\n\tPG_END_TRY();\n\treturn failed;\n}\n";
		$o .= stand_in('int', $name, $decl);
	}
	elsif ($call->{escalates})
	{
		my $decl = c_params($call->{params});
		$h .= extern_decl($returns, $name, $decl) . "\n";
		$o .= stand_in($returns, $name, $decl);
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
		$h .= extern_decl($returns, $name, $decl) . "\n";
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
		$o .= stand_in($returns, $name, $decl);
	}
}

my $e = $header;
$e .=
  "#include \"postgres.h\"\n\n#include \"fmgr.h\"\n#include \"doltgres/hooks.h\"\n#include \"utils/fmgrprotos.h\"\n\n#include \"calls.h\"\n\n";
$e .=
  "/* dg_raise raises what an entry's Rust side returned, at level for all but an error Postgres raised. */\npg_noreturn static void\ndg_raise(DgOutcome *outcome, int level)\n{\n\tswitch (outcome->kind)\n\t{\n\t\tcase DG_POSTGRES_ERROR:\n\t\t\tReThrowError(outcome->error.data);\n\t\t\tbreak;\n\t\tcase DG_OUT_OF_MEMORY:\n\t\t\tereport(level, errcode(ERRCODE_OUT_OF_MEMORY), errmsg(\"out of memory\"),\n\t\t\t\t\terrdetail(\"Failed on request of size %zu.\", outcome->size));\n\t\t\tbreak;\n\t\tcase DG_PANIC:\n\t\t\tereport(level, errcode(ERRCODE_INTERNAL_ERROR), errmsg_internal(\"%s\", outcome->message));\n\t\t\tbreak;\n\t\tcase DG_FORK_ERROR:\n\t\t\tereport(level, errcode(outcome->error.sqlerrcode), errmsg_internal(\"%s\", outcome->message),\n\t\t\t\t\toutcome->hint[0] != '\\0' ? errhint(\"%s\", outcome->hint) : 0);\n\t\t\tbreak;\n\t\tcase DG_OK:\n\t\t\tbreak;\n\t}\n\tpg_unreachable();\n}\n";

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
	$h .= extern_decl($entry->{returns}, $entry->{name}, $full) . "\n";
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
	  "\n$entry->{returns}\n$entry->{name}($full)\n{\n\tDgOutcome\toutcome;\n$result_decl\n\toutcome.kind = DG_OK;\n\t${assign}doltgres_rs_$entry->{name}("
	  . join(', ', @call_args)
	  . ");\n\tif (outcome.kind != DG_OK)\n\t\tdg_raise(&outcome, $level);\n$ret}\n";
}
$h .= "\n#endif\t\t\t\t\t\t\t/* DOLTGRES_CALLS_H */\n";

written('calls.h', $h);
written('calls.c', $c);
written('entries.c', $e);
written('outside.c', $o);

# The Rust side of the server's binding: each setting's getter and
# constants and each call's Rust function in calls.rs, and the exported Rust
# side of each entry in export.rs's export_entries! macro.
my $rs =
  "/// The fork's settings (`settings.list`): each one's value, and the values an enum setting can take.\npub mod settings {\n    use super::ffi;\n";
foreach my $setting (@settings)
{
	my $name = $setting->[0];
	my $enumerated = $setting->[1] eq 'enum';
	my @values = $enumerated ? split(/,/, $setting->[2], -1) : ();
	$rs .=
		"\n    /// $name returns the value of the setting `$name`"
	  . ($enumerated ? ', one of its constants below' : '')
	  . ".\n    pub fn $name() -> i32 {\n        // SAFETY: the getter only reads the setting's variable, which Postgres' GUC code sets in this process. Tested by $setting->[4].\n        unsafe { ffi::doltgres_setting_$name() }\n    }\n";
	for (my $i = 0; $i < @values; $i++)
	{
		$rs .=
			"\n    /// The value `$values[$i]` of `$name`.\n    pub const "
		  . uc($name) . '_'
		  . uc($values[$i])
		  . ": i32 = $i;\n";
	}
}
$rs .= "}\n\n";

foreach my $call (@calls)
{
	my $name = $call->{name};
	my $returns = $call->{returns};
	my @args;
	my @pass;
	my $lends = $call->{raises} && $returns eq 'DgBytes';
	my ($generics, $lent) = $lends ? ("<'a>", "'a ") : ('', '');
	push @args, "_raise: &${lent}crate::MayRaise" if $call->{raises};
	push @args, '_catalog: &crate::Catalog' if $call->{catalog};
	my $unsafe = 0;

	foreach my $param (@{ $call->{params} })
	{
		my ($type, $pname) = @$param;
		my ($rust, $expr) = rust_param($type, $pname);
		if ($lends)
		{
			$rust = "&'a crate::Call" if $type eq 'FunctionCallInfo';
			$rust = "&'a crate::Slot" if $type eq 'const TupleTableSlot *';
			$rust = "&'a crate::Datums" if $type eq 'Datum *';
			$rust = "&'a crate::IndexScan" if $type eq 'const IndexScanDesc';
		}
		push @args, "$pname: $rust";
		push @pass, $expr;
		$unsafe = 1 if (rust_type($type))[1];
	}
	my $ret = $lends ? "&'a [u8]" : rust_return($returns);
	my $many = @args > 7 ? "#[allow(clippy::too_many_arguments)]\n" : '';
	my $args = join(', ', @args);
	my ($doc, $unsafety) =
	  $unsafe
	  ? (
		"///\n/// # Safety\n///\n/// Each pointer is valid for the call, as the C it wraps requires.\n",
		'unsafe ')
	  : ('', '');
	$doc .= $many;
	if ($call->{raises})
	{
		my ($out_decl, $out_pass, $out_ret);
		if ($returns eq 'void')
		{
			($out_decl, $out_pass, $out_ret) = ('', '', '()');
		}
		elsif ($lends)
		{
			($out_decl, $out_pass, $out_ret) = (
				"    let mut out = core::mem::MaybeUninit::uninit();\n",
				'out.as_mut_ptr(), ',
				'unsafe { crate::borrowed(out.assume_init()) }');
		}
		else
		{
			my ($open, $close) = rust_wrap($returns);
			($out_decl, $out_pass, $out_ret) = (
				"    let mut out = core::mem::MaybeUninit::uninit();\n",
				'out.as_mut_ptr(), ',
				"${open}unsafe { out.assume_init() }$close");
		}
		my $passed = @pass ? join(', ', @pass) . ', ' : '';
		my $out_safety =
		  $returns eq 'void' ? ''
		  : "    // SAFETY: the wrapper wrote `out`, since it returned 0."
		  . (
			$lends
			? " The bytes it names are the argument's, the slot's value's, or the index entry's value's own or its detoasted copy in the memory context the entry was called in, which outlive the entry's capability and the call, the slot, or the values it borrows."
			: '') . " Tested by $call->{test}.\n";
		$rs .=
		  "/// The wrapper of `$call->{body}` (`calls.list`).\n${doc}pub ${unsafety}fn $name$generics($args) -> Result<$ret, crate::PgError> {\n$out_decl    let mut error = ffi::DgError { data: core::ptr::null_mut(), sqlerrcode: 0 };\n    // SAFETY: the generated wrapper catches what its call raises, so it returns normally. Tested by $call->{test}.\n    if unsafe { ffi::doltgres_call_$name($passed$out_pass&mut error) } != 0 {\n        return Err(crate::PgError::new(error));\n    }\n$out_safety    Ok($out_ret)\n}\n\n";
	}
	else
	{
		my ($open, $close) = rust_wrap($returns);
		my $call_expr = "ffi::doltgres_call_$name(" . join(', ', @pass) . ')';
		my $scans =
		  " The relation or snapshot it returns is the scan's or the index fetch's, which lives as long as it,                      past the borrow of it.";
		my ($expr, $borrows);
		if ($returns eq 'DgBytes')
		{
			($expr, $borrows) = (
				"unsafe { crate::borrowed($call_expr) }",
				" The bytes it returns are the record's, the slot's row's, the scan's shared scan's, or the index fetch's key, which outlive the borrow of it."
			);
		}
		elsif ($returns eq 'Relation')
		{
			($expr, $borrows) =
			  ("unsafe { &*$call_expr.cast::<crate::Rel>() }", $scans);
		}
		elsif ($returns eq 'Snapshot')
		{
			($expr, $borrows) = (
				"unsafe { $call_expr.cast::<crate::Snapshot>().as_ref() }",
				$scans);
		}
		elsif (($returns eq 'Datum *' || $returns eq 'const bool *')
			&& @{ $call->{params} }
			&& $call->{params}[0][0] eq 'Relation')
		{
			my ($rust) = rust_type($returns);
			($expr, $borrows) = (
				"unsafe { &*$call_expr.cast::<" . substr($rust, 1) . ">() }",
				" The values it returns are the removal's state's, which lives while the table's statement holds the relation, past the borrow of it."
			);
		}
		else
		{
			($expr, $borrows) = ("${open}unsafe { $call_expr }$close", '');
		}
		my $why =
		  $call->{escalates}
		  ? "the call can't raise into Rust, since its wrapper turns anything raised into a PANIC."
		  : "the call can't raise, as `calls.list` states with its reason.";
		my $returning = $returns eq 'void' ? '' : " -> $ret";
		$rs .=
		  "/// The wrapper of `$call->{body}` (`calls.list`), which can't raise.\n${doc}pub ${unsafety}fn $name($args)$returning {\n    // SAFETY: $why$borrows Tested by $call->{test}.\n    $expr\n}\n\n";
	}
}

# DEFAULTS gives an entry's value type its Rust return type, the default its
# thunk gets when the entry raises instead, and what turns an entry's given
# value into the pointer its thunk stores.
my %DEFAULTS = (
	'void' => [ '', '()', '' ],
	'Datum' => [ ' -> $crate::Datum', '$crate::Datum::NULL', '' ],
	'int64' => [ ' -> i64', '0', '' ],
	'TM_Result' => [ ' -> i32', '0', '' ],
	'int' => [ ' -> i32', '0', '' ],
	'TransactionId' => [ ' -> u32', '0', '' ],
	'Oid' => [ ' -> u32', '0', '' ],
	'char *' => [
		' -> *mut core::ffi::c_char',
		'core::ptr::null_mut()',
		'.map(|given: Option<$crate::PgString>| given.map_or(core::ptr::null_mut(), $crate::PgString::into_raw))'
	],
	'bool' => [ ' -> bool', 'false', '' ],
	'uint64' => [ ' -> u64', '0', '' ],
	'Size' => [ ' -> usize', '0', '' ],
	'double' => [ ' -> f64', '0.0', '' ],
	'IndexBuildResult *' =>
	  [ ' -> $crate::BuildResult', '$crate::BuildResult::NULL', '' ],
	'IndexBulkDeleteResult *' => [
		' -> *mut $crate::ffi::IndexBulkDeleteResult',
		'core::ptr::null_mut()',
		'.map(|given: Option<$crate::IndexStats>| given.map_or(core::ptr::null_mut(), $crate::IndexStats::into_raw))'
	],
	'pg_locale_t' => [
		' -> $crate::ffi::DgLocale',
		'core::ptr::null_mut()',
		'.map(|given: Option<$crate::Locale>| given.map_or(core::ptr::null_mut(), $crate::Locale::into_raw))'
	],
	'HeapTuple' => [ ' -> $crate::HeapTuple', '$crate::HeapTuple::NULL', '' ],
	'MinimalTuple' =>
	  [ ' -> $crate::MinimalTuple', '$crate::MinimalTuple::NULL', '' ],
	'const TableAmRoutine *' => [
		' -> *const $crate::ffi::TableAmRoutine',
		'core::ptr::null()',
		'.map(|given: Option<$crate::TableRoutine>| given.map_or(core::ptr::null(), $crate::TableRoutine::into_raw))'
	],
	'const TupleTableSlotOps *' =>
	  [ ' -> $crate::SlotOps', '$crate::SlotOps::NULL', '' ],
	'TableScanDesc' =>
	  [ ' -> $crate::ScanDesc', '$crate::ScanDesc::NULL', '' ],
	'IndexScanDesc' =>
	  [ ' -> $crate::IndexScanDesc', '$crate::IndexScanDesc::NULL', '' ],
	'struct IndexFetchTableData *' =>
	  [ ' -> $crate::IndexFetchDesc', '$crate::IndexFetchDesc::NULL', '' ],);

my $m =
  "/// Expands into the crate that links the fork the exported Rust side of every entry in `entries.list` (design item\n/// 11), each calling the safe function of the same name in that crate's `entries` module, which `crate` names on\n/// purpose, after that module's `prepare` for an ordinary entry.\n#[macro_export]\n#[allow(clippy::crate_in_macro_def)]\nmacro_rules! export_entries {\n    () => {\n";
foreach my $entry (@entries)
{
	my $name = $entry->{name};
	my $gives = $entry->{returns};
	if (defined($entry->{out}))
	{
		$gives = substr($entry->{out}[0], 0, -1);
		$gives =~ s/\s+\z//;
	}
	my @rargs;
	my $passed = '';
	my $counts;
	my @params = @{ $entry->{params} };
	my $varlena = 0;
	for (my $at = 0; $at < @params; $at++)
	{
		my ($type, $pname) = @{ $params[$at] };
		if (defined($counts) && $counts eq $pname)
		{
			push @rargs, "$pname: i32";
		}
		elsif ($type eq 'TupleTableSlot **')
		{
			die "slots without a count\n"
			  unless $at + 1 < @params && $params[ $at + 1 ][0] eq 'int';
			my $count = $params[ $at + 1 ][1];
			push @rargs, "$pname: *mut *mut \$crate::ffi::TupleTableSlot";
			$passed .= ", \$crate::slots($pname, $count)";
			$counts = $count;
		}
		elsif ($type eq 'const char *')
		{
			push @rargs, "$pname: *const core::ffi::c_char";
			$passed .= ", core::ffi::CStr::from_ptr($pname)";
		}
		elsif ($type eq 'varlena *')
		{
			die "a varlena without its length\n"
			  unless $at > 0 && $params[ $at - 1 ][0] eq 'int32';
			my $length = $params[ $at - 1 ][1];
			push @rargs, "$pname: *mut core::ffi::c_void";
			$passed .= ", \$crate::toast_slice($pname, $length)";
			$varlena = 1;
		}
		elsif ($at + 1 < @params && $params[ $at + 1 ][0] eq 'varlena *')
		{
			push @rargs, "$pname: i32";
		}
		elsif (grep { $_ eq $pname } @NULLABLE)
		{
			(my $rust = (rust_type($type))[0]) =~ s/crate::/\$crate::/g;
			push @rargs, "$pname: Option<$rust>";
			$passed .= ", $pname";
		}
		else
		{
			(my $rust = (rust_type($type))[0]) =~ s/crate::/\$crate::/g;
			push @rargs, "$pname: $rust";
			$passed .= ", $pname";
		}
	}
	push @rargs, 'outcome: *mut $crate::ffi::DgOutcome';
	my $default;
	if ($gives eq 'const char *')
	{
		$default =
		  defined($entry->{out})
		  ? [
			' -> *const core::ffi::c_char',
			'core::ptr::null()',
			'.map(|given: Option<$crate::PgString>| given.map_or(core::ptr::null(), |text| text.into_raw().cast_const()))'
		  ]
		  : [
			' -> *const core::ffi::c_char',
			'core::ptr::null()',
			'.map(|name: Option<&core::ffi::CStr>| name.map_or(core::ptr::null(), core::ffi::CStr::as_ptr))'
		  ];
	}
	else
	{
		$default = $DEFAULTS{$gives}
		  or die
		  "$name: the binding has no default for an entry returning $gives\n";
	}
	my ($ret, $value, $pointer) = @$default;
	my $slots =
	  defined($counts)
	  ? " The slots of a multi-row insert are distinct, non-null, and as many as their count, and outlive the call, as `CopyMultiInsertBufferFlush` and `ExecBatchInsert` pass them."
	  : $varlena
	  ? " A TOAST slice's varlena has room for its length past its header and outlives the call, as `toast_fetch_datum` and `toast_fetch_datum_slice` allocate it."
	  : '';
	my $prepare = $entry->{panic} ? '' : 'crate::entries::prepare(); ';
	$m .=
	  "        /// The exported Rust side of `$name` (`entries.list`).\n        ///\n        /// # Safety\n        ///\n        /// Only its C thunk calls it, with the thunk's own `DgOutcome` and the arguments Postgres gave the thunk.\n        #[unsafe(no_mangle)]\n        pub unsafe extern \"C\" fn doltgres_rs_$name("
	  . join(', ', @rargs) . ')'
	  . "$ret {\n            // SAFETY: the caller passes its own outcome, live for this call and used by nothing else, and each string Postgres gave it ends with a NUL and outlives the call.$slots Tested by $entry->{test}.\n            unsafe { \$crate::run::<\$crate::$entry->{caps}, _>(outcome, $value, |caps| { ${prepare}crate::entries::$name(caps$passed)$pointer }) }\n        }\n";
}
$m .= "    };\n}\n";
written('calls.rs', $rs);
written('export.rs', $m);

exit 0 unless $forms;

# The C forms of the generated Rust (the port text's sections 1, 12, and 16):
# each calls.list line's function in pg::calls and each settings.list line's
# getter and constants, in pg_calls.c and pg_calls.h beside the wrappers,
# since meson writes a custom target's files into one folder, with their
# port-map rows.

# FORM_PARAMS gives a C type of the lists the C form of its parameter: the
# declaration, with N for its name, and the expression that passes it to the
# wrapper, as build.rs's rust_param passes the Rust's.
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
# and the wrapping of the wrapper's value V into it, as build.rs's
# rust_return and rust_wrap give the Rust's; a type it doesn't name returns
# as its parameter form, unwrapped.
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

my $fh = "/* Generated by gen_binding.pl from calls.list and settings.list; never edited by hand. */\n";
$fh .= "#ifndef DOLTGRES_PG_PG_CALLS_H\n#define DOLTGRES_PG_PG_CALLS_H\n\n";
$fh .= "#ifndef FRONTEND\n\n#include \"doltgres/pg/lib.h\"\n\n";
my $fc = "/* Generated by gen_binding.pl from calls.list and settings.list; never edited by hand. */\n";
$fc .= "#ifndef FRONTEND\n#include \"postgres.h\"\n\n#include \"pg_calls.h\"\n";
my $rows = '';
my $server = 'not(doltgres_frontend)';
my $tested = 'all(not(doltgres_frontend), feature = "injection_points")';

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
	$rows .=
	  "pg::calls::settings::$name\t$server\tpg_calls_settings_$name\tfn\t-\t-\n";
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
	my $cfg = $call->{tested} ? $tested : $server;
	$rows .= "pg::calls::$name\t$cfg\tpg_calls_$name\tfn\t-\t-\n";
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
written('port-map.tsv', $rows);
