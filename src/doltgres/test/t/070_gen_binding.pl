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

# The binding generator's capability marks (design item 17): each entry's
# exported side takes the capabilities its marks in entries.list grant, an
# entry whose panic becomes a PANIC none, and the generator refuses the marks
# item 17 forbids.

use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Utils;
use Test::More;

my $pg = $ENV{doltgres_pg_dir};
my $script = "$pg/gen_binding.pl";

# capabilities returns the capabilities an entry's exported side runs with in
# the generator's export.rs.
sub capabilities
{
	my ($export, $entry) = @_;
	return $export =~
	  /fn doltgres_rs_$entry\(.*?\$crate::run::<\$crate::(\w+), _>/s
	  ? $1
	  : 'none';
}

my $out = PostgreSQL::Test::Utils::tempdir;
command_ok([ $^X, $script, $pg, $out ],
	'the generator writes the binding from the lists');
my $export = slurp_file("$out/export.rs");
is(capabilities($export, 'doltgres_shared_release'),
	'Cleanup', 'an entry whose panic becomes a PANIC gets no capability');
is(capabilities($export, 'doltgres_dolt_version'),
	'CatalogCaps',
	'an entry with a catalog mark and its reason gets catalog access');
is(capabilities($export, 'doltgres_shared_preload'),
	'Caps', 'an entry with neither mark may raise without catalog access');

# refused runs the generator over the lists with one entry line in place of
# entries.list's, which it must refuse with the message.
sub refused
{
	my ($line, $message, $name) = @_;
	my $lists = PostgreSQL::Test::Utils::tempdir;
	foreach my $list (
		qw(calls.list test-calls.list test-entries.list settings.list
		structs.list globals.list))
	{
		copy("$pg/$list", "$lists/$list") or die "cannot copy $list: $!";
	}
	open(my $fh, '>', "$lists/entries.list")
	  or die "cannot write entries.list: $!";
	print $fh "$line\n";
	close($fh);
	command_fails_like(
		[ $^X, $script, $lists, PostgreSQL::Test::Utils::tempdir ],
		$message, $name);
}

refused(
	"doltgres_bad\tvoid\t\tpanic\tcatalog\tan abort entry\ttest",
	qr/an entry whose panic becomes a PANIC gets no catalog access/,
	'an entry whose panic becomes a PANIC is refused catalog access');
refused(
	"doltgres_bad\tvoid\t\terror\tcatalog\t \ttest",
	qr/a catalog mark needs its reason/,
	'a catalog mark without its reason is refused');

done_testing();
