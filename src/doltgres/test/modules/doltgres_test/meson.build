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

# The fork's test module, built as Postgres' test modules are, so it is in the
# test installation and out of the default one, and only with injection
# points, as the test entries it calls are.

doltgres_test = shared_module('doltgres_test',
  files('doltgres_test.c', 'doltgres_test_none.c') + [doltgres_cport_pg_binding[0], doltgres_cport_pg_binding[4]],
  include_directories: [doltgres_cport_pg_include],
  kwargs: pg_test_mod_args,
)
test_install_libs += doltgres_test

test_install_data += files(
  'doltgres_test.control',
  'doltgres_test--1.0.sql',
)
