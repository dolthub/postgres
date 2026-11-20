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
  input: files('../../embed.pl', '../../icu/windows-names.tsv', '../../icu/windows-sorts.tsv',
    '../../icu/glibc-aliases.tsv'),
  output: 'locale_tables.h',
  command: [perl, '@INPUT0@', '@OUTPUT@', 'DOLTGRES_LOCALE_WINDOWS_BYTES', '@INPUT1@', 'DOLTGRES_LOCALE_SORTS_BYTES',
    '@INPUT2@', 'DOLTGRES_LOCALE_GLIBC_BYTES', '@INPUT3@'],
)
doltgres_cport_doltgres_test_tables = custom_target('doltgres_cport_doltgres_test_tables',
  input: [files('../../embed.pl'), subproject('icu').get_variable('icu_locmap')],
  output: 'locale_tests.h',
  command: [perl, '@INPUT0@', '@OUTPUT@', 'DOLTGRES_LOCALE_TESTS_LOCMAP_BYTES', '@INPUT1@'],
)
# The fork's version, which dolt_version() answers, from VERSION, which a doltgres test keeps Cargo's package version.
doltgres_cport_doltgres_version = configure_file(output: 'version.h', configuration: {
  'DOLTGRES_VERSION': '"' + import('fs').read('../../VERSION').strip() + '"',
})

# The compression corpus testing.rs compiles in with include_bytes!, as C arrays (embed.pl), in the order its CORPUS
# lists them, for the test module's zstd entry.
doltgres_cport_doltgres_corpus = custom_target('doltgres_cport_doltgres_corpus',
  input: files('../../embed.pl', '../../test/compression/corpus/text.txt', '../../test/compression/corpus/leaves.bin',
    '../../test/compression/corpus/repeated.txt', '../../test/compression/corpus/random.bin'),
  output: 'testing_corpus.h',
  command: [perl, '@INPUT0@', '@OUTPUT@', 'DOLTGRES_ENTRIES_TESTING_TEXT', '@INPUT1@', 'DOLTGRES_ENTRIES_TESTING_LEAVES',
    '@INPUT2@', 'DOLTGRES_ENTRIES_TESTING_REPEATED', '@INPUT3@', 'DOLTGRES_ENTRIES_TESTING_RANDOM', '@INPUT4@'],
)
doltgres_cport_doltgres_include = include_directories('.')

# The doltgres crate's C port, one file per Rust file.
doltgres_cport_doltgres_sources = files(
  'changes.c',
  'deletes.c',
  'entries.c',
  'entries_hooks.c',
  'entries_index.c',
  'entries_locale.c',
  'entries_slot.c',
  'entries_tail.c',
  'entries/searches.c',
  'entries/testing.c',
  'identity.c',
  'index.c',
  'kinds.c',
  'layout.c',
  'layouts.c',
  'locale.c',
  'reform.c',
  'rekey.c',
  'relation.c',
  'rewrite.c',
  'scan.c',
  'settle.c',
  'sizes.c',
) + [doltgres_cport_doltgres_tables, doltgres_cport_doltgres_corpus]
doltgres_cport_sources += doltgres_cport_doltgres_sources
