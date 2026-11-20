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

# access' C port, one file per Rust file.
doltgres_cport_access_sources = files(
  'heaptuple.c',
  'index.c',
  'nbtsearch.c',
  'key.c',
  'nbtutils.c',
  'relation.c',
  'slot.c',
  'transform.c',
)
doltgres_cport_sources += doltgres_cport_access_sources

# The text access' index tests compile in with include_str!, as a C array (embed.pl): the btree corpus recorded from
# Doltgrust's encodings.
doltgres_cport_access_test_tables = custom_target('doltgres_cport_access_test_tables',
  input: files('../../embed.pl', '../../test/btree/expected.tsv'),
  output: 'index_tests.h',
  command: [perl, '@INPUT0@', '@OUTPUT@', 'ACCESS_INDEX_TESTS_EXPECTED_BYTES', '@INPUT1@'],
)
doltgres_cport_access_include = include_directories('.')
