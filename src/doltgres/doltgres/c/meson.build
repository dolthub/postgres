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

# The texts the doltgres crate's Rust compiles in with include_str!, as C arrays (embed.pl): locale's tables of names,
# and, for locale's tests, the vendored ICU's table from a Windows locale id to its locale, which a build reads only
# through ICU's own build file.
doltgres_cport_doltgres_tables = custom_target('doltgres_cport_doltgres_tables',
  input: files('embed.pl', '../../icu/windows-names.tsv', '../../icu/windows-sorts.tsv',
    '../../icu/glibc-aliases.tsv'),
  output: 'locale_tables.h',
  command: [perl, '@INPUT0@', '@OUTPUT@', 'DOLTGRES_LOCALE_WINDOWS_BYTES', '@INPUT1@', 'DOLTGRES_LOCALE_SORTS_BYTES',
    '@INPUT2@', 'DOLTGRES_LOCALE_GLIBC_BYTES', '@INPUT3@'],
)
doltgres_cport_doltgres_test_tables = custom_target('doltgres_cport_doltgres_test_tables',
  input: [files('embed.pl'), subproject('icu').get_variable('icu_locmap')],
  output: 'locale_tests.h',
  command: [perl, '@INPUT0@', '@OUTPUT@', 'DOLTGRES_LOCALE_TESTS_LOCMAP_BYTES', '@INPUT1@'],
)
doltgres_cport_doltgres_include = include_directories('.')

# The doltgres crate's C port, one file per Rust file.
doltgres_cport_doltgres_sources = files(
  'deletes.c',
  'locale.c',
  'sizes.c',
) + [doltgres_cport_doltgres_tables]
doltgres_cport_sources += doltgres_cport_doltgres_sources

# The doltgres crate's files whose calls reach files not yet in a test module (access's and store's, WinC's, and txn's
# unlinked ones), compiled into the port's library but linked into no test module until those files are.
doltgres_cport_doltgres_unlinked_sources = files(
  'changes.c',
  'identity.c',
  'kinds.c',
  'layout.c',
  'relation.c',
  'scan.c',
  'settle.c',
)
doltgres_cport_sources += doltgres_cport_doltgres_unlinked_sources
